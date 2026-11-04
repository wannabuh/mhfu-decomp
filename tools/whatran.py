#!/usr/bin/env python3
"""What ran: from traces made by tools/readable/trace.py, which functions ran over a span of game frames, and who called them.

  whatran.py coverage                         functions run, by module, over all traces (writes coverage.json)
  whatran.py during REC --from F --to T       functions that ran in those frames, most specific first
      --minus REC2 | --minus-frames F2 T2     leaving out what also ran there: what is special about these frames
      --tree                                  as a call tree instead of a list
  whatran.py where ADDR [--module M]          which recordings and frames a function ran in, and its callers

Traces are in spans of 10 game frames. Function names come from tools/readable/names/*.toml, when there.
"""
import argparse
import bisect
import collections
import glob
import json
import os
import struct
import sys
import tomllib

HOME = os.path.expanduser("~/projects/mhfu-ghidra/work")
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def load_modules(work):
    mods = []
    with open(os.path.join(work, "modules.tsv")) as f:
        for line in f:
            name, base, size = line.rstrip("\n").split("\t")
            mods.append((name, int(base, 16), int(size, 16)))
    return mods


def load_funcs(work, name, cache={}):
    if name not in cache:
        rows = []
        path = os.path.join(work, name, "funcs.tsv")
        if os.path.exists(path):
            with open(path) as f:
                for line in f:
                    p = line.split("\t")
                    rows.append((int(p[0], 16), int(p[1], 16)))
        rows.sort()
        cache[name] = ([r[0] for r in rows], rows)
    return cache[name]


def containing(work, name, addr):
    starts, rows = load_funcs(work, name)
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and rows[i][0] <= addr < rows[i][1]:
        return rows[i][0]
    return None


def load_names():
    names = {}
    for path in glob.glob(os.path.join(ROOT, "tools/readable/names/*.toml")):
        with open(path, "rb") as f:
            data = tomllib.load(f)
        for e in data.get("function", []):
            names[(e["module"], int(e["address"], 16))] = e["name"]
    return names


class Trace:
    def __init__(self, path, mods, work):
        self.mods, self.work = mods, work
        with open(path, "rb") as f:
            data = f.read()
        self.records = list(struct.iter_unpack("<IIII", data))
        self.name = os.path.basename(path)[:-len(".trace")]

    def caller(self, active, ra):
        """(module, function) a return address is in; among overlays sharing a region, one running in that span."""
        for name, base, size in self.mods[:1]:
            f = containing(self.work, name, ra - 8)
            if f is not None:
                return name, f
        for name in active:
            f = containing(self.work, name, ra - 8) if name != "EBOOT" else None
            if f is not None:
                return name, f
        return None

    def span(self, start, end):
        """{(module, fn): {callers}}, for records in [start, end)."""
        active = collections.defaultdict(set)
        for frame, m, _fn, _ra in self.records:
            if start <= frame < end:
                active[frame].add(self.mods[m][0])
        out = collections.defaultdict(set)
        for frame, m, fn, ra in self.records:
            if start <= frame < end:
                out[(self.mods[m][0], fn)].add(self.caller(active[frame], ra))
        return out


def label(names, key):
    if key is None:
        return "(outside the game's code)"
    n = names.get(key)
    return f"{key[0]} {key[1]:08X}" + (f" {n}" if n else "")


def traces(args, mods):
    return {os.path.basename(p)[:-len(".trace")]: p for p in sorted(glob.glob(os.path.join(args.traces, "*.trace")))}


def coverage(args, mods):
    seen = collections.defaultdict(set)
    per = {}
    for name, path in traces(args, mods).items():
        t = Trace(path, mods, args.work)
        got = {(mods[m][0], fn) for _f, m, fn, _ra in t.records}
        per[name] = len(got)
        for mod, fn in got:
            seen[mod].add(fn)
    total = 0
    for name, _b, _s in mods:
        n = len(load_funcs(args.work, name)[1])
        total += n
        if seen.get(name):
            print(f"{name:16} {len(seen[name]):6} of {n}")
    run = sum(map(len, seen.values()))
    print(f"{'all':16} {run:6} of {total}")
    print("by recording:", per)
    with open(os.path.join(args.work, "coverage.json"), "w") as f:
        json.dump({k: sorted(v) for k, v in seen.items()}, f)


def resolve(all_traces, rec):
    for name in (rec, "overnight-" + rec):
        if name in all_traces:
            return all_traces[name]
    sys.exit(f"no trace {rec}: {', '.join(all_traces)}")


def during(args, mods):
    all_traces = traces(args, mods)
    names = load_names()
    t = Trace(resolve(all_traces, args.rec), mods, args.work)
    here = t.span(args.start, args.end)
    other = set()
    if args.minus:
        o = Trace(resolve(all_traces, args.minus), mods, args.work)
        other = set(o.span(0, 1 << 31))
    if args.minus_frames:
        other |= set(t.span(args.minus_frames[0], args.minus_frames[1]))
    keep = {k: v for k, v in here.items() if k not in other}
    print(f"{len(keep)} functions in {t.name} frames {args.start}..{args.end}"
          + (f" (of {len(here)}, the rest ran in the baseline too)" if other else ""))
    if args.tree:
        children = collections.defaultdict(set)
        for k, callers in keep.items():
            for c in callers:
                children[c if c in keep else None].add(k)

        def show(key, depth, path):
            for child in sorted(children.get(key, ()), key=lambda k: (k[0], k[1])):
                print("  " * depth + label(names, child) + (" (recursion)" if child in path else ""))
                if child not in path:
                    show(child, depth + 1, path | {child})
        show(None, 0, frozenset())
    else:
        for k, callers in sorted(keep.items()):
            print(f"{label(names, k):60}  from {', '.join(sorted(label(names, c) for c in callers))}"[:170])


def where(args, mods):
    names = load_names()
    want = int(args.addr, 16)
    for name, path in traces(args, mods).items():
        t = Trace(path, mods, args.work)
        frames, callers = [], set()
        active = collections.defaultdict(set)
        for frame, m, _fn, _ra in t.records:
            active[frame].add(mods[m][0])
        for frame, m, fn, ra in t.records:
            if fn == want and (not args.module or mods[m][0] == args.module):
                frames.append(frame)
                callers.add(t.caller(active[frame], ra))
        if frames:
            print(f"{name}: {len(set(frames))} spans, frames {min(frames)}..{max(frames)}; from "
                  + ", ".join(sorted(label(names, c) for c in callers)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=HOME)
    ap.add_argument("--traces", default=os.path.join(HOME, "traces"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("coverage")
    d = sub.add_parser("during")
    d.add_argument("rec")
    d.add_argument("--from", dest="start", type=int, required=True)
    d.add_argument("--to", dest="end", type=int, required=True)
    d.add_argument("--minus")
    d.add_argument("--minus-frames", type=int, nargs=2)
    d.add_argument("--tree", action="store_true")
    w = sub.add_parser("where")
    w.add_argument("addr")
    w.add_argument("--module")
    args = ap.parse_args()
    mods = load_modules(args.work)
    {"coverage": coverage, "during": during, "where": where}[args.cmd](args, mods)


if __name__ == "__main__":
    main()
