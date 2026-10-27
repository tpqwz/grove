#!/usr/bin/env python3
"""Turn page.html into a C string header."""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
lines = [
    "/* generated from page.html. do not edit. */",
    "#ifndef GROVE_PAGE_H",
    "#define GROVE_PAGE_H",
    "static const char page_html[] =",
]
buf = []


def flush():
    if buf:
        lines.append('"' + "".join(buf) + '"')
        buf.clear()


for ch in text:
    if ch in '\\"':
        buf.append("\\" + ch)
    elif ch == "\n":
        buf.append("\\n")
        flush()
    elif ch == "\r":
        buf.append("\\r")
    elif ord(ch) < 32:
        buf.append("\\%03o" % ord(ch))
    else:
        buf.append(ch)
    if len(buf) >= 100:
        flush()
flush()
lines.append(";")
lines.append("#endif")
open(dst, "w", encoding="utf-8").write("\n".join(lines) + "\n")
