#!/usr/bin/env python3
"""What the code does with each class's fields, for naming them: fieldlist.py WORK OUT VTABLE... Writes OUT/<VTABLE>.tsv: offset,
whether a base class's code reaches it too, accesses as kind*count (W for writes), what it points at, and the named functions that reach it. From the game: stays out of git."""
import json
import os
import sys


def main():
    work, out, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(work, "classes.json")) as f:
        classes = json.load(f)["classes"]
    names = {}
    with open(os.path.join(work, "view-names.tsv")) as f:
        for line in f:
            p = line.split("\t")
            names[f"{p[0]} {p[1]}"] = p[2]
    for v in want:
        with open(os.path.join(out, v + ".tsv"), "w") as f:
            f.write("offset\tin_base\taccess\tpoints_at\tderef\tseen_in\n")
            up, a = set(), classes[v]["parent"]
            while a:
                up |= set(classes[a]["fields"])
                a = classes[a]["parent"]
            for off, c in sorted(classes[v]["fields"].items(), key=lambda kv: int(kv[0], 16)):
                acc = " ".join(f"{k}{'W' if w else ''}*{n}" for k, _s, w, n in c["access"])
                seen = ", ".join(f"{s} ({names.get(s, '?')})" for s in c["seen_in"])
                f.write(f"{int(off, 16):X}\t{'yes' if off in up else ''}\t{acc}\t{c['pointer'] or ''}\t{'yes' if c['deref'] else ''}\t{seen}\n")


if __name__ == "__main__":
    main()
