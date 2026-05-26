#!/usr/bin/env python3
"""Parse the Scarab stat files under results/ and produce a tidy CSV plus
matplotlib bar plots comparing prefetcher configurations.

Outputs (in results/):
  summary.csv                 -- one row per (bench, config) with headline metrics
  ipc.png                     -- IPC bar chart, grouped by bench
  speedup.png                 -- IPC speedup over no-prefetcher
  l1_mpki.png                 -- L2 (Scarab "L1" = "UL1") miss rate
  pref_metrics.png            -- accuracy, coverage, lookahead depth
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RESULTS = ROOT / "results"

BENCHES = ["stride", "strided", "2dstencil", "linkedlist", "random"]
CONFIGS = ["nopref", "stride", "stream", "spp"]
CONFIG_COLORS = {"nopref": "#888888",
                 "stride": "#4C9AFF",
                 "stream": "#36B37E",
                 "spp":    "#FF5630"}

# Stat names we extract. The value parser picks the first numeric column.
HEADLINE_STATS = {
    "cycles":              "NODE_CYCLE",
    "insts":               "NODE_INST_COUNT",
    "icache_hit":          "ICACHE_HIT",
    "icache_miss":         "ICACHE_MISS",
    "dcache_hit":          "DCACHE_HIT",
    "dcache_miss":         "DCACHE_MISS",
    "ul1_hits":            "L1_HIT_ALL",
    "ul1_misses":          "L1_MISS_ALL",
    "ul1_pref_hit":        "L1_PREF_HIT",
    "ul1_pref_unique":     "L1_PREF_UNIQUE_HIT",
    "ul1_pref_late":       "L1_PREF_LATE",
    "pref_sent":           "PREF_UL1REQ_QUEUE_SENTREQ",
    "pref_matched":        "PREF_UL1REQ_QUEUE_MATCHED_REQ",
    "spp_operate":         "PREF_SPP_OPERATE",
    "spp_pf_issued_l2":    "PREF_SPP_PF_ISSUED_L2",
    "spp_pf_issued_llc":   "PREF_SPP_PF_ISSUED_LLC",
    "spp_pf_filtered":     "PREF_SPP_PF_FILTERED",
    "spp_pf_page_cross":   "PREF_SPP_PF_PAGE_CROSS",
    "spp_pf_useful":       "PREF_SPP_PF_USEFUL",
    "spp_lookahead_total": "PREF_SPP_LOOKAHEAD_DEPTH_TOTAL",
    "spp_ghr_boot":        "PREF_SPP_GHR_BOOT",
}


def parse_stat_file(stat_path: Path) -> dict:
    """Pull the first numeric column for each known stat key."""
    vals = {}
    if not stat_path.exists():
        return vals
    for line in stat_path.read_text(errors="ignore").splitlines():
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if key in vals:
            continue
        try:
            vals[key] = int(parts[1].rstrip("%"))
        except (IndexError, ValueError):
            continue
    return vals


def load_one(rundir: Path) -> dict:
    """Aggregate all stat files in a run directory."""
    out = {}
    for sf in rundir.glob("*.stat.0.out"):
        out.update(parse_stat_file(sf))
    return out


def summarise(results_dir: Path) -> list[dict]:
    rows = []
    for bench in BENCHES:
        for cfg in CONFIGS:
            rd = results_dir / f"{bench}_{cfg}"
            if not rd.is_dir():
                continue
            stats = load_one(rd)
            row = {"bench": bench, "config": cfg}
            for short, full in HEADLINE_STATS.items():
                row[short] = stats.get(full, 0)
            # Derived metrics.
            if row["cycles"] > 0:
                row["ipc"] = row["insts"] / row["cycles"]
            else:
                row["ipc"] = 0.0
            if row["pref_sent"] > 0:
                useful = row["ul1_pref_unique"] + row["ul1_pref_late"]
                row["pref_accuracy"] = useful / row["pref_sent"]
            else:
                row["pref_accuracy"] = 0.0
            ul1_demand = row["ul1_hits"] + row["ul1_misses"]
            if ul1_demand > 0:
                row["mpki"] = 1000 * row["ul1_misses"] / max(row["insts"], 1)
                row["pref_coverage"] = (row["ul1_pref_unique"]
                                        + row["ul1_pref_late"]) / max(
                                            row["ul1_pref_unique"]
                                            + row["ul1_pref_late"]
                                            + row["ul1_misses"], 1)
            else:
                row["mpki"] = 0.0
                row["pref_coverage"] = 0.0
            if row["spp_operate"] > 0:
                row["spp_avg_depth"] = (row["spp_lookahead_total"]
                                        / row["spp_operate"])
            else:
                row["spp_avg_depth"] = 0.0
            rows.append(row)
    return rows


def write_csv(rows, out_path):
    if not rows: return
    keys = ["bench", "config"] + [k for k in rows[0] if k not in ("bench", "config")]
    with out_path.open("w", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"Wrote {out_path}")


def bar_grouped(rows, metric, ylabel, title, out_path, normalize_to=None):
    by = {(r["bench"], r["config"]): r.get(metric, 0) for r in rows}
    base = None
    if normalize_to is not None:
        base = {r["bench"]: r.get(metric, 0)
                for r in rows if r["config"] == normalize_to}
    x = np.arange(len(BENCHES))
    width = 0.18
    fig, ax = plt.subplots(figsize=(9, 4.2))
    for i, cfg in enumerate(CONFIGS):
        vals = []
        for bench in BENCHES:
            v = by.get((bench, cfg), 0)
            if base:
                ref = base.get(bench, 0)
                vals.append((v / ref) if ref else 0)
            else:
                vals.append(v)
        ax.bar(x + (i - 1.5) * width, vals, width,
               label=cfg, color=CONFIG_COLORS.get(cfg, "#666"))
    ax.set_xticks(x)
    ax.set_xticklabels(BENCHES, rotation=15)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if base:
        ax.axhline(1.0, color="black", linewidth=0.5, linestyle=":")
    ax.legend(loc="upper right", fontsize=9)
    ax.grid(axis="y", linestyle="--", linewidth=0.4, alpha=0.5)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)
    print(f"Wrote {out_path}")


def text_summary(rows: list[dict], out_path: Path):
    """Write a human-readable plain-text leaderboard, one section per benchmark."""
    lines = ["# Headline numbers", ""]
    for bench in BENCHES:
        rs = [r for r in rows if r["bench"] == bench]
        if not rs: continue
        base_ipc = next((r["ipc"] for r in rs if r["config"] == "nopref"), 0)
        lines.append(f"## {bench}")
        lines.append(f"{'config':<10} {'IPC':>7} {'speedup':>8} {'MPKI':>7} "
                     f"{'acc':>6} {'cov':>6} {'depth':>6}")
        for r in rs:
            speed = (r["ipc"] / base_ipc) if base_ipc else 0
            lines.append(f"{r['config']:<10} {r['ipc']:>7.3f} {speed:>7.2f}x "
                         f"{r['mpki']:>7.2f} {r.get('pref_accuracy',0):>6.2%} "
                         f"{r.get('pref_coverage',0):>6.2%} "
                         f"{r.get('spp_avg_depth',0):>6.2f}")
        lines.append("")
    out_path.write_text("\n".join(lines))
    print(f"Wrote {out_path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(DEFAULT_RESULTS))
    args = ap.parse_args()
    results_dir = Path(args.results)
    rows = summarise(results_dir)
    if not rows:
        sys.exit("No run results found under " + str(results_dir))
    write_csv(rows, results_dir / "summary.csv")
    text_summary(rows, results_dir / "summary.txt")
    bar_grouped(rows, "ipc", "IPC", "Instruction-per-cycle by benchmark / config",
                results_dir / "ipc.png")
    bar_grouped(rows, "ipc", "Speedup over no-prefetcher",
                "IPC speedup (norm. to nopref)",
                results_dir / "speedup.png",
                normalize_to="nopref")
    bar_grouped(rows, "mpki", "L2 misses per kilo-instruction (lower better)",
                "L2 (UL1) MPKI", results_dir / "l1_mpki.png")
    bar_grouped(rows, "pref_accuracy", "Accuracy (useful / sent)",
                "Prefetcher accuracy", results_dir / "pref_accuracy.png")
    bar_grouped(rows, "pref_coverage", "Coverage = useful / (useful + miss)",
                "Prefetcher coverage", results_dir / "pref_coverage.png")
    bar_grouped(rows, "spp_avg_depth", "Avg. lookahead depth per demand",
                "SPP lookahead depth (others = 0)",
                results_dir / "spp_depth.png")


if __name__ == "__main__":
    main()
