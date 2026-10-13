#!/usr/bin/env python3
"""Find every function in the EBOOT and the overlays, for the recompiler.

Recursive descent from entry points, in passes, each adding entry points only where they cannot be inside a function
already found (and look like a function start: a stack frame set up, or after a return or unconditional jump):
  1. the EBOOT's entry, the overlays' static constructors, and every call target (jal / bal) in the module's own code
  2. pointers into the module's own code: words in its data, and lui + addiu/ori address pairs in its code (jump table
     targets excluded - every table is found from the code first)
  3. code still not covered (after nop padding): functions are contiguous, so the first instruction after a covered
     stretch starts one
  4. calls and pointers from other modules into an overlay region - weak evidence, as the target may be meant for
     another overlay of the region; cross-module seeds that end up inside a function found otherwise are dropped
Then every function is walked again with all the starts known.

Branches stay within a function. `j` is a tail call - unless its target is within the function's extent, as CodeWarrior
also uses `j` to jump back from out-of-line blocks placed after the epilogue. `jr $ra` returns. `jr <reg>` is resolved as
a jump table when it is one (a table base from lui/addiu, an index scaled by 4, a bound from the sltiu before it, or
else the table running to the next table) - or as a tail call through a table of functions, when its targets are
functions already found or mostly start by setting up a stack frame - and recorded as unresolved otherwise (calls
through vtables, mostly).

Writes game/analysis/functions.json: per module, its functions (start, end, how found, jump tables, unresolved jrs).
"""
import argparse
import bisect
import collections
import json
import os
import sys

import rabbitizer

import modules

I = rabbitizer.InstrId
NOP = 0
JR_RA = 0x03E00008


def reg(r):
    return r.value if r is not None else None


class Function:
    def __init__(self, start, how):
        self.start = start
        self.how = how                  # entry, ctor, call, tail, pointer, xcall, xpointer, gap
        self.end = start
        self.calls = set()              # direct call targets (any module)
        self.tails = set()              # tail-jump targets
        self.tables = []                # (jr address, table address, [targets])
        self.fptables = []              # the same, for tail calls through a table of functions
        self.unresolved = []            # jr <reg> not resolved as a table
        self.problems = []


TABLES = set()     # every table address found so far: an unbounded table ends where the next one starts


def jump_table(mod, mods, start, jr_addr):
    """The targets of the jump table jr_addr jumps through, or None. Simulates the straight-line code before the jr
    (from at most 64 instructions back, not before the function's start), tracking constants, a scaled index, the table
    address and a table entry; the bound comes from the last sltiu on the index."""
    regs = {}
    bound = None
    skip = set()
    a = max(start, jr_addr - 64 * 4)
    # a table base hoisted into a callee-saved register (out of a loop, say) is set further back: track constants in
    # s0-s7 and fp from up to 4K instructions before the window
    for b in range(max(start, a - 0x4000), a, 4):
        i = mod.ins(b)
        g = reg(i.getDestinationGpr())
        if g is None or not (16 <= g <= 23 or g == 30):
            continue
        u = i.uniqueId
        if u == I.cpu_lui:
            regs[g] = ("c", i.getImmediate() << 16)
        elif u == I.cpu_addiu and reg(i.rs) == g and regs.get(g, ("",))[0] == "c":
            regs[g] = ("c", (regs[g][1] + i.getProcessedImmediate()) & 0xFFFFFFFF)
        else:
            regs.pop(g, None)
    while a < jr_addr:
        if a in skip:
            a += 4
            continue
        i = mod.ins(a)
        u = i.uniqueId
        if i.isBranchLikely():
            skip.add(a + 4)            # its delay slot runs only when it branches (away from this path)
        if i.isBranch() or i.isJump():
            a += 4
            continue
        dst = None
        val = None
        if u == I.cpu_lui:
            dst, val = reg(i.rt), ("c", i.getImmediate() << 16)
        elif u in (I.cpu_addiu, I.cpu_ori):
            dst = reg(i.rt)
            src = regs.get(reg(i.rs))
            if reg(i.rs) == 0:
                src = ("c", 0)
            if src and src[0] in ("c", "t"):
                imm = i.getProcessedImmediate() if u == I.cpu_addiu else i.getImmediate()
                val = (src[0], (src[1] + imm) & 0xFFFFFFFF if u == I.cpu_addiu else src[1] | imm)
        elif u == I.cpu_sll and i.sa == 2:
            dst, val = reg(i.rd), ("i",)
        elif u == I.cpu_addu:
            dst = reg(i.rd)
            x, y = regs.get(reg(i.rs)), regs.get(reg(i.rt))
            if x and y:
                if x[0] == "i" and y[0] == "c":
                    val = ("t", y[1])
                elif y[0] == "i" and x[0] == "c":
                    val = ("t", x[1])
                elif x[0] == "c" and y[0] == "c":
                    val = ("c", (x[1] + y[1]) & 0xFFFFFFFF)
        elif u == I.cpu_lw:
            dst = reg(i.rt)
            src = regs.get(reg(i.rs))
            if src and src[0] == "t":
                val = ("e", (src[1] + i.getProcessedImmediate()) & 0xFFFFFFFF)
        elif u == I.cpu_sltiu:
            dst, bound = reg(i.rt), i.getImmediate()
        else:
            g = i.getDestinationGpr()
            dst = reg(g)
        if dst is not None and dst != 0:
            if val is None:
                regs.pop(dst, None)
            else:
                regs[dst] = val
        a += 4
    v = regs.get(reg(mod.ins(jr_addr).rs))
    if not v or v[0] != "e":
        return None
    table = v[1]
    holder = next((m for m in (mod, mods[0]) if m.contains(table)), None)
    if holder is None:
        return None
    targets = []
    TABLES.add(table)
    for k in range(bound if bound else 1024):
        addr = table + 4 * k
        if not holder.contains(addr + 3) or (k and not bound and addr in TABLES):
            break
        t = holder.word(addr)
        if not (mod.in_text(t) and t >= start and t % 4 == 0):
            if bound:
                return None            # a bounded table must be all targets
            break
        targets.append(t)
    return (table, targets) if targets else None


class Discovery:
    def __init__(self, mods):
        self.mods = mods
        self.funcs = {m.name: {} for m in mods}     # module -> start -> Function
        self.starts = {m.name: [] for m in mods}    # sorted starts, for interior tests
        self.mod_of = {}

    def interior(self, mod, addr):
        """The function whose body contains addr (not at its start), if any."""
        fs = self.funcs[mod.name]
        st = self.starts[mod.name]
        k = bisect.bisect_right(st, addr) - 1
        if k >= 0:
            f = fs[st[k]]
            if f.start < addr < f.end:
                return f
        return None

    def add(self, mod, addr, how, queue):
        if addr in self.funcs[mod.name] or not mod.in_text(addr) or addr % 4:
            return False
        f = Function(addr, how)
        self.funcs[mod.name][addr] = f
        bisect.insort(self.starts[mod.name], addr)
        queue.append((mod, f))
        return True

    def walk(self, mod, f):
        """Follow f's control flow. A `j` is usually a tail call, but CodeWarrior also uses it to jump back from
        out-of-line blocks placed after the epilogue, so a `j` to an address within the function's extent so far is
        followed instead, until the extent stops growing."""
        known = self.funcs[mod.name]
        seen = set()
        jumps = set()
        work = [f.start]
        while True:
            self._walk(mod, f, known, seen, jumps, work)
            end = max(seen) + 4
            inside = [t for t in jumps if f.start <= t < end and t not in seen]
            if not inside:
                break
            work.extend(inside)
        f.tails |= {t for t in jumps if not f.start <= t < end}
        f.end = end

    def _walk(self, mod, f, known, seen, jumps, work):
        while work:
            a = work.pop()
            while True:
                if a in seen:
                    break
                if not mod.in_text(a):
                    f.problems.append(f"runs out of text at {a:#x}")
                    break
                if a != f.start and a in known:
                    f.tails.add(a)       # ran into the next function: treat as a tail call
                    break
                seen.add(a)
                i = mod.ins(a)
                if i.isBranch():
                    t = i.getBranchVramGeneric()
                    seen.add(a + 4)
                    if i.doesLink():
                        f.calls.add(t)
                        a += 8
                        continue
                    if t != f.start and t in known:
                        f.tails.add(t)
                    else:
                        work.append(t)
                    if i.isUnconditionalBranch():
                        break
                    a += 8
                    continue
                if i.isJumpWithAddress():
                    t = i.getInstrIndexAsVram()
                    if i.doesLink():
                        f.calls.add(t)
                        a += 4
                        continue
                    seen.add(a + 4)
                    if t != f.start and t in known:
                        f.tails.add(t)
                    else:
                        jumps.add(t)
                    break
                if i.isJrRa():
                    seen.add(a + 4)
                    break
                if i.isJrNotRa():
                    seen.add(a + 4)
                    jt = jump_table(mod, self.mods, f.start, a)
                    if jt and (any(t != f.start and t in known for t in jt[1]) or
                               2 * sum(self.frame_setup(mod, t) for t in jt[1]) >= len(jt[1])):
                        # a table of functions (return handlers[i]()): a tail call through it
                        f.tails.update(jt[1])
                        if all(x[0] != a for x in f.fptables):
                            f.fptables.append((a, jt[0], jt[1]))
                    elif jt:
                        if all(x[0] != a for x in f.tables):
                            f.tables.append((a, jt[0], jt[1]))
                        work.extend(t for t in jt[1] if t not in seen)
                    elif a not in f.unresolved:
                        f.unresolved.append(a)
                    break
                a += 4

    def run(self, queue):
        while queue:
            mod, f = queue.pop()
            self.walk(mod, f)
            for t in f.tails | f.calls:
                if mod.in_text(t) and not (t in f.tails and self.interior(mod, t)):
                    self.add(mod, t, "tail" if t in f.tails else "call", queue)

    def seed(self, cands, queue, label, log):
        """Add the candidates that are plausible function starts outside any function, in rounds (each round's
        functions can put later candidates inside them)."""
        rounds = 0
        while True:
            stats = collections.Counter()
            for c, t, how in cands:
                if t in self.funcs[c.name]:
                    stats["already a function"] += 1
                elif self.interior(c, t):
                    stats["inside a function"] += 1
                elif not self.plausible_start(c, t):
                    stats["not a function start"] += 1
                else:
                    stats["new"] += self.add(c, t, how, queue)
            rounds += 1
            if not queue:
                break
            self.run(queue)
        log(f"{label}, {rounds} rounds", self.count(), dict(stats))

    @staticmethod
    def frame_setup(m, a):
        i = m.ins(a)
        return i.uniqueId == I.cpu_addiu and reg(i.rs) == 29 and reg(i.rt) == 29 and i.getProcessedImmediate() < 0

    def plausible_start(self, m, a):
        """Does a look like the start of a function: a stack frame being set up, or the first instruction after an
        unconditional jump or return (and its delay slot), past any nop padding?"""
        if self.frame_setup(m, a):
            return True
        if any(a == s for s, _ in m.ranges):
            return True
        q = a - 4
        padded = False
        while m.in_text(q) and m.word(q) == NOP and a - q <= 32:
            q -= 4
            padded = True
        if not m.in_text(q):
            return True

        def ends(x):
            if not m.in_text(x):
                return False
            j = m.ins(x)
            return j.isJrRa() or j.isJrNotRa() or (j.isJumpWithAddress() and not j.doesLink()) or \
                (j.isUnconditionalBranch() and not j.doesLink())
        return ends(q - 4) or (padded and ends(q))

    def table_labels(self):
        """Every jump table's targets, found from the code alone (before function discovery) so that pointers to
        case labels in the tables are never taken for functions."""
        labels = collections.defaultdict(set)
        for m in self.mods:
            for a in m.code():
                if m.ins(a).isJrNotRa():
                    jt = jump_table(m, self.mods, m.text_start, a)
                    if jt:
                        labels[m.name].update(jt[1])
        return labels

    def drop_covered_xseeds(self):
        """Remove functions seeded only from other modules (xcall, xpointer) that lie inside a function the module's
        own evidence found: the call or pointer was meant for another overlay of the region."""
        dropped = 0
        for mod in self.mods:
            fs = self.funcs[mod.name]
            st = self.starts[mod.name]
            spans = [(f.start, f.end) for f in fs.values() if f.how not in ("xcall", "xpointer", "gap")]
            spans.sort()
            for s in list(st):
                f = fs[s]
                if f.how not in ("xcall", "xpointer"):
                    continue
                k = bisect.bisect_left(spans, (s,)) - 1
                if k >= 0 and spans[k][0] < s < spans[k][1]:
                    del fs[s]
                    st.remove(s)
                    dropped += 1
        return dropped

    def resolve_conflicts(self):
        """Report functions whose walk ran over a later start."""
        for mod in self.mods:
            st = self.starts[mod.name]
            fs = self.funcs[mod.name]
            for k in range(len(st) - 1):
                f = fs[st[k]]
                if f.end > st[k + 1]:
                    f.problems.append(f"overlaps {st[k + 1]:#x} (walked to {f.end:#x})")

    def targets_in(self, region_mods, addr):
        return [m for m in region_mods if m.in_text(addr)]

    def discover(self, log):
        mods = self.mods
        by_region = collections.defaultdict(list)
        for m in mods:
            by_region[m.region].append(m)
        queue = []
        # 1. entries, own calls and tail jumps
        self.add(mods[0], mods[0].entry, "entry", queue)
        for m in mods:
            for c in m.ctors:
                self.add(m, c, "ctor", queue)
        own = collections.defaultdict(set)      # module -> its jal / j / bal targets
        for m in mods:
            for a in m.code():
                i = m.ins(a)
                if i.isJumpWithAddress() or (i.isBranch() and i.doesLink()):
                    t = i.getInstrIndexAsVram() if i.isJumpWithAddress() else i.getBranchVramGeneric()
                    own[m.name].add(t)
                    if i.doesLink() and m.in_text(t):    # a j within the module may be intra-function: walk decides
                        self.add(m, t, "call", queue)
        self.run(queue)
        log("pass 1 (own calls)", self.count())

        # 2. pointers into the module's own text; and (4, below) calls and pointers from other modules into overlay
        # regions - weak evidence, as the target may be meant for another overlay of the region, so only for code
        # still uncovered after the gaps
        labels = self.table_labels()
        log("jump table labels", sum(len(v) for v in labels.values()))
        own_ptrs, x = [], []
        for m in mods:
            for t in self.pointers(m):
                for c in self.pointer_targets(m, t, by_region):
                    if t not in labels[c.name]:
                        (own_ptrs if c is m else x).append((c, t, "pointer" if c is m else "xpointer"))
            for t in own[m.name]:
                if not m.in_text(t):
                    x.extend((c, t, "xcall") for region, rmods in by_region.items()
                             if region not in (0, m.region) for c in self.targets_in(rmods, t))
        self.seed(own_ptrs, queue, "pass 2 (own pointers)", log)

        # 3. uncovered text: functions are contiguous, so the first instruction after a covered stretch starts one
        log(f"pass 3 (gaps, {self.fill_gaps(queue)} rounds)", self.count())
        self.seed(x, queue, "pass 4 (calls and pointers from other modules)", log)

        # every function again, now that all the starts are known (an early walk may have run through later ones);
        # a function that ends earlier can leave a gap, so fill those and repeat
        rounds = 0
        while True:
            self.rewalk(queue)
            rounds += 1
            if not self.fill_gaps(queue):
                break
        log(f"re-walk ({rounds} rounds)", self.count())
        log("dropped cross-module seeds inside the module's own functions:", self.drop_covered_xseeds())
        self.resolve_conflicts()

    def fill_gaps(self, queue):
        """Start functions in uncovered code until there is none; returns the number of rounds that added any."""
        rounds = 0
        while True:
            added = 0
            for m in self.mods:
                for a in self.gaps(m):
                    added += self.add(m, a, "gap", queue)
            if not added:
                return rounds
            self.run(queue)
            rounds += 1

    def rewalk(self, queue):
        while True:
            for m in self.mods:
                for f in list(self.funcs[m.name].values()):
                    f.calls, f.tails, f.tables, f.fptables, f.unresolved, f.problems = set(), set(), [], [], [], []
                    self.walk(m, f)
                    for t in f.tails | f.calls:
                        if m.in_text(t) and not (t in f.tails and self.interior(m, t)):
                            self.add(m, t, "tail" if t in f.tails else "call", queue)
            if not queue:
                return
            self.run(queue)

    def pointers(self, m):
        """Candidate code addresses: aligned words in m's data, and lui + addiu/ori pairs in m's code."""
        out = set()
        for a in m.data_words():
            out.add(m.word(a))
        last_lui = {}
        for a in m.code():
            i = m.ins(a)
            u = i.uniqueId
            if u == I.cpu_lui:
                last_lui[reg(i.rt)] = (a, i.getImmediate() << 16)
            elif u in (I.cpu_addiu, I.cpu_ori) and reg(i.rs) in last_lui:
                la, hi = last_lui[reg(i.rs)]
                if a - la <= 64 * 4:
                    out.add((hi + i.getProcessedImmediate()) & 0xFFFFFFFF if u == I.cpu_addiu else hi | i.getImmediate())
            if i.isJrRa():
                last_lui.clear()
        return {p for p in out if p % 4 == 0 and 0x08804000 <= p < 0x0A000000}

    def pointer_targets(self, m, t, by_region):
        if m.in_text(t):
            return [m]
        if self.mods[0].in_text(t):
            return [self.mods[0]]
        return [c for region, rmods in by_region.items() if region not in (0, m.region)
                for c in self.targets_in(rmods, t)]

    def gaps(self, m):
        """The first non-nop instruction of each stretch of text not covered by a function."""
        st = self.starts[m.name]
        fs = self.funcs[m.name]
        out = []
        for lo, hi in m.ranges:
            covered_to = lo
            for s in [x for x in st if lo <= x < hi] + [hi]:
                a = covered_to
                while a < s and m.word(a) == NOP:
                    a += 4
                if a < s:
                    out.append(a)
                if s in fs:
                    covered_to = max(covered_to, fs[s].end)
        return out

    def check(self, log):
        """Code no function covers (other than nop padding), and how calls between modules resolve: a call into an
        overlay region should have a function at the target in at least one overlay of the region."""
        uncovered = 0
        for m in self.mods:
            cov = bytearray(len(m.data))
            for f in self.funcs[m.name].values():
                cov[f.start - m.base:f.end - m.base] = b"\1" * (f.end - f.start)
            uncovered += sum(1 for a in m.code() if not cov[a - m.base] and m.word(a) != NOP)
        by_region = collections.defaultdict(list)
        for m in self.mods:
            by_region[m.region].append(m)
        stats = collections.Counter()
        main = self.mods[0]
        for m in self.mods:
            for a in m.code():
                i = m.ins(a)
                if not i.isJumpWithAddress():
                    continue
                t = i.getInstrIndexAsVram()
                if m.in_text(t) or t in main.imports:
                    continue
                if main.in_text(t):
                    stats["into the EBOOT" + ("" if t in self.funcs["EBOOT"] or not i.doesLink() else ", not a start")] += 1
                    continue
                hits = [c for region, cs in by_region.items() if region not in (0, m.region)
                        for c in cs if t in self.funcs[c.name]]
                stats[f"into overlays, {min(len(hits), 2)}{'+' if len(hits) > 1 else ''} with a function there"] += 1
        log(f"uncovered non-nop words: {uncovered}; calls between modules: {dict(sorted(stats.items()))}")

    def count(self):
        return sum(len(v) for v in self.funcs.values())

    def report(self):
        out = {}
        for m in self.mods:
            fs = [self.funcs[m.name][s] for s in self.starts[m.name]]
            covered = sum(f.end - f.start for f in fs)
            out[m.name] = {
                "region": m.region, "base": m.base, "ranges": m.ranges, "coverage": covered,
                "functions": [{"start": f.start, "end": f.end, "how": f.how,
                               **({"tables": [[a, t, ts] for a, t, ts in f.tables]} if f.tables else {}),
                               **({"fptables": [[a, t, ts] for a, t, ts in f.fptables]} if f.fptables else {}),
                               **({"unresolved": f.unresolved} if f.unresolved else {}),
                               **({"problems": f.problems} if f.problems else {})} for f in fs],
            }
        if self.mods[0].imports:
            out["EBOOT"]["imports"] = {str(a): [lib, nid] for a, (lib, nid) in sorted(self.mods[0].imports.items())}
        return out


def summary(result, log):
    total = collections.Counter()
    for name, r in result.items():
        fs = r["functions"]
        text = sum(e - s for s, e in r["ranges"])
        total["modules"] += 1
        total["functions"] += len(fs)
        total["text"] += text
        total["covered"] += min(r["coverage"], text)
        for f in fs:
            total["how " + f["how"]] += 1
            total["jump tables"] += len(f.get("tables", []))
            total["function tables"] += len(f.get("fptables", []))
            total["unresolved jr"] += len(f.get("unresolved", []))
            total["functions with problems"] += bool(f.get("problems"))
    log(f"{total['functions']} functions in {total['modules']} modules; text covered "
        f"{total['covered'] / total['text']:.2%}")
    log("  " + ", ".join(f"{k}: {v}" for k, v in sorted(total.items()) if k.startswith("how ")))
    log(f"  jump tables {total['jump tables']}, function tables {total['function tables']}, "
        f"unresolved jr {total['unresolved jr']}, "
        f"functions with problems {total['functions with problems']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--out", default=os.path.join(modules.ROOT, "game/analysis/functions.json"))
    args = ap.parse_args()

    def log(*a):
        print(*a, file=sys.stderr)

    d = Discovery(modules.load())
    d.discover(log)
    d.check(log)
    result = d.report()
    summary(result, log)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(result, f)


if __name__ == "__main__":
    main()
