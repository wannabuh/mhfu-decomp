/* Blood kept on an animal's skin texel by texel: see bloodmap.h. */
#include "bloodmap.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD 1.5f                    /* texels about a triangle's place that are painted as it is (the pixel stage reads between texels) */
#define MAPS (RUN_MESHES + RUN_MESHES * RUN_TRIS * RUN_MAP_WORDS)
#define TEXELS (BLOODMAP_W * BLOODMAP_H)

/* A triangle's place in its skin's map: the texels it has (x, y, w, h), and its corners there */
typedef struct { uint16_t x, y, w, h; float c[3][2]; } Place;

static uint32_t store[MAPS + RUN_MESHES * TEXELS];
static Place *places[RUN_MESHES];
static float (*rests[RUN_MESHES])[3][3];    /* each triangle's corners as the game gave them */
static uint32_t nplaces[RUN_MESHES];
static float units[RUN_MESHES];     /* units of the world a texel is, in each */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t dirty_from, dirty_to;
static double now_code = 1;

static void touch(uint32_t from, uint32_t to) {
    pthread_mutex_lock(&lock);
    if (dirty_from == dirty_to)
        dirty_from = from, dirty_to = to;
    else
        dirty_from = from < dirty_from ? from : dirty_from, dirty_to = to > dirty_to ? to : dirty_to;
    pthread_mutex_unlock(&lock);
}

const uint32_t *bloodmap_words(uint32_t *from, uint32_t *to) {
    pthread_mutex_lock(&lock);
    *from = dirty_from, *to = dirty_to;
    dirty_from = dirty_to = 0;
    pthread_mutex_unlock(&lock);
    return store;
}

int bloodmap_changed(void) { return dirty_to != dirty_from; }

float bloodmap_now(void) { return (float)now_code; }

static uint32_t *texels(int mesh) { return store + MAPS + (size_t)mesh * TEXELS; }

void bloodmap_drop(int mesh) {
    if (mesh < 0 || mesh >= RUN_MESHES || !places[mesh])
        return;
    free(places[mesh]);
    free(rests[mesh]);
    rests[mesh] = NULL;
    places[mesh] = NULL;
    nplaces[mesh] = 0;
    memset(store + RUN_MESHES + (size_t)mesh * RUN_TRIS * RUN_MAP_WORDS, 0, sizeof(uint32_t) * RUN_TRIS * RUN_MAP_WORDS);
    memset(texels(mesh), 0, sizeof(uint32_t) * TEXELS);
    touch(RUN_MESHES + (uint32_t)mesh * RUN_TRIS * RUN_MAP_WORDS, RUN_MESHES + (uint32_t)(mesh + 1) * RUN_TRIS * RUN_MAP_WORDS);
    touch(MAPS + (uint32_t)mesh * TEXELS, MAPS + (uint32_t)(mesh + 1) * TEXELS);
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }

typedef struct { uint32_t t; float base, high, foot; int first; } Size;   /* (a triangle in the world: its longest edge, its height on it, where the third corner's foot is along it, the corner the edge starts at) */

static int taller(const void *a, const void *b) {
    const Size *x = a, *y = b;
    return x->high > y->high ? -1 : x->high < y->high ? 1 : x->t < y->t ? -1 : x->t > y->t;
}

float bloodmap_layout(int mesh, uint32_t ntri, const TrailSkin *skin, const uint8_t *rgb) {
    if (mesh < 0 || mesh >= RUN_MESHES)
        return 0;
    /* What blood the skin has is kept through a new layout - the game draws an animal from another model as it dies,
     * or far off, and its blood went with the old one (#90): the old triangles that have any, each with its corners
     * as given and its texels, and after the layout every new texel takes what was at its place among those */
    typedef struct { float R[3][3], lo[3], hi[3], n[3], size; Place p; uint32_t *tex; } Kept;
    Kept *kept = NULL;
    uint32_t nkept = 0;
    if (places[mesh] && rests[mesh]) {
        kept = malloc(sizeof *kept * (nplaces[mesh] + 1));
        const uint32_t *tex = texels(mesh);
        for (uint32_t t = 0; t < nplaces[mesh]; t++) {
            const Place *p = &places[mesh][t];
            int any = 0;
            for (uint32_t y = p->y; y < (uint32_t)p->y + p->h && !any; y++)
                for (uint32_t x = p->x; x < (uint32_t)p->x + p->w && !any; x++)
                    any = (tex[y * BLOODMAP_W + x] & 0xFF00FFFFu) != 0;
            if (!p->w || !any)
                continue;
            Kept *k = &kept[nkept++];
            memcpy(k->R, rests[mesh][t], sizeof k->R);
            k->p = *p;
            k->tex = malloc(sizeof(uint32_t) * p->w * p->h);
            for (uint32_t y = 0; y < p->h; y++)
                memcpy(k->tex + y * p->w, tex + (p->y + y) * BLOODMAP_W + p->x, sizeof(uint32_t) * p->w);
            float u[3], v[3];
            for (int i = 0; i < 3; i++) {
                k->lo[i] = fminf(k->R[0][i], fminf(k->R[1][i], k->R[2][i]));
                k->hi[i] = fmaxf(k->R[0][i], fmaxf(k->R[1][i], k->R[2][i]));
                u[i] = k->R[1][i] - k->R[0][i], v[i] = k->R[2][i] - k->R[0][i];
            }
            k->n[0] = u[1] * v[2] - u[2] * v[1], k->n[1] = u[2] * v[0] - u[0] * v[2], k->n[2] = u[0] * v[1] - u[1] * v[0];
            float l = len3(k->n);
            k->size = sqrtf(l);
            for (int i = 0; i < 3; i++)
                k->n[i] /= l > 0 ? l : 1;
        }
    }
    bloodmap_drop(mesh);
    ntri = ntri < RUN_TRIS ? ntri : RUN_TRIS;
    Size *sizes = malloc(sizeof *sizes * (ntri + 1));
    float (*rest)[3][3] = malloc(sizeof *rest * (ntri + 1));
    uint32_t n = 0;
    for (uint32_t t = 0; t < ntri; t++) {
        float P[3][3];
        if (!skin->corners(skin->ctx, t, P, rest[t]))
            continue;
        /* its longest edge along the map, the corner facing it above */
        int first = 0;
        float longest = -1;
        for (int k = 0; k < 3; k++) {
            float e[3] = {P[(k + 1) % 3][0] - P[k][0], P[(k + 1) % 3][1] - P[k][1], P[(k + 1) % 3][2] - P[k][2]};
            if (dot3(e, e) > longest)
                longest = dot3(e, e), first = k;
        }
        const float *A = P[first], *B = P[(first + 1) % 3], *C = P[(first + 2) % 3];
        float e[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, g[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]}, base = len3(e);
        if (!(base > 1e-3f))
            continue;
        float foot = dot3(e, g) / base, h2 = dot3(g, g) - foot * foot, high = h2 > 0 ? sqrtf(h2) : 0;
        /* (as given too: one with no area among the vertices cannot be placed) */
        const float (*R)[3] = rest[t];
        float re[3] = {R[1][0] - R[0][0], R[1][1] - R[0][1], R[1][2] - R[0][2]}, rg[3] = {R[2][0] - R[0][0], R[2][1] - R[0][1], R[2][2] - R[0][2]};
        float den = dot3(re, re) * dot3(rg, rg) - dot3(re, rg) * dot3(re, rg);
        if (!(high > 1e-3f) || !(den > 1e-12f * dot3(re, re) * dot3(rg, rg)))
            continue;
        sizes[n++] = (Size){t, base, high, foot, first};
    }
    qsort(sizes, n, sizeof *sizes, taller);
    /* in rows, the tallest first: as fine as a unit a texel, coarser until they all fit */
    Place *pl = calloc(ntri + 1, sizeof *pl);
    float unit = 1.0f;
    for (int tries = 0; tries < 60; tries++, unit *= 1.08f) {
        uint32_t x = 2, y = 2, row = 0, k = 0;
        for (; k < n; k++) {
            uint32_t w = (uint32_t)ceilf(sizes[k].base / unit + 2 * PAD) + 1, h = (uint32_t)ceilf(sizes[k].high / unit + 2 * PAD) + 1;
            if (w > BLOODMAP_W - 4)
                break;
            if (x + w > BLOODMAP_W - 2)
                x = 2, y += row, row = 0;
            if (y + h > BLOODMAP_H - 2)
                break;
            Place *p = &pl[sizes[k].t];
            *p = (Place){(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, {{0}}};
            int f = sizes[k].first;
            p->c[f][0] = (float)x + PAD + 0.5f, p->c[f][1] = (float)y + PAD + 0.5f;
            p->c[(f + 1) % 3][0] = p->c[f][0] + sizes[k].base / unit, p->c[(f + 1) % 3][1] = p->c[f][1];
            p->c[(f + 2) % 3][0] = p->c[f][0] + sizes[k].foot / unit, p->c[(f + 2) % 3][1] = p->c[f][1] + sizes[k].high / unit;
            x += w;
            row = h > row ? h : row;
        }
        if (k == n)
            break;
        memset(pl, 0, sizeof *pl * (ntri + 1));
    }
    /* each triangle's rows: a place p among its vertices as given is A + s (B - A) + t (C - A), s and t each p - A's
     * dot with a vector of the triangle's - so its texel is one row's dot with (p, 1) */
    uint32_t *maps = store + RUN_MESHES + (size_t)mesh * RUN_TRIS * RUN_MAP_WORDS;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t t = sizes[k].t;
        const Place *p = &pl[t];
        if (!p->w)
            continue;
        const float (*R)[3] = rest[t];
        float u[3], v[3];
        for (int i = 0; i < 3; i++)
            u[i] = R[1][i] - R[0][i], v[i] = R[2][i] - R[0][i];
        float uu = dot3(u, u), uv = dot3(u, v), vv = dot3(v, v), den = uu * vv - uv * uv, rows[2][4];
        for (int r = 0; r < 2; r++) {
            float ds = p->c[1][r] - p->c[0][r], dt = p->c[2][r] - p->c[0][r];
            rows[r][3] = p->c[0][r] + (r ? (float)(mesh * BLOODMAP_H) : 0.0f);
            for (int i = 0; i < 3; i++) {
                float gs = (vv * u[i] - uv * v[i]) / den, gt = (uu * v[i] - uv * u[i]) / den;
                rows[r][i] = gs * ds + gt * dt;
                rows[r][3] -= rows[r][i] * R[0][i];
            }
        }
        memcpy(maps + (size_t)t * RUN_MAP_WORDS, rows, sizeof rows);
    }
    int show = getenv("MHFU_BLOODMAP_SHOW") != NULL;
    store[mesh] = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16 | (show ? 1u << 24 : 0);
    touch((uint32_t)mesh, (uint32_t)mesh + 1);
    touch(RUN_MESHES + (uint32_t)mesh * RUN_TRIS * RUN_MAP_WORDS, RUN_MESHES + (uint32_t)(mesh + 1) * RUN_TRIS * RUN_MAP_WORDS);
    places[mesh] = pl;
    nplaces[mesh] = ntri;
    units[mesh] = unit;
    rests[mesh] = rest;
    uint32_t moved = 0;
    for (uint32_t t = 0; t < ntri && nkept; t++) {
        const Place *p = &pl[t];
        if (!p->w)
            continue;
        const float (*R)[3] = rest[t];
        float lo[3], hi[3];
        for (int i = 0; i < 3; i++)
            lo[i] = fminf(R[0][i], fminf(R[1][i], R[2][i])), hi[i] = fmaxf(R[0][i], fmaxf(R[1][i], R[2][i]));
        uint32_t cand[24], nc = 0;
        for (uint32_t k = 0; k < nkept && nc < 24; k++) {
            int over = 1;
            for (int i = 0; i < 3 && over; i++) {
                float slack = 0.15f * kept[k].size;
                over = lo[i] <= kept[k].hi[i] + slack && hi[i] >= kept[k].lo[i] - slack;
            }
            if (over)
                cand[nc++] = k;
        }
        if (!nc)
            continue;
        uint32_t *tex = texels(mesh), first = ~0u, last = 0;
        float ax = p->c[0][0], ay = p->c[0][1], ux = p->c[1][0] - ax, uy = p->c[1][1] - ay, vx = p->c[2][0] - ax, vy = p->c[2][1] - ay;
        float den = ux * vy - uy * vx;
        if (fabsf(den) < 1e-6f)
            continue;
        for (uint32_t y = p->y; y < (uint32_t)p->y + p->h; y++)
            for (uint32_t x = p->x; x < (uint32_t)p->x + p->w; x++) {
                float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay, b1 = (px * vy - py * vx) / den, b2 = (ux * py - uy * px) / den, at[3];
                for (int i = 0; i < 3; i++)
                    at[i] = R[0][i] + b1 * (R[1][i] - R[0][i]) + b2 * (R[2][i] - R[0][i]);
                float best = 0.6f;
                uint32_t word = 0;
                for (uint32_t c = 0; c < nc; c++) {
                    const Kept *k = &kept[cand[c]];
                    float u[3], v[3], w[3];
                    for (int i = 0; i < 3; i++)
                        u[i] = k->R[1][i] - k->R[0][i], v[i] = k->R[2][i] - k->R[0][i], w[i] = at[i] - k->R[0][i];
                    float uu = dot3(u, u), uv = dot3(u, v), vv = dot3(v, v), wu = dot3(w, u), wv = dot3(w, v), d2 = uu * vv - uv * uv;
                    if (!(d2 > 0))
                        continue;
                    float s1 = (vv * wu - uv * wv) / d2, s2 = (uu * wv - uv * wu) / d2, s0 = 1.0f - s1 - s2;
                    /* (how far it is from being on that triangle: outside its edges, or off its plane) */
                    float out = fmaxf(fmaxf(-s0, fmaxf(-s1, -s2)), 0.0f) + fabsf(dot3(w, k->n)) / fmaxf(k->size, 1e-9f);
                    if (out >= best)
                        continue;
                    float ox = k->p.c[0][0] + s1 * (k->p.c[1][0] - k->p.c[0][0]) + s2 * (k->p.c[2][0] - k->p.c[0][0]) - (float)k->p.x;
                    float oy = k->p.c[0][1] + s1 * (k->p.c[1][1] - k->p.c[0][1]) + s2 * (k->p.c[2][1] - k->p.c[0][1]) - (float)k->p.y;
                    int ix = (int)ox, iy = (int)oy;
                    ix = ix < 0 ? 0 : ix >= k->p.w ? k->p.w - 1 : ix, iy = iy < 0 ? 0 : iy >= k->p.h ? k->p.h - 1 : iy;
                    best = out, word = k->tex[iy * k->p.w + ix];
                }
                if (word) {
                    tex[y * BLOODMAP_W + x] = word;
                    moved++;
                    first = y * BLOODMAP_W + x < first ? y * BLOODMAP_W + x : first, last = y * BLOODMAP_W + x;
                }
            }
        if (first <= last)
            touch(MAPS + (uint32_t)mesh * TEXELS + first, MAPS + (uint32_t)mesh * TEXELS + last + 1);
    }
    if (kept)
        fprintf(stderr, "bloodmap %d: laid out anew (%u triangles), %u of the old one's triangles had blood, %u texels of it kept\n", mesh, ntri, nkept, moved);
    for (uint32_t k = 0; k < nkept; k++)
        free(kept[k].tex);
    free(kept);
    if (getenv("MHFU_BLOODMAP_LOG") || show)
        fprintf(stderr, "bloodmap %d: %u triangles of %u placed, a texel %.2f units\n", mesh, n, ntri, (double)unit);
    free(sizes);
    return unit;
}

/* The texels of a triangle's place, each with where it is among the triangle's corners (outside it too, within PAD of
 * its edges): what paints them is asked for each */
typedef float (*Paint)(void *ctx, const float *at, const float *normal);     /* how much blood there: 0 none, to 1 */

/* (field: 0 blood - it adds, lying so thick; 1 a cut, 2 a bruise - the most there has been) */
static void paint_triangle(int mesh, uint32_t t, const float (*P)[3], const float *normal, Paint paint, void *ctx, uint32_t code, float thick, int field) {
    const Place *p = &places[mesh][t];
    if (!p->w)
        return;
    uint32_t *tex = texels(mesh);
    float ax = p->c[0][0], ay = p->c[0][1], ux = p->c[1][0] - ax, uy = p->c[1][1] - ay, vx = p->c[2][0] - ax, vy = p->c[2][1] - ay;
    float den = ux * vy - uy * vx;
    if (fabsf(den) < 1e-6f)
        return;
    /* (how far outside an edge, in texels, a texel may be: its barycentric coordinate times the corner's height) */
    float high[3];
    for (int k = 0; k < 3; k++) {
        float ex = p->c[(k + 2) % 3][0] - p->c[(k + 1) % 3][0], ey = p->c[(k + 2) % 3][1] - p->c[(k + 1) % 3][1];
        high[k] = fabsf(den) / fmaxf(sqrtf(ex * ex + ey * ey), 1e-6f);
    }
    uint32_t lo = ~0u, hi = 0;
    for (uint32_t y = p->y; y < (uint32_t)p->y + p->h; y++)
        for (uint32_t x = p->x; x < (uint32_t)p->x + p->w; x++) {
            float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay, bc[3];
            bc[1] = (px * vy - py * vx) / den, bc[2] = (ux * py - uy * px) / den, bc[0] = 1.0f - bc[1] - bc[2];
            if (bc[0] * high[0] < -PAD - 0.5f || bc[1] * high[1] < -PAD - 0.5f || bc[2] * high[2] < -PAD - 0.5f)
                continue;
            float at[3];
            for (int i = 0; i < 3; i++)
                at[i] = bc[0] * P[0][i] + bc[1] * P[1][i] + bc[2] * P[2][i];
            float much = paint(ctx, at, normal);
            if (!(much > 0.004f))
                continue;
            uint32_t *w = &tex[y * BLOODMAP_W + x], had = *w & 255u, lay = *w >> 8 & 255u;
            if (field) {
                uint32_t shift = field == 1 ? 24 : 28, v = (uint32_t)((much > 1 ? 1 : much) * 15.0f + 0.5f);
                if (v <= (*w >> shift & 15u))
                    continue;
                *w = (*w & ~(15u << shift)) | v << shift;
                uint32_t aw = y * BLOODMAP_W + x;
                lo = aw < lo ? aw : lo, hi = aw > hi ? aw : hi;
                continue;
            }
            /* (it adds: what is there and what comes, the skin left bare by both less) */
            float a = (float)had / 255.0f, sum = 1.0f - (1.0f - a) * (1.0f - (much > 1 ? 1 : much));
            float l = (float)lay / 255.0f + thick * much * (0.35f + 0.65f * a);
            *w = (uint32_t)(sum * 255.0f + 0.5f) | (uint32_t)((l > 1 ? 1 : l) * 255.0f + 0.5f) << 8 | code << 16;
            uint32_t at_word = y * BLOODMAP_W + x;
            lo = at_word < lo ? at_word : lo, hi = at_word > hi ? at_word : hi;
        }
    if (lo <= hi)
        touch(MAPS + (uint32_t)mesh * TEXELS + lo, MAPS + (uint32_t)mesh * TEXELS + hi + 1);
}

static uint32_t wet_code(float now) {
    now_code = 1.0 + fmod(now > 0 ? now : 0, 255.0);
    return (uint32_t)now_code;
}

static float hash2(float x, float y) {
    float s = sinf(x * 127.1f + y * 311.7f) * 43758.5453f;
    return s - floorf(s);
}
static float noise2(float x, float y) {
    float xi = floorf(x), yi = floorf(y), fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* A splash: seen flat on the skin where it landed (across: two ways at right angles to out), a blot at its middle with
 * a ragged rim, arms thrown out from it, and drops beyond - all less the further behind or before the skin there */
typedef struct { BloodSplash s; float u[3], v[3]; } Splash;

static float splash_paint(void *ctx, const float *at, const float *normal) {
    const Splash *sp = ctx;
    const BloodSplash *s = &sp->s;
    float d[3] = {at[0] - s->at[0], at[1] - s->at[1], at[2] - s->at[2]};
    float x = dot3(d, sp->u), y = dot3(d, sp->v), z = dot3(d, s->out), facing = dot3(normal, s->out);
    if (facing < -0.1f)
        return 0;
    if (s->lengthen > 1)
        x /= s->lengthen;
    float deep = fabsf(z) / (s->radius * 0.6f + 6.0f);
    if (s->kind == SPLASH_CUT) {
        /* the cut: along x (as long as it was made, here 1 at its ends), tapering, its middle wandering, its width
         * ragged - 1 down its middle, 0 at its edge */
        float sd = s->seed * 53.0f, taper = 1.0f - x * x / (s->radius * s->radius);
        if (taper <= 0 || deep > 1)
            return 0;
        taper = powf(taper, 0.6f);
        float along = x / s->radius * s->lengthen;
        float mid = ((noise2(along * 0.35f + sd, sd) - 0.5f) * 1.6f + (noise2(along * 1.3f, sd + 3) - 0.5f) * 0.5f) * s->radius * taper;
        float wide = s->radius * taper * (0.65f + 0.6f * noise2(along * 0.9f + sd, 5.0f));
        float in = 1.0f - fabsf(y - mid) / fmaxf(wide, 0.3f);
        return in > 0 ? (0.25f + 0.75f * in) * s->much * smooth(-0.1f, 0.25f, facing) : 0;
    }
    float r = sqrtf(x * x + y * y) / s->radius;
    if (s->kind == SPLASH_HOLE)
        return deep > 1 ? 0 : (1.0f - smooth(0.5f, 1.0f, r)) * smooth(-0.1f, 0.25f, facing);
    if (s->kind == SPLASH_BRUISE) {
        float sd = s->seed * 47.0f, blotch = 0.6f + 0.5f * noise2(x / s->radius * 2.3f + sd, y / s->radius * 2.3f - sd);
        return deep > 1 ? 0 : (1.0f - smooth(0.35f, 1.0f, r / blotch)) * s->much * smooth(-0.1f, 0.25f, facing);
    }
    if (r > 2.2f || deep > 1)
        return 0;
    float ang = atan2f(y, x), sd = s->seed * 61.0f;
    /* the blot: its rim in and out by the way round */
    float rim = 0.42f + 0.22f * noise2(ang * 1.9f + sd, sd) + 0.12f * noise2(ang * 5.3f, sd + 7);
    float blot = 1.0f - smooth(rim - 0.07f, rim + 0.03f, r);
    /* its arms: some ways round, each its own length, thinning outward */
    float arm = noise2(ang * 3.3f + sd * 3, 3.0f + sd), reach = rim + (1.6f - rim) * smooth(0.55f, 0.95f, arm) * (0.5f + 0.5f * noise2(ang * 7.0f, sd + 11));
    float thin = noise2(ang * 14.0f + sd, 1.0f);      /* (fine streaks within an arm) */
    float arms = (1.0f - smooth(reach - 0.25f, reach, r)) * smooth(0.5f, 0.75f, arm) * smooth(0.25f, 0.6f, thin + 0.5f * (1.0f - r / 1.6f));
    /* drops about it: cells, some with a drop in */
    float cell = s->radius * 0.22f, cx = floorf(x / cell), cy = floorf(y / cell), drops = 0;
    float pick = hash2(cx + sd, cy - sd);
    if (pick > 0.72f && r > rim && r < 2.1f) {
        float ox = (cx + 0.3f + 0.4f * hash2(cx, cy + sd)) * cell, oy = (cy + 0.3f + 0.4f * hash2(cx + 9 + sd, cy)) * cell;
        float size = cell * (0.1f + 0.28f * hash2(cx - sd, cy + 5)) * (1.3f - 0.4f * r);
        float dd = sqrtf((x - ox) * (x - ox) + (y - oy) * (y - oy));
        drops = 1.0f - smooth(size * 0.7f, size, dd);
    }
    float much = fmaxf(blot, fmaxf(arms * 0.9f, drops * 0.85f));
    return much * s->much * (1.0f - smooth(0.5f, 1.0f, deep)) * smooth(-0.1f, 0.25f, facing);
}

static void frame_of(const float *out, const float *along, float *u, float *v) {
    float a[3] = {along[0], along[1], along[2]};
    float d = dot3(a, out);
    for (int i = 0; i < 3; i++)
        a[i] -= out[i] * d;
    if (dot3(a, a) < 1e-6f) {
        float pick[3] = {fabsf(out[1]) < 0.9f ? 0.0f : 1.0f, fabsf(out[1]) < 0.9f ? 1.0f : 0.0f, 0};
        d = dot3(pick, out);
        for (int i = 0; i < 3; i++)
            a[i] = pick[i] - out[i] * d;
    }
    float l = len3(a);
    for (int i = 0; i < 3; i++)
        u[i] = a[i] / l;
    v[0] = out[1] * u[2] - out[2] * u[1], v[1] = out[2] * u[0] - out[0] * u[2], v[2] = out[0] * u[1] - out[1] * u[0];
}

static int triangle_normal(const float (*P)[3], float *n) {
    float u[3], v[3];
    for (int i = 0; i < 3; i++)
        u[i] = P[1][i] - P[0][i], v[i] = P[2][i] - P[0][i];
    n[0] = u[1] * v[2] - u[2] * v[1], n[1] = u[2] * v[0] - u[0] * v[2], n[2] = u[0] * v[1] - u[1] * v[0];
    float l = len3(n);
    if (!(l > 1e-9f))
        return 0;
    for (int i = 0; i < 3; i++)
        n[i] /= l;
    return 1;
}

void bloodmap_splash(int mesh, const TrailSkin *skin, const BloodSplash *s, const uint8_t *flip, float now) {
    if (mesh < 0 || mesh >= RUN_MESHES || !places[mesh])
        return;
    Splash sp = {.s = *s};
    frame_of(s->out, s->along, sp.u, sp.v);
    uint32_t code = wet_code(now);
    float far = s->radius * 2.3f * (s->lengthen > 1 ? s->lengthen : 1) + 4.0f;
    for (uint32_t t = 0; t < nplaces[mesh]; t++) {
        float P[3][3], R[3][3], n[3];
        if (!places[mesh][t].w || !skin->corners(skin->ctx, t, P, R))
            continue;
        int near = 1;
        for (int i = 0; i < 3 && near; i++) {
            float lo = fminf(P[0][i], fminf(P[1][i], P[2][i])), hi = fmaxf(P[0][i], fmaxf(P[1][i], P[2][i]));
            near = s->at[i] > lo - far && s->at[i] < hi + far;
        }
        if (!near || !triangle_normal((const float (*)[3])P, n))
            continue;
        if (flip && flip[t])
            n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
        paint_triangle(mesh, t, (const float (*)[3])P, n, splash_paint, &sp, code, 0.5f,
                       s->kind == SPLASH_CUT || s->kind == SPLASH_HOLE ? 1 : s->kind == SPLASH_BRUISE ? 2 : 0);
    }
}

typedef struct { float a[3], ab[3], l2, half, much; } Stroke;

static float stroke_paint(void *ctx, const float *at, const float *normal) {
    const Stroke *k = ctx;
    (void)normal;
    float d[3] = {at[0] - k->a[0], at[1] - k->a[1], at[2] - k->a[2]};
    float u = k->l2 > 0 ? dot3(d, k->ab) / k->l2 : 0;
    u = u < 0 ? 0 : u > 1 ? 1 : u;
    for (int i = 0; i < 3; i++)
        d[i] -= k->ab[i] * u;
    return k->much * (1.0f - smooth(k->half * 0.55f, k->half, len3(d)));
}

void bloodmap_stroke(int mesh, const TrailSkin *skin, const uint32_t *tris, int ntris, const float *a, const float *b, float half, float much, float now) {
    if (mesh < 0 || mesh >= RUN_MESHES || !places[mesh])
        return;
    /* (no thinner than a texel and a bit is wide: or it would fall between them) */
    half = fmaxf(half, 0.75f * units[mesh]);
    Stroke k = {{a[0], a[1], a[2]}, {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, 0, half, much};
    k.l2 = dot3(k.ab, k.ab);
    uint32_t code = wet_code(now);
    for (int i = 0; i < ntris; i++) {
        float P[3][3], R[3][3], n[3] = {0, 1, 0};
        if (tris[i] >= nplaces[mesh] || !skin->corners(skin->ctx, tris[i], P, R))
            continue;
        paint_triangle(mesh, tris[i], (const float (*)[3])P, n, stroke_paint, &k, code, 0.8f, 0);
    }
}

/* (MHFU_BLOODMAP_SHOW) */
float bloodmap_unit(int mesh) { return mesh >= 0 && mesh < RUN_MESHES && places[mesh] ? units[mesh] : 0; }

/* Each frame: a few rows of each map looked over, and what got wet two minutes ago or more marked dry for good - the
 * moment a texel keeps goes round in 255 seconds, and it would be wet again */
void bloodmap_frame(float now) {
    static uint32_t row;
    uint32_t code = (uint32_t)(1.0 + fmod(now > 0 ? now : 0, 255.0));
    now_code = 1.0 + fmod(now > 0 ? now : 0, 255.0);
    for (int mesh = 0; mesh < RUN_MESHES; mesh++) {
        if (!places[mesh])
            continue;
        uint32_t *tex = texels(mesh), lo = ~0u, hi = 0;
        for (uint32_t y = row; y < row + 24 && y < BLOODMAP_H; y++)
            for (uint32_t x = 0; x < BLOODMAP_W; x++) {
                uint32_t w = tex[y * BLOODMAP_W + x], c = w >> 16 & 255u;
                if (c && (code + 255u - c) % 255u > 120u) {
                    tex[y * BLOODMAP_W + x] = w & 0xFF00FFFFu;
                    lo = y * BLOODMAP_W + x < lo ? y * BLOODMAP_W + x : lo, hi = y * BLOODMAP_W + x;
                }
            }
        if (lo <= hi)
            touch(MAPS + (uint32_t)mesh * TEXELS + lo, MAPS + (uint32_t)mesh * TEXELS + hi + 1);
    }
    row = row + 24 >= BLOODMAP_H ? 0 : row + 24;
}
