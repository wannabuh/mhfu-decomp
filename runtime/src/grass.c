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
#define BLADES_PER_CELL 8           /* tried at density 1 */
#define SETTLE 8                    /* frames without anything new before a tile's blades are made */
#define SETTLE_FULL 2               /* (nearer than the full density's reach: the field there made at once - a blade
                                     * made before is not grown again when its tile is made again) */
#define PROBE_FRAMES 15

typedef struct {
    float y;                        /* the top surface's height at the cell's centre */
    uint8_t r, g, b;
    int8_t nx, nz;                  /* its slope: the normal's x, z (times 127) */
    uint8_t state;                  /* 0 unknown, 1 ground, 2 grass */
    float born;                     /* when its blades were first made (the game's seconds modulo 3600; 0 not yet) */
} Cell;

typedef struct {
    int tx, tz, used;
    Cell cells[TILE_CELLS * TILE_CELLS];
    unsigned long changed;          /* the frame something was last laid in it */
    int dirty;                      /* laid since its blades were made */
    int slot;                       /* its blades' slot in the pool (-1: none) */
    int nblades, full;              /* its blades in the slot; made (the order's length) */
    float lo, hi;                   /* the heights of its ground */
} Tile;

static Tile *tiles;
static int ntiles, max_tiles = 4096;  /* (grass_settings: three times those about the camera, at least 4096) */
static int *tile_index;             /* hash: tile key -> index + 1 */
static int index_size;
static uint64_t *seen;              /* the triangles laid: their corners, rounded, hashed (0: empty) */
static size_t seen_size, seen_used;
/* The big triangles (reaching beyond the grass about the camera) laid: the tiles about the camera they were laid over
 * - laid again only over the tiles the camera has brought in since */
typedef struct {
    uint64_t key;
    int tx0, tx1, tz0, tz1;
} Laid;
static Laid *laid;
static size_t laid_size, laid_used;
static unsigned long frame;
static double last_call = -1;       /* the moment (seconds of the game's) scenery was last drawn */
static int probing, probe_known, probe_new;     /* after a gap: frames left to tell by; the ground drawn, known or new */
static int near_slots, far_slots;  /* the pool's (grass_settings: enough for the reach) */
static uint8_t *slot_used;
static float eye[3];
static float radius = 3000.0f, density = 1.0f, full_reach = 700.0f;
static int tries;                   /* blades tried a cell (BLADES_PER_CELL times the density, rounded up) */
static uint32_t slot_blades;        /* a near slot's: TILE_CELLS^2 tries; a far slot's an eighth */
#define FAR_SHARE 8
static GrassBlade *build_buf;

/* The share of a tile's blades drawn at a distance d (its nearest point): those dense enough there, and those coming
 * in (grass.vert) */
static float drawn_share(float d) {
    float k = d < full_reach * GRASS_BAND ? 1.0f : (full_reach * GRASS_BAND / d) * (full_reach * GRASS_BAND / d);
    return fminf(fmaxf(k, 0.02f), 1.0f);
}

/* Tiles this far (their nearest point) draw no more than a far slot holds (drawn_share at most 1 / FAR_SHARE); nearer,
 * a near slot. A tile changes kind a little past it either way (so that one on the line is not made again and again) */
static float far_from(void) {
    return full_reach * GRASS_BAND * sqrtf((float)FAR_SHARE) + 50.0f;
}
#define FAR_MARGIN 300.0f

/* tiles about a point as far as r reaches (and one more each way, for the camera's moving) */
static int tiles_within(float r) {
    float a = r + 2 * TILE;
    return (int)ceilf(3.14159f * a * a / (TILE * TILE)) + 32;
}

void grass_settings(float d, float r, float f) {
    density = d;
    radius = r;
    full_reach = fmaxf(f, 0.0f);
    tries = (int)ceilf(BLADES_PER_CELL * fmaxf(d, 0.0f));
    tries = tries < 1 ? 1 : tries > 4 * BLADES_PER_CELL ? 4 * BLADES_PER_CELL : tries;
    slot_blades = (uint32_t)(TILE_CELLS * TILE_CELLS * tries);
    free(build_buf);
    build_buf = malloc(slot_blades * sizeof *build_buf);
    float near = fminf(far_from() + FAR_MARGIN, r);
    near_slots = tiles_within(near);
    far_slots = r > far_from() ? tiles_within(r) : 0;
    free(slot_used);
    slot_used = calloc((size_t)(near_slots + far_slots), 1);
    if (!tiles && 3 * tiles_within(r) > max_tiles)
        max_tiles = 3 * tiles_within(r);
}

float grass_full(void) {
    return full_reach;
}

uint32_t grass_slot_blades(void) {
    return slot_blades;
}

uint32_t grass_pool_blades(void) {
    return (uint32_t)near_slots * slot_blades + (uint32_t)far_slots * (slot_blades / FAR_SHARE);
}

int grass_slots(void) {
    return near_slots + far_slots;
}

uint32_t grass_slot_base(int slot) {
    return slot < near_slots ? (uint32_t)slot * slot_blades
                             : (uint32_t)near_slots * slot_blades + (uint32_t)(slot - near_slots) * (slot_blades / FAR_SHARE);
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
    if (laid)
        memset(laid, 0, sizeof *laid * laid_size);
    laid_used = 0;
}

static Tile *tile_at(int tx, int tz, int make) {
    if (!tile_index) {
        for (index_size = 1; index_size < 2 * max_tiles; index_size *= 2)   /* (a power of two: masked) */
            ;
        tile_index = calloc((size_t)index_size, sizeof *tile_index);
        tiles = calloc((size_t)max_tiles, sizeof *tiles);
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
    if (!make || ntiles >= max_tiles)
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

/* The ground's triangle being laid (grass_ground) */
typedef struct {
    const float (*p)[3], (*uv)[2], (*col)[3];
    const uint32_t *tex;
    int tw, th, doubled;
    float e1[3], e2[3], d;
    int8_t snx, snz;
} Ground;

/* Its cells from cx0..cx1, cz0..cz1 whose centres it covers (in x, z): each row's span of it only. 0 if the tiles ran
 * out */
static int lay(const Ground *G, int cx0, int cx1, int cz0, int cz1) {
    const float (*p)[3] = G->p, (*uv)[2] = G->uv, (*col)[3] = G->col;
    const float *e1 = G->e1, *e2 = G->e2;
    const uint32_t *tex = G->tex;
    float d = G->d;
    int tw = G->tw, th = G->th, doubled = G->doubled;
    int8_t snx = G->snx, snz = G->snz;
    for (int cz = cz0; cz <= cz1; cz++) {
        /* the row's span: where its centre line crosses the triangle's edges */
        float z = ((float)cz + 0.5f) * CELL, lo = 1e30f, hi = -1e30f;
        for (int k = 0; k < 3; k++) {
            const float *a = p[k], *b = p[(k + 1) % 3];
            if ((a[2] <= z && b[2] >= z) || (b[2] <= z && a[2] >= z)) {
                float x = a[2] == b[2] ? fminf(a[0], b[0]) : a[0] + (b[0] - a[0]) * (z - a[2]) / (b[2] - a[2]);
                float x2 = a[2] == b[2] ? fmaxf(a[0], b[0]) : x;
                lo = fminf(lo, x);
                hi = fmaxf(hi, x2);
            }
        }
        if (lo > hi)
            continue;
        int rx0 = (int)floorf(lo / CELL) - 1, rx1 = (int)floorf(hi / CELL) + 1;
        if (rx0 < cx0)
            rx0 = cx0;
        if (rx1 > cx1)
            rx1 = cx1;
        for (int cx = rx0; cx <= rx1; cx++) {
                float x = ((float)cx + 0.5f) * CELL, z = ((float)cz + 0.5f) * CELL;
                float px = x - p[0][0], pz = z - p[0][2];
                float b1 = (px * e2[2] - e2[0] * pz) / d, b2 = (e1[0] * pz - px * e1[2]) / d, b0 = 1 - b1 - b2;
                if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f)
                    continue;
                float y = b0 * p[0][1] + b1 * p[1][1] + b2 * p[2][1];
                int tx = (int)floorf((float)cx / TILE_CELLS), tz = (int)floorf((float)cz / TILE_CELLS);
                Tile *t = tile_at(tx, tz, 1);
                if (!t)
                    return 0;
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
    return 1;
}


static Laid *laid_at(uint64_t key) {
    if (laid_used * 2 >= laid_size) {
        size_t old = laid_size;
        Laid *was = laid;
        laid_size = old ? old * 2 : 1024;
        laid = calloc(laid_size, sizeof *laid);
        laid_used = 0;
        for (size_t i = 0; i < old; i++)
            if (was[i].key) {
                size_t j = was[i].key & (laid_size - 1);
                while (laid[j].key)
                    j = (j + 1) & (laid_size - 1);
                laid[j] = was[i];
                laid_used++;
            }
        free(was);
    }
    size_t j = key & (laid_size - 1);
    while (laid[j].key && laid[j].key != key)
        j = (j + 1) & (laid_size - 1);
    if (!laid[j].key) {
        laid[j].key = key;
        laid[j].tx0 = laid[j].tz0 = 1;      /* (an empty box) */
        laid[j].tx1 = laid[j].tz1 = 0;
        laid_used++;
    }
    return &laid[j];
}

void grass_ground(const float p[3][3], const float uv[3][2], const float col[3][3], const uint32_t *tex, int tw,
                  int th, int doubled) {
    if (density <= 0 || !build_buf)
        return;
    /* facing up, near */
    Ground G = {p, uv, col, tex, tw, th, doubled, {0}, {0}, 0, 0, 0};
    float *e1 = G.e1, *e2 = G.e2, n[3];
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
    G.d = e1[0] * e2[2] - e2[0] * e1[2];
    if (fabsf(G.d) < 1e-6f)
        return;
    G.snx = (int8_t)lroundf(n[0] / nl * 127);
    G.snz = (int8_t)lroundf(n[2] / nl * 127);
    int whole = x0 >= eye[0] - reach && x1 <= eye[0] + reach && z0 >= eye[2] - reach && z1 <= eye[2] + reach;
    if (whole) {
        int known = seen_before(p);
        if (probing)
            known ? probe_known++ : probe_new++;
        if (known)
            return;
        lay(&G, (int)floorf(x0 / CELL), (int)floorf(x1 / CELL), (int)floorf(z0 / CELL), (int)floorf(z1 / CELL));
        return;
    }
    /* one reaching beyond: laid over the tiles about the camera (as far as it reaches), but those it was laid over
     * already */
    uint64_t k = 0x6789;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            k = mix64(k ^ (uint64_t)(int64_t)lroundf(p[i][j]));
    Laid *L = laid_at(k | 1);
    int r = (int)ceilf(reach / TILE), etx = (int)floorf(eye[0] / TILE), etz = (int)floorf(eye[2] / TILE);
    int tx0 = etx - r, tx1 = etx + r, tz0 = etz - r, tz1 = etz + r;
    int bx0 = (int)floorf(x0 / TILE), bx1 = (int)floorf(x1 / TILE), bz0 = (int)floorf(z0 / TILE), bz1 = (int)floorf(z1 / TILE);
    tx0 = tx0 > bx0 ? tx0 : bx0;
    tx1 = tx1 < bx1 ? tx1 : bx1;
    tz0 = tz0 > bz0 ? tz0 : bz0;
    tz1 = tz1 < bz1 ? tz1 : bz1;
    if (probing)
        L->tx0 <= tx0 && L->tx1 >= tx1 && L->tz0 <= tz0 && L->tz1 >= tz1 ? probe_known++ : probe_new++;
    /* the new box less the old: the rows of tiles above and below it, and the columns either side between */
    int ox0 = L->tx0, ox1 = L->tx1, oz0 = L->tz0, oz1 = L->tz1;
    int ok = 1;
    for (int tz = tz0; tz <= tz1 && ok; tz++) {
        int in_rows = tz >= oz0 && tz <= oz1;
        int a0 = tx0, a1 = tx1;
        if (in_rows) {                      /* (the columns left of the old box, then right of it) */
            if (a0 < ox0)
                ok = lay(&G, a0 * TILE_CELLS, (ox0 < a1 + 1 ? ox0 : a1 + 1) * TILE_CELLS - 1, tz * TILE_CELLS,
                         tz * TILE_CELLS + TILE_CELLS - 1);
            if (ok && a1 > ox1)
                ok = lay(&G, (ox1 + 1 > a0 ? ox1 + 1 : a0) * TILE_CELLS, a1 * TILE_CELLS + TILE_CELLS - 1,
                         tz * TILE_CELLS, tz * TILE_CELLS + TILE_CELLS - 1);
        } else {
            ok = lay(&G, a0 * TILE_CELLS, a1 * TILE_CELLS + TILE_CELLS - 1, tz * TILE_CELLS,
                     tz * TILE_CELLS + TILE_CELLS - 1);
        }
    }
    if (ok) {
        L->tx0 = tx0;
        L->tx1 = tx1;
        L->tz0 = tz0;
        L->tz1 = tz1;
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

/* A tile's blades: scattered over its grass cells in tufts, shorter and fewer towards the grass's edges; those of a
 * cell made for the first time born now */
static int build(Tile *t) {
    int n = 0;
    float now = (float)fmod(last_call > 0 ? last_call : 0.0, 3600.0), share = BLADES_PER_CELL * density / (float)tries;
    if (now == 0.0f)
        now = 1e-3f;                    /* (0: not born yet) */
    for (int cz = 0; cz < TILE_CELLS; cz++)
        for (int cx = 0; cx < TILE_CELLS; cx++) {
            Cell *c = &t->cells[cz * TILE_CELLS + cx];
            if (c->state != 2)
                continue;
            if (c->born == 0.0f)
                c->born = now;
            int around = grass_cell(t, cx - 1, cz) + grass_cell(t, cx + 1, cz) + grass_cell(t, cx, cz - 1) +
                         grass_cell(t, cx, cz + 1);
            int wx = t->tx * TILE_CELLS + cx, wz = t->tz * TILE_CELLS + cz;
            float ny = sqrtf(fmaxf(1.0f - ((float)c->nx * c->nx + (float)c->nz * c->nz) / (127.0f * 127.0f), 0.1f));
            float nx = (float)c->nx / 127.0f, nz = (float)c->nz / 127.0f;
            for (int k = 0; k < tries && n < (int)slot_blades; k++) {
                uint32_t h = hash2(wx, wz, (uint32_t)k * 0x51ED27u);
                float ox = unit(h) - 0.5f, oz = unit(h >> 7 ^ h << 11) - 0.5f;
                float x = ((float)wx + 0.5f + ox) * CELL, z = ((float)wz + 0.5f + oz) * CELL;
                /* tufts: the clump noise thins the field in patches; the edges of the grass thin it more */
                float clump = 0.7f * noise(x * 0.012f, z * 0.012f, 3) + 0.3f * noise(x * 0.04f, z * 0.04f, 5);
                float keep = (0.25f + 1.1f * clump - 0.15f * (float)(4 - around)) * share;
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
                memcpy(&bl->extra, &c->born, sizeof bl->extra);
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

/* the kind of slot a tile wants: near (1) or far (0) - kept a little past the line */
static int wants_near(const Tile *t, float d) {
    float line = far_from();
    if (t->slot >= 0)
        return t->slot < near_slots ? d < line + FAR_MARGIN : d < line;
    return d < line || !far_slots;
}

int grass_build(int *slot, const GrassBlade **blades, int *count) {
    /* a tile near, settled, whose blades are not made (or are older than its ground, or in the other kind of slot):
     * the nearest. Until it is made again its old blades are drawn */
    Tile *best = NULL;
    float best_d = 1e30f;
    for (int i = 0; i < ntiles; i++) {
        Tile *t = &tiles[i];
        float d = tile_distance(t);
        if (d >= radius || d >= best_d)
            continue;
        int moved = t->slot >= 0 && (t->slot < near_slots) != wants_near(t, d);
        if ((t->dirty && frame - t->changed >= (d < full_reach ? SETTLE_FULL : SETTLE)) || moved)
            best = t, best_d = d;
    }
    if (!best)
        return 0;
    int near = wants_near(best, best_d), lo = near ? 0 : near_slots, hi = near ? near_slots : near_slots + far_slots;
    int s = best->slot >= lo && best->slot < hi ? best->slot : -1;
    for (int i = lo; i < hi && s < 0; i++)
        if (!slot_used[i])
            s = i;
    if (s < 0) {                        /* (full: the farthest tile's of this kind given up, if it is farther) */
        Tile *far = NULL;
        float far_d = -1;
        for (int i = 0; i < ntiles; i++)
            if (tiles[i].slot >= lo && tiles[i].slot < hi && tile_distance(&tiles[i]) > far_d)
                far = &tiles[i], far_d = tile_distance(&tiles[i]);
        if (!far || far_d <= best_d)
            return 0;
        s = far->slot;
        far->slot = -1;
        far->nblades = 0;
        far->dirty = 1;
    }
    if (best->slot >= 0 && best->slot != s)
        slot_used[best->slot] = 0;
    slot_used[s] = 1;
    best->slot = s;
    best->dirty = 0;
    best->full = build(best);
    int far_blades = (int)(slot_blades / FAR_SHARE);
    best->nblades = near ? best->full : best->full < far_blades ? best->full : far_blades;
    *slot = s;
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
        int count = (int)ceilf((float)t->full * drawn_share(d));
        if (count > t->nblades)
            count = t->nblades;
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
