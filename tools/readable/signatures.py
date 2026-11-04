#!/usr/bin/env python3
"""Every function's signature from its machine code: signatures.py WORK [HINTS]. Liveness over each function's instructions,
delay slots included: an argument is a register in a0-a3, t0-t3, f12-f19 read on some path before it is written. A call clobbers
the caller-saved registers and reads what its callee takes, so arguments that are only forwarded count, and nothing survives a
call by accident; repeated over the whole game to a fixed point. Also:
- stack arguments: loads through sp at or above the function's frame;
- return register: v0 or f0 where a caller reads it straight after the call and the function writes it (or tail-calls one that
  does). A function no direct call reaches takes the decompiler's return, from HINTS, when that is v0 or f0;
- methods (WORK/methods.json) take a0 as `this`; a virtual call passes what the method in that vtable slot of its own class reads, and a0;
- variadic functions (saving their last argument registers to the stack on entry) take the ones before those, and "...";
- imports take the registers the runtime's handlers read.
Writes WORK/signatures.tsv: module, address, return register or -, arguments (registers, s<offset> for the stack)."""
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import modules  # noqa: E402

A0, V0, SP, RA = 4, 2, 29, 31
GPR_ARGS = list(range(4, 12))
FLT_ARGS = list(range(12, 20))
F = 32          # float registers follow the 32 GPRs in a register set
F0 = F
ARG_MASK = sum(1 << r for r in GPR_ARGS) | sum(1 << (F + r) for r in FLT_ARGS)
CLOBBER = sum(1 << r for r in [1, 2, 3] + list(range(4, 16)) + [24, 25]) | sum(1 << (F + r) for r in range(0, 20))
FUNCTIONS = os.path.join(HERE, "..", "..", "game", "analysis", "functions.json")
GPR_NAMES = {4: "a0", 5: "a1", 6: "a2", 7: "a3", 8: "t0", 9: "t1", 10: "t2", 11: "t3"}


def reg_name(r):
    return f"f{r - F}" if r >= F else GPR_NAMES[r]


def uses_defs(i):
    use, dfn = 0, 0
    if i.readsRs():
        use |= 1 << (i.getRaw() >> 21 & 31)
    if i.readsRt():
        use |= 1 << (i.getRaw() >> 16 & 31)
    if i.readsRd():
        use |= 1 << (i.getRaw() >> 11 & 31)
    if i.modifiesRs():
        dfn |= 1 << (i.getRaw() >> 21 & 31)
    if i.modifiesRt():
        dfn |= 1 << (i.getRaw() >> 16 & 31)
    if i.modifiesRd():
        dfn |= 1 << (i.getRaw() >> 11 & 31)
    if True:
        if i.readsFs():
            use |= 1 << (F + (i.getRaw() >> 11 & 31))
        if i.readsFt():
            use |= 1 << (F + (i.getRaw() >> 16 & 31))
        if i.readsFd():
            use |= 1 << (F + (i.getRaw() >> 6 & 31))
        if i.modifiesFs():
            dfn |= 1 << (F + (i.getRaw() >> 11 & 31))
        if i.modifiesFt():
            dfn |= 1 << (F + (i.getRaw() >> 16 & 31))
        if i.modifiesFd():
            dfn |= 1 << (F + (i.getRaw() >> 6 & 31))
    name = i.getOpcodeName()
    if name.startswith("sv.") or name in ("svl.q", "svr.q"):    # (rabbitizer: flagged as writing the base it reads)
        base = 1 << (i.getRaw() >> 21 & 31)
        use |= base
        dfn &= ~base
    elif name in ("ins", "movn", "movz"):                       # they merge into what they write
        use |= dfn
    use &= ~1
    dfn &= ~1
    return use, dfn


class Fn:
    __slots__ = ("mod", "start", "end", "nodes", "exits", "calls", "tails", "args", "ret", "stack", "variadic", "writes_v0",
                 "writes_f0", "called", "tail_callees", "fixed")


def import_args(main):
    """{stub: mask of argument registers} from the runtime's hle_<name> functions, which take them as ARGn or c->r[n]."""
    root = os.path.join(HERE, "..", "..", "runtime", "src")
    src = ""
    for name in sorted(os.listdir(root)):
        if name.endswith(".c"):
            with open(os.path.join(root, name), errors="replace") as f:
                src += f.read()
    with open(os.path.join(root, "nids.inc")) as f:
        nids = {(lib, int(nid, 16)): name for lib, nid, name in re.findall(r"X\((\w+), 0x([0-9A-Fa-f]{8}), (\w+)\)", f.read())}
    out = {}
    for stub, key in main.imports.items():
        name = nids.get(key)
        mask = 0
        at = src.find(f"void hle_{name}(Ctx *c) {{") if name else -1
        if at >= 0:
            end = src.find("\n}\n", at)
            body = src[at:end]
            for n in re.findall(r"\bARG(\d)\b", body):
                mask |= 1 << GPR_ARGS[int(n)]
            for n in re.findall(r"c->r\[(\d+)\]", body):
                if int(n) in GPR_ARGS:
                    mask |= 1 << int(n)
        out[stub] = mask
    return out


def method_slot(m, a, start, reg):
    """A call through `reg`: the vtable slot it was loaded from just before (lw reg, slot(x)), or -1 where it can't be told."""
    at = a - 4
    while at >= start and at >= a - 32:
        i = m.ins(at)
        if i.getOpcodeName() == "lw" and (i.getRaw() >> 16 & 31) == reg:
            off = i.getProcessedImmediate()
            return ("slot", off) if 0 <= off < 0x400 else -1
        if uses_defs(i)[1] >> reg & 1:
            return -1
        at -= 4
    return -1


def build(m, f, starts, table):
    """Nodes: a list (succs, use, def, call): call is the target (a callee address), or -1 for an indirect one."""
    fn = Fn()
    fn.mod, fn.start, fn.end = m.name, f["start"], f["end"]
    start, end = f["start"], f["end"]
    tables = {jr: ts for jr, _t, ts in f.get("tables", [])}
    inside = lambda t: start <= t < end and (t == start or t not in starts)     # noqa: E731
    nodes = {}
    fn.exits, fn.calls, fn.tails = [], [], []
    fn.writes_v0 = fn.writes_f0 = False
    a = start
    frame = 0
    seen_store_args = set()
    stack = set()
    while a < end:
        i = m.ins(a)
        use, dfn = uses_defs(i)
        name = i.getOpcodeName()
        if dfn & (1 << V0):
            fn.writes_v0 = True
        if dfn & (1 << F0):
            fn.writes_f0 = True
        if a < start + 16 and name == "addiu" and (i.getRaw() >> 16 & 31) == SP and (i.getRaw() >> 21 & 31) == SP:
            imm = i.getProcessedImmediate()
            if imm < 0:
                frame = -imm
        if i.readsRs() and (i.getRaw() >> 21 & 31) == SP and i.doesLoad():
            off = i.getProcessedImmediate() - frame
            if 0 <= off < 0x100:
                stack.add(off & ~3)
        if a < start + 64 and i.doesStore() and (i.getRaw() >> 21 & 31) == SP and not i.isFloat():
            if (i.getRaw() >> 16 & 31) in GPR_ARGS:
                seen_store_args.add(i.getRaw() >> 16 & 31)
        nodes[a] = [[], use, dfn, None]
        if i.isBranch() or i.isJump() or i.isFunctionCall() or i.isReturn() or i.isJrNotRa():
            d = a + 4
            duse, ddfn = uses_defs(m.ins(d)) if d < end else (0, 0)
            if d < end and (ddfn & (1 << V0)):
                fn.writes_v0 = True
            if d < end and (ddfn & (1 << F0)):
                fn.writes_f0 = True
            nodes[d] = [[], duse, ddfn, None]
            nodes[a][0] = [d]
            after = a + 8
            if i.isFunctionCall():
                target = i.getInstrIndexAsVram() if name == "jal" else method_slot(m, a, start, i.getRaw() >> 21 & 31)
                call = ("call", a)
                nodes[call] = [[after], 0, 0, target]
                nodes[d][0] = [call]
                fn.calls.append(target)
            elif i.isBranch():
                t = i.getBranchVramGeneric()
                if i.isBranchLikely():
                    nodes[a][0] = [d, after]
                    nodes[d][0] = [t]
                elif i.isUnconditionalBranch():
                    nodes[d][0] = [t]
                else:
                    nodes[d][0] = [t, after]
            elif name == "j":
                t = i.getInstrIndexAsVram()
                if inside(t):
                    nodes[d][0] = [t]
                else:
                    tail = ("tail", a)
                    nodes[tail] = [[], 0, 0, t]
                    nodes[d][0] = [tail]
                    fn.tails.append(t)
            elif i.isReturn():
                ex = ("exit", a)
                nodes[ex] = [[], 0, 0, None]
                nodes[d][0] = [ex]
                fn.exits.append(ex)
            elif a in tables:
                nodes[d][0] = [t for t in set(tables[a]) if inside(t)]
            else:       # an unresolved jr: a virtual call as a tail call
                tail = ("tail", a)
                nodes[tail] = [[], 0, 0, method_slot(m, a, start, i.getRaw() >> 21 & 31)]
                nodes[d][0] = [tail]
            a += 8
            continue
        nodes[a][0] = [a + 4]
        a += 4
    fn.nodes = nodes
    fn.stack = stack
    # Variadic: saves the argument registers from some point up to t3, for a format's va_list; those before it are fixed
    fn.fixed = 0
    fn.variadic = {9, 10, 11}.issubset(seen_store_args)
    if fn.variadic:
        low = 9
        while low - 1 in seen_store_args and low - 1 >= 4:
            low -= 1
        fn.fixed = sum(1 << r for r in GPR_ARGS if r < low)
    fn.args, fn.ret = 0, None
    fn.called = False
    fn.tail_callees = fn.tails
    return fn


def analyse(fn, callee_args, callee_ret):
    """Backward liveness to a fixed point. Returns (args at entry, whether v0 / f0 is live right after a call, per target)."""
    live = {k: 0 for k in fn.nodes}
    order = sorted(fn.nodes, key=lambda k: k if isinstance(k, int) else k[1] + 2, reverse=True)
    exit_use = 0
    if fn.ret == "v0":
        exit_use = 1 << V0
    elif fn.ret == "f0":
        exit_use = 1 << F0
    changed = True
    while changed:
        changed = False
        for k in order:
            succs, use, dfn, target = fn.nodes[k]
            out = 0
            for s in succs:
                out |= live.get(s, 0)
            if isinstance(k, tuple) and k[0] == "exit":
                v = exit_use
            elif isinstance(k, tuple) and k[0] == "call":
                v = (out & ~CLOBBER) | callee_args(fn, target)
            elif isinstance(k, tuple) and k[0] == "tail":
                v = callee_args(fn, target) | (exit_use & ~callee_ret(fn, target))
            else:
                v = (out & ~dfn) | use
            if v != live[k]:
                live[k] = v
                changed = True
    wants = []
    for k, (succs, _u, _d, target) in fn.nodes.items():
        if isinstance(k, tuple) and k[0] == "call" and isinstance(target, int) and target != -1:
            out = 0
            for s in succs:
                out |= live.get(s, 0)
            wants.append((target, bool(out & (1 << V0)), bool(out & (1 << F0))))
    return live[fn.start] & ARG_MASK, wants


def main():
    work = sys.argv[1]
    hints = {}
    if len(sys.argv) > 2:
        with open(sys.argv[2]) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if p[2] in ("v0", "f0"):
                    hints[(p[0], int(p[1], 16))] = p[2]
    with open(FUNCTIONS) as f:
        functions = json.load(f)
    with open(os.path.join(work, "methods.json")) as f:
        methods = {tuple(k.split()) for k in json.load(f)}
    mods = modules.load()
    by_name = {m.name: m for m in mods}
    fns, owners = {}, collections.defaultdict(list)
    for m in mods:
        starts = {x["start"] for x in functions[m.name]["functions"]}
        for x in functions[m.name]["functions"]:
            if x["end"] <= x["start"]:
                continue
            fn = build(m, x, starts, None)
            fns[(m.name, x["start"])] = fn
            owners[x["start"]].append(m.name)
    # imports: the registers the runtime's handler reads (hle_<name>)
    main_mod = by_name["EBOOT"]
    imp_mask = import_args(main_mod)

    def targets_of(fn, target):
        o = owners.get(target)
        if not o:
            return []
        if fn.mod in o:
            return [fns[(fn.mod, target)]]
        return [fns[(m, target)] for m in o]

    # A virtual call takes what the method in that slot of the caller's own class reads: other classes' slots mean other things
    with open(os.path.join(work, "vtables.json")) as f:
        vtables = json.load(f)
    this_class = {}
    classes_path = os.path.join(work, "classes.json")
    if os.path.exists(classes_path):
        with open(classes_path) as f:
            cdata = json.load(f)
        for k, (v, _how) in cdata["this"].items():
            m, a = k.split()
            this_class[(m, int(a, 16))] = v

    def slot_args(fn, off):
        # (with no class to go by, a0 only: all of a tree's methods together invented arguments, as one rare method's float
        # reached every caller)
        mask = 1 << A0
        entry = vtables.get(this_class.get((fn.mod, fn.start)), {}).get(str(off))
        t = fns.get((entry[0], int(entry[1], 16))) if entry else None
        if t:
            mask |= t.fixed if t.variadic else t.args
        return mask

    def callee_args(fn, target):
        if isinstance(target, tuple):
            return slot_args(fn, target[1])
        if target == -1:
            return 1 << A0
        if target in imp_mask:
            return imp_mask[target]
        found = targets_of(fn, target)
        if not found:
            return 1 << A0
        mask = None
        for t in found:
            a = t.fixed if t.variadic else t.args
            mask = a if mask is None else mask & a
        return mask

    def callee_ret(fn, target):
        if isinstance(target, tuple) or target == -1 or target in imp_mask:
            return 0
        for t in targets_of(fn, target):
            if t.ret == "v0":
                return 1 << V0
            if t.ret == "f0":
                return 1 << F0
        return 0

    for key in methods:
        pass
    is_method = {(m, int(a, 16)) for m, a in methods}
    for k, fn in fns.items():
        if k in is_method:
            fn.args = 1 << A0
    for round_no in range(12):
        changed = 0
        wants_v0, wants_f0 = set(), set()
        for k, fn in fns.items():
            args, wants = analyse(fn, callee_args, callee_ret)
            if k in is_method:
                args |= 1 << A0
            if fn.variadic:
                args = fn.fixed
            if args != fn.args:
                fn.args = args
                changed += 1
            for target, v, f in wants:
                if target in imp_mask:
                    continue
                for t in targets_of(fn, target):
                    t.called = True
                    if v:
                        wants_v0.add((t.mod, t.start))
                    if f:
                        wants_f0.add((t.mod, t.start))
        # tail calls return what their callers want
        for _ in range(4):
            for k, fn in fns.items():
                for t in fn.tails:
                    if not isinstance(t, int) or t == -1 or t in imp_mask:
                        continue
                    for c in targets_of(fn, t):
                        c.called = True
                        if k in wants_v0:
                            wants_v0.add((c.mod, c.start))
                        if k in wants_f0:
                            wants_f0.add((c.mod, c.start))
        for k, fn in fns.items():
            # A caller reads it straight after the call, and nothing compiled reads a dead register: it returns it, whether
            # it writes it itself or passes on a callee's
            if k in wants_f0:
                ret = "f0"
            elif k in wants_v0:
                ret = "v0"
            elif not fn.called and k in hints:
                ret = hints[k]
            else:
                ret = None
            if ret != fn.ret:
                fn.ret = ret
                changed += 1
        print(f"round {round_no + 1}: {changed} changed", flush=True)
        if not changed:
            break
    order = [reg for reg in GPR_ARGS] + [F + r for r in FLT_ARGS]
    with open(os.path.join(work, "signatures.tsv"), "w") as f:
        stats = collections.Counter()
        for (m, a), fn in sorted(fns.items()):
            regs = [reg_name(r) for r in order if fn.args & (1 << r)]
            if fn.variadic:
                stats["variadic"] += 1
                regs.append("...")
            else:
                regs += [f"s{o}" for o in sorted(fn.stack)]
            stats["stack"] += bool(fn.stack)
            stats[fn.ret or "void"] += 1
            f.write(f"{m}\t{a:x}\t{fn.ret or '-'}\t{','.join(regs)}\n")
        for t, mask in sorted(imp_mask.items()):
            regs = [reg_name(r) for r in GPR_ARGS if mask & (1 << r)]
            f.write(f"EBOOT\t{t:x}\tv0\t{','.join(regs)}\n")
    print(dict(stats))


if __name__ == "__main__":
    main()
