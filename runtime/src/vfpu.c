/* The VFPU: 128 single-precision registers seen as 8 4x4 matrices, read and written as singles, pairs, triples, quads
 * (rows or columns) or matrices, with prefixes that swizzle, negate and replace an instruction's source lanes and
 * saturate and mask its destination lanes. The recompiled code calls one function per instruction, with the raw
 * instruction word (the recompiler chose the function by decoding it).
 *
 * Semantics follow PPSSPP's interpreter (Core/MIPS/MIPSIntVFPU.cpp); tools/difftest.py checks them against it.
 * Registers are stored by register number: c->v[mtx * 4 + col + row * 32], as PPSSPP's debugger lists them. */
#include "runtime.h"

#include <math.h>

enum { PFX_S = 0, PFX_T = 1, PFX_D = 2 };

#define VD(op) ((op) & 0x7F)
#define VS(op) (((op) >> 8) & 0x7F)
#define VT(op) (((op) >> 16) & 0x7F)

/* 1 single, 2 pair, 3 triple, 4 quad */
static int vsize(uint32_t op) {
    return (((op >> 7) & 1) | ((op >> 14) & 2)) + 1;
}

static void vector_regs(int regs[4], int n, int reg) {
    int mtx = (reg >> 2) & 7, col = reg & 3, row = 0, transpose = (reg >> 5) & 1;
    switch (n) {
    case 1: transpose = 0; row = (reg >> 5) & 3; break;
    case 2: row = (reg >> 5) & 2; break;
    case 3: row = (reg >> 6) & 1; break;
    case 4: row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++)
        regs[i] = mtx * 4 + (transpose ? ((row + i) & 3) + col * 32 : col + ((row + i) & 3) * 32);
}

static void matrix_regs(int regs[16], int n, int reg) {
    int mtx = (reg >> 2) & 7, col = reg & 3, row = 0, transpose = (reg >> 5) & 1;
    switch (n) {
    case 1: transpose = 0; row = (reg >> 5) & 3; break;
    case 2: row = (reg >> 5) & 2; break;
    case 3: row = (reg >> 6) & 1; break;
    case 4: row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            regs[j * 4 + i] = mtx * 4 + (transpose ? ((row + i) & 3) + ((col + j) & 3) * 32
                                                   : ((col + j) & 3) + ((row + i) & 3) * 32);
}

static void read_vec(Ctx *c, float *v, int n, int reg) {
    int r[4];
    vector_regs(r, n, reg);
    for (int i = 0; i < 4; i++)
        v[i] = i < n ? c->v[r[i]] : 0.0f;
}

static void read_mtx(Ctx *c, float *m, int n, int reg) {
    int r[16];
    matrix_regs(r, n, reg);
    for (int i = 0; i < 16; i++)
        m[i] = 0.0f;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            m[j * 4 + i] = c->v[r[j * 4 + i]];
}

static void write_mtx(Ctx *c, const float *m, int n, int reg) {
    int r[16];
    matrix_regs(r, n, reg);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            c->v[r[j * 4 + i]] = m[j * 4 + i];
}

static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* S and T prefixes: each lane takes a source lane (or a constant), optionally its absolute value, optionally negated */
static void apply_prefix(Ctx *c, float *v, int n, int which) {
    static const float constants[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};
    uint32_t data = c->vfpu_ctrl[which];
    if (data == 0xE4)
        return;
    float orig[4] = {v[0], v[1], v[2], v[3]};
    for (int i = 0; i < n; i++) {
        int regnum = (data >> (i * 2)) & 3, abs = (data >> (8 + i)) & 1;
        int constant = (data >> (12 + i)) & 1, negate = (data >> (16 + i)) & 1;
        if (!constant) {
            v[i] = orig[regnum];
            if (abs)
                v[i] = fbits(ubits(v[i]) & 0x7FFFFFFFu);
        } else {
            v[i] = constants[regnum + (abs << 2)];
        }
        if (negate)
            v[i] = fbits(ubits(v[i]) ^ 0x80000000u);
    }
}

/* The D prefix: saturate each lane (to [0, 1] or [-1, 1]), and write only the lanes not masked */
static void write_vec(Ctx *c, float *v, int n, int reg) {
    uint32_t data = c->vfpu_ctrl[PFX_D];
    int r[4];
    vector_regs(r, n, reg);
    for (int i = 0; i < n; i++) {
        int sat = (data >> (i * 2)) & 3;
        if (sat == 1)
            v[i] = v[i] != v[i] ? 0.0f : v[i] <= 0.0f ? 0.0f : v[i] > 1.0f ? 1.0f : v[i];
        else if (sat == 3)
            v[i] = v[i] != v[i] ? v[i] : v[i] < -1.0f ? -1.0f : v[i] > 1.0f ? 1.0f : v[i];
        if (!((data >> (8 + i)) & 1))
            c->v[r[i]] = v[i];
    }
}

static void eat_prefixes(Ctx *c) {
    c->vfpu_ctrl[PFX_S] = 0xE4;
    c->vfpu_ctrl[PFX_T] = 0xE4;
    c->vfpu_ctrl[PFX_D] = 0;
}

/* sin / cos / asin of angles in quarter turns (x = 1 is 90 degrees), as the VFPU takes them */
static float vsin(float x) {
    double r = fmod((double)x, 4.0);
    if (r == floor(r)) {                        /* exact at the quadrants: 0, 1, 0, -1 */
        int k = (((int)r % 4) + 4) % 4;
        return k == 1 ? 1.0f : k == 3 ? -1.0f : 0.0f;
    }
    return (float)sin(r * (M_PI / 2));
}

static float vcos(float x) {
    return vsin(x + 1.0f);
}

/* -- loads and stores ------------------------------------------------------------------------------------------- */

void vfpu_lv_s(Ctx *c, uint32_t op) {
    int vt = ((op >> 16) & 0x1F) | ((op & 3) << 5);
    uint32_t addr = c->r[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
    int r[4];
    vector_regs(r, 1, vt);
    c->v[r[0]] = fbits(LW(addr));
}

void vfpu_sv_s(Ctx *c, uint32_t op) {
    int vt = ((op >> 16) & 0x1F) | ((op & 3) << 5);
    uint32_t addr = c->r[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
    int r[4];
    vector_regs(r, 1, vt);
    SW(addr, ubits(c->v[r[0]]));
}

void vfpu_lv_q(Ctx *c, uint32_t op) {
    int vt = ((op >> 16) & 0x1F) | ((op & 1) << 5);
    uint32_t addr = c->r[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
    int r[4];
    vector_regs(r, 4, vt);
    for (int i = 0; i < 4; i++)
        c->v[r[i]] = fbits(LW(addr + 4 * i));
}

void vfpu_sv_q(Ctx *c, uint32_t op) {
    int vt = ((op >> 16) & 0x1F) | ((op & 1) << 5);
    uint32_t addr = c->r[(op >> 21) & 31] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
    int r[4];
    vector_regs(r, 4, vt);
    for (int i = 0; i < 4; i++)
        SW(addr + 4 * i, ubits(c->v[r[i]]));
}

void vfpu_mtv(Ctx *c, uint32_t op) {
    int vd = op & 0xFF, rt = (op >> 16) & 31;
    if (vd < 128) {
        int r[4];
        vector_regs(r, 1, vd);
        c->v[r[0]] = fbits(c->r[rt]);
    } else if (vd - 128 < 16) {
        c->vfpu_ctrl[vd - 128] = c->r[rt];
    }
}

void vfpu_mfv(Ctx *c, uint32_t op) {
    int vd = op & 0xFF, rt = (op >> 16) & 31;
    uint32_t v = 0;
    if (vd < 128) {
        int r[4];
        vector_regs(r, 1, vd);
        v = ubits(c->v[r[0]]);
    } else if (vd - 128 < 16) {
        v = c->vfpu_ctrl[vd - 128];
    }
    if (rt)
        c->r[rt] = v;
}

/* -- prefixes ----------------------------------------------------------------------------------------------------- */

void vfpu_vpfxs(Ctx *c, uint32_t op) { c->vfpu_ctrl[PFX_S] = op & 0xFFFFF; }
void vfpu_vpfxt(Ctx *c, uint32_t op) { c->vfpu_ctrl[PFX_T] = op & 0xFFFFF; }
void vfpu_vpfxd(Ctx *c, uint32_t op) { c->vfpu_ctrl[PFX_D] = op & 0xFFF; }

/* -- element-wise ------------------------------------------------------------------------------------------------- */

typedef float (*Binary)(float, float);
static float op_add(float a, float b) { return a + b; }
static float op_sub(float a, float b) { return a - b; }
static float op_mul(float a, float b) { return a * b; }
static float op_div(float a, float b) { return a / b; }
static float op_min(float a, float b) { return a < b ? a : b; }
static float op_max(float a, float b) { return a > b ? a : b; }

static void binary(Ctx *c, uint32_t op, Binary f) {
    int n = vsize(op);
    float s[4], t[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    read_vec(c, t, n, VT(op));
    apply_prefix(c, t, n, PFX_T);
    for (int i = 0; i < n; i++)
        d[i] = f(s[i], t[i]);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vadd(Ctx *c, uint32_t op) { binary(c, op, op_add); }
void vfpu_vsub(Ctx *c, uint32_t op) { binary(c, op, op_sub); }
void vfpu_vmul(Ctx *c, uint32_t op) { binary(c, op, op_mul); }
void vfpu_vdiv(Ctx *c, uint32_t op) { binary(c, op, op_div); }
void vfpu_vmin(Ctx *c, uint32_t op) { binary(c, op, op_min); }
void vfpu_vmax(Ctx *c, uint32_t op) { binary(c, op, op_max); }

typedef float (*Unary)(float);
static float u_mov(float a) { return a; }
static float u_abs(float a) { return fbits(ubits(a) & 0x7FFFFFFFu); }
static float u_neg(float a) { return fbits(ubits(a) ^ 0x80000000u); }
static float u_rcp(float a) { return 1.0f / a; }
static float u_rsq(float a) { return 1.0f / sqrtf(a); }
static float u_sqrt(float a) { return sqrtf(a); }
static float u_sin(float a) { return vsin(a); }
static float u_cos(float a) { return vcos(a); }
static float u_exp2(float a) { return exp2f(a); }
static float u_log2(float a) { return log2f(a); }
static float u_asin(float a) { return (float)(asin((double)a) / (M_PI / 2)); }
static float u_ocp(float a) { return 1.0f - a; }
static float u_sgn(float a) { return a > 0.0f ? 1.0f : a < 0.0f ? -1.0f : 0.0f; }

static void unary(Ctx *c, uint32_t op, Unary f) {
    int n = vsize(op);
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    for (int i = 0; i < n; i++)
        d[i] = f(s[i]);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vmov(Ctx *c, uint32_t op) { unary(c, op, u_mov); }
void vfpu_vabs(Ctx *c, uint32_t op) { unary(c, op, u_abs); }
void vfpu_vneg(Ctx *c, uint32_t op) { unary(c, op, u_neg); }
void vfpu_vrcp(Ctx *c, uint32_t op) { unary(c, op, u_rcp); }
void vfpu_vrsq(Ctx *c, uint32_t op) { unary(c, op, u_rsq); }
void vfpu_vsqrt(Ctx *c, uint32_t op) { unary(c, op, u_sqrt); }
void vfpu_vsin(Ctx *c, uint32_t op) { unary(c, op, u_sin); }
void vfpu_vcos(Ctx *c, uint32_t op) { unary(c, op, u_cos); }
void vfpu_vexp2(Ctx *c, uint32_t op) { unary(c, op, u_exp2); }
void vfpu_vlog2(Ctx *c, uint32_t op) { unary(c, op, u_log2); }
void vfpu_vasin(Ctx *c, uint32_t op) { unary(c, op, u_asin); }
void vfpu_vocp(Ctx *c, uint32_t op) { unary(c, op, u_ocp); }
void vfpu_vsgn(Ctx *c, uint32_t op) { unary(c, op, u_sgn); }

static void fill(Ctx *c, uint32_t op, float value) {
    int n = vsize(op);
    float d[4] = {value, value, value, value};
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vzero(Ctx *c, uint32_t op) { fill(c, op, 0.0f); }
void vfpu_vone(Ctx *c, uint32_t op) { fill(c, op, 1.0f); }

void vfpu_vcst(Ctx *c, uint32_t op) {
    static const float table[32] = {
        0.0f, 3.402823466e+38f, 1.41421356237f, 0.70710678118f, 1.12837916709f, 0.63661977236f,
        0.31830988618f, 0.78539816339f, 1.57079632679f, 3.14159265359f, 2.71828182846f, 1.44269504088f,
        0.43429448190f, 0.69314718056f, 2.30258509299f, 6.28318530718f, 0.52359877559f, 0.30102999566f,
        3.32192809489f, 0.86602540378f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    fill(c, op, table[(op >> 16) & 0x1F]);
}

void vfpu_vidt(Ctx *c, uint32_t op) {
    int n = vsize(op), vd = VD(op);
    float d[4] = {0, 0, 0, 0};
    d[vd & (n == 2 ? 1 : 3)] = 1.0f;
    write_vec(c, d, n, vd);
    eat_prefixes(c);
}

void vfpu_vscl(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float s[4], t[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    read_vec(c, t, 1, VT(op));
    apply_prefix(c, t, n, PFX_T);
    for (int i = 0; i < n; i++)
        d[i] = s[i] * t[0];
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vdot(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float s[4], t[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    read_vec(c, t, n, VT(op));
    apply_prefix(c, t, n, PFX_T);
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += s[i] * t[i];
    d[0] = sum;
    write_vec(c, d, 1, VD(op));
    eat_prefixes(c);
}

void vfpu_vfad(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += s[i];
    d[0] = sum;
    write_vec(c, d, 1, VD(op));
    eat_prefixes(c);
}

void vfpu_vcrsp(Ctx *c, uint32_t op) {      /* vcrsp.t: the cross product */
    float s[4], t[4], d[4];
    read_vec(c, s, 3, VS(op));
    read_vec(c, t, 3, VT(op));
    d[0] = s[1] * t[2] - s[2] * t[1];
    d[1] = s[2] * t[0] - s[0] * t[2];
    d[2] = s[0] * t[1] - s[1] * t[0];
    write_vec(c, d, 3, VD(op));
    eat_prefixes(c);
}

void vfpu_vsocp(Ctx *c, uint32_t op) {      /* vsocp.s: (1 - s, s), each clamped to [0, 1], as a pair */
    float s[4], d[4];
    read_vec(c, s, 1, VS(op));
    apply_prefix(c, s, 1, PFX_S);
    float a = 1.0f - s[0], b = s[0];
    d[0] = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    d[1] = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    write_vec(c, d, 2, VD(op));
    eat_prefixes(c);
}

void vfpu_vfim(Ctx *c, uint32_t op) {       /* vfim.s: a half-float immediate */
    uint32_t h = op & 0xFFFF, sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float f;
    if (exp == 0x1F)
        f = fbits((sign << 31) | 0x7F800000u | (man << 13));
    else if (exp == 0)
        f = (sign ? -1.0f : 1.0f) * ldexpf((float)man, -24);
    else
        f = fbits((sign << 31) | ((exp + 112) << 23) | (man << 13));
    float d[4] = {f};
    write_vec(c, d, 1, VT(op));
    eat_prefixes(c);
}

void vfpu_vi2f(Ctx *c, uint32_t op) {       /* integers (in the registers' bits) to floats, divided by 2^imm */
    int n = vsize(op), scale = (op >> 16) & 0x1F;
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    for (int i = 0; i < n; i++)
        d[i] = ldexpf((float)(int32_t)ubits(s[i]), -scale);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

static void f2i(Ctx *c, uint32_t op, double (*round)(double)) {
    int n = vsize(op), scale = (op >> 16) & 0x1F;
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    for (int i = 0; i < n; i++) {
        double v = ldexp((double)s[i], scale);
        int32_t r;
        if (v != v)
            r = 0x7FFFFFFF;
        else if (v >= 2147483647.0)
            r = 0x7FFFFFFF;
        else if (v <= -2147483648.0)
            r = (int32_t)0x80000000u;
        else
            r = (int32_t)round(v);
        d[i] = fbits((uint32_t)r);
    }
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vf2id(Ctx *c, uint32_t op) { f2i(c, op, floor); }
void vfpu_vf2iu(Ctx *c, uint32_t op) { f2i(c, op, ceil); }
void vfpu_vf2iz(Ctx *c, uint32_t op) { f2i(c, op, trunc); }
void vfpu_vf2in(Ctx *c, uint32_t op) { f2i(c, op, rint); }

/* -- comparisons -------------------------------------------------------------------------------------------------- */

void vfpu_vcmp(Ctx *c, uint32_t op) {
    int n = vsize(op), cond = op & 15;
    float s[4], t[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    read_vec(c, t, n, VT(op));
    apply_prefix(c, t, n, PFX_T);
    uint32_t cc = 0, any = 0, all = 1, affected = (1u << 4) | (1u << 5);
    for (int i = 0; i < n; i++) {
        float a = s[i], b = t[i];
        int r = 0;
        switch (cond) {
        case 0: r = 0; break;
        case 1: r = a == b; break;
        case 2: r = a < b; break;
        case 3: r = a <= b; break;
        case 4: r = 1; break;
        case 5: r = a != b; break;
        case 6: r = a >= b; break;
        case 7: r = a > b; break;
        case 8: r = a == 0.0f; break;
        case 9: r = a != a; break;
        case 10: r = isinf(a); break;
        case 11: r = a != a || isinf(a); break;
        case 12: r = a != 0.0f; break;
        case 13: r = a == a; break;
        case 14: r = !isinf(a); break;
        case 15: r = !(a != a || isinf(a)); break;
        }
        cc |= (uint32_t)r << i;
        any |= (uint32_t)r;
        all &= (uint32_t)r;
        affected |= 1u << i;
    }
    c->vfpu_ctrl[VFPU_CTRL_CC] = (c->vfpu_ctrl[VFPU_CTRL_CC] & ~affected) | ((cc | (any << 4) | (all << 5)) & affected);
    eat_prefixes(c);
}

static void cmov(Ctx *c, uint32_t op, int when) {
    int n = vsize(op), imm3 = (op >> 16) & 7;
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    apply_prefix(c, s, n, PFX_S);
    read_vec(c, d, n, VD(op));
    uint32_t cc = c->vfpu_ctrl[VFPU_CTRL_CC];
    if (imm3 < 6) {
        if ((int)((cc >> imm3) & 1) == when)
            for (int i = 0; i < n; i++)
                d[i] = s[i];
    } else if (imm3 == 6) {
        for (int i = 0; i < n; i++)
            if ((int)((cc >> i) & 1) == when)
                d[i] = s[i];
    }
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vcmovt(Ctx *c, uint32_t op) { cmov(c, op, 1); }
void vfpu_vcmovf(Ctx *c, uint32_t op) { cmov(c, op, 0); }

/* -- rotation and matrices ---------------------------------------------------------------------------------------- */

void vfpu_vrot(Ctx *c, uint32_t op) {
    int n = vsize(op), imm = (op >> 16) & 0x1F;
    float s[4], d[4];
    read_vec(c, s, 1, VS(op));
    float sine = vsin(s[0]), cosine = vcos(s[0]);
    if (imm & 0x10)
        sine = -sine;
    char what[4] = {'0', '0', '0', '0'};
    if (((imm >> 2) & 3) == (imm & 3))
        for (int i = 0; i < 4; i++)
            what[i] = 'S';
    what[(imm >> 2) & 3] = 'S';
    what[imm & 3] = 'C';
    for (int i = 0; i < n; i++)
        d[i] = what[i] == 'S' ? sine : what[i] == 'C' ? cosine : 0.0f;
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vmidt(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float m[16] = {0};
    for (int i = 0; i < n; i++)
        m[i * 4 + i] = 1.0f;
    write_mtx(c, m, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vmmov(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float m[16];
    read_mtx(c, m, n, VS(op));
    write_mtx(c, m, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vmmul(Ctx *c, uint32_t op) {
    int n = vsize(op);
    float s[16], t[16], d[16] = {0};
    read_mtx(c, s, n, VS(op));
    read_mtx(c, t, n, VT(op));
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++) {
            float sum = 0.0f;
            for (int k = 0; k < n; k++)
                sum += s[b * 4 + k] * t[a * 4 + k];
            d[a * 4 + b] = sum;
        }
    write_mtx(c, d, n, VD(op));
    eat_prefixes(c);
}

static void tfm(Ctx *c, uint32_t op, int n) {    /* matrix (vs) times vector (vt) */
    float s[16], t[4], d[4];
    read_mtx(c, s, n, VS(op));
    read_vec(c, t, n, VT(op));
    for (int i = 0; i < n; i++) {
        float sum = 0.0f;
        for (int k = 0; k < n; k++)
            sum += s[k * 4 + i] * t[k];
        d[i] = sum;
    }
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void vfpu_vtfm2(Ctx *c, uint32_t op) { tfm(c, op, 2); }
void vfpu_vtfm3(Ctx *c, uint32_t op) { tfm(c, op, 3); }
void vfpu_vtfm4(Ctx *c, uint32_t op) { tfm(c, op, 4); }

/* Anything not above: the recompiler emits vfpu_exec for it */
void vfpu_exec(Ctx *c, uint32_t instr, uint32_t at) {
    rt_fatal(c, "VFPU instruction %08X at %08X: not implemented", instr, at);
}
