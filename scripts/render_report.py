#!/usr/bin/env python3
"""Render the final report by injecting numbers from results/summary.csv
into a template under docs/final_report.template.md.

Usage:
    python3 scripts/render_report.py
        --csv  results/summary.csv
        --template docs/final_report.template.md
        --out  docs/final_report.md
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path


def load(csv_path: Path) -> dict[tuple[str, str], dict]:
    rows = {}
    with csv_path.open() as fp:
        for row in csv.DictReader(fp):
            rows[(row["bench"], row["config"])] = row
    return rows


def render(template: str, rows) -> str:
    """Replace markers like {{stride.spp.ipc}} with actual numbers."""
    def replace(m):
        bench, cfg, field = m.group(1).split(".")
        row = rows.get((bench, cfg), {})
        val = row.get(field, "?")
        if field == "ipc":
            try:
                return f"{float(val):.3f}"
            except Exception:
                return str(val)
        if field == "speedup":
            base = rows.get((bench, "nopref"), {}).get("ipc", 0)
            try:
                ratio = float(val) / float(base)
                return f"{ratio:.2f}×"
            except Exception:
                return "?"
        if field == "mpki":
            try:
                return f"{float(val):.2f}"
            except Exception:
                return str(val)
        if field in ("pref_accuracy", "pref_coverage"):
            try:
                return f"{float(val):.2%}"
            except Exception:
                return str(val)
        return str(val)
    return re.sub(r"{{([a-z0-9_]+\.[a-z0-9_]+\.[a-z0-9_]+)}}", replace, template)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--template", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rows = load(Path(args.csv))
    template_txt = Path(args.template).read_text()
    rendered = render(template_txt, rows)
    Path(args.out).write_text(rendered)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
