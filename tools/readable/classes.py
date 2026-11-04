#!/usr/bin/env python3
"""The game's classes, from evidence: classes.py WORK TRACES SNAPSHOT...

- Which class each function works on: what it was handed at run time (TRACES/*.args, trace_hook.c), vtable slots, and, for code
  that never ran, callers that all pass it the same object.
- Inheritance: a constructor stores its vtable at +0 after calling its base's constructor, which stores the base's.
- Fields: every load and store through `this`, or through a field known to point at a class, with its width and kind.
- Pointer fields and globals: words in RAM snapshots that point at an object.

Writes WORK/classes.json. Everything in it is derived from the game: it stays out of git; names and notes go in types.toml."""
import collections
import glob
import json
import os
import struct
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import modules  # noqa: E402
from signatures import uses_defs  # noqa: E402

FUNCTIONS = os.path.join(HERE, "..", "..", "game", "analysis", "functions.json")
RAM = 0x08800000
A0, SP, RA, ZERO = 4, 29, 31, 0
CALLER_SAVED = [1, 2, 3] + list(range(4, 16)) + [24, 25]
LOADS = {"lb": ("s8", 1), "lbu": ("u8", 1), "lh": ("s16", 2), "lhu": ("u16", 2), "lw": ("u32", 4), "lwc1": ("f32", 4),
         "lv.s": ("f32", 4), "lv.q": ("vec", 16), "ulv.q": ("vec", 16)}
STORES = {"sb": ("x8", 1), "sh": ("x16", 2), "sw": ("u32", 4), "swc1": ("f32", 4), "sv.s": ("f32", 4), "sv.q": ("vec", 16),
          "usv.q": ("vec", 16)}
MAX_FIELD = 0x100000
SCAN_FIELD = 0x8000       # how far into an object RAM is searched for pointers


def walk(m, f, starts):
    """The function's instructions in address order, with leaders: a register's facts don't survive a label."""
    start, end = f["start"], f["end"]
    leaders = set()
    for a in range(start, end, 4):
        i = m.ins(a)
        if i.isBranch():
            leaders.add(i.getBranchVramGeneric())
    for _jr, _t, targets in f.get("tables", []):
        leaders.update(targets)
    return start, end, leaders


class Flow:
    """A forward scan with registers holding facts: a constant, or (kind, value). Conservative: a label clears everything but
    what the callee-saved registers held when nothing in the function reassigns them, a call clears the caller-saved."""

    def __init__(self, m, f, starts):
        self.m, self.f = m, f
        self.start, self.end, self.leaders = walk(m, f, starts)
        self.writes = collections.Counter()
        for a in range(self.start, self.end, 4):
            _u, d = uses_defs(m.ins(a))
            if m.ins(a).getOpcodeName() == "lw" and (m.ins(a).getRaw() >> 21 & 31) == SP:
                continue        # an s-register restored on the way out says nothing about its value in the body
            for r in range(32):
                if d >> r & 1:
                    self.writes[r] += 1

    def run(self, entry, visit):
        """entry: {register: fact}. visit(address, instruction, name, facts, is_delay) sees each instruction before it applies.
        A fact on a register written just once (after its copy) holds across labels: compilers keep `this` in s-registers."""
        facts = dict(entry)
        stable = {r: v for r, v in entry.items() if not self.writes[r]}
        a = self.start
        while a < self.end:
            if a in self.leaders:
                facts = {r: v for r, v in facts.items() if stable.get(r) == v}
            i = self.m.ins(a)
            name = i.getOpcodeName()
            visit(a, i, name, facts)
            self.step(i, name, facts, stable)
            if i.isFunctionCall():
                # the delay slot runs before the call: applied, then the call clobbers
                d = a + 4
                if d < self.end:
                    di = self.m.ins(d)
                    visit(d, di, di.getOpcodeName(), facts)
                    self.step(di, di.getOpcodeName(), facts, stable)
                self.call(a, i, name, facts)
                for r in CALLER_SAVED:
                    facts.pop(r, None)
                a += 8
                continue
            a += 4

    def call(self, a, i, name, facts):
        pass

    def step(self, i, name, facts, stable):
        raw = i.getRaw()
        rs, rt, rd = raw >> 21 & 31, raw >> 16 & 31, raw >> 11 & 31
        _u, d = uses_defs(i)
        out = None
        target = None
        if name == "lui":
            target, out = rt, ("k", (raw & 0xFFFF) << 16)
        elif name in ("addiu", "ori") and facts.get(rs, (None,))[0] == "k":
            imm = i.getProcessedImmediate()
            target, out = rt, ("k", (facts[rs][1] + imm) & 0xFFFFFFFF if name == "addiu" else facts[rs][1] | (raw & 0xFFFF))
        elif name == "addu" and rt != ZERO and rs != ZERO and self.big_offset(facts.get(rs), facts.get(rt)):
            target, out = rd, self.big_offset(facts.get(rs), facts.get(rt))
        elif name in ("move", "addu", "or") and rt == ZERO and rs in facts:      # (rabbitizer spells move as such)
            target, out = rd, facts[rs]
        elif name in ("move", "addu", "or") and rs == ZERO and rt in facts:
            target, out = rd, facts[rt]
        elif name == "lw" and rs in facts and facts[rs][0] == "o":
            target = rt
            ptr = self.field_type(facts[rs][1], i.getProcessedImmediate())
            out = ("o", ptr) if ptr else ("p", facts[rs][1], i.getProcessedImmediate())
        elif name == "lw" and rs in facts and facts[rs][0] == "k":
            target = rt
            ptr = self.global_type(facts[rs][1] + i.getProcessedImmediate())
            out = ("o", ptr) if ptr else None
        for r in range(32):
            if d >> r & 1:
                facts.pop(r, None)
        if out is not None and target != ZERO:
            facts[target] = out
            if self.writes[target] <= 1 and target >= 16 and target != 28:
                stable[target] = out

    def field_type(self, cls, off):
        return None

    def global_type(self, addr):
        return None

    @staticmethod
    def big_offset(a, b):
        """lui k; addu r, base, k: an offset too big for one instruction. (kind "b": a class and how far into it)"""
        for x, y in ((a, b), (b, a)):
            if x and y and x[0] == "o" and y[0] == "k" and 0 < y[1] < MAX_FIELD:
                return ("b", x[1], y[1])
        return None


def load_args(traces, mods):
    names = [m.name for m in mods]
    seen = collections.defaultdict(lambda: collections.defaultdict(set))
    for path in glob.glob(os.path.join(traces, "*.args")):
        with open(path, "rb") as f:
            data = f.read()
        for mod, fn, reg, word in struct.iter_unpack("<4I", data[:len(data) & ~15]):
            seen[(names[mod], fn)][reg].add(word)
    return seen


def main():
    work, traces, snaps = sys.argv[1], sys.argv[2], sys.argv[3:]
    with open(FUNCTIONS) as f:
        functions = json.load(f)
    with open(os.path.join(work, "vtables.json")) as f:
        vtables = {int(k, 16): v for k, v in json.load(f).items()}
    with open(os.path.join(work, "methods.json")) as f:
        methods = json.load(f)
    # Structures with no vtable, reached through a global: types.toml's [[global]] with a struct. Each is a class of its own
    with open(os.path.join(HERE, "types.toml"), "rb") as f:
        declared = {int(g["address"], 16) for g in tomllib.load(f).get("global", []) if "struct" in g}
    real_vtables = set(vtables)
    for g in declared:
        vtables[g] = {}
    mods = modules.load()
    by_name = {m.name: m for m in mods}
    funcs = {}
    owners = collections.defaultdict(list)
    for m in mods:
        for x in functions[m.name]["functions"]:
            if x["end"] > x["start"]:
                funcs[(m.name, x["start"])] = x
                owners[x["start"]].append(m.name)
    starts = {m.name: {x["start"] for x in functions[m.name]["functions"]} for m in mods}

    def resolve(mod, target):
        """A direct call's callee: its own module's, else the EBOOT's, else an overlay only if it is the only one."""
        o = owners.get(target, [])
        if mod in o:
            return (mod, target)
        if "EBOOT" in o:
            return ("EBOOT", target)
        return (o[0], target) if len(o) == 1 else None

    # 1. Constructors: a stored vtable at +0 of this, and which vtables its callees store. Base from the order of the two.
    stored = {}
    calls_this = collections.defaultdict(list)

    class CtorFlow(Flow):
        pass

    for key, x in funcs.items():
        m = by_name[key[0]]
        fl = CtorFlow(m, x, starts[m.name])
        vts, callees = [], []

        def visit(a, i, name, facts, vts=vts):
            raw = i.getRaw()
            rs, rt = raw >> 21 & 31, raw >> 16 & 31
            if name == "sw" and i.getProcessedImmediate() == 0 and facts.get(rs) == ("t", 0):
                fact = facts.get(rt)
                if fact and fact[0] == "k" and fact[1] in vtables:
                    vts.append(fact[1])

        def call(a, i, name, facts, callees=callees, key=key):
            if name == "jal" and facts.get(A0) == ("t", 0):
                c = resolve(key[0], i.getInstrIndexAsVram())
                if c:
                    callees.append(c)

        fl.call = call
        fl.run({A0: ("t", 0)}, visit)
        if vts:
            stored[key] = vts
        if callees:
            calls_this[key] = callees
    base_of = {}
    ctors = collections.defaultdict(list)
    for key, vts in stored.items():
        for v in vts:
            ctors[v].append(f"{key[0]} {key[1]:x}")
    for key, vts in stored.items():
        last = vts[-1]
        chain = list(vts[:-1])
        for c in calls_this.get(key, []):
            if c in stored:
                chain.append(stored[c][-1])
        chain = [v for v in chain if v != last]
        if chain:
            base_of.setdefault(last, collections.Counter())[chain[-1]] += 1
    parent = {v: c.most_common(1)[0][0] for v, c in base_of.items()}
    # An inlined destructor stores derived then base, the reverse of a constructor, so two classes can claim each other: the base
    # is the one with a constructor that stores only itself
    for v in list(parent):
        p = parent.get(v)
        if p and parent.get(p) == v:
            alone = [k for k, vts in stored.items() if set(vts) == {v} and not any(c in stored for c in calls_this.get(k, []))]
            del parent[v if alone else p]

    def ancestors(v):
        out, seen = [v], {v}
        while v in parent and parent[v] not in seen:
            v = parent[v]
            out.append(v)
            seen.add(v)
        return out

    def lub(vs):
        """The most specific class all of vs are, or None."""
        vs = list(vs)
        if not vs:
            return None
        common = ancestors(vs[0])
        for v in vs[1:]:
            have = set(ancestors(v))
            common = [c for c in common if c in have]
        return common[0] if common else None

    # 2. What each function works on
    args = load_args(traces, mods)
    this = {}
    other_args = {}
    how = {}
    for key, regs in args.items():
        for reg, words in regs.items():
            objs = {w for w in words if w in vtables}
            if not objs or words - objs:
                continue        # never an object, or handed something else too: a helper
            c = lub(objs)
            if not c:
                continue
            if reg == A0:
                this[key] = c
                how[key] = "ran"
            else:
                other_args[f"{key[0]} {key[1]:x} {reg}"] = c
    for k, slots in methods.items():
        m, a = k.split()
        key = (m, int(a, 16))
        if key in this or key not in funcs:
            continue
        ran = args.get(key, {}).get(A0)
        if ran and any(w not in vtables for w in ran):
            continue
        c = lub({int(v, 16) for v, _o in slots})
        if c:
            this[key] = c
            how[key] = "slot"

    # 3. Pointer fields and globals, from RAM. An object reaches as far as its class's code does (objects can hold objects)
    def scan_fields(flow_class):
        fields = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
        examples = collections.defaultdict(lambda: collections.defaultdict(list))
        pointer_used = set()
        for key, x in funcs.items():
            m = by_name[key[0]]
            fl = flow_class(m, x, starts[m.name])
            entry = {A0: ("o", this[key])} if key in this else {}

            def visit(a, i, name, facts, key=key):
                kind = LOADS.get(name) or STORES.get(name)
                if not kind:
                    return
                rs = i.getRaw() >> 21 & 31
                fact = facts.get(rs)
                if fact and fact[0] == "p":
                    pointer_used.add((fact[1], fact[2]))
                if not fact or fact[0] not in ("o", "b"):
                    return
                off = i.getProcessedImmediate() + (fact[2] if fact[0] == "b" else 0)
                if not 0 <= off < MAX_FIELD:
                    return
                write = name in STORES
                fields[fact[1]][off][(kind[0], kind[1], write)] += 1
                ex = examples[fact[1]][off]
                label = f"{key[0]} {key[1]:x}"
                if len(ex) < 8 and label not in ex:
                    ex.append(label)

            fl.run(entry, visit)
        return fields, examples, pointer_used

    first, _ex, _p = scan_fields(Flow)
    extent = {}
    for v in vtables:
        e = 0x10
        for a in ancestors(v):
            for off, c in first.get(a, {}).items():
                e = max(e, off + max(k[1] for k in c))
        extent[v] = e

    pointers = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    embedded = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    global_ptr = collections.defaultdict(collections.Counter)
    instances = collections.Counter()
    for snap in snaps:
        with open(snap, "rb") as f:
            ram = f.read()
        words = struct.unpack(f"<{len(ram) // 4}I", ram[:len(ram) & ~3])
        objs = {RAM + i * 4: w for i, w in enumerate(words) if w in real_vtables}
        covered = bytearray(len(words))
        for at, cls in objs.items():
            instances[cls] += 1
            i0 = (at - RAM) // 4
            n = min(extent[cls], SCAN_FIELD) // 4
            covered[i0:i0 + n] = b"\1" * len(covered[i0:i0 + n])
            for j in range(1, n):
                if i0 + j >= len(words):
                    break
                w = words[i0 + j]
                if w in objs:
                    pointers[cls][j * 4][objs[w]] += 1
                elif w in real_vtables:
                    embedded[cls][j * 4][w] += 1
        for i in range(0, (0x09000000 - RAM) // 4):     # below the heap, outside any object
            w = words[i]
            if w in objs and not covered[i]:
                global_ptr[RAM + i * 4][objs[w]] += 1
    field_ptr = {}
    for cls, offs in pointers.items():
        for off, seen in offs.items():
            c = lub(seen)
            if c:
                field_ptr[(cls, off)] = c
    # a global: the same class family every time, in several snapshots
    global_type = {}
    for addr, seen in global_ptr.items():
        c = lub(seen)
        if c and sum(seen.values()) >= 3:
            global_type[addr] = c
    for g in declared:
        global_type[g] = g

    # 4. Code that never ran: callers that all pass one class in a0. Each caller's a0 tracked from its own `this`/fields.
    class TypedFlow(Flow):
        def field_type(self, cls, off):
            return field_ptr.get((cls, off))

        def global_type(self, addr):
            return global_type.get(addr)

    for round_no in range(6):
        passed = collections.defaultdict(list)
        for key, x in funcs.items():
            m = by_name[key[0]]
            fl = TypedFlow(m, x, starts[m.name])
            entry = {A0: ("o", this[key])} if key in this else {}

            def call(a, i, name, facts, key=key):
                if name == "jal":
                    c = resolve(key[0], i.getInstrIndexAsVram())
                    if c:
                        fact = facts.get(A0)
                        passed[c].append(fact[1] if fact and fact[0] == "o" else None)

            fl.call = call
            fl.run(entry, lambda *a: None)
        added = 0
        for key, seen in passed.items():
            if key in this or key not in funcs or None in seen:
                continue
            c = lub(set(seen))
            if args.get(key, {}).get(A0) and c not in declared:     # it ran on no object: only a declared struct has no vtable
                continue
            if c:
                this[key] = c
                how[key] = "callers"
                added += 1
        print(f"round {round_no + 1}: {added} from callers", flush=True)
        if not added:
            break

    # 5. Fields
    fields, examples, pointer_used = scan_fields(TypedFlow)
    for v in vtables:
        for off, c in fields.get(v, {}).items():
            extent[v] = max(extent[v], off + max(k[1] for k in c))

    out = {
        "classes": {f"{v:08X}": {
            "parent": f"{parent[v]:08X}" if v in parent else None,
            "slots": len(vtables[v]),
            "ctors": ctors.get(v, []),
            "declared": v in declared,
            "instances": instances.get(v, 0),
            "extent": extent[v],
            "embedded": {f"{off:x}": f"{c.most_common(1)[0][0]:08X}" for off, c in sorted(embedded.get(v, {}).items())},
            "fields": {f"{off:x}": {"access": [[k[0], k[1], k[2], n] for k, n in sorted(c.items())],
                                    "pointer": f"{field_ptr[(v, off)]:08X}" if (v, off) in field_ptr else None,
                                    "deref": (v, off) in pointer_used,
                                    "seen_in": examples[v][off]}
                       for off, c in sorted(fields.get(v, {}).items())},
            "pointer_only": {f"{off:x}": f"{p:08X}" for (w, off), p in sorted(field_ptr.items())
                             if w == v and off not in fields.get(v, {})},
        } for v in vtables},
        "this": {f"{k[0]} {k[1]:x}": [f"{v:08X}", how[k]] for k, v in sorted(this.items())},
        "args": {k: f"{v:08X}" for k, v in sorted(other_args.items())},
        "globals": {f"{a:08X}": f"{v:08X}" for a, v in sorted(global_type.items())},
    }
    with open(os.path.join(work, "classes.json"), "w") as f:
        json.dump(out, f)
    print(f"{len(vtables)} vtables; {len(parent)} with a base; {len(ctors)} with a constructor; {sum(1 for v in vtables if instances.get(v))} seen live")
    print("functions typed:", collections.Counter(how.values()), "of", len(funcs))
    print(f"other typed args: {len(other_args)}; typed globals: {len(global_type)}; pointer fields: {len(field_ptr)}")
    tot = sum(len(c) for c in fields.values())
    print(f"fields: {tot} across {len(fields)} classes")


if __name__ == "__main__":
    main()
