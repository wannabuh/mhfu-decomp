/* Blood off the animal (the wounds: gpu.c): drops in the air, and the ground's blood - a map of it, painted.
 *
 * gpu.c says where a drop leaves a wound and how fast (blood_spawn, the game's thread); the rest is the renderer's
 * thread's, each frame (blood_step). A drop falls, slowed by the air, to the ground: as high as the grass's ground
 * is there (grass.c lays it from the scenery drawn - where it knows it), else the height gpu.c guessed. Where it
 * lands it is painted into the map: a blot, ragged, with specks thrown about it; one that came in across the ground
 * a head and a tail drawn out the way it went, over a fifth of a second, and specks past its end.
 *
 * The map: BLOOD_N x BLOOD_N cells of BLOOD_UNIT units of the world, wrapped (a cell is its place's, modulo), in
 * tiles of 32 x 32 - a tile is one place's at a time, emptied as another's is first touched, and when it is far
 * from the eye. A cell: how much blood (BLOOD_AMOUNT: 255 covers the ground, more is deeper), how wet, how high its
 * ground is (in eights of a unit... 8 units a step, modulo; BLOOD_ANY: not known - any ground under it), and its
 * colour (one of 16). The pixel stage stains the scenery's upward faces by it (ge_frag.glsl), from a copy on the GPU
 * (the tiles changed, each frame).
 *
 * Wet blood moves: more than a cell holds spreads to its neighbours with less (a pool, wider as it is fed, its edge
 * as uneven as what the cells hold), and on a slope (the grass's ground's) it creeps downhill, leaving a little
 * behind. It dries in most of a minute and stays, till the area is left. */
#include "blood.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

int grass_ground_at(float x, float z, float *y, float *nx, float *nz);     /* grass.c */

#define TILE 32
#define TILES (BLOOD_N / TILE)
#define REACH_TILES 56                  /* a tile further than this from the eye's is emptied */
#define HOLD 210                        /* what a cell holds before it spreads (by the cell: 3/4 to 5/4 of it) */
#define AMOUNT_MAX 1023
#define GRAVITY 980.0f
#define DRAG 1.2f
#define DROPS (4 * BLOOD_DROP_SETS)

typedef struct { float at[3], speed[3], floor_y, size; uint8_t rgb[3]; int clear; } Event;
typedef struct { float at[3], speed[3], from_y, floor_y, size; double born; uint8_t rgb[3]; int used; } Drop;
typedef struct { float x, z, r, thick; int ycode; uint8_t color; double when; int used; float x1, z1, r1; int line; } Paint;
typedef struct { int16_t tx, tz; uint8_t used, wet, dirty; } TileState;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Event events[64];
static int nevents;
static float eye_now[3];

static uint32_t *map, *gpu;
static TileState *tile;
static Drop drops[DROPS];
static Paint paints[512];
static float palette[16][3];
static int ncolors, any_blood;
static double then = -1, flow_due, dry_due;
static uint32_t chance_state = 0x9E3779B9u;

static float chance(void) {
    chance_state = chance_state * 1664525u + 1013904223u;
    return (float)(chance_state >> 8 & 0xFFFF) / 65535.0f;
}

static uint32_t hash2(int x, int z) {
    uint32_t h = (uint32_t)x * 0x85EBCA6Bu ^ (uint32_t)z * 0xC2B2AE35u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    return h ^ h >> 13;
}

/* -- the game's thread -------------------------------------------------------------------------------------------- */

void blood_spawn(const float *at, const float *speed, float floor_y, float size, const uint8_t *rgb) {
    pthread_mutex_lock(&lock);
    if (nevents < (int)(sizeof events / sizeof *events)) {
        Event *e = &events[nevents++];
        memcpy(e->at, at, sizeof e->at);
        memcpy(e->speed, speed, sizeof e->speed);
        e->floor_y = floor_y;
        e->size = size;
        memcpy(e->rgb, rgb, 3);
        e->clear = 0;
    }
    pthread_mutex_unlock(&lock);
}

void blood_clear(void) {
    pthread_mutex_lock(&lock);
    nevents = 1;
    events[0].clear = 1;
    pthread_mutex_unlock(&lock);
}

void blood_eye(const float *eye) {
    pthread_mutex_lock(&lock);
    memcpy(eye_now, eye, sizeof eye_now);
    pthread_mutex_unlock(&lock);
}

/* -- the map ------------------------------------------------------------------------------------------------------ */

void blood_init(uint32_t *gpu_map) {
    gpu = gpu_map;
    map = calloc((size_t)BLOOD_N * BLOOD_N, sizeof *map);
    tile = calloc((size_t)TILES * TILES, sizeof *tile);
}

static int cell_of(float w) {
    return (int)floorf(w / BLOOD_UNIT);
}

/* The cell (cx, cz) of the world: its tile made this place's (emptied of another's) */
static uint32_t *cell(int cx, int cz) {
    int tx = cx >> 5, tz = cz >> 5;
    TileState *t = &tile[(tz & (TILES - 1)) * TILES + (tx & (TILES - 1))];
    if (!t->used || t->tx != (int16_t)tx || t->tz != (int16_t)tz) {
        if (t->used)
            for (int r = 0; r < TILE; r++)
                memset(&map[(size_t)(((tz * TILE + r) & (BLOOD_N - 1))) * BLOOD_N + ((tx * TILE) & (BLOOD_N - 1))], 0, TILE * sizeof *map);
        t->used = 1;
        t->tx = (int16_t)tx;
        t->tz = (int16_t)tz;
    }
    t->dirty = t->wet = 1;
    return &map[(size_t)(cz & (BLOOD_N - 1)) * BLOOD_N + (cx & (BLOOD_N - 1))];
}

static int ycode_of(float y) {
    int c = (int)floorf(y / 8.0f) & 1023;
    return c == BLOOD_ANY ? BLOOD_ANY - 1 : c;
}

/* Blood into a cell: more of it, wet again; an empty one takes its ground's height (the grass's ground, where known) */
static void add(int cx, int cz, int amount, int ycode, int color) {
    if (amount <= 0)
        return;
    uint32_t *c = cell(cx, cz), v = *c;
    int a = (int)(v & 1023u);
    if (!a) {
        float gy, nx, nz;
        if (grass_ground_at(((float)cx + 0.5f) * BLOOD_UNIT, ((float)cz + 0.5f) * BLOOD_UNIT, &gy, &nx, &nz) &&
            (ycode == BLOOD_ANY || abs(((ycode_of(gy) - ycode + 512) & 1023) - 512) < 12))
            ycode = ycode_of(gy);
        v = (uint32_t)ycode << 18 | (uint32_t)color << 28;
    }
    a += amount;
    if (a > AMOUNT_MAX)
        a = AMOUNT_MAX;
    *c = (v & 0xFFFC0000u) | 255u << 10 | (uint32_t)a;
    any_blood = 1;
}

/* A round blot of it, r units wide (its edge ragged by `ragged`, a share of r), `thick` at its deepest */
static void blot(float x, float z, float r, float thick, float ragged, int ycode, int color) {
    float reach = r * (1.0f + ragged) + BLOOD_UNIT;
    int x0 = cell_of(x - reach), x1 = cell_of(x + reach), z0 = cell_of(z - reach), z1 = cell_of(z + reach);
    float p1 = chance() * 6.28f, p2 = chance() * 6.28f;
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++) {
            float dx = ((float)cx + 0.5f) * BLOOD_UNIT - x, dz = ((float)cz + 0.5f) * BLOOD_UNIT - z;
            float d = sqrtf(dx * dx + dz * dz), turn = atan2f(dz, dx);
            float edge = r * (1.0f + ragged * (0.6f * sinf(turn * 3.0f + p1) + 0.4f * sinf(turn * 7.0f + p2)));
            float cover = (edge - d) / BLOOD_UNIT + 0.5f;
            if (cover <= 0)
                continue;
            float deep = 0.45f + 0.55f * (1.0f - fminf(d / fmaxf(edge, 0.01f), 1.0f));
            add(cx, cz, (int)(thick * fminf(cover, 1.0f) * deep), ycode, color);
        }
}

static void paint_later(float x, float z, float r, float thick, int ycode, int color, double when) {
    for (int i = 0; i < (int)(sizeof paints / sizeof *paints); i++)
        if (!paints[i].used) {
            paints[i] = (Paint){x, z, r, thick, ycode, (uint8_t)color, when, 1, 0, 0, 0, 0};
            return;
        }
}

/* A line of it, from one place to another: half as wide as w0 at its start, w1 at its end, thinner as it goes */
static void streak(float x0, float z0, float x1, float z1, float w0, float w1, float thick, int ycode, int color) {
    float reach = fmaxf(w0, w1) + BLOOD_UNIT, lx = x1 - x0, lz = z1 - z0, ll = lx * lx + lz * lz;
    int cx0 = cell_of(fminf(x0, x1) - reach), cx1 = cell_of(fmaxf(x0, x1) + reach);
    int cz0 = cell_of(fminf(z0, z1) - reach), cz1 = cell_of(fmaxf(z0, z1) + reach);
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float px = ((float)cx + 0.5f) * BLOOD_UNIT - x0, pz = ((float)cz + 0.5f) * BLOOD_UNIT - z0;
            float t = ll > 0 ? fminf(fmaxf((px * lx + pz * lz) / ll, 0.0f), 1.0f) : 0;
            float dx = px - lx * t, dz = pz - lz * t;
            float cover = ((w0 + (w1 - w0) * t) - sqrtf(dx * dx + dz * dz)) / BLOOD_UNIT + 0.5f;
            if (cover > 0)
                add(cx, cz, (int)(thick * fminf(cover, 1.0f) * (1.0f - 0.35f * t)), ycode, color);
        }
}

static void streak_later(float x0, float z0, float x1, float z1, float w0, float w1, float thick, int ycode, int color, double when) {
    for (int i = 0; i < (int)(sizeof paints / sizeof *paints); i++)
        if (!paints[i].used) {
            paints[i] = (Paint){x0, z0, w0, thick, ycode, (uint8_t)color, when, 1, x1, z1, w1, 1};
            return;
        }
}

/* What a splash throws out: thin rays from about its middle - all round, or in a fan the way it was going - each
 * drawn out in two goes, some with a bead at the end; and fine specks beyond */
static void rays(float x, float z, float r, float thick, float dx, float dz, float e, int ycode, int color, double now) {
    int fan = e > 0, n = fan ? 3 + (int)(e * 2.0f) + (int)(chance() * 3.0f) : 2 + (int)(chance() * 5.0f);
    float mid = fan ? atan2f(dz, dx) : 0;
    for (int i = 0; i < n; i++) {
        float turn = fan ? mid + (chance() - 0.5f) * (1.3f - 0.2f * e) : chance() * 6.2832f;
        float far = r * (fan ? 2.2f + (1.8f + 3.0f * e) * chance() : 1.6f + 3.2f * chance() * chance());
        float cx = cosf(turn), cz = sinf(turn), bend = (chance() - 0.5f) * 0.25f;
        float w0 = fmaxf(r * (0.16f + 0.16f * chance()), 0.9f), from = r * 0.45f, half = from + (far - from) * 0.5f;
        float mx = x + cx * half - cz * bend * far * 0.3f, mz = z + cz * half + cx * bend * far * 0.3f;
        float ex = x + cx * far, ez = z + cz * far;
        streak(x + cx * from, z + cz * from, mx, mz, w0, w0 * 0.7f, thick, ycode, color);
        streak_later(mx, mz, ex, ez, w0 * 0.7f, 0.35f, thick * 0.9f, ycode, color, now + 0.03 + 0.06 * chance());
        if (chance() < 0.45f)
            paint_later(ex, ez, fmaxf(r * (0.14f + 0.16f * chance()), 0.9f), thick * 0.9f, ycode, color, now + 0.1 + 0.05 * chance());
    }
    int specks = 5 + (int)(chance() * 9.0f);
    for (int i = 0; i < specks; i++) {
        float turn = fan ? mid + (chance() - 0.5f) * 1.7f : chance() * 6.2832f, far = r * (1.6f + (3.4f + 2.5f * e) * chance());
        paint_later(x + cosf(turn) * far, z + sinf(turn) * far, 0.6f + 0.9f * chance() * chance() * fminf(r * 0.3f, 2.0f), thick * 0.9f,
                    ycode, color, now + 0.03 + 0.12 * chance());
    }
}

/* A drop lands: the splash it makes, by how fast it came in across the ground */
static void land(const float *at, const float *speed, float size, int ycode, int color, double now) {
    float across = sqrtf(speed[0] * speed[0] + speed[2] * speed[2]);
    float thick = 150.0f + 22.0f * size;
    float r = size * 1.25f;
    /* its middle: a blot and a lobe or two of it, no two alike */
    int lobes = 1 + (int)(chance() * 2.5f);
    for (int i = 0; i < lobes; i++) {
        float turn = chance() * 6.2832f, off = r * (0.3f + 0.4f * chance());
        blot(at[0] + cosf(turn) * off, at[2] + sinf(turn) * off, r * (0.35f + 0.35f * chance()), thick * 0.8f, 0.3f, ycode, color);
    }
    if (across < 70.0f) {               /* straight down: a blot, rays and specks thrown out all round it */
        blot(at[0], at[2], r, thick, 0.36f, ycode, color);
        rays(at[0], at[2], r, thick, 0, 0, 0, ycode, color, now);
        return;
    }
    /* across the ground: its head where it hit, a tail drawn out the way it went - narrower as it goes, a little
     * aside here and there - and rays and specks thrown on ahead, in a fan */
    float dx = speed[0] / across, dz = speed[2] / across, e = fminf(across / 200.0f, 3.0f);
    float length = r * (1.2f + 1.8f * e);
    int steps = 3 + (int)(length / (BLOOD_UNIT * 0.8f));
    blot(at[0], at[2], r * 0.9f, thick, 0.3f, ycode, color);
    for (int i = 1; i <= steps; i++) {
        float s = (float)i / (float)steps, far = r * 0.5f + length * s, aside = (chance() - 0.5f) * r * 0.4f * s;
        paint_later(at[0] + dx * far - dz * aside, at[2] + dz * far + dx * aside, r * (0.75f - 0.6f * s), thick * (1.0f - 0.45f * s),
                    ycode, color, now + 0.15 * s);
    }
    rays(at[0] + dx * r * 0.4f, at[2] + dz * r * 0.4f, r, thick, dx, dz, e, ycode, color, now);
}

/* Wet blood moves (a tile's cells, one step): what a cell cannot hold goes to its neighbours with less; on a slope
 * some of it goes downhill. (In place, the way gone over turned about each time: no side is favoured for long) */
static void flow_tile(int slot, unsigned turn) {
    TileState *t = &tile[slot];
    float gy, nx = 0, nz = 0;
    int sloped = grass_ground_at(((float)t->tx + 0.5f) * TILE * BLOOD_UNIT, ((float)t->tz + 0.5f) * TILE * BLOOD_UNIT, &gy, &nx, &nz);
    float steep = sloped ? sqrtf(nx * nx + nz * nz) : 0;
    int wet = 0;
    for (int i = 0; i < TILE * TILE; i++) {
        int k = turn & 1 ? TILE * TILE - 1 - i : i;
        int cx = t->tx * TILE + (turn & 2 ? TILE - 1 - k % TILE : k % TILE), cz = t->tz * TILE + k / TILE;
        uint32_t *c = &map[(size_t)(cz & (BLOOD_N - 1)) * BLOOD_N + (cx & (BLOOD_N - 1))], v = *c;
        int a = (int)(v & 1023u), w = (int)(v >> 10 & 255u);
        if (!a || !w)
            continue;
        wet = 1;
        if (w < 70)
            continue;                   /* (drying: it has stopped) */
        int ycode = (int)(v >> 18 & 1023u), color = (int)(v >> 28);
        int holds = HOLD * (48 + (int)(hash2(cx, cz) & 31u)) / 64, moved = 0;
        static const int side[8][2] = {{1, 0}, {-1, 1}, {0, 1}, {-1, -1}, {-1, 0}, {1, -1}, {0, -1}, {1, 1}};
        if (a > holds) {
            int first = (int)(hash2(cz, cx + (int)turn) & 7u);
            for (int s = 0; s < 8 && a - moved > holds; s++) {
                const int *d = side[(first + s) & 7];
                int other = (int)(map[(size_t)((cz + d[1]) & (BLOOD_N - 1)) * BLOOD_N + ((cx + d[0]) & (BLOOD_N - 1))] & 1023u);
                int give = (a - moved - other) / ((d[0] && d[1]) ? 9 : 6);
                if (give > a - moved - holds)
                    give = a - moved - holds;
                if (give > 0) {
                    add(cx + d[0], cz + d[1], give, ycode, color);
                    moved += give;
                }
            }
        }
        if (steep > 0.1f && a - moved > 70) {     /* downhill: by the slope, to the neighbour that way */
            int give = (int)((float)(a - moved - 45) * fminf(steep * 0.9f, 0.45f));
            int along_x = chance() * (fabsf(nx) + fabsf(nz)) < fabsf(nx);
            if (give > 0) {
                add(cx + (along_x ? (nx > 0 ? 1 : -1) : 0), cz + (along_x ? 0 : (nz > 0 ? 1 : -1)), give, ycode, color);
                moved += give;
            }
        }
        if (moved) {
            c = &map[(size_t)(cz & (BLOOD_N - 1)) * BLOOD_N + (cx & (BLOOD_N - 1))];
            *c = (*c & ~1023u) | (uint32_t)(a - moved);
            t->dirty = 1;
        }
    }
    t->wet = (uint8_t)wet;
}

/* It dries (a tile's cells, a step a second): what lies deep more slowly */
static void dry_tile(int slot) {
    TileState *t = &tile[slot];
    int wet = 0;
    for (int r = 0; r < TILE; r++) {
        uint32_t *row = &map[(size_t)((t->tz * TILE + r) & (BLOOD_N - 1)) * BLOOD_N + ((t->tx * TILE) & (BLOOD_N - 1))];
        for (int i = 0; i < TILE; i++) {
            int w = (int)(row[i] >> 10 & 255u);
            if (!w)
                continue;
            w -= (row[i] & 1023u) > 300u ? 3 : 6;
            if (w < 0)
                w = 0;
            wet |= w;
            row[i] = (row[i] & ~(255u << 10)) | (uint32_t)w << 10;
        }
    }
    t->wet = wet != 0;
    t->dirty = 1;
}

static int color_of(const uint8_t *rgb) {
    float c[3] = {(float)rgb[0] / 255.0f, (float)rgb[1] / 255.0f, (float)rgb[2] / 255.0f};
    for (int i = 0; i < ncolors; i++)
        if (fabsf(palette[i][0] - c[0]) + fabsf(palette[i][1] - c[1]) + fabsf(palette[i][2] - c[2]) < 0.02f)
            return i;
    if (ncolors == 16)
        return 0;
    memcpy(palette[ncolors], c, sizeof c);
    return ncolors++;
}

/* -- the renderer's thread, each frame ---------------------------------------------------------------------------- */

void blood_step(double now, BloodFrame *out) {
    static Event got[64];
    static int eye_tile[2] = {1 << 20, 1 << 20};
    float eye[3];
    pthread_mutex_lock(&lock);
    int n = nevents;
    memcpy(got, events, sizeof *got * (size_t)n);
    nevents = 0;
    memcpy(eye, eye_now, sizeof eye);
    pthread_mutex_unlock(&lock);
    memset(out, 0, sizeof *out);
    if (!map)
        return;
    float dt = then < 0 ? 0 : (float)(now - then);
    if (dt < 0 || dt > 0.1f)
        dt = 0;
    then = now;
    for (int i = 0; i < n; i++) {
        if (got[i].clear) {             /* the area left: all of it gone */
            for (int s = 0; s < TILES * TILES; s++)
                if (tile[s].used) {
                    for (int r = 0; r < TILE; r++) {
                        size_t at = (size_t)((tile[s].tz * TILE + r) & (BLOOD_N - 1)) * BLOOD_N + ((tile[s].tx * TILE) & (BLOOD_N - 1));
                        memset(&map[at], 0, TILE * sizeof *map);
                        memset(&gpu[at], 0, TILE * sizeof *gpu);
                    }
                    memset(&tile[s], 0, sizeof tile[s]);
                }
            memset(drops, 0, sizeof drops);
            memset(paints, 0, sizeof paints);
            any_blood = 0;
            continue;
        }
        for (int d = 0; d < DROPS; d++)
            if (!drops[d].used) {
                drops[d] = (Drop){{got[i].at[0], got[i].at[1], got[i].at[2]}, {got[i].speed[0], got[i].speed[1], got[i].speed[2]},
                                  got[i].at[1], got[i].floor_y, got[i].size, now, {got[i].rgb[0], got[i].rgb[1], got[i].rgb[2]}, 1};
                break;
            }
    }
    /* the drops in the air */
    for (int i = 0; i < DROPS; i++) {
        Drop *d = &drops[i];
        if (!d->used)
            continue;
        float slow = expf(-DRAG * dt), was = d->at[1], gy, nx, nz;
        d->speed[0] *= slow;
        d->speed[2] *= slow;
        d->speed[1] -= GRAVITY * dt;
        for (int k = 0; k < 3; k++)
            d->at[k] += d->speed[k] * dt;
        /* the ground: the grass's, where it is known and the drop came from above it; else as high as was guessed */
        int known = grass_ground_at(d->at[0], d->at[2], &gy, &nx, &nz) && d->from_y > gy - 20.0f && gy > d->floor_y - 400.0f;
        float ground = known ? gy : d->floor_y;
        if (now - d->born > 3.0 || now < d->born || (d->at[1] <= ground && d->speed[1] < 0 && (was > ground - 60.0f || !known))) {
            d->used = 0;
            land(d->at, d->speed, d->size, known ? ycode_of(gy) : BLOOD_ANY, color_of(d->rgb), now);
            continue;
        }
        int o = out->ndrops++;
        memcpy(out->drop_at[o], d->at, sizeof d->at);
        out->drop_at[o][3] = fminf(2.2f, fmaxf(1.0f, d->size * 0.22f));
        for (int k = 0; k < 3; k++) {
            out->drop_color[o][k] = (float)d->rgb[k] / 255.0f;
            out->drop_streak[o][k] = d->speed[k] / 60.0f;       /* (a streak: back to where it was a 60th of a second ago) */
        }
    }
    /* what is still being painted */
    for (int i = 0; i < (int)(sizeof paints / sizeof *paints); i++)
        if (paints[i].used && (paints[i].when <= now || paints[i].when > now + 2.0)) {
            paints[i].used = 0;
            if (paints[i].line)
                streak(paints[i].x, paints[i].z, paints[i].x1, paints[i].z1, paints[i].r, paints[i].r1, paints[i].thick, paints[i].ycode, paints[i].color);
            else
                blot(paints[i].x, paints[i].z, paints[i].r, paints[i].thick, 0.2f, paints[i].ycode, paints[i].color);
        }
    /* the map: far tiles emptied as the eye goes to another; the wet ones moved on and dried; those changed to the GPU */
    int etx = cell_of(eye[0]) >> 5, etz = cell_of(eye[2]) >> 5;
    int moved = etx != eye_tile[0] || etz != eye_tile[1];
    int flow = 0, dry = 0;
    static unsigned turn;
    eye_tile[0] = etx;
    eye_tile[1] = etz;
    if (now >= flow_due || now < flow_due - 1.0) {
        flow = 1;
        flow_due = now + 1.0 / 20.0;
        turn++;
    }
    if (now >= dry_due || now < dry_due - 2.0) {
        dry = 1;
        dry_due = now + 1.0;
    }
    for (int s = 0; any_blood && s < TILES * TILES; s++) {
        TileState *t = &tile[s];
        if (!t->used)
            continue;
        if (moved && (abs(t->tx - etx) > REACH_TILES || abs(t->tz - etz) > REACH_TILES)) {
            for (int r = 0; r < TILE; r++) {
                size_t at = (size_t)((t->tz * TILE + r) & (BLOOD_N - 1)) * BLOOD_N + ((t->tx * TILE) & (BLOOD_N - 1));
                memset(&map[at], 0, TILE * sizeof *map);
                memset(&gpu[at], 0, TILE * sizeof *gpu);
            }
            memset(t, 0, sizeof *t);
            continue;
        }
        if (t->wet && flow && dt > 0)
            flow_tile(s, turn);
        if (t->wet && dry && dt > 0)
            dry_tile(s);
    }
    for (int s = 0; any_blood && s < TILES * TILES; s++) {      /* (after all: one tile's flow changes its neighbours) */
        TileState *t = &tile[s];
        if (!t->used || !t->dirty)
            continue;
        t->dirty = 0;
        for (int r = 0; r < TILE; r++) {
            size_t at = (size_t)((t->tz * TILE + r) & (BLOOD_N - 1)) * BLOOD_N + ((t->tx * TILE) & (BLOOD_N - 1));
            memcpy(&gpu[at], &map[at], TILE * sizeof *gpu);
        }
    }
    memcpy(out->palette, palette, sizeof palette);
    memcpy(out->eye, eye, sizeof eye);
    out->any = any_blood;
}
