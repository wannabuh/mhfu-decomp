#!/usr/bin/env python3
"""Code rewritten by hand against the generated code, by RAM: handdiff.py REC[:SAVES]... Replays each with MHFU_HAND=1 and 0 in the same
build, taking RAM every --every frames, and accounts for every word that differs. RAM hashes can't: the game copies uninitialised stack
bytes into some structures, and hand-written code leaves other bytes on the stack, without either changing what happens. Anything not on
the list of those fails. Add to the list only with evidence of why a place doesn't matter, as these:

- `stale_stack`: below any live frame. Stack contents are only read back by the function that wrote them.
- `sound_voice`: a word of the sound engine's voice records, built on the stack with that word unset. Audio output compared the same
  byte for byte on overnight-fight2 (MHFU_AUDIO_DUMP=1).
- `vector_w`: the unused fourth word of a position copied whole from the stack: in the last slot of 16 aligned bytes, after three float-like
  words the same in both runs, and not within a live hunter, monster or NPC, whose fields are never excused. Mostly effects' +0x1C.
"""
import argparse
import bisect
import collections
import concurrent.futures as cf
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("WORK", os.path.expanduser("~/projects/mhfu-ghidra/work"))
RAM = 0x08800000
VOICES = (0x09A0A000, 0x09A11000)
STACKS = (0x09E00000, 0x0A000000)
OBJECT_ROOT = "089BAC64"       # hunters, monsters, NPCs


def character_vtables():
    with open(os.path.join(WORK, "classes.json")) as f:
        classes = json.load(f)["classes"]
    out = {}        # vtable: how far its code reaches into an object, itself or its bases
    for v in classes:
        a, extent, character = v, 0, False
        while a:
            extent = max(extent, classes[a]["extent"])
            character = character or a == OBJECT_ROOT
            a = classes[a]["parent"]
        if character:
            out[int(v, 16)] = extent
    return out


def float_like(word):
    """Any ordinary float, however small: a position's coordinate can be 1e-11. Not an infinity or NaN, nor what an address looks like."""
    exponent = word >> 23 & 0xFF
    return exponent != 0xFF and not 0x08800000 <= word < 0x0A000000


def replay(rec, mode, out, every, tick, binary):
    env = dict(os.environ, MHFU_HAND=str(mode), TRACE_BIN=binary, MHFU_TICK_HZ=str(tick))
    subprocess.run([sys.executable, os.path.join(HERE, "trace.py"), os.path.join(out, f"t{mode}"), rec, "--snapshots",
                    os.path.join(out, f"s{mode}"), "--every", str(every), "-j", "1", "--timeout", "400"], env=env,
                   stdout=subprocess.DEVNULL, check=True)


def compare(a_path, b_path, characters):
    with open(a_path, "rb") as f:
        x = f.read()
    with open(b_path, "rb") as f:
        y = f.read()
    a = struct.unpack(f"<{len(x) // 4}I", x)
    b = struct.unpack(f"<{len(y) // 4}I", y)
    kinds = collections.Counter()
    unexplained = []
    diffs = [i for i, (p, q) in enumerate(zip(a, b)) if p != q]
    if not diffs:
        return kinds, unexplained
    live = sorted(i for i, w in enumerate(a) if w in characters)
    for i in diffs:
        at = RAM + i * 4
        k = bisect.bisect_right(live, i) - 1
        in_character = k >= 0 and (i - live[k]) * 4 < characters[a[live[k]]]
        if in_character:
            unexplained.append((at, a[i], b[i]))
        elif VOICES[0] <= at < VOICES[1]:
            kinds["sound_voice"] += 1
        elif STACKS[0] <= at < STACKS[1]:
            kinds["stale_stack"] += 1
        elif at & 0xF == 0xC and all(a[i - n] == b[i - n] and float_like(a[i - n]) for n in (1, 2, 3)):
            kinds["vector_w"] += 1
        else:
            unexplained.append((at, a[i], b[i]))
    return kinds, unexplained


def one(rec, args, characters):
    name = os.path.basename(rec.partition(":")[0])[:-4]
    out = os.path.join(args.out, f"{name}.{args.tick}")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for mode in (0, 1):
        replay(rec, mode, out, args.every, args.tick, args.bin)
    kinds, bad, n = collections.Counter(), [], 0
    d0 = os.path.join(out, "s0")
    for snap in sorted(os.listdir(d0)):
        other = os.path.join(out, "s1", snap)
        if os.path.exists(other):
            k, u = compare(other, os.path.join(d0, snap), characters)
            kinds.update(k)
            bad += [(snap[-10:-4],) + w for w in u]
            n += 1
    shutil.rmtree(out, ignore_errors=True)
    return name, n, kinds, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recs", nargs="+")
    ap.add_argument("--bin", default=os.path.join(HERE, "..", "..", "build", "mhfu"))
    ap.add_argument("--out", default=os.path.join(WORK, "handdiff"))
    ap.add_argument("--every", type=int, default=600)
    ap.add_argument("--tick", type=int, default=30)
    ap.add_argument("-j", type=int, default=3)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)
    characters = character_vtables()
    failed, total = 0, collections.Counter()
    with cf.ThreadPoolExecutor(args.j) as pool:
        for name, n, kinds, bad in pool.map(lambda r: one(r, args, characters), args.recs):
            total.update(kinds)
            print(f"{name}: {n} snapshots, {dict(kinds)}, {'UNEXPLAINED ' + str(len(bad)) if bad else 'all explained'}", flush=True)
            for frame, at, p, q in bad[:12]:
                print(f"   frame {frame} {at:08X} hand {p:08X} generated {q:08X}")
            failed += bool(bad) or n == 0
    print(f"total {dict(total)}; {failed} recordings failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
