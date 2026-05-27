#!/usr/bin/env python3
"""Render docs/slides.md as a self-contained HTML slide deck using reveal.js
(loaded from a CDN). This avoids the marp/Chromium dependency on machines
without a browser installed.

Usage:
    python3 scripts/render_slides.py
        --input  docs/slides.md
        --out    docs/slides.html
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import markdown

REVEAL_VERSION = "4.6.1"

TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/dist/reset.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/dist/reveal.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/dist/theme/white.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/plugin/highlight/monokai.css">
<style>
.reveal {{ font-size: 26px; }}
.reveal h1 {{ font-size: 1.6em; }}
.reveal h2 {{ font-size: 1.3em; }}
.reveal table {{ font-size: 0.7em; margin: auto; }}
.reveal pre code {{ font-size: 0.7em; padding: 0.6em; max-height: 80vh; }}
.reveal ul, .reveal ol {{ display: block; text-align: left; }}
.reveal section {{ text-align: left; }}
.reveal section h1, .reveal section h2 {{ text-align: center; }}
.reveal .slide-number {{ font-size: 16px; }}
.reveal blockquote {{ font-size: 0.85em; padding: 0.6em 1em; }}
</style>
</head>
<body>
<div class="reveal"><div class="slides">
{slides}
</div></div>
<script src="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/dist/reveal.js"></script>
<script src="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/plugin/highlight/highlight.js"></script>
<script src="https://cdn.jsdelivr.net/npm/reveal.js@{rev_ver}/plugin/notes/notes.js"></script>
<script>
Reveal.initialize({{
  hash: true,
  slideNumber: true,
  controls: true,
  plugins: [ RevealHighlight, RevealNotes ],
}});
</script>
</body>
</html>
"""


def split_slides(text: str) -> list[str]:
    """Marp uses `---` on its own line as the slide separator. The first
    `---`-bracketed block is YAML frontmatter; the rest are slide separators."""
    # Strip HTML comments first (marp-specific build instructions).
    text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL).lstrip()
    # Drop YAML frontmatter if present.
    if text.startswith("---"):
        m = re.match(r"^---\s*\n.*?\n---\s*\n", text, re.DOTALL)
        if m:
            text = text[m.end():]
    text = text.strip()
    # Split by '---' on its own line.
    raw_slides = re.split(r"^---\s*$", text, flags=re.MULTILINE)
    return [s.strip() for s in raw_slides if s.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="docs/slides.md")
    ap.add_argument("--out", default="docs/slides.html")
    ap.add_argument("--title", default="CSE220 Final Project — SPP in Scarab")
    args = ap.parse_args()

    md_path = Path(args.input)
    md_text = md_path.read_text()
    slides = split_slides(md_text)

    md = markdown.Markdown(extensions=["fenced_code", "tables", "attr_list", "sane_lists"])
    rendered = []
    for s in slides:
        md.reset()
        html = md.convert(s)
        rendered.append(f"<section>{html}</section>")
    out_html = TEMPLATE.format(
        title=args.title,
        rev_ver=REVEAL_VERSION,
        slides="\n".join(rendered),
    )
    Path(args.out).write_text(out_html)
    print(f"Wrote {args.out} ({len(slides)} slides)")


if __name__ == "__main__":
    main()
