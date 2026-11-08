"""List log()/printf string literals long enough to overflow the 80-column
log budget once concatenated. Run from the scriba repo root."""
import glob
import re

DQ = re.compile(r'"((?:[^"\\]|\\.)*)"')
SQ = re.compile(r"'([^'\\]*)'")
CALL = re.compile(r"\b(log|printf|fprintf|snprintf)\s*\(")

files = ["web/src/app.js"] + glob.glob("src/*.c") + glob.glob("web/src/*.c")
for f in files:
    with open(f, encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    for i, line in enumerate(lines, 1):
        if not CALL.search(line):
            continue
        # the call and up to 3 continuation lines
        text = "".join(lines[i - 1:i + 3])
        lits = DQ.findall(text) + (SQ.findall(text) if f.endswith(".js") else [])
        total = sum(len(s) for s in lits)
        if total > 70:
            print(f"{f}:{i}: ~{total}: {' + '.join(lits)[:120]}")
