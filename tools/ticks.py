#!/usr/bin/env python3
"""What still ticks at 30, and how big it is (docs/ticks.md). Reads only: the generated C (build/gen), the names, and the
output of tools/readable/ticks_hook.c (which functions ran on the frames between 30ths, under a gate, or neither).

  ticks.py static [--gen DIR]                 per overlay and per class of monster: lines, and steps a rate could hide in
  ticks.py ran FILE [--root ADDR ...]         a hook file summed: functions and generated lines every frame / at 30ths only
  ticks.py tree FILE ADDR [--depth N]         the callers' graph under a function, each with the frames it ran on
  ticks.py systems FILE...                    the same, by the systems behind the gates (SYSTEMS below)
  ticks.py streak TRACE@FROM...               fields of a traced object still counting game frames
  ticks.py convert SRC DST RATE AFTER         a recording made at 30 at RATE a second after frame AFTER (any rate)

The steps counted by `static` are candidates, read from the generated C, not findings: an integer +1 or -1 stored straight
back to a field (a count of game frames, or of anything else), a float add stored straight back (a velocity, an ease, a
place), and those patched already (rt_tick30, rt_ticks60, rt_time_scale).
"""
import argparse
import collections
import glob
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.expanduser("~/projects/mhfu-ghidra/work")

DEF = re.compile(r"^void f_(\w+?)_([0-9A-F]{8})\(Ctx \*c\) \{")
CALL = re.compile(r"\bf_(\w+?)_([0-9A-F]{8})\(c\)")
INT_STEP = re.compile(r"c->r\[(\d+)\] = c->r\[\1\] (?:\+ 0x1u|\+ 0xFFFFFFFFu|- 0x1u);")
FLOAT_ADD = re.compile(r"c->f\[(\d+)\]\.f = c->f\[\d+\]\.f [-+] c->f\[\d+\]\.f;")
STORE_INT = re.compile(r"\bS[BHW]\(\(?c->r\[\d+\] \+ 0x[0-9A-Fa-f]+u\)?, c->r\[(\d+)\]\)")
STORE_FLOAT = re.compile(r"\bSW\(\(?c->r\[\d+\] \+ 0x[0-9A-Fa-f]+u\)?, c->f\[(\d+)\]\.u\)")
RATE = re.compile(r"rt_tick30|rt_ticks60|rt_time_scale|rt_ease")


def load_gen(gen=None):
    """Every generated function: (module, addr) -> {lines, ints, floats, patched, calls}"""
    gen = gen or os.path.join(ROOT, "build", "gen")
    out = {}
    for path in sorted(glob.glob(os.path.join(gen, "*.c"))):
        cur = None
        prev = []
        for line in open(path, errors="replace"):
            m = DEF.match(line)
            if m:
                cur = out.setdefault((m.group(1), int(m.group(2), 16)),
                                     dict(lines=0, ints=0, floats=0, patched=0, calls=set()))
                prev = []
                continue
            if cur is None:
                continue
            cur["lines"] += 1
            if RATE.search(line):
                cur["patched"] += 1
            for c in CALL.finditer(line):
                cur["calls"].add((c.group(1), int(c.group(2), 16)))
            for p, kind in prev[-2:]:
                if kind[0] == "i":
                    s = STORE_INT.search(line)
                    if s and s.group(1) == kind[1:]:
                        cur["ints"] += 1
                else:
                    s = STORE_FLOAT.search(line)
                    if s and s.group(1) == kind[1:]:
                        cur["floats"] += 1
            m = INT_STEP.search(line)
            if m:
                prev.append((line, "i" + m.group(1)))
            m = FLOAT_ADD.search(line)
            if m:
                prev.append((line, "f" + m.group(1)))
            prev = prev[-2:]
    return out


def names():
    import tomllib
    out = {}
    for path in glob.glob(os.path.join(ROOT, "tools", "readable", "names", "*.toml")):
        with open(path, "rb") as f:
            d = tomllib.load(f)
        for rows in d.values():
            if isinstance(rows, list):
                for r in rows:
                    if "address" in r and "name" in r:
                        out[int(str(r["address"]), 16)] = r["name"]
    return out


def add(total, f):
    total["funcs"] += 1
    for k in ("lines", "ints", "floats", "patched"):
        total[k] += f[k]


def row(name, t):
    return f"{name:34} {t['funcs']:6} {t['lines']:8} {t['ints']:6} {t['floats']:6} {t['patched']:6}"


HEAD = f"{'':34} {'funcs':>6} {'lines':>8} {'ints':>6} {'floats':>6} {'scaled':>6}"


def cmd_static(args):
    gen = load_gen(args.gen)
    by_mod = collections.defaultdict(lambda: collections.Counter())
    for (mod, _), f in gen.items():
        add(by_mod[mod], f)
    print(HEAD)
    for mod in sorted(by_mod, key=lambda m: -by_mod[m]["lines"]):
        if mod == "EBOOT" or mod.startswith(("em", "game_")):
            print(row(mod, by_mod[mod]))
    # The monsters of game_task and their code: the span of the functions their vtables hold alone
    with open(os.path.join(WORK, "vtables.json")) as fh:
        vts = json.load(fh)
    import tomllib
    with open(os.path.join(ROOT, "tools", "readable", "classes.toml"), "rb") as fh:
        classes = tomllib.load(fh)["class"]
    uses = collections.Counter()
    for vt in vts.values():
        for s in set(tuple(v) for v in vt.values()):
            uses[s] += 1
    print("\nclasses of monster: what their vtable's slots reach by direct calls, that no other class's slots reach (read from the "
          "generated C: state tables held in data are not followed)")
    print(HEAD)
    monsters = [c for c in classes if c.get("base") == "Monster" and c["vtable"] in vts]
    reached = {}
    for c in monsters:
        stack = [(m, int(a, 16)) for m, a in vts[c["vtable"]].values()]
        seen = set()
        while stack:
            k = stack.pop()
            if k in seen or k not in gen:
                continue
            seen.add(k)
            stack.extend(gen[k]["calls"])
        reached[c["vtable"]] = seen
    count = collections.Counter(k for r in reached.values() for k in r)
    for c in monsters:
        t, sh = collections.Counter(), collections.Counter()
        for k in reached[c["vtable"]]:
            add(t if count[k] == 1 else sh, gen[k])
        print(row(f"{c['name'][:25]} {c['vtable']}", t), f"shared with others: {sh['funcs']} funcs {sh['lines']} lines")


def read_hook(path):
    """A hook's file: (module number, fn, caller, class, calls)"""
    with open(os.path.join(WORK, "modules.tsv")) as fh:
        mods = [line.split("\t")[0] for line in fh]
    for line in open(path):
        p = line.split()
        yield mods[int(p[0])], int(p[1], 16), int(p[2], 16), int(p[3]), int(p[4])


CLASS = ("between", "gated", "native")


def cmd_ran(args):
    gen = load_gen(args.gen)
    fn = collections.defaultdict(lambda: [0, 0, 0])
    for mod, a, _, cls, calls in read_hook(args.file):
        fn[(mod, a)][cls] += calls
    every, only30, over = collections.Counter(), collections.Counter(), collections.Counter()
    by_mod = collections.defaultdict(lambda: [collections.Counter(), collections.Counter()])
    for k, (b, g, n) in fn.items():
        f = gen.get(k, dict(lines=0, ints=0, floats=0, patched=0))
        side = every if b or n else only30
        add(side, f)
        add(by_mod[k[0]][0 if b or n else 1], f)
        over["calls_between"] += b
        over["calls_gated"] += g
        over["calls_native"] += n
    print(HEAD)
    print(row("every frame (or at a 30th, ungated)", every))
    print(row("at 30ths only, under a gate", only30))
    print(dict(over))
    for m in sorted(by_mod, key=lambda m: -(by_mod[m][0]["lines"] + by_mod[m][1]["lines"])):
        print(row(m + " every frame", by_mod[m][0]))
        print(row(m + " at 30ths", by_mod[m][1]))


def cmd_tree(args):
    gen = load_gen(args.gen)
    nm = names()
    root = int(args.addr, 16)
    kids = collections.defaultdict(set)
    fn = collections.defaultdict(lambda: [0, 0, 0])
    funcs = sorted(gen)
    starts = collections.defaultdict(list)
    for m, a in funcs:
        starts[m].append(a)
    import bisect

    def owner(mod, ra):
        for m in (mod, "EBOOT", "game_task", "game_sub"):
            s = starts.get(m)
            if s:
                i = bisect.bisect_right(s, ra) - 1
                if i >= 0 and ra - s[i] < 0x8000:
                    return s[i]
        return None
    for mod, a, ra, cls, calls in read_hook(args.file):
        fn[a][cls] += calls
        o = owner(mod, ra)
        if o is not None:
            kids[o].add(a)
    seen = set()

    def walk(a, d):
        b, g, n = fn[a]
        print("  " * d + f"{a:08X} {nm.get(a, '')[:60]} between {b} gated {g} native {n}")
        if a in seen or d >= args.depth:
            return
        seen.add(a)
        for k in sorted(kids[a], key=lambda x: -sum(fn[x])):
            walk(k, d + 1)
    walk(root, 0)


# The systems under the gates (read from the hunt's call tree): where each is ticked from. What only one reaches is its own
SYSTEMS = {
    "monsters and small monsters (ticks)": [0x09AB13D8, 0x09AB3940, 0x09AB3998],
    "projectiles, attack volumes (owned objects)": [0x09B62D90],
    "hits": [0x09C41E78],
    "push and map collision": [0x09C37C68, 0x09A633B0],
    "particles, effects": [0x09C2D3E0],
    "stage: its state, its objects": [0x088C32E8, 0x088C33E8],
    "HUD": [0x0883F270],
    "sound": [0x08885F50, 0x08880708],
    "quest, timers": [0x0886B0E0, 0x08850FB8],
    "scene (camera_scene_tick)": [0x088CD230],
    "text": [0x08890BBC, 0x08891B14],
    "assets": [0x088DDA44],
    "pad": [0x08868F50],
    "hunt's other draw calls, fades": [0x09A628F8, 0x09A63480, 0x0884D2F8],
    "screen states": [0x0884C22C],
}


def callers(args, gen):
    """(kids, calls by function) from a hook's files: the callers told by return address, overlays' clashes taken as the
    module's, else the EBOOT's, game_task's, game_sub's"""
    import bisect
    starts = collections.defaultdict(list)
    for m, a in sorted(gen):
        starts[m].append(a)

    def owner(mod, ra):
        for m in (mod, "EBOOT", "game_task", "game_sub"):
            s = starts.get(m)
            if s:
                i = bisect.bisect_right(s, ra - 1) - 1
                if i >= 0 and ra - s[i] < 0x10000:
                    return (m, s[i])
        return None
    kids = collections.defaultdict(set)
    fn = collections.defaultdict(lambda: [0, 0, 0])
    for path in args.files:
        for mod, a, ra, cls, calls in read_hook(path):
            fn[(mod, a)][cls] += calls
            o = owner(mod, ra)
            if o is not None:
                kids[o].add((mod, a))
    return kids, fn


def cmd_systems(args):
    gen = load_gen(args.gen)
    kids, fn = callers(args, gen)
    by_addr = collections.defaultdict(list)
    for k in fn:
        by_addr[k[1]].append(k)
    reached = collections.defaultdict(set)
    # (what runs every frame, the hunter's above all, is another side: it isn't walked into)
    for name, roots in SYSTEMS.items():
        stack = [k for r in roots for k in by_addr[r]]
        seen = set()
        while stack:
            k = stack.pop()
            if k in seen:
                continue
            seen.add(k)
            stack.extend(x for x in kids[k])
        for k in seen:
            reached[k].add(name)
    print(HEAD, "   every-frame share of its own")
    for name in SYSTEMS:
        own, own_every = collections.Counter(), collections.Counter()
        for k, names_ in reached.items():
            if names_ == {name}:
                f = gen.get(k, dict(lines=0, ints=0, floats=0, patched=0))
                if fn[k][0]:
                    add(own_every, f)
                else:
                    add(own, f)
        print(row(name[:34], own), f"  + every frame too: {own_every['funcs']} funcs {own_every['lines']} lines")
    shared, every, rest = collections.Counter(), collections.Counter(), collections.Counter()
    for k in fn:
        f = gen.get(k, dict(lines=0, ints=0, floats=0, patched=0))
        if len(reached.get(k, ())) > 1:
            add(shared, f)
        elif not reached.get(k):
            add(every if fn[k][0] or fn[k][2] else rest, f)
    print(row("shared by several systems", shared))
    print(row("in none: every frame or native", every))
    print(row("in none: at 30ths only", rest))


def cmd_streak(args):
    """Fields counting game frames: by the same 1 on 3 or more frames in a row, small numbers. A count in 30ths steps every
    2nd frame at 60, every 4th or 5th at 144. (Not angles' low bytes, colours fading, a count in 60ths at 60: read the list)"""
    import numpy as np
    import tomllib
    words = 0x800
    fields = {}
    for g in glob.glob(os.path.join(ROOT, "tools", "readable", "fields", "*.toml")):
        with open(g, "rb") as fh:
            for r in tomllib.load(fh).get("field", []):
                fields.setdefault(r["offset"], f"{r.get('struct', '?')}.{r['name']}")
    total, first = collections.Counter(), {}
    for spec in args.traces:
        path, _, frm = spec.partition("@")
        rows, times = [], []
        for line in open(path):
            q = line.split()
            if line[0] == "#" or len(q) < 8 + words or float(q[0]) < float(frm or 0):
                continue
            times.append(float(q[0]))
            rows.append(np.fromiter((int(x, 16) for x in q[-words:]), dtype=np.uint32, count=words))
        r = np.array(rows)
        exp = (r >> 23) & 0xFF
        floaty = ((exp >= 0x60) & (exp <= 0x9F)).mean(axis=0) > 0.3
        for size, view in ((2, r.view(np.uint16)), (1, r.view(np.uint8))):
            v = view.astype(np.int64)
            mod = 1 << (8 * size)
            d = (np.diff(v, axis=0) + mod // 2) % mod - mod // 2
            for sign in (1, -1):
                one = d == sign
                one[:, np.repeat(floaty, 4 // size)] = False
                s = one[:-2] & one[1:-1] & one[2:] & (v[:-3] < 4096)
                for i in np.nonzero(s.sum(axis=0))[0]:
                    k = (int(i) * size, sign)
                    if k not in total or size == 2:
                        total[k] = max(total[k], int(s[:, i].sum()))
                        first.setdefault(k, times[int(np.argmax(s[:, i]))])
    for (off, sign), n in sorted(total.items()):
        print(f"+{off:04X} {'up' if sign > 0 else 'down':4} {n:6} frames, first at 30th {first[(off, sign)]:.1f}  {fields.get(off, '')}")


def convert(src, dst, rate, after, zero_mouse=False):
    k = rate / 30

    def at(f):
        # (the frame the same 30th falls on: thread.c's set_tick_hz makes the frame after the switch one, then every k frames)
        return f if f <= after else after + 1 + math.ceil((f - after - 1) * k - 0.005)
    out = []
    for line in open(src):
        p = line.split()
        if p and p[0].isdigit() and len(p) >= 6:
            if zero_mouse and int(p[0]) > after:
                p[4] = p[5] = "0"
            out.append(f"{at(int(p[0]))} {' '.join(p[1:])}")
        elif p and p[0] == "end":
            out.append(f"end {at(int(p[1]))}")
        elif p and p[0] == "pointer":
            out.append(f"pointer {at(int(p[1]))} {' '.join(p[2:])}")
        elif p and p[0] == "poke":
            a, _, b = p[1].partition("-")
            b = b or a
            last = at(int(b) + 1) - 1 if int(b) > after else at(int(b))
            out.append(f"poke {at(int(a))}-{last} {' '.join(p[2:])}")
        elif p and p[0] in ("camera", "follows"):
            out.append(f"{p[0]} {at(int(p[1])) if int(p[1]) else 0}")
        else:
            out.append(line.rstrip("\n"))
    with open(dst, "w") as f:
        f.write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("static")
    p.add_argument("--gen")
    p = sub.add_parser("ran")
    p.add_argument("file")
    p.add_argument("--gen")
    p = sub.add_parser("tree")
    p.add_argument("file")
    p.add_argument("addr")
    p.add_argument("--depth", type=int, default=3)
    p.add_argument("--gen")
    p = sub.add_parser("systems")
    p.add_argument("files", nargs="+")
    p.add_argument("--gen")
    p = sub.add_parser("streak")
    p.add_argument("traces", nargs="+", help="MHFU_NATIVE_TRACE files (MHFU_TRACE_WORDS=0,800), each @ the 30th to read from")
    p = sub.add_parser("convert")
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("rate", type=int)
    p.add_argument("after", type=int)
    p.add_argument("--zero-mouse", action="store_true", help="no mouse after that frame, at any rate: nothing steers the runs apart")
    args = ap.parse_args()
    if args.cmd == "static":
        cmd_static(args)
    elif args.cmd == "ran":
        cmd_ran(args)
    elif args.cmd == "tree":
        cmd_tree(args)
    elif args.cmd == "systems":
        cmd_systems(args)
    elif args.cmd == "streak":
        cmd_streak(args)
    elif args.cmd == "convert":
        convert(args.src, args.dst, args.rate, args.after, args.zero_mouse)


if __name__ == "__main__":
    sys.exit(main())
