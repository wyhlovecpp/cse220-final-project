#!/usr/bin/env python3
"""Run all benchmarks under all prefetcher configurations and collect stats.

Usage:
    python3 run_experiments.py [--inst-limit N] [--bench foo,bar,...]
                               [--config nopref,stride,stream,spp]
                               [--outdir results/]

Each (bench, config) combination produces a directory under outdir/ containing
the simulator's stat dump. We then parse a fixed set of headline counters
(IPC, L2 MPKI, prefetcher accuracy, etc.) and emit a results.csv with one row
per (bench, config).
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCARAB_ROOT = ROOT / "scarab"
PIN_ROOT = ROOT / "tools" / "pin-3.15-98253-gb56e429b1-gcc-linux"
SCARAB_LAUNCH = SCARAB_ROOT / "bin" / "scarab_launch.py"
BENCH_DIR = ROOT / "benchmarks"

DEFAULT_BENCHES = ["stride", "strided", "2dstencil", "linkedlist", "random"]

# Each config is a list of additional --foo=bar args that overlay the base PARAMS.in.
CONFIGS = {
    "nopref":  ["--pref_framework_on=0",
                "--pref_stream_on=0",
                "--pref_stride_on=0",
                "--pref_spp_on=0"],
    "stride":  ["--pref_framework_on=1",
                "--pref_stream_on=0",
                "--pref_stride_on=1",
                "--pref_spp_on=0"],
    "stream":  ["--pref_framework_on=1",
                "--pref_stream_on=1",
                "--pref_stride_on=0",
                "--pref_spp_on=0"],
    "spp":     ["--pref_framework_on=1",
                "--pref_stream_on=0",
                "--pref_stride_on=0",
                "--pref_spp_on=1"],
}


def base_params_in() -> str:
    """Return the contents of the canonical kaby_lake PARAMS, with all
    prefetchers turned off by default so the per-config overlay alone
    decides which prefetcher runs."""
    src = SCARAB_ROOT / "utils" / "qsort" / "PARAMS.qsort"
    txt = src.read_text()
    # Force prefetcher framework defaults to OFF; the overlay re-enables.
    txt = re.sub(r"^--pref_framework_on.*$",
                 "--pref_framework_on 0", txt, flags=re.MULTILINE)
    txt = re.sub(r"^--pref_stream_on.*$",
                 "--pref_stream_on 0", txt, flags=re.MULTILINE)
    return txt


def run_one(bench: str, config: str, outdir: Path, inst_limit: int,
            extra_scarab: list[str]) -> Path:
    """Run a single (bench, config) and return the run directory."""
    rundir = outdir / f"{bench}_{config}"
    rundir.mkdir(parents=True, exist_ok=True)
    # scarab_launch.py copies the param file into simdir as PARAMS.in, so we
    # keep the template *outside* the simdir to avoid SameFileError.
    params_in = outdir / f"PARAMS.{bench}_{config}.template"
    params_in.write_text(base_params_in())

    extra = " ".join(CONFIGS[config])
    scarab_args = (f"--inst_limit {inst_limit} "
                   f"--heartbeat_interval 1000000 "
                   f"--num_heartbeats 5 {extra} "
                   + " ".join(extra_scarab))

    program = BENCH_DIR / f"bench_{bench}"
    assert program.exists(), f"Missing binary: {program}"

    env = os.environ.copy()
    env["PIN_ROOT"] = str(PIN_ROOT)
    cmd = [
        sys.executable, str(SCARAB_LAUNCH),
        "--program", str(program),
        "--param", str(params_in),
        "--pintool_args", "-fast_forward_to_start_inst 1",
        "--scarab_args", scarab_args,
    ]
    log = rundir / "run.log"
    print(f"[run] {bench:12s} {config:8s} -> {rundir}")
    with log.open("w") as f:
        rc = subprocess.call(cmd, cwd=rundir, env=env,
                             stdout=f, stderr=subprocess.STDOUT)
    if rc != 0:
        print(f"  WARN: return code {rc} -- see {log}")
    return rundir


# ----- stat parsing -----
STAT_PATTERNS = {
    "insts":        re.compile(r"^NODE_INST_COUNT\s+(\d+)"),
    "cycles":       re.compile(r"^NODE_CYCLE\s+(\d+)"),
    "ipc":          None,  # derived
    "l2_miss":      re.compile(r"^L1_HIT_DEMAND_MISS_DATA_INST_(?:LD|ST)\s+(\d+)"),
    "ul1_demand":   re.compile(r"^L1_HIT_DEMAND_MISS_DATA_INST_(?:LD|ST)\s+(\d+)"),
    "ul1_hits":     re.compile(r"^L1_HIT_ALL\s+(\d+)"),
    "ul1_misses":   re.compile(r"^L1_MISS_ALL\s+(\d+)"),
    "pref_sent":    re.compile(r"^PREF_UL1_REQ_SENT\s+(\d+)"),
    "pref_useful":  re.compile(r"^PREF_UL1_USEFUL\s+(\d+)"),
    "pref_late":    re.compile(r"^PREF_UL1_LATE\s+(\d+)"),
}


def parse_stats(rundir: Path) -> dict:
    """Parse the bundled core.stat.out file produced by Scarab."""
    out = {"rundir": str(rundir)}
    # Scarab dumps several stat files; the per-core one is usually
    # `core.stat.out` (no tag) or `<file_tag>.core.stat.out` if a tag is set.
    candidates = list(rundir.glob("*.stat.out")) + list(rundir.glob("core.stat.out"))
    if not candidates:
        out["error"] = "no stat output"
        return out
    for sf in candidates:
        try:
            for line in sf.read_text(errors="ignore").splitlines():
                parts = line.split()
                if not parts:
                    continue
                key = parts[0]
                # Pick the leading numeric column.
                try:
                    val = int(parts[1])
                except (IndexError, ValueError):
                    continue
                out.setdefault(key, val)
        except Exception as exc:
            out["error"] = f"parse {sf}: {exc}"
    try:
        if out.get("NODE_CYCLE", 0) > 0:
            out["IPC"] = out.get("NODE_INST_COUNT", 0) / out["NODE_CYCLE"]
        if (out.get("PREF_UL1_REQ_SENT", 0) > 0):
            out["pref_accuracy"] = (out.get("PREF_UL1_USEFUL", 0)
                                    / out["PREF_UL1_REQ_SENT"])
        useful = out.get("PREF_UL1_USEFUL", 0)
        misses = out.get("ICACHE_UL1_MISS", 0) + out.get("L1_MISS_ALL", 0)
        if useful + misses > 0:
            out["pref_coverage"] = useful / (useful + misses)
    except Exception:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst-limit", type=int, default=20_000_000)
    ap.add_argument("--bench", default=",".join(DEFAULT_BENCHES))
    ap.add_argument("--config", default=",".join(CONFIGS.keys()))
    ap.add_argument("--outdir", default=str(ROOT / "results"))
    ap.add_argument("--extra-scarab-args", default="", help="extra space-separated args to scarab")
    args = ap.parse_args()

    benches = [b for b in args.bench.split(",") if b]
    configs = [c for c in args.config.split(",") if c]
    extra_scarab = [a for a in args.extra_scarab_args.split() if a]
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    rows = []
    for bench in benches:
        for cfg in configs:
            rd = run_one(bench, cfg, outdir, args.inst_limit, extra_scarab)
            stats = parse_stats(rd)
            stats["bench"] = bench
            stats["config"] = cfg
            rows.append(stats)

    csv_path = outdir / "results.csv"
    keys = sorted({k for r in rows for k in r.keys()})
    # Put bench/config first.
    keys = ["bench", "config"] + [k for k in keys if k not in ("bench", "config")]
    with csv_path.open("w", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"\nWrote {csv_path}")


if __name__ == "__main__":
    main()
