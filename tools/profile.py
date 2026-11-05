#!/usr/bin/env python3
"""Names what MHFU_PROFILE sampled: profile.py BINARY SAMPLES [BINARY SAMPLES]... Each sample's function from the binary's symbols, most
sampled first; with several, side by side as a share of each."""
import bisect
import collections
import struct
import subprocess
import sys


def table(binary):
    rows = []
    for line in subprocess.run(["nm", "-n", binary], capture_output=True, text=True).stdout.split("\n"):
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            rows.append((int(p[0], 16), p[2]))
    return [a for a, _ in rows], [n for _, n in rows]


def count(binary, path):
    addrs, names = table(binary)
    with open(path, "rb") as f:
        data = f.read()
    c = collections.Counter()
    for (rip,) in struct.iter_unpack("<Q", data):
        i = bisect.bisect_right(addrs, rip) - 1
        c[names[i] if 0 <= i and rip < addrs[-1] + 0x100000 else "(outside: a library)"] += 1
    return c, sum(c.values())


def main():
    args = sys.argv[1:]
    runs = [count(args[i], args[i + 1]) for i in range(0, len(args), 2)]
    names = collections.Counter()
    for c, n in runs:
        for k, v in c.items():
            names[k] += v / n
    print("  ".join(f"{n:>8}" for _, n in runs), "samples")
    for name, _ in names.most_common(40):
        print("  ".join(f"{100 * c[name] / n:7.2f}%" for c, n in runs), name)


if __name__ == "__main__":
    main()
