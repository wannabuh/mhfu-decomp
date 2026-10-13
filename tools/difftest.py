#!/usr/bin/env python3
"""Differential testing of the recompiled code against PPSSPP.

The game runs in PPSSPP with breakpoints on the entries of functions not yet tested. When one is hit, the CPU state and
memory there are saved, the function is run to its return (a breakpoint on its return address, with the stack pointer
back where it was), and the state there is saved too. build/difftest then runs the recompiled function from the entry
state, and the two results are compared: registers, scratchpad and RAM. Functions are tested on the arguments and data
the game actually calls them with.

A function that interrupts can disturb in PPSSPP (they may run in the middle of it) can show differences that are not
the recompiler's; such failures are retried (--retries).

Results accumulate in game/analysis/difftest.json: per module and function, "pass", "fail: <what differed>" or
"skip: <why the recompiled code could not run it>" (an import or the VFPU, before the runtime has them).
"""
import argparse
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import time

import modules
import ppsspp

SCRATCH, SCRATCH_SIZE = 0x00010000, 0x4000
RAM, RAM_SIZE = 0x08000000, 0x2000000
HEADER = struct.Struct("<8s34I33I128I16I160sI252s")


def regions(mods):
    """{region: [(base, number, module name)]} for the overlay regions."""
    out = {}
    for m in mods[1:]:
        out.setdefault(m.region, []).append((m.base, m.number, m.name))
    return out


class State:
    def __init__(self, gpr, fpr, vfpu, scratch, ram):
        self.gpr, self.fpr, self.vfpu, self.scratch, self.ram = gpr, fpr, vfpu, scratch, ram

    @classmethod
    def capture(cls, pp):
        cats = {c["id"]: c["uintValues"] for c in pp.request("cpu.getAllRegs")["categories"]}
        return cls(cats[0], cats[1], cats[2], pp.read(SCRATCH, SCRATCH_SIZE), pp.read(RAM, RAM_SIZE))

    def ram_word(self, addr):
        return struct.unpack_from("<I", self.ram, addr - RAM)[0]

    def write(self, path):
        gpr = list(self.gpr[:32]) + [self.gpr[33], self.gpr[34]]          # r0-r31, hi, lo
        with open(path, "wb") as f:
            f.write(HEADER.pack(b"MHFUST01", *gpr, *self.fpr, 0, *self.vfpu, *([0] * 16), b"", 0, b""))
            f.write(self.scratch)
            f.write(self.ram)

    @classmethod
    def read(cls, path):
        with open(path, "rb") as f:
            h = HEADER.unpack(f.read(HEADER.size))
            scratch = f.read(SCRATCH_SIZE)
            ram = f.read(RAM_SIZE)
        gpr = list(h[1:33]) + [0, h[33], h[34]]                         # as PPSSPP: r0-r31, pc, hi, lo
        st = cls(gpr, list(h[35:67]), list(h[68:196]), scratch, ram)
        st.status = h[213]
        st.message = h[214].split(b"\0", 1)[0].decode(errors="replace")
        return st


def resident(state, by_region):
    """The overlay resident in each region, from the header at the region's base."""
    out = {}
    for region, ovls in by_region.items():
        base = ovls[0][0]
        if state.ram[base - RAM:base - RAM + 4] == b"MWo3":
            num = state.ram_word(base + 4)
            out[region] = next((name for _b, n, name in ovls if n == num), None)
    return out


def module_at(addr, state, mods, by_region):
    if mods[0].in_text(addr):
        return "EBOOT"
    res = resident(state, by_region)
    for region, ovls in by_region.items():
        name = res.get(region)
        if name and modules_by_name[name].in_text(addr):
            return name
    return None


def ranges(diff_offsets, base):
    """Contiguous runs of differing byte offsets, as 'start-end' strings (at most 6)."""
    out = []
    for o in diff_offsets:
        if out and o <= out[-1][1] + 16:
            out[-1][1] = o
        else:
            out.append([o, o])
    return [f"{base + s:08X}-{base + e:08X}" for s, e in out[:6]] + (["..."] if len(out) > 6 else [])


# What a caller can see of a function: the return value, the callee-saved registers and memory. The rest - the
# temporaries, hi / lo - are dead after a call by the calling convention, and PPSSPP replaces some library functions
# (memset, memcpy...) with native code that leaves them differently from the game's own code.
CHECKED_GPRS = [2] + list(range(16, 24)) + [28, 29, 30, 31]
CHECKED_FPRS = [0] + list(range(20, 32))


DEAD_STACK = 0x10000       # how far below the entry stack pointer to ignore


def compare(pp_after, native, entry_sp):
    """What differs, of what a caller can see, between PPSSPP's state after the function and the recompiled code's.
    The stack below the stack pointer at entry is dead once the function returns: the function's own frame, and its
    callees' - which differ where PPSSPP replaced a library function with native code."""
    diffs = []
    names = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
             "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
    for k in CHECKED_GPRS:
        if pp_after.gpr[k] != native.gpr[k]:
            diffs.append(f"{names[k]} {native.gpr[k]:08X} (PPSSPP {pp_after.gpr[k]:08X})")
    for k in CHECKED_FPRS:
        if pp_after.fpr[k] != native.fpr[k]:
            diffs.append(f"f{k} {native.fpr[k]:08X} (PPSSPP {pp_after.fpr[k]:08X})")
    for name, a, b, base in (("scratchpad", pp_after.scratch, native.scratch, SCRATCH),
                             ("RAM", pp_after.ram, native.ram, RAM)):
        if a != b:
            offs = [i for i in range(0, len(a), 4) if a[i:i + 4] != b[i:i + 4]
                    and not entry_sp - DEAD_STACK <= base + i < entry_sp]
            if offs:
                diffs.append(f"{name} {' '.join(ranges(offs, base))}")
    return diffs


def run_native(binary, before, func, tmp):
    inp, out = os.path.join(tmp, "in.state"), os.path.join(tmp, "out.state")
    before.write(inp)
    try:
        subprocess.run([binary, inp, out, f"{func:08X}"], check=True, timeout=30, capture_output=True)
    except subprocess.TimeoutExpired:
        return None, "the recompiled function did not return within 30 s"
    except subprocess.CalledProcessError as e:
        return None, f"difftest crashed ({e.returncode}): {e.stderr.decode(errors='replace')[-200:]}"
    return State.read(out), None


def main():
    root = modules.ROOT
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--iso", default=os.path.expanduser("~/Games/psp/mhfu.iso"))
    ap.add_argument("--functions", default=os.path.join(root, "game/analysis/functions.json"))
    ap.add_argument("--results", default=os.path.join(root, "game/analysis/difftest.json"))
    ap.add_argument("--binary", default=os.path.join(root, "build/difftest"))
    ap.add_argument("--minutes", type=float, default=20, help="stop after this long")
    ap.add_argument("--batch", type=int, default=200, help="breakpoints set at a time")
    ap.add_argument("--wait", type=float, default=10, help="seconds to wait for a batch's breakpoints")
    ap.add_argument("--retries", type=int, default=2, help="re-tests of a failing function")
    ap.add_argument("--retest", action="store_true", help="test functions already tested too")
    args = ap.parse_args()

    global modules_by_name
    mods = modules.load()
    modules_by_name = {m.name: m for m in mods}
    by_region = regions(mods)
    with open(args.functions) as f:
        funcs = {n: [x["start"] for x in r["functions"]] for n, r in json.load(f).items()}
    results = {}
    if os.path.exists(args.results):
        with open(args.results) as f:
            results = json.load(f)

    def done(name, addr):
        r = results.get(name, {}).get(f"{addr:08X}")
        return r is not None and not args.retest and not (r.startswith("fail") and r.count("retry") < args.retries)

    # candidates: addresses, with the modules that have a function there
    cands = {}
    for name, starts in funcs.items():
        for a in starts:
            cands.setdefault(a, set()).add(name)
    pending = [a for a, names in cands.items() if any(not done(n, a) for n in names)]
    random.shuffle(pending)
    print(f"{len(pending)} addresses to test", file=sys.stderr)

    deadline = time.time() + args.minutes * 60
    stats = {"pass": 0, "fail": 0, "skip": 0}
    tmp = tempfile.mkdtemp(prefix="mhfu-difftest-")
    with ppsspp.Headless(args.iso) as pp:
        print("connected to PPSSPP", file=sys.stderr)
        while pending and time.time() < deadline:
            batch, pending = pending[:args.batch], pending[args.batch:]
            pp.pause()
            for a in batch:
                pp.request("cpu.breakpoint.add", address=a, enabled=True)
            pp.resume()
            untested = set(batch)
            while untested and time.time() < deadline:
                ev = pp.wait_event("cpu.stepping", timeout=args.wait)
                if ev is None:
                    pp.pause()
                    break
                pc = ev["pc"]
                if pc not in untested:
                    pp.resume()
                    continue
                for a in untested:
                    pp.request("cpu.breakpoint.remove", address=a)
                before = State.capture(pp)
                name = module_at(pc, before, mods, by_region)
                if name is None or pc not in set(funcs[name]) or (results.get(name, {}).get(f"{pc:08X}") and done(name, pc)):
                    untested.discard(pc)
                    for a in untested:
                        pp.request("cpu.breakpoint.add", address=a, enabled=True)
                    pp.resume()
                    continue
                ra, sp = before.gpr[31], before.gpr[29]
                pp.request("cpu.breakpoint.add", address=ra, enabled=True)
                pp.resume()
                after = None
                t_end = time.time() + 20
                while time.time() < t_end:
                    ev = pp.wait_event("cpu.stepping", timeout=max(0.1, t_end - time.time()))
                    if ev is None:
                        break
                    if ev["pc"] == ra:
                        regs = pp.request("cpu.getAllRegs")["categories"][0]["uintValues"]
                        if regs[29] == sp:
                            after = State.capture(pp)
                            break
                    pp.resume()
                if after is None:
                    pp.pause()
                pp.request("cpu.breakpoint.remove", address=ra)
                untested.discard(pc)

                key = f"{pc:08X}"
                prev = results.get(name, {}).get(key, "")
                if after is None:
                    verdict = "skip: did not return in PPSSPP within 20 s"
                else:
                    native, err = run_native(args.binary, before, pc, tmp)
                    if err:
                        verdict = "fail: " + err
                    elif native.status:
                        verdict = "skip: " + native.message
                    else:
                        d = compare(after, native, sp)
                        verdict = "pass" if not d else "fail: " + "; ".join(d)
                if verdict.startswith("fail") and prev.startswith("fail"):
                    verdict += " (retry " + str(prev.count("retry") + 1) + ")"
                results.setdefault(name, {})[key] = verdict
                stats[verdict.split(":")[0]] += 1
                print(f"{name} {key}: {verdict[:200]}", file=sys.stderr)
                with open(args.results, "w") as f:
                    json.dump(results, f, indent=0, sort_keys=True)

                for a in untested:
                    pp.request("cpu.breakpoint.add", address=a, enabled=True)
                pp.resume()
            for a in untested:
                pp.request("cpu.breakpoint.remove", address=a)
            pending.extend(a for a in untested)
    total = {"pass": 0, "fail": 0, "skip": 0}
    for r in results.values():
        for v in r.values():
            total[v.split(":")[0]] += 1
    print(f"this run: {stats}; all results: {total}", file=sys.stderr)


if __name__ == "__main__":
    main()
