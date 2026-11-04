#!/usr/bin/env python3
"""Translate the game's code to C: every function found by tools/funcs.py becomes a C function on the CPU state.

Output (default build/gen/): <module>[_<n>].c with the functions (the EBOOT split into several files), funcs.h with
their prototypes, and tables.c with each module's function table (for dispatch by address) and the imports.

Control flow: a function's instructions are emitted in address order, each with a label if anything jumps to it.
A branch evaluates its condition, runs its delay slot, then jumps (a branch-likely runs the delay slot only when it
branches). Calls are direct within a module and into the EBOOT, through rt_call into overlay regions and through
pointers, and through hle_call to the imports. A jump out of the function is a tail call. Jump tables become switches.
VFPU instructions, except its branches, go to the runtime (vfpu_exec).
"""
import argparse
import collections
import glob
import json
import os
import sys
import tomllib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import rabbitizer  # noqa: E402

rabbitizer.config.pseudos_enablePseudos = False

import modules  # noqa: E402
from patches import COUNTED, GATES, NOT_PLACED, PATCHES, PLACED  # noqa: E402

# RECOMP_TRACE=hex,...: functions whose every call is logged at run time (MHFU_CALL_LOG: native.c), for finding who
# calls what at which rate
TRACE = {int(x, 16) for x in os.environ.get("RECOMP_TRACE", "").split(",") if x}
# RECOMP_TRACE_ALL=1: every function logs that it ran and from where (tools/readable/trace_hook.c, linked by
# `make TRACE_HOOK=tools/readable/trace_hook.c`): the readable view's evidence of what drives what. Never for the game.
TRACE_ALL = bool(os.environ.get("RECOMP_TRACE_ALL"))

# a gate's kind (patches.py's GATES) -> the runtime's function for it (runtime/src/native.c)
GATE_FUNCTIONS = {"on": "rt_gate", "before": "rt_gate_before", "object": "rt_gate_object", "native": "rt_gate_native"}

ROOT = modules.ROOT


def R(n):
    return "0" if n == 0 else f"c->r[{n}]"


def S(n):
    return "0" if n == 0 else f"(int32_t)c->r[{n}]"


def F(n):
    return f"c->f[{n}].f"


def FU(n):
    return f"c->f[{n}].u"


def hx(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Unsupported(Exception):
    pass


# VFPU operations the runtime implements (runtime/src/vfpu.c), as vfpu_<name>(Ctx *, uint32_t instr); any other VFPU
# instruction goes to vfpu_exec, which stops the game
VFPU_OPS = {
    "lv_s", "sv_s", "lv_q", "sv_q", "mtv", "mfv", "vpfxs", "vpfxt", "vpfxd",
    "vadd", "vsub", "vmul", "vdiv", "vmin", "vmax", "vmov", "vabs", "vneg", "vrcp", "vrsq", "vsqrt", "vsin", "vcos",
    "vexp2", "vlog2", "vasin", "vocp", "vsgn", "vzero", "vone", "vcst", "vidt", "vscl", "vdot", "vfad", "vcrsp",
    "vsocp", "vfim", "vi2f", "vf2id", "vf2iu", "vf2iz", "vf2in", "vcmp", "vcmovt", "vcmovf", "vrot", "vmidt", "vmmov",
    "vmmul", "vtfm2", "vtfm3", "vtfm4",
}


def vfpu_op(name):
    """The runtime function for a VFPU instruction (rabbitizer's name for it, without the r4000allegrex_ prefix)."""
    if name in ("lv_s", "sv_s", "lv_q", "sv_q"):
        return name
    base = name[:-2] if name[-2:] in ("_s", "_p", "_t", "_q") else name
    return base if base in VFPU_OPS else None


class Translator:
    def __init__(self, mods, functions):
        self.patched = 0
        self.counted = 0
        self.placed = 0
        self.gated = 0
        self.mods = mods
        self.by_name = {m.name: m for m in mods}
        self.main = mods[0]
        self.functions = functions                       # module name -> [function dicts]
        self.starts = {n: {f["start"] for f in fs} for n, fs in functions.items()}
        self.vfpu_used = set()

    def fname(self, mod, addr):
        return f"f_{mod.name}_{addr:08X}"

    # -- calls -------------------------------------------------------------------------------------------------
    def call(self, mod, target):
        """C for a call to target from mod."""
        if mod.in_text(target):
            if target not in self.starts[mod.name]:
                raise Unsupported(f"call to {target:#x}, not a function start in {mod.name}")
            return f"{self.fname(mod, target)}(c);"
        if self.main.in_text(target):
            if target not in self.starts["EBOOT"]:
                raise Unsupported(f"call to {target:#x}, not a function start in the EBOOT")
            return f"{self.fname(self.main, target)}(c);"
        if target in self.main.imports:
            return f"hle_call(c, {hx(target)});"
        return f"rt_call(c, {hx(target)});"

    def count_step(self, mod, a):
        """A count of game frames (patches.py COUNTED): addiu $x, $x, 1 (or -1) stored back by the next instruction to
        one of the module's counted fields (or by the one after, a branch between) - the step a 30th, not a game
        frame"""
        fields = COUNTED.get(mod.name)
        if not fields:
            return None
        w = mod.ins(a).getRaw()
        rs, rt, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if w >> 26 != 0x09 or rs != rt or rt == 0 or imm not in (1, 0xFFFF):
            return None
        for b in (a + 4, a + 8):
            if not mod.in_text(b):
                return None
            x = mod.ins(b).getRaw()
            if x >> 26 in (0x28, 0x29, 0x2B) and (x >> 16) & 31 == rt and (x & 0xFFFF) in fields:
                self.counted += 1
                return (f"{R(rt)} = {R(rs)} {'+' if imm == 1 else '-'} (uint32_t)rt_tick30;"
                        " /* counted in 30ths (recomp/patches.py COUNTED) */")
            if b == a + 4 and not (mod.ins(b).isBranch() or mod.ins(b).doesStore()):
                return None
        return None

    def place_step(self, mod, a):
        """A step of a character's place (patches.py PLACED): add.s (or sub.s) $fd, $fs, $ft stored by the next
        instruction or the one after to one of the module's place fields, $fs loaded from that field (same base) in
        the 8 before - the step ($ft: a displacement, a velocity) times the time scale. Not where a patch is among
        those 8 (the step scaled there already), nor those in NOT_PLACED (a place moved once)"""
        if (mod.name, a) in NOT_PLACED:
            return None
        fields = PLACED.get(mod.name)
        if fields is None:
            fields = PLACED.get("em") if mod.name.startswith("em") else None
        if not fields:
            return None
        w = mod.ins(a).getRaw()
        if w >> 26 != 0x11 or (w >> 21) & 31 != 16 or w & 0x3F not in (0, 1):
            return None
        fd, fs, ft = (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31
        for b in (a + 4, a + 8):
            if not mod.in_text(b):
                return None
            x = mod.ins(b).getRaw()
            if x >> 26 == 0x39 and (x >> 16) & 31 == fd and (x & 0xFFFF) in fields:
                base, off = (x >> 21) & 31, x & 0xFFFF
                if any(not mod.in_text(a - 4 * k) or (mod.name, a - 4 * k) in PATCHES for k in range(1, 9)):
                    return None
                for k in range(1, 9):
                    y = mod.ins(a - 4 * k).getRaw()
                    if y >> 26 == 0x31 and (y >> 16) & 31 in (fs, ft) and (y >> 21) & 31 == base and y & 0xFFFF == off:
                        place, step = ((fs, ft) if (y >> 16) & 31 == fs else (ft, fs))
                        if place == step or (place == ft and w & 0x3F == 1):
                            return None
                        self.placed += 1
                        op = "-" if w & 0x3F == 1 else "+"
                        if place == fs:
                            expr = f"c->f[{fs}].f {op} c->f[{ft}].f * rt_time_scale"
                        else:
                            expr = f"c->f[{fs}].f * rt_time_scale {op} c->f[{ft}].f"
                        return f"c->f[{fd}].f = {expr}; /* a place's step in game time (recomp/patches.py PLACED) */"
                return None
            if b == a + 4 and not (mod.ins(b).isBranch() or mod.ins(b).doesStore()) and \
                    (x >> 26 != 0x31 or (x >> 16) & 31 == fd):
                return None
        return None

    # -- one instruction (not control flow) --------------------------------------------------------------------
    def op(self, mod, a):
        i = mod.ins(a)
        patch = PATCHES.get((mod.name, a))
        if patch:
            expected, code, _why = patch
            if " ".join(i.disassemble().split()) != " ".join(expected.split()):
                raise SystemExit(f"patch at {mod.name} {a:#x}: expected {expected!r}, found {i.disassemble()!r}")
            self.patched += 1
            return code + " /* patched (recomp/patches.py) */"
        step = self.count_step(mod, a) or self.place_step(mod, a)
        if step:
            return step
        n = i.uniqueId.name
        w = i.getRaw()
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        sa = (w >> 6) & 31
        uimm = w & 0xFFFF
        simm = uimm - 0x10000 if uimm & 0x8000 else uimm
        fs, ft, fd = (w >> 11) & 31, (w >> 16) & 31, (w >> 6) & 31
        ea = f"({R(rs)} + {hx(simm)})"

        def setr(dst, expr):
            return "" if dst == 0 else f"{R(dst)} = {expr};"

        if n.startswith("r4000allegrex_") and (n.startswith("r4000allegrex_v") or n[14:] in VFPU_OPS):
            fn = vfpu_op(n[14:])
            if fn:
                self.vfpu_used.add(fn)
                return f"vfpu_{fn}(c, {hx(w)});"
        if n.startswith("r4000allegrex_v") or n in ("r4000allegrex_lv_q", "r4000allegrex_lv_s", "r4000allegrex_sv_q",
                                                     "r4000allegrex_sv_s", "r4000allegrex_mtv", "r4000allegrex_mfv",
                                                     "r4000allegrex_mtvc", "r4000allegrex_mfvc", "r4000allegrex_lvl_q",
                                                     "r4000allegrex_lvr_q", "r4000allegrex_svl_q",
                                                     "r4000allegrex_svr_q", "r4000allegrex_vpfxs",
                                                     "r4000allegrex_vpfxt", "r4000allegrex_vpfxd"):
            return f"vfpu_exec(c, {hx(w)}, {hx(a)});"
        T = {
            # integer
            "cpu_nop": lambda: "",
            "cpu_sll": lambda: setr(rd, f"{R(rt)} << {sa}"),
            "cpu_srl": lambda: setr(rd, f"{R(rt)} >> {sa}"),
            "r4000allegrex_srl": lambda: setr(rd, f"{R(rt)} >> {sa}"),
            "r4000allegrex_rotr": lambda: setr(rd, f"rt_rotr({R(rt)}, {sa})"),
            "cpu_sra": lambda: setr(rd, f"(uint32_t)({S(rt)} >> {sa})"),
            "cpu_sllv": lambda: setr(rd, f"{R(rt)} << ({R(rs)} & 31)"),
            "cpu_srlv": lambda: setr(rd, f"{R(rt)} >> ({R(rs)} & 31)"),
            "r4000allegrex_srlv": lambda: setr(rd, f"{R(rt)} >> ({R(rs)} & 31)"),
            "r4000allegrex_rotrv": lambda: setr(rd, f"rt_rotr({R(rt)}, {R(rs)})"),
            "cpu_srav": lambda: setr(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))"),
            "cpu_add": lambda: setr(rd, f"{R(rs)} + {R(rt)}"),
            "cpu_addu": lambda: setr(rd, f"{R(rs)} + {R(rt)}"),
            "cpu_sub": lambda: setr(rd, f"{R(rs)} - {R(rt)}"),
            "cpu_subu": lambda: setr(rd, f"{R(rs)} - {R(rt)}"),
            "cpu_and": lambda: setr(rd, f"{R(rs)} & {R(rt)}"),
            "cpu_or": lambda: setr(rd, f"{R(rs)} | {R(rt)}"),
            "cpu_xor": lambda: setr(rd, f"{R(rs)} ^ {R(rt)}"),
            "cpu_nor": lambda: setr(rd, f"~({R(rs)} | {R(rt)})"),
            "cpu_slt": lambda: setr(rd, f"{S(rs)} < {S(rt)}"),
            "cpu_sltu": lambda: setr(rd, f"{R(rs)} < {R(rt)}"),
            "cpu_movz": lambda: "" if rd == 0 else f"if ({R(rt)} == 0) {R(rd)} = {R(rs)};",
            "cpu_movn": lambda: "" if rd == 0 else f"if ({R(rt)} != 0) {R(rd)} = {R(rs)};",
            "cpu_addi": lambda: setr(rt, f"{R(rs)} + {hx(simm)}"),
            "cpu_addiu": lambda: setr(rt, f"{R(rs)} + {hx(simm)}"),
            "cpu_slti": lambda: setr(rt, f"{S(rs)} < {simm}"),
            "cpu_sltiu": lambda: setr(rt, f"{R(rs)} < {hx(simm)}"),
            "cpu_andi": lambda: setr(rt, f"{R(rs)} & {hx(uimm)}"),
            "cpu_ori": lambda: setr(rt, f"{R(rs)} | {hx(uimm)}"),
            "cpu_xori": lambda: setr(rt, f"{R(rs)} ^ {hx(uimm)}"),
            "cpu_lui": lambda: setr(rt, hx(uimm << 16)),
            "cpu_mfhi": lambda: setr(rd, "c->hi"),
            "cpu_mflo": lambda: setr(rd, "c->lo"),
            "cpu_mthi": lambda: f"c->hi = {R(rs)};",
            "cpu_mtlo": lambda: f"c->lo = {R(rs)};",
            "cpu_mult": lambda: f"{{ int64_t p = (int64_t){S(rs)} * {S(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            "cpu_multu": lambda: f"{{ uint64_t p = (uint64_t){R(rs)} * {R(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            "cpu_div": lambda: f"rt_div(c, {S(rs)}, {S(rt)});",
            "cpu_divu": lambda: f"rt_divu(c, {R(rs)}, {R(rt)});",
            "r4000allegrex_madd": lambda: f"{{ int64_t p = (int64_t)(((uint64_t)c->hi << 32) | c->lo) + (int64_t){S(rs)} * {S(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }}",
            "r4000allegrex_maddu": lambda: f"{{ uint64_t p = (((uint64_t)c->hi << 32) | c->lo) + (uint64_t){R(rs)} * {R(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            "r4000allegrex_msub": lambda: f"{{ int64_t p = (int64_t)(((uint64_t)c->hi << 32) | c->lo) - (int64_t){S(rs)} * {S(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); }}",
            "r4000allegrex_msubu": lambda: f"{{ uint64_t p = (((uint64_t)c->hi << 32) | c->lo) - (uint64_t){R(rs)} * {R(rt)}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}",
            "r4000allegrex_clz": lambda: setr(rd, f"rt_clz({R(rs)})"),
            "r4000allegrex_clo": lambda: setr(rd, f"rt_clz(~{R(rs)})"),
            "r4000allegrex_max": lambda: setr(rd, f"{S(rs)} > {S(rt)} ? {R(rs)} : {R(rt)}"),
            "r4000allegrex_min": lambda: setr(rd, f"{S(rs)} < {S(rt)} ? {R(rs)} : {R(rt)}"),
            "r4000allegrex_seb": lambda: setr(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}"),
            "r4000allegrex_seh": lambda: setr(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}"),
            "r4000allegrex_wsbh": lambda: setr(rd, f"((({R(rt)}) & 0xFF00FF00u) >> 8) | ((({R(rt)}) & 0x00FF00FFu) << 8)"),
            "r4000allegrex_wsbw": lambda: setr(rd, f"__builtin_bswap32({R(rt)})"),
            "r4000allegrex_bitrev": lambda: setr(rd, f"rt_bitrev({R(rt)})"),
            "r4000allegrex_ext": lambda: setr(rt, f"({R(rs)} >> {sa}) & {hx((1 << (rd + 1)) - 1 if rd < 31 else 0xFFFFFFFF)}"),
            "r4000allegrex_ins": lambda: (lambda pos, size: setr(rt, f"({R(rt)} & ~{hx(((1 << size) - 1) << pos)}) | (({R(rs)} << {pos}) & {hx(((1 << size) - 1) << pos)})"))(sa, rd - sa + 1),
            # memory
            "cpu_lb": lambda: setr(rt, f"(uint32_t)(int32_t)LB({ea})"),
            "cpu_lbu": lambda: setr(rt, f"LBU({ea})"),
            "cpu_lh": lambda: setr(rt, f"(uint32_t)(int32_t)LH({ea})"),
            "cpu_lhu": lambda: setr(rt, f"LHU({ea})"),
            "cpu_lw": lambda: setr(rt, f"LW({ea})"),
            "cpu_ll": lambda: setr(rt, f"LW({ea})"),
            "cpu_lwl": lambda: setr(rt, f"rt_lwl(c, {ea}, {R(rt)})"),
            "cpu_lwr": lambda: setr(rt, f"rt_lwr(c, {ea}, {R(rt)})"),
            "cpu_sb": lambda: f"SB({ea}, {R(rt)});",
            "cpu_sh": lambda: f"SH({ea}, {R(rt)});",
            "cpu_sw": lambda: f"SW({ea}, {R(rt)});",
            "cpu_sc": lambda: f"SW({ea}, {R(rt)});" + ("" if rt == 0 else f" {R(rt)} = 1;"),
            "cpu_swl": lambda: f"rt_swl(c, {ea}, {R(rt)});",
            "cpu_swr": lambda: f"rt_swr(c, {ea}, {R(rt)});",
            "cpu_lwc1": lambda: f"{FU(ft)} = LW({ea});",
            "cpu_swc1": lambda: f"SW({ea}, {FU(ft)});",
            "cpu_sync": lambda: "",
            "cpu_cache": lambda: "",
            "cpu_break": lambda: f"rt_break(c, {hx(a)});",
            # FPU
            "cpu_add_s": lambda: f"{F(fd)} = {F(fs)} + {F(ft)};",
            "cpu_sub_s": lambda: f"{F(fd)} = {F(fs)} - {F(ft)};",
            "cpu_mul_s": lambda: f"{F(fd)} = {F(fs)} * {F(ft)};",
            "cpu_div_s": lambda: f"{F(fd)} = {F(fs)} / {F(ft)};",
            "cpu_sqrt_s": lambda: f"{F(fd)} = __builtin_sqrtf({F(fs)});",
            "cpu_abs_s": lambda: f"{FU(fd)} = {FU(fs)} & 0x7FFFFFFFu;",
            "cpu_neg_s": lambda: f"{FU(fd)} = {FU(fs)} ^ 0x80000000u;",
            "cpu_mov_s": lambda: f"{FU(fd)} = {FU(fs)};",
            "cpu_cvt_s_w": lambda: f"{F(fd)} = (float)c->f[{fs}].i;",
            "cpu_cvt_w_s": lambda: f"{FU(fd)} = rt_trunc_w(__builtin_rintf({F(fs)}));",
            "cpu_trunc_w_s": lambda: f"{FU(fd)} = rt_trunc_w({F(fs)});",
            "cpu_round_w_s": lambda: f"{FU(fd)} = rt_trunc_w(__builtin_rintf({F(fs)}));",
            "cpu_ceil_w_s": lambda: f"{FU(fd)} = rt_trunc_w(__builtin_ceilf({F(fs)}));",
            "cpu_floor_w_s": lambda: f"{FU(fd)} = rt_trunc_w(__builtin_floorf({F(fs)}));",
            "cpu_c_f_s": lambda: "c->fpcond = 0;",
            "cpu_c_un_s": lambda: f"c->fpcond = {F(fs)} != {F(fs)} || {F(ft)} != {F(ft)};",
            "cpu_c_eq_s": lambda: f"c->fpcond = {F(fs)} == {F(ft)};",
            "cpu_c_ueq_s": lambda: f"c->fpcond = !({F(fs)} < {F(ft)} || {F(fs)} > {F(ft)});",
            "cpu_c_olt_s": lambda: f"c->fpcond = {F(fs)} < {F(ft)};",
            "cpu_c_ult_s": lambda: f"c->fpcond = !({F(fs)} >= {F(ft)});",
            "cpu_c_ole_s": lambda: f"c->fpcond = {F(fs)} <= {F(ft)};",
            "cpu_c_ule_s": lambda: f"c->fpcond = !({F(fs)} > {F(ft)});",
            "cpu_c_sf_s": lambda: "c->fpcond = 0;",
            "cpu_c_ngle_s": lambda: f"c->fpcond = {F(fs)} != {F(fs)} || {F(ft)} != {F(ft)};",
            "cpu_c_seq_s": lambda: f"c->fpcond = {F(fs)} == {F(ft)};",
            "cpu_c_ngl_s": lambda: f"c->fpcond = !({F(fs)} < {F(ft)} || {F(fs)} > {F(ft)});",
            "cpu_c_lt_s": lambda: f"c->fpcond = {F(fs)} < {F(ft)};",
            "cpu_c_nge_s": lambda: f"c->fpcond = !({F(fs)} >= {F(ft)});",
            "cpu_c_le_s": lambda: f"c->fpcond = {F(fs)} <= {F(ft)};",
            "cpu_c_ngt_s": lambda: f"c->fpcond = !({F(fs)} > {F(ft)});",
            "cpu_mfc1": lambda: setr(rt, FU(fs)),
            "cpu_mtc1": lambda: f"{FU(fs)} = {R(rt)};",
            "cpu_cfc1": lambda: setr(rt, f"rt_cfc1(c, {fs})"),
            "cpu_ctc1": lambda: f"rt_ctc1(c, {fs}, {R(rt)});",
        }
        if n not in T:
            raise Unsupported(f"{n} at {a:#x}")
        return T[n]()

    # -- branches ----------------------------------------------------------------------------------------------
    def branch_cond(self, mod, a):
        i = mod.ins(a)
        n = i.uniqueId.name
        w = i.getRaw()
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        conds = {
            "cpu_beq": f"{R(rs)} == {R(rt)}", "cpu_beql": f"{R(rs)} == {R(rt)}",
            "cpu_bne": f"{R(rs)} != {R(rt)}", "cpu_bnel": f"{R(rs)} != {R(rt)}",
            "cpu_blez": f"{S(rs)} <= 0", "cpu_blezl": f"{S(rs)} <= 0",
            "cpu_bgtz": f"{S(rs)} > 0", "cpu_bgtzl": f"{S(rs)} > 0",
            "cpu_bltz": f"{S(rs)} < 0", "cpu_bltzl": f"{S(rs)} < 0",
            "cpu_bgez": f"{S(rs)} >= 0", "cpu_bgezl": f"{S(rs)} >= 0",
            "cpu_bc1t": "c->fpcond", "cpu_bc1tl": "c->fpcond",
            "cpu_bc1f": "!c->fpcond", "cpu_bc1fl": "!c->fpcond",
            "r4000allegrex_bvt": f"(c->vfpu_ctrl[VFPU_CTRL_CC] >> {(w >> 18) & 7}) & 1",
            "r4000allegrex_bvtl": f"(c->vfpu_ctrl[VFPU_CTRL_CC] >> {(w >> 18) & 7}) & 1",
            "r4000allegrex_bvf": f"!((c->vfpu_ctrl[VFPU_CTRL_CC] >> {(w >> 18) & 7}) & 1)",
            "r4000allegrex_bvfl": f"!((c->vfpu_ctrl[VFPU_CTRL_CC] >> {(w >> 18) & 7}) & 1)",
        }
        if n not in conds:
            raise Unsupported(f"branch {n} at {a:#x}")
        return conds[n]

    def function(self, mod, f):
        """C for one function."""
        start, end = f["start"], f["end"]
        tables = {jr: targets for jr, _t, targets in f.get("tables", [])}
        unresolved = set(f.get("unresolved", [])) | {jr for jr, _t, _ts in f.get("fptables", [])}
        starts = self.starts[mod.name]

        def inside(t):
            return start <= t < end and (t == start or t not in starts)

        def goto(t):
            return f"goto L_{t:08X};"

        # labels: everything a branch, intra-function j or jump table reaches
        labels = set()
        for a in range(start, end, 4):
            i = mod.ins(a)
            if i.isBranch() and not i.doesLink():
                t = i.getBranchVramGeneric()
                if inside(t):
                    labels.add(t)
            elif i.isJumpWithAddress() and not i.doesLink():
                t = i.getInstrIndexAsVram()
                if inside(t):
                    labels.add(t)
            elif a in tables:
                labels.update(t for t in tables[a] if inside(t))

        # a gated function (patches.py's GATES): its body behind rt_gate, which runs it on the frames a 30th falls on
        gated = (mod.name, start) in GATES
        if gated:
            self.gated += 1
            if GATES[(mod.name, start)][1] != "on":
                raise SystemExit(f"gate at {mod.name} {start:#x}: a function's gate is \"on\" (the others are calls')")
        out = [f"static void {self.fname(mod, start)}_gated(Ctx *c) {{" if gated else
               f"void {self.fname(mod, start)}(Ctx *c) {{"]
        emit = out.append
        if TRACE_ALL:
            emit(f"    rt_trace_all({list(self.by_name).index(mod.name)}, {hx(start)}, c->r[31]);")
        if start in TRACE:                  # RECOMP_TRACE=addr,...: the function's calls logged (rt_trace_call)
            emit(f"    rt_trace_call(c, {hx(start)});")
        a = start
        while a < end:
            if a in labels:
                emit(f"L_{a:08X}:;")
            i = mod.ins(a)
            delay_pc = a + 4
            if i.hasDelaySlot():
                delay = self.op(mod, delay_pc) if delay_pc < end else ""
                if delay_pc < end and mod.ins(delay_pc).hasDelaySlot():
                    raise Unsupported(f"control transfer in a delay slot at {delay_pc:#x}")
                body = self.transfer(mod, a, delay, inside, goto, tables, unresolved)
                emit(f"    /* {a:08X} */ {body}")
                if delay_pc in labels:      # something branches to the delay slot itself
                    emit(f"    goto L_{a + 8:08X};")
                    emit(f"L_{delay_pc:08X}:;")
                    emit(f"    /* {delay_pc:08X} */ {delay}")
                    labels.add(a + 8)
                a += 8
                continue
            code = self.op(mod, a)
            if code:
                emit(f"    /* {a:08X} */ {code}")
            a += 4
        if end in labels:
            emit(f"L_{end:08X}:;")
        # falling off the end: into the next function (a walk that ran into another start), or nowhere
        if end in starts:
            emit(f"    {self.call(mod, end)} return;")
        else:
            emit(f"    rt_unreachable(c, {hx(end)});")
        emit("}")
        if gated:
            emit(f"void {self.fname(mod, start)}(Ctx *c) {{ rt_gate(c, {self.fname(mod, start)}_gated, 0, {hx(start)}); }}"
                 " /* gated (recomp/patches.py) */")
        return "\n".join(out)

    def transfer(self, mod, a, delay, inside, goto, tables, unresolved):
        i = mod.ins(a)
        w = i.getRaw()
        rs, rd = (w >> 21) & 31, (w >> 11) & 31

        def tail(t):
            return f"{{ {self.call(mod, t)} return; }}"

        if i.isBranch():
            if i.doesLink():
                raise Unsupported(f"branch-and-link at {a:#x}")
            t = i.getBranchVramGeneric()
            dest = goto(t) if inside(t) else tail(t)
            if i.isUnconditionalBranch():
                return f"{delay} {dest}"
            cond = self.branch_cond(mod, a)
            if i.isBranchLikely():
                return f"if ({cond}) {{ {delay} {dest} }}"
            return f"{{ int t = {cond}; {delay} if (t) {dest} }}"
        gate = (mod.name, a) in GATES   # a gated call (patches.py's GATES): rt_gate runs it as at 30 a second
        if gate:
            self.gated += 1
        if i.isJumpWithAddress():
            t = i.getInstrIndexAsVram()
            if i.doesLink():
                if gate:
                    fn = self.call(mod, t)
                    if not fn.startswith("f_"):
                        raise SystemExit(f"gate at {mod.name} {a:#x}: a call to {t:#x}, not to a function")
                    gfn = GATE_FUNCTIONS[GATES[(mod.name, a)][1]]
                    return f"c->r[31] = {hx(a + 8)}; {delay} {gfn}(c, {fn[:-len('(c);')]}, 0, {hx(a)}); /* gated */"
                return f"c->r[31] = {hx(a + 8)}; {delay} {self.call(mod, t)}"
            return f"{delay} " + (goto(t) if inside(t) else tail(t))
        n = i.uniqueId.name
        if n == "cpu_jalr":
            return (f"{{ uint32_t t = {R(rs)}; " + ("" if rd == 0 else f"{R(rd)} = {hx(a + 8)}; ") +
                    (f"{delay} {GATE_FUNCTIONS[GATES[(mod.name, a)][1]]}(c, 0, t, {hx(a)}); }} /* gated */" if gate else f"{delay} rt_call(c, t); }}"))
        if gate:
            raise SystemExit(f"gate at {mod.name} {a:#x}: not a call")
        if n == "cpu_jr":
            if rs == 31:
                return f"{delay} return;"
            if a in tables:
                cases = " ".join(f"case {hx(t)}: {goto(t) if inside(t) else tail(t)}"
                                 for t in sorted(set(tables[a])))
                return (f"{{ uint32_t t = {R(rs)}; {delay} switch (t) {{ {cases} "
                        f"default: rt_bad_jump(c, t, {hx(a)}); return; }} }}")
            return f"{{ uint32_t t = {R(rs)}; {delay} rt_call(c, t); return; }}"
        raise Unsupported(f"{n} at {a:#x}")


def load_names():
    """tools/readable/names/*.toml: {(module, address): (name, purpose, confidence)}, a stronger entry over a weaker one."""
    rank = {"confirmed": 2, "inferred": 1, "guess": 0}
    names = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "tools/readable/names/*.toml"))):
        with open(path, "rb") as f:
            for e in tomllib.load(f).get("function", []):
                key = (e["module"], int(e["address"], 16))
                known = os.path.basename(path) == "known.toml"
                if key not in names or known or rank[e["confidence"]] > rank[names[key][2]]:
                    names[key] = (e["name"], " ".join(e.get("purpose", "").split()).replace("*/", "* /"), e["confidence"])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--functions", default=os.path.join(ROOT, "game/analysis/functions.json"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build/gen"))
    ap.add_argument("--max-lines", type=int, default=60000, help="split a module's C files at about this many lines")
    ap.add_argument("modules", nargs="*", help="only these modules (default: all)")
    args = ap.parse_args()

    mods = modules.load()
    with open(args.functions) as f:
        result = json.load(f)
    tr = Translator(mods, {n: r["functions"] for n, r in result.items()})
    os.makedirs(args.out, exist_ok=True)
    errors = 0
    protos = []
    files = 0
    names = load_names()
    for m in mods:
        if args.modules and m.name not in args.modules:
            continue
        chunks, lines = [[]], 0
        for f in tr.functions[m.name]:
            protos.append(f"void {tr.fname(m, f['start'])}(Ctx *c);")
            try:
                code = tr.function(m, f)
            except Unsupported as e:
                errors += 1
                print(f"{m.name} {f['start']:#x}: {e}", file=sys.stderr)
                code = f"void {tr.fname(m, f['start'])}(Ctx *c) {{ rt_unreachable(c, {hx(f['start'])}); }}"
            n = code.count("\n") + 1
            named = names.get((m.name, f["start"]))
            if named:                       # comments only: what is compiled does not change (nor where the files split)
                code = f"/* {named[0]} [{named[2]}]: {named[1]} */\n" + code
            if lines + n > args.max_lines and chunks[-1]:
                chunks.append([])
                lines = 0
            chunks[-1].append(code)
            lines += n
        for k, chunk in enumerate(chunks):
            name = m.name if len(chunks) == 1 else f"{m.name}_{k}"
            with open(os.path.join(args.out, name + ".c"), "w") as out:
                out.write(f"/* Generated by recomp/recomp.py from {m.name}: do not edit. */\n")
                out.write('#include "recomp.h"\n#include "funcs.h"\n\n')
                out.write("\n\n".join(chunk) + "\n")
            files += 1
    with open(os.path.join(args.out, "names.h"), "w") as out:
        # For hand-written code: a function by its name.
        out.write("/* Generated by recomp/recomp.py from tools/readable/names/: do not edit. */\n#pragma once\n#include \"funcs.h\"\n\n")
        counts = collections.Counter(n[0] for n in names.values())
        have = set(tr.functions)
        for (mod_name, addr), (name, _p, _c) in sorted(names.items()):
            if mod_name in have and any(f["start"] == addr for f in tr.functions[mod_name]):
                unique = name if counts[name] == 1 else f"{name}_{addr:08X}"     # (as gen_types.py has it)
                out.write(f"#define {unique} f_{mod_name}_{addr:08X}\n")
    with open(os.path.join(args.out, "funcs.h"), "w") as out:
        out.write("/* Generated by recomp/recomp.py: do not edit. */\n#pragma once\n#include \"recomp.h\"\n\n")
        out.write("\n".join(protos) + "\n\n")
        if TRACE_ALL:
            out.write("void rt_trace_all(int module, uint32_t fn, uint32_t ra);\n\n")
        out.write("".join(f"void vfpu_{fn}(Ctx *c, uint32_t instr);\n" for fn in sorted(tr.vfpu_used)))
    with open(os.path.join(args.out, "tables.c"), "w") as out:
        out.write("/* Generated by recomp/recomp.py: do not edit. */\n#include \"funcs.h\"\n\n")
        entries = []
        for m in mods:
            if args.modules and m.name not in args.modules:
                continue
            fs = tr.functions[m.name]
            out.write(f"static const RecompEntry funcs_{m.name}[] = {{\n")
            out.write("".join(f"    {{{hx(f['start'])}, {tr.fname(m, f['start'])}}},\n" for f in fs))
            out.write("};\n")
            entries.append(f"    {{\"{m.name}\", {m.region}, {m.number}, {hx(m.base)}, funcs_{m.name}, {len(fs)}}},")
        out.write("\nconst RecompModule recomp_modules[] = {\n" + "\n".join(entries) + "\n};\n")
        out.write(f"const int recomp_nmodules = {len(entries)};\n\n")
        imps = sorted(mods[0].imports.items())
        out.write("const RecompImport recomp_imports[] = {\n")
        out.write("".join(f"    {{{hx(a)}, \"{lib}\", {hx(nid)}}},\n" for a, (lib, nid) in imps))
        out.write(f"}};\nconst int recomp_nimports = {len(imps)};\n")
    print(f"{files} C files, {len(protos)} functions, {errors} not translated, {tr.patched} patched, {tr.counted} counts in 30ths, "
          f"{tr.placed} places' steps in game time, {tr.gated} gated",
          file=sys.stderr)
    if not args.modules and tr.patched != len(PATCHES):
        raise SystemExit(f"{len(PATCHES) - tr.patched} patches in recomp/patches.py were not applied")
    if not args.modules and tr.gated != len(GATES):
        raise SystemExit(f"{len(GATES) - tr.gated} gates in recomp/patches.py are neither a function's start nor a call")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
