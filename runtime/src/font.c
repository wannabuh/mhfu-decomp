/* sceFont: the PSP's system fonts (PGF), which the game draws its text with. The PGF files are the PSP's firmware fonts
 * (flash0:/font) - not on the disc; PPSSPP ships compatible ones, found in its assets (or MHFU_FONTS=<dir>).
 *
 * PGF: a header (0x188 bytes for revision 2), tables of glyph metrics (pairs of 26.6 values: dimensions, x and y
 * bearings, advances), a shadow map, the character map (code - first glyph -> glyph index, charMapBpe bits each) and
 * the glyph pointers (charPointerBpe bits each, in 4-byte units), then the glyphs: a bit stream (LSB first) of the
 * bitmap's size, offsets, flags, shadow, metrics (as table indices or 32-bit values) and the bitmap, 4 bits a pixel,
 * run-length coded in nibbles, by rows or by columns. */
#include "hle.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t map_len, ptr_len, map_bpe, ptr_bpe, first, last;
    int32_t max_ascender, max_descender, max_left_x, max_base_y, min_center_x, max_top_y;
    int32_t max_advance[2], max_size[2];
    uint16_t max_glyph_w, max_glyph_h;
    uint8_t bpp;
    int32_t (*dim)[2], (*xadj)[2], (*yadj)[2], (*adv)[2];
    const uint8_t *map, *ptrs, *glyphs;
    size_t glyphs_size;
    char name[64], type[64];
} Pgf;

typedef struct {
    int w, h, left, top, flags, shadow_flags, shadow_id;
    int32_t dim[2], xadj[2], yadj[2], adv[2];
    size_t bitmap;              /* bit position of the bitmap */
} Glyph;

/* The PSP's font registry: size (26.6 points), resolution, family, style, language, name, file */
typedef struct { uint32_t h, v, hres, vres; uint16_t family, style, language; const char *name, *file; } FontEntry;
enum { SANS = 1, SERIF = 2 };
enum { REGULAR = 1, ITALIC = 2, BOLD = 5, BOLD_ITALIC = 6, DB = 103 };
enum { JAPANESE = 1, LATIN = 2, KOREAN = 3 };
static const FontEntry registry[] = {
    {0x288, 0x288, 0x2000, 0x2000, SANS, DB, JAPANESE, "FTT-NewRodin Pro DB", "jpn0.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SANS, REGULAR, LATIN, "FTT-NewRodin Pro Latin", "ltn0.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SERIF, REGULAR, LATIN, "FTT-Matisse Pro Latin", "ltn1.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SANS, ITALIC, LATIN, "FTT-NewRodin Pro Latin", "ltn2.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SERIF, ITALIC, LATIN, "FTT-Matisse Pro Latin", "ltn3.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SANS, BOLD, LATIN, "FTT-NewRodin Pro Latin", "ltn4.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SERIF, BOLD, LATIN, "FTT-Matisse Pro Latin", "ltn5.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SANS, BOLD_ITALIC, LATIN, "FTT-NewRodin Pro Latin", "ltn6.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SERIF, BOLD_ITALIC, LATIN, "FTT-Matisse Pro Latin", "ltn7.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SANS, REGULAR, LATIN, "FTT-NewRodin Pro Latin", "ltn8.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SERIF, REGULAR, LATIN, "FTT-Matisse Pro Latin", "ltn9.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SANS, ITALIC, LATIN, "FTT-NewRodin Pro Latin", "ltn10.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SERIF, ITALIC, LATIN, "FTT-Matisse Pro Latin", "ltn11.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SANS, BOLD, LATIN, "FTT-NewRodin Pro Latin", "ltn12.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SERIF, BOLD, LATIN, "FTT-Matisse Pro Latin", "ltn13.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SANS, BOLD_ITALIC, LATIN, "FTT-NewRodin Pro Latin", "ltn14.pgf"},
    {0x1C0, 0x1C0, 0x2000, 0x2000, SERIF, BOLD_ITALIC, LATIN, "FTT-Matisse Pro Latin", "ltn15.pgf"},
    {0x288, 0x288, 0x2000, 0x2000, SANS, REGULAR, KOREAN, "AsiaNHH(512Johab)", "kr0.pgf"},
};
#define NFONTS (int)(sizeof registry / sizeof registry[0])

static Pgf *loaded[NFONTS];

/* -- PGF ---------------------------------------------------------------------------------------------------------- */

static uint32_t bits(const uint8_t *p, size_t size, size_t pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t b = pos + (size_t)i;
        if ((b >> 3) < size)
            v |= (uint32_t)((p[b >> 3] >> (b & 7)) & 1) << i;
    }
    return v;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static const char *font_dir(void) {
    static const char *dirs[] = {"/usr/share/ppsspp/assets/flash0/font", "/usr/local/share/ppsspp/assets/flash0/font",
                                 "/usr/share/games/ppsspp/assets/flash0/font"};
    const char *e = getenv("MHFU_FONTS");
    if (e)
        return e;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/ltn0.pgf", dirs[i]);
        FILE *f = fopen(p, "rb");
        if (f) {
            fclose(f);
            return dirs[i];
        }
    }
    return NULL;
}

static Pgf *pgf_load(int index) {
    if (loaded[index])
        return loaded[index];
    const char *dir = font_dir();
    if (!dir) {
        HLE_WARN("no PSP fonts (install PPSSPP, or set MHFU_FONTS to a directory of .pgf files)");
        return NULL;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, registry[index].file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        HLE_WARN("%s: not found", path);
        return NULL;
    }
    Pgf *p = calloc(1, sizeof *p);
    fseek(f, 0, SEEK_END);
    p->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    p->data = malloc(p->size);
    if (fread(p->data, 1, p->size, f) != p->size) {
        fclose(f);
        free(p->data);
        free(p);
        return NULL;
    }
    fclose(f);
    const uint8_t *d = p->data;
    uint16_t header_size;
    memcpy(&header_size, d + 2, 2);
    p->map_len = rd32(d + 0x10);
    p->ptr_len = rd32(d + 0x14);
    p->map_bpe = rd32(d + 0x18);
    p->ptr_bpe = rd32(d + 0x1C);
    p->bpp = d[0x22];
    memcpy(p->name, d + 0x35, 63);
    memcpy(p->type, d + 0x75, 63);
    p->first = d[0xB6] | (uint32_t)d[0xB7] << 8;
    p->last = d[0xB8] | (uint32_t)d[0xB9] << 8;
    p->max_ascender = (int32_t)rd32(d + 0xD4);
    p->max_descender = (int32_t)rd32(d + 0xD8);
    p->max_left_x = (int32_t)rd32(d + 0xDC);
    p->max_base_y = (int32_t)rd32(d + 0xE0);
    p->min_center_x = (int32_t)rd32(d + 0xE4);
    p->max_top_y = (int32_t)rd32(d + 0xE8);
    p->max_advance[0] = (int32_t)rd32(d + 0xEC);
    p->max_advance[1] = (int32_t)rd32(d + 0xF0);
    p->max_size[0] = (int32_t)rd32(d + 0xF4);
    p->max_size[1] = (int32_t)rd32(d + 0xF8);
    memcpy(&p->max_glyph_w, d + 0xFC, 2);
    memcpy(&p->max_glyph_h, d + 0xFE, 2);
    int nd = d[0x102], nx = d[0x103], ny = d[0x104], na = d[0x105];
    uint32_t shadow_len = rd32(d + 0x16C), shadow_bpe = rd32(d + 0x170);
    size_t off = header_size;
    p->dim = (int32_t(*)[2])(void *)(p->data + off); off += (size_t)nd * 8;
    p->xadj = (int32_t(*)[2])(void *)(p->data + off); off += (size_t)nx * 8;
    p->yadj = (int32_t(*)[2])(void *)(p->data + off); off += (size_t)ny * 8;
    p->adv = (int32_t(*)[2])(void *)(p->data + off); off += (size_t)na * 8;
    off += ((size_t)shadow_len * shadow_bpe + 31) / 32 * 4;
    p->map = p->data + off; off += ((size_t)p->map_len * p->map_bpe + 31) / 32 * 4;
    p->ptrs = p->data + off; off += ((size_t)p->ptr_len * p->ptr_bpe + 31) / 32 * 4;
    p->glyphs = p->data + off;
    p->glyphs_size = off < p->size ? p->size - off : 0;
    loaded[index] = p;
    return p;
}

static int pgf_glyph(const Pgf *p, uint32_t code, Glyph *g) {
    if (code < p->first || code > p->last)
        return 0;
    uint32_t gi = bits(p->map, p->ptrs - p->map, (size_t)(code - p->first) * p->map_bpe, (int)p->map_bpe);
    if (gi >= p->ptr_len)
        return 0;
    uint32_t ptr = bits(p->ptrs, p->glyphs - p->ptrs, (size_t)gi * p->ptr_bpe, (int)p->ptr_bpe);
    const uint8_t *d = p->glyphs;
    size_t n = p->glyphs_size;
    size_t pos = (size_t)ptr * 32 + 14;         /* past the shadow offset */
    g->w = (int)bits(d, n, pos, 7); pos += 7;
    g->h = (int)bits(d, n, pos, 7); pos += 7;
    g->left = (int)bits(d, n, pos, 7); pos += 7;
    g->top = (int)bits(d, n, pos, 7); pos += 7;
    if (g->left >= 64) g->left -= 128;
    if (g->top >= 64) g->top -= 128;
    g->flags = (int)bits(d, n, pos, 6); pos += 6;
    g->shadow_flags = (int)(bits(d, n, pos, 2) << 5 | bits(d, n, pos + 2, 2) << 3 | bits(d, n, pos + 4, 3)); pos += 7;
    g->shadow_id = (int)bits(d, n, pos, 9); pos += 9;
    int32_t (*tables[4])[2] = {p->dim, p->xadj, p->yadj, p->adv};
    int32_t *out[4] = {g->dim, g->xadj, g->yadj, g->adv};
    for (int k = 0; k < 4; k++) {
        if (g->flags & (4 << k)) {
            uint32_t i = bits(d, n, pos, 8); pos += 8;
            out[k][0] = tables[k][i][0];
            out[k][1] = tables[k][i][1];
        } else {
            out[k][0] = (int32_t)bits(d, n, pos, 32); pos += 32;
            out[k][1] = (int32_t)bits(d, n, pos, 32); pos += 32;
        }
    }
    g->bitmap = pos;
    return 1;
}

/* Decode a glyph's bitmap: 4-bit alpha per pixel, w * h */
static void pgf_bitmap(const Pgf *p, const Glyph *g, uint8_t *out) {
    const uint8_t *d = p->glyphs;
    size_t n = p->glyphs_size, pos = g->bitmap;
    int total = g->w * g->h, i = 0;
    memset(out, 0, (size_t)total);
    while (i < total && (pos >> 3) < n) {
        uint32_t nib = bits(d, n, pos, 4), value = 0;
        pos += 4;
        int count;
        if (nib < 8) {
            value = bits(d, n, pos, 4);
            pos += 4;
            count = (int)nib + 1;
        } else {
            count = 16 - (int)nib;
        }
        for (int k = 0; k < count && i < total; k++, i++) {
            if (nib >= 8) {
                value = bits(d, n, pos, 4);
                pos += 4;
            }
            int x, y;
            if ((g->flags & 3) == 1) { x = i % g->w; y = i / g->w; }
            else { x = i / g->h; y = i % g->h; }
            out[y * g->w + x] = (uint8_t)value;
        }
    }
}

/* -- the library -------------------------------------------------------------------------------------------------- */

#define MAX_OPEN 16
static struct { uint32_t handle; int index; } open_fonts[MAX_OPEN];
static uint32_t lib_handle;
static uint32_t alt_char = 0x5F;

static Pgf *font_of(uint32_t handle) {
    for (int i = 0; i < MAX_OPEN; i++)
        if (open_fonts[i].handle && open_fonts[i].handle == handle)
            return pgf_load(open_fonts[i].index);
    return NULL;
}

/* (params, error*) -> the library's handle */
void hle_sceFontNewLib(Ctx *c) {
    if (!lib_handle)
        lib_handle = kmem_alloc(0x100, 0, 0x100, 0);
    if (ARG1)
        SW(ARG1, 0);
    RET(lib_handle);
}

void hle_sceFontDoneLib(Ctx *c) { RET(0); }

void hle_sceFontGetNumFontList(Ctx *c) {
    if (ARG1)
        SW(ARG1, 0);
    RET(NFONTS);
}

/* PGFFontStyle: fontH, fontV, fontHRes, fontVRes, fontWeight (floats); family, style, styleSub, language, region,
 * country (u16); name[64], fileName[64]; attributes, expire */
void hle_sceFontFindOptimumFont(Ctx *c) {
    uint32_t s = ARG1;
    float h;
    uint32_t hb = LW(s);
    memcpy(&h, &hb, 4);
    uint16_t family = LHU(s + 20), style = LHU(s + 22), language = LHU(s + 26);
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < NFONTS; i++) {
        const FontEntry *e = &registry[i];
        if ((family && e->family != family) || (style && e->style != style) || (language && e->language != language))
            continue;
        float d = h > 0 ? (float)e->h / 64.0f - h : 0;
        d = d < 0 ? -d : d;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best < 0)
        best = 1;                       /* the Latin sans serif */
    if (ARG2)
        SW(ARG2, 0);
    RET(best);
}

/* (lib, index, mode, error*) -> a font handle */
void hle_sceFontOpen(Ctx *c) {
    int index = (int)ARG1;
    if (index < 0 || index >= NFONTS || !pgf_load(index)) {
        if (ARG3)
            SW(ARG3, 0x80460004u);
        RET(0);
        return;
    }
    for (int i = 0; i < MAX_OPEN; i++)
        if (!open_fonts[i].handle) {
            open_fonts[i].handle = kmem_alloc(0x100, 0, 0x100, 0);
            open_fonts[i].index = index;
            if (ARG3)
                SW(ARG3, 0);
            RET(open_fonts[i].handle);
            return;
        }
    if (ARG3)
        SW(ARG3, 0x80460001u);
    RET(0);
}

void hle_sceFontClose(Ctx *c) {
    for (int i = 0; i < MAX_OPEN; i++)
        if (open_fonts[i].handle == ARG0) {
            kmem_free(open_fonts[i].handle);
            open_fonts[i].handle = 0;
        }
    RET(0);
}

static void put_float(uint32_t addr, float f) { uint32_t u; memcpy(&u, &f, 4); SW(addr, u); }

/* PGFFontInfo (264 bytes): ten 26.6 metrics, the same as floats, max glyph width / height (s16), glyph count,
 * shadow map length, the font's style (as in FindOptimumFont), bpp */
void hle_sceFontGetFontInfo(Ctx *c) {
    Pgf *p = font_of(ARG0);
    if (!p) {
        RET(0x80460005u);
        return;
    }
    int index = 0;
    for (int i = 0; i < MAX_OPEN; i++)
        if (open_fonts[i].handle == ARG0)
            index = open_fonts[i].index;
    uint32_t fi = ARG1;
    memset(PTR(fi), 0, 264);
    int32_t m[10] = {p->max_size[0], p->max_size[1], p->max_ascender, p->max_descender, p->max_left_x, p->max_base_y,
                     p->min_center_x, p->max_top_y, p->max_advance[0], p->max_advance[1]};
    for (int i = 0; i < 10; i++) {
        SW(fi + 4 * (uint32_t)i, (uint32_t)m[i]);
        put_float(fi + 40 + 4 * (uint32_t)i, (float)m[i] / 64.0f);
    }
    SH(fi + 80, p->max_glyph_w);
    SH(fi + 82, p->max_glyph_h);
    SW(fi + 84, p->ptr_len);
    SW(fi + 88, 0);
    const FontEntry *e = &registry[index];
    uint32_t st = fi + 92;
    put_float(st, (float)e->h / 64.0f);
    put_float(st + 4, (float)e->v / 64.0f);
    put_float(st + 8, (float)e->hres / 64.0f);
    put_float(st + 12, (float)e->vres / 64.0f);
    put_float(st + 16, 0.0f);
    SH(st + 20, e->family);
    SH(st + 22, e->style);
    SH(st + 26, e->language);
    strncpy((char *)PTR(st + 32), e->name, 63);
    strncpy((char *)PTR(st + 96), e->file, 63);
    SB(fi + 260, p->bpp);
    RET(0);
}

/* PGFCharInfo (0x3C): bitmap width, height, left, top; 26.6 width, height, ascender, descender, bearings HX HY VX VY,
 * advances H V; shadow flags, shadow id (s16) */
void hle_sceFontGetCharInfo(Ctx *c) {
    Pgf *p = font_of(ARG0);
    if (!p) {
        RET(0x80460005u);
        return;
    }
    Glyph g;
    if (!pgf_glyph(p, ARG1, &g) && !pgf_glyph(p, alt_char, &g)) {
        memset(PTR(ARG2), 0, 0x3C);
        RET(0);
        return;
    }
    uint32_t ci = ARG2;
    SW(ci, (uint32_t)g.w);
    SW(ci + 4, (uint32_t)g.h);
    SW(ci + 8, (uint32_t)g.left);
    SW(ci + 12, (uint32_t)g.top);
    SW(ci + 16, (uint32_t)g.dim[0]);
    SW(ci + 20, (uint32_t)g.dim[1]);
    SW(ci + 24, (uint32_t)g.yadj[0]);
    SW(ci + 28, (uint32_t)(g.yadj[0] - g.dim[1]));
    SW(ci + 32, (uint32_t)g.xadj[0]);
    SW(ci + 36, (uint32_t)g.yadj[0]);
    SW(ci + 40, (uint32_t)g.xadj[1]);
    SW(ci + 44, (uint32_t)g.yadj[1]);
    SW(ci + 48, (uint32_t)g.adv[0]);
    SW(ci + 52, (uint32_t)g.adv[1]);
    SH(ci + 56, (uint32_t)g.shadow_flags);
    SH(ci + 58, (uint32_t)g.shadow_id);
    RET(0);
}

/* GlyphImage: pixel format (0 4-bit, 1 4-bit high nibble first, 2 8-bit, 3 24-bit, 4 32-bit), x and y (26.6), buffer
 * width, height, bytes per line (u16), pad, buffer */
static void set_pixel(uint32_t buf, int bpl, int fmt, int x, int y, uint32_t v4) {
    uint32_t v8 = v4 * 17;
    switch (fmt) {
    case 0: case 1: {
        uint32_t a = buf + (uint32_t)(y * bpl + x / 2);
        uint32_t b = LBU(a);
        int shift = ((x & 1) ^ (fmt == 1)) * 4;
        uint32_t old = (b >> shift) & 15;
        if (v4 > old)
            SB(a, (b & ~(15u << shift)) | (v4 << shift));
        break;
    }
    case 2: {
        uint32_t a = buf + (uint32_t)(y * bpl + x);
        if (v8 > LBU(a))
            SB(a, v8);
        break;
    }
    case 3: {
        uint32_t a = buf + (uint32_t)(y * bpl + x * 3);
        if (v8 > LBU(a)) {
            SB(a, v8); SB(a + 1, v8); SB(a + 2, v8);
        }
        break;
    }
    default: {
        uint32_t a = buf + (uint32_t)(y * bpl + x * 4);
        if (v8 > (LW(a) >> 24))
            SW(a, (v8 << 24) | 0x00FFFFFFu);
        break;
    }
    }
}

void hle_sceFontGetCharGlyphImage(Ctx *c) {
    Pgf *p = font_of(ARG0);
    if (!p) {
        RET(0x80460005u);
        return;
    }
    Glyph g;
    if (!pgf_glyph(p, ARG1, &g) && !pgf_glyph(p, alt_char, &g)) {
        RET(0);
        return;
    }
    uint32_t im = ARG2;
    int fmt = (int)LW(im);
    int x0 = (int32_t)LW(im + 4) >> 6, y0 = (int32_t)LW(im + 8) >> 6;
    int bw = LHU(im + 12), bh = LHU(im + 14), bpl = LHU(im + 16);
    uint32_t buf = LW(im + 20);
    static uint8_t bitmap[128 * 128];
    if (g.w * g.h > (int)sizeof bitmap) {
        RET(0);
        return;
    }
    pgf_bitmap(p, &g, bitmap);
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++) {
            int px = x0 + x, py = y0 + y;
            if (px >= 0 && py >= 0 && px < bw && py < bh && bitmap[y * g.w + x])
                set_pixel(buf, bpl, fmt, px, py, bitmap[y * g.w + x]);
        }
    RET(0);
}
