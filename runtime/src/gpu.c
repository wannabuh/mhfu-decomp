/* A software implementation of the GE's drawing, into emulated VRAM: the reference for checking frames against PPSSPP,
 * until the Vulkan renderer. The vertex stage (every vertex format; skinning; the world, view and projection matrices;
 * the viewport and screen offset; through mode), near-plane clipping, triangles (lists, strips, fans) and sprites,
 * perspective-correct texturing (the 16/32-bit, CLUT and swizzled formats; nearest texel), Gouraud or flat colour,
 * the texture functions, alpha and depth tests, back-face culling, blending, clear mode, and block transfers.
 * Lighting (four lights: directional, point, spot; ambient, diffuse, powered diffuse, specular), bilinear filtering.
 * Not yet: fog, mipmaps, texture coordinate generation, the DXT formats, lines and points, stencil, logic ops,
 * dithering. */
#include "hle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t reg[256];          /* the last value of each command */
static uint32_t vaddr, iaddr;

static unsigned long unhandled_prims[8];

/* -- framebuffer -------------------------------------------------------------------------------------------------- */

static uint32_t fb_addr(void) { return 0x04000000u | ((reg[0x9C] & 0xFFFFF0) | ((reg[0x9D] & 0xFF0000) << 8)) % 0x200000u; }
static uint32_t fb_stride(void) { return reg[0x9D] & 0x7FC; }
static int fb_format(void) { return reg[0xD2] & 3; }      /* 0 5650, 1 5551, 2 4444, 3 8888 */

static uint32_t to_rgba(uint32_t c, int fmt) {       /* a pixel in one of the 16-bit formats (or 8888) to ABGR8888 */
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0: r = c & 31; g = (c >> 5) & 63; b = (c >> 11) & 31;
        return 0xFF000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
    case 1: r = c & 31; g = (c >> 5) & 31; b = (c >> 10) & 31; a = (c >> 15) ? 255 : 0;
        return (a << 24) | ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2);
    case 2: r = c & 15; g = (c >> 4) & 15; b = (c >> 8) & 15; a = (c >> 12) & 15;
        return (a * 17u << 24) | (b * 17u << 16) | (g * 17u << 8) | r * 17u;
    default: return c;
    }
}

static uint32_t from_rgba(uint32_t c, int fmt) {
    uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
    switch (fmt) {
    case 0: return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case 1: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    case 2: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
    default: return c;
    }
}

static uint32_t fb_read(int x, int y) {
    int fmt = fb_format();
    uint32_t a = fb_addr() + (uint32_t)(y * (int)fb_stride() + x) * (fmt == 3 ? 4 : 2);
    return fmt == 3 ? LW(a) : to_rgba(LHU(a), fmt);
}

static void fb_write(int x, int y, uint32_t c) {
    int fmt = fb_format();
    uint32_t a = fb_addr() + (uint32_t)(y * (int)fb_stride() + x) * (fmt == 3 ? 4 : 2);
    if (fmt == 3)
        SW(a, c);
    else
        SH(a, from_rgba(c, fmt));
}

/* -- textures ----------------------------------------------------------------------------------------------------- */

static uint32_t clut[256];

static void clut_load(void) {
    uint32_t addr = (reg[0xB0] & 0xFFFFF0) | ((reg[0xB1] & 0x0F0000) << 8);
    uint32_t bytes = (reg[0xC4] & 0x3F) * 32;
    int fmt = reg[0xC5] & 3;
    uint32_t n = fmt == 3 ? bytes / 4 : bytes / 2;
    for (uint32_t i = 0; i < n && i < 256; i++)
        clut[i] = fmt == 3 ? LW(addr + 4 * i) : to_rgba(LHU(addr + 2 * i), fmt);
}

static uint32_t clut_lookup(uint32_t index) {
    uint32_t mode = reg[0xC5];
    uint32_t shift = (mode >> 2) & 31, mask = (mode >> 8) & 0xFF, start = ((mode >> 16) & 31) * 16;
    return clut[(((index >> shift) & mask) | start) & 255];
}

/* The texel at (u, v) of level 0 */
static uint32_t texel(int u, int v) {
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    int wrap_u = reg[0xC7] & 1, wrap_v = (reg[0xC7] >> 8) & 1;     /* 1: clamp */
    u = wrap_u ? (u < 0 ? 0 : u >= w ? w - 1 : u) : (u & (w - 1));
    v = wrap_v ? (v < 0 ? 0 : v >= h ? h - 1 : v) : (v & (h - 1));
    uint32_t base = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    uint32_t bw = reg[0xA8] & 0x7FF;
    int fmt = reg[0xC3] & 0xF;
    int bits = fmt == 3 || fmt == 7 ? 32 : fmt == 4 ? 4 : fmt == 5 ? 8 : 16;
    uint32_t bytes_per_row = bw * (uint32_t)bits / 8;
    uint32_t off;
    if (reg[0xC2] & 1) {        /* swizzled: 16-byte x 8-row blocks */
        uint32_t xb = (uint32_t)u * (uint32_t)bits / 8;
        uint32_t block = (uint32_t)(v / 8) * (bytes_per_row / 16) + xb / 16;
        off = block * 128 + (uint32_t)(v % 8) * 16 + xb % 16;
    } else {
        off = (uint32_t)v * bytes_per_row + (uint32_t)u * (uint32_t)bits / 8;
    }
    uint32_t a = base + off;
    switch (fmt) {
    case 0: case 1: case 2: return to_rgba(LHU(a), fmt);
    case 3: return LW(a);
    case 4: return clut_lookup((LBU(a) >> ((u & 1) * 4)) & 15);
    case 5: return clut_lookup(LBU(a));
    case 6: return clut_lookup(LHU(a));
    case 7: return clut_lookup(LW(a));
    default: return 0xFFFF00FFu;            /* DXT: not yet */
    }
}

/* Sample the texture at (u, v) in texels: bilinear when the filter (0xC6: minification in bits 0-2, magnification in
 * bit 8) says linear, else the nearest texel. Level 0 only. */
static uint32_t sample(float u, float v) {
    uint32_t f = reg[0xC6];
    int linear = ((f >> 8) & 1) || (f & 1);
    if (!linear)
        return texel((int)floorf(u), (int)floorf(v));
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    float ax = fu - (float)x0, ay = fv - (float)y0;
    uint32_t c00 = texel(x0, y0), c10 = texel(x0 + 1, y0), c01 = texel(x0, y0 + 1), c11 = texel(x0 + 1, y0 + 1);
    uint32_t out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        float a = (float)((c00 >> ch) & 255) * (1 - ax) + (float)((c10 >> ch) & 255) * ax;
        float b = (float)((c01 >> ch) & 255) * (1 - ax) + (float)((c11 >> ch) & 255) * ax;
        out |= (uint32_t)(a * (1 - ay) + b * ay + 0.5f) << ch;
    }
    return out;
}

/* -- vertices ----------------------------------------------------------------------------------------------------- */

/* A vertex after the vertex stage: screen position (pixels, framebuffer coordinates), depth (0-65535), 1/w for
 * perspective-correct interpolation, texture coordinates (texels), colour (0-255 a channel) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a; } Vertex;

/* Before the viewport: clip-space position and the rest */
typedef struct { float cx, cy, cz, cw, u, v, r, g, b, a; } ClipVertex;

static float world[12], view[12], proj[16], bones[8][12];
static int world_i, view_i, proj_i, bone_i;

static float ge_float(uint32_t data) {
    uint32_t u = data << 8;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int align_up(int x, int a) { return (x + a - 1) & ~(a - 1); }

typedef struct { int size, wo, to, co, no, po; } Layout;

/* The vertex format's layout: weights, texture coordinates, colour, normal, position, each aligned to its size */
static Layout layout(void) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
    int nweights = ((vt >> 14) & 7) + 1;
    static const int tsz[4] = {0, 1, 2, 4}, csz[8] = {0, 0, 0, 0, 2, 2, 2, 4}, psz[4] = {0, 1, 2, 4};
    Layout l = {0};
    int off = 0, maxal = 1;
#define FIELD(present, sz, n, at) \
    if (present) { off = align_up(off, sz); at = off; off += (sz) * (n); maxal = (sz) > maxal ? (sz) : maxal; }
    FIELD(wt, tsz[wt], nweights, l.wo)
    FIELD(tc, tsz[tc], 2, l.to)
    FIELD(col, csz[col], 1, l.co)
    FIELD(nrm, psz[nrm], 3, l.no)
    FIELD(pos, psz[pos], 3, l.po)
#undef FIELD
    l.size = align_up(off, maxal);
    return l;
}

static float read_comp(uint32_t a, int fmt, int through, int is_pos) {
    switch (fmt) {
    case 1: return through ? (float)(int8_t)LBU(a) : (float)(int8_t)LBU(a) / (is_pos ? 127.0f : 128.0f);
    case 2: return through ? (float)(int16_t)LHU(a) : (float)(int16_t)LHU(a) / (is_pos ? 32767.0f : 32768.0f);
    case 3: { uint32_t u = LW(a); float f; memcpy(&f, &u, 4); return f; }
    default: return 0.0f;
    }
}

static void mul43(const float *m, const float *in, float *out) {
    for (int i = 0; i < 3; i++)
        out[i] = in[0] * m[i] + in[1] * m[3 + i] + in[2] * m[6 + i] + m[9 + i];
}

static void mul33(const float *m, const float *in, float *out) {
    for (int i = 0; i < 3; i++)
        out[i] = in[0] * m[i] + in[1] * m[3 + i] + in[2] * m[6 + i];
}

static void unpack(uint32_t c, float *f) {
    f[0] = (float)(c & 255) / 255.0f;
    f[1] = (float)((c >> 8) & 255) / 255.0f;
    f[2] = (float)((c >> 16) & 255) / 255.0f;
}

/* The GE's lighting of one vertex: emissive, plus the global ambient light on the material's ambient colour, plus each
 * enabled light's ambient, diffuse and specular terms (attenuated with distance for point and spot lights, cut to the
 * cone for spots). The material's ambient / diffuse / specular colours are the vertex's own where the material mode
 * (0x53) says. Positions and normals in world space. */
static void light(const float *pos, const float *normal, uint32_t vcol, int has_vcol, ClipVertex *out) {
    float n[3] = {normal[0], normal[1], normal[2]};
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0)
        for (int i = 0; i < 3; i++)
            n[i] /= len;
    uint32_t mode = has_vcol ? reg[0x53] : 0;
    float mat_amb[3], mat_dif[3], mat_spe[3], emi[3], glob[3], vc[3];
    unpack(vcol, vc);
    unpack(reg[0x55], mat_amb);
    unpack(reg[0x56], mat_dif);
    unpack(reg[0x57], mat_spe);
    unpack(reg[0x54], emi);
    unpack(reg[0x5C], glob);
    float mat_amb_a = (float)(reg[0x58] & 255) / 255.0f;
    if (mode & 1) { memcpy(mat_amb, vc, sizeof vc); mat_amb_a = (float)(vcol >> 24) / 255.0f; }
    if (mode & 2) memcpy(mat_dif, vc, sizeof vc);
    if (mode & 4) memcpy(mat_spe, vc, sizeof vc);
    float spow = ge_float(reg[0x5B]);
    float sum[3], spec[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++)
        sum[i] = emi[i] + glob[i] * mat_amb[i];
    for (int k = 0; k < 4; k++) {
        if (!(reg[0x18 + k] & 1))
            continue;
        uint32_t lt = reg[0x5F + k];
        int comp = lt & 3, type = (lt >> 8) & 3;
        float lp[3] = {ge_float(reg[0x63 + 3 * k]), ge_float(reg[0x64 + 3 * k]), ge_float(reg[0x65 + 3 * k])};
        float L[3], att = 1.0f;
        if (type == 0) {
            memcpy(L, lp, sizeof L);
        } else {
            for (int i = 0; i < 3; i++)
                L[i] = lp[i] - pos[i];
        }
        float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (d > 0)
            for (int i = 0; i < 3; i++)
                L[i] /= d;
        if (type != 0) {
            float ka = ge_float(reg[0x7B + 3 * k]), kb = ge_float(reg[0x7C + 3 * k]), kc = ge_float(reg[0x7D + 3 * k]);
            float den = ka + kb * d + kc * d * d;
            att = den > 0 ? 1.0f / den : 1.0f;
            if (att > 1.0f) att = 1.0f;
        }
        if (type == 2) {                /* spot: the cone around the light's direction */
            float dir[3] = {ge_float(reg[0x6F + 3 * k]), ge_float(reg[0x70 + 3 * k]), ge_float(reg[0x71 + 3 * k])};
            float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            float cosang = dl > 0 ? -(L[0] * dir[0] + L[1] * dir[1] + L[2] * dir[2]) / dl : 0;
            float cutoff = ge_float(reg[0x8B + k]), exponent = ge_float(reg[0x87 + k]);
            att *= cosang >= cutoff ? powf(cosang > 0 ? cosang : 0, exponent) : 0.0f;
        }
        float la[3], ld[3], ls[3];
        unpack(reg[0x8F + 3 * k], la);
        unpack(reg[0x90 + 3 * k], ld);
        unpack(reg[0x91 + 3 * k], ls);
        float dot = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (dot < 0) dot = 0;
        if (comp == 2)
            dot = powf(dot, spow);
        for (int i = 0; i < 3; i++)
            sum[i] += (la[i] * mat_amb[i] + ld[i] * mat_dif[i] * dot) * att;
        if (comp == 1 && dot > 0) {     /* specular: the half vector with the viewer straight ahead */
            float h[3] = {L[0], L[1], L[2] + 1.0f};
            float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            float sd = hl > 0 ? (n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) / hl : 0;
            float sp = sd > 0 ? powf(sd, spow) : 0;
            for (int i = 0; i < 3; i++)
                spec[i] += ls[i] * mat_spe[i] * sp * att;
        }
    }
    float rgb[3];
    for (int i = 0; i < 3; i++) {
        rgb[i] = sum[i] + spec[i];
        rgb[i] = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
    }
    float a = mat_amb_a * ((float)(reg[0x5D] & 255) / 255.0f);
    (void)a;
    out->r = rgb[0] * 255.0f;
    out->g = rgb[1] * 255.0f;
    out->b = rgb[2] * 255.0f;
    out->a = mat_amb_a * 255.0f;
}

/* Decode vertex n and run it through the vertex stage up to clip space (through mode: straight to the screen, w 1) */
static void vertex(uint32_t base, int n, const Layout *l, ClipVertex *out, int *through_out) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
    int nweights = ((vt >> 14) & 7) + 1;
    int through = (vt >> 23) & 1;
    uint32_t v = base + (uint32_t)(n * l->size);
    *through_out = through;

    float p[3];
    if (through && pos == 2) {          /* screen x, y (signed), depth (unsigned) */
        p[0] = (float)(int16_t)LHU(v + l->po);
        p[1] = (float)(int16_t)LHU(v + l->po + 2);
        p[2] = (float)LHU(v + l->po + 4);
    } else {
        static const int psz[4] = {0, 1, 2, 4};
        for (int i = 0; i < 3; i++)
            p[i] = read_comp(v + l->po + (uint32_t)(i * psz[pos]), pos, through, 1);
    }

    out->u = out->v = 0;
    if (tc) {
        static const int tsz[4] = {0, 1, 2, 4};
        float u = tc == 3 ? read_comp(v + l->to, 3, 0, 0) : through ? (float)(tc == 1 ? LBU(v + l->to) : LHU(v + l->to))
                  : (tc == 1 ? LBU(v + l->to) / 128.0f : LHU(v + l->to) / 32768.0f);
        float vv = tc == 3 ? read_comp(v + l->to + 4, 3, 0, 0)
                   : through ? (float)(tc == 1 ? LBU(v + l->to + 1) : LHU(v + l->to + 2))
                   : (tc == 1 ? LBU(v + l->to + 1) / 128.0f : LHU(v + l->to + (uint32_t)tsz[tc]) / 32768.0f);
        if (!through) {                 /* scaled, offset, then in texels */
            u = u * ge_float(reg[0x48]) + ge_float(reg[0x4A]);
            vv = vv * ge_float(reg[0x49]) + ge_float(reg[0x4B]);
            u *= (float)(1 << (reg[0xB8] & 0xF));
            vv *= (float)(1 << ((reg[0xB8] >> 8) & 0xF));
        }
        out->u = u;
        out->v = vv;
    }

    uint32_t c;
    if (col >= 4)
        c = col == 7 ? LW(v + l->co) : to_rgba(LHU(v + l->co), col - 4);
    else
        c = (reg[0x55] & 0xFFFFFF) | ((reg[0x58] & 0xFF) << 24);     /* the material's ambient colour */
    out->r = (float)(c & 255);
    out->g = (float)((c >> 8) & 255);
    out->b = (float)((c >> 16) & 255);
    out->a = (float)(c >> 24);

    if (through) {
        out->cx = p[0];
        out->cy = p[1];
        out->cz = p[2];
        out->cw = 1.0f;
        return;
    }
    /* the normal, for lighting */
    int nrm = (vt >> 5) & 3;
    float nr[3] = {0, 0, 1};
    if (nrm) {
        static const int nsz[4] = {0, 1, 2, 4};
        for (int i = 0; i < 3; i++)
            nr[i] = read_comp(v + l->no + (uint32_t)(i * nsz[nrm]), nrm, 0, 1);
        if (reg[0x51] & 1)
            for (int i = 0; i < 3; i++)
                nr[i] = -nr[i];
    }
    float m[3];
    if (wt) {                           /* skinning: the weighted sum of the bones' transforms */
        static const int tsz[4] = {0, 1, 2, 4};
        float acc[3] = {0, 0, 0}, nacc[3] = {0, 0, 0};
        for (int b = 0; b < nweights; b++) {
            float w = read_comp(v + l->wo + (uint32_t)(b * tsz[wt]), wt, 0, 0);
            if (wt == 1) w = LBU(v + l->wo + (uint32_t)b) / 128.0f;
            if (wt == 2) w = LHU(v + l->wo + (uint32_t)(b * 2)) / 32768.0f;
            float t[3], tn[3];
            mul43(bones[b], p, t);
            mul33(bones[b], nr, tn);
            for (int i = 0; i < 3; i++) {
                acc[i] += t[i] * w;
                nacc[i] += tn[i] * w;
            }
        }
        memcpy(p, acc, sizeof p);
        memcpy(nr, nacc, sizeof nr);
    }
    mul43(world, p, m);
    if (reg[0x17] & 1) {                /* lighting, in world space */
        float wn[3];
        mul33(world, nr, wn);
        light(m, wn, col >= 4 ? c : 0xFFFFFFFFu, col >= 4, out);
    }
    float e[3];
    mul43(view, m, e);
    float in[4] = {e[0], e[1], e[2], 1.0f};
    float clip[4];
    for (int i = 0; i < 4; i++)
        clip[i] = in[0] * proj[i] + in[1] * proj[4 + i] + in[2] * proj[8 + i] + in[3] * proj[12 + i];
    out->cx = clip[0];
    out->cy = clip[1];
    out->cz = clip[2];
    out->cw = clip[3];
}

/* Clip space to the screen: the perspective divide, the viewport, the screen offset */
static void to_screen(const ClipVertex *c, int through, Vertex *s) {
    if (through) {
        s->x = c->cx;
        s->y = c->cy;
        s->z = c->cz;
        s->rw = 1.0f;
    } else {
        float rw = 1.0f / c->cw;
        s->x = c->cx * rw * ge_float(reg[0x42]) + ge_float(reg[0x45]) - (float)(reg[0x4C] & 0xFFFF) / 16.0f;
        s->y = c->cy * rw * ge_float(reg[0x43]) + ge_float(reg[0x46]) - (float)(reg[0x4D] & 0xFFFF) / 16.0f;
        s->z = c->cz * rw * ge_float(reg[0x44]) + ge_float(reg[0x47]);
        s->rw = rw;
    }
    s->u = c->u;
    s->v = c->v;
    s->r = c->r;
    s->g = c->g;
    s->b = c->b;
    s->a = c->a;
}

/* -- pixels ------------------------------------------------------------------------------------------------------- */

static uint32_t lerp_mul(uint32_t a, uint32_t b) { return (a * b + 127) / 255; }

static uint32_t tex_function(uint32_t prim, uint32_t tex) {
    uint32_t f = reg[0xC9], fn = f & 7, use_alpha = (f >> 8) & 1;
    uint32_t pr = prim & 255, pg = (prim >> 8) & 255, pb = (prim >> 16) & 255, pa = prim >> 24;
    uint32_t tr = tex & 255, tg = (tex >> 8) & 255, tb = (tex >> 16) & 255, ta = tex >> 24;
    uint32_t r, g, b, a;
    switch (fn) {
    case 0: r = lerp_mul(pr, tr); g = lerp_mul(pg, tg); b = lerp_mul(pb, tb); a = use_alpha ? lerp_mul(pa, ta) : pa; break;
    case 1: r = (tr * ta + pr * (255 - ta)) / 255; g = (tg * ta + pg * (255 - ta)) / 255;
            b = (tb * ta + pb * (255 - ta)) / 255; a = pa; break;
    case 3: r = tr; g = tg; b = tb; a = use_alpha ? ta : pa; break;
    case 4: r = pr + tr; g = pg + tg; b = pb + tb; a = use_alpha ? lerp_mul(pa, ta) : pa;
            r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b; break;
    default: {                                  /* 2: blend with the environment colour */
        uint32_t e = reg[0xCA];
        r = (pr * (255 - tr) + (e & 255) * tr) / 255; g = (pg * (255 - tg) + ((e >> 8) & 255) * tg) / 255;
        b = (pb * (255 - tb) + ((e >> 16) & 255) * tb) / 255; a = use_alpha ? lerp_mul(pa, ta) : pa;
    }
    }
    if ((f >> 16) & 1) {                        /* colour doubling */
        r = r * 2 > 255 ? 255 : r * 2; g = g * 2 > 255 ? 255 : g * 2; b = b * 2 > 255 ? 255 : b * 2;
    }
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static int alpha_test(uint32_t c) {
    if (!(reg[0x22] & 1))
        return 1;
    uint32_t t = reg[0xDB], fn = t & 7, ref = (t >> 8) & 255, mask = (t >> 16) & 255;
    uint32_t a = (c >> 24) & mask;
    ref &= mask;
    switch (fn) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == ref;
    case 3: return a != ref;
    case 4: return a < ref;
    case 5: return a <= ref;
    case 6: return a > ref;
    default: return a >= ref;
    }
}

static uint32_t blend_factor(int which, int f, uint32_t src, uint32_t dst, int ch) {
    uint32_t sa = src >> 24, da = dst >> 24;
    uint32_t sc = (src >> (8 * ch)) & 255, dc = (dst >> (8 * ch)) & 255;
    uint32_t fix = (reg[which ? 0xE1 : 0xE0] >> (8 * ch)) & 255;
    switch (f) {
    case 0: return which ? sc : dc;
    case 1: return 255 - (which ? sc : dc);
    case 2: return sa;
    case 3: return 255 - sa;
    case 4: return da;
    case 5: return 255 - da;
    case 6: return sa * 2 > 255 ? 255 : sa * 2;
    case 7: return (255 - sa) * 2 > 255 ? 255 : (255 - sa) * 2;
    case 8: return da * 2 > 255 ? 255 : da * 2;
    case 9: return (255 - da) * 2 > 255 ? 255 : (255 - da) * 2;
    default: return fix;
    }
}

static uint32_t blend(uint32_t src, uint32_t dst) {
    if (!(reg[0x21] & 1))
        return src;
    uint32_t b = reg[0xDF];
    int fs = b & 15, fd = (b >> 4) & 15, eq = (b >> 8) & 15;
    uint32_t out = src & 0xFF000000u;
    for (int ch = 0; ch < 3; ch++) {
        int32_t s = (int32_t)(((src >> (8 * ch)) & 255) * blend_factor(0, fs, src, dst, ch) / 255);
        int32_t d = (int32_t)(((dst >> (8 * ch)) & 255) * blend_factor(1, fd, src, dst, ch) / 255);
        int32_t sc = (src >> (8 * ch)) & 255, dc = (dst >> (8 * ch)) & 255;
        int32_t r;
        switch (eq) {
        case 1: r = s - d; break;
        case 2: r = d - s; break;
        case 3: r = sc < dc ? sc : dc; break;
        case 4: r = sc > dc ? sc : dc; break;
        case 5: r = sc > dc ? sc - dc : dc - sc; break;
        default: r = s + d; break;
        }
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        out |= (uint32_t)r << (8 * ch);
    }
    return out;
}

static void scissor(int *x0, int *y0, int *x1, int *y1) {
    int sx0 = reg[0xD4] & 0x3FF, sy0 = (reg[0xD4] >> 10) & 0x3FF;
    int sx1 = reg[0xD5] & 0x3FF, sy1 = (reg[0xD5] >> 10) & 0x3FF;
    if (*x0 < sx0) *x0 = sx0;
    if (*y0 < sy0) *y0 = sy0;
    if (*x1 > sx1 + 1) *x1 = sx1 + 1;
    if (*y1 > sy1 + 1) *y1 = sy1 + 1;
}

/* -- primitives --------------------------------------------------------------------------------------------------- */

static uint32_t depth_addr(int x, int y) {
    uint32_t base = 0x04000000u | ((reg[0x9E] & 0xFFFFF0) | ((reg[0x9F] & 0xFF0000) << 8)) % 0x200000u;
    return base + (uint32_t)(y * (int)(reg[0x9F] & 0x7FC) + x) * 2;
}

static int depth_test(int x, int y, float z) {
    if (!(reg[0x23] & 1))
        return 1;
    uint32_t d = LHU(depth_addr(x, y)), zi = z < 0 ? 0 : z > 65535 ? 65535 : (uint32_t)z;
    switch (reg[0xDE] & 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return zi == d;
    case 3: return zi != d;
    case 4: return zi < d;
    case 5: return zi <= d;
    case 6: return zi > d;
    default: return zi >= d;
    }
}

static void depth_write(int x, int y, float z) {
    if ((reg[0x23] & 1) && !(reg[0xE7] & 1))
        SH(depth_addr(x, y), z < 0 ? 0 : z > 65535 ? 65535 : (uint32_t)z);
}

static uint32_t pack(float r, float g, float b, float a) {
    uint32_t ri = r < 0 ? 0 : r > 255 ? 255 : (uint32_t)r, gi = g < 0 ? 0 : g > 255 ? 255 : (uint32_t)g;
    uint32_t bi = b < 0 ? 0 : b > 255 ? 255 : (uint32_t)b, ai = a < 0 ? 0 : a > 255 ? 255 : (uint32_t)a;
    return (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

/* One pixel of a primitive: texture, alpha test, depth, blend, write (or, in clear mode, the clear) */
static void shade(int x, int y, float z, uint32_t color, float u, float v) {
    if (reg[0xD3] & 1) {                /* clear mode: colour / alpha / depth as the flags say */
        uint32_t mode = reg[0xD3] >> 8, old = fb_read(x, y), c = color;
        if (!(mode & 1)) c = (c & 0xFF000000u) | (old & 0x00FFFFFFu);
        if (!(mode & 2)) c = (c & 0x00FFFFFFu) | (old & 0xFF000000u);
        if (mode & 3)
            fb_write(x, y, c);
        if (mode & 4)
            SH(depth_addr(x, y), z < 0 ? 0 : z > 65535 ? 65535 : (uint32_t)z);
        return;
    }
    uint32_t c = color;
    if (reg[0x1E] & 1)
        c = tex_function(c, sample(u, v));
    if (!alpha_test(c) || !depth_test(x, y, z))
        return;
    depth_write(x, y, z);
    fb_write(x, y, blend(c, fb_read(x, y)));
}

static void draw_sprite(const Vertex *a, const Vertex *b) {
    float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y, u0 = a->u, v0 = a->v, u1 = b->u, v1 = b->v;
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    scissor(&ix0, &iy0, &ix1, &iy1);
    uint32_t color = pack(b->r, b->g, b->b, b->a);      /* sprites take the second vertex's colour and depth */
    for (int y = iy0; y < iy1; y++) {
        float fv = y1 > y0 ? v0 + (v1 - v0) * ((float)y + 0.5f - y0) / (y1 - y0) : v0;
        for (int x = ix0; x < ix1; x++) {
            float fu = x1 > x0 ? u0 + (u1 - u0) * ((float)x + 0.5f - x0) / (x1 - x0) : u0;
            shade(x, y, b->z, color, fu, fv);
        }
    }
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void raster_triangle(const Vertex *a, const Vertex *b, const Vertex *c, int through) {
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (area == 0)
        return;
    if (!through && (reg[0x1D] & 1)) {          /* back-face culling: the front face's winding from FFACE */
        int ccw = area < 0;
        if (ccw == (int)(reg[0x9B] & 1))
            return;
    }
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    int x0 = (int)floorf(minx), y0 = (int)floorf(miny), x1 = (int)ceilf(maxx) + 1, y1 = (int)ceilf(maxy) + 1;
    scissor(&x0, &y0, &x1, &y1);
    int flat = !(reg[0x50] & 1);                /* flat shading: the last vertex's colour */
    float inv = 1.0f / area;
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = edge(b->x, b->y, c->x, c->y, px, py) * inv;
            float w1 = edge(c->x, c->y, a->x, a->y, px, py) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            /* perspective-correct attributes: interpolate them over w, and 1 / w */
            float pa = w0 * a->rw, pb = w1 * b->rw, pc = w2 * c->rw, ps = 1.0f / (pa + pb + pc);
            pa *= ps; pb *= ps; pc *= ps;
            float u = pa * a->u + pb * b->u + pc * c->u, v = pa * a->v + pb * b->v + pc * c->v;
            uint32_t color = flat ? pack(c->r, c->g, c->b, c->a)
                                  : pack(pa * a->r + pb * b->r + pc * c->r, pa * a->g + pb * b->g + pc * c->g,
                                         pa * a->b + pb * b->b + pc * c->b, pa * a->a + pb * b->a + pc * c->a);
            shade(x, y, z, color, u, v);
        }
    }
}

/* Clip a triangle against the near plane (w > a little) in clip space, then rasterize what is left */
static void clip_triangle(const ClipVertex *v0, const ClipVertex *v1, const ClipVertex *v2, int through) {
    if (through) {
        Vertex s[3];
        to_screen(v0, 1, &s[0]);
        to_screen(v1, 1, &s[1]);
        to_screen(v2, 1, &s[2]);
        raster_triangle(&s[0], &s[1], &s[2], 1);
        return;
    }
    const float near = 1e-4f;
    ClipVertex in[3] = {*v0, *v1, *v2}, out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const ClipVertex *p = &in[i], *q = &in[(i + 1) % 3];
        int pin = p->cw > near, qin = q->cw > near;
        if (pin)
            out[n++] = *p;
        if (pin != qin) {
            float t = (near - p->cw) / (q->cw - p->cw);
            ClipVertex m;
            float *mf = &m.cx;
            const float *pf = &p->cx, *qf = &q->cx;
            for (int k = 0; k < 10; k++)
                mf[k] = pf[k] + (qf[k] - pf[k]) * t;
            out[n++] = m;
        }
    }
    if (n < 3)
        return;
    Vertex s[4];
    for (int i = 0; i < n; i++)
        to_screen(&out[i], 0, &s[i]);
    for (int i = 1; i + 1 < n; i++)
        raster_triangle(&s[0], &s[i], &s[i + 1], 0);
}

static void prim(uint32_t data) {
    int type = (data >> 16) & 7, count = data & 0xFFFF;
    uint32_t vt = reg[0x12];
    int idx = (vt >> 11) & 3;
    Layout l = layout();
    if (count > 0) {
        ClipVertex *v = malloc(sizeof *v * (size_t)count);
        int through = 0;
        for (int i = 0; i < count; i++) {
            int n = i;
            if (idx == 1) n = LBU(iaddr + (uint32_t)i);
            else if (idx == 2) n = LHU(iaddr + 2 * (uint32_t)i);
            vertex(vaddr, n, &l, &v[i], &through);
        }
        switch (type) {
        case 3:                                     /* triangles */
            for (int i = 0; i + 2 < count; i += 3)
                clip_triangle(&v[i], &v[i + 1], &v[i + 2], through);
            break;
        case 4:                                     /* strip: alternate the winding to keep it */
            for (int i = 0; i + 2 < count; i++)
                if (i & 1)
                    clip_triangle(&v[i + 1], &v[i], &v[i + 2], through);
                else
                    clip_triangle(&v[i], &v[i + 1], &v[i + 2], through);
            break;
        case 5:                                     /* fan */
            for (int i = 1; i + 1 < count; i++)
                clip_triangle(&v[0], &v[i], &v[i + 1], through);
            break;
        case 6:                                     /* sprites: pairs of corners */
            for (int i = 0; i + 1 < count; i += 2) {
                Vertex a, b;
                to_screen(&v[i], through, &a);
                to_screen(&v[i + 1], through, &b);
                draw_sprite(&a, &b);
            }
            break;
        default:
            unhandled_prims[type]++;
            break;
        }
        free(v);
    }
    /* the vertex (or index) address moves past what was drawn */
    if (idx)
        iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
    else
        vaddr += (uint32_t)(count * l.size);
}

/* Block transfer: copy a rectangle of bytes between RAM / VRAM */
static void transfer(uint32_t data) {
    int bpp = (data & 1) ? 4 : 2;
    uint32_t src = (reg[0xB2] & 0xFFFFF0) | ((reg[0xB3] & 0xFF0000) << 8), sw = reg[0xB3] & 0x7FF;
    uint32_t dst = (reg[0xB4] & 0xFFFFF0) | ((reg[0xB5] & 0xFF0000) << 8), dw = reg[0xB5] & 0x7FF;
    int sx = reg[0xEB] & 0x3FF, sy = (reg[0xEB] >> 10) & 0x3FF;
    int dx = reg[0xEC] & 0x3FF, dy = (reg[0xEC] >> 10) & 0x3FF;
    int w = (reg[0xEE] & 0x3FF) + 1, h = ((reg[0xEE] >> 10) & 0x3FF) + 1;
    for (int y = 0; y < h; y++)
        memmove(MEM(dst + (uint32_t)(((dy + y) * (int)dw + dx) * bpp)),
                MEM(src + (uint32_t)(((sy + y) * (int)sw + sx) * bpp)), (size_t)(w * bpp));
}

/* One command of a display list; addr is its data as an address (BASE and OFFSET applied) */
void gpu_command(uint32_t cmd, uint32_t addr) {
    uint32_t op = cmd >> 24, data = cmd & 0xFFFFFF;
    reg[op] = data;
    switch (op) {
    case 0x01: vaddr = addr; break;
    case 0x02: iaddr = addr; break;
    case 0x04: prim(data); break;
    case 0x2A: bone_i = (int)(data & 0x7F); break;
    case 0x2B: if (bone_i < 96) bones[bone_i / 12][bone_i % 12] = ge_float(data); bone_i++; break;
    case 0x3A: world_i = (int)(data & 0xF); break;
    case 0x3B: if (world_i < 12) world[world_i] = ge_float(data); world_i++; break;
    case 0x3C: view_i = (int)(data & 0xF); break;
    case 0x3D: if (view_i < 12) view[view_i] = ge_float(data); view_i++; break;
    case 0x3E: proj_i = (int)(data & 0xF); break;
    case 0x3F: if (proj_i < 16) proj[proj_i] = ge_float(data); proj_i++; break;
    case 0xC4: clut_load(); break;
    case 0xEA: transfer(data); break;
    default: break;
    }
}
