#!/usr/bin/env python3
"""Where two runs of a recording part: firstdiff.py REC [--saves kit] [--hz 60@3574] A=1 ... -- B=1 ...

Plays REC under two sets of settings (MHFU_ is added to each name), hashing the game's RAM every tick, then again to write RAM at the first tick
that differs and the one before. Says the tick, how many words differ and where: which of them are in the hunter (named, from
tools/readable/fields/Hunter.toml, and by what it was doing), and the watch.py command that finds what wrote the first.

    tools/firstdiff.py tests/data/rec/popo.rec --hz 60@3574 PAD_EARLY=1 -- PAD_EARLY=0
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tests", "data")
RAM_BASE = 0x08800000


def run(work, name, args, sets, extra, until="replay done"):
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "build"))
    os.symlink(os.path.join(ROOT, "game"), os.path.join(d, "game"))
    shutil.copytree(os.path.join(DATA, "saves", args.saves), os.path.join(d, "SAVEDATA"))
    os.makedirs(os.path.join(d, "cfg", "mhfu-recomp"))
    with open(os.path.join(d, "cfg", "mhfu-recomp", "settings.ini"), "w") as f:
        f.write(f"iso = {args.iso}\n")
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(d, "cfg"), MHFU_HEADLESS="1", MHFU_MUTE="1", MHFU_FPS="0", MHFU_SCALE="1",
               MHFU_RENDERER="vulkan", MHFU_TICK_HZ=args.hz, MHFU_REPLAY=os.path.abspath(args.rec),
               MHFU_SAVEDIR=os.path.join(d, "SAVEDATA"), MHFU_STATEDIR=os.path.join(d, "states"))
    for k in [k for k in env if k.startswith("MHFU_TEST_")]:
        del env[k]
    for s in sets:
        k, _, v = s.partition("=")
        env["MHFU_" + k] = v
    env.update(extra)
    log = open(os.path.join(d, "log"), "w")
    p = subprocess.Popen([os.path.abspath(args.bin)], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
    return p, d, until


def wait(p, d, until, timeout):
    t0 = time.time()
    path = os.path.join(d, "log")
    while time.time() - t0 < timeout and p.poll() is None:
        with open(path, errors="replace") as f:
            if until in f.read():
                break
        time.sleep(0.5)
    if p.poll() is None:
        p.kill()
    p.wait()


def hashes(d):
    out = {}
    for line in open(os.path.join(d, "log"), errors="replace"):
        m = re.match(r"ram hash (\d+) ([0-9a-f]+)", line)
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


def hunter_fields():
    with open(os.path.join(ROOT, "tools", "readable", "fields", "Hunter.toml"), "rb") as f:
        doc = tomllib.load(f)
    fields = {}
    for x in doc.get("field", []):
        off = x.get("offset")
        off = int(off, 0) if isinstance(off, str) else off
        fields[off] = x.get("name", "?")
    return fields


def main():
    if "--" not in sys.argv:
        print(__doc__)
        return 2
    cut = sys.argv.index("--")
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("--saves", default="kit")
    ap.add_argument("--hz", default="30")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "mhfu"))
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--work", default=None)
    args, a_sets = ap.parse_known_args(sys.argv[1:cut])
    b_sets = sys.argv[cut + 1:]
    work = args.work or tempfile.mkdtemp(prefix="firstdiff")
    runs = [run(work, n, args, s, {"MHFU_RAM_HASH": "1"}) for n, s in (("a", a_sets), ("b", b_sets))]
    for p, d, u in runs:
        wait(p, d, u, args.timeout)
    ha, hb = hashes(runs[0][1]), hashes(runs[1][1])
    common = sorted(set(ha) & set(hb))
    first = next((f for f in common if ha[f] != hb[f]), None)
    if first is None:
        print(f"the same over {len(common)} ticks")
        return 0
    before = max([f for f in common if f < first], default=first)
    print(f"RAM first differs at tick {first} (the same at {before}), of {len(common)}", flush=True)
    runs = [run(work, n, args, s, {"MHFU_SNAPSHOT": f"{before},{first}"}, until="ZZZ")
            for n, s in (("a2", a_sets), ("b2", b_sets))]
    want = [os.path.join(d, "build", f"ram-{first:06d}.bin") for _, d, _ in runs]
    t0 = time.time()
    while time.time() - t0 < args.timeout and not all(os.path.exists(x) and os.path.getsize(x) > 0 for x in want):
        time.sleep(0.5)
    time.sleep(1)
    for p, *_ in runs:
        p.kill()
        p.wait()
    a = open(want[0], "rb").read()
    b = open(want[1], "rb").read()
    n = min(len(a), len(b)) // 4
    wa, wb = struct.unpack(f"<{n}I", a[:4 * n]), struct.unpack(f"<{n}I", b[:4 * n])
    diff = [i for i in range(n) if wa[i] != wb[i]]
    print(f"{len(diff)} words differ")
    hunters = [RAM_BASE + 4 * i for i in range(n) if wa[i] == 0x089BB3CC]   # (its vtable, as MHFU_TRACE_VT knows it)
    base = hunters[0] if hunters else 0
    if base:
        print(f"the hunter is at {base:08X}")
    fields = hunter_fields() if base else {}
    for i in diff[:48]:
        addr = RAM_BASE + 4 * i
        where = ""
        if base and base <= addr < base + 0x2000:
            where = f"  hunter+0x{addr - base:X} {fields.get(addr - base, '')}"
        print(f"  {addr:08X}: {wa[i]:08X} / {wb[i]:08X}{where}")
    if len(diff) > 48:
        print(f"  ... and {len(diff) - 48} more")
    if diff:
        print(f"what wrote the first: tools/watch.py {RAM_BASE + 4 * diff[0]:08X} --frame {first} -- MHFU_REPLAY={os.path.abspath(args.rec)} "
              f"MHFU_TICK_HZ={args.hz} MHFU_HEADLESS=1 " + " ".join("MHFU_" + s for s in a_sets))
    print(f"runs kept in {work}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
