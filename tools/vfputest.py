#!/usr/bin/env python3
"""Check the runtime's VFPU (runtime/src/vfpu.c) against PPSSPP's, instruction by instruction.

The cases are the game's own VFPU instructions, each with the prefix instructions that come before it. Each case runs
as a small program, in PPSSPP (through its debugger) and in build/vfputest (the runtime's VFPU functions): set the
condition codes, load all 128 registers from memory (random values), run the instructions, store the registers back.
The registers and condition codes afterwards are compared - exactly, or within a relative 1e-5 for the transcendental
operations.
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys

import rabbitizer

import modules
import ppsspp

sys.path.insert(0, os.path.join(modules.ROOT, "recomp"))
from recomp import vfpu_op  # noqa: E402

BASE = 0x08780000           # scratch memory PPSSPP's kernel area leaves alone
CODE = BASE + 0x1000
INEXACT = {"vsin", "vcos", "vasin", "vexp2", "vlog2", "vrsq", "vrcp", "vsqrt", "vrot", "vdiv"}


def mtv_cc(rt):              # mtv rt, CC (VFPU control register 3)
    return 0x48E00000 | (rt << 16) | (128 + 3)


def mfv_cc(rt):
    return 0x48600000 | (rt << 16) | (128 + 3)


def lsv_q(op, k, offset):    # lv.q / sv.q of column quad k (matrix k / 4, column k % 4) at a0 + offset
    vt = k
    return (op << 26) | (4 << 21) | ((vt & 0x1F) << 16) | (offset & 0xFFFC) | ((vt >> 5) & 1)


def sequences(limit_per_op):
    """The game's VFPU operations with their prefixes: {op: [[(word, name)...], ...]}"""
    out = collections.defaultdict(list)
    seen = set()
    for m in modules.load():
        prefix = []
        for a in m.code():
            i = m.ins(a)
            n = i.uniqueId.name
            if not n.startswith("r4000allegrex_v"):
                prefix = []
                continue
            name = vfpu_op(n[14:])
            if name in ("vpfxs", "vpfxt", "vpfxd"):
                prefix.append((i.getRaw(), name))
                continue
            if name is None or n.startswith("r4000allegrex_bv"):
                prefix = []
                continue
            seq = tuple(prefix + [(i.getRaw(), name)])
            prefix = []
            if seq in seen or len(out[name]) >= limit_per_op:
                continue
            seen.add(seq)
            out[name].append(list(seq))
    return out


def random_floats(rng, n):
    vals = []
    for _ in range(n):
        r = rng.random()
        if r < 0.6:
            vals.append(rng.uniform(-2, 2))
        elif r < 0.8:
            vals.append(float(rng.randint(-3, 3)))
        elif r < 0.9:
            vals.append(rng.choice([0.0, 1.0, -1.0, 0.5, -0.5]))
        else:
            vals.append(rng.uniform(-100, 100))
    return vals


def program(seq):
    """The whole sequence, as (word, name) pairs; name None for what only PPSSPP runs (the CC store)"""
    p = [(mtv_cc(8), "mtv")]
    p += [(lsv_q(0x36, k, 16 * k), "lv_q") for k in range(32)]
    p += seq
    p += [(lsv_q(0x3E, k, 0x200 + 16 * k), "sv_q") for k in range(32)]
    p += [(mfv_cc(9), None), ((0x2B << 26) | (4 << 21) | (9 << 16) | 0x400, None), (0, None)]
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--binary", default=os.path.join(modules.ROOT, "build/vfputest"))
    ap.add_argument("--per-op", type=int, default=12)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    cases = []
    for name, seqs in sorted(sequences(args.per_op).items()):
        for seq in seqs:
            data = struct.pack("<128f", *random_floats(rng, 128)) + b"\0" * 0x200
            cc = rng.randrange(0, 64)
            cases.append((name, seq, data, cc))
    print(f"{len(cases)} cases, {len(set(c[0] for c in cases))} operations", file=sys.stderr)

    # the runtime's results
    native = subprocess.Popen([args.binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    blob = b""
    for name, seq, data, cc in cases:
        prog = [(w, n) for w, n in program(seq) if n]
        blob += struct.pack("<3I", BASE, BASE, cc) + data + struct.pack("<I", len(prog))
        blob += b"".join(struct.pack("<I16s", w, n.encode()) for w, n in prog)
    out, _ = native.communicate(blob)
    native_results = [out[k * 0x404:(k + 1) * 0x404] for k in range(len(cases))]

    # PPSSPP's
    bad = collections.defaultdict(list)
    h = ppsspp.Headless(args.iso)
    with h as pp:
        print("connected to PPSSPP", file=sys.stderr)
        pp.pause()
        for k, (name, seq, data, cc) in enumerate(cases):
          try:
            prog = program(seq)
            code = b"".join(struct.pack("<I", w) for w, _ in prog)
            end = CODE + 4 * (len(prog) - 1)
            pp.write(BASE, data)
            pp.write(CODE, code)
            pp.request("cpu.setReg", category=0, register=4, value=BASE)
            pp.request("cpu.setReg", category=0, register=8, value=cc)
            pp.request("cpu.setReg", category=0, register=32, value=CODE)
            pp.request("cpu.breakpoint.add", address=end, enabled=True)
            pp.resume()
            ev = pp.wait_event("cpu.stepping", timeout=10)
            pp.request("cpu.breakpoint.remove", address=end)
            if ev is None or ev["pc"] != end:
                print(f"{name}: PPSSPP did not stop at the end ({ev})", file=sys.stderr)
                pp.pause()
                continue
            theirs = pp.read(BASE + 0x200, 0x204)
            ours = native_results[k][0x200:]
            t = struct.unpack("<128fI", theirs)
            o = struct.unpack("<128fI", ours)
            diffs = []
            for r in range(128):
                a, b = t[r], o[r]
                if a == b or (a != a and b != b):
                    continue
                if name in INEXACT and abs(a - b) <= 1e-5 * max(1.0, abs(a)):
                    continue
                diffs.append(f"v[{r}] {b!r} (PPSSPP {a!r})")
            if (t[128] & 0x3F) != (o[128] & 0x3F):
                diffs.append(f"CC {o[128]:#x} (PPSSPP {t[128]:#x})")
            if diffs:
                words = " ".join(f"{w:08X}" for w, _ in seq)
                bad[name].append(f"{words} {rabbitizer.Instruction(seq[-1][0], 0, modules.CATEGORY).disassemble()}: "
                                 + "; ".join(diffs[:4]))
          except ConnectionError:
              words = " ".join(f"{w:08X}" for w, _ in seq)
              print(f"PPSSPP went away running case {k} ({name}: {words}); its log ends:", file=sys.stderr)
              print(open(h.log.name, errors="replace").read()[-1500:], file=sys.stderr)
              raise
    ops = sorted(set(c[0] for c in cases))
    print(f"\n{len(ops) - len(bad)} of {len(ops)} operations agree with PPSSPP")
    for name in sorted(bad):
        print(f"{name}: {len(bad[name])} of {sum(1 for c in cases if c[0] == name)} cases differ")
        for line in bad[name][:3]:
            print("   ", line)


if __name__ == "__main__":
    main()
