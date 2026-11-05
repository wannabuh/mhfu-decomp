/* A software implementation of the GE's drawing, into emulated VRAM: the reference for checking frames against PPSSPP,
 * until the Vulkan renderer. The vertex stage (every vertex format; skinning; the world, view and projection matrices;
 * the viewport and screen offset; through mode), near-plane clipping, triangles (lists, strips, fans) and sprites,
 * perspective-correct texturing (the 16/32-bit, CLUT and swizzled formats; nearest texel), Gouraud or flat colour,
 * the texture functions, alpha and depth tests, back-face culling, blending, clear mode, and block transfers.
 * Lighting (four lights: directional, point, spot; ambient, diffuse, powered diffuse, specular), bilinear filtering.
 * Fog. Not yet: mipmaps, texture coordinate generation, the DXT formats, lines and points, stencil, logic ops,
 * dithering. */
#include "hle.h"
#include "savestate.h"
#include "render.h"
#include "render_queue.h"
#include "shared.h"
#include "texpack.h"
#include "perf.h"
#include "inspect.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

SAVED static uint32_t reg[256];          /* the last value of each command */
SAVED static uint32_t vaddr, iaddr;

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

/* (the render queue: below) */
static int draw_v = 512;            /* (seen_v: the draw's, for the texture pack's key - given the renderer with it) */
static void q_shadow_frame(const float *m, int on);
static void q_prim_world(int on);
static void q_prim_view(const float *m, const float *wv);
static void q_prim_lights(int n, const float *dirs, const float *colors, int sun);
static void q_time(double seconds);
static void q_override(uint32_t slot, int w, int h);
static void q_clut(uint32_t addr, uint32_t bytes);
static void q_begin(const uint32_t *r);
static int queued(void);

/* -- textures ----------------------------------------------------------------------------------------------------- */

SAVED static uint32_t clut[256];

static void clut_load(void) {
    uint32_t addr = (reg[0xB0] & 0xFFFFF0) | ((reg[0xB1] & 0x0F0000) << 8);
    uint32_t bytes = (reg[0xC4] & 0x3F) * 32;
    if (texpack_active())           /* (as PPSSPP loads it: 0x40 blocks are 0x40, not 0) */
        q_clut(addr, (reg[0xC4] & 0x7F) == 0x40 ? 0x40 * 32 : bytes);
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

/* render_texture.c: a texture decoded, as the registers given have it (the renderer's: its own thread's copy) - its
 * palette read from where they say, as the GE has it loaded from there */
void gpu_decode_texture(uint32_t *out, int w, int h, const uint32_t *r) {
    uint32_t pal[256];
    int fmt = r[0xC3] & 0xF;
    if (fmt >= 4 && fmt <= 7) {
        uint32_t addr = (r[0xB0] & 0xFFFFF0) | ((r[0xB1] & 0x0F0000) << 8), bytes = (r[0xC4] & 0x3F) * 32;
        int cf = r[0xC5] & 3;
        uint32_t n = cf == 3 ? bytes / 4 : bytes / 2;
        memset(pal, 0, sizeof pal);
        for (uint32_t i = 0; i < n && i < 256; i++)
            pal[i] = cf == 3 ? LW(addr + 4 * i) : to_rgba(LHU(addr + 2 * i), cf);
    }
    int tw = 1 << (r[0xB8] & 0xF), th = 1 << ((r[0xB8] >> 8) & 0xF);
    uint32_t base = (r[0xA0] & 0xFFFFF0) | ((r[0xA8] & 0x0F0000) << 8), bw = r[0xA8] & 0x7FF;
    int bits = fmt == 3 || fmt == 7 ? 32 : fmt == 4 ? 4 : fmt == 5 ? 8 : 16;
    uint32_t bytes_per_row = bw * (uint32_t)bits / 8, mode = r[0xC5];
    uint32_t shift = (mode >> 2) & 31, mask = (mode >> 8) & 0xFF, start = ((mode >> 16) & 31) * 16;
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++) {
            int uu = u & (tw - 1), vv = v & (th - 1);
            uint32_t off;
            if (r[0xC2] & 1) {
                uint32_t xb = (uint32_t)uu * (uint32_t)bits / 8;
                uint32_t block = (uint32_t)(vv / 8) * (bytes_per_row / 16) + xb / 16;
                off = block * 128 + (uint32_t)(vv % 8) * 16 + xb % 16;
            } else {
                off = (uint32_t)vv * bytes_per_row + (uint32_t)uu * (uint32_t)bits / 8;
            }
            uint32_t a = base + off, c, i = 0;
            switch (fmt) {
            case 0: case 1: case 2: c = to_rgba(LHU(a), fmt); break;
            case 3: c = LW(a); break;
            case 4: i = (LBU(a) >> ((uu & 1) * 4)) & 15; c = 1; break;
            case 5: i = LBU(a); c = 1; break;
            case 6: i = LHU(a); c = 1; break;
            case 7: i = LW(a); c = 1; break;
            default: c = 0xFFFF00FFu; break;
            }
            if (fmt >= 4 && fmt <= 7)
                c = pal[(((i >> shift) & mask) | start) & 255];
            out[v * w + u] = c;
        }
}

/* -- vertices ----------------------------------------------------------------------------------------------------- */

/* A vertex after the vertex stage: screen position (pixels, framebuffer coordinates), depth (0-65535), 1/w for
 * perspective-correct interpolation, texture coordinates (texels), colour (0-255 a channel), fog (1 none, 0 all fog);
 * for the renderer's shadows, its position in the world (the camera taken out) and the part of its colour the sun gave
 * (0-255, in the colour) */
typedef struct { float x, y, z, rw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb, nx, ny, nz, mx, my, mz; } Vertex;

/* Before the viewport: clip-space position and the rest */
typedef struct { float cx, cy, cz, cw, u, v, r, g, b, a, fog, wx, wy, wz, sr, sg, sb, nx, ny, nz, mx, my, mz; } ClipVertex;
#define VERTEX_FLOATS 23

SAVED static float world[12], view[12], proj[16], bones[8][12];
/* Motion blur (render.c): a replayed draw's world and bones a frame shown earlier - its vertices' motion in the world
 * since (mx, my, mz) */
static float earlier_world[12], earlier_bones[8][12];
static int earlier;
int render_motion_wanted(void);     /* (the motion blur's, the anti-aliasing's) */
SAVED static int world_i, view_i, proj_i, bone_i;

/* Shadows (render_shadow.c): the GE's world space - which has the camera in it: the game multiplies it into each world
 * matrix - to the world, for the frame; and the light that is the sun among the GE's (-1: none known) */
static int shadow_world;            /* to_world is known this frame */
static float to_world[12], game_to_world[12];
static int sun_light = -1;
static int prim_world;              /* this primitive's vertices get their place in the world */
/* Lighting by the pixel (render_lights.c): the primitive's directional lights are left to the GPU - the vertex keeps the rest
 * of the lighting, its material's diffuse colour (in sr, sg, sb) and its normal in the world */
static int prim_pixel_light;
static uint32_t pixel_lights;       /* the lights (bits) the GPU does */
/* (SAVED: rt_cull_margin, which the game's run depends on, goes by whether the sun is known) */
SAVED static float sun_dir[3];      /* towards the sun, in the world (as last seen) */
SAVED static int sun_known;

int camera_game(float *inv, float *target);
void camera_invert(const float *v, float *out);
void render_shadow_frame(const float *light_vp, int on);
void render_prim_world(int on);
void render_prim_view(const float *screen_from_view, const float *world_from_view);
void camera_invert(const float *v, float *out);
void render_prim_lights(int n, const float *dirs, const float *colors, int sun);
void render_time(double seconds);
unsigned long present_frame_number(void);
double rt_game_30ths(void);         /* thread.c: the game's time in 30ths of a second; a frame's */
float rt_frame_30ths(void);

/* The shadow map covers this far each way from what the camera looks at (MHFU_SHADOW_DISTANCE), and this far towards
 * and away from the sun; render_shadow.c's size (MHFU_SHADOW_RESOLUTION) */
static float SHADOW_HALF = 3000.0f, SHADOW_RANGE = 6000.0f;
int render_shadow_size(void);
#define SHADOW_SIZE render_shadow_size()


static float ge_float(uint32_t data) {
    uint32_t u = data << 8;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int align_up(int x, int a) { return (x + a - 1) & ~(a - 1); }

typedef struct { int size, wo, to, co, no, po; } Layout;

/* The vertex format's layout: weights, texture coordinates, colour, normal, position, each aligned to its size */
static Layout layout_of(uint32_t vt) {
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

static Layout layout(void) { return layout_of(reg[0x12]); }

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
static float prev_inv[12], latest_inv[12];     /* (the last two frames' cameras undone: below) */
static int frames_late;
/* At the native frame rate (below): the frame drawn shown with another camera than the game's (the mouse's orbit or
 * raise: camera.c), the game's undone */
static int native_shown;
static float native_shown_view[12], native_game_inv[12];

static void shadow_frame(const float *shown) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = !getenv("MHFU_SHADOWS") || atoi(getenv("MHFU_SHADOWS")) != 0;
        if (getenv("MHFU_SHADOW_DISTANCE") && atof(getenv("MHFU_SHADOW_DISTANCE")) >= 500)
            SHADOW_HALF = (float)atof(getenv("MHFU_SHADOW_DISTANCE"));
        SHADOW_RANGE = fmaxf(6000.0f, 2.0f * SHADOW_HALF);
    }
    float c[3];
    shadow_world = enabled && camera_game(game_to_world, c);
    if (!shadow_world) {
        q_shadow_frame(NULL, 0);
        return;
    }
    if (shown) {
        camera_invert(shown, to_world);
        /* the lights are given in the space of the camera the frame was drawn with (a frame late, as told) */
        memcpy(game_to_world, native_shown ? native_game_inv : frames_late ? prev_inv : latest_inv, sizeof game_to_world);
    } else
        memcpy(to_world, game_to_world, sizeof to_world);
    if (!sun_known) {
        q_shadow_frame(NULL, 0);
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
    q_shadow_frame(m, 1);
}

/* Before a primitive's vertices: whether they are placed in the world (3D, through the camera), and which light is the
 * sun - the directional one giving most - its direction in the world noted for the next frame's shadows */
static void shadow_prim(void) {
    int through = (reg[0x12] >> 23) & 1;
    prim_world = shadow_world && !through && proj[11] != 0;
    q_prim_world(prim_world);
    if (prim_world) {
        /* the camera's space to the screen (by columns): the projection, then the viewport - x, y in PSP pixels and
         * the depth as drawn (0 to 1), over w - for what reads the depth back (ambient occlusion) */
        float sx = ge_float(reg[0x42]), sy = ge_float(reg[0x43]), sz = ge_float(reg[0x44]) / 65535.0f;
        float ox = ge_float(reg[0x45]) - (float)(reg[0x4C] & 0xFFFF) / 16.0f;
        float oy = ge_float(reg[0x46]) - (float)(reg[0x4D] & 0xFFFF) / 16.0f, oz = ge_float(reg[0x47]) / 65535.0f;
        float m[16];
        for (int c = 0; c < 4; c++) {
            const float *p = &proj[4 * c];
            m[4 * c + 0] = p[0] * sx + p[3] * ox;
            m[4 * c + 1] = p[1] * sy + p[3] * oy;
            m[4 * c + 2] = p[2] * sz + p[3] * oz;
            m[4 * c + 3] = p[3];
        }
        /* and the camera's space to the world: the view undone, then to_world */
        float inv[12], wv[12];
        camera_invert(view, inv);
        for (int c = 0; c < 4; c++) {
            float col[3];
            if (c < 3)
                for (int i = 0; i < 3; i++)
                    col[i] = to_world[i] * inv[3 * c] + to_world[3 + i] * inv[3 * c + 1] + to_world[6 + i] * inv[3 * c + 2];
            else
                mul43(to_world, &inv[9], col);
            memcpy(&wv[3 * c], col, sizeof col);
        }
        q_prim_view(m, wv);
    }
    sun_light = -1;
    prim_pixel_light = 0;
    pixel_lights = 0;
    if (through || !(reg[0x17] & 1)) {
        q_prim_lights(0, NULL, NULL, -1);
        return;
    }
    /* The sun: the directional light along the sun as known, if there is one - in the shade the game dims it, and a
     * light from the camera (from below the horizon, as like as not) gives more - else the one giving most, from above
     * the horizon (in the world) */
    float best = 0;
    int along = 0;
    for (int k = 0; k < 4 && !along; k++) {
        if (!(reg[0x18 + k] & 1) || ((reg[0x5F + k] >> 8) & 3) != 0)
            continue;
        uint32_t ld = reg[0x90 + 3 * k];
        float lum = (float)(ld & 255) + (float)((ld >> 8) & 255) + (float)((ld >> 16) & 255);
        if (!(lum > 0))
            continue;
        if (shadow_world) {
            float L[3] = {ge_float(reg[0x63 + 3 * k]), ge_float(reg[0x64 + 3 * k]), ge_float(reg[0x65 + 3 * k])}, w[3];
            mul33(game_to_world, L, w);
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (!(l > 0))
                continue;
            if (sun_known && (w[0] * sun_dir[0] + w[1] * sun_dir[1] + w[2] * sun_dir[2]) > 0.999f * l) {
                sun_light = k;
                along = 1;
                continue;
            }
            if (w[1] < 0.1f * l)
                continue;
        }
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
            q_prim_lights(n, dirs, cols, sun);
        } else
            pixel_lights = 0;
    }
    if (!prim_pixel_light)
        q_prim_lights(0, NULL, NULL, -1);
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

/* A vertex's position and colour as the game gave them */
static void read_position(uint32_t v, const Layout *l, uint32_t vt, float *p) {
    int pos = (vt >> 7) & 3, through = (vt >> 23) & 1;
    if (through && pos == 2) {          /* screen x, y (signed), depth (unsigned) */
        p[0] = (float)(int16_t)LHU(v + l->po);
        p[1] = (float)(int16_t)LHU(v + l->po + 2);
        p[2] = (float)LHU(v + l->po + 4);
    } else {
        static const int psz[4] = {0, 1, 2, 4};
        for (int i = 0; i < 3; i++)
            p[i] = read_comp(v + l->po + (uint32_t)(i * psz[pos]), pos, through, 1);
    }
}

static uint32_t read_color(uint32_t v, const Layout *l, uint32_t vt, const uint32_t *regs) {
    int col = (vt >> 2) & 7;
    if (col >= 4)
        return col == 7 ? LW(v + l->co) : to_rgba(LHU(v + l->co), col - 4);
    return (regs[0x55] & 0xFFFFFF) | ((regs[0x58] & 0xFF) << 24);     /* the material's ambient colour */
}

/* Vertices blended between frames (gpu_render_frame): the previous frame's match's registers, its vertex (the index of
 * the one blended with), how far along */
static const uint32_t *blend_reg;
static uint32_t blend_vertex;
static float blend_by;
#define SCRATCH_BLEND_VERTICES 0x08700000u
#define SCRATCH_BLEND_INDICES  0x08780000u

/* Decode vertex n and run it through the vertex stage up to clip space (through mode: straight to the screen, w 1) */
static void vertex(uint32_t base, int n, const Layout *l, ClipVertex *out, int *through_out) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, wt = (vt >> 9) & 3;
    int nweights = ((vt >> 14) & 7) + 1;
    int through = (vt >> 23) & 1;
    uint32_t v = base + (uint32_t)(n * l->size);
    *through_out = through;

    float p[3];
    read_position(v, l, vt, p);
    uint32_t c = read_color(v, l, vt, reg);
    if (blend_reg) {                    /* where it was, and its colour then, towards where it is */
        uint32_t bv = SCRATCH_BLEND_VERTICES + blend_vertex * (uint32_t)l->size;
        float q[3];
        read_position(bv, l, vt, q);
        for (int i = 0; i < 3; i++)
            p[i] = q[i] + (p[i] - q[i]) * blend_by;
        uint32_t d = read_color(bv, l, vt, blend_reg), mixed = 0;
        for (int k = 0; k < 32; k += 8) {
            float a = (float)((d >> k) & 255), b = (float)((c >> k) & 255);
            mixed |= (uint32_t)(a + (b - a) * blend_by + 0.5f) << k;
        }
        c = mixed;
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

    out->r = (float)(c & 255);
    out->g = (float)((c >> 8) & 255);
    out->b = (float)((c >> 16) & 255);
    out->a = (float)(c >> 24);
    out->wx = out->wy = out->wz = 0;
    out->sr = out->sg = out->sb = 0;
    out->nx = out->ny = out->nz = 0;
    out->mx = out->my = out->mz = 0;

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
    if (earlier && prim_world) {        /* where it was a frame shown earlier */
        float q[3] = {0, 0, 0}, em[3], ew[3];
        if (wt) {
            static const int tsz[4] = {0, 1, 2, 4};
            for (int b = 0; b < nweights; b++) {
                float w = read_comp(v + l->wo + (uint32_t)(b * tsz[wt]), wt, 0, 0);
                if (wt == 1) w = LBU(v + l->wo + (uint32_t)b) / 128.0f;
                if (wt == 2) w = LHU(v + l->wo + (uint32_t)(b * 2)) / 32768.0f;
                float t[3];
                mul43(earlier_bones[b], p, t);
                for (int i = 0; i < 3; i++)
                    q[i] += t[i] * w;
            }
        } else
            memcpy(q, p, sizeof q);
        mul43(earlier_world, q, em);
        mul43(to_world, em, ew);
        out->mx = ew[0];                /* (the motion, once its place now is known) */
        out->my = ew[1];
        out->mz = ew[2];
    }
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
    if (prim_world) {
        mul43(to_world, m, &out->wx);
        if (earlier) {
            out->mx = out->wx - out->mx;
            out->my = out->wy - out->my;
            out->mz = out->wz - out->mz;
        }
    }
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
    memcpy(&s->wx, &c->wx, 12 * sizeof(float));
}

/* -- the render queue: the renderer's half of the GE's work, on a thread of its own (render_queue.c) -------------
 *
 * What gpu.c tells the renderer goes into the queue in order - the primitive's state, its place in the world, lights,
 * and its vertices after the vertex stage, written into the queue as they are made - and the render thread does it:
 * the clipping, the renderer's batches and textures (as the registers given have them: its own copy), Vulkan. Off
 * (MHFU_RENDER_THREAD=0, or the software renderer), each is done at once, as before. */
enum { Q_BEGIN, Q_WORLD, Q_VIEW, Q_LIGHTS, Q_SHADOW_FRAME, Q_TIME, Q_OVERRIDE, Q_CLUT, Q_DRAW, Q_TL, Q_TL_STORE, Q_TL_SLOT };
typedef struct { uint32_t kind, a, b, c; } QHead;   /* (16 bytes: what follows stays aligned) */

static void gpu_run_command(const void *cmd, size_t n);

static int queued(void) {
    static int started;
    if (!started && render_on) {
        started = 1;
        render_queue_start(gpu_run_command);
    }
    return render_on && render_queue_on_flag;
}

static void *q_cmd(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, size_t n) {
    QHead *h = render_queue_reserve(sizeof *h + n);
    *h = (QHead){kind, a, b, c};
    return h + 1;
}

static void q_prim_world(int on) {
    if (!queued()) {
        render_prim_world(on);
        return;
    }
    q_cmd(Q_WORLD, (uint32_t)on, 0, 0, 0);
    render_queue_commit();
}

static void q_prim_view(const float *m, const float *wv) {
    if (!queued()) {
        render_prim_view(m, wv);
        return;
    }
    float *p = q_cmd(Q_VIEW, 0, 0, 0, 28 * sizeof(float));
    memcpy(p, m, 16 * sizeof(float));
    memcpy(p + 16, wv, 12 * sizeof(float));
    render_queue_commit();
}

static void q_prim_lights(int n, const float *dirs, const float *colors, int sun) {
    if (!queued()) {
        render_prim_lights(n, dirs, colors, sun);
        return;
    }
    float *p = q_cmd(Q_LIGHTS, (uint32_t)n, (uint32_t)sun, 0, (size_t)n * 8 * sizeof(float));
    memcpy(p, dirs, (size_t)n * 4 * sizeof(float));
    memcpy(p + 4 * n, colors, (size_t)n * 4 * sizeof(float));
    render_queue_commit();
}

static void q_shadow_frame(const float *m, int on) {
    if (!queued()) {
        render_shadow_frame(m, on);
        return;
    }
    float *p = q_cmd(Q_SHADOW_FRAME, (uint32_t)on, m != NULL, 0, 16 * sizeof(float));
    if (m)
        memcpy(p, m, 16 * sizeof(float));
    render_queue_commit();
}

static void q_time(double seconds) {
    if (!queued()) {
        render_time(seconds);
        return;
    }
    memcpy(q_cmd(Q_TIME, 0, 0, 0, sizeof seconds), &seconds, sizeof seconds);
    render_queue_commit();
}

static void q_override(uint32_t slot, int w, int h) {
    if (!queued()) {
        render_override_texture(slot, w, h);
        return;
    }
    q_cmd(Q_OVERRIDE, slot, (uint32_t)w, (uint32_t)h, 0);
    render_queue_commit();
}

static void q_clut(uint32_t addr, uint32_t bytes) {
    if (!queued()) {
        texpack_clut_loaded(addr, bytes);
        return;
    }
    q_cmd(Q_CLUT, addr, bytes, 0, 0);
    render_queue_commit();
}

/* The primitive's state (its registers: sent again only when they changed) */
static void q_begin(const uint32_t *r) {
    if (!queued()) {
        texpack_draw_v = draw_v;
        render_prim_begin(r);
        return;
    }
    static uint32_t sent[256];
    static int have;
    int same = have && !memcmp(sent, r, sizeof sent);
    uint32_t *p = q_cmd(Q_BEGIN, (uint32_t)draw_v, (uint32_t)same, 0, same ? 0 : sizeof sent);
    if (!same) {
        memcpy(p, r, sizeof sent);
        memcpy(sent, r, sizeof sent);
        have = 1;
    }
    render_queue_commit();
}

/* (the render thread's) The registers of the primitive being drawn */
static uint32_t rq_reg[256];

/* A clip-space vertex to the screen, as r's viewport has it */
static void to_screen_r(const ClipVertex *c, int through, Vertex *s, const uint32_t *r) {
    if (through) {
        s->x = c->cx;
        s->y = c->cy;
        s->z = c->cz;
        s->rw = 1.0f;
    } else {
        float rw = 1.0f / c->cw;
        s->x = c->cx * rw * ge_float(r[0x42]) + ge_float(r[0x45]) - (float)(r[0x4C] & 0xFFFF) / 16.0f;
        s->y = c->cy * rw * ge_float(r[0x43]) + ge_float(r[0x46]) - (float)(r[0x4D] & 0xFFFF) / 16.0f;
        s->z = c->cz * rw * ge_float(r[0x44]) + ge_float(r[0x47]);
        s->rw = rw;
    }
    s->u = c->u;
    s->v = c->v;
    s->r = c->r;
    s->g = c->g;
    s->b = c->b;
    s->a = c->a;
    s->fog = c->fog;
    memcpy(&s->wx, &c->wx, 12 * sizeof(float));
}

static float edge(float ax, float ay, float bx, float by, float px, float py);

/* A triangle to the renderer: cut by the near plane (the shadows have it whole), to the screen, culled as the GE
 * would where the GPU does not (clip_triangle and raster_triangle, the renderer's way, with the registers given) */
static void clip_render(const ClipVertex *v0, const ClipVertex *v1, const ClipVertex *v2, int through, const uint32_t *r,
                        int pw) {
    const float near = 1e-4f;
    Vertex s[4];
    int n = 0;
    if (through) {
        to_screen_r(v0, 1, &s[0], r);
        to_screen_r(v1, 1, &s[1], r);
        to_screen_r(v2, 1, &s[2], r);
        n = 3;
    } else {
        if (pw && (v0->cw <= near || v1->cw <= near || v2->cw <= near)) {
            RVertex rv[3];
            const ClipVertex *cv[3] = {v0, v1, v2};
            for (int i = 0; i < 3; i++) {
                memset(&rv[i], 0, sizeof rv[i]);
                rv[i].u = cv[i]->u;
                rv[i].v = cv[i]->v;
                rv[i].a = cv[i]->a;
                rv[i].wx = cv[i]->wx;
                rv[i].wy = cv[i]->wy;
                rv[i].wz = cv[i]->wz;
            }
            render_shadow_triangle(&rv[0], &rv[1], &rv[2]);
        }
        const ClipVertex *in[3] = {v0, v1, v2};
        ClipVertex out[4];
        for (int i = 0; i < 3; i++) {
            const ClipVertex *p = in[i], *q = in[(i + 1) % 3];
            int pin = p->cw > near, qin = q->cw > near;
            if (pin)
                out[n++] = *p;
            if (pin != qin) {
                float t = (near - p->cw) / (q->cw - p->cw);
                float *mf = &out[n].cx;
                const float *pf = &p->cx, *qf = &q->cx;
                for (int k = 0; k < VERTEX_FLOATS; k++)
                    mf[k] = pf[k] + (qf[k] - pf[k]) * t;
                n++;
            }
        }
        if (n < 3)
            return;
        for (int i = 0; i < n; i++)
            to_screen_r(&out[i], 0, &s[i], r);
    }
    for (int i = 1; i + 1 < n; i++) {
        const Vertex *a = &s[0], *b = &s[i], *c = &s[i + 1];
        float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
        if (area == 0)
            continue;
        if (!through && (r[0x1D] & 1) && !pw && (area < 0) == (int)(r[0x9B] & 1))
            continue;
        render_triangle((const RVertex *)a, (const RVertex *)b, (const RVertex *)c);
    }
}

/* A primitive's vertices (after the vertex stage) to the renderer */
static void emit_render(int type, int count, const ClipVertex *v, int through, const uint32_t *r, int pw) {
    switch (type) {
    case 3:
        for (int i = 0; i + 2 < count; i += 3)
            clip_render(&v[i], &v[i + 1], &v[i + 2], through, r, pw);
        break;
    case 4:
        for (int i = 0; i + 2 < count; i++)
            if (i & 1)
                clip_render(&v[i + 1], &v[i], &v[i + 2], through, r, pw);
            else
                clip_render(&v[i], &v[i + 1], &v[i + 2], through, r, pw);
        break;
    case 5:
        for (int i = 1; i + 1 < count; i++)
            clip_render(&v[0], &v[i], &v[i + 1], through, r, pw);
        break;
    case 6:
        for (int i = 0; i + 1 < count; i += 2) {
            Vertex a, b;
            to_screen_r(&v[i], through, &a, r);
            to_screen_r(&v[i + 1], through, &b, r);
            render_sprite((const RVertex *)&a, (const RVertex *)&b);
        }
        break;
    default:
        unhandled_prims[type]++;
        break;
    }
}

/* (the render thread) A command done */
static void gpu_run_command(const void *cmd, size_t n) {
    (void)n;
    const QHead *h = cmd;
    const void *p = h + 1;
    switch (h->kind) {
    case Q_BEGIN:
        if (!h->b)
            memcpy(rq_reg, p, sizeof rq_reg);
        texpack_draw_v = (int)h->a;
        render_prim_begin(rq_reg);
        break;
    case Q_WORLD:
        render_prim_world((int)h->a);
        break;
    case Q_VIEW:
        render_prim_view(p, (const float *)p + 16);
        break;
    case Q_LIGHTS:
        render_prim_lights((int)h->a, p, (const float *)p + 4 * h->a, (int)h->b);
        break;
    case Q_SHADOW_FRAME:
        render_shadow_frame(h->b ? p : NULL, (int)h->a);
        break;
    case Q_TIME: {
        double t;
        memcpy(&t, p, sizeof t);
        render_time(t);
        break;
    }
    case Q_OVERRIDE:
        render_override_texture(h->a, (int)h->b, (int)h->c);
        break;
    case Q_CLUT:
        texpack_clut_loaded(h->a, h->b);
        break;
    case Q_DRAW: {
        const uint32_t *d = p;          /* type, count, through, in the world; then the vertices */
        emit_render((int)d[0], (int)d[1], (const ClipVertex *)(d + 4), (int)d[2], rq_reg, (int)d[3]);
        break;
    }
    case Q_TL: {                        /* (a: type, and 256 if its draw is the last's; b: count; c: vertices; then
                                         * the feet's count - 16 bytes -, the draw, vertices, indices, feet) */
        const uint8_t *q = (const uint8_t *)p + 16;
        int nfeet = (int)*(const uint32_t *)p;
        const TlDraw *d = h->a & 256 ? NULL : (const TlDraw *)q;
        q += d ? sizeof *d : 0;
        const TlVertex *v = (const TlVertex *)q;
        const uint16_t *ix = (const uint16_t *)(v + h->c);
        const float *feet = (const float *)(q + sizeof *v * h->c + ((2 * h->b + 3) & ~3u));
        render_tl_draw((int)(h->a & 255), (int)h->b, d, v, (int)h->c, ix, feet, nfeet);
        break;
    }
    case Q_TL_STORE: {                  /* (a: slot; b: vertices; c: indices; then their first, the vertices, indices) */
        const uint32_t *at = p;
        const TlVertex *v = (const TlVertex *)(at + 4);
        render_tl_store((int)h->a, at[0], v, (int)h->b, at[1], (const uint32_t *)(v + h->b), (int)h->c);
        break;
    }
    case Q_TL_SLOT: {                   /* (a: slot, and 256 if its draw is the last's; b: first index; c: indices; then
                                         * the feet's count - 16 bytes -, the draw, the feet) */
        const uint8_t *q = (const uint8_t *)p + 16;
        int nfeet = (int)*(const uint32_t *)p;
        const TlDraw *d = h->a & 256 ? NULL : (const TlDraw *)q;
        render_tl_slot_draw((int)(h->a & 255), d, h->b, h->c, (const float *)(q + (d ? sizeof *d : 0)), nfeet);
        break;
    }
    }
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
    if (render_on && prim_world && (v0->cw <= near || v1->cw <= near || v2->cw <= near)) {
        /* cut by the near plane: the shadows have it whole (they need only its place in the world) */
        RVertex r[3];
        const ClipVertex *cv[3] = {v0, v1, v2};
        for (int i = 0; i < 3; i++) {
            memset(&r[i], 0, sizeof r[i]);
            r[i].u = cv[i]->u;
            r[i].v = cv[i]->v;
            r[i].a = cv[i]->a;
            r[i].wx = cv[i]->wx;
            r[i].wy = cv[i]->wy;
            r[i].wz = cv[i]->wz;
        }
        render_shadow_triangle(&r[0], &r[1], &r[2]);
    }
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

uint32_t gpu_cmd_at;                /* the command being run's address (ge.c) */
static int draw_hud;                /* the HUD's part the draw being made is (hud_scaled): by its command, or recorded */
static int draw_far;                /* the draw being made is placed for the camera shown (FAR_ below), or 0 */
static int world_far;               /* the world matrix in the GE is such a draw's: by the command loading it */

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
    int group;                      /* its group for the GPU's vertex stage (below), or -1 */
    int hud;                        /* the HUD's part it is (draw_hud) */
    int far;                        /* placed for the camera shown (draw_far) */
} Draw;

/* Draws in a row the GPU's vertex stage can draw as one, replayed (MHFU_GPU_TL): the same state, matrices and texture -
 * their vertices sent once, as the frame is recorded, into its slot; their triangles' indices there from index */
typedef struct {
    int first, n;                   /* its draws */
    uint32_t index, nindex;
    uint32_t vertex, nvertex;       /* (its vertices: for tessellation's shape normals) */
    int feet, nfeet;                /* (lit: its lowest vertices, where they are in tl_feet; -1 not found yet) */
} TlGroup;

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
    /* (for matching: each draw's key and position, kept apart from the draws - and the draws in order of key, sorted
     * once, as the frame is first matched) */
    uint64_t *keys;
    float (*pos)[3];
    int *order, kcap, sorted;
    /* (the GPU's vertex stage) its groups; their vertices and triangles' indices, sent into its slot (its
     * generation's) as it becomes the latest; none more once they do not fit */
    TlGroup *tlg;
    int ntlg, tlgcap, tl_full;
    TlVertex *tlv;
    uint32_t *tli, *tl_feet;
    uint32_t tl_nv, tl_ni, tlv_cap, tli_cap, tl_nfeet, tl_feet_cap;
    unsigned tl_gen;
} Frame;

static const TlGroup *replay_group;     /* (a group being replayed as one: its first draw's replay draws it) */
static unsigned long tl_groups_drawn, tl_group_draws;     /* (for MHFU_PRIM_STATS) */

static int recording;               /* primitives are recorded, not drawn */
static const Draw *held_texture;    /* (a kept draw being drawn again: gpu_held_draw) */
/* What gated code drew at the last 30th, kept for the frames between (gpu_held_draw): its chunks' commands in order,
 * each a command or a primitive (its draw in held) */
typedef struct { uint32_t cmd, addr; int chunk, draw; } HeldCommand;
static Frame held;
static HeldCommand *held_cmds;
static int held_n, held_cap;
static int held_chunk = -1;         /* the gated chunk the commands come from, or -1 */
static int held_drawing;

static void held_command(uint32_t cmd, uint32_t addr, int draw) {
    if (held_n == held_cap) {
        held_cap = held_cap ? held_cap * 2 : 4096;
        held_cmds = realloc(held_cmds, sizeof *held_cmds * (size_t)held_cap);
    }
    held_cmds[held_n++] = (HeldCommand){cmd, addr, held_chunk, draw};
}
static int in_replay;               /* drawing a recorded frame (gpu_render_frame) */

/* MHFU_MOTION_LOG=file: each skinned draw in the world as drawn - when (us), its key, where its root lands on the screen
 * (-1 to 1) - for measuring how evenly things move in the frames shown, whatever draws them */
int fast_forward(void);
static int motion_log_on = -1;
static FILE *motion_file;
static uint64_t replay_key;             /* (a recorded draw being drawn again: its key, as recorded) */
static void motion_log(uint64_t key) {
    float p[3], v[3];
    mul43(world, &bones[0][9], p);
    mul43(view, p, v);
    float x = proj[0] * v[0] + proj[4] * v[1] + proj[8] * v[2] + proj[12];
    float y = proj[1] * v[0] + proj[5] * v[1] + proj[9] * v[2] + proj[13];
    float w = proj[3] * v[0] + proj[7] * v[1] + proj[11] * v[2] + proj[15];
    if (w > 0.01f)
        fprintf(motion_file, "%llu %016llx %.5f %.5f\n", (unsigned long long)time_us(), (unsigned long long)key, x / w,
                y / w);
}

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

static uint64_t draw_key(const Draw *d);
static void draw_position(const Draw *d, float *p);

static void record(Frame *rec, uint32_t data, const Layout *l) {
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
    d->far = draw_far;
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
    d->group = -1;
    d->hud = draw_hud;
    d->tex_slot = 0xFFFFFFFFu;
    if (held_texture) {                 /* a kept draw drawn again: the texture it had (since written over, perhaps) */
        d->tex_slot = held_texture->tex_slot;
        d->tex_w = held_texture->tex_w;
        d->tex_h = held_texture->tex_h;
    } else if (render_on && !(reg[0xD3] & 1) && (reg[0x1E] & 1))
        d->tex_slot = render_texture(reg, &d->tex_w, &d->tex_h);
    if (rec->n > rec->kcap) {
        rec->kcap = rec->cap;
        rec->keys = realloc(rec->keys, sizeof *rec->keys * (size_t)rec->kcap);
        rec->pos = realloc(rec->pos, sizeof *rec->pos * (size_t)rec->kcap);
        rec->order = realloc(rec->order, sizeof *rec->order * (size_t)rec->kcap);
    }
    rec->keys[rec->n - 1] = draw_key(d);
    draw_position(d, rec->pos[rec->n - 1]);
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

static void tl_send(Frame *f);

void gpu_frame_end(uint32_t display_addr, uint64_t time) {
    close_run(rec);
    tl_send(rec);
    Frame *old = previous;
    previous = latest;
    latest = rec;
    rec = old;
    rec->n = 0;
    rec->nruns = 0;
    rec->used = 0;
    rec->sorted = 0;
    static unsigned tl_gen_next;
    rec->ntlg = rec->tl_full = 0;
    rec->tl_nv = rec->tl_ni = rec->tl_nfeet = 0;
    rec->tl_gen = ++tl_gen_next;
    latest->display_addr = display_addr;
    latest->time = time;
    frame_generation++;
}

static void lerp(float *out, const float *a, const float *b, int n, float t) {
    for (int i = 0; i < n; i++)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

/* A 3 x 4 matrix (by columns) as a rotation, the scale of each axis (negative: mirrored) and a translation; 0 if it is
 * not that (an axis of no length, sheared) */
static int to_rotation(const float *m, float *q, float *s) {
    float c[9];
    for (int j = 0; j < 3; j++) {
        s[j] = sqrtf(m[3 * j] * m[3 * j] + m[3 * j + 1] * m[3 * j + 1] + m[3 * j + 2] * m[3 * j + 2]);
        if (s[j] < 1e-6f)
            return 0;
        for (int i = 0; i < 3; i++)
            c[3 * j + i] = m[3 * j + i] / s[j];
    }
    for (int j = 0; j < 3; j++) {
        const float *a = &c[3 * j], *b = &c[3 * ((j + 1) % 3)];
        if (fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.05f)
            return 0;
    }
    float det = c[0] * (c[4] * c[8] - c[5] * c[7]) - c[3] * (c[1] * c[8] - c[2] * c[7]) +
                c[6] * (c[1] * c[5] - c[2] * c[4]);
    if (det < 0) {          /* mirrored: the first axis's scale negative, the rest a rotation */
        s[0] = -s[0];
        c[0] = -c[0], c[1] = -c[1], c[2] = -c[2];
    }
    /* (R(i, j) = c[3 j + i]) */
    float tr = c[0] + c[4] + c[8];
    if (tr > 0) {
        float k = 0.5f / sqrtf(tr + 1);
        q[0] = 0.25f / k, q[1] = (c[5] - c[7]) * k, q[2] = (c[6] - c[2]) * k, q[3] = (c[1] - c[3]) * k;
    } else if (c[0] > c[4] && c[0] > c[8]) {
        float k = 2 * sqrtf(1 + c[0] - c[4] - c[8]);
        q[0] = (c[5] - c[7]) / k, q[1] = 0.25f * k, q[2] = (c[3] + c[1]) / k, q[3] = (c[6] + c[2]) / k;
    } else if (c[4] > c[8]) {
        float k = 2 * sqrtf(1 + c[4] - c[0] - c[8]);
        q[0] = (c[6] - c[2]) / k, q[1] = (c[3] + c[1]) / k, q[2] = 0.25f * k, q[3] = (c[7] + c[5]) / k;
    } else {
        float k = 2 * sqrtf(1 + c[8] - c[0] - c[4]);
        q[0] = (c[1] - c[3]) / k, q[1] = (c[6] + c[2]) / k, q[2] = (c[7] + c[5]) / k, q[3] = 0.25f * k;
    }
    return 1;
}

/* Between two placements (3 x 4, by columns): turning by the shortest way, their scales and translations lerped -
 * lerped as they are, a half turn would pass through a matrix that flattens what it places. Matrices that are not
 * rotations and scales (or mirror differently) are lerped as they are. */
static void interp43(float *out, const float *a, const float *b, float t) {
    /* (turning little, lerped as they are: it shrinks them by a part in 8 of the angle squared - nothing) */
    float most = 0, change = 0;
    for (int i = 0; i < 9; i++) {
        most = fmaxf(most, fabsf(a[i]));
        change = fmaxf(change, fabsf(b[i] - a[i]));
    }
    float qa[4], qb[4], sa[3], sb[3];
    if (change <= 0.02f * most || !to_rotation(a, qa, sa) || !to_rotation(b, qb, sb) || (sa[0] < 0) != (sb[0] < 0)) {
        lerp(out, a, b, 12, t);
        return;
    }
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) {
        d = -d;
        for (int i = 0; i < 4; i++)
            qb[i] = -qb[i];
    }
    float wa = 1 - t, wb = t;
    if (d < 0.9995f) {
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1 - t) * th) / sn;
        wb = sinf(t * th) / sn;
    }
    float q[4], n = 0;
    for (int i = 0; i < 4; i++) {
        q[i] = wa * qa[i] + wb * qb[i];
        n += q[i] * q[i];
    }
    n = 1 / sqrtf(n);
    float w = q[0] * n, x = q[1] * n, y = q[2] * n, z = q[3] * n;
    float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y),
                  2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x),
                  2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
    for (int j = 0; j < 3; j++) {
        float s = sa[j] + (sb[j] - sa[j]) * t;
        for (int i = 0; i < 3; i++)
            out[3 * j + i] = r[3 * j + i] * s;
    }
    lerp(&out[9], &a[9], &b[9], 3, t);
}

static void compose43(const float *a, const float *b, float *out);

/* The inverse of a 3 x 4 placement; 0 if it has none */
static int invert43(const float *m, float *out) {
    float c0 = m[4] * m[8] - m[5] * m[7], c1 = m[2] * m[7] - m[1] * m[8], c2 = m[1] * m[5] - m[2] * m[4];
    float det = m[0] * c0 + m[3] * c1 + m[6] * c2;
    if (fabsf(det) < 1e-12f)
        return 0;
    float k = 1 / det, r[12];
    r[0] = c0 * k, r[1] = c1 * k, r[2] = c2 * k;
    r[3] = (m[5] * m[6] - m[3] * m[8]) * k, r[4] = (m[0] * m[8] - m[2] * m[6]) * k, r[5] = (m[2] * m[3] - m[0] * m[5]) * k;
    r[6] = (m[3] * m[7] - m[4] * m[6]) * k, r[7] = (m[1] * m[6] - m[0] * m[7]) * k, r[8] = (m[0] * m[4] - m[1] * m[3]) * k;
    for (int i = 0; i < 3; i++)
        r[9 + i] = -(r[i] * m[9] + r[3 + i] * m[10] + r[6 + i] * m[11]);
    memcpy(out, r, sizeof r);
    return 1;
}

/* Between two frames' bones (n): the first by interp43, the rest as placed from it - a model turning about its root
 * turns its bones' places with it, rather than drawing them in through the middle */
static void interp_bones(float *out, const float *a, const float *b, int n, float t) {
    float ia[12], ib[12];
    if (n < 2 || !invert43(a, ia) || !invert43(b, ib)) {
        for (int k = 0; k < n; k++)
            interp43(out + 12 * k, a + 12 * k, b + 12 * k, t);
        return;
    }
    float first[12];
    interp43(first, a, b, t);
    for (int k = 1; k < n; k++) {
        float la[12], lb[12], l[12];
        compose43(ia, a + 12 * k, la);
        compose43(ib, b + 12 * k, lb);
        interp43(l, la, lb, t);
        compose43(first, l, out + 12 * k);
    }
    memcpy(out, first, sizeof first);
}

/* Between two frames' world matrix and bones (n, 0: not skinned): the bones as placed in the world (the world matrix
 * times each) by interp_bones - a turn the game puts in the world matrix and takes back in the bones (as a model turns
 * about) then cancels as it does in the frames - and back through the world matrix lerped by interp43 */
static void interp_model(float *world_out, float *bones_out, const float *wa, const float *wb, const float *ba,
                         const float *bb, int n, float t) {
    interp43(world_out, wa, wb, t);
    float inv[12];
    if (!n)
        return;
    if (!invert43(world_out, inv)) {
        interp_bones(bones_out, ba, bb, n, t);
        return;
    }
    float ca[8 * 12], cb[8 * 12];
    for (int k = 0; k < n; k++) {
        compose43(wa, ba + 12 * k, ca + 12 * k);
        compose43(wb, bb + 12 * k, cb + 12 * k);
    }
    interp_bones(bones_out, ca, cb, n, t);
    for (int k = 0; k < n; k++)
        compose43(inv, bones_out + 12 * k, bones_out + 12 * k);
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
static const Draw *blend_from;      /* a draw's match in the previous frame, its vertices blended in (vertex()) */
static float blend_t;
static uint8_t *loose_near;
static int through_draw(const Draw *d);

/* Replay a draw of the latest frame; with its match in the previous (or NULL), its matrices interpolated by t */
static int draw_bones(const Draw *d);

/* A GE float register's value (its 24 bits: a float's top) */
static uint32_t to_ge_float(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u >> 8;
}

static uint64_t draw_key(const Draw *d);

/* The click inspector's picking (below): going over a recorded frame's draws, their vertices noted rather than drawn */
static int pick_on, pick_pass, pick_wanted, pick_collecting;
static float pick_view[12], pick_geview[12], pick_proj[16], pick_vp[8];
static int pick_camera_ok;
static void pick_begin(void);
static void pick_end(uint32_t display_addr);
static void pick_camera_recorded(void);
static void pick_set_source(uint32_t vaddr);

/* -- what the stage places by the camera -----------------------------------------------------------------------------
 *
 * The light shafts (StageRampedGroup, vtable 089BE310: tick game_sub 09CBB278, draw 09CBADC8) have no place in the world.
 * Each tick the game puts one FAR_DISTANCE from its eye towards the stage's point +0x3A4, and draws it turned as its
 * camera is, so that it is seen in that direction wherever the eye is. The glare (StageCameraFacingGlare, 089BE3B0: tick
 * 09CBDB88, draw 09CBD2A8) it puts GLARE_DISTANCE straight ahead of its eye, turned the same, filling the picture.
 * Taken for things in the world, with another camera shown than the game's (camera_distance, the mouse's raise and
 * orbit, free look), they are points near the game's eye seen from another place: the shaft near the ground in front of
 * the camera, moving the wrong way; the glare a small rectangle round the hunter.
 * So each is placed as the game places it, for the camera shown: its rotation as the game gave it (the game's matrix has
 * its camera undone in it, so this is the camera's own, whichever camera), its place as above from the eye shown. They
 * still follow the eye and are drawn over everything, as in the game. The game's memory is not touched.
 * Told apart by the draw's code (patches.py FAR_DRAW): what it adds to the display list holds its world matrix (its
 * primitives are the model's own, called from there). The GE runs those commands a frame later (the lists are
 * double-buffered, see hud_command). */
#define FAR_SHAFT 1
#define FAR_GLARE 2
#define FAR_DISTANCE 150.0f         /* (the game's: 09CBB41C) */
#define GLARE_DISTANCE 40.0f        /* (the game's: 09CBDC28) */
static struct { uint32_t start, end; unsigned long frame; int kind; } far_draws[16];
static int far_next;
static unsigned long far_last;
static float far_point[3];

void rt_far_draw_begin(int kind) {
    far_draws[far_next].kind = kind;
    far_draws[far_next].start = LW(0x089CC120u) & 0x0FFFFFFFu;  /* (the display list's end) */
    far_draws[far_next].end = 0;
    far_last = far_draws[far_next].frame = present_frame_number();
    uint32_t stage = LW(LW(0x09A4F1A0u));
    memcpy(far_point, PTR(stage + 0x3A4u), sizeof far_point);
}

void rt_far_draw_end(void) {
    far_draws[far_next].end = LW(0x089CC120u) & 0x0FFFFFFFu;
    far_next = (far_next + 1) % 16;
}

static int far_command(uint32_t at) {
    at &= 0x0FFFFFFFu;
    unsigned long now = present_frame_number();
    if (far_last + 1 < now)
        return 0;
    for (int i = 0; i < 16; i++)
        if (far_draws[i].frame + 1 == now && at >= far_draws[i].start && at < far_draws[i].end)
            return far_draws[i].kind;
    return 0;
}

/* world (the game's rotation in it): placed for the camera cam */
static void far_place(int kind, const float *cam) {
    if (kind == FAR_GLARE) {
        world[9] = world[10] = 0;
        world[11] = -GLARE_DISTANCE;
        return;
    }
    float s[3];
    mul43(cam, far_point, s);
    float k = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (k < 1)
        return;
    for (int i = 0; i < 3; i++)
        world[9 + i] = FAR_DISTANCE * s[i] / k;
}
/* How far the frame shown now is from the one shown before, in the game's frames (t's unit): their time on the screen,
 * not a nominal one, as the camera's motion is between those two. 0: no motion wanted */
static float shown_step;

static void replay(const Draw *d, const Draw *prev, const Draw *moved, float t) {
    memcpy(reg, d->reg, sizeof reg);
    /* a texture scrolled (water, clouds): its offset interpolated too - unless it jumped (a frame of a sprite sheet,
     * wrapping round) */
    if (prev && !through_draw(d) && (d->reg[0x4A] != prev->reg[0x4A] || d->reg[0x4B] != prev->reg[0x4B]))
        for (int k = 0x4A; k <= 0x4B; k++) {
            float a = ge_float(prev->reg[k]), b = ge_float(d->reg[k]);
            if (fabsf(b - a) < 0.1f)
                reg[k] = to_ge_float(a + (b - a) * t);
        }
    /* the hunter, monsters and people (lit by the GE, in the world, matched): where they were a frame shown earlier */
    float step = shown_step;
    earlier = 0;
    if (step > 0 && moved && native_camera && d->proj[11] != 0 && (d->reg[0x17] & 1) && !d->far) {
        int late = stale_known && stale[d - latest->draws];
        float wl[12], wp[12], we[12];
        compose43(late ? prev_inv : latest_inv, d->world, wl);
        compose43(late ? before_inv : prev_inv, moved->world, wp);
        interp_model(we, &earlier_bones[0][0], wp, wl, &moved->bones[0][0], &d->bones[0][0], draw_bones(d), t - step);
        compose43(shown_view, we, earlier_world);
        earlier = 1;
    }
    if (native_camera && d->proj[11] != 0) {
        /* the game multiplied its camera into the world matrix: take each frame's out, interpolate in the world, and
         * put the camera shown in */
        float wl[12];
        int late = stale_known && stale[d - latest->draws];     /* placed with the camera a frame late */
        compose43(late ? prev_inv : latest_inv, d->world, wl);
        if (prev) {
            float wp[12];
            compose43(late ? before_inv : prev_inv, prev->world, wp);
            interp_model(world, &bones[0][0], wp, wl, &prev->bones[0][0], &d->bones[0][0], draw_bones(d), t);
            lerp(view, prev->view, d->view, 12, t);
            lerp(proj, prev->proj, d->proj, 16, t);
        } else {
            memcpy(world, wl, sizeof world);
            memcpy(view, d->view, sizeof view);
            memcpy(proj, d->proj, sizeof proj);
            memcpy(bones, d->bones, sizeof(float) * 12 * (size_t)draw_bones(d));
        }
        compose43(shown_view, world, world);
        if (d->far) {
            memcpy(world, d->world, sizeof world);
            far_place(d->far, shown_view);
        }
    } else if (prev) {
        interp_model(world, &bones[0][0], prev->world, d->world, &prev->bones[0][0], &d->bones[0][0], draw_bones(d), t);
        lerp(view, prev->view, d->view, 12, t);
        lerp(proj, prev->proj, d->proj, 16, t);
    } else {
        memcpy(world, d->world, sizeof world);
        memcpy(view, d->view, sizeof view);
        memcpy(proj, d->proj, sizeof proj);
        memcpy(bones, d->bones, sizeof(float) * 12 * (size_t)draw_bones(d));
    }
    const Frame *f = latest;
    if (d->run != placed_run && !replay_group) {     /* (a group's vertices are the GPU's already) */
        const Run *r = &f->runs[d->run];
        memcpy(PTR(SCRATCH_VERTICES + r->lo), f->bytes + r->copy, r->hi - r->lo);
        placed_run = d->run;
    }
    if (d->ibytes && !replay_group)
        memcpy(PTR(SCRATCH_INDICES), f->bytes + d->icopy, d->ibytes);
    vaddr = SCRATCH_VERTICES;
    iaddr = SCRATCH_INDICES;
    blend_from = prev && loose_near[d - latest->draws] ? prev : NULL;
    blend_t = t;
    if (!pick_pass)
        q_override(d->tex_slot, d->tex_w, d->tex_h);
    replay_key = draw_key(d);
    draw_hud = d->hud;
    prim(d->data);
    if (!pick_pass)
        q_override(0xFFFFFFFEu, 0, 0);
    blend_from = NULL;
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
/* In the world (the camera taken out) only things' own moves count: a big animal's limbs move 60 units a frame as it
 * flinches - the copies of a model (a herd) stand further apart than that */
#define MATCH_DISTANCE_WORLD 150.0f
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

/* A frame's draws in order of key (once) */
static void sort_frame(Frame *f) {
    if (f->sorted)
        return;
    f->sorted = 1;
    const uint64_t *keys = f->keys;
    int *order = f->order;
    for (int i = 0; i < f->n; i++)
        order[i] = i;
    /* insertion sort is too slow for thousands: a simple bottom-up merge sort on order[] by keys[] */
    static int *spare;
    static int spare_cap;
    if (f->n > spare_cap) {
        spare_cap = f->n * 2;
        spare = realloc(spare, sizeof *spare * (size_t)spare_cap);
    }
    int *tmp = spare;
    for (int w = 1; w < f->n; w *= 2) {
        for (int lo = 0; lo < f->n; lo += 2 * w) {
            int mid = lo + w < f->n ? lo + w : f->n, hi = lo + 2 * w < f->n ? lo + 2 * w : f->n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = keys[order[i]] <= keys[order[j]] ? order[i++] : order[j++];
            while (i < mid) tmp[k++] = order[i++];
            while (j < hi) tmp[k++] = order[j++];
        }
        int *t = order; order = tmp; tmp = t;
    }
    if (order != f->order)
        memcpy(f->order, order, sizeof *order * (size_t)f->n);
}

/* Where a key's draws start in a sorted frame */
static int key_start(const Frame *f, uint64_t k) {
    int lo = 0, hi = f->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (f->keys[f->order[mid]] < k) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* How many of the latest frame's draws have a key */
static int latest_count(uint64_t k) {
    int lo = key_start(latest, k), n = 0;
    while (lo + n < latest->n && latest->keys[latest->order[lo + n]] == k)
        n++;
    return n;
}

/* -- what the game writes the vertices of -------------------------------------------------------------------------
 * The HUD, the menus, the text and fades (2D: in screen space), and particles (3D): drawn from vertices the game writes
 * each frame into the frame's display list or a ring of buffers - a different place each frame - so not matched by where
 * they are, but by what they are (vertex format, primitive, count, texture) in the order they are drawn: the next of
 * the kind if it shows the same picture (texture coordinates) nearby, else the nearest with that picture, else the next
 * of the kind nearby. Matched, their matrices are interpolated as any draw's, and their vertices blended (vertex()):
 * the 2D's corners and colours (a fade), the game's own grass. */
#define LOOSE_2D_DISTANCE 64.0f     /* PSP pixels a corner may move in a frame */
#define LOOSE_BEHIND 32             /* how far from the order drawn a match is looked for (windows come in any order) */
#define LOOSE_AHEAD 64

typedef struct { float box[4], at[3]; uint32_t uvs; int ok; } Shape;

static int through_draw(const Draw *d) { return (d->reg[0x12] >> 23) & 1; }

/* A draw's vertices as recorded: their box on the screen (2D), or their middle (3D, in the model), and what they show
 * (a hash of their texture coordinates) */
static Shape shape(const Frame *f, const Draw *d) {
    Shape s = {{1e30f, 1e30f, -1e30f, -1e30f}, {0, 0, 0}, 2166136261u, 0};
    uint32_t vt = d->reg[0x12];
    Layout l = layout_of(vt);
    int tc = vt & 3, pos = (vt >> 7) & 3, idx = (vt >> 11) & 3, count = d->data & 0xFFFF, through = (vt >> 23) & 1;
    static const int sz[4] = {0, 1, 2, 4};
    const Run *r = &f->runs[d->run];
    for (int i = 0; i < count; i++) {
        int n = i;
        if (idx == 1) n = f->bytes[d->icopy + (size_t)i];
        else if (idx == 2) { uint16_t x; memcpy(&x, f->bytes + d->icopy + 2 * (size_t)i, 2); n = x; }
        uint32_t at = (uint32_t)(n * l.size);
        if (at < r->lo || at + (uint32_t)l.size > r->hi)
            return s;
        const uint8_t *v = f->bytes + r->copy + (at - r->lo);
        float p[3];
        for (int k = 0; k < 3; k++) {
            const uint8_t *c = v + l.po + k * sz[pos];
            if (pos == 1) p[k] = through ? (float)(int8_t)c[0] : (float)(int8_t)c[0] / 127.0f;
            else if (pos == 2) { int16_t x; memcpy(&x, c, 2); p[k] = through ? (float)x : (float)x / 32767.0f; }
            else memcpy(&p[k], c, 4);
            s.at[k] += p[k] / (float)count;
        }
        if (p[0] < s.box[0]) s.box[0] = p[0];
        if (p[1] < s.box[1]) s.box[1] = p[1];
        if (p[0] > s.box[2]) s.box[2] = p[0];
        if (p[1] > s.box[3]) s.box[3] = p[1];
        for (int k = 0; k < 2 * sz[tc]; k++)
            s.uvs = (s.uvs ^ v[l.to + k]) * 16777619u;
    }
    s.ok = count > 0;
    return s;
}

/* The same kind of draw: vertex format, primitive and count, texture */
static int same_kind(const Draw *a, const Draw *b) {
    if ((a->reg[0x12] ^ b->reg[0x12]) & 0xFFFFFF || (a->data ^ b->data) & 0x7FFFFF || (a->reg[0x1E] ^ b->reg[0x1E]) & 1)
        return 0;
    return !(a->reg[0x1E] & 1) ||
           (!((a->reg[0xA0] ^ b->reg[0xA0]) & 0xFFFFFF) && !((a->reg[0xA8] ^ b->reg[0xA8]) & 0xFF0000));
}

/* How far a draw moved between frames, against how far it may (below 1: near): in 2D the most any side of its box
 * moved, in 3D its middle in the world (its world matrix, and the frame's camera taken out of it) */
static float apart(const Draw *d, const Shape *a, const Draw *e, const Shape *b, int in_world) {
    if (through_draw(d)) {
        float m = 0;
        for (int k = 0; k < 4; k++)
            m = fmaxf(m, fabsf(a->box[k] - b->box[k]));
        return m / LOOSE_2D_DISTANCE;
    }
    float p[3], q[3];
    mul43(d->world, a->at, p);
    mul43(e->world, b->at, q);
    if (in_world) {
        to_frame_world(latest_inv, p);
        to_frame_world(prev_inv, q);
        return sqrtf(dist2(p, q)) / MATCH_DISTANCE_WORLD;
    }
    return sqrtf(dist2(p, q)) / (MATCH_DISTANCE + MATCH_TURN * camera_distance(d, p));
}

static uint8_t *loose_near;         /* for each draw of the latest frame: its vertices blend with its match's */

/* The draws of one kind (2D or 3D) not matched where they are, matched by what they are */
static void match_loose(int *m, uint8_t *taken, int through, int in_world) {
    static int *pick, pcap;
    static Shape *pshape;
    if (previous->n > pcap) {
        pcap = previous->n * 2;
        pick = realloc(pick, sizeof *pick * (size_t)pcap);
        pshape = realloc(pshape, sizeof *pshape * (size_t)pcap);
    }
    int np = 0;
    for (int j = 0; j < previous->n; j++)
        if (!taken[j] && through_draw(&previous->draws[j]) == through && (through || !draw_bones(&previous->draws[j])))
            pick[np++] = j;
    if (!np)
        return;
    for (int k = 0; k < np; k++)
        pshape[k].ok = -1;              /* (worked out when first needed) */
    int cursor = 0;
    for (int i = 0; i < latest->n; i++) {
        const Draw *d = &latest->draws[i];
        if (through_draw(d) != through || (!through && draw_bones(d)))
            continue;
        if (m[i] >= 0) {                /* matched where it is: blended if it changed (2D: the HUD's own vertices) */
            if (through) {
                const Draw *e = &previous->draws[m[i]];
                Shape a = shape(latest, d), b = shape(previous, e);
                loose_near[i] = a.ok && b.ok && apart(d, &a, e, &b, in_world) < 1;
            }
            continue;
        }
        Shape sh = shape(latest, d);
        if (!sh.ok)
            continue;
        int lo = cursor - LOOSE_BEHIND > 0 ? cursor - LOOSE_BEHIND : 0;
        int hi = cursor + LOOSE_AHEAD < np ? cursor + LOOSE_AHEAD : np;
        int best = -1, first = -1;
        float best_d = 1;
        /* the next in order first (copies of one picture - blades of grass - may stand nearer each other than they
         * move) */
        int next = cursor;
        while (next < hi && (taken[pick[next]] || !same_kind(d, &previous->draws[pick[next]])))
            next++;
        if (next < hi) {
            if (pshape[next].ok < 0)
                pshape[next] = shape(previous, &previous->draws[pick[next]]);
            if (pshape[next].ok && pshape[next].uvs == sh.uvs &&
                apart(d, &sh, &previous->draws[pick[next]], &pshape[next], in_world) < 1)
                best = next;
        }
        for (int k = lo; k < hi && best != next; k++) {
            const Draw *e = &previous->draws[pick[k]];
            if (taken[pick[k]] || !same_kind(d, e))
                continue;
            if (pshape[k].ok < 0)
                pshape[k] = shape(previous, e);
            if (!pshape[k].ok)
                continue;
            float a = apart(d, &sh, e, &pshape[k], in_world);
            if (a >= 1)
                continue;
            if (pshape[k].uvs == sh.uvs && a < best_d) {
                best_d = a;
                best = k;
            }
            if (first < 0 && k >= cursor)
                first = k;
        }
        if (best < 0)
            best = first;
        if (best < 0)
            continue;
        m[i] = pick[best];
        taken[pick[best]] = 1;
        loose_near[i] = 1;
        cursor = best + 1;
    }
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
    /* the frames' draws by key, to find a key's group by binary search */
    sort_frame(previous);
    sort_frame(latest);
    const uint64_t *keys = previous->keys;
    const int *order = previous->order;
    for (int i = 0; i < latest->n; i++) {
        const Draw *d = &latest->draws[i];
        uint64_t k = latest->keys[i];
        int lo = key_start(previous, k);
        float p[3], p_late[3];
        memcpy(p, latest->pos[i], sizeof p);
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
                    memcpy(q, previous->pos[m[i]], sizeof q);
                    memcpy(q_late, q, sizeof q);
                    to_frame_world(prev_inv, q);
                    to_frame_world(before_inv, q_late);
                    vote(i, dist2(p, q), dist2(p_late, q_late));
                }
            }
            continue;
        }
        /* in the world the camera's turn moves nothing: only the distance (as seen, things far off sweep further) */
        float reach = in_world ? MATCH_DISTANCE_WORLD : MATCH_DISTANCE + MATCH_TURN * camera_distance(d, p);
        float best_d = reach * reach;
        for (int j = lo; j < previous->n && keys[order[j]] == k; j++) {
            int c = order[j];
            if (taken[c])
                continue;
            float q[3], q_late[3];
            memcpy(q, previous->pos[c], sizeof q);
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
    static int ncap;
    if (latest->n > ncap) {
        ncap = latest->n * 2;
        loose_near = realloc(loose_near, (size_t)ncap);
    }
    memset(loose_near, 0, (size_t)latest->n);
    /* the 2D (the HUD, menus, text) is the latest frame's as it is: a menu's text changing matched glyph to glyph
     * nearby, blended from one to another, jittered (MHFU_BLEND_2D=1: matched and blended, as before) */
    static int blend_2d = -1;
    if (blend_2d < 0)
        blend_2d = getenv("MHFU_BLEND_2D") && atoi(getenv("MHFU_BLEND_2D")) != 0;
    if (blend_2d)
        match_loose(m, taken, 1, in_world);
    match_loose(m, taken, 0, in_world);
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

/* MHFU_PRIM_STATS (with MHFU_FPS_STATS): primitives, vertices and the vertex stage's time a frame drawn, by class */
enum { PS_THROUGH, PS_SPRITE, PS_BLENDED, PS_OTHER3D, PS_SCENERY, PS_GE_LIT, PS_GE_SKINNED, PS_PIXEL_LIT,
       PS_PIXEL_SKINNED, PS_CLASSES };
static const char *const ps_names[PS_CLASSES] = {"through/2D", "sprites 3D", "blended (loose)", "other 3D",
                                                 "world unlit (scenery)", "world GE-lit", "world GE-lit skinned",
                                                 "world pixel-lit", "world pixel-lit skinned"};
static int prim_stats = -1;
static unsigned long ps_frames, ps_prims[PS_CLASSES], ps_verts[PS_CLASSES], ps_runs[PS_CLASSES];
static float ps_last[12 + 96];
static int ps_last_class = -1;
static uint64_t ps_ns[PS_CLASSES];
static uint64_t ps_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
static int ps_class(int type, int blend) {
    uint32_t vt = reg[0x12];
    if ((vt >> 23) & 1)
        return PS_THROUGH;
    if (type == 6)
        return PS_SPRITE;
    if (blend)
        return PS_BLENDED;
    if (!prim_world)
        return PS_OTHER3D;
    if (!(reg[0x17] & 1))
        return PS_SCENERY;
    int skinned = ((vt >> 9) & 3) != 0;
    return prim_pixel_light ? (skinned ? PS_PIXEL_SKINNED : PS_PIXEL_LIT) : (skinned ? PS_GE_SKINNED : PS_GE_LIT);
}
static void ps_note(int type, int blend, int count, uint64_t at) {
    int k = ps_class(type, blend);
    ps_prims[k]++;
    ps_verts[k] += (unsigned long)count;
    ps_ns[k] += ps_now() - at;
    /* (a run: the same world - and bones, if skinned - as the primitive before) */
    float m[12 + 96];
    int nb = ((reg[0x12] >> 9) & 3) ? (int)((reg[0x12] >> 14) & 7) + 1 : 0;
    memset(m, 0, sizeof m);
    memcpy(m, world, sizeof world);
    memcpy(m + 12, bones, sizeof(float) * 12 * (size_t)nb);
    if (k != ps_last_class || memcmp(m, ps_last, sizeof m))
        ps_runs[k]++;
    memcpy(ps_last, m, sizeof m);
    ps_last_class = k;
}
static void prim_stats_print(void) {
    if (prim_stats <= 0 || !ps_frames)
        return;
    double f = (double)ps_frames;
    fprintf(stderr, "  primitives a frame drawn (%lu frames):\n", ps_frames);
    for (int k = 0; k < PS_CLASSES; k++)
        if (ps_prims[k])
            fprintf(stderr, "    %-24s %7.0f prims %8.0f vertices %6.3f ms, %6.0f matrix changes\n", ps_names[k],
                    (double)ps_prims[k] / f, (double)ps_verts[k] / f, (double)ps_ns[k] / 1e6 / f,
                    (double)ps_runs[k] / f);
    if (tl_groups_drawn)
        fprintf(stderr, "    the GPU's vertex stage: %.0f groups drawn as one (%.0f draws)\n",
                (double)tl_groups_drawn / f, (double)tl_group_draws / f);
    tl_groups_drawn = tl_group_draws = 0;
    ps_frames = 0;
    memset(ps_prims, 0, sizeof ps_prims);
    memset(ps_verts, 0, sizeof ps_verts);
    memset(ps_ns, 0, sizeof ps_ns);
    memset(ps_runs, 0, sizeof ps_runs);
}

void gpu_prim_stats_now(void) {     /* (MHFU_FRAME_LOG with MHFU_PRIM_STATS: every so many frames, whatever the display) */
    prim_stats_print();
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
/* Whether a group of the latest frame can be replayed as one: its draws all matched or none - into one group of the
 * previous frame (whose draws have the same matrices, as its own do) -, placed with the same camera, none blended
 * (loose) (once a frame recorded, and matching) */
static int tl_group_ok(int g, const int *m) {
    static int groups = -1;             /* (MHFU_TL_GROUPS=0: each draw replayed on its own, for comparing) */
    if (groups < 0)
        groups = !getenv("MHFU_TL_GROUPS") || atoi(getenv("MHFU_TL_GROUPS")) != 0;
    if (!groups)
        return 0;
    static uint8_t *ok;
    static int cap;
    static unsigned long stamp = ~0ul;
    unsigned long now = (unsigned long)frame_generation << 2 | (unsigned long)(native_camera != 0) << 1 | (m != NULL);
    if (latest->ntlg > cap) {
        cap = latest->ntlg * 2;
        ok = realloc(ok, (size_t)cap);
        stamp = ~0ul;
    }
    if (stamp != now) {
        stamp = now;
        memset(ok, 2, (size_t)latest->ntlg);
    }
    if (ok[g] != 2)
        return ok[g];
    const TlGroup *G = &latest->tlg[g];
    int f = G->first, yes = 1;
    if (m) {
        int pg = m[f] >= 0 ? previous->draws[m[f]].group : -1;
        for (int j = f; j < f + G->n && yes; j++) {
            if (loose_near[j] || stale[j] != stale[f] || (m[j] >= 0) != (m[f] >= 0))
                yes = 0;
            else if (m[f] >= 0 && (pg < 0 || previous->draws[m[j]].group != pg))
                yes = 0;
        }
    }
    ok[g] = (uint8_t)yes;
    return yes;
}

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
    ps_frames++;
    placed_run = -1;
    q_time((rt_game_30ths() - (1.0 - t) * rt_frame_30ths()) / 30.0);
    int matched_frames = previous->n && !camera_cut(previous, latest);
    int interpolate = matched_frames && t < 1.0f;
    {
        static float last_t;
        static unsigned last_generation;
        float step = (float)(frame_generation - last_generation) + t - last_t;
        shown_step = render_motion_wanted() ? (step < 0 ? 0 : step > 1 ? 1 : step) : 0;
        last_t = t;
        last_generation = frame_generation;
    }
    camera_frames_late(frames_late);
    native_camera = camera_views(t, interpolate, prev_inv, latest_inv, shown_view);
    /* (matched for the motion blur too, at the frame's end) */
    int *m = interpolate || (matched_frames && native_camera && render_motion_wanted()) ? match(native_camera) : NULL;
    stale_known = m && native_camera;
    shadow_frame(native_camera ? shown_view : NULL);
    if (pick_on)
        pick_camera_recorded();
    recording = 0;
    in_replay = 1;
    for (int i = 0; i < latest->n; i++) {
        const Draw *prev = m && m[i] >= 0 ? &previous->draws[m[i]] : NULL;
        const Draw *d = &latest->draws[i];
        const TlGroup *g = d->group >= 0 ? &latest->tlg[d->group] : NULL;
        if (g && g->first == i && g->n > 0 && tl_group_ok(d->group, m)) {
            /* the GPU's vertex stage's group: replayed as one */
            if (m)
                for (int j = i; j < i + g->n; j++)
                    m[j] >= 0 ? matched++ : unmatched++;
            replay_group = g;
            replay(d, interpolate ? prev : NULL, prev, t);
            replay_group = NULL;
            i += g->n - 1;
            continue;
        }
        if (m)
            prev ? matched++ : unmatched++;
        replay(d, interpolate ? prev : NULL, prev, t);
    }
    earlier = 0;
    if (pick_on && pick_wanted) {       /* (the inspector: each draw gone over as drawn, none drawn) */
        pick_wanted = 0;
        pick_begin();
        pick_pass = 1;
        placed_run = -1;
        for (int i = 0; i < latest->n; i++) {
            const Draw *prev = interpolate && m && m[i] >= 0 ? &previous->draws[m[i]] : NULL;
            pick_set_source(latest->draws[i].vaddr);
            replay(&latest->draws[i], prev, NULL, t);
        }
        pick_pass = 0;
        placed_run = -1;
        pick_end(latest->display_addr);
    }
    recording = 1;
    in_replay = 0;
    q_time(-1.0);
    memcpy(reg, saved, sizeof reg);
    vaddr = sv;
    iaddr = si;
    memcpy(world, sw, sizeof sw);
    memcpy(view, svw, sizeof svw);
    memcpy(proj, sp, sizeof sp);
    memcpy(bones, sb, sizeof sb);
    return latest->display_addr;
}

/* -- the native frame rate: what still ticks at 30, smoothed between its 30ths (native.c) ---------------------------
 *
 * At MHFU_TICK_HZ above 30 the game draws every frame, but what is not converted yet moves at its 30ths alone. The
 * render pass places everything anew each frame - the frame's camera multiplied into each world matrix - so a thing
 * ticking at 30 is told by what does not change between 30ths: its bones (the camera is not in them) and its place in
 * the world (the camera taken out: as far as the GE's 24-bit floats keep it). Each frame's draws in the world are noted
 * as they are drawn - their key, place and bones - and matched to the last 30th's (to the frame before's, at a 30th):
 * a draw ticking at 30 that moved between the last two 30ths is drawn between its places there, by how far the frame
 * is into the 30th - one 30th behind, as MHFU_FPS shows the game - with the frame's camera; at a 30th, at the last's.
 * The rest is drawn as the game drew it. What gated code drew at the 30th and is drawn again (gpu_held_draw) has that
 * 30th's camera in it: it is taken out as such. (A frame's list is the game's frame's own, made after its cameras: drawn
 * with the camera the game has as it is drawn.) MHFU_NATIVE_SMOOTH=0: every frame as the game draws it (each the same
 * as a frame at 30 a second, with nothing converted). */
int native_frame30(int back);

/* The game frames since the last a 30th fell on (0: this one) - counted by the game's, not the frames drawn (some may
 * not be: gpu_skip_draw) */
static int native_since30(void) {
    int k = 0;
    while (k < 31 && !native_frame30(k))
        k++;
    return k;
}

typedef struct {                        /* (in a cache line) */
    float w[12];                        /* its place in the world */
    uint64_t bones_hash;                /* its bones, told apart by their hash */
    int32_t match;                      /* its match in the frame it was told by (-1: none) */
    int8_t nb, slow;                    /* how many bones; ticking at 30 */
} Placing;
/* A frame's draws in the world, and once it is done, by key: a table of the keys (open addressing), each with its
 * draws in the order they came */
typedef struct {
    uint64_t key;
    int start, count;
} Slot;
typedef struct {
    Placing *p;
    float (*bones)[8][12];              /* each draw's bones */
    uint64_t *keys;                     /* each draw's key and where it is (read in turn, matching) */
    float (*pos)[3];
    int n, cap;
    Slot *slots;
    int *order, nslots, scap, ocap;
    int next;                           /* matching into it: the draw after the last matched */
    uint8_t *taken;
    int *ba;                            /* (a 30th's) each draw's match in the 30th before (-1: none), once known */
    int ba_known;
    float cam[12], inv[12];             /* the camera it was drawn with, and undone */
    int is30, cams;
} Placings;
static Placings pls[4];
static Placings *nat_c, *nat_p, *nat_b, *nat_a;    /* this frame, the one before it, the last two 30ths' */
static int nat_on = -1, since30;
static unsigned nat_generation;
static unsigned long nat_slow, nat_drawn;      /* (for MHFU_FPS_STATS) */
static int nat_override;                        /* this draw's matrices are smoothed: as they were, to put back */
static float nat_world[12], nat_bones[8][12];

/* Whether frames are smoothed (the game at a rate above 30: present.c; with MHFU_FPS too, as they are recorded) */
int gpu_native_smoothing(void) {
    if (nat_on < 0)
        nat_on = !getenv("MHFU_NATIVE_SMOOTH") || atoi(getenv("MHFU_NATIVE_SMOOTH")) != 0;
    return nat_on && render_on && rt_frame_30ths() < 0.999f;
}

/* How many draws were smoothed, of those in the world (for MHFU_FPS_STATS) */
void gpu_native_stats(unsigned long *smoothed, unsigned long *drawn) {
    *smoothed = nat_slow;
    *drawn = nat_drawn;
    nat_slow = nat_drawn = 0;
}

/* The slot of a key in f's table (an empty one if it has none) */
static int slot_of(const Placings *f, uint64_t key) {
    int mask = f->nslots - 1, s = (int)((key ^ key >> 29) * 0x9E3779B97F4A7C15ull >> 40) & mask;
    while (f->slots[s].count && f->slots[s].key != key)
        s = (s + 1) & mask;
    return s;
}

/* Of r's draws with this key: the one after the last matched if it has it (the draws come in the same order, mostly),
 * else the nearest not taken - within reach: its index, or -1 */
static int find(Placings *r, int *next, uint8_t *taken, uint64_t key, const float *pos) {
    const float reach = MATCH_DISTANCE_WORLD * MATCH_DISTANCE_WORLD;
    int c = *next;
    if (c < r->n && r->keys[c] == key && !taken[c] && dist2(pos, r->pos[c]) <= reach) {
        taken[c] = 1;
        *next = c + 1;
        return c;
    }
    if (!r->nslots)
        return -1;
    const Slot *s = &r->slots[slot_of(r, key)];
    int best = -1;
    float best_d = reach;
    for (int j = s->start; j < s->start + s->count && j < s->start + 256; j++) {   /* (grass comes in thousands) */
        int i = r->order[j];
        float dd = taken[i] ? reach * 2 : dist2(pos, r->pos[i]);
        if (dd <= best_d) {
            best_d = dd;
            best = i;
        }
    }
    if (best >= 0) {
        taken[best] = 1;
        *next = best + 1;
    }
    return best;
}

/* (the slot of a key that has one, counts or not: placings_done) */
static int slot_of_filled(const Placings *f, uint64_t key) {
    int mask = f->nslots - 1, s = (int)((key ^ key >> 29) * 0x9E3779B97F4A7C15ull >> 40) & mask;
    while (f->slots[s].key != key)
        s = (s + 1) & mask;
    return s;
}

/* A frame is done: its draws by key */
static void placings_done(Placings *f) {
    int ns = 16;
    while (ns < 2 * f->n)
        ns *= 2;
    if (ns > f->scap) {
        f->scap = ns;
        f->slots = realloc(f->slots, sizeof *f->slots * (size_t)ns);
    }
    if (f->n > f->ocap) {
        f->ocap = f->n * 2;
        f->order = realloc(f->order, sizeof *f->order * (size_t)f->ocap);
        f->taken = realloc(f->taken, (size_t)f->ocap);
    }
    f->nslots = ns;
    memset(f->slots, 0, sizeof *f->slots * (size_t)ns);
    for (int i = 0; i < f->n; i++) {
        Slot *t = &f->slots[slot_of(f, f->keys[i])];
        t->key = f->keys[i];
        t->count++;
    }
    int at = 0;
    for (int i = 0; i < ns; i++) {
        f->slots[i].start = at;
        at += f->slots[i].count;
        f->slots[i].count = 0;          /* (counted again as they are put in order) */
    }
    for (int i = 0; i < f->n; i++) {
        Slot *t = &f->slots[slot_of_filled(f, f->keys[i])];
        f->order[t->start + t->count++] = i;
    }
    f->ba_known = 0;
}

static void ready_to_match(Placings *r) {
    r->next = 0;
    memset(r->taken, 0, (size_t)r->n);
}

/* The first draw of a frame: the one before is done, and this one's camera and 30th known (the game's camera now;
 * native.c: whether a 30th fell on the game frame, the one whose drawing this is) */
static void native_frame(void) {
    if (nat_c) {
        placings_done(nat_c);
        nat_p = nat_c;
        if (nat_c->is30) {
            nat_a = nat_b;
            nat_b = nat_c;
        }
    }
    nat_c = NULL;
    for (int i = 0; i < 4 && !nat_c; i++)
        if (&pls[i] != nat_p && &pls[i] != nat_b && &pls[i] != nat_a)
            nat_c = &pls[i];
    nat_c->n = 0;
    float target[3];
    nat_c->is30 = native_frame30(0);
    nat_c->cams = camera_game(nat_c->inv, target);
    if (nat_c->cams)
        camera_invert(nat_c->inv, nat_c->cam);
    since30 = native_since30();
    Placings *ref = nat_c->is30 ? nat_p : nat_b;
    if (ref)
        ready_to_match(ref);
}

/* A 30th's draws matched to the 30th before's (once) */
static int *b_to_a(void) {
    if (!nat_b->ba_known) {
        nat_b->ba = realloc(nat_b->ba, sizeof *nat_b->ba * (size_t)(nat_b->n ? nat_b->n : 1));
        int next = 0;
        uint8_t *taken = calloc((size_t)(nat_a->n ? nat_a->n : 1), 1);
        for (int i = 0; i < nat_b->n; i++)
            nat_b->ba[i] = find(nat_a, &next, taken, nat_b->keys[i], nat_b->pos[i]);
        free(taken);
        nat_b->ba_known = 1;
    }
    return nat_b->ba;
}

/* Two places in the world the same, as far as the GE's floats keep them (the camera, at up to some 20000 units, is
 * multiplied in with 16 bits of mantissa): each axis against its length, the translation within a unit */
static int same_world(const float *a, const float *b) {
    for (int c = 0; c < 3; c++) {
        float len = fabsf(a[3 * c]) + fabsf(a[3 * c + 1]) + fabsf(a[3 * c + 2]);
        for (int i = 3 * c; i < 3 * c + 3; i++)
            if (fabsf(a[i] - b[i]) > 0.001f * len + 1e-4f)
                return 0;
    }
    for (int i = 9; i < 12; i++)
        if (fabsf(a[i] - b[i]) > 1.0f)
            return 0;
    return 1;
}

static int same_place(const Placing *a, const Placing *b) {
    return a->nb == b->nb && a->bones_hash == b->bones_hash && same_world(a->w, b->w);
}

/* The game's camera, if it moves only at 30ths (as everything does where nothing is converted: the village), shown
 * between its places at the last two 30ths as what ticks at 30 is (below), by how far the frame is into the 30th; view:
 * the game's now. 1 if so (base: the camera to show), 0 if it is the game's as it is */
static int native_camera_between(const float *view, const float *target, float *base, float *base_target) {
    static float at30[2][12], last[12], target30[2][3];
    static int have, moved, slow;
    int is30 = native_frame30(0);
    if (have && !is30 && memcmp(view, last, sizeof last))
        moved = 1;                      /* it moved between 30ths: the game moves it every frame */
    memcpy(last, view, sizeof last);
    if (is30) {
        memcpy(at30[0], at30[1], sizeof at30[0]);
        memcpy(at30[1], view, sizeof at30[1]);
        memcpy(target30[0], target30[1], sizeof target30[0]);
        memcpy(target30[1], target, sizeof target30[1]);
        have = have < 2 ? have + 1 : 2;
        slow = !moved;
        moved = 0;
    }
    int since = native_since30();
    if (have < 2 || !slow || !memcmp(at30[0], at30[1], sizeof at30[0]))
        return 0;
    float t = (float)since * rt_frame_30ths(), a[12], b[12], c[12];
    if (t > 1)
        t = 1;
    /* the camera's place in the world (the view undone), between; and back. Not across a cut */
    if (!invert43(at30[0], a) || !invert43(at30[1], b))
        return 0;
    float d = sqrtf((a[9] - b[9]) * (a[9] - b[9]) + (a[10] - b[10]) * (a[10] - b[10]) + (a[11] - b[11]) * (a[11] - b[11]));
    if (d > 600)
        return 0;
    interp43(c, a, b, t);
    for (int i = 0; i < 3; i++)
        base_target[i] = target30[0][i] + (target30[1][i] - target30[0][i]) * t;
    return invert43(c, base);
}

/* camera.c: a camera (view, 4x3) t of the way from a to b - its place in the world between */
void gpu_camera_between(const float *a, const float *b, float t, float *out) {
    float ia[12], ib[12], c[12];
    if (!invert43(a, ia) || !invert43(b, ib) || (interp43(c, ia, ib, t), !invert43(c, out)))
        memcpy(out, t < 0.5f ? a : b, sizeof(float) * 12);
}

/* A frame's next draw: room for it, its key */
static int placing_add(Placings *f, uint64_t key) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 4096;
        f->p = realloc(f->p, sizeof *f->p * (size_t)f->cap);
        f->bones = realloc(f->bones, sizeof *f->bones * (size_t)f->cap);
        f->keys = realloc(f->keys, sizeof *f->keys * (size_t)f->cap);
        f->pos = realloc(f->pos, sizeof *f->pos * (size_t)f->cap);
    }
    f->keys[f->n] = key;
    return f->n++;
}

/* prim: a draw in the world about to be drawn (key: its draw_key) - noted, and its matrices smoothed if it ticks at
 * 30 (nat_override: they are put back after) */
static void native_place(uint64_t key) {
    if (nat_generation != frame_generation_direct) {
        nat_generation = frame_generation_direct;
        native_frame();
    }
    Placings *f = nat_c;
    if (!f->cams)
        return;
    int qi = placing_add(f, key);
    Placing *q = &f->p[qi];
    q->nb = (int8_t)((reg[0x12] >> 9) & 3 ? (int)((reg[0x12] >> 14) & 7) + 1 : 0);
    /* drawn again from the last 30th: with its camera */
    compose43(held_drawing && nat_b && nat_b->cams ? nat_b->inv : f->inv, world, q->w);
    memcpy(f->bones[qi], bones, sizeof(float) * 12 * (size_t)q->nb);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const uint32_t *u = (const uint32_t *)f->bones[qi];
    for (int i = 0; i < 12 * q->nb; i++)
        h = (h ^ u[i]) * 0x100000001B3ull;
    q->bones_hash = h;
    if (q->nb)
        mul43(q->w, &f->bones[qi][0][9], f->pos[qi]);
    else
        memcpy(f->pos[qi], &q->w[9], sizeof f->pos[qi]);
    q->match = -1;
    q->slow = 0;
    nat_drawn++;
    /* drawn between from (its place at the 30th before) and to (the last's) */
    const Placing *from = q, *to = q;
    const float *from_bones = &f->bones[qi][0][0], *to_bones = from_bones;
    float t = 1;
    Placings *ref = f->is30 ? nat_p : nat_b;
    int j = ref && ref->cams ? find(ref, &ref->next, ref->taken, key, f->pos[qi]) : -1;
    q->match = j;
    if (j >= 0 && !f->is30) {
        q->slow = (int8_t)same_place(q, &ref->p[j]);
        int k = q->slow && nat_a && nat_a->cams ? b_to_a()[j] : -1;
        if (k >= 0) {
            from = &nat_a->p[k];
            from_bones = &nat_a->bones[k][0][0];
            to = &ref->p[j];
            to_bones = &ref->bones[j][0][0];
            t = (float)since30 * rt_frame_30ths();
            if (t > 1)
                t = 1;
        }
    } else if (j >= 0) {
        q->slow = (int8_t)(ref->p[j].slow && !ref->is30);
        if (q->slow && nat_b && nat_b->cams && ref->p[j].match >= 0) {
            from = &nat_b->p[ref->p[j].match];
            from_bones = &nat_b->bones[ref->p[j].match][0][0];
            t = 0;
        }
    }
    /* as it is, if it moves every frame or not at all - unless drawn again from the last 30th (its camera that one's) */
    if (same_place(from, to) && !held_drawing)
        return;
    interp_model(world, &bones[0][0], from->w, to->w, from_bones, to_bones, q->nb, t);
    compose43(f->cam, world, world);
    nat_slow += from != to;
}

/* The game's own frames, drawn as it draws them (no MHFU_FPS): where the hunter, monsters and people were in the frame
 * drawn before, as replay() has it from the frames recorded - for the anti-aliasing and the motion blur. Each frame's
 * such draws are noted as they were drawn, in the world (to_world: whichever camera is in the matrices now), bones and
 * all, and matched by key and place as native_place matches; unmatched, a draw has no motion. */
static Placings drawn[2];
static Placings *drawn_now = &drawn[0], *drawn_last;
static unsigned drawn_generation;

static void native_motion(uint64_t key) {
    if (drawn_generation != frame_generation_direct) {
        drawn_generation = frame_generation_direct;
        placings_done(drawn_now);
        ready_to_match(drawn_now);
        drawn_last = drawn_now;
        drawn_now = drawn_now == &drawn[0] ? &drawn[1] : &drawn[0];
        drawn_now->n = 0;
    }
    Placings *f = drawn_now;
    int qi = placing_add(f, key);
    Placing *q = &f->p[qi];
    q->nb = (int8_t)((reg[0x12] >> 9) & 3 ? (int)((reg[0x12] >> 14) & 7) + 1 : 0);
    compose43(to_world, world, q->w);
    memcpy(f->bones[qi], bones, sizeof(float) * 12 * (size_t)q->nb);
    if (q->nb)
        mul43(q->w, &f->bones[qi][0][9], f->pos[qi]);
    else
        memcpy(f->pos[qi], &q->w[9], sizeof f->pos[qi]);
    Placings *r = drawn_last;
    int j = r ? find(r, &r->next, r->taken, key, f->pos[qi]) : -1;
    if (j < 0 || r->p[j].nb != q->nb)
        return;
    float shown[12];
    camera_invert(to_world, shown);
    compose43(shown, r->p[j].w, earlier_world);
    memcpy(earlier_bones, r->bones[j], sizeof(float) * 12 * (size_t)q->nb);
    earlier = 1;
}

/* A state loaded (savestate.c): what ticks at 30 smoothed from the next 30ths only - not from where it was before */
void gpu_state_loaded(void) {
    nat_a = nat_b = nat_p = NULL;
    drawn_last = NULL;
    drawn_now->n = 0;
}

/* -- what gated code drew, kept for the frames between 30ths (native.c) --------------------------------------------
 *
 * The GE's commands from the chunks gated code linked at the last 30th (ge.c tells which) are kept, the primitives with
 * their vertices as recorded frames keep them, and run again where a chunk in its place says (gpu_held_draw): the
 * state they set as they set it, the primitives drawn from the copies. What moves the list on (jumps, calls, BASE), or
 * reads memory then (CLUT loads, transfers), is left out. */

/* -- the HUD smoothed between 30ths (the native frame rate) --------------------------------------------------------
 *
 * The HUD and menus are drawn by gated code: at a 30th, and the same again on the frames between. What moves in them
 * (the gauges, the map turning with the camera, the item bar's slide) is drawn between its places at the last two
 * 30ths by how far the frame is into the 30th - one 30th behind, as the world is (above). A 2D draw is matched to the
 * 30th before's strictly: the same kind, the same picture (texture coordinates), none of its sides moved over HUD_MOVE
 * pixels - and over HUD_NEAR only the next in the order drawn: text that changes is not matched letter to the same
 * letter elsewhere (a dialogue's next page), and is drawn as it is. Its positions and
 * colours are blended into a scratch area it is drawn (and recorded, MHFU_FPS) from. MHFU_HUD_SMOOTH=0: not smoothed. */
#define HUD_MOVE 24.0f
#define HUD_NEAR 4.0f               /* (moved further, a draw is matched only to the next in order) */
#define SCRATCH_HUD 0x087C0000u     /* (to 0x08800000) */
static Frame held_prev;             /* the 2D and the rest gated code drew at the 30th before */
static Shape *hud_prev_shape;       /* its draws' shapes (ok -1: not 2D) */
static uint8_t *hud_taken;
static int hud_prev_cap, *hud_match, hud_match_cap, hud_cursor;
static int held_index = -1;         /* (the kept draw gpu_held_draw is drawing again) */

static int hud_on(void) {
    static int on = -1;
    if (on < 0)
        on = !getenv("MHFU_HUD_SMOOTH") || atoi(getenv("MHFU_HUD_SMOOTH")) != 0;
    return on && gpu_native_smoothing();
}

/* The 30th's kept draw i (2D, its run closed): its match in the 30th before, or -1 */
static int hud_find(int i) {
    if (i >= hud_match_cap) {
        hud_match_cap = (i + 1) * 2;
        hud_match = realloc(hud_match, sizeof *hud_match * (size_t)hud_match_cap);
    }
    hud_match[i] = -1;
    const Draw *d = &held.draws[i];
    Shape a = shape(&held, d);
    if (!a.ok)
        return -1;
    int lo = hud_cursor - 8 > 0 ? hud_cursor - 8 : 0, hi = hud_cursor + 32 < held_prev.n ? hud_cursor + 32 : held_prev.n;
    int best = -1, next = hud_cursor;
    while (next < held_prev.n && (hud_taken[next] || hud_prev_shape[next].ok != 1))
        next++;
    float best_m = HUD_MOVE + 1;
    for (int j = lo; j < hi; j++) {
        const Draw *e = &held_prev.draws[j];
        const Shape *b = &hud_prev_shape[j];
        if (hud_taken[j] || b->ok != 1 || b->uvs != a.uvs || !same_kind(d, e))
            continue;
        float m = 0;
        for (int k = 0; k < 4; k++)
            m = fmaxf(m, fabsf(a.box[k] - b->box[k]));
        if (m > HUD_NEAR && j != next)
            continue;               /* (moved far: only as the next in order - a window sliding in, its letters in turn) */
        if (m < best_m) {
            best_m = m;
            best = j;
        }
    }
    if (best_m > HUD_MOVE)
        return -1;
    hud_taken[best] = 1;
    hud_cursor = best + 1;
    return hud_match[i] = best;
}

/* A 2D draw's vertices (count, at src) blended from its match's (j, the 30th before) by t, into SCRATCH_HUD */
static void hud_blend(int j, uint32_t src, int count, const Layout *l, float t) {
    const Draw *e = &held_prev.draws[j];
    const Run *r = &held_prev.runs[e->run];
    const uint8_t *from = held_prev.bytes + r->copy - r->lo;
    uint32_t vt = reg[0x12];
    int pos = (vt >> 7) & 3, col = (vt >> 2) & 7;
    memcpy(PTR(SCRATCH_HUD), PTR(src), (size_t)(count * l->size));
    for (int i = 0; i < count; i++) {
        uint8_t *v = PTR(SCRATCH_HUD + (uint32_t)(i * l->size));
        const uint8_t *q = from + i * l->size;
        for (int k = 0; k < 3; k++) {
            if (pos == 1) {
                int8_t a = (int8_t)q[l->po + k], b = (int8_t)v[l->po + k];
                v[l->po + k] = (uint8_t)(int8_t)lrintf(a + (b - a) * t);
            } else if (pos == 2) {
                int16_t a, b;
                memcpy(&a, q + l->po + 2 * k, 2);
                memcpy(&b, v + l->po + 2 * k, 2);
                b = (int16_t)lrintf(a + (b - a) * t);
                memcpy(v + l->po + 2 * k, &b, 2);
            } else if (pos == 3) {
                float a, b;
                memcpy(&a, q + l->po + 4 * k, 4);
                memcpy(&b, v + l->po + 4 * k, 4);
                b = a + (b - a) * t;
                memcpy(v + l->po + 4 * k, &b, 4);
            }
        }
        if (col == 7) {
            for (int k = 0; k < 4; k++)
                v[l->co + k] = (uint8_t)lrintf(q[l->co + k] + (v[l->co + k] - q[l->co + k]) * t);
        } else if (col >= 4) {          /* 565, 5551, 4444: each field */
            static const int bits[3][4] = {{5, 6, 5, 0}, {5, 5, 5, 1}, {4, 4, 4, 4}};
            uint16_t a, b, out = 0;
            memcpy(&a, q + l->co, 2);
            memcpy(&b, v + l->co, 2);
            for (int k = 0, at = 0; k < 4; at += bits[col - 4][k], k++) {
                int mask = (1 << bits[col - 4][k]) - 1, fa = (a >> at) & mask, fb = (b >> at) & mask;
                out |= (uint16_t)(lrintf(fa + (fb - fa) * t) << at);
            }
            memcpy(v + l->co, &out, 2);
        }
    }
}

void gpu_held_clear(void) {
    /* the 30th's kept draws become the 30th before's, their 2D's shapes noted */
    close_run(&held);
    Frame f = held_prev;
    held_prev = held;
    held = f;
    if (held_prev.n > hud_prev_cap) {
        hud_prev_cap = held_prev.n * 2;
        hud_prev_shape = realloc(hud_prev_shape, sizeof *hud_prev_shape * (size_t)hud_prev_cap);
        hud_taken = realloc(hud_taken, (size_t)hud_prev_cap);
    }
    for (int j = 0; j < held_prev.n; j++) {
        const Draw *e = &held_prev.draws[j];
        hud_prev_shape[j].ok = -1;
        if (through_draw(e) && !e->ibytes)
            hud_prev_shape[j] = shape(&held_prev, e);
    }
    memset(hud_taken, 0, (size_t)held_prev.n);
    hud_cursor = 0;
    held.n = held.nruns = held.open = 0;
    held.used = 0;
    held_n = 0;
}

void gpu_held_chunk(int chunk) {
    if (chunk != held_chunk)
        close_run(&held);           /* (its vertices copied while they are there) */
    held_chunk = chunk;
}

static int held_kept(uint32_t op) {
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x04: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0E:
    case 0x0F: case 0x10: case 0x13: case 0x14: case 0xC4: case 0xEA:
        return 0;
    default:
        return 1;
    }
}

void gpu_held_draw(int chunk) {
    close_run(&held);
    uint32_t sv = vaddr, si = iaddr;
    held_drawing = 1;
    for (int i = 0; i < held_n; i++) {
        const HeldCommand *h = &held_cmds[i];
        if (h->chunk != chunk)
            continue;
        if (h->draw < 0) {
            gpu_command(h->cmd, h->addr);
            continue;
        }
        const Draw *d = &held.draws[h->draw];
        const Run *r = &held.runs[d->run];
        memcpy(PTR(SCRATCH_VERTICES + r->lo), held.bytes + r->copy, r->hi - r->lo);
        if (d->ibytes)
            memcpy(PTR(SCRATCH_INDICES), held.bytes + d->icopy, d->ibytes);
        vaddr = SCRATCH_VERTICES;
        iaddr = SCRATCH_INDICES;
        reg[0x04] = d->data;
        if (render_on)
            q_override(d->tex_slot, d->tex_w, d->tex_h);
        held_texture = d;
        held_index = h->draw;
        draw_hud = d->hud;
        prim(d->data);
        held_index = -1;
        held_texture = NULL;
        if (render_on)
            q_override(0xFFFFFFFEu, 0, 0);
        if (recording)
            close_run(rec);         /* (its copy made now: the scratch area is used again) */
    }
    placed_run = -1;
    held_drawing = 0;
    vaddr = sv;
    iaddr = si;
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
    prim_stats_print();
}

/* A draw blended between frames: its match's vertices (and indices) where vertex() reads them; 0 if they do not fit */
static int blend_place(int count) {
    const Draw *p = blend_from;
    const Run *r = &previous->runs[p->run];
    if (r->hi > 0x80000u || p->ibytes > 0x40000u || (p->data & 0xFFFF) != (uint32_t)count)
        return 0;
    memcpy(PTR(SCRATCH_BLEND_VERTICES + r->lo), previous->bytes + r->copy, r->hi - r->lo);
    if (p->ibytes)
        memcpy(PTR(SCRATCH_BLEND_INDICES), previous->bytes + p->icopy, p->ibytes);
    blend_by = blend_t;
    return 1;
}

/* present.c: this game frame is not drawn - the game at its own rate above 30, running late: what is between 30ths
 * left out, so that it keeps time (its sound with it) */
int gpu_skip_draw, gpu_divisor = 1;    /* (present.c: the game late - the next frame not drawn, if above 1) */
static unsigned skip_generation;
static int nat_between;                 /* the frame's camera shown between 30ths (native_camera_between) */
static float nat_view_now[12], nat_base[12], nat_target_now[3], nat_base_target[3];
static int nat_view_ok(void) {
    return nat_view_now[0] != 0 || nat_view_now[4] != 0 || nat_view_now[8] != 0;
}
static void native_place(uint64_t key);
static void native_motion(uint64_t key);
static int native_camera_between(const float *view, const float *target, float *base, float *base_target);
int gpu_native_smoothing(void);
int camera_native_shown(const float *view, const float *target, float *shown);

/* For texture packs, as PPSSPP has it: how far down a 512-tall texture a 2D draw reaches (at least 272), else all */
static void seen_v(int count, const Layout *l) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, idx = (vt >> 11) & 3;
    draw_v = 512;
    if (!((vt >> 23) & 1) || ((reg[0xB8] >> 8) & 0xF) != 9 || !tc)
        return;
    float most = 0;
    for (int i = 0; i < count; i++) {
        int n = idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i;
        uint32_t at = vaddr + (uint32_t)(n * l->size) + (uint32_t)l->to;
        float v = tc == 1 ? (float)LBU(at + 1) : tc == 2 ? (float)LHU(at + 2) : read_comp(at + 4, 3, 0, 0);
        if (v > most)
            most = v;
    }
    draw_v = most > 272 ? (int)most : 272;
}

/* -- the GE's vertex stage on the GPU (MHFU_GPU_TL, ge_tl.vert) ---------------------------------------------------
 * A primitive's vertices go to the renderer as the game gave them (decoded) with its draw - what vertex() and light()
 * read: the matrices, viewport, fog, texture scaling, material and lights - and the GPU does their vertex stage.
 * MHFU_GPU_TL=1: the hunter, monsters and people's (lit, skinned, 3D); 2 (the default): all 3D triangles; 0: none -
 * the CPU's, the reference the kit's pixel-exact checks use.
 * The effects that read the vertices take them from the renderer's copies (render.c: shadows, feet, tessellation, the
 * motion blur, the scenery's shading, wind and grass); what adds light (fire, glows) is left to the CPU. */
static int tl_mode = -1;

static int tl_take(int type, int blend) {
    if (tl_mode < 0) {
        tl_mode = getenv("MHFU_GPU_TL") ? atoi(getenv("MHFU_GPU_TL")) : 2;
        if (tl_mode && !render_tl_ready()) {
            fprintf(stderr, "GPU vertex stage (MHFU_GPU_TL): not available, the CPU's used\n");
            tl_mode = 0;
        }
    }
    uint32_t vt = reg[0x12];
    if (!tl_mode || type < 3 || type > 5 || blend || ((vt >> 23) & 1) || proj[11] == 0)
        return 0;
    /* left to the CPU's vertex stage, as the renderer reads their vertices (render.c render_prim_begin): what adds to
     * the picture - fire and glows: the point lights are made from them, the glow pass draws them again - and the
     * game's round shadows (dropped by the vertex, where the shadows are drawn) */
    if (reg[0x21] & 1) {
        uint32_t b = reg[0xDF];
        int fd = (b >> 4) & 15, op = (b >> 8) & 7;
        if (fd == 10 && (reg[0xE1] & 0xFFFFFF) == 0xFFFFFF && (op == 0 || op >= 5))
            return 0;
        if (!(reg[0x17] & 1) && (b & 0xFFF) == 0x32 && (reg[0xC3] & 0xF) == 5 && (reg[0xB8] & 0xF0F) == 0x707)
            return 0;
    }
    if (tl_mode == 1)
        return (reg[0x17] & 1) && ((vt >> 9) & 3);
    return 1;
}

/* A 3 x 4 matrix (by columns) as rows */
static void tl_rows(const float *m, float (*r)[4]) {
    for (int i = 0; i < 3; i++) {
        r[i][0] = m[i];
        r[i][1] = m[3 + i];
        r[i][2] = m[6 + i];
        r[i][3] = m[9 + i];
    }
}

/* The primitive's draw, as vertex() and light() would read it now */
static void tl_fill(TlDraw *d) {
    uint32_t vt = reg[0x12];
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, wt = (vt >> 9) & 3;
    memset(d, 0, sizeof *d);
    tl_rows(world, d->world);
    tl_rows(view, d->view);
    memcpy(d->proj, proj, sizeof d->proj);
    tl_rows(to_world, d->to_world);
    d->bones = wt ? ((vt >> 14) & 7) + 1 : 0;
    for (uint32_t b = 0; b < d->bones; b++)
        tl_rows(bones[b], &d->bone[3 * b]);
    d->flags = (prim_world ? TL_WORLD : 0u) | (tc ? TL_TEX : 0u) | (col >= 4 ? TL_VCOL : 0u) | (nrm && (reg[0x51] & 1) ? TL_FLIP : 0u);
    if (earlier && prim_world) {
        d->flags |= TL_EARLIER;
        tl_rows(earlier_world, d->earlier_world);
        for (uint32_t b = 0; b < d->bones; b++)
            tl_rows(earlier_bones[b], &d->earlier_bone[3 * b]);
    }
    d->viewport[0] = ge_float(reg[0x42]);
    d->viewport[1] = ge_float(reg[0x43]);
    d->viewport[2] = ge_float(reg[0x44]);
    d->viewport[3] = ge_float(reg[0x47]);
    d->offset[0] = ge_float(reg[0x45]) - (float)(reg[0x4C] & 0xFFFF) / 16.0f;
    d->offset[1] = ge_float(reg[0x46]) - (float)(reg[0x4D] & 0xFFFF) / 16.0f;
    d->offset[2] = ge_float(reg[0xCD]);
    d->offset[3] = ge_float(reg[0xCE]);
    /* scaled, offset, then in texels (by a power of two: the same folded into the scale and offset) */
    float su = (float)(1 << (reg[0xB8] & 0xF)), sv = (float)(1 << ((reg[0xB8] >> 8) & 0xF));
    d->tex[0] = ge_float(reg[0x48]) * su;
    d->tex[1] = ge_float(reg[0x49]) * sv;
    d->tex[2] = ge_float(reg[0x4A]) * su;
    d->tex[3] = ge_float(reg[0x4B]) * sv;
    if (!(reg[0x17] & 1))
        return;
    d->flags |= TL_LIT;
    d->mode = col >= 4 ? reg[0x53] : 0;
    d->sun_light = (uint32_t)sun_light;
    unpack(reg[0x54], d->emissive);
    d->emissive[3] = ge_float(reg[0x5B]);
    unpack(reg[0x55], d->ambient);
    d->ambient[3] = (float)(reg[0x58] & 255) / 255.0f;
    unpack(reg[0x56], d->diffuse);
    unpack(reg[0x57], d->specular);
    unpack(reg[0x5C], d->global);
    d->global[3] = (float)(reg[0x5D] & 255) / 255.0f;
    for (int k = 0; k < 4; k++) {
        if (!(reg[0x18 + k] & 1))
            continue;
        d->flags |= (TL_LIGHT_ON << k) | (((pixel_lights >> k) & 1) ? TL_LIGHT_PIXEL << k : 0u);
        uint32_t lt = reg[0x5F + k];
        for (int i = 0; i < 3; i++) {
            d->light_pos[k][i] = ge_float(reg[0x63 + 3 * k + i]);
            d->light_dir[k][i] = ge_float(reg[0x6F + 3 * k + i]);
            d->light_att[k][i] = ge_float(reg[0x7B + 3 * k + i]);
        }
        d->light_pos[k][3] = (float)((lt >> 8) & 3);
        d->light_dir[k][3] = (float)(lt & 3);
        d->light_att[k][3] = ge_float(reg[0x8B + k]);
        unpack(reg[0x8F + 3 * k], d->light_ambient[k]);
        d->light_ambient[k][3] = ge_float(reg[0x87 + k]);
        unpack(reg[0x90 + 3 * k], d->light_diffuse[k]);
        unpack(reg[0x91 + 3 * k], d->light_specular[k]);
    }
}

/* Vertex n as the game gave it, decoded as vertex() reads it */
static void tl_vertex(uint32_t base, int n, const Layout *l, TlVertex *o) {
    uint32_t vt = reg[0x12], v = base + (uint32_t)(n * l->size);
    int tc = vt & 3, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3, wt = (vt >> 9) & 3;
    static const int sz[4] = {0, 1, 2, 4};
    for (int i = 0; i < 3; i++) {
        o->pos[i] = read_comp(v + l->po + (uint32_t)(i * sz[pos]), pos, 0, 1);
        o->normal[i] = nrm ? read_comp(v + l->no + (uint32_t)(i * sz[nrm]), nrm, 0, 1) : i == 2 ? 1.0f : 0.0f;
    }
    o->color = read_color(v, l, vt, reg);
    o->uv[0] = o->uv[1] = 0;
    if (tc == 3) {
        o->uv[0] = read_comp(v + l->to, 3, 0, 0);
        o->uv[1] = read_comp(v + l->to + 4, 3, 0, 0);
    } else if (tc) {
        o->uv[0] = tc == 1 ? LBU(v + l->to) / 128.0f : LHU(v + l->to) / 32768.0f;
        o->uv[1] = tc == 1 ? LBU(v + l->to + 1) / 128.0f : LHU(v + l->to + 2) / 32768.0f;
    }
    memset(o->weights, 0, sizeof o->weights);
    memset(o->shape, 0, sizeof o->shape);
    o->edges = 0;
    if (wt) {
        int nweights = ((vt >> 14) & 7) + 1;
        for (int b = 0; b < nweights; b++)
            o->weights[b] = wt == 1 ? LBU(v + l->wo + (uint32_t)b) / 128.0f
                            : wt == 2 ? LHU(v + l->wo + (uint32_t)(b * 2)) / 32768.0f
                                      : read_comp(v + l->wo + (uint32_t)(b * 4), 3, 0, 0);
    }
}

/* The primitive to the renderer for the GPU's vertex stage: its draw (unless the last one sent is the same), and its
 * vertices in the order drawn (indexed: those the indices name - small strips index big shared buffers) */
/* A primitive's triangles as indices (from base), as gpu.c's clipping takes them (a strip's every other one turned
 * round, to keep its winding) - the third corner of each its own vertex (the one it ends with); how many */
static int tl_triangles(int type, int count, uint32_t base, uint32_t *o) {
    int ntri = type == 3 ? count / 3 : count - 2;
    for (int t = 0; t < ntri; t++) {
        int a, b, c;
        if (type == 3)
            a = 3 * t, b = 3 * t + 1, c = 3 * t + 2;
        else if (type == 4)
            a = (t & 1) ? t + 1 : t, b = (t & 1) ? t : t + 1, c = t + 2;
        else
            a = 0, b = t + 1, c = t + 2;
        *o++ = base + (uint32_t)a;
        *o++ = base + (uint32_t)b;
        *o++ = base + (uint32_t)c;
    }
    return ntri > 0 ? ntri : 0;
}

/* For tessellation (render_shading.c shape_normals, as the CPU's vertex stage's batches have it): the nv vertices'
 * shape normals - at each place (welded: the same position as given), its corners' normals averaged - and each of
 * the ntri triangles' (indices among them, from base) edges no other shares, on its third corner */
static void tl_shape(TlVertex *v, uint32_t nv, const uint32_t *tri, uint32_t ntri, uint32_t base) {
    static uint32_t *table, *owner, *first;     /* a place's first vertex + 1 (0: empty); each one's place; its first */
    static float (*sum)[3];
    static struct { uint64_t key; uint32_t uses; } *edges;
    static size_t cap, table_cap;
    if (nv > cap) {
        cap = nv * 2;
        owner = realloc(owner, cap * sizeof *owner);
        first = realloc(first, cap * sizeof *first);
        sum = realloc(sum, cap * sizeof *sum);
    }
    size_t tsize = 1024;
    while (tsize < nv * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
        edges = realloc(edges, table_cap * 2 * sizeof *edges);
    }
    memset(table, 0, tsize * sizeof *table);
    uint32_t places = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t hb[3];
        memcpy(hb, v[i].pos, sizeof hb);
        uint32_t x = hb[0] * 0x9E3779B1u ^ hb[1] * 0x85EBCA77u ^ hb[2] * 0xC2B2AE3Du;
        x ^= x >> 15;
        size_t j = x & (tsize - 1);
        for (;;) {
            uint32_t e = table[j];
            if (!e) {
                table[j] = ++places;
                owner[i] = places - 1;
                first[places - 1] = i;
                memset(sum[places - 1], 0, sizeof sum[0]);
                break;
            }
            if (!memcmp(v[first[e - 1]].pos, v[i].pos, sizeof v[i].pos)) {
                owner[i] = e - 1;
                break;
            }
            j = (j + 1) & (tsize - 1);
        }
        const float *n = v[i].normal;
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0)
            for (int k = 0; k < 3; k++)
                sum[owner[i]][k] += n[k] / l;
    }
    for (uint32_t i = 0; i < nv; i++) {
        const float *a = sum[owner[i]];
        float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        for (int k = 0; k < 3; k++)
            v[i].shape[k] = l > 1e-3f ? a[k] / l : 0;
    }
    size_t esize = tsize * 2;
    memset(edges, 0, esize * sizeof *edges);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < ntri; t++) {
            uint32_t o[3] = {owner[tri[3 * t] - base], owner[tri[3 * t + 1] - base], owner[tri[3 * t + 2] - base]};
            uint32_t open = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t a = o[(k + 1) % 3], b = o[(k + 2) % 3];
                uint64_t key = (a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a) + 1;
                size_t j = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (esize - 1);
                while (edges[j].key && edges[j].key != key)
                    j = (j + 1) & (esize - 1);
                if (pass == 0) {
                    edges[j].key = key;
                    edges[j].uses++;
                } else if (edges[j].uses < 2) {
                    open |= 1u << k;
                }
            }
            if (pass == 1)
                v[tri[3 * t + 2] - base].edges = open;
        }
}

/* Where a vertex is in the world, as ge_tl.vert places it (for the feet: render_scene.c) */
static void tl_world_of(const TlDraw *d, const TlVertex *v, float *w) {
    float p[3] = {0, 0, 0}, m[3];
    if (d->bones) {
        for (uint32_t b = 0; b < d->bones; b++)
            for (int r = 0; r < 3; r++) {
                const float *row = d->bone[3 * b + (uint32_t)r];
                p[r] += (row[0] * v->pos[0] + row[1] * v->pos[1] + row[2] * v->pos[2] + row[3]) * v->weights[b];
            }
    } else {
        memcpy(p, v->pos, sizeof p);
    }
    for (int r = 0; r < 3; r++)
        m[r] = d->world[r][0] * p[0] + d->world[r][1] * p[1] + d->world[r][2] * p[2] + d->world[r][3];
    for (int r = 0; r < 3; r++)
        w[r] = d->to_world[r][0] * m[0] + d->to_world[r][1] * m[1] + d->to_world[r][2] * m[2] + d->to_world[r][3];
}

/* Whether a draw's feet are wanted (render.c finds_feet: what the GE lights in the world, for the wind and grass) */
static int tl_feet_wanted(const TlDraw *d) {
    return (d->flags & TL_LIT) && (d->flags & TL_WORLD) && render_tl_feet();
}

static TlDraw tl_sent;                   /* the last draw sent (the renderer keeps it) */
static int tl_have_sent;

/* The primitive's draw as it is now; 1 if it is the one sent last (not to be sent again) */
static int tl_draw_now(TlDraw *d) {
    tl_fill(d);
    if (tl_have_sent && !memcmp(d, &tl_sent, sizeof *d))
        return 1;
    tl_sent = *d;
    tl_have_sent = 1;
    return 0;
}

static void tl_prim(int type, int count, int idx, const Layout *l) {
    static TlDraw d;
    int same = tl_draw_now(&d);
    int nv = count, ntri = type == 3 ? count / 3 : count - 2, nfeet = tl_feet_wanted(&d) && ntri > 0 ? ntri : 0;
    size_t dbytes = same ? 0 : sizeof d, ibytes = ((size_t)(2 * count) + 3) & ~(size_t)3;
    size_t bytes = 16 + dbytes + sizeof(TlVertex) * (size_t)nv + ibytes + 12 * (size_t)nfeet;
    int to_queue = queued();
    uint8_t *p = to_queue ? q_cmd(Q_TL, (uint32_t)type | (same ? 256u : 0u), (uint32_t)count, (uint32_t)nv, bytes)
                          : malloc(bytes);
    memset(p, 0, 16);
    *(uint32_t *)p = (uint32_t)nfeet;
    if (!same)
        memcpy(p + 16, &d, sizeof d);
    TlVertex *v = (TlVertex *)(p + 16 + dbytes);
    uint16_t *ix = (uint16_t *)(v + nv);
    for (int i = 0; i < count; i++) {
        tl_vertex(vaddr, idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i, l, &v[i]);
        ix[i] = (uint16_t)i;
    }
    /* (the feet: where each triangle's third corner is in the world - all of its own vertices but the first two) */
    float *feet = (float *)((uint8_t *)(v + nv) + ibytes);
    for (int t = 0; t < nfeet; t++)
        tl_world_of(&d, &v[type == 3 ? 3 * t + 2 : t + 2], feet + 3 * t);
    if ((reg[0x17] & 1) && render_tl_tessellates()) {     /* (its own shape normals: tessellated, perhaps) */
        static uint32_t *tri;
        static int tcap;
        if (3 * count > tcap) {
            tcap = 3 * count * 2;
            tri = realloc(tri, sizeof *tri * (size_t)tcap);
        }
        int ntri = tl_triangles(type, count, 0, tri);
        tl_shape(v, (uint32_t)nv, tri, (uint32_t)ntri, 0);
    }
    static int check = -1;
    if (check < 0)
        check = getenv("MHFU_TL_CHECK") != NULL;
    if (check) {        /* MHFU_TL_CHECK (debug): the draw and vertices placed here, as ge_tl.vert would, against
                         * vertex(); and what of the vertex stage the draws use (light kinds, near clipping) */
        static unsigned long bad, all;
        for (int i = 0; i < count; i++) {
            const TlVertex *t = &v[ix[i]];
            float p3[3] = {0, 0, 0};
            if (d.bones) {
                for (uint32_t b = 0; b < d.bones; b++)
                    for (int r = 0; r < 3; r++)
                        p3[r] += (d.bone[3 * b + r][0] * t->pos[0] + d.bone[3 * b + r][1] * t->pos[1] +
                                  d.bone[3 * b + r][2] * t->pos[2] + d.bone[3 * b + r][3]) * t->weights[b];
            } else
                memcpy(p3, t->pos, sizeof p3);
            float m3[3], e3[3];
            for (int r = 0; r < 3; r++)
                m3[r] = d.world[r][0] * p3[0] + d.world[r][1] * p3[1] + d.world[r][2] * p3[2] + d.world[r][3];
            for (int r = 0; r < 3; r++)
                e3[r] = d.view[r][0] * m3[0] + d.view[r][1] * m3[1] + d.view[r][2] * m3[2] + d.view[r][3];
            float cw = d.proj[3] * e3[0] + d.proj[7] * e3[1] + d.proj[11] * e3[2] + d.proj[15];
            float cx = d.proj[0] * e3[0] + d.proj[4] * e3[1] + d.proj[8] * e3[2] + d.proj[12];
            ClipVertex c;
            int th;
            int n = idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i;
            vertex(vaddr, n, l, &c, &th);
            all++;
            if (fabsf(c.cw - cw) > 1e-3f * (1 + fabsf(cw)) || fabsf(c.cx - cx) > 1e-3f * (1 + fabsf(cx))) {
                if (bad++ < 20)
                    fprintf(stderr, "tl check: prim type %d count %d idx %d vertex %d (n %d): cpu %g %g, tl %g %g\n",
                            type, count, idx, i, n, c.cx, c.cw, cx, cw);
            }
        }
        static unsigned long kinds[4], spec, near;
        for (int k = 0; k < 4; k++)
            if (d.flags & (TL_LIGHT_ON << k)) {
                kinds[(int)d.light_pos[k][3]]++;
                spec += d.light_dir[k][3] == 1.0f;
            }
        for (int i = 0; i < count; i++) {
            ClipVertex c;
            int th, n = idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i;
            vertex(vaddr, n, l, &c, &th);
            if (c.cw <= 1e-4f) {
                near++;
                break;
            }
        }
        if (all % 100000 < (unsigned long)count)
            fprintf(stderr, "tl check: %lu of %lu vertices differ; lights directional %lu point %lu spot %lu, specular %lu;"
                    " near-clipped prims %lu\n", bad, all, kinds[0], kinds[1], kinds[2], spec, near);
    }
    if (to_queue) {
        render_queue_commit();
    } else {
        render_tl_draw(type, count, same ? NULL : &d, v, nv, ix, feet, nfeet);
        free(p);
    }
}

/* (recording a frame: MHFU_FPS) A recorded draw the GPU's vertex stage may draw: its vertices (as tl_prim sends them)
 * and triangles' indices kept with the frame (sent once, into its slot, as it becomes the latest - not as recorded:
 * recording waits for the renderer at each draw's texture); in a group with the draw before if they can be drawn as one -
 * the same registers but for those of the vertices' place, upload counters and the primitive itself, the same
 * matrices and texture */
static int tl_reg_free(int r) {
    return r <= 0x11 || r == 0x13 || r == 0x14 || r == 0x2A || r == 0x2B || (r >= 0x3A && r <= 0x3F);
}

static int tl_same_group(const Draw *a, const Draw *b) {
    static uint8_t mask[256];
    static int masked;
    if (!masked) {
        masked = 1;
        for (int r = 0; r < 256; r++)
            mask[r] = !tl_reg_free(r);
    }
    for (int r = 0; r < 256; r++)
        if (mask[r] && a->reg[r] != b->reg[r])
            return 0;
    return !memcmp(a->world, b->world, sizeof a->world) && !memcmp(a->view, b->view, sizeof a->view) &&
           !memcmp(a->proj, b->proj, sizeof a->proj) &&
           !memcmp(a->bones, b->bones, sizeof a->bones[0] * (size_t)draw_bones(a)) && a->tex_slot == b->tex_slot &&
           a->tex_w == b->tex_w && a->tex_h == b->tex_h;
}

#define TL_SLOTS 4                      /* (render.c's: the slots' sizes) */
#define TL_SLOT_VERTICES (128u << 10)
#define TL_SLOT_INDICES (512u << 10)

/* A group's shape normals, if it may be tessellated (lit; render.c: lit by the pixel) */
static void tl_group_shape(Frame *f, const TlGroup *g) {
    if (render_tl_tessellates() && (f->draws[g->first].reg[0x17] & 1))
        tl_shape(f->tlv + g->vertex, g->nvertex, f->tli + g->index, g->nindex / 3, g->vertex);
}

static void tl_record(Frame *f, int type, int count, int idx, const Layout *l) {
    Draw *d = &f->draws[f->n - 1];
    int ntri = type == 3 ? count / 3 : count - 2;
    if (!render_on || f->tl_full || ntri <= 0 || !tl_take(type, 0))
        return;
    uint32_t ni = 3u * (uint32_t)ntri;
    if (f->tl_nv + (uint32_t)count > TL_SLOT_VERTICES || f->tl_ni + ni > TL_SLOT_INDICES) {
        f->tl_full = 1;                 /* (the rest as each is drawn) */
        return;
    }
    if (f->tl_nv + (uint32_t)count > f->tlv_cap) {
        f->tlv_cap = (f->tl_nv + (uint32_t)count) * 2;
        f->tlv = realloc(f->tlv, sizeof *f->tlv * f->tlv_cap);
    }
    if (f->tl_ni + ni > f->tli_cap) {
        f->tli_cap = (f->tl_ni + ni) * 2;
        f->tli = realloc(f->tli, sizeof *f->tli * f->tli_cap);
    }
    TlVertex *v = f->tlv + f->tl_nv;
    for (int i = 0; i < count; i++)
        tl_vertex(vaddr, idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i, l, &v[i]);
    uint32_t base = f->tl_nv;
    tl_triangles(type, count, base, f->tli + f->tl_ni);
    uint32_t first = f->tl_ni;
    f->tl_nv += (uint32_t)count;
    f->tl_ni += ni;
    if (f->ntlg && f->n >= 2) {
        TlGroup *g = &f->tlg[f->ntlg - 1];
        const Draw *before = &f->draws[f->n - 2];
        if (g->first + g->n == f->n - 1 && g->index + g->nindex == first && before->group == f->ntlg - 1 &&
            tl_same_group(before, d)) {
            g->n++;
            g->nindex += ni;
            g->nvertex += (uint32_t)count;
            d->group = f->ntlg - 1;
            return;
        }
    }
    if (f->ntlg)
        tl_group_shape(f, &f->tlg[f->ntlg - 1]);
    if (f->ntlg == f->tlgcap) {
        f->tlgcap = f->tlgcap ? f->tlgcap * 2 : 1024;
        f->tlg = realloc(f->tlg, sizeof *f->tlg * (size_t)f->tlgcap);
    }
    f->tlg[f->ntlg] = (TlGroup){f->n - 1, 1, first, ni, base, (uint32_t)count, -1, 0};
    d->group = f->ntlg++;
}

/* A recorded frame's vertices and indices into its slot */
static void tl_send(Frame *f) {
    if (!f->tl_nv || !render_on)
        return;
    if (f->ntlg)
        tl_group_shape(f, &f->tlg[f->ntlg - 1]);
    int slot = (int)(f->tl_gen % TL_SLOTS);
    if (!queued()) {
        render_tl_store(slot, 0, f->tlv, (int)f->tl_nv, 0, f->tli, (int)f->tl_ni);
        return;
    }
    size_t vb = sizeof(TlVertex) * f->tl_nv, ib = 4 * (size_t)f->tl_ni;
    uint32_t *at = q_cmd(Q_TL_STORE, (uint32_t)slot, f->tl_nv, f->tl_ni, 16 + vb + ib);
    at[0] = at[1] = at[2] = at[3] = 0;
    memcpy(at + 4, f->tlv, vb);
    memcpy((uint8_t *)(at + 4) + vb, f->tli, ib);
    render_queue_commit();
}

/* (replaying) A group as one: its draw as replay() placed its first, its triangles from the latest frame's slot */
/* (lit: the feet) A group's lowest vertices as it is drawn now - each triangle's third corner within 65 units of the
 * lowest (the feet's 25 and room for the moves between frames shown) - kept with the frame: once a frame recorded */
static void tl_group_feet(Frame *f, TlGroup *g, const TlDraw *d) {
    static float *ys;
    static uint32_t ycap;
    uint32_t ntri = g->nindex / 3;
    if (ntri > ycap) {
        ycap = ntri * 2;
        ys = realloc(ys, sizeof *ys * ycap);
    }
    float lo = 1e30f;
    for (uint32_t t = 0; t < ntri; t++) {
        float w[3];
        tl_world_of(d, &f->tlv[f->tli[g->index + 3 * t + 2]], w);
        ys[t] = w[1];
        lo = fminf(lo, w[1]);
    }
    g->feet = (int)f->tl_nfeet;
    g->nfeet = 0;
    for (uint32_t t = 0; t < ntri; t++)
        if (ys[t] < lo + 65.0f) {
            if (f->tl_nfeet == f->tl_feet_cap) {
                f->tl_feet_cap = f->tl_feet_cap ? f->tl_feet_cap * 2 : 4096;
                f->tl_feet = realloc(f->tl_feet, sizeof *f->tl_feet * f->tl_feet_cap);
            }
            f->tl_feet[f->tl_nfeet++] = f->tli[g->index + 3 * t + 2];
            g->nfeet++;
        }
}

static void tl_group_draw(const TlGroup *g) {
    static TlDraw d;
    int same = tl_draw_now(&d);
    int slot = (int)(latest->tl_gen % TL_SLOTS);
    tl_groups_drawn++;
    tl_group_draws += (unsigned long)g->n;
    int nfeet = 0;
    if (tl_feet_wanted(&d)) {
        TlGroup *mg = &latest->tlg[g - latest->tlg];
        if (mg->feet < 0)
            tl_group_feet(latest, mg, &d);
        nfeet = mg->nfeet;
    }
    size_t bytes = 16 + (same ? 0 : sizeof d) + 12 * (size_t)nfeet;
    uint8_t *p = queued() ? q_cmd(Q_TL_SLOT, (uint32_t)slot | (same ? 256u : 0u), g->index, g->nindex, bytes)
                          : malloc(bytes);
    memset(p, 0, 16);
    *(uint32_t *)p = (uint32_t)nfeet;
    if (!same)
        memcpy(p + 16, &d, sizeof d);
    float *feet = (float *)(p + 16 + (same ? 0 : sizeof d));
    for (int k = 0; k < nfeet; k++)
        tl_world_of(&d, &latest->tlv[latest->tl_feet[g->feet + k]], feet + 3 * k);
    if (queued()) {
        render_queue_commit();
    } else {
        render_tl_slot_draw(slot, same ? NULL : &d, g->index, g->nindex, feet, nfeet);
        free(p);
    }
}

/* The game's sun flare (models of the map's drawn towards the sun: glares and rainbow rings, in several maps), left out unless
 * MHFU_SUN_FLARE=1: it stood in the middle of the picture wherever the sun was behind the trees or turned from, and moved about with the
 * camera. Told by what its textures are, not which: added onto the picture, and a quarter of a circle round one corner, drawn four times
 * mirrored - so what lies at the same distance from that corner is alike, and the far side black. A fire's glow is round its middle, and
 * nothing else the game adds comes near: over the recordings' 39 textures, these score under 0.04 and the next 0.41. Each texture is looked
 * at once, while it stays at its address. The hashes (as texture packs name them) are glares known from before that the recordings don't reach. */
static const uint32_t flare_hashes[] = {0x16144919u, 0x48BCDF67u};
#define FLARE_BINS 48
static int flare_look(int w, int h) {
    static uint32_t px[256 * 256];
    static float lum[256 * 256];
    gpu_decode_texture(px, w, h, reg);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = px[i];
        lum[i] = (float)((c & 255) + ((c >> 8) & 255) + ((c >> 16) & 255)) / (3 * 255.0f) * (float)(c >> 24) / 255.0f;
    }
    for (int corner = 0; corner < 4; corner++) {
        float cx = corner & 1 ? (float)(w - 1) : 0, cy = corner & 2 ? (float)(h - 1) : 0;
        float sum[FLARE_BINS] = {0}, sq[FLARE_BINS] = {0};
        int n[FLARE_BINS] = {0};
        float all = 0, allsq = 0, far = 0;
        int nfar = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float dx = ((float)x - cx) / (float)w, dy = ((float)y - cy) / (float)h, d = sqrtf(dx * dx + dy * dy);
                float v = lum[y * w + x];
                int k = (int)(d * 32);
                if (k >= FLARE_BINS)
                    k = FLARE_BINS - 1;
                sum[k] += v;
                sq[k] += v * v;
                n[k]++;
                all += v;
                allsq += v * v;
                if (d >= 1.2f)
                    far += v, nfar++;
            }
        float count = (float)(w * h), mean = all / count, total = allsq / count - mean * mean, within = 0;
        for (int k = 0; k < FLARE_BINS; k++)
            if (n[k])
                within += sq[k] - sum[k] * sum[k] / (float)n[k];
        within /= count;
        if (total > 1e-5f && within / total < 0.1f && (nfar == 0 || far / (float)nfar < 0.02f))
            return 1;
    }
    return 0;
}
static int flare_texture(void) {
    static int keep = -1;
    if (keep < 0)
        keep = getenv("MHFU_SUN_FLARE") && atoi(getenv("MHFU_SUN_FLARE")) != 0;
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    if (keep || !(reg[0x1E] & 1) || !(reg[0x21] & 1) || ((reg[0xDF] >> 4) & 15) != 10 || w != h || w < 16 || w > 256 || (reg[0x12] >> 23 & 1))
        return 0;
    uint32_t addr = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    static struct { uint32_t addr, first[4]; int8_t flare; } seen[64];
    static int nseen;
    if (addr < 0x08800000u || addr >= 0x0A000000u)  /* (the game's memory: not VRAM) */
        return 0;
    int k;
    for (k = 0; k < nseen; k++)
        if (seen[k].addr == addr)
            break;
    if (k < nseen && !memcmp(seen[k].first, PTR(addr), 16))
        return seen[k].flare;
    if (k == nseen)
        k = nseen < 64 ? nseen++ : (int)(addr >> 4 & 63);
    uint64_t key;
    uint32_t hash;
    texpack_key(reg, 512, &key, &hash);
    seen[k].addr = addr;
    memcpy(seen[k].first, PTR(addr), 16);
    seen[k].flare = (int8_t)flare_look(w, h);
    for (size_t i = 0; i < sizeof flare_hashes / sizeof *flare_hashes; i++)
        if (hash == flare_hashes[i])
            seen[k].flare = 1;
    return seen[k].flare;
}

/* The Snowy Mountains' haze drawn (patches.py: its mist dome's and its snowstorm's draws), unless MHFU_SNOW_HAZE=0 */
int rt_snow_haze(void) {
    static int on = -1;
    if (on < 0)
        on = !getenv("MHFU_SNOW_HAZE") || atoi(getenv("MHFU_SNOW_HAZE")) != 0;
    return on;
}

static void prim_drawn(uint32_t data);

/* The HUD smaller in the hunt (MHFU_HUD_SCALE, e.g. 0.75; 1: as the game draws it): its parts - the gauges (the clock,
 * health, stamina, sharpness, the name), the map, the item bar - each scaled towards its corner of the screen (top left,
 * top right, bottom right), whole, its text with it. A part is told by what is drawing (patches.py HUD: rt_hud_part):
 * the chunks linked while it is (2D draws: their commands' addresses), and the texts it queues (drawn later, by the
 * text's own code: their entries). The menus' windows, the supply box and other windows drawn among them are no part */
#define HUD_CHUNKS 1024
static struct { uint32_t start, end; unsigned long frame; int part; } hud_chunks[HUD_CHUNKS];
static int hud_chunk_next, hud_part;
#define HUD_TEXTS 256
static struct { uint32_t entry; unsigned long frame; int part; } hud_texts[HUD_TEXTS];
static int hud_text_next;
static unsigned long gauges_frame;  /* (the last frame the gauges were drawn) */
void rt_hud_part(int part) {
    hud_part = part;
    if (part == 1)
        gauges_frame = present_frame_number();
}
int gpu_hud_part(void) { return hud_part; }
void rt_hud_text_queued(uint32_t entry) {
    hud_texts[hud_text_next].entry = entry;
    hud_texts[hud_text_next].frame = present_frame_number();
    hud_texts[hud_text_next].part = hud_part;
    hud_text_next = (hud_text_next + 1) % HUD_TEXTS;
}
void rt_hud_text_drawn(uint32_t entry) {
    unsigned long now = present_frame_number();
    hud_part = 0;
    for (int i = 0, k = hud_text_next; i < HUD_TEXTS; i++) {   /* (the latest entry there, queued this frame: the
                                                                   * text's code draws them in the same pass) */
        k = (k + HUD_TEXTS - 1) % HUD_TEXTS;
        if (hud_texts[k].frame != now)
            break;
        if (hud_texts[k].entry == entry) {
            hud_part = hud_texts[k].part;
            return;
        }
    }
    /* a text kept from before (the hunter's name, given once): the gauges' if it is on their name plate, while they
     * are drawn (its place: the entry's first two halfwords) */
    int16_t x = (int16_t)LH(entry), y = (int16_t)LH(entry + 2);
    if (gauges_frame + 3 >= now && x >= 0 && x < 110 && y >= 50 && y < 80)
        hud_part = 1;
}
void gpu_hud_chunk(uint32_t start, uint32_t end) {
    if (!hud_part)
        return;
    hud_chunks[hud_chunk_next].start = start;
    hud_chunks[hud_chunk_next].end = end;
    hud_chunks[hud_chunk_next].frame = present_frame_number();
    hud_chunks[hud_chunk_next].part = hud_part;
    hud_chunk_next = (hud_chunk_next + 1) % HUD_CHUNKS;
}
/* The HUD's part a command (its address) is in: a chunk it linked the frame before - the list the GE runs is the one
 * made then (they are double-buffered: one two frames old is in the same buffer, other draws where its were - matched,
 * they were scaled into a corner, flashing); 0 none */
static int hud_command(uint32_t at) {
    at &= 0x0FFFFFFFu;
    unsigned long now = present_frame_number();
    for (int i = 0, k = hud_chunk_next; i < HUD_CHUNKS; i++) {
        k = (k + HUD_CHUNKS - 1) % HUD_CHUNKS;
        if (hud_chunks[k].frame + 1 < now || hud_chunks[k].end == 0)
            break;
        if (hud_chunks[k].frame + 1 == now && at >= hud_chunks[k].start && at < hud_chunks[k].end)
            return hud_chunks[k].part;
    }
    return 0;
}
extern uint32_t rt_local_player;
static float hud_scale_value(void) {
    static float s = -1;
    if (s < 0) {
        s = getenv("MHFU_HUD_SCALE") ? (float)atof(getenv("MHFU_HUD_SCALE")) : 1.0f;
        if (!(s >= 0.3f && s <= 1.5f))
            s = 1.0f;
    }
    return s;
}

/* A part's corner (ax, ay, PSP pixels) and the scale: 0 if it is not scaled */
int gpu_hud_anchor(int part, float *ax, float *ay, float *scale) {
    *scale = hud_scale_value();
    if (*scale == 1.0f || part < 1 || part > 3)
        return 0;
    *ax = part == 1 ? 0 : 480;
    *ay = part == 3 ? 272 : 0;
    return 1;
}

static void hud_scaled(ClipVertex *v, int n) {
    float ax, ay, k;
    if (n <= 0 || !gpu_hud_anchor(draw_hud, &ax, &ay, &k))
        return;
    for (int i = 0; i < n; i++) {
        v[i].cx = ax + (v[i].cx - ax) * k;
        v[i].cy = ay + (v[i].cy - ay) * k;
    }
}

/* A primitive: kept, if gated code draws it (for the frames between 30ths); the HUD's smoothed (above) */
static void prim(uint32_t data) {
    int count = data & 0xFFFF, j = -1;
    uint32_t vt = reg[0x12];
    if (!held_drawing && !in_replay)    /* (the HUD's part: by where its command is) */
        draw_hud = (vt >> 23 & 1) && hud_scale_value() != 1.0f ? hud_command(gpu_cmd_at) : 0;
    if (!in_replay)
        draw_far = !held_drawing && !(vt >> 23 & 1) && proj[11] != 0 ? world_far : 0;    /* (what comes after it: not) */
    if (held_chunk >= 0 && !held_drawing && !in_replay && count > 0) {
        Layout l = layout();
        record(&held, data, &l);
        held_command(0x04000000u | data, 0, held.n - 1);
        if ((vt >> 23 & 1) && !((vt >> 11) & 3) && hud_on()) {
            close_run(&held);
            j = hud_find(held.n - 1);
        }
    } else if (held_drawing && held_index >= 0 && held_index < hud_match_cap && (vt >> 23 & 1) && !((vt >> 11) & 3) &&
               hud_on()) {
        j = hud_match[held_index];
    }
    float t = (float)native_since30() * rt_frame_30ths();
    Layout l = layout();
    if (j < 0 || t >= 1 || count * l.size > 0x40000) {
        prim_drawn(data);
        return;
    }
    uint32_t v = vaddr;
    hud_blend(j, v, count, &l, t);
    vaddr = SCRATCH_HUD;
    prim_drawn(data);
    if (recording)
        close_run(rec);             /* (its copy made now: the scratch area is used again) */
    vaddr = v + (uint32_t)(count * l.size);
}

/* MHFU_TEX_LOG=<dir>: each texture drawn blended onto what's behind it (additive or a fixed blend), once per address and hash: a line in
 * <dir>/log.tsv and its pixels in <dir>/<hash>.rgba, to look over what the game draws as glows and flares */
extern unsigned long present_frame_number(void);
static void tex_log(uint32_t prims) {
    static const char *dir;
    static int read;
    static FILE *log;
    if (!read) {
        read = 1;
        dir = getenv("MHFU_TEX_LOG");
    }
    if (!dir || !(reg[0x1E] & 1))
        return;
    uint32_t blend = reg[0xDF] & 0xFFFFFF;
    int src = blend & 15, dst = (blend >> 4) & 15;
    if (!(reg[0x21] & 1) || dst != 10 || src > 10)      /* (blending on; the destination's factor fixed) */
        return;
    uint64_t key;
    uint32_t hash;
    static struct { uint32_t hash; unsigned long count; } seen[4096];
    static int nseen;
    texpack_key(reg, 512, &key, &hash);
    for (int k = 0; k < nseen; k++)
        if (seen[k].hash == hash) {
            seen[k].count += prims;
            return;
        }
    if (nseen == 4096)
        return;
    seen[nseen++].hash = hash;
    int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
    if (!log) {
        char path[1024];
        snprintf(path, sizeof path, "%s/log.tsv", dir);
        log = fopen(path, "w");
    }
    uint32_t addr = (reg[0xA0] & 0xFFFFF0) | ((reg[0xA8] & 0x0F0000) << 8);
    fprintf(log, "%08X\t%08X\t%d\t%d\t%lu\t%06X\t%06X\t%06X\t%u\t%d\t%d\n", hash, addr, w, h, present_frame_number(), blend, reg[0xE0] & 0xFFFFFF,
            reg[0xE1] & 0xFFFFFF, prims, (reg[0x12] >> 23) & 1, flare_texture());
    fflush(log);
    if (w <= 512 && h <= 512) {
        static uint32_t px[512 * 512];
        gpu_decode_texture(px, w, h, reg);
        char path[1024];
        snprintf(path, sizeof path, "%s/%08X.rgba", dir, hash);
        FILE *o = fopen(path, "wb");
        if (o) {
            fwrite(px, 4, (size_t)(w * h), o);
            fclose(o);
        }
    }
}

/* MHFU_TEX_HIDE=hash,hash,...: textures left out, by their hash, to see what one is in the picture */
static int tex_hidden(void) {
    static uint32_t hide[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("MHFU_TEX_HIDE");
        while (e && *e && n < 32) {
            char *end;
            hide[n++] = (uint32_t)strtoul(e, &end, 16);
            e = *end ? end + 1 : end;
        }
    }
    if (!n || !(reg[0x1E] & 1))
        return 0;
    uint64_t key;
    uint32_t hash;
    texpack_key(reg, 512, &key, &hash);
    for (int i = 0; i < n; i++)
        if (hide[i] == hash)
            return 1;
    return 0;
}

/* -- the click inspector: what the frame shown drew, by the pixel (inspect.c) --------------------------------------------
 *
 * While the inspector is open each draw's triangles are noted as they are placed on the screen - its registers kept, for
 * the texture's hash and how it draws - either as the game draws them (each game frame) or, with recorded frames, going over
 * a frame just drawn, as asked. Then for a pixel: all of the frame's triangles in the order drawn, into the framebuffer
 * shown, tested as the GE would for that one pixel. Near clipping comes with testing them in homogeneous coordinates, where
 * a triangle crossing the eye's plane is whole. The world matrix already has the camera shown in it, the same as what is
 * drawn. Nothing here is written to the game or the renderer. */
typedef struct {
    uint32_t reg[256];
    uint32_t target, vaddr;
    int type, count, through;
} PickDraw;
typedef struct {
    float v[3][4];                      /* x, y, depth, w: x / w, y / w, depth / w on the screen */
    float x0, y0, x1, y1;               /* where it can be, on the screen */
    int draw;
} PickTri;
typedef struct {
    PickDraw *d;
    PickTri *t;
    int nd, dcap, nt, tcap;
    uint32_t display;
} PickSet;
static PickSet pick_sets[2];
static int pick_filling;
static unsigned pick_generation;
static uint32_t pick_source_vaddr;      /* (a recorded draw: where its vertices were) */

static void pick_set_source(uint32_t v) {
    pick_source_vaddr = v;
}

static void pick_begin(void) {
    pick_sets[pick_filling].nd = pick_sets[pick_filling].nt = 0;
    pick_collecting = 1;
}

static void pick_end(uint32_t display_addr) {
    pick_collecting = 0;
    PickSet *s = &pick_sets[pick_filling];
    if (!s->nd)
        return;                         /* (a frame not drawn: what there was stays) */
    s->display = 0x04000000u | (display_addr & 0x1FFFFFu);
    pick_filling ^= 1;
    pick_generation++;
}

/* Where a vertex lands, up to clip space: as vertex() has it, no more */
static void pick_vertex(uint32_t base, int n, const Layout *l, ClipVertex *out) {
    uint32_t vt = reg[0x12];
    int wt = (vt >> 9) & 3, nweights = ((vt >> 14) & 7) + 1, through = (vt >> 23) & 1;
    uint32_t v = base + (uint32_t)(n * l->size);
    float p[3];
    read_position(v, l, vt, p);
    if (through) {
        out->cx = p[0];
        out->cy = p[1];
        out->cz = p[2];
        out->cw = 1.0f;
        return;
    }
    if (wt) {
        static const int tsz[4] = {0, 1, 2, 4};
        float acc[3] = {0, 0, 0};
        for (int b = 0; b < nweights; b++) {
            float w = read_comp(v + l->wo + (uint32_t)(b * tsz[wt]), wt, 0, 0), t[3];
            if (wt == 1) w = LBU(v + l->wo + (uint32_t)b) / 128.0f;
            if (wt == 2) w = LHU(v + l->wo + (uint32_t)(b * 2)) / 32768.0f;
            mul43(bones[b], p, t);
            for (int i = 0; i < 3; i++)
                acc[i] += t[i] * w;
        }
        memcpy(p, acc, sizeof p);
    }
    float m[3], e[3];
    mul43(world, p, m);
    mul43(view, m, e);
    out->cx = e[0] * proj[0] + e[1] * proj[4] + e[2] * proj[8] + proj[12];
    out->cy = e[0] * proj[1] + e[1] * proj[5] + e[2] * proj[9] + proj[13];
    out->cz = e[0] * proj[2] + e[1] * proj[6] + e[2] * proj[10] + proj[14];
    out->cw = e[0] * proj[3] + e[1] * proj[7] + e[2] * proj[11] + proj[15];
}

/* The viewport (x, y, depth: scale and centre) and the screen's offset */
static void pick_viewport(const uint32_t *r, float *vp) {
    for (int i = 0; i < 6; i++)
        vp[i] = ge_float(r[0x42 + i]);
    vp[6] = (float)(r[0x4C] & 0xFFFF) / 16.0f;
    vp[7] = (float)(r[0x4D] & 0xFFFF) / 16.0f;
}

static void pick_tri(PickSet *s, const ClipVertex *a, const ClipVertex *b, const ClipVertex *c, int through, const float *vp) {
    if (s->nt == s->tcap) {
        s->tcap = s->tcap ? s->tcap * 2 : 65536;
        s->t = realloc(s->t, sizeof *s->t * (size_t)s->tcap);
    }
    PickTri *t = &s->t[s->nt];
    const ClipVertex *in[3] = {a, b, c};
    int front = 1;
    t->x0 = t->y0 = 1e30f;
    t->x1 = t->y1 = -1e30f;
    for (int i = 0; i < 3; i++) {
        float x = in[i]->cx, y = in[i]->cy, z = in[i]->cz, w = in[i]->cw;
        if (!through) {
            x = x * vp[0] + w * (vp[3] - vp[6]);
            y = y * vp[1] + w * (vp[4] - vp[7]);
            z = z * vp[2] + w * vp[5];
        }
        t->v[i][0] = x, t->v[i][1] = y, t->v[i][2] = z, t->v[i][3] = w;
        if (w > 1e-3f) {
            float sx = x / w, sy = y / w;
            if (sx < t->x0) t->x0 = sx;
            if (sx > t->x1) t->x1 = sx;
            if (sy < t->y0) t->y0 = sy;
            if (sy > t->y1) t->y1 = sy;
        } else
            front = 0;
    }
    if (!front) {                       /* (crossing the eye's plane: it could be anywhere) */
        t->x0 = t->y0 = -1e30f;
        t->x1 = t->y1 = 1e30f;
    }
    if (t->x1 < -64 || t->y1 < -64 || t->x0 > 1024 || t->y0 > 1024)
        return;                         /* (off any screen) */
    t->draw = s->nd - 1;
    s->nt++;
}

static void pick_note(int type, int count, int idx, const Layout *l) {
    if (type < 3 || type > 6)
        return;
    PickSet *s = &pick_sets[pick_filling];
    if (s->nd == s->dcap) {
        s->dcap = s->dcap ? s->dcap * 2 : 4096;
        s->d = realloc(s->d, sizeof *s->d * (size_t)s->dcap);
    }
    PickDraw *p = &s->d[s->nd++];
    memcpy(p->reg, reg, sizeof reg);
    int through = (reg[0x12] >> 23) & 1;
    p->target = fb_addr();
    p->vaddr = pick_pass ? pick_source_vaddr : vaddr;
    p->type = type;
    p->count = count;
    p->through = through;
    static ClipVertex *cv;
    static int cap;
    if (count > cap) {
        cap = count * 2;
        cv = realloc(cv, sizeof *cv * (size_t)cap);
    }
    for (int i = 0; i < count; i++)
        pick_vertex(vaddr, idx == 1 ? LBU(iaddr + (uint32_t)i) : idx == 2 ? LHU(iaddr + 2 * (uint32_t)i) : i, l, &cv[i]);
    if (through)
        hud_scaled(cv, count);
    float vp[8];
    pick_viewport(reg, vp);
    if (type == 3) {
        for (int i = 0; i + 2 < count; i += 3)
            pick_tri(s, &cv[i], &cv[i + 1], &cv[i + 2], through, vp);
    } else if (type == 4) {
        for (int i = 0; i + 2 < count; i++)
            if (i & 1)
                pick_tri(s, &cv[i + 1], &cv[i], &cv[i + 2], through, vp);
            else
                pick_tri(s, &cv[i], &cv[i + 1], &cv[i + 2], through, vp);
    } else if (type == 5) {
        for (int i = 1; i + 1 < count; i++)
            pick_tri(s, &cv[0], &cv[i], &cv[i + 1], through, vp);
    } else {                            /* sprites: two corners, as two triangles on the screen, the second corner's depth */
        for (int i = 0; i + 1 < count; i += 2) {
            ClipVertex c[4];
            float x[2], y[2], z = cv[i + 1].cz;
            for (int k = 0; k < 2; k++) {
                const ClipVertex *q = &cv[i + k];
                x[k] = through ? q->cx : q->cx / q->cw * vp[0] + vp[3] - vp[6];
                y[k] = through ? q->cy : q->cy / q->cw * vp[1] + vp[4] - vp[7];
            }
            if (!through)
                z = cv[i + 1].cz / cv[i + 1].cw * vp[2] + vp[5];
            for (int k = 0; k < 4; k++) {
                c[k].cx = x[k == 1 || k == 2];
                c[k].cy = y[k >= 2];
                c[k].cz = z;
                c[k].cw = 1;
            }
            pick_tri(s, &c[0], &c[1], &c[2], 1, vp);
            pick_tri(s, &c[0], &c[2], &c[3], 1, vp);
        }
    }
}

void gpu_pick_enable(int on) {
    pick_on = on;
    pick_collecting = 0;
    pick_camera_ok = 0;
    if (!on)
        for (int k = 0; k < 2; k++) {
            free(pick_sets[k].d);
            free(pick_sets[k].t);
            memset(&pick_sets[k], 0, sizeof pick_sets[k]);
        }
}

void gpu_pick_request(void) {
    pick_wanted = 1;
}

/* present.c, each game frame: the frame as the game draws it is done, the next gone over (not with recorded frames) */
void gpu_pick_frame(uint32_t display_addr) {
    if (!pick_on || recording)
        return;
    if (pick_collecting)
        pick_end(display_addr);
    pick_begin();
}

unsigned gpu_pick_generation(void) {
    return pick_generation;
}

/* The camera shown (recorded frames: as gpu_render_frame has it now): the view that is in the world matrices, and the GE's
 * view and projection and viewport from the frame's last draw in 3D into what is shown */
static void pick_camera_of(const float *v, const float *gv, const float *pr, const uint32_t *r) {
    memcpy(pick_view, v, sizeof pick_view);
    memcpy(pick_geview, gv, sizeof pick_geview);
    memcpy(pick_proj, pr, sizeof pick_proj);
    pick_viewport(r, pick_vp);
    pick_camera_ok = 1;
}

static void pick_camera_recorded(void) {
    float inv[12], target[3], game[12];
    const float *v = native_camera ? shown_view : NULL;
    if (!v) {
        if (!camera_game(inv, target))
            return;
        camera_invert(inv, game);
        v = game;
    }
    for (int i = latest->n - 1; i >= 0; i--) {
        const Draw *d = &latest->draws[i];
        if (d->proj[11] != 0 && !((d->reg[0x12] >> 23) & 1)) {
            pick_camera_of(v, d->view, d->proj, d->reg);
            return;
        }
    }
}

/* A point in the world, on the screen as shown */
int gpu_pick_project(const float *p, float *x, float *y) {
    if (!pick_camera_ok)
        return 0;
    float c[3], e[3];
    mul43(pick_view, p, c);
    mul43(pick_geview, c, e);
    float cx = e[0] * pick_proj[0] + e[1] * pick_proj[4] + e[2] * pick_proj[8] + pick_proj[12];
    float cy = e[0] * pick_proj[1] + e[1] * pick_proj[5] + e[2] * pick_proj[9] + pick_proj[13];
    float cw = e[0] * pick_proj[3] + e[1] * pick_proj[7] + e[2] * pick_proj[11] + pick_proj[15];
    if (!(cw > 1e-3f))
        return 0;
    *x = cx / cw * pick_vp[0] + pick_vp[3] - pick_vp[6];
    *y = cy / cw * pick_vp[1] + pick_vp[4] - pick_vp[7];
    return 1;
}

/* The draws that end up in a pixel of the frame gone over, the top first: its triangles tested in the order drawn */
int gpu_pick_at(float x, float y, GpuPickHit *out, int max) {
    const PickSet *s = &pick_sets[pick_filling ^ 1];
    if (!s->nd || max <= 0)
        return 0;
    enum { STACK = 64 };
    int stack[STACK], depths = 0, have_z = 0;
    float stack_z[STACK], depth = 0;
    /* (if nothing is drawn into the frame shown, as told: its framebuffer's address isn't what the display has - all of them) */
    int any = 0;
    for (int i = 0; i < s->nd && !any; i++)
        any = s->d[i].target == s->display;
    int last_draw = -1;
    for (int i = 0; i < s->nt; i++) {
        const PickTri *t = &s->t[i];
        if (x < t->x0 || x > t->x1 || y < t->y0 || y > t->y1)
            continue;
        const PickDraw *p = &s->d[t->draw];
        const uint32_t *r = p->reg;
        if (any && p->target != s->display)
            continue;
        int sx0 = r[0xD4] & 0x3FF, sy0 = (r[0xD4] >> 10) & 0x3FF, sx1 = r[0xD5] & 0x3FF, sy1 = (r[0xD5] >> 10) & 0x3FF;
        if (x < (float)sx0 || y < (float)sy0 || x >= (float)(sx1 + 1) || y >= (float)(sy1 + 1))
            continue;
        const float *a = t->v[0], *b = t->v[1], *c = t->v[2];
        /* M = (a b c), its columns x, y, w; M l = (x, y, 1): the point's weights, all at least 0 inside */
        float bc[3] = {b[1] * c[3] - b[3] * c[1], b[3] * c[0] - b[0] * c[3], b[0] * c[1] - b[1] * c[0]};
        float ca[3] = {c[1] * a[3] - c[3] * a[1], c[3] * a[0] - c[0] * a[3], c[0] * a[1] - c[1] * a[0]};
        float ab[3] = {a[1] * b[3] - a[3] * b[1], a[3] * b[0] - a[0] * b[3], a[0] * b[1] - a[1] * b[0]};
        float det = a[0] * bc[0] + a[1] * bc[1] + a[3] * bc[2];
        if (det == 0)
            continue;
        float l0 = (x * bc[0] + y * bc[1] + bc[2]) / det, l1 = (x * ca[0] + y * ca[1] + ca[2]) / det, l2 = (x * ab[0] + y * ab[1] + ab[2]) / det;
        if (l0 < 0 || l1 < 0 || l2 < 0)
            continue;
        if (!p->through && (r[0x1D] & 1)) {   /* culled, as the GE would; the area's sign on the screen */
            float area = det * a[3] * b[3] * c[3];
            if ((area < 0) == (int)(r[0x9B] & 1))
                continue;
        }
        float z = l0 * a[2] + l1 * b[2] + l2 * c[2];
        z = z < 0 ? 0 : z > 65535 ? 65535 : z;
        if (r[0xD3] & 1) {              /* clear mode: what it clears, gone from under it */
            if (r[0xD3] & 0x400)
                depth = z, have_z = 1;
            if (r[0xD3] & 0x100)
                depths = 0, last_draw = -1;
            continue;
        }
        int testing = r[0x23] & 1;
        if (testing && have_z) {
            int pass;
            switch (r[0xDE] & 7) {
            case 0: pass = 0; break;
            case 1: pass = 1; break;
            case 2: pass = z == depth; break;
            case 3: pass = z != depth; break;
            case 4: pass = z < depth; break;
            case 5: pass = z <= depth; break;
            case 6: pass = z > depth; break;
            default: pass = z >= depth; break;
            }
            if (!pass)
                continue;
        }
        int alpha_tested = r[0x22] & 1, blending = r[0x21] & 1;
        /* (what an alpha test may leave a hole in isn't taken as hiding what follows it) */
        if (testing && !(r[0xE7] & 1) && !alpha_tested)
            depth = z, have_z = 1;
        if (!blending && !alpha_tested)
            depths = 0, last_draw = -1;  /* (all that was under it is hidden) */
        if (t->draw == last_draw && depths > 0) {
            stack_z[depths - 1] = z;
            continue;
        }
        if (depths == STACK) {
            memmove(stack, stack + 1, sizeof stack[0] * (STACK - 1));
            memmove(stack_z, stack_z + 1, sizeof stack_z[0] * (STACK - 1));
            depths--;
        }
        stack[depths] = t->draw;
        stack_z[depths++] = z;
        last_draw = t->draw;
    }
    /* top first: 2D as drawn, over the 3D by how near it is - by the depth, as what an alpha test may have left holes in
     * didn't hide what was drawn after it */
    for (int i = 1; i < depths; i++) {
        int d = stack[i];
        float zi = stack_z[i];
        const PickDraw *pi = &s->d[d];
        int near_high = (pi->reg[0xDE] & 7) >= 6, j = i;
        while (j > 0 && !s->d[stack[j - 1]].through && !pi->through &&
               (near_high ? stack_z[j - 1] > zi : stack_z[j - 1] < zi)) {
            stack[j] = stack[j - 1];
            stack_z[j] = stack_z[j - 1];
            j--;
        }
        stack[j] = d;
        stack_z[j] = zi;
    }
    int n = 0;
    for (int i = depths - 1; i >= 0 && n < max; i--, n++) {
        const PickDraw *p = &s->d[stack[i]];
        const uint32_t *r = p->reg;
        GpuPickHit *h = &out[n];
        memset(h, 0, sizeof *h);
        h->draw = stack[i];
        h->z = stack_z[i];
        h->type = p->type;
        h->count = p->count;
        h->vaddr = p->vaddr;
        h->through = p->through;
        h->blend = r[0x21] & 1;
        h->alpha_test = r[0x22] & 1;
        h->depth_test = r[0x23] & 1;
        h->blend_reg = r[0xDF] & 0xFFFFFF;
        h->textured = r[0x1E] & 1;
        if (h->textured) {
            uint64_t key;
            h->addr = (r[0xA0] & 0xFFFFF0) | ((r[0xA8] & 0x0F0000) << 8);
            h->w = 1 << (r[0xB8] & 0xF);
            h->h = 1 << ((r[0xB8] >> 8) & 0xF);
            h->fmt = r[0xC3] & 0xF;
            texpack_key(r, 512, &key, &h->hash);
        }
    }
    return n;
}

static void prim_drawn(uint32_t data) {
    if (prim_stats < 0)
        prim_stats = getenv("MHFU_PRIM_STATS") && atoi(getenv("MHFU_PRIM_STATS")) != 0;
    if (data & 0xFFFF)
        tex_log(data & 0xFFFF);
    if ((data & 0xFFFF) && (flare_texture() || tex_hidden())) {      /* (left out: its vertices passed over) */
        int n = data & 0xFFFF, idx = (reg[0x12] >> 11) & 3;
        if (idx)
            iaddr += (uint32_t)(n * (idx == 1 ? 1 : 2));
        else
            vaddr += (uint32_t)(n * layout().size);
        return;
    }
    if (motion_log_on < 0) {
        motion_file = getenv("MHFU_MOTION_LOG") ? fopen(getenv("MHFU_MOTION_LOG"), "w") : NULL;
        motion_log_on = motion_file != NULL;
    }
    int type = (data >> 16) & 7, count = data & 0xFFFF;
    uint32_t vt = reg[0x12];
    int idx = (vt >> 11) & 3;
    Layout l = layout();
    if (pick_pass) {                    /* (the inspector going over a recorded frame: noted, not drawn) */
        if (count > 0)
            pick_note(type, count, idx, &l);
        if (idx)
            iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
        else
            vaddr += (uint32_t)(count * l.size);
        return;
    }
    if (perf_on && !in_replay && count > 0) {   /* (the performance panel: as the game issues them) */
        perf_draws++;
        perf_tris += (unsigned long)(type == 3 ? count / 3 : type == 4 || type == 5 ? (count > 2 ? count - 2 : 0) : type == 6 ? count : 0);
    }
    if (texpack_active() && !in_replay && !held_drawing && count > 0)
        seen_v(count, &l);
    if (recording && count > 0) {
        /* (the native frame rate under MHFU_FPS: what ticks at 30 recorded smoothed, as it would be drawn) */
        int smooth = !in_replay && !(vt >> 23 & 1) && proj[11] != 0 && gpu_native_smoothing();
        float sw[12], sb[8][12];
        if (smooth) {
            memcpy(sw, world, sizeof world);
            memcpy(sb, bones, sizeof bones);
            native_place(((uint64_t)vaddr << 32 | (idx ? iaddr : 0)) ^ ((uint64_t)vt << 8 ^ data) * 0x9E3779B97F4A7C15ull);
            if (draw_far)               /* (as the game gave it: replay places it) */
                memcpy(world, sw, sizeof world);
        }
        record(rec, data, &l);
        tl_record(rec, (data >> 16) & 7, count, idx, &l);
        if (smooth) {
            memcpy(world, sw, sizeof world);
            memcpy(bones, sb, sizeof bones);
        }
        if (idx)
            iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
        else
            vaddr += (uint32_t)(count * l.size);
        return;
    }
    if (!in_replay && skip_generation != frame_generation_direct) {     /* a frame's first: drawn or not */
        skip_generation = frame_generation_direct;
        /* every gpu_divisor'th drawn (present.c: the game late), evenly - not as it happens to fall behind */
        gpu_skip_draw = gpu_divisor > 1;
        if (!gpu_skip_draw && render_on)
            ps_frames++;
        /* (the native frame rate: the game's camera now, its 30ths noted, drawn or not) */
        float target[3];
        nat_between = 0;
        if (render_on && gpu_native_smoothing() && camera_game(native_game_inv, target)) {
            camera_invert(native_game_inv, nat_view_now);
            memcpy(nat_target_now, target, sizeof target);
            nat_between = native_camera_between(nat_view_now, target, nat_base, nat_base_target);
        }
    }
    if (gpu_skip_draw && !in_replay && count > 0) {     /* (a frame not drawn: present.c) */
        if (!(vt >> 23 & 1) && proj[11] != 0 && gpu_native_smoothing()) {    /* noted all the same */
            memcpy(nat_world, world, sizeof world);
            memcpy(nat_bones, bones, sizeof bones);
            native_place(((uint64_t)vaddr << 32 | (idx ? iaddr : 0)) ^ ((uint64_t)vt << 8 ^ data) * 0x9E3779B97F4A7C15ull);
            memcpy(world, nat_world, sizeof world);
            memcpy(bones, nat_bones, sizeof bones);
        }
        if (idx)
            iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
        else
            vaddr += (uint32_t)(count * l.size);
        return;
    }
    if (render_on && count > 0) {
        if (!in_replay && shadow_generation != frame_generation_direct) {
            shadow_generation = frame_generation_direct;
            /* (the native frame rate: the camera shown, if it is not the game's) */
            native_shown = 0;
            if (gpu_native_smoothing() && nat_view_ok()) {
                native_shown = camera_native_shown(nat_between ? nat_base : nat_view_now,
                                                   nat_between ? nat_base_target : nat_target_now, native_shown_view);
                if (!native_shown && nat_between) {
                    memcpy(native_shown_view, nat_base, sizeof nat_base);
                    native_shown = 1;
                }
            }
            shadow_frame(native_shown ? native_shown_view : NULL);
        }
        if (!in_replay && !(vt >> 23 & 1) && proj[11] != 0 && gpu_native_smoothing()) {
            memcpy(nat_world, world, sizeof world);     /* (put back after the draw) */
            memcpy(nat_bones, bones, sizeof bones);
            nat_override = 1;
            native_place(((uint64_t)vaddr << 32 | (idx ? iaddr : 0)) ^ ((uint64_t)vt << 8 ^ data) * 0x9E3779B97F4A7C15ull);
            if (native_shown) {             /* with the camera shown: the game's taken out, it put in */
                float w[12];
                compose43(native_game_inv, world, w);
                compose43(native_shown_view, w, world);
            }
            if (draw_far && nat_view_ok()) {
                memcpy(world, nat_world, sizeof world);
                far_place(draw_far, native_shown ? native_shown_view : nat_view_now);
            }
        }
        /* (lit by the GE, in the world: where it was in the frame drawn before; as replay()) */
        if (!in_replay && !(vt >> 23 & 1) && proj[11] != 0 && (reg[0x17] & 1) && shadow_world && render_motion_wanted())
            native_motion(((uint64_t)vaddr << 32 | (idx ? iaddr : 0)) ^ ((uint64_t)vt << 8 ^ data) * 0x9E3779B97F4A7C15ull);
        if (motion_log_on && !fast_forward() && !(vt >> 23 & 1) && proj[11] != 0 && ((vt >> 9) & 3))
            motion_log(in_replay ? replay_key : ((uint64_t)vaddr << 32 | (idx ? iaddr : 0)) ^
                                                ((uint64_t)vt << 8 ^ data) * 0x9E3779B97F4A7C15ull);
        shadow_prim();
        q_begin(reg);
    }
    if (pick_on && !recording && !in_replay && count > 0) {   /* (the inspector: the game's frame as drawn, the camera shown) */
        if (pick_collecting)
            pick_note(type, count, idx, &l);
        if (proj[11] != 0 && !(vt >> 23 & 1)) {
            float inv[12], target[3], game[12];
            if (native_shown)
                pick_camera_of(native_shown_view, view, proj, reg);
            else if (camera_game(inv, target)) {
                camera_invert(inv, game);
                pick_camera_of(game, view, proj, reg);
            }
        }
    }
    if (count > 0 && render_on && replay_group) {
        tl_group_draw(replay_group);
    } else if (count > 0 && render_on && tl_take(type, blend_from != NULL)) {
        uint64_t ps_at = prim_stats > 0 ? ps_now() : 0;
        tl_prim(type, count, idx, &l);
        if (ps_at)
            ps_note(type, 0, count, ps_at);
    } else if (count > 0) {
        /* (the vertices made into the render queue, when there is one - its render thread does the rest) */
        int to_queue = render_on && queued();
        uint32_t *qd = to_queue ? q_cmd(Q_DRAW, 0, 0, 0, 16 + sizeof(ClipVertex) * (size_t)count) : NULL;
        ClipVertex *v = to_queue ? (ClipVertex *)(qd + 4) : malloc(sizeof *v * (size_t)count);
        int through = 0, blend = blend_from && blend_place(count);
        uint64_t ps_at = prim_stats > 0 && render_on ? ps_now() : 0;
        for (int i = 0; i < count; i++) {
            int n = i;
            if (idx == 1) n = LBU(iaddr + (uint32_t)i);
            else if (idx == 2) n = LHU(iaddr + 2 * (uint32_t)i);
            if (blend) {
                blend_reg = blend_from->reg;
                blend_vertex = (uint32_t)(idx == 1 ? LBU(SCRATCH_BLEND_INDICES + (uint32_t)i)
                                          : idx == 2 ? LHU(SCRATCH_BLEND_INDICES + 2 * (uint32_t)i) : i);
            }
            vertex(vaddr, n, &l, &v[i], &through);
        }
        blend_reg = NULL;
        if (through)
            hud_scaled(v, count);
        if (ps_at)
            ps_note(type, blend, count, ps_at);
        if (render_on) {
            if (to_queue) {
                qd[0] = (uint32_t)type, qd[1] = (uint32_t)count, qd[2] = (uint32_t)through, qd[3] = (uint32_t)prim_world;
                render_queue_commit();
            } else {
                emit_render(type, count, v, through, reg, prim_world);
                free(v);
            }
        } else {
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
    }
    if (!in_replay)
        earlier = 0;
    if (nat_override) {                 /* (the matrices as the game gave them, for what comes after) */
        nat_override = 0;
        memcpy(world, nat_world, sizeof world);
        memcpy(bones, nat_bones, sizeof nat_bones);
    }
    /* the vertex (or index) address moves past what was drawn */
    if (idx)
        iaddr += (uint32_t)(count * (idx == 1 ? 1 : 2));
    else
        vaddr += (uint32_t)(count * l.size);
}

/* Block transfer: copy a rectangle of bytes between RAM / VRAM */
static void transfer(uint32_t data) {
    render_queue_sync();                /* (what the renderer has yet to read could be written over) */
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
    if (held_chunk >= 0 && !held_drawing && held_kept(op))
        held_command(cmd, addr, -1);
    reg[op] = data;
    switch (op) {
    case 0x01: vaddr = addr; break;
    case 0x02: iaddr = addr; break;
    case 0x04: prim(data); break;
    case 0x2A: bone_i = (int)(data & 0x7F); break;
    case 0x2B: if (bone_i < 96) bones[bone_i / 12][bone_i % 12] = ge_float(data); bone_i++; break;
    case 0x3A: world_i = (int)(data & 0xF); world_far = far_command(gpu_cmd_at); break;
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
