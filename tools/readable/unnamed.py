#!/usr/bin/env python3
"""Functions with no name yet, most called first, for naming: unnamed.py WORK GEN OUT. Writes OUT/<group>.tsv: address, module, direct calls
to it in the generated C, its lines, whether it ran in a recording, and up to 5 named callers. A function called from many places makes all of
them easier to read. Stages and monsters are left out: much of them is copies. From the game: stays out of git."""
import collections
import glob
import json
import os
import re
import sys

WORK, GEN, OUT = sys.argv[1:4]
named = set()
with open(os.path.join(WORK, "view-names.tsv")) as f:
    for line in f:
        m, a, name = line.split("\t")[:3]
        named.add((m, int(a, 16)))
names = {}
with open(os.path.join(WORK, "view-names.tsv")) as f:
    for line in f:
        p = line.split("\t")
        names[f"f_{p[0]}_{int(p[1], 16):08X}"] = p[2]
calls = collections.Counter()
callers = collections.defaultdict(set)
lines = {}
start = re.compile(r"^void (f_(\w+?)_([0-9A-F]{8}))\(Ctx \*c\) \{")
call = re.compile(r"\b(f_\w+?_[0-9A-F]{8})\(c\)")
for path in glob.glob(os.path.join(GEN, "*.c")):
    if os.path.basename(path).startswith("tables"):
        continue
    cur = None
    with open(path, errors="replace") as f:
        for text in f:
            m = start.match(text)
            if m:
                cur = m.group(1)
                lines[cur] = 0
                continue
            if cur:
                if text.startswith("}"):
                    cur = None
                    continue
                lines[cur] += 1
                # (a generated function ends with a call of the next, for falling through: no address on it, not a call)
                if text.lstrip().startswith("f_") and "/*" not in text:
                    continue
                for callee in call.findall(text):
                    if callee != cur:
                        calls[callee] += 1
                        callers[callee].add(cur)
with open(os.path.join(WORK, "coverage.json")) as f:
    coverage = json.load(f)
ran = set()
for m, fns in coverage.items():
    for a in (fns if isinstance(fns, (list, dict)) else []):
        ran.add((m, int(a, 16) if isinstance(a, str) else a))
os.makedirs(OUT, exist_ok=True)
groups = collections.defaultdict(list)
for fn, n in lines.items():
    m = re.match(r"f_(\w+?)_([0-9A-F]{8})$", fn)
    module, addr = m.group(1), int(m.group(2), 16)
    if (module, addr) in named or module.startswith("stage") or re.match(r"em\d", module):
        continue
    groups[module].append((calls[fn], n, addr, module, fn))
for module, rows in groups.items():
    rows.sort(reverse=True)
    with open(os.path.join(OUT, module + ".tsv"), "w") as f:
        f.write("address\tmodule\tcalls\tlines\tcallers\n")
        for c, n, addr, mod, fn in rows:
            who = sorted(names[x] for x in callers[fn] if x in names)[:5]
            f.write(f"{addr:08X}\t{mod}\t{c}\t{n}\t{', '.join(who)}\n")
print({k: len(v) for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:8]})
