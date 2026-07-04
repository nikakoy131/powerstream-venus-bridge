#!/usr/bin/env python3
"""Regenerate src/index_html.h from src/web/index.html.

Run after any edit to index.html:
    python3 src/web/genheader.py
"""
import pathlib

here = pathlib.Path(__file__).resolve().parent
src = (here / "index.html").read_text(encoding="utf-8")

lines = []
for line in src.split("\n"):
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append(f'"{esc}\\n"')
# Drop the trailing empty line artifact if the file ends with a newline.
if lines and lines[-1] == '"\\n"':
    lines.pop()

out = "#pragma once\n\n/* Generated from src/web/index.html by src/web/genheader.py — do not edit. */\n\nstatic const char INDEX_HTML[] =\n" + "\n".join(lines) + "\n;\n"
(here.parent / "index_html.h").write_text(out, encoding="utf-8")
print(f"index_html.h: {len(out)} bytes")
