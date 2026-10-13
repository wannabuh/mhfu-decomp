#!/usr/bin/env python3
"""Find a game variable from RAM snapshots (F9 in the game: build/snap-*.bin, 24 MB from 0x08800000).

Give the snapshots in order and, for each step between two of them, what the variable did: '+' it went up, '-' down,
'=' stayed, '?' anything. Prints the 32-bit floats (and, with --int, the 32-bit integers) that did exactly that.

    tools/ramdiff.py build/snap-*-0{1,2,3,4}.bin --steps '+ + ='     # rose, rose, then held
"""
import argparse
import array
import sys

BASE = 0x08800000


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("snapshots", nargs="+")
    ap.add_argument("--steps", required=True, help="per step between snapshots: + - = ? (or 'c' changed)")
    ap.add_argument("--int", action="store_true", help="integers instead of floats")
    ap.add_argument("--limit", type=int, default=60)
    args = ap.parse_args()
    steps = args.steps.split()
    if len(steps) != len(args.snapshots) - 1:
        sys.exit("one step for each pair of consecutive snapshots")
    snaps = []
    for path in args.snapshots:
        a = array.array("i" if args.int else "f")
        with open(path, "rb") as f:
            a.frombytes(f.read())
        snaps.append(a)
    found = 0
    for i in range(len(snaps[0])):
        vals = [s[i] for s in snaps]
        if not args.int and not all(v == v and abs(v) < 1e8 for v in vals):
            continue
        ok = True
        for k, st in enumerate(steps):
            d = vals[k + 1] - vals[k]
            if (st == "+" and not d > 0) or (st == "-" and not d < 0) or (st == "=" and d != 0) or \
                    (st == "c" and d == 0):
                ok = False
                break
        if ok:
            print(f"{BASE + 4 * i:08X}", " ".join(f"{v:.5g}" if not args.int else str(v) for v in vals))
            found += 1
            if found >= args.limit:
                print("...")
                break
    print(f"{found} found", file=sys.stderr)


if __name__ == "__main__":
    main()
