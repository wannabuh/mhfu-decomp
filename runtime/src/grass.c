/* Ground grass (MHFU_GRASS): blades of our own over the game's ground, where its texture is grass - after ao-vk's
 * (randy-vk rvk/grass.cpp).
 *
 * The ground: render_grass.c hands over the scenery's triangles (lit beforehand, solid) as they are drawn; those near the
 * camera that face up are laid into world-aligned tiles of square cells, each keeping the top surface over it - its
 * height, slope and colour as drawn (the vertex colour, which holds the scenery's light, times the texture) - and
 * whether that colour is grass. A triangle is laid once: one seen before (its corners, rounded) is skipped, so once the
 * ground about the camera is known a frame costs a lookup a triangle.
 *
 * The blades: once a tile's ground has settled (nothing new in it for a few frames) its blades are made - scattered in
 * tufts over its grass cells, thinning and shortening towards the edges of the grass, each a 32-byte record - and
 * shuffled, so that drawing the first part of a tile's draws a thinner field of the same look (the distance's level of
 * detail). render_grass.c uploads them into a slot of its blade pool and draws them (grass.vert expands each into a blade).
 *
 * The game's zones have their own ground at the same coordinates: after a while with no scenery drawn (loading), or the
 * camera's jump, the next frames' ground is checked against what is known - mostly new, it is another zone, and all is
 * forgotten. */
#include "grass.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CELL 10.0f                  /* world units (about centimetres) */
#define TILE_CELLS 32
#define TILE (CELL * TILE_CELLS)
#define BLADES_PER_CELL 8
#define SETTLE 8                    /* frames without anything new before a tile's blades are made */
#define MAX_TILES 4096
#define PROBE_FRAMES 15

typedef struct {
    float y;                        /* the top surface's height at the cell's centre */
    uint8_t r, g, b;
    int8_t nx, nz;                  /* its slope: the normal's x, z (times 127) */
    uint8_t state;                  /* 0 unknown, 1 ground, 2 grass */
} Cell;

typedef struct {
    int tx, tz, used;
    Cell cells[TILE_CELLS * TILE_CELLS];
    unsigned long changed;          /* the frame something was last laid in it */
    int dirty;                      /* laid since its blades were made */
    int slot;                       /* its blades' slot in the pool (-1: none) */
    int nblades;
    float lo, hi;                   /* the heights of its ground */
} Tile;

static Tile *tiles;
static int ntiles;
static int *tile_index;             /* hash: tile key -> index + 1 */
static int index_size;
static uint64_t *seen;              /* the triangles laid: their corners, rounded, hashed (0: empty) */
static size_t seen_size, seen_used;
static unsigned long frame;
static double last_call = -1;       /* the moment (seconds of the game's) scenery was last drawn */
static int probing, probe_known, probe_new;     /* after a gap: frames left to tell by; the ground drawn, known or new */
static int slot_used[GRASS_SLOTS];
static float eye[3];
static float radius = 3000.0f, density = 1.0f;
static GrassBlade build_buf[GRASS_BLADES_PER_SLOT];

void grass_settings(float d, float r) {
    density = d;
    radius = r;
}

float grass_radius(void) {
    return radius;
}

static uint64_t mix64(uint64_t h) {
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    h *= 0x94D049BB133111EBull;
    return h ^ (h >> 32);
}

static uint32_t hash2(int x, int z, uint32_t salt) {
    return (uint32_t)mix64((uint64_t)(uint32_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)z * 0xC2B2AE3D27D4EB4Full ^
                           salt);
}

static float unit(uint32_t h) {
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

/* smooth value noise over the world, 0..1 */
static float noise(float x, float z, uint32_t salt) {
    float fx = floorf(x), fz = floorf(z);
    int xi = (int)fx, zi = (int)fz;
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
    float a = unit(hash2(xi, zi, salt)), b = unit(hash2(xi + 1, zi, salt));
    float c = unit(hash2(xi, zi + 1, salt)), d = unit(hash2(xi + 1, zi + 1, salt));
    return (a + (b - a) * tx) * (1 - tz) + (c + (d - c) * tx) * tz;
}

static void forget(void) {
    for (int i = 0; i < ntiles; i++)
        if (tiles[i].slot >= 0)
            slot_used[tiles[i].slot] = 0;
    ntiles = 0;
    if (tile_index)
        memset(tile_index, 0, sizeof *tile_index * (size_t)index_size);
    if (seen)
        memset(seen, 0, sizeof *seen * seen_size);
    seen_used = 0;
}

static Tile *tile_at(int tx, int tz, int make) {
    if (!tile_index) {
        index_size = 2 * MAX_TILES;
        tile_index = calloc((size_t)index_size, sizeof *tile_index);
        tiles = calloc(MAX_TILES, sizeof *tiles);
    }
    uint32_t h = hash2(tx, tz, 7) & (uint32_t)(index_size - 1);
    for (;;) {
        int i = tile_index[h];
        if (!i)
            break;
        if (tiles[i - 1].tx == tx && tiles[i - 1].tz == tz)
            return &tiles[i - 1];
        h = (h + 1) & (uint32_t)(index_size - 1);
    }
    if (!make || ntiles >= MAX_TILES)
        return NULL;
    Tile *t = &tiles[ntiles];
    memset(t, 0, sizeof *t);
    t->tx = tx;
    t->tz = tz;
    t->slot = -1;
    t->lo = 1e30f;
    t->hi = -1e30f;
    tile_index[h] = ++ntiles;
    return t;
}

/* a triangle seen before (its corners rounded to a unit): 1; else noted */
static int seen_before(const float p[3][3]) {
    uint64_t k = 0x12345;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            k = mix64(k ^ (uint64_t)(int64_t)lroundf(p[i][j]));
    k |= 1;
    if (seen_used * 2 >= seen_size) {
        size_t old = seen_size;
        uint64_t *was = seen;
        seen_size = old ? old * 2 : (1u << 16);
        seen = calloc(seen_size, sizeof *seen);
        seen_used = 0;
        for (size_t i = 0; i < old; i++)
            if (was[i]) {
                size_t j = was[i] & (seen_size - 1);
                while (seen[j])
                    j = (j + 1) & (seen_size - 1);
                seen[j] = was[i];
                seen_used++;
            }
        free(was);
    }
    size_t j = k & (seen_size - 1);
    while (seen[j]) {
        if (seen[j] == k)
            return 1;
        j = (j + 1) & (seen_size - 1);
    }
    seen[j] = k;
    seen_used++;
    return 0;
}

/* grass's colour: green to olive (not brown: red above green; nor grey - snow, rock, paths - blue near green) */
static int green(int r, int g, int b) {
    return g > 24 && g * 100 >= r * 96 && g * 100 > b * 122;
}

void grass_frame(const float camera[3], double now) {
    frame++;
    /* the frames after a gap: their ground mostly new - another zone - all is forgotten. Told by the ground of up to
     * PROBE_FRAMES frames (the first may show little of it, fading in), until enough is seen */
    if (probing) {
        if (probe_new > 32 && probe_new > probe_known) {
            forget();
            probing = 0;
        } else if (probe_new + probe_known >= 256 || !--probing)
            probing = 0;
    }
    float jump = fabsf(camera[0] - eye[0]) + fabsf(camera[2] - eye[2]);
    if (last_call >= 0 && (fabs(now - last_call) > 0.4 || jump > 1500.0f)) {
        probing = PROBE_FRAMES;
        probe_known = probe_new = 0;
    }
    last_call = now;
    memcpy(eye, camera, sizeof eye);
}

void grass_ground(const float p[3][3], const float uv[3][2], const float col[3][3], const uint32_t *tex, int tw,
                  int th, int doubled) {
    if (density <= 0)
        return;
    /* facing up, near */
    float e1[3], e2[3], n[3];
    for (int i = 0; i < 3; i++) {
        e1[i] = p[1][i] - p[0][i];
        e2[i] = p[2][i] - p[0][i];
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(nl > 0))
        return;
    if (n[1] < 0)
        n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    if (n[1] < 0.64f * nl)
        return;
    float x0 = fminf(p[0][0], fminf(p[1][0], p[2][0])), x1 = fmaxf(p[0][0], fmaxf(p[1][0], p[2][0]));
    float z0 = fminf(p[0][2], fminf(p[1][2], p[2][2])), z1 = fmaxf(p[0][2], fmaxf(p[1][2], p[2][2]));
    float reach = radius + TILE;
    if (x1 < eye[0] - reach || x0 > eye[0] + reach || z1 < eye[2] - reach || z0 > eye[2] + reach)
        return;
    /* (one reaching beyond is laid as far as it reaches, and again as the camera comes nearer: not noted) */
    int whole = x0 >= eye[0] - reach && x1 <= eye[0] + reach && z0 >= eye[2] - reach && z1 <= eye[2] + reach;
    int known = whole ? seen_before(p) : 0;
    if (probing)
        known ? probe_known++ : probe_new++;
    if (known)
        return;
    x0 = fmaxf(x0, eye[0] - reach);
    x1 = fminf(x1, eye[0] + reach);
    z0 = fmaxf(z0, eye[2] - reach);
    z1 = fminf(z1, eye[2] + reach);
    /* each cell whose centre it covers (in x, z) */
    float d = e1[0] * e2[2] - e2[0] * e1[2];
    if (fabsf(d) < 1e-6f)
        return;
    int cx0 = (int)floorf(x0 / CELL), cx1 = (int)floorf(x1 / CELL), cz0 = (int)floorf(z0 / CELL), cz1 = (int)floorf(z1 / CELL);
    int8_t snx = (int8_t)lroundf(n[0] / nl * 127), snz = (int8_t)lroundf(n[2] / nl * 127);
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float x = ((float)cx + 0.5f) * CELL, z = ((float)cz + 0.5f) * CELL;
            float px = x - p[0][0], pz = z - p[0][2];
            float b1 = (px * e2[2] - e2[0] * pz) / d, b2 = (e1[0] * pz - px * e1[2]) / d, b0 = 1 - b1 - b2;
            if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
                continue;
            float y = b0 * p[0][1] + b1 * p[1][1] + b2 * p[2][1];
            int tx = (int)floorf((float)cx / TILE_CELLS), tz = (int)floorf((float)cz / TILE_CELLS);
            Tile *t = tile_at(tx, tz, 1);
            if (!t)
                return;
            Cell *c = &t->cells[(cz - tz * TILE_CELLS) * TILE_CELLS + (cx - tx * TILE_CELLS)];
            if (c->state) {
                if (y < c->y + 2.0f)        /* (the top surface wins: under it, or the same, kept) */
                    continue;
            }
            /* its colour as drawn: the vertex colour (the scenery's light) times the texture's there */
            float u = b0 * uv[0][0] + b1 * uv[1][0] + b2 * uv[2][0], v = b0 * uv[0][1] + b1 * uv[1][1] + b2 * uv[2][1];
            float rgb[3] = {1, 1, 1};
            int state = 1;
            if (tex) {
                u -= floorf(u);
                v -= floorf(v);
                int ix = (int)(u * (float)tw), iy = (int)(v * (float)th);
                uint32_t texel = tex[(iy < th ? iy : th - 1) * tw + (ix < tw ? ix : tw - 1)];
                if ((texel >> 24) < 128)
                    continue;
                for (int k = 0; k < 3; k++)
                    rgb[k] = (float)((texel >> (8 * k)) & 255) / 255.0f;
            }
            int out[3];
            for (int k = 0; k < 3; k++) {
                float cc = (b0 * col[0][k] + b1 * col[1][k] + b2 * col[2][k]) * rgb[k] * (doubled ? 2.0f : 1.0f);
                out[k] = cc > 255 ? 255 : cc < 0 ? 0 : (int)cc;
            }
            if (green(out[0], out[1], out[2]))
                state = 2;
            c->y = y;
            c->r = (uint8_t)out[0];
            c->g = (uint8_t)out[1];
            c->b = (uint8_t)out[2];
            c->nx = snx;
            c->nz = snz;
            c->state = (uint8_t)state;
            t->changed = frame;
            t->dirty = 1;
            t->lo = fminf(t->lo, y);
            t->hi = fmaxf(t->hi, y);
        }
}

static int grass_cell(const Tile *t, int cx, int cz) {
    if (cx < 0 || cz < 0 || cx >= TILE_CELLS || cz >= TILE_CELLS) {
        const Tile *o = tile_at(t->tx + (cx < 0 ? -1 : cx >= TILE_CELLS ? 1 : 0),
                                t->tz + (cz < 0 ? -1 : cz >= TILE_CELLS ? 1 : 0), 0);
        if (!o)
            return 1;               /* (not known yet: as if grass, so that a tile's edge does not thin) */
        t = o;
        cx = (cx + TILE_CELLS) % TILE_CELLS;
        cz = (cz + TILE_CELLS) % TILE_CELLS;
    }
    return t->cells[cz * TILE_CELLS + cx].state == 2;
}

static uint32_t pack_rgb(float r, float g, float b) {
    int ri = (int)fminf(fmaxf(r, 0), 255), gi = (int)fminf(fmaxf(g, 0), 255), bi = (int)fminf(fmaxf(b, 0), 255);
    return (uint32_t)ri | (uint32_t)gi << 8 | (uint32_t)bi << 16;
}

/* A tile's blades: scattered over its grass cells in tufts, shorter and fewer towards the grass's edges */
static int build(Tile *t) {
    int n = 0;
    for (int cz = 0; cz < TILE_CELLS; cz++)
        for (int cx = 0; cx < TILE_CELLS; cx++) {
            const Cell *c = &t->cells[cz * TILE_CELLS + cx];
            if (c->state != 2)
                continue;
            int around = grass_cell(t, cx - 1, cz) + grass_cell(t, cx + 1, cz) + grass_cell(t, cx, cz - 1) +
                         grass_cell(t, cx, cz + 1);
            int wx = t->tx * TILE_CELLS + cx, wz = t->tz * TILE_CELLS + cz;
            float ny = sqrtf(fmaxf(1.0f - ((float)c->nx * c->nx + (float)c->nz * c->nz) / (127.0f * 127.0f), 0.1f));
            float nx = (float)c->nx / 127.0f, nz = (float)c->nz / 127.0f;
            for (int k = 0; k < BLADES_PER_CELL && n < GRASS_BLADES_PER_SLOT; k++) {
                uint32_t h = hash2(wx, wz, (uint32_t)k * 0x51ED27u);
                float ox = unit(h) - 0.5f, oz = unit(h >> 7 ^ h << 11) - 0.5f;
                float x = ((float)wx + 0.5f + ox) * CELL, z = ((float)wz + 0.5f + oz) * CELL;
                /* tufts: the clump noise thins the field in patches; the edges of the grass thin it more */
                float clump = 0.7f * noise(x * 0.012f, z * 0.012f, 3) + 0.3f * noise(x * 0.04f, z * 0.04f, 5);
                float keep = 0.25f + 1.1f * clump - 0.15f * (float)(4 - around);
                if (unit(mix64(h)) > keep)
                    continue;
                float tall = noise(x * 0.008f, z * 0.008f, 11);
                float height = (14.0f + 22.0f * tall) * (0.7f + 0.5f * unit(h >> 3)) * (0.55f + 0.45f * (float)around / 4);
                float half = 0.9f + 0.7f * unit(h >> 9);
                float y = c->y - (nx * (x - ((float)wx + 0.5f) * CELL) + nz * (z - ((float)wz + 0.5f) * CELL)) / ny;
                /* its colour: the ground's, varied a little (some drier, some greener) */
                float dry = noise(x * 0.02f, z * 0.02f, 13), vary = 0.88f + 0.24f * unit(h >> 13);
                float r = c->r * vary * (0.96f + 0.12f * dry), g = c->g * vary * (1.02f - 0.02f * dry);
                float b = c->b * vary * (1.02f - 0.1f * dry);
                GrassBlade *bl = &build_buf[n++];
                bl->x = x;
                bl->y = y;
                bl->z = z;
                bl->color = pack_rgb(r, g, b);
                bl->shape = (uint32_t)fminf(height * 4.0f, 255.0f) | (uint32_t)fminf(half * 64.0f, 255.0f) << 8 |
                            (h >> 24 & 255) << 16;
                bl->normal = (uint32_t)(uint8_t)c->nx | (uint32_t)(uint8_t)c->nz << 8 | (h >> 16 & 255) << 16;
                bl->extra = 0;
                bl->pad = 0;
            }
        }
    /* shuffled (the first part of them a thinner field alike), each knowing its place in the order */
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(mix64((uint64_t)i * 0x9E37u ^ (uint64_t)(uint32_t)t->tx << 20 ^ (uint64_t)(uint32_t)t->tz << 40) %
                      (uint64_t)(i + 1));
        GrassBlade s = build_buf[i];
        build_buf[i] = build_buf[j];
        build_buf[j] = s;
    }
    for (int i = 0; i < n; i++)
        build_buf[i].shape |= (uint32_t)(i * 255 / (n > 1 ? n - 1 : 1)) << 24;
    return n;
}

static float tile_distance(const Tile *t) {
    float x0 = (float)t->tx * TILE, z0 = (float)t->tz * TILE;
    float dx = fmaxf(fmaxf(x0 - eye[0], eye[0] - (x0 + TILE)), 0), dz = fmaxf(fmaxf(z0 - eye[2], eye[2] - (z0 + TILE)), 0);
    return sqrtf(dx * dx + dz * dz);
}

int grass_build(int *slot, const GrassBlade **blades, int *count) {
    /* a tile near, settled, whose blades are not made (or are older than its ground): the nearest */
    Tile *best = NULL;
    float best_d = 1e30f;
    for (int i = 0; i < ntiles; i++) {
        Tile *t = &tiles[i];
        if (!t->dirty || frame - t->changed < SETTLE)
            continue;
        float d = tile_distance(t);
        if (d < radius && d < best_d)
            best = t, best_d = d;
    }
    if (!best)
        return 0;
    best->dirty = 0;
    if (best->slot < 0) {
        for (int s = 0; s < GRASS_SLOTS && best->slot < 0; s++)
            if (!slot_used[s])
                best->slot = s, slot_used[s] = 1;
        if (best->slot < 0) {           /* (full: the farthest tile's given up) */
            Tile *far = NULL;
            float far_d = -1;
            for (int i = 0; i < ntiles; i++)
                if (tiles[i].slot >= 0 && tile_distance(&tiles[i]) > far_d)
                    far = &tiles[i], far_d = tile_distance(&tiles[i]);
            if (!far || far_d <= best_d)
                return 0;
            best->slot = far->slot;
            far->slot = -1;
            far->nblades = 0;
            far->dirty = 1;
        }
    }
    best->nblades = build(best);
    *slot = best->slot;
    *blades = build_buf;
    *count = best->nblades;
    return 1;
}

int grass_tiles(GrassDraw *out, int max) {
    int n = 0;
    for (int i = 0; i < ntiles && n < max; i++) {
        Tile *t = &tiles[i];
        if (t->slot < 0 || !t->nblades)
            continue;
        float d = tile_distance(t);
        if (d >= radius)
            continue;
        /* drawn: as many of its blades as the field's density at its nearest point (grass.vert thins each by its
         * own distance) */
        float k = d < GRASS_NEAR ? 1.0f : (GRASS_NEAR / d) * (GRASS_NEAR / d);
        int count = (int)ceilf((float)t->nblades * fminf(fmaxf(k * density, 0.02f), 1.0f));
        out[n].slot = t->slot;
        out[n].count = count;
        out[n].box[0] = (float)t->tx * TILE;
        out[n].box[1] = t->lo;
        out[n].box[2] = (float)t->tz * TILE;
        out[n].box[3] = (float)(t->tx + 1) * TILE;
        out[n].box[4] = t->hi + 60.0f;
        out[n].box[5] = (float)(t->tz + 1) * TILE;
        n++;
    }
    return n;
}

/* -- the trodden grass ------------------------------------------------------------------------------------------- */

static float trail[GRASS_TRAIL * GRASS_TRAIL][2];
static int trail_ox = 1 << 30, trail_oz;
static double trail_time = -1;

/* the ground's height at a world x, z (unknown: NAN) */
static float ground_y(float x, float z) {
    int cx = (int)floorf(x / CELL), cz = (int)floorf(z / CELL);
    int tx = (int)floorf((float)cx / TILE_CELLS), tz = (int)floorf((float)cz / TILE_CELLS);
    const Tile *t = tile_at(tx, tz, 0);
    if (!t)
        return NAN;
    const Cell *c = &t->cells[(cz - tz * TILE_CELLS) * TILE_CELLS + (cx - tx * TILE_CELLS)];
    return c->state ? c->y : NAN;
}

void grass_trail(const float *feet, int nfeet, double now, uint32_t *out, int *origin_x, int *origin_z) {
    const int n = GRASS_TRAIL, mask = GRASS_TRAIL - 1;
    float dt = trail_time >= 0 ? (float)(now - trail_time) : 0;
    if (dt < 0 || dt > 0.25f)
        dt = 0;
    trail_time = now;
    /* the window, about the foot near its ground nearest the camera (the hunter's: not the middle of all - monsters
     * come into view and leave it as the camera turns, and the window would follow them, the hunter's trail cleared),
     * or the camera: the cells it has left cleared (they come back as the far side's) */
    float cx = eye[0], cz = eye[2], best = 1e30f;
    for (int f = 0; f < nfeet; f++) {
        const float *ft = feet + f * GRASS_FOOT;
        float dx = ft[0] - eye[0], dz = ft[2] - eye[2];
        if (fabsf(ft[1] - ground_y(ft[0], ft[2])) < 70.0f && dx * dx + dz * dz < best) {
            best = dx * dx + dz * dz;
            cx = ft[0];
            cz = ft[2];
        }
    }
    int ox = (int)floorf(cx / GRASS_TRAIL_CELL) - n / 2, oz = (int)floorf(cz / GRASS_TRAIL_CELL) - n / 2;
    if (trail_ox != 1 << 30 && abs(ox - trail_ox) < 4 && abs(oz - trail_oz) < 4) {
        ox = trail_ox;              /* (a little way off centre is no matter: the window stays, no cells cleared) */
        oz = trail_oz;
    }
    if (abs(ox - trail_ox) >= n || abs(oz - trail_oz) >= n) {
        memset(trail, 0, sizeof trail);
    } else {
        for (int x = trail_ox; x != ox; x += ox > trail_ox ? 1 : -1) {
            int col = (ox > trail_ox ? x : x + n - 1) & mask;
            for (int z = 0; z < n; z++)
                trail[z * n + col][0] = trail[z * n + col][1] = 0;
        }
        for (int z = trail_oz; z != oz; z += oz > trail_oz ? 1 : -1) {
            int row = (oz > trail_oz ? z : z + n - 1) & mask;
            for (int x = 0; x < n; x++)
                trail[row * n + x][0] = trail[row * n + x][1] = 0;
        }
    }
    trail_ox = ox;
    trail_oz = oz;
    /* springing back */
    float keep = expf(-dt / 1.1f);
    for (int i = 0; i < n * n; i++) {
        trail[i][0] *= keep;
        trail[i][1] *= keep;
    }
    /* pressed by each foot near its ground: the way it goes (standing, aside), most under it */
    float in = 1.0f - expf(-dt * 10.0f);
    for (int f = 0; f < nfeet; f++) {
        const float *ft = feet + f * GRASS_FOOT;
        float gy = ground_y(ft[0], ft[2]);
        if (!(fabsf(ft[1] - gy) < 70.0f))
            continue;
        float r = 30.0f + ft[3], vx = ft[4], vz = ft[5], speed = sqrtf(vx * vx + vz * vz);
        float t = fminf(fmaxf((speed - 15.0f) / 65.0f, 0), 1), going = t * t * (3 - 2 * t);
        int c0x = (int)floorf((ft[0] - r) / GRASS_TRAIL_CELL), c1x = (int)floorf((ft[0] + r) / GRASS_TRAIL_CELL);
        int c0z = (int)floorf((ft[2] - r) / GRASS_TRAIL_CELL), c1z = (int)floorf((ft[2] + r) / GRASS_TRAIL_CELL);
        for (int cz = c0z; cz <= c1z; cz++)
            for (int cx = c0x; cx <= c1x; cx++) {
                if (cx < ox || cz < oz || cx >= ox + n || cz >= oz + n)
                    continue;
                float dx = ((float)cx + 0.5f) * GRASS_TRAIL_CELL - ft[0], dz = ((float)cz + 0.5f) * GRASS_TRAIL_CELL - ft[2];
                float l = sqrtf(dx * dx + dz * dz);
                if (l >= r)
                    continue;
                float u = fminf(fmaxf((l - 0.2f * r) / (0.8f * r), 0), 1), near = 1.0f - u * u * (3 - 2 * u);
                float ax = l > 1e-3f ? dx / l : 0.7071f, az = l > 1e-3f ? dz / l : 0.7071f;
                float wx = ax * (1 - 0.85f * going) + (speed > 1e-3f ? vx / speed : 0) * going;
                float wz = az * (1 - 0.85f * going) + (speed > 1e-3f ? vz / speed : 0) * going;
                float wl = sqrtf(wx * wx + wz * wz);
                if (wl < 1e-4f)
                    continue;
                float tx = wx / wl * near, tz = wz / wl * near;
                float *c = trail[(cz & mask) * n + (cx & mask)];
                if (tx * tx + tz * tz < c[0] * c[0] + c[1] * c[1])    /* (pressed less than it is: left as it is) */
                    continue;
                c[0] += (tx - c[0]) * in;
                c[1] += (tz - c[1]) * in;
            }
    }
    for (int i = 0; i < n * n; i++) {
        int x = (int)lroundf(fminf(fmaxf(trail[i][0], -1), 1) * 32767.0f);
        int z = (int)lroundf(fminf(fmaxf(trail[i][1], -1), 1) * 32767.0f);
        out[i] = (uint32_t)(uint16_t)(int16_t)x | (uint32_t)(uint16_t)(int16_t)z << 16;
    }
    *origin_x = ox;
    *origin_z = oz;
}
