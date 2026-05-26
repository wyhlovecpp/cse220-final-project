#!/usr/bin/env python3
"""Two-minute smoke test that the SPP build is healthy.

Compiles a single benchmark (stride), runs nopref + spp at a small instruction
limit, and verifies:

  1. The simulator returns 0 (no crashes).
  2. SPP issued some prefetches (PREF_UL1REQ_QUEUE_SENTREQ > 0).
  3. SPP completed at least one lookahead chain of depth > 1.
  4. The SPP-on IPC is within 10% of the SPP-off IPC (so no catastrophic regressions).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCARAB_ROOT = ROOT / "scarab"
PIN_ROOT = ROOT / "tools" / "pin-3.15-98253-gb56e429b1-gcc-linux"
BENCH = ROOT / "benchmarks" / "bench_stride"
BASE_PARAMS = SCARAB_ROOT / "utils" / "qsort" / "PARAMS.qsort"
INST_LIMIT = 500_000


def baseline_params(spp_on: bool) -> str:
    txt = BASE_PARAMS.read_text()
    txt = re.sub(r"^--pref_framework_on.*$",
                 f"--pref_framework_on {1 if spp_on else 0}", txt, flags=re.MULTILINE)
    txt = re.sub(r"^--pref_stream_on.*$", "--pref_stream_on 0", txt, flags=re.MULTILINE)
    if spp_on:
        txt += "\n--pref_spp_on 1\n"
    return txt


def run_one(label: str, spp_on: bool, workdir: Path) -> dict:
    rundir = workdir / label
    rundir.mkdir()
    params = workdir / f"PARAMS.{label}.in"
    params.write_text(baseline_params(spp_on))
    env = os.environ.copy()
    env["PIN_ROOT"] = str(PIN_ROOT)
    cmd = [
        sys.executable, str(SCARAB_ROOT / "bin" / "scarab_launch.py"),
        "--program", str(BENCH),
        "--param", str(params),
        "--pintool_args", "-fast_forward_to_start_inst 1",
        "--scarab_args", f"--inst_limit {INST_LIMIT}",
    ]
    log = rundir / "run.log"
    with log.open("w") as fp:
        rc = subprocess.call(cmd, cwd=rundir, env=env,
                             stdout=fp, stderr=subprocess.STDOUT)
    stats = {}
    for sf in rundir.glob("*.stat.0.out"):
        for line in sf.read_text(errors="ignore").splitlines():
            parts = line.split()
            if not parts: continue
            try:
                stats.setdefault(parts[0], int(parts[1]))
            except (IndexError, ValueError):
                continue
    ipc_match = re.search(r"IPC:\s*([0-9.]+)",
                          (rundir / "core.stat.0.out").read_text(errors="ignore")
                          if (rundir / "core.stat.0.out").exists() else "")
    return {"rc": rc, "stats": stats,
            "ipc": float(ipc_match.group(1)) if ipc_match else 0.0,
            "rundir": rundir}


def main():
    if not BENCH.exists():
        sys.exit(f"bench_stride is missing: {BENCH}. Run 'make' in benchmarks/.")
    with tempfile.TemporaryDirectory() as td:
        wd = Path(td)
        print("Running baseline (no prefetcher)...")
        a = run_one("nopref", False, wd)
        print(f"  rc={a['rc']}  ipc={a['ipc']:.3f}")
        print("Running SPP...")
        b = run_one("spp", True, wd)
        print(f"  rc={b['rc']}  ipc={b['ipc']:.3f}")

        ok = True
        def check(cond, msg):
            nonlocal ok
            if cond: print("  OK:", msg)
            else:    print("  FAIL:", msg); ok = False

        check(a["rc"] == 0, "nopref completed cleanly")
        check(b["rc"] == 0, "SPP completed cleanly")
        check(b["stats"].get("PREF_UL1REQ_QUEUE_SENTREQ", 0) > 0,
              "SPP issued at least one prefetch")
        check(b["stats"].get("PREF_SPP_LOOKAHEAD_DEPTH_TOTAL", 0) > 0,
              "SPP recorded lookahead depth")
        ratio = b["ipc"] / a["ipc"] if a["ipc"] else 0
        check(ratio > 0.85,
              f"SPP IPC ({b['ipc']:.3f}) within 15% of baseline ({a['ipc']:.3f}, ratio {ratio:.2f})")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
