#!/usr/bin/env python3
"""Names what MHFU_PROFILE sampled: profile.py BINARY SAMPLES [BINARY SAMPLES]... Each sample's function from the binary's symbols, most
sampled first; with several, side by side as a share of each.
profile.py --threads BINARY SAMPLES: the game's work, the display's frames and other threads apart."""
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


def libraries(path):
    """What MHFU_PROFILE noted of the process's libraries: (start, end, name), by address"""
    rows = []
    try:
        with open(path + ".maps") as f:
            for line in f:
                p = line.split()
                if len(p) >= 6 and p[1].startswith("r") and "x" in p[1]:
                    a, b = p[0].split("-")
                    rows.append((int(a, 16), int(b, 16), p[5].rsplit("/", 1)[-1]))
    except OSError:
        pass
    return rows


def count(binary, path, only=None):
    addrs, names = table(binary)
    libs = libraries(path)
    with open(path, "rb") as f:
        data = f.read()
    c = collections.Counter()
    for (word,) in struct.iter_unpack("<Q", data):
        rip, tag = word & (1 << 56) - 1, word >> 56
        if only is not None and tag != only:
            continue
        i = bisect.bisect_right(addrs, rip) - 1
        if 0 <= i and rip < addrs[-1] + 0x100000:
            c[names[i]] += 1
            continue
        lib = next((n for a, b, n in libs if a <= rip < b), "?")
        c[f"(in {lib})"] += 1
    return c, sum(c.values())


def show(runs, top=40):
    names = collections.Counter()
    for c, n in runs:
        for k, v in c.items():
            names[k] += v / max(n, 1)
    print("  ".join(f"{n:>8}" for _, n in runs), "samples")
    for name, _ in names.most_common(top):
        print("  ".join(f"{100 * c[name] / max(n, 1):7.2f}%" for c, n in runs), name)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--threads":     # one run, by what the thread was at
        for tag, what in ((1, "the game's thread: the game's work (ticks)"), (2, "the game's thread: the display's frames"),
                          (0, "other threads (render)")):
            print(f"-- {what}")
            show([count(args[1], args[2], tag)], 30)
        return
    show([count(args[i], args[i + 1]) for i in range(0, len(args), 2)])


if __name__ == "__main__":
    main()
