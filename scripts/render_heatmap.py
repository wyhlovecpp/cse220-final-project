#!/usr/bin/env python3
"""Render a 6-prefetcher × 8-benchmark heatmap of IPC speedup vs nopref.
Pulls per-(bench, config) IPCs from the various results_* directories.

Usage:
    python3 scripts/render_heatmap.py
"""

from __future__ import annotations

import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent


def ipc(p: Path) -> float | None:
    try:
        for line in (p / "core.stat.0.out").read_text(errors="ignore").splitlines():
            m = re.search(r"IPC:\s*([0-9.]+)", line)
            if m:
                return float(m.group(1))
    except FileNotFoundError:
        pass
    return None


# (bench display name, baseline result-dir name)
BENCHES = [
    ("stride",      "stride"),
    ("strided",     "strided"),
    ("2dstencil",   "2dstencil"),
    ("linkedlist",  "linkedlist"),
    ("random",      "random"),
    ("hashtable",   "hashtable"),
    ("matmul",      "matmul"),
    ("matmul+pad",  "matmul_padded"),
]
CONFIGS = ["nopref", "stride", "stream", "spp", "ghb", "markov"]

# Where to find each (bench, config) pair. Most live under results/{bench}_{cfg}/
# but the alt prefetchers ghb/markov live under results_alt/{bench}_{cfg}/
# except for linkedlist which is under results_extras/.
def find(bench: str, cfg: str) -> Path:
    cands = [
        ROOT / "results" / f"{bench}_{cfg}",
        ROOT / "results_alt" / f"{bench}_{cfg}",
        ROOT / "results_extras" / f"{bench}_{cfg}",
        ROOT / "results_tuned" / bench,         # tuned SPP runs
    ]
    for d in cands:
        if (d / "core.stat.0.out").exists():
            return d
    return cands[0]


# Build the value grid (speedup vs nopref). Rows = bench, cols = config.
n_b, n_c = len(BENCHES), len(CONFIGS)
mat = np.full((n_b, n_c), np.nan)
abs_ipc = np.full((n_b, n_c), np.nan)
for i, (label, dirname) in enumerate(BENCHES):
    nopref_dir = find(dirname, "nopref")
    base = ipc(nopref_dir)
    if base is None or base == 0:
        continue
    for j, cfg in enumerate(CONFIGS):
        d = find(dirname, cfg)
        val = ipc(d)
        if val is not None:
            abs_ipc[i, j] = val
            mat[i, j] = val / base

# Plot heatmap of speedup (centered at 1.0).
fig, ax = plt.subplots(figsize=(8.5, 6))
# Use a diverging colormap centered at 1.0 (speedup of 1×).
vmax = max(np.nanmax(mat[mat > 1]) if np.any(mat > 1) else 1.05, 1.05)
vmin = min(np.nanmin(mat[mat < 1]) if np.any(mat < 1) else 0.95, 0.5)
im = ax.imshow(
    mat,
    cmap="RdYlGn",
    aspect="auto",
    vmin=vmin,
    vmax=vmax,
)
ax.set_xticks(range(n_c))
ax.set_xticklabels(CONFIGS, rotation=30, ha="right")
ax.set_yticks(range(n_b))
ax.set_yticklabels([label for label, _ in BENCHES])
ax.set_title("Speedup vs no-prefetcher (each cell = IPC / IPC[nopref])")
for i in range(n_b):
    for j in range(n_c):
        v = mat[i, j]
        if np.isnan(v):
            txt, color = "—", "black"
        elif v >= 1:
            txt, color = f"{v:.2f}×", "black"
        else:
            txt, color = f"{v:.2f}×", "white" if v < 0.7 else "black"
        ax.text(j, i, txt, ha="center", va="center", fontsize=9, color=color)
cbar = plt.colorbar(im, ax=ax)
cbar.set_label("Speedup (1.00× = nopref)")
fig.tight_layout()
out = ROOT / "results" / "heatmap_speedup.png"
fig.savefig(out, dpi=140)
plt.close(fig)
print(f"Wrote {out}")
print("\nAbsolute IPC table:")
print(f"{'bench':<14} " + " ".join(f"{c:>8}" for c in CONFIGS))
for i, (label, _) in enumerate(BENCHES):
    cells = " ".join(
        f"{abs_ipc[i, j]:>8.3f}" if not np.isnan(abs_ipc[i, j]) else f"{'—':>8}"
        for j in range(n_c)
    )
    print(f"{label:<14} {cells}")
