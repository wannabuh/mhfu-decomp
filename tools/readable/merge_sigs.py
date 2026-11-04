#!/usr/bin/env python3
"""Merge a pass's signatures into the running ones: merge_sigs.py PASS_DIR SIGNATURES. A function's arguments are the union
of those of every pass (a register read stays an argument; more show as callees' signatures become known); its return
register is the last pass's that has one. Prints how many functions changed: all.sh repeats passes until that settles."""
import glob
import os
import sys

ORDER = ["a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19"]


def main():
    pass_dir, path = sys.argv[1:]
    sigs = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                m, a, ret, args = line.rstrip("\n").split("\t")
                sigs[(m, a)] = (ret, set(filter(None, args.split(","))))
    changed = 0
    for tsv in glob.glob(os.path.join(pass_dir, "*", "signatures.tsv")):
        m = os.path.basename(os.path.dirname(tsv))
        with open(tsv) as f:
            for line in f:
                a, ret, args = line.rstrip("\n").split("\t")
                old = sigs.get((m, a))
                new_args = set(filter(None, args.split(",")))
                if old is None:
                    sigs[(m, a)] = (ret, new_args)
                    changed += 1
                    continue
                merged = (ret if ret != "-" else old[0], old[1] | new_args)
                if merged != old:
                    changed += 1
                sigs[(m, a)] = merged
    with open(path + ".new", "w") as f:
        for (m, a), (ret, args) in sorted(sigs.items()):
            f.write(f"{m}\t{a}\t{ret}\t{','.join(r for r in ORDER if r in args)}\n")
    os.replace(path + ".new", path)
    print(f"{changed} of {len(sigs)} signatures changed", flush=True)
    return changed


if __name__ == "__main__":
    main()
