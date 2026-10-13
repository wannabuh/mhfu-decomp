/* A software implementation of the GE's drawing, into emulated VRAM: the reference for checking frames against PPSSPP
 * until the Vulkan renderer, and enough for the 2D the game starts with. So far: sprites (screen-aligned rectangles),
 * through mode, textured (the 16/32-bit, CLUT and swizzled formats) or not, the texture functions, alpha test,
 * blending, clear mode, and block transfers. Triangles, 3D transforms and depth are still to do. */
#include "hle.h"

#include <math.h>
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

/* -- vertices ----------------------------------------------------------------------------------------------------- */

typedef struct { float x, y, z, u, v; uint32_t color; } Vertex;

static int align_up(int x, int a) { return (x + a - 1) & ~(a - 1); }

/* Decode vertex i of the current vertex format at vaddr (indexed or not) */
static void decode_vertex(uint32_t base, int index, Vertex *out) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
    int nweights = ((vt >> 14) & 7) + 1;
    static const int tsz[4] = {0, 1, 2, 4}, csz[8] = {0, 0, 0, 0, 2, 2, 2, 4}, psz[4] = {0, 1, 2, 4};
    int off = 0, maxal = 1;
    int wo = 0, to = 0, co = 0, no = 0, po = 0;
    if (wt) { off = align_up(off, tsz[wt]); wo = off; off += tsz[wt] * nweights; maxal = tsz[wt] > maxal ? tsz[wt] : maxal; }
    if (tc) { off = align_up(off, tsz[tc]); to = off; off += tsz[tc] * 2; maxal = tsz[tc] > maxal ? tsz[tc] : maxal; }
    if (col) { off = align_up(off, csz[col]); co = off; off += csz[col]; maxal = csz[col] > maxal ? csz[col] : maxal; }
    if (nrm) { off = align_up(off, psz[nrm]); no = off; off += psz[nrm] * 3; maxal = psz[nrm] > maxal ? psz[nrm] : maxal; }
    if (pos) { off = align_up(off, psz[pos]); po = off; off += psz[pos] * 3; maxal = psz[pos] > maxal ? psz[pos] : maxal; }
    (void)wo; (void)no;
    int size = align_up(off, maxal);
    uint32_t v = base + (uint32_t)(index * size);
    int through = (vt >> 23) & 1;

    memset(out, 0, sizeof *out);
    out->color = 0xFFFFFFFFu;
    if (tc == 1) { out->u = LBU(v + to); out->v = LBU(v + to + 1); if (!through) { out->u /= 128.0f; out->v /= 128.0f; } }
    else if (tc == 2) { out->u = LHU(v + to); out->v = LHU(v + to + 2); if (!through) { out->u /= 32768.0f; out->v /= 32768.0f; } }
    else if (tc == 3) { uint32_t a = LW(v + to), b = LW(v + to + 4); memcpy(&out->u, &a, 4); memcpy(&out->v, &b, 4); }
    if (col >= 4)
        out->color = col == 7 ? LW(v + co) : to_rgba(LHU(v + co), col - 4);
    if (pos == 1) { out->x = (int8_t)LBU(v + po); out->y = (int8_t)LBU(v + po + 1); out->z = (int8_t)LBU(v + po + 2); }
    else if (pos == 2) {
        out->x = (int16_t)LHU(v + po); out->y = (int16_t)LHU(v + po + 2);
        out->z = through ? LHU(v + po + 4) : (int16_t)LHU(v + po + 4);
    } else if (pos == 3) {
        uint32_t a = LW(v + po), b = LW(v + po + 4), c3 = LW(v + po + 8);
        memcpy(&out->x, &a, 4); memcpy(&out->y, &b, 4); memcpy(&out->z, &c3, 4);
    }
}

static int vertex_size(void) {
    /* the stride decode_vertex uses: decode two and difference their addresses would cost more; recompute */
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
    int nweights = ((vt >> 14) & 7) + 1;
    static const int tsz[4] = {0, 1, 2, 4}, csz[8] = {0, 0, 0, 0, 2, 2, 2, 4}, psz[4] = {0, 1, 2, 4};
    int off = 0, maxal = 1;
    if (wt) { off = align_up(off, tsz[wt]) + tsz[wt] * nweights; maxal = tsz[wt] > maxal ? tsz[wt] : maxal; }
    if (tc) { off = align_up(off, tsz[tc]) + tsz[tc] * 2; maxal = tsz[tc] > maxal ? tsz[tc] : maxal; }
    if (col) { off = align_up(off, csz[col]) + csz[col]; maxal = csz[col] > maxal ? csz[col] : maxal; }
    if (nrm) { off = align_up(off, psz[nrm]) + psz[nrm] * 3; maxal = psz[nrm] > maxal ? psz[nrm] : maxal; }
    if (pos) { off = align_up(off, psz[pos]) + psz[pos] * 3; maxal = psz[pos] > maxal ? psz[pos] : maxal; }
    return align_up(off, maxal);
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

static void draw_sprite(const Vertex *a, const Vertex *b) {
    int clear = reg[0xD3] & 1;
    int textured = !clear && (reg[0x1E] & 1);
    float x0 = a->x, y0 = a->y, x1 = b->x, y1 = b->y, u0 = a->u, v0 = a->v, u1 = b->u, v1 = b->v;
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    int ox = (reg[0x4C] & 0xFFFF) >> 4, oy = (reg[0x4D] & 0xFFFF) >> 4;
    int through = (reg[0x12] >> 23) & 1;
    if (!through) {             /* not through: the vertices are not screen coordinates yet - approximate */
        x0 -= (float)ox; x1 -= (float)ox; y0 -= (float)oy; y1 -= (float)oy;
    }
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
    scissor(&ix0, &iy0, &ix1, &iy1);
    if (textured && !through) {
        int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
        u0 *= (float)w; u1 *= (float)w; v0 *= (float)h; v1 *= (float)h;
    }
    uint32_t color = b->color;          /* sprites take the second vertex's colour */
    for (int y = iy0; y < iy1; y++) {
        float fv = y1 > y0 ? v0 + (v1 - v0) * ((float)y + 0.5f - y0) / (y1 - y0) : v0;
        for (int x = ix0; x < ix1; x++) {
            uint32_t c = color;
            if (clear) {
                uint32_t mode = reg[0xD3] >> 8, old = fb_read(x, y);
                if (!(mode & 1)) c = (c & 0xFF000000u) | (old & 0x00FFFFFFu);
                if (!(mode & 2)) c = (c & 0x00FFFFFFu) | (old & 0xFF000000u);
                fb_write(x, y, c);
                continue;
            }
            if (textured) {
                float fu = x1 > x0 ? u0 + (u1 - u0) * ((float)x + 0.5f - x0) / (x1 - x0) : u0;
                c = tex_function(c, texel((int)floorf(fu), (int)floorf(fv)));
            }
            if (!alpha_test(c))
                continue;
            fb_write(x, y, blend(c, fb_read(x, y)));
        }
    }
}

static void prim(uint32_t data) {
    int type = (data >> 16) & 7, count = data & 0xFFFF;
    uint32_t vt = reg[0x12];
    int idx = (vt >> 11) & 3;
    int size = vertex_size();
    Vertex v[2];
    if (type == 6) {
        for (int i = 0; i + 1 < count; i += 2) {
            for (int k = 0; k < 2; k++) {
                int n = i + k;
                if (idx == 1) n = LBU(iaddr + (uint32_t)n);
                else if (idx == 2) n = LHU(iaddr + 2 * (uint32_t)n);
                decode_vertex(vaddr, n, &v[k]);
            }
            draw_sprite(&v[0], &v[1]);
        }
    } else {
        unhandled_prims[type]++;
    }
    /* the vertex (or index) address moves past what was drawn */
    if (idx)
        iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
    else
        vaddr += (uint32_t)(count * size);
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
    case 0xC4: clut_load(); break;
    case 0xEA: transfer(data); break;
    default: break;
    }
}
