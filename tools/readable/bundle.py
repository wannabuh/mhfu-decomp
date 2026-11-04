#!/usr/bin/env python3
"""Naming bundles: bundle.py WORK VIEW OUT. Groups the functions that ran and have no name yet, one body each, by adjacency in
their module (a source file's functions stay together), into files of about MAX_LINES lines: each function's code, callers,
the recordings it ran in, and its vtable slots. A bundle holds what an agent needs, so it needn't go looking. Order: EBOOT,
game_task, game_sub, monsters, stages, lobby_task, the rest. Bundles hold game code: they stay out of git."""
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from whatran import Trace, load_modules, containing  # noqa: E402

MAX_LINES = 2500
MAX_FN = 400


def norm(text):
    return hashlib.md5(re.sub(r"\b(f_[A-Za-z0-9_]+?_[0-9A-F]{6,8}|[a-z_0-9]+_[0-9A-F]{7,8}|FUN_[0-9a-f]+|DAT_[0-9a-f]+|LAB_[0-9a-f]+|"
                              r"PTR_\w+|0x[0-9a-f]{7,8}|switchD_\w+|code_r0x\w+)\b", "X", text).encode()).hexdigest()


def rank(m):
    if m == "EBOOT":
        return 0
    if m == "game_task":
        return 1
    if m == "game_sub":
        return 2
    if m.startswith("em"):
        return 3
    if m.startswith("stage"):
        return 4
    if m == "lobby_task":
        return 5
    return 6


def main():
    work, view, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    mods = load_modules(work)
    known = set()
    if os.path.exists(os.path.join(work, "view-names.tsv")):
        with open(os.path.join(work, "view-names.tsv")) as f:
            for line in f:
                p = line.split("\t")
                known.add((p[0], int(p[1], 16)))
    with open(os.path.join(work, "methods.json")) as f:
        methods = json.load(f)
    callers, ran = collections.defaultdict(set), collections.defaultdict(dict)
    for path in sorted(glob.glob(os.path.join(work, "traces", "*.trace"))):
        t = Trace(path, mods, work)
        active = collections.defaultdict(set)
        for frame, m, _f, _r in t.records:
            active[frame].add(mods[m][0])
        for frame, m, fn, ra in t.records:
            key = (mods[m][0], fn)
            c = t.caller(active[frame], ra)
            if c:
                callers[key].add(c)
            span = ran[key].setdefault(t.name, [frame, frame])
            span[1] = max(span[1], frame)
    # one body each
    seen, dupes, todo = {}, {}, []
    for key in sorted(ran, key=lambda k: (rank(k[0]), k[0], k[1])):
        m, fn = key
        path = os.path.join(view, m, f"{fn:X}.c")
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as f:
            text = f.read()
        body = text.split("\n", 1)[1] if "\n" in text else ""
        h = norm(body)
        if h in seen:
            dupes.setdefault(f"{seen[h][0]} {seen[h][1]:X}", []).append(f"{m} {fn:X}")
            continue
        seen[h] = key
        if key not in known:
            todo.append((m, fn, text))
    with open(os.path.join(work, "dupes.json"), "w") as f:
        json.dump(dupes, f)
    chunks, cur, lines, last = [], [], 0, None
    for m, fn, text in todo:
        n = min(text.count("\n"), MAX_FN)
        gap = last is None or last[0] != m
        if cur and (gap or lines + n > MAX_LINES):
            chunks.append(cur)
            cur, lines = [], 0
        cur.append((m, fn, text))
        lines += n + 6
        last = (m, fn)
    if cur:
        chunks.append(cur)
    index = []
    for i, chunk in enumerate(chunks):
        name = f"{i:03}-{chunk[0][0]}.md"
        with open(os.path.join(out, name), "w") as f:
            for m, fn, text in chunk:
                key = (m, fn)
                who = sorted(callers.get(key, ()))
                callers_text = ", ".join(f"{c[0]} {c[1]:08X}" for c in who[:8]) + (" ..." if len(who) > 8 else "")
                recs = ", ".join(f"{k.replace('overnight-', '')} {v[0]}-{v[1]}" for k, v in sorted(ran[key].items())[:6])
                slots = methods.get(f"{m} {fn:x}", [])
                slot_text = ", ".join(f"vtable {v} +0x{o:X}" for v, o in slots[:5]) + (" ..." if len(slots) > 5 else "")
                f.write(f"\n=== {m} {fn:08X}\ncallers: {callers_text or '(none in traces)'}\nran: {recs}\n")
                if slot_text:
                    f.write(f"methods: {slot_text}\n")
                ls = text.split("\n")
                f.write("\n".join(ls[1:MAX_FN + 1]) + "\n")
                if len(ls) > MAX_FN + 1:
                    f.write(f"[... {len(ls) - MAX_FN - 1} more lines: {view}/{m}/{fn:X}.c]\n")
        index.append((name, len(chunk)))
    with open(os.path.join(out, "index.tsv"), "w") as f:
        for name, n in index:
            f.write(f"{name}\t{n}\n")
    print(f"{len(todo)} functions in {len(chunks)} bundles; {sum(map(len, dupes.values()))} duplicates carry a name over")
    count = collections.Counter(n.split("-", 1)[1][:-3] for n, _ in index)
    print(dict(count.most_common(12)))


if __name__ == "__main__":
    main()
