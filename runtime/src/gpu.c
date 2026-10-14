/* A software implementation of the GE's drawing, into emulated VRAM: the reference for checking frames against PPSSPP,
 * until the Vulkan renderer. The vertex stage (every vertex format; skinning; the world, view and projection matrices;
 * the viewport and screen offset; through mode), near-plane clipping, triangles (lists, strips, fans) and sprites,
 * perspective-correct texturing (the 16/32-bit, CLUT and swizzled formats; nearest texel), Gouraud or flat colour,
 * the texture functions, alpha and depth tests, back-face culling, blending, clear mode, and block transfers.
 * Lighting (four lights: directional, point, spot; ambient, diffuse, powered diffuse, specular), bilinear filtering.
 * Fog. Not yet: mipmaps, texture coordinate generation, the DXT formats, lines and points, stencil, logic ops,
 * dithering. */
#include "hle.h"
#include "render.h"

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

void gpu_decode_texture(uint32_t *out, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[y * w + x] = texel(x, y);
}

/* -- vertices ----------------------------------------------------------------------------------------------------- */

/* A vertex after the vertex stage: screen position (pixels, framebuffer coordinates), depth (0-65535), 1/w for
 * perspective-correct interpolation, texture coordinates (texels), colour (0-255 a channel), fog (1 none, 0 all fog);
 * for the renderer's shadows, its position in the world (the camera taken out) and the part of its colour the sun gave
 * (0-255, in the colour) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb, nx, ny, nz; } Vertex;

/* Before the viewport: clip-space position and the rest */
typedef struct { float cx, cy, cz, cw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb, nx, ny, nz; } ClipVertex;
#define VERTEX_FLOATS 20

static float world[12], view[12], proj[16], bones[8][12];
static int world_i, view_i, proj_i, bone_i;

/* Shadows (render.c): the GE's world space - which has the camera in it: the game multiplies it into each world
 * matrix - to the world, for the frame; and the light that is the sun among the GE's (-1: none known) */
static int shadow_world;            /* to_world is known this frame */
static float to_world[12], game_to_world[12];
static int sun_light = -1;
static int prim_world;              /* this primitive's vertices get their place in the world */
/* Lighting by the pixel (render.c): the primitive's directional lights are left to the GPU - the vertex keeps the rest
 * of the lighting, its material's diffuse colour (in sr, sg, sb) and its normal in the world */
static int prim_pixel_light;
static uint32_t pixel_lights;       /* the lights (bits) the GPU does */
static float sun_dir[3];            /* towards the sun, in the world (as last seen) */
static int sun_known;

int camera_game(float *inv, float *target);
void camera_invert(const float *v, float *out);
void render_shadow_frame(const float *light_vp, int on);
void render_prim_world(int on);
void render_prim_lights(int n, const float *dirs, const float *colors, int sun);

#define SHADOW_HALF 1500.0f         /* the shadow map covers this far each way from what the camera looks at */
#define SHADOW_RANGE 6000.0f        /* and this far towards and away from the sun */
#define SHADOW_SIZE 2048


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

/* A frame's shadows: the world from what is drawn (the camera shown, or the game's), and the sun's view of the world
 * around what the camera looks at - its centre moved in whole texels, so that the shadows' edges stand still */
static void shadow_frame(const float *shown) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = !getenv("MHFU_SHADOWS") || atoi(getenv("MHFU_SHADOWS")) != 0;
    float c[3];
    shadow_world = enabled && camera_game(game_to_world, c);
    if (!shadow_world) {
        render_shadow_frame(NULL, 0);
        return;
    }
    if (shown)
        camera_invert(shown, to_world);
    else
        memcpy(to_world, game_to_world, sizeof to_world);
    if (!sun_known) {
        render_shadow_frame(NULL, 0);
        return;
    }
    const float *d = sun_dir;
    float up[3] = {0, 1, 0};
    if (fabsf(d[1]) > 0.99f)
        up[0] = 1, up[1] = 0;
    float r[3] = {up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int i = 0; i < 3; i++)
        r[i] /= rl;
    float u[3] = {d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]};
    float texel = 2 * SHADOW_HALF / SHADOW_SIZE;
    float cr = r[0] * c[0] + r[1] * c[1] + r[2] * c[2], cu = u[0] * c[0] + u[1] * c[1] + u[2] * c[2];
    float sr = floorf(cr / texel) * texel - cr, su = floorf(cu / texel) * texel - cu;
    for (int i = 0; i < 3; i++)
        c[i] += r[i] * sr + u[i] * su;
    float m[16] = {0};                  /* by columns: x along r, y along u, depth away from the sun (0 to 1) */
    for (int i = 0; i < 3; i++) {
        m[4 * i + 0] = r[i] / SHADOW_HALF;
        m[4 * i + 1] = u[i] / SHADOW_HALF;
        m[4 * i + 2] = -d[i] / (2 * SHADOW_RANGE);
    }
    m[12] = -(r[0] * c[0] + r[1] * c[1] + r[2] * c[2]) / SHADOW_HALF;
    m[13] = -(u[0] * c[0] + u[1] * c[1] + u[2] * c[2]) / SHADOW_HALF;
    m[14] = 0.5f + (d[0] * c[0] + d[1] * c[1] + d[2] * c[2]) / (2 * SHADOW_RANGE);
    m[15] = 1;
    render_shadow_frame(m, 1);
}

/* Before a primitive's vertices: whether they are placed in the world (3D, through the camera), and which light is the
 * sun - the directional one giving most - its direction in the world noted for the next frame's shadows */
static void shadow_prim(void) {
    int through = (reg[0x12] >> 23) & 1;
    prim_world = shadow_world && !through && proj[11] != 0;
    render_prim_world(prim_world);
    sun_light = -1;
    prim_pixel_light = 0;
    pixel_lights = 0;
    if (through || !(reg[0x17] & 1)) {
        render_prim_lights(0, NULL, NULL, -1);
        return;
    }
    float best = 0;
    for (int k = 0; k < 4; k++) {
        if (!(reg[0x18 + k] & 1) || ((reg[0x5F + k] >> 8) & 3) != 0)
            continue;
        uint32_t ld = reg[0x90 + 3 * k];
        float lum = (float)(ld & 255) + (float)((ld >> 8) & 255) + (float)((ld >> 16) & 255);
        if (lum > best)
            best = lum, sun_light = k;
    }
    prim_pixel_light = 0;
    pixel_lights = 0;
    static int pixel = -1;
    if (pixel < 0)
        pixel = !getenv("MHFU_PIXEL_LIGHTING") || atoi(getenv("MHFU_PIXEL_LIGHTING")) != 0;
    if (pixel && prim_world) {
        /* the directional lights giving diffuse light (no specular) go to the GPU, in the world */
        float dirs[16], cols[16];
        int n = 0, sun = -1, ok = 1;
        for (int k = 0; k < 4; k++) {
            if (!(reg[0x18 + k] & 1))
                continue;
            int comp = reg[0x5F + k] & 3, type = (reg[0x5F + k] >> 8) & 3;
            if (comp == 1 && (reg[0x91 + 3 * k] & 0xFFFFFF))
                ok = 0;                 /* specular: all by the vertex */
            if (type != 0)
                continue;
            float L[3] = {ge_float(reg[0x63 + 3 * k]), ge_float(reg[0x64 + 3 * k]), ge_float(reg[0x65 + 3 * k])}, w[3];
            mul33(game_to_world, L, w);
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (!(l > 0))
                continue;
            uint32_t ld = reg[0x90 + 3 * k];
            for (int i = 0; i < 3; i++)
                dirs[4 * n + i] = w[i] / l;
            dirs[4 * n + 3] = comp == 2 ? ge_float(reg[0x5B]) : 0;      /* a power of the diffuse term */
            cols[4 * n + 0] = (float)(ld & 255) / 255.0f;
            cols[4 * n + 1] = (float)((ld >> 8) & 255) / 255.0f;
            cols[4 * n + 2] = (float)((ld >> 16) & 255) / 255.0f;
            cols[4 * n + 3] = 0;
            if (k == sun_light)
                sun = n;
            pixel_lights |= 1u << k;
            n++;
        }
        if (ok && n) {
            prim_pixel_light = 1;
            render_prim_lights(n, dirs, cols, sun);
        } else
            pixel_lights = 0;
    }
    if (!prim_pixel_light)
        render_prim_lights(0, NULL, NULL, -1);
    if (sun_light >= 0 && prim_world) {
        int k = sun_light;
        float L[3] = {ge_float(reg[0x63 + 3 * k]), ge_float(reg[0x64 + 3 * k]), ge_float(reg[0x65 + 3 * k])}, w[3];
        mul33(game_to_world, L, w);
        float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        if (l > 0) {
            for (int i = 0; i < 3; i++)
                sun_dir[i] = w[i] / l;
            sun_known = 1;
        }
    }
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
    float sum[3], spec[3] = {0, 0, 0}, sun[3] = {0, 0, 0};
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
        int gpu = (pixel_lights >> k) & 1;      /* its diffuse term by the pixel */
        for (int i = 0; i < 3; i++) {
            sum[i] += (la[i] * mat_amb[i] + (gpu ? 0 : ld[i] * mat_dif[i] * dot)) * att;
            if (k == sun_light && !gpu)
                sun[i] += ld[i] * mat_dif[i] * dot * att;
        }
        if (comp == 1 && dot > 0) {     /* specular: the half vector with the viewer straight ahead */
            float h[3] = {L[0], L[1], L[2] + 1.0f};
            float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            float sd = hl > 0 ? (n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) / hl : 0;
            float sp = sd > 0 ? powf(sd, spow) : 0;
            for (int i = 0; i < 3; i++) {
                spec[i] += ls[i] * mat_spe[i] * sp * att;
                if (k == sun_light)
                    sun[i] += ls[i] * mat_spe[i] * sp * att;
            }
        }
    }
    float rgb[3];
    for (int i = 0; i < 3; i++) {
        rgb[i] = sum[i] + spec[i];
        rgb[i] = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
        /* the sun's part: what the colour loses without it (as clamped) */
        float without = rgb[i] - sun[i];
        sun[i] = rgb[i] - (without < 0 ? 0 : without);
    }
    out->sr = sun[0] * 255.0f;
    out->sg = sun[1] * 255.0f;
    out->sb = sun[2] * 255.0f;
    if (pixel_lights) {                 /* the material's diffuse colour, for the GPU's lights */
        out->sr = mat_dif[0] * 255.0f;
        out->sg = mat_dif[1] * 255.0f;
        out->sb = mat_dif[2] * 255.0f;
    }
    /* alpha: the material's ambient alpha (or the vertex's), times the global ambient light's */
    float a = mat_amb_a * ((float)(reg[0x5D] & 255) / 255.0f);
    out->r = rgb[0] * 255.0f;
    out->g = rgb[1] * 255.0f;
    out->b = rgb[2] * 255.0f;
    out->a = a * 255.0f;
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
    out->wx = out->wy = out->wz = 0;
    out->sr = out->sg = out->sb = 0;
    out->nx = out->ny = out->nz = 0;

    if (through) {
        out->cx = p[0];
        out->cy = p[1];
        out->cz = p[2];
        out->cw = 1.0f;
        out->fog = 1.0f;
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
    if (prim_world)
        mul43(to_world, m, &out->wx);
    if (reg[0x17] & 1) {                /* lighting, in world space */
        float wn[3];
        mul33(world, nr, wn);
        light(m, wn, col >= 4 ? c : 0xFFFFFFFFu, col >= 4, out);
        if (pixel_lights)
            mul33(to_world, wn, &out->nx);
    }
    float e[3];
    mul43(view, m, e);
    /* fog: (end + view-space depth) * slope, 1 for none and 0 for all fog */
    float fog = (ge_float(reg[0xCD]) + e[2]) * ge_float(reg[0xCE]);
    out->fog = fog != fog ? 1.0f : fog < 0 ? 0 : fog > 1 ? 1 : fog;
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
    s->fog = c->fog;
    memcpy(&s->wx, &c->wx, 9 * sizeof(float));
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
static uint32_t apply_fog(uint32_t c, float fog) {
    uint32_t f = reg[0xCF], out = c & 0xFF000000u;
    for (int ch = 0; ch < 24; ch += 8) {
        float v = (float)((c >> ch) & 255) * fog + (float)((f >> ch) & 255) * (1 - fog);
        out |= (uint32_t)(v + 0.5f) << ch;
    }
    return out;
}

static void shade(int x, int y, float z, uint32_t color, float u, float v, float fog) {
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
    if ((reg[0x1F] & 1) && fog < 1)
        c = apply_fog(c, fog);
    if (!alpha_test(c) || !depth_test(x, y, z))
        return;
    depth_write(x, y, z);
    fb_write(x, y, blend(c, fb_read(x, y)));
}

static void draw_sprite(const Vertex *a, const Vertex *b) {
    if (render_on) {
        render_sprite((const RVertex *)a, (const RVertex *)b);
        return;
    }
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
            shade(x, y, b->z, color, fu, fv, 1.0f);
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
    /* back-face culling: the front face's winding from FFACE (on the GPU for what casts shadows: the faces turned
     * away from the camera may face the sun) */
    if (!through && (reg[0x1D] & 1) && !(render_on && prim_world)) {
        int ccw = area < 0;
        if (ccw == (int)(reg[0x9B] & 1))
            return;
    }
    if (render_on) {
        render_triangle((const RVertex *)a, (const RVertex *)b, (const RVertex *)c);
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
            shade(x, y, z, color, u, v, pa * a->fog + pb * b->fog + pc * c->fog);
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
            for (int k = 0; k < VERTEX_FLOATS; k++)
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

static void prim(uint32_t data);

/* -- frames, recorded for interpolation --------------------------------------------------------------------------
 *
 * With the Vulkan renderer at a frame rate of its own (MHFU_FPS), the game's primitives are recorded rather than drawn:
 * the GE's registers, the matrices, the texture (resolved and uploaded now) and a copy of the vertex and index data.
 * A frame ends at the game's flip. The display then draws frames from the last two recorded, replaying each primitive
 * through the same vertex stage - its data copied back into a scratch area of PSP memory the game does not use (the
 * kernel's) - with its matrices interpolated between the two frames. */

typedef struct {
    uint32_t reg[256];
    float world[12], view[12], proj[16], bones[8][12];
    uint32_t data;                  /* the PRIM command's */
    uint32_t vaddr, iaddr;          /* where the vertices (and indices) were, to match primitives between frames */
    uint32_t ibytes;                /* the indices' size */
    size_t icopy;                   /* their copy's offset in the frame's data */
    int run;                        /* the run of draws whose vertices it is among */
    uint32_t tex_slot;
    int tex_w, tex_h;
} Draw;

/* Consecutive draws on the same vertices (strips into a model's buffer) share a copy of the span they use */
typedef struct {
    uint32_t vaddr, lo, hi;         /* the bytes from vaddr + lo to vaddr + hi */
    size_t copy;                    /* their offset in the frame's data */
} Run;

typedef struct {
    Draw *draws;
    int n, cap;
    Run *runs;
    int nruns, rcap, open;          /* open: the last run is still taking draws (its copy not made yet) */
    uint8_t *bytes;
    size_t used, size;
    uint32_t display_addr;
    uint64_t time;                  /* when the game finished it */
} Frame;

static int recording;               /* primitives are recorded, not drawn */
static int in_replay;               /* drawing a recorded frame (gpu_render_frame) */
static unsigned frame_generation_direct = 1, shadow_generation;    /* frames drawn as the game draws them */
static unsigned frame_generation;   /* counts the frames recorded: what is worked out from the last two keeps until it changes */
static Frame frames[3];             /* recording, latest, previous */
static Frame *rec = &frames[0], *latest = &frames[1], *previous = &frames[2];
#define SCRATCH_VERTICES 0x08000000u
#define SCRATCH_INDICES  0x08600000u

static size_t keep(Frame *f, const void *p, size_t n) {
    if (f->used + n > f->size) {
        f->size = (f->used + n) * 2;
        f->bytes = realloc(f->bytes, f->size);
    }
    memcpy(f->bytes + f->used, p, n);
    size_t at = f->used;
    f->used += (n + 15) & ~(size_t)15;
    return at;
}

/* The open run is done: its vertices are kept */
static void close_run(Frame *f) {
    if (!f->open)
        return;
    Run *r = &f->runs[f->nruns - 1];
    r->copy = keep(f, PTR(r->vaddr + r->lo), r->hi - r->lo);
    f->open = 0;
}

static void record(uint32_t data, const Layout *l) {
    int count = data & 0xFFFF, idx = (reg[0x12] >> 11) & 3;
    if (rec->n == rec->cap) {
        rec->cap = rec->cap ? rec->cap * 2 : 1024;
        rec->draws = realloc(rec->draws, sizeof *rec->draws * (size_t)rec->cap);
    }
    Draw *d = &rec->draws[rec->n++];
    memcpy(d->reg, reg, sizeof reg);
    memcpy(d->world, world, sizeof world);
    memcpy(d->view, view, sizeof view);
    memcpy(d->proj, proj, sizeof proj);
    memcpy(d->bones, bones, sizeof bones);
    d->data = data;
    d->vaddr = vaddr;
    d->iaddr = idx ? iaddr : 0;
    int nverts = count, first = 0;
    d->ibytes = 0;
    if (idx) {
        /* small strips index into big shared buffers: only the vertices they use are kept */
        d->ibytes = (uint32_t)(count * (idx == 1 ? 1 : 2));
        nverts = 0;
        first = 0xFFFF;
        for (int i = 0; i < count; i++) {
            int n = idx == 1 ? LBU(iaddr + (uint32_t)i) : LHU(iaddr + 2 * (uint32_t)i);
            if (n + 1 > nverts)
                nverts = n + 1;
            if (n < first)
                first = n;
        }
        if (first > nverts)
            first = 0;
        d->icopy = keep(rec, PTR(iaddr), d->ibytes);
    }
    uint32_t lo = (uint32_t)(first * l->size), hi = (uint32_t)(nverts * l->size);
    Run *r = rec->open ? &rec->runs[rec->nruns - 1] : NULL;
    if (r && r->vaddr == vaddr) {
        if (lo < r->lo) r->lo = lo;
        if (hi > r->hi) r->hi = hi;
    } else {
        close_run(rec);
        if (rec->nruns == rec->rcap) {
            rec->rcap = rec->rcap ? rec->rcap * 2 : 256;
            rec->runs = realloc(rec->runs, sizeof *rec->runs * (size_t)rec->rcap);
        }
        rec->runs[rec->nruns++] = (Run){vaddr, lo, hi, 0};
        rec->open = 1;
    }
    d->run = rec->nruns - 1;
    d->tex_slot = 0xFFFFFFFFu;
    if (!(reg[0xD3] & 1) && (reg[0x1E] & 1))
        d->tex_slot = render_texture(reg, &d->tex_w, &d->tex_h);
}

/* The time between the last two recorded frames (the game's frame time, normally 1/30 s) */
uint64_t gpu_frame_period(void) {
    return latest->time > previous->time ? latest->time - previous->time : 33333;
}

uint64_t gpu_last_frame_time(void) { return latest->time; }

/* Interpolation between frames: on (with Vulkan) when MHFU_FPS is set */
void gpu_record(int on) { recording = on; }

/* The game's frame is done: it becomes the latest, the latest the previous */
/* present.c, each game frame: the next is drawn with its own shadows' placement */
void gpu_frame_tick(void) {
    frame_generation_direct++;
}

void gpu_frame_end(uint32_t display_addr, uint64_t time) {
    close_run(rec);
    Frame *old = previous;
    previous = latest;
    latest = rec;
    rec = old;
    rec->n = 0;
    rec->nruns = 0;
    rec->used = 0;
    latest->display_addr = display_addr;
    latest->time = time;
    frame_generation++;
}

static void lerp(float *out, const float *a, const float *b, int n, float t) {
    for (int i = 0; i < n; i++)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

/* camera.c: the camera shown, with the mouse's turn at once (and the frames' cameras, to take out of world matrices) */
int camera_views(float t, int interpolating, float *prev_inv, float *latest_inv, float *shown);
static int native_camera;
static float prev_inv[12], latest_inv[12], shown_view[12];
static float before_inv[12];        /* the camera before the previous frame's, undone */
/* stale (below): for each draw of the latest frame, placed with the camera of the frame before. The game draws a frame
 * with the camera of its update before (the scene's matrix read after a frame is the next's): told apart by which keeps
 * the draws still in the world, as most say, so that it holds whatever the game does */
static uint8_t *stale;
static int stale_known;             /* (worked out for this frame: interpolating, the camera shown) */
void camera_before_inverse(float *inv);

static void compose43(const float *a, const float *b, float *out) {
    float m[12];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            m[3 * j + i] = a[i] * b[3 * j] + a[3 + i] * b[3 * j + 1] + a[6 + i] * b[3 * j + 2] + (j == 3 ? a[9 + i] : 0);
    memcpy(out, m, sizeof m);
}

static int placed_run = -1;         /* the run whose vertices are in the scratch area */

/* Replay a draw of the latest frame; with its match in the previous (or NULL), its matrices interpolated by t */
static int draw_bones(const Draw *d);

static void replay(const Draw *d, const Draw *prev, float t) {
    memcpy(reg, d->reg, sizeof reg);
    if (native_camera && d->proj[11] != 0) {
        /* the game multiplied its camera into the world matrix: take each frame's out, interpolate in the world, and
         * put the camera shown in */
        float wl[12];
        int late = stale_known && stale[d - latest->draws];     /* placed with the camera a frame late */
        compose43(late ? prev_inv : latest_inv, d->world, wl);
        if (prev) {
            float wp[12];
            compose43(late ? before_inv : prev_inv, prev->world, wp);
            lerp(world, wp, wl, 12, t);
            lerp(view, prev->view, d->view, 12, t);
            lerp(proj, prev->proj, d->proj, 16, t);
            lerp(&bones[0][0], &prev->bones[0][0], &d->bones[0][0], 12 * draw_bones(d), t);
        } else {
            memcpy(world, wl, sizeof world);
            memcpy(view, d->view, sizeof view);
            memcpy(proj, d->proj, sizeof proj);
            memcpy(bones, d->bones, sizeof(float) * 12 * (size_t)draw_bones(d));
        }
        compose43(shown_view, world, world);
    } else if (prev) {
        lerp(world, prev->world, d->world, 12, t);
        lerp(view, prev->view, d->view, 12, t);
        lerp(proj, prev->proj, d->proj, 16, t);
        lerp(&bones[0][0], &prev->bones[0][0], &d->bones[0][0], 12 * draw_bones(d), t);
    } else {
        memcpy(world, d->world, sizeof world);
        memcpy(view, d->view, sizeof view);
        memcpy(proj, d->proj, sizeof proj);
        memcpy(bones, d->bones, sizeof(float) * 12 * (size_t)draw_bones(d));
    }
    const Frame *f = latest;
    if (d->run != placed_run) {
        const Run *r = &f->runs[d->run];
        memcpy(PTR(SCRATCH_VERTICES + r->lo), f->bytes + r->copy, r->hi - r->lo);
        placed_run = d->run;
    }
    if (d->ibytes)
        memcpy(PTR(SCRATCH_INDICES), f->bytes + d->icopy, d->ibytes);
    vaddr = SCRATCH_VERTICES;
    iaddr = SCRATCH_INDICES;
    render_override_texture(d->tex_slot, d->tex_w, d->tex_h);
    prim(d->data);
    render_override_texture(0xFFFFFFFEu, 0, 0);
}

/* A primitive's identity across frames: where its vertices (and indices: strips share a model's vertices) are, their
 * format, the primitive type and count */
static uint64_t draw_key(const Draw *d) {
    return ((uint64_t)d->vaddr << 32 | d->iaddr) ^ ((uint64_t)d->reg[0x12] << 8 ^ d->data) * 0x9E3779B97F4A7C15ull;
}

/* How many bone matrices a draw uses (0 if it is not skinned) */
static int draw_bones(const Draw *d) {
    return (d->reg[0x12] >> 9) & 3 ? (int)((d->reg[0x12] >> 14) & 7) + 1 : 0;
}

/* Where a draw is: its world matrix's translation, through its first bone's if it is skinned */
static void draw_position(const Draw *d, float *p) {
    if ((d->reg[0x12] >> 9) & 3) {
        float t[3];
        mul43(d->world, &d->bones[0][9], t);
        memcpy(p, t, sizeof t);
    } else {
        memcpy(p, &d->world[9], 3 * sizeof(float));
    }
}

/* Farther than this between frames: not the same thing moving - 40 game units, and as the camera turns, things far
 * from it sweep further: a fifth of their distance from it (11 degrees a frame) */
#define MATCH_DISTANCE 40.0f
#define MATCH_TURN 0.2f

/* How far a draw is from the camera: its position through the view matrix (the game may put the camera in the view
 * matrix or in each world matrix) */
static float camera_distance(const Draw *d, const float *p) {
    float e[3];
    mul43(d->view, p, e);
    return sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
}

/* A position in what the game draws in (its camera in it) to the world, through the frame's camera undone */
static void to_frame_world(const float *inv, float *p) {
    float w[3];
    mul43(inv, p, w);
    memcpy(p, w, sizeof w);
}

/* A matched draw's evidence of the camera it was placed with: its moves (squared) between the frames with the frame's
 * camera undone and with the one before's. Marks the draw 2 (clearly late) or 3 (clearly not), else 0 (as the frame) */
static int votes_late, votes_fresh;
static int frames_late = 1;         /* the frames are drawn with the camera of the update before (as last told) */
void camera_frames_late(int late);
static uint8_t *stale;

static void vote(int i, float fresh, float late) {
    if (fresh < 1 && late < 1)
        return;                 /* the camera did not move enough to tell */
    if (late < 0.25f * fresh)
        votes_late++;
    else if (fresh < 0.25f * late)
        votes_fresh++;
    if (late < 0.01f * fresh && fresh > 25)
        stale[i] = 2;
    else if (fresh < 0.01f * late && late > 25)
        stale[i] = 3;
}

static float dist2(const float *a, const float *b) {
    return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
}

static int cmp_key(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The latest frame's keys, sorted, to count a key's draws */
static uint64_t *latest_keys;
static int latest_n;

static int latest_count(uint64_t k) {
    int lo = 0, hi = latest_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (latest_keys[mid] < k) lo = mid + 1;
        else hi = mid;
    }
    int n = 0;
    while (lo + n < latest_n && latest_keys[lo + n] == k)
        n++;
    return n;
}

/* Match each draw of the latest frame to the previous frame's: the previous draw with the same key (vertex address,
 * format, type, count) if there is one in each; among several - copies of a model (a herd, dust) come in a different
 * order from frame to frame - the nearest not already taken, within MATCH_DISTANCE (and MATCH_TURN) */
static int *match(int in_world) {
    static int *m, cap;
    static unsigned done = ~0u;
    static int done_world;
    if (done == frame_generation && done_world == in_world)
        return m;
    done = frame_generation;
    done_world = in_world;
    static uint8_t *taken;
    static int tcap;
    if (latest->n > cap) {
        cap = latest->n * 2;
        m = realloc(m, sizeof *m * (size_t)cap);
        stale = realloc(stale, (size_t)cap);
    }
    if (in_world)
        camera_before_inverse(before_inv);
    votes_late = votes_fresh = 0;
    if (previous->n > tcap) {
        tcap = previous->n * 2;
        taken = realloc(taken, (size_t)tcap);
    }
    memset(taken, 0, (size_t)previous->n);
    /* the previous frame's draws, sorted by key, to find a key's group by binary search */
    static int *order;
    static int ocap;
    if (previous->n > ocap) {
        ocap = previous->n * 2;
        order = realloc(order, sizeof *order * (size_t)ocap);
    }
    static uint64_t *keys;
    static int kcap;
    if (previous->n > kcap) {
        kcap = previous->n * 2;
        keys = realloc(keys, sizeof *keys * (size_t)kcap);
    }
    for (int i = 0; i < previous->n; i++) {
        keys[i] = draw_key(&previous->draws[i]);
        order[i] = i;
    }
    /* insertion sort is too slow for thousands: a simple bottom-up merge sort on order[] by keys[] */
    static int *tmp;
    static int tmpcap;
    if (previous->n > tmpcap) {
        tmpcap = previous->n * 2;
        tmp = realloc(tmp, sizeof *tmp * (size_t)tmpcap);
    }
    for (int w = 1; w < previous->n; w *= 2) {
        for (int lo = 0; lo < previous->n; lo += 2 * w) {
            int mid = lo + w < previous->n ? lo + w : previous->n, hi = lo + 2 * w < previous->n ? lo + 2 * w : previous->n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = keys[order[i]] <= keys[order[j]] ? order[i++] : order[j++];
            while (i < mid) tmp[k++] = order[i++];
            while (j < hi) tmp[k++] = order[j++];
        }
        int *t = order; order = tmp; tmp = t;
    }
    static int lcap;
    if (latest->n > lcap) {
        lcap = latest->n * 2;
        latest_keys = realloc(latest_keys, sizeof *latest_keys * (size_t)lcap);
    }
    for (int i = 0; i < latest->n; i++)
        latest_keys[i] = draw_key(&latest->draws[i]);
    latest_n = latest->n;
    qsort(latest_keys, (size_t)latest_n, sizeof *latest_keys, cmp_key);
    for (int i = 0; i < latest->n; i++) {
        const Draw *d = &latest->draws[i];
        uint64_t k = draw_key(d);
        int lo = 0, hi = previous->n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (keys[order[mid]] < k) lo = mid + 1;
            else hi = mid;
        }
        float p[3], p_late[3];
        draw_position(d, p);
        memcpy(p_late, p, sizeof p);
        if (in_world) {
            to_frame_world(latest_inv, p);
            to_frame_world(prev_inv, p_late);
        }
        stale[i] = 0;
        int best = -1;
        float best_fresh = 0, best_late = 0;
        if (lo < previous->n && keys[order[lo]] == k && (lo + 1 == previous->n || keys[order[lo + 1]] != k) &&
            latest_count(k) == 1) {
            m[i] = taken[order[lo]] ? -1 : order[lo];
            if (m[i] >= 0) {
                taken[order[lo]] = 1;
                if (in_world) {         /* which camera it was placed with: the one that keeps it still-er */
                    float q[3], q_late[3];
                    draw_position(&previous->draws[m[i]], q);
                    memcpy(q_late, q, sizeof q);
                    to_frame_world(prev_inv, q);
                    to_frame_world(before_inv, q_late);
                    vote(i, dist2(p, q), dist2(p_late, q_late));
                }
            }
            continue;
        }
        /* in the world the camera's turn moves nothing: only the distance (as seen, things far off sweep further) */
        float reach = in_world ? MATCH_DISTANCE : MATCH_DISTANCE + MATCH_TURN * camera_distance(d, p);
        float best_d = reach * reach;
        for (int j = lo; j < previous->n && keys[order[j]] == k; j++) {
            int c = order[j];
            if (taken[c])
                continue;
            float q[3], q_late[3];
            draw_position(&previous->draws[c], q);
            memcpy(q_late, q, sizeof q);
            int late = 0;
            if (in_world) {
                to_frame_world(prev_inv, q);
                to_frame_world(before_inv, q_late);
            }
            float dd = dist2(p, q), dd_late = in_world ? dist2(p_late, q_late) : dd;
            if (dd_late < dd) {
                dd = dd_late;
                late = 1;
            }
            if (dd <= best_d) {
                best_d = dd;
                best = c;
                best_fresh = dist2(p, q);
                best_late = dd_late;
            }
            (void)late;
        }
        m[i] = best;
        if (best >= 0) {
            taken[best] = 1;
            if (in_world)
                vote(i, best_fresh, best_late);
        }
    }
    /* the frame's camera: as most of its draws say; a draw says otherwise only on clear evidence of its own (its move
     * with one camera a tenth of that with the other, and over 5 units) */
    if (in_world) {
        /* when the camera turns enough to tell; kept until it does again */
        if (votes_late + votes_fresh > 50)
            frames_late = votes_late > votes_fresh;
        int frame_late = frames_late;
        for (int i = 0; i < latest->n; i++)
            stale[i] = stale[i] == 2 ? 1 : stale[i] == 3 ? 0 : (uint8_t)frame_late;
    }
    return m;
}

static unsigned long cuts, shown_frames;      /* for MHFU_FPS_STATS */
static float cut_turn, cut_move;

/* Where a view matrix's camera is: the point it takes to the origin, -R^T t */
static void view_eye(const float *v, float *eye) {
    for (int j = 0; j < 3; j++)
        eye[j] = -(v[3 * j] * v[9] + v[3 * j + 1] * v[10] + v[3 * j + 2] * v[11]);
}

/* Has the camera jumped between the frames (a cut): it moved far, or turned more than any camera turns in a frame?
 * (Its position is compared, not the view matrix's translation: that is -R eye, which swings with any turn.) */
static int camera_cut(const Frame *a, const Frame *b) {
    const Draw *da = NULL, *db = NULL;
    for (int i = 0; i < a->n && !da; i++)
        if (!((a->draws[i].reg[0x12] >> 23) & 1))
            da = &a->draws[i];
    for (int i = 0; i < b->n && !db; i++)
        if (!((b->draws[i].reg[0x12] >> 23) & 1))
            db = &b->draws[i];
    if (!da || !db)
        return 0;
    /* the angle between the rotations: trace(Ra^T Rb) = 1 + 2 cos(angle) */
    float tr = 0;
    for (int i = 0; i < 9; i++)
        tr += da->view[i] * db->view[i];
    float ea[3], eb[3];
    view_eye(da->view, ea);
    view_eye(db->view, eb);
    float move = sqrtf((ea[0] - eb[0]) * (ea[0] - eb[0]) + (ea[1] - eb[1]) * (ea[1] - eb[1]) +
                       (ea[2] - eb[2]) * (ea[2] - eb[2]));
    int cut = tr < 1.0f + 2.0f * 0.5f || move > 600.0f;     /* over 60 degrees, or 600 units */
    if (cut) {
        cuts++;
        cut_turn = tr;
        cut_move = move;
    }
    return cut;
}

static unsigned long matched, unmatched;

/* Draw a frame t of the way (0-1) from the previous recorded frame to the latest; returns its display address, 0 if
 * there is none */
uint32_t gpu_render_frame(float t) {
    if (!latest->n)
        return 0;
    uint32_t saved[256];
    memcpy(saved, reg, sizeof reg);
    uint32_t sv = vaddr, si = iaddr;
    float sw[12], svw[12], sp[16], sb[8][12];
    memcpy(sw, world, sizeof sw);
    memcpy(svw, view, sizeof svw);
    memcpy(sp, proj, sizeof sp);
    memcpy(sb, bones, sizeof sb);
    shown_frames++;
    placed_run = -1;
    int interpolate = previous->n && t < 1.0f && !camera_cut(previous, latest);
    camera_frames_late(frames_late);
    native_camera = camera_views(t, interpolate, prev_inv, latest_inv, shown_view);
    int *m = interpolate ? match(native_camera) : NULL;
    stale_known = m && native_camera;
    shadow_frame(native_camera ? shown_view : NULL);
    recording = 0;
    in_replay = 1;
    for (int i = 0; i < latest->n; i++) {
        const Draw *prev = m && m[i] >= 0 ? &previous->draws[m[i]] : NULL;
        if (m)
            prev ? matched++ : unmatched++;
        replay(&latest->draws[i], prev, t);
    }
    recording = 1;
    in_replay = 0;
    memcpy(reg, saved, sizeof reg);
    vaddr = sv;
    iaddr = si;
    memcpy(world, sw, sizeof sw);
    memcpy(view, svw, sizeof svw);
    memcpy(proj, sp, sizeof sp);
    memcpy(bones, sb, sizeof sb);
    return latest->display_addr;
}

/* How much the last frames' primitives matched (for MHFU_FPS_STATS) */
void gpu_match_stats(unsigned long *m, unsigned long *u) {
    *m = matched;
    *u = unmatched;
    matched = unmatched = 0;
    if (cuts)
        fprintf(stderr, "  %lu of %lu frames shown were camera cuts; the last turned %.0f degrees, moved %.0f\n", cuts,
                shown_frames, acosf(fmaxf(-1, fminf(1, (cut_turn - 1) / 2))) * 57.29578f, cut_move);
    cuts = shown_frames = 0;
}

static void prim(uint32_t data) {
    int type = (data >> 16) & 7, count = data & 0xFFFF;
    uint32_t vt = reg[0x12];
    int idx = (vt >> 11) & 3;
    Layout l = layout();
    if (recording && count > 0) {
        record(data, &l);
        if (idx)
            iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
        else
            vaddr += (uint32_t)(count * l.size);
        return;
    }
    if (render_on && count > 0) {
        if (!in_replay && shadow_generation != frame_generation_direct) {
            shadow_generation = frame_generation_direct;
            shadow_frame(NULL);
        }
        shadow_prim();
        render_prim_begin(reg);
    }
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
