#!/usr/bin/env python3
"""What the stall recorder saw (MHFU_STALL, runtime/src/stall.c): stalls.py LOG [--bin build/mhfu] [--top 6] [--each]

For the whole log: how many stalls and how long, how much of them was on the CPU, and the functions most often on the
stack of the game's thread and of the render thread, innermost first in each line."""
import argparse
import bisect
import collections
import os
import subprocess


def table(binary):
    rows = []
    for line in subprocess.run(["nm", "-n", binary], capture_output=True, text=True).stdout.split("\n"):
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            rows.append((int(p[0], 16), p[2]))
    return [a for a, _ in rows], [n for _, n in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "mhfu"))
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--each", action="store_true", help="each stall's samples too")
    args = ap.parse_args()
    addrs, names = table(args.bin)
    libs, stalls = [], []
    for line in open(args.log, errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "M" and len(p) >= 7:
            a, b = p[1].split("-")
            libs.append((int(a, 16), int(b, 16), p[6].rsplit("/", 1)[-1]))
        elif p[0] == "S":
            stalls.append({"at": float(p[1]), "gap": float(p[2]), "frame": int(p[3]), "cpu": float(p[4]), "faults": int(p[5]),
                           "major": int(p[6]), "waits": int(p[7]), "preempted": int(p[8]), "samples": []})
        elif p[0] == "P" and stalls:
            stalls[-1]["samples"].append((int(p[1]), float(p[2]), p[3], [int(x, 16) for x in p[4:]]))

    def name(pc):
        i = bisect.bisect_right(addrs, pc) - 1
        if i >= 0 and pc < addrs[-1] + 0x100000:
            return names[i]
        return next((f"({n})" for a, b, n in libs if a <= pc < b), "?")

    def stack(pcs):
        out = []
        for pc in pcs[2:]:              # (the handler's own, and the signal's return)
            n = name(pc)
            if not out or out[-1] != n:
                out.append(n)
        return out

    print(f"{len(stalls)} stalls")
    if not stalls:
        return
    gaps = sorted(s["gap"] for s in stalls)
    print(f"gap: median {gaps[len(gaps) // 2]:.1f} ms, longest {gaps[-1]:.1f} ms; on the CPU: "
          f"{100 * sum(s['cpu'] for s in stalls) / sum(gaps):.0f}% of the time; page faults {sum(s['faults'] for s in stalls)}, "
          f"major {sum(s['major'] for s in stalls)}; waits {sum(s['waits'] for s in stalls)}, preempted {sum(s['preempted'] for s in stalls)}")
    if args.each:
        for s in stalls:
            print(f"\n{s['at']:.2f} s, frame {s['frame']}: {s['gap']:.1f} ms, CPU {s['cpu']:.1f}, faults {s['faults']}, major {s['major']}, "
                  f"waits {s['waits']}, preempted {s['preempted']}")
            for th, at, what, pcs in s["samples"]:
                print(f"  {'game' if th == 0 else 'render'} {what} {at:6.1f} " + " < ".join(stack(pcs)[:8]))
        print()
    for th, label in ((0, "game's thread"), (1, "render thread")):
        top, inner, n = collections.Counter(), collections.Counter(), 0
        for s in stalls:
            for t, at, what, pcs in s["samples"]:
                if t != th:
                    continue
                st = stack(pcs)
                n += 1
                inner[st[0] if st else "?"] += 1
                top[" < ".join(st[:5])] += 1
        print(f"\n{label}: {n} samples\n innermost:")
        for k, v in inner.most_common(args.top):
            print(f"  {100 * v / max(n, 1):5.1f}%  {k}")
        print(" stacks:")
        for k, v in top.most_common(args.top):
            print(f"  {100 * v / max(n, 1):5.1f}%  {k}")


if __name__ == "__main__":
    main()
