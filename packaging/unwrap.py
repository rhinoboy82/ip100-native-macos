#!/usr/bin/env python3
"""Join hard-wrapped lines into paragraphs so the macOS Installer can wrap text itself.
Blank lines separate paragraphs; lines starting with spaces, '-', digits or '---' keep their breaks."""
import re
import sys

out, para = [], []


def flush():
    if para:
        out.append(" ".join(para))
        para.clear()


for line in open(sys.argv[1], encoding="utf-8").read().splitlines():
    s = line.rstrip()
    if not s.strip():
        flush()
        out.append("")
    elif re.match(r"^(\s|-|\d+\.)", s):
        flush()
        out.append(s)
    else:
        para.append(s.strip())
flush()
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(out).rstrip() + "\n")
