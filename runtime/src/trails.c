/* Blood running down an animal: see trails.h. */
#include "trails.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t key; uint32_t side; } Edge;   /* (side: its triangle, times 4, its corner, and 2 if it runs from the greater place) */

static int edge_order(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->side < y->side ? -1 : x->side > y->side;
}

static uint32_t root(uint32_t *up, uint32_t i) {
    while (up[i] != i)
        i = up[i] = up[up[i]];
    return i;
}

void trail_topology(const float (*pos)[3], uint32_t nv, const uint32_t *tri, uint32_t ntri, float weld, int32_t (*nbr)[3],
                    TrailStats *stats) {
    TrailStats st = {.ntri = ntri};
    /* the places: a grid of cells weld wide, each place in the cell it is in - a vertex is at a place already there
     * if one lies within weld of it in its cell or those about it */
    uint32_t tsize = 1024;
    while (tsize < nv * 2)
        tsize *= 2;
    uint32_t *head = calloc(tsize, sizeof *head), *next = malloc(sizeof *next * (nv + 1)), *first = malloc(sizeof *first * (nv + 1));
    uint32_t *place = malloc(sizeof *place * (nv + 1));
    for (uint32_t i = 0; i < nv; i++) {
        int32_t c[3];
        for (int k = 0; k < 3; k++)
            c[k] = (int32_t)floorf(pos[i][k] / weld);
        uint32_t found = 0;
        for (int n = 0; n < 27 && !found; n++) {
            uint32_t h = (uint32_t)(c[0] + n % 3 - 1) * 0x9E3779B1u ^ (uint32_t)(c[1] + n / 3 % 3 - 1) * 0x85EBCA77u ^
                         (uint32_t)(c[2] + n / 9 - 1) * 0xC2B2AE3Du;
            for (uint32_t p = head[(h ^ h >> 15) & (tsize - 1)]; p && !found; p = next[p]) {
                const float *o = pos[first[p]];
                float d[3] = {o[0] - pos[i][0], o[1] - pos[i][1], o[2] - pos[i][2]};
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= weld * weld)
                    found = p;
            }
        }
        if (!found) {
            uint32_t h = (uint32_t)c[0] * 0x9E3779B1u ^ (uint32_t)c[1] * 0x85EBCA77u ^ (uint32_t)c[2] * 0xC2B2AE3Du;
            found = ++st.places;
            first[found] = i;
            next[found] = head[(h ^ h >> 15) & (tsize - 1)];
            head[(h ^ h >> 15) & (tsize - 1)] = found;
        }
        place[i] = found;
    }
    /* the edges, each with its triangle: those between the same two places together, and of them a pair that run
     * opposite ways (two triangles of one skin, side by side) before any other */
    Edge *edges = malloc(sizeof *edges * (3 * (size_t)ntri + 1));
    size_t ne = 0;
    for (uint32_t t = 0; t < ntri; t++) {
        uint32_t p[3] = {place[tri[3 * t]], place[tri[3 * t + 1]], place[tri[3 * t + 2]]};
        nbr[t][0] = nbr[t][1] = nbr[t][2] = -1;
        if (p[0] == p[1] || p[1] == p[2] || p[0] == p[2]) {
            st.flat++;
            continue;
        }
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t a = p[(k + 1) % 3], b = p[(k + 2) % 3];
            edges[ne++] = (Edge){a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a, t * 4 + k + (a > b ? 0x80000000u : 0)};
        }
    }
    qsort(edges, ne, sizeof *edges, edge_order);
    for (size_t i = 0; i < ne;) {
        size_t j = i;
        while (j < ne && edges[j].key == edges[i].key)
            j++;
        if (j - i == 1)
            st.open++;
        else
            st.inner++, st.many += j - i > 2;
        for (int pass = 0; pass < 2; pass++)
            for (size_t a = i; a < j; a++)
                for (size_t b = a + 1; b < j; b++) {
                    uint32_t sa = edges[a].side & 0x7FFFFFFFu, sb = edges[b].side & 0x7FFFFFFFu;
                    if (nbr[sa / 4][sa % 4] >= 0 || nbr[sb / 4][sb % 4] >= 0 || sa / 4 == sb / 4 ||
                        (pass == 0 && !((edges[a].side ^ edges[b].side) & 0x80000000u)))
                        continue;
                    nbr[sa / 4][sa % 4] = (int32_t)(sb / 4);
                    nbr[sb / 4][sb % 4] = (int32_t)(sa / 4);
                }
        i = j;
    }
    /* its pieces: triangles joined across their edges */
    uint32_t *up = malloc(sizeof *up * (ntri + 1)), *size = calloc(ntri + 1, sizeof *size);
    for (uint32_t t = 0; t < ntri; t++)
        up[t] = t;
    for (uint32_t t = 0; t < ntri; t++)
        for (int k = 0; k < 3; k++)
            if (nbr[t][k] >= 0)
                up[root(up, t)] = root(up, (uint32_t)nbr[t][k]);
    for (uint32_t t = 0; t < ntri; t++) {
        uint32_t r = root(up, t);
        if (place[tri[3 * t]] == place[tri[3 * t + 1]] || place[tri[3 * t + 1]] == place[tri[3 * t + 2]] || place[tri[3 * t]] == place[tri[3 * t + 2]])
            continue;
        st.islands += !size[r];
        if (++size[r] > st.largest)
            st.largest = size[r];
    }
    free(head), free(next), free(first), free(place), free(edges), free(up), free(size);
    if (stats)
        *stats = st;
}

void trail_pieces(int32_t (*nbr)[3], uint32_t ntri, uint32_t least, uint8_t *big, uint32_t *which) {
    uint32_t *up = malloc(sizeof *up * (ntri + 1)), *size = calloc(ntri + 1, sizeof *size);
    for (uint32_t t = 0; t < ntri; t++)
        up[t] = t;
    for (uint32_t t = 0; t < ntri; t++)
        for (int k = 0; k < 3; k++)
            if (nbr[t][k] >= 0)
                up[root(up, t)] = root(up, (uint32_t)nbr[t][k]);
    for (uint32_t t = 0; t < ntri; t++)
        size[root(up, t)]++;
    for (uint32_t t = 0; t < ntri; t++)
        big[t] = size[root(up, t)] >= least;
    for (uint32_t t = 0; t < ntri && which; t++)
        which[t] = root(up, t);
    free(up), free(size);
}

/* -- the blood itself --------------------------------------------------------------------------------------------- */
#include <pthread.h>

#define HEADS (RUN_MESHES * RUN_TRIS)
#define TRICKLES 64
#define PER_TRIANGLE 30             /* a triangle's pieces, at most (the pixel stage looks at 32) */
#define WANDER 1.4f                 /* how far a trickle is drawn to either side of its line, units (ge_frag.glsl's) */

typedef struct {
    uint64_t owner;
    uint32_t ntri;
    int32_t (*nbr)[3];
    uint8_t *flip;
    int used;
    double touched;
    uint32_t *over_at, *over;       /* what lies over each triangle (fur): over[over_at[t]] to over[over_at[t + 1]] */
} Mesh;
#define OVER 12                     /* of them, as many as a piece is drawn on */
typedef struct {
    int live, mesh;
    uint32_t tri;
    float bc[3];
    float reach, left, width, speed, s, seed, still;
    uint32_t rgb;
    int edge;                       /* the edge it runs along, not able to cross it (a fold of the skin), or -1 */
    float a_bc[3], a_s, a_t, a_w;   /* where its piece being laid began */
    uint32_t ent[4], bead;          /* that piece: on its triangle, and on those across its edges; and its bead */
    /* what it is laying on the triangles over those it runs on (a hide's outer layer, fur): on each a piece of its
     * own, going on from one triangle underneath to the next for as long as it is straight - one of these is often
     * as large as many of those. Where each began and last ended among its triangle's corners, how far along the
     * trickle, when and how wide; ticks since it was last on it */
    struct { uint32_t tri, e; float a[3], b[3], a_s, a_t, a_w, b_s, b_t, b_w; int missed; } ov[OVER];
    int idle, hops;                 /* (hops: times it has gone from one piece of the skin on to another) */
} Trickle;

int trails_told;                    /* (MHFU_TRAIL_LOG: what each trickle does) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t store[HEADS + RUN_ENTRIES * RUN_WORDS];
static uint32_t dirty_from, dirty_to;
static struct { uint8_t used, pinned, mesh; uint32_t tri; } ent[RUN_ENTRIES + 1];
static uint32_t ent_at = 1;
static Mesh meshes[RUN_MESHES];
static Trickle trickles[TRICKLES];
static double epoch = -1, last30;
static float now_shown;

static void touch(uint32_t from, uint32_t to) {
    pthread_mutex_lock(&lock);
    if (dirty_from == dirty_to)
        dirty_from = from, dirty_to = to;
    else
        dirty_from = from < dirty_from ? from : dirty_from, dirty_to = to > dirty_to ? to : dirty_to;
    pthread_mutex_unlock(&lock);
}

const uint32_t *trails_words(uint32_t *from, uint32_t *to) {
    pthread_mutex_lock(&lock);
    *from = dirty_from, *to = dirty_to;
    dirty_from = dirty_to = 0;
    pthread_mutex_unlock(&lock);
    return store;
}

float trails_now(void) { return now_shown; }

static uint32_t *words(uint32_t e) { return store + HEADS + (e - 1) * RUN_WORDS; }

static void set_word(uint32_t at, uint32_t v) {
    if (store[at] != v) {
        store[at] = v;
        touch(at, at + 1);
    }
}

/* A piece out of its triangle's */
static void ent_free(uint32_t e) {
    if (!e || !ent[e].used)
        return;
    uint32_t head = (uint32_t)ent[e].mesh * RUN_TRIS + ent[e].tri;
    if (store[head] == e) {
        set_word(head, words(e)[12]);
    } else {
        uint32_t p = store[head];
        for (int n = 0; p && p <= RUN_ENTRIES && n < 64 && words(p)[12] != e; n++)
            p = words(p)[12];
        if (p && p <= RUN_ENTRIES && words(p)[12] == e)
            set_word(HEADS + (p - 1) * RUN_WORDS + 12, words(e)[12]);
    }
    ent[e].used = ent[e].pinned = 0;
}

/* A new piece on a triangle, the first of its pieces (pinned: not to be taken for another while it is being laid);
 * the triangle's oldest gone if it has too many, the oldest of all if there is no room. 0: none to be had */
static uint32_t ent_new(int mesh, uint32_t tri) {
    uint32_t head = (uint32_t)mesh * RUN_TRIS + tri, n = 0, last = 0;
    for (uint32_t p = store[head]; p && p <= RUN_ENTRIES && n < 64; p = words(p)[12], n++)
        if (!ent[p].pinned)
            last = p;
    if (n >= PER_TRIANGLE && last)
        ent_free(last);
    uint32_t e = 0;
    for (uint32_t k = 0; k < RUN_ENTRIES && !e; k++) {
        uint32_t c = (ent_at + k - 1) % RUN_ENTRIES + 1;
        if (!ent[c].used)
            e = c;
    }
    for (uint32_t k = 0; k < RUN_ENTRIES && !e; k++) {
        uint32_t c = (ent_at + k - 1) % RUN_ENTRIES + 1;
        if (!ent[c].pinned) {
            ent_free(c);
            e = c;
        }
    }
    if (!e)
        return 0;
    ent_at = e % RUN_ENTRIES + 1;
    ent[e].used = ent[e].pinned = 1;
    ent[e].mesh = (uint8_t)mesh;
    ent[e].tri = tri;
    memset(words(e), 0, sizeof(uint32_t) * RUN_WORDS);
    words(e)[12] = store[head];
    touch(HEADS + (e - 1) * RUN_WORDS, HEADS + e * RUN_WORDS);
    return e;
}

/* (the piece written whole before its triangle names it: the pixel stage reads as this writes) */
static void ent_show(uint32_t e) {
    set_word((uint32_t)ent[e].mesh * RUN_TRIS + ent[e].tri, e);
}

static void ent_write(uint32_t e, const float *a, float aw, const float *b, float bw, float sa, float sb, float ta, float tb, uint32_t look,
                      float seed, float scale) {
    float f[12] = {a[0], a[1], a[2], aw * scale, b[0], b[1], b[2], bw * scale, sa, sb, ta, tb};
    uint32_t *w = words(e);
    memcpy(w, f, sizeof f);
    w[13] = look;
    memcpy(&w[14], &seed, 4);
    memcpy(&w[15], &scale, 4);
    touch(HEADS + (e - 1) * RUN_WORDS, HEADS + e * RUN_WORDS);
}

int trails_mesh(uint64_t owner) {
    for (int m = 0; m < RUN_MESHES; m++)
        if (meshes[m].used && meshes[m].owner == owner)
            return m;
    return -1;
}

void trails_mesh_drop(int m) {
    if (m < 0 || m >= RUN_MESHES || !meshes[m].used)
        return;
    for (int k = 0; k < TRICKLES; k++)
        if (trickles[k].mesh == m)
            trickles[k].live = 0;
    for (uint32_t e = 1; e <= RUN_ENTRIES; e++)
        if (ent[e].used && ent[e].mesh == m)
            ent[e].used = ent[e].pinned = 0;
    memset(store + (size_t)m * RUN_TRIS, 0, sizeof(uint32_t) * RUN_TRIS);
    touch((uint32_t)m * RUN_TRIS, (uint32_t)(m + 1) * RUN_TRIS);
    free(meshes[m].nbr);
    free(meshes[m].flip);
    free(meshes[m].over_at);
    free(meshes[m].over);
    memset(&meshes[m], 0, sizeof meshes[m]);
}

void trails_clear(void) {
    for (int m = 0; m < RUN_MESHES; m++)
        trails_mesh_drop(m);
}

int trails_mesh_make(uint64_t owner, uint32_t ntri, int32_t (*nbr)[3], uint8_t *flip, uint32_t *over_at, uint32_t *over) {
    int m = trails_mesh(owner);
    for (int k = 0; k < RUN_MESHES && m < 0; k++)
        if (!meshes[k].used)
            m = k;
    for (int k = 0; k < RUN_MESHES && m < 0; k++) {
        int older = 1;
        for (int o = 0; o < RUN_MESHES; o++)
            older = older && meshes[k].touched <= meshes[o].touched;
        if (older)
            m = k;
    }
    trails_mesh_drop(m);
    meshes[m] = (Mesh){owner, ntri < RUN_TRIS ? ntri : RUN_TRIS, nbr, flip, 1, last30, over_at, over};
    return m;
}

/* -- a trickle's step -- */
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static void at3(const float (*c)[3], const float *bc, float *o) {
    for (int i = 0; i < 3; i++)
        o[i] = bc[0] * c[0][i] + bc[1] * c[1][i] + bc[2] * c[2][i];
}
/* Where x is among a triangle's corners (as it lies in the triangle's plane; outside it too); 0 if it has no area */
static int among(const float (*c)[3], const float *x, float *bc) {
    float u[3], v[3], w[3];
    for (int i = 0; i < 3; i++)
        u[i] = c[1][i] - c[0][i], v[i] = c[2][i] - c[0][i], w[i] = x[i] - c[0][i];
    float uu = dot3(u, u), uv = dot3(u, v), vv = dot3(v, v), wu = dot3(w, u), wv = dot3(w, v), den = uu * vv - uv * uv;
    if (!(den > 1e-12f * uu * vv) || !(den > 0))
        return 0;
    bc[1] = (vv * wu - uv * wv) / den;
    bc[2] = (uu * wv - uv * wu) / den;
    bc[0] = 1.0f - bc[1] - bc[2];
    return 1;
}
/* A triangle's normal, out of the skin, and its area */
static float normal(const float (*c)[3], int flip, float *n) {
    float u[3], v[3];
    for (int i = 0; i < 3; i++)
        u[i] = c[1][i] - c[0][i], v[i] = c[2][i] - c[0][i];
    cross3(u, v, n);
    float l = sqrtf(dot3(n, n));
    for (int i = 0; i < 3 && l > 0; i++)
        n[i] /= flip ? -l : l;
    return 0.5f * l;
}
static float wander(float x) {
    float i = floorf(x), f = x - i;
    f = f * f * (3.0f - 2.0f * f);
    float a = sinf(i * 127.1f) * 43758.5453f, b = sinf((i + 1.0f) * 127.1f) * 43758.5453f;
    a -= floorf(a), b -= floorf(b);
    return a + (b - a) * f;
}

/* How wide (half) a trickle is where it has run s: thin where it starts, thinner as it runs out */
static float width_at(const Trickle *t, float s) {
    float start = 0.45f + s / 12.0f, spent = t->reach > 0 ? 1.0f - s / t->reach : 0;
    return t->width * (start < 1 ? start : 1) * (0.6f + 0.4f * (spent < 0 ? 0 : spent));
}

/* The piece a trickle is laying, as far as it has come: on its triangle (P, R: its corners now, and as given), and on
 * each triangle across an edge it comes near, there too - placed among that one's corners as it lies now. done: the
 * piece is finished - those it does not reach let go, the rest left to be taken in their turn */
static void piece(Trickle *t, const Mesh *m, const TrailSkin *skin, const float (*P)[3], const float (*R)[3], float now, int done) {
    float a[3], b[3], A[3], B[3], n[3], rn[3];
    at3(R, t->a_bc, a), at3(R, t->bc, b);
    at3(P, t->a_bc, A), at3(P, t->bc, B);
    float area = normal(P, 0, n), rest = normal(R, 0, rn), scale = area > 0 ? sqrtf(rest / area) : 1.0f, bw = width_at(t, t->s);
    if (done && trails_told)
        fprintf(stderr, "piece: trickle %d triangle %u entry %u, %.4f %.4f %.4f to %.4f %.4f %.4f (%.1f units of the world), s %.1f to %.1f, wide %.2f and %.2f, scale %.5f\n",
                (int)(t - trickles), t->tri, t->ent[0], (double)a[0], (double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2],
                (double)sqrtf((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2])), (double)t->a_s, (double)t->s,
                (double)t->a_w, (double)bw, (double)scale);
    if (t->ent[0]) {
        ent_write(t->ent[0], a, t->a_w, b, bw, t->a_s, t->s, t->a_t, now, t->rgb, t->seed, scale);
        ent_show(t->ent[0]);
    }
    float reach = 1.8f * t->width + WANDER + 0.5f;
    for (int j = 0; j < 3; j++) {
        int32_t nb = m->nbr[t->tri][j];
        const float *e0 = P[(j + 1) % 3], *e1 = P[(j + 2) % 3];
        float e[3] = {e1[0] - e0[0], e1[1] - e0[1], e1[2] - e0[2]}, el = sqrtf(dot3(e, e));
        float high = el > 0 ? 2.0f * area / el : 0, near = (t->a_bc[j] < t->bc[j] ? t->a_bc[j] : t->bc[j]) * high;
        float Pn[3][3], Rn[3][3], ba[3], bb[3];
        if (nb < 0 || near > reach || !skin->corners(skin->ctx, (uint32_t)nb, Pn, Rn) || !among(Pn, A, ba) || !among(Pn, B, bb)) {
            if (done || nb < 0) {
                ent_free(t->ent[1 + j]);
                t->ent[1 + j] = 0;
            }
            continue;
        }
        if (!t->ent[1 + j] && !(t->ent[1 + j] = ent_new(t->mesh, (uint32_t)nb)))
            continue;
        float an[3], bn[3], nn[3], na = normal(Pn, 0, nn), nr = normal(Rn, 0, nn);
        at3(Rn, ba, an), at3(Rn, bb, bn);
        ent_write(t->ent[1 + j], an, t->a_w, bn, bw, t->a_s, t->s, t->a_t, now, t->rgb, t->seed, na > 0 ? sqrtf(nr / na) : 1.0f);
        ent_show(t->ent[1 + j]);
    }
    /* and on what lies over its triangle: where its end is as seen through each such triangle, if that is within it */
    uint32_t from = m->over_at ? m->over_at[t->tri] : 0, to = m->over_at ? m->over_at[t->tri + 1] : 0;
    for (int k = 0; k < OVER; k++)
        t->ov[k].missed += t->ov[k].e != 0;
    for (uint32_t o = from; o < to; o++) {
        uint32_t c = m->over[o];
        float Pn[3][3], Rn[3][3], ba[3], bb[3];
        if (c >= m->ntri || !skin->corners(skin->ctx, c, Pn, Rn) || !among(Pn, B, bb) || bb[0] < -0.1f || bb[1] < -0.1f || bb[2] < -0.1f)
            continue;
        int k = -1;
        for (int i = 0; i < OVER && k < 0; i++)
            if (t->ov[i].e && t->ov[i].tri == c)
                k = i;
        float an[3], bn[3], nn[3], na = normal(Pn, 0, nn), nr = normal(Rn, 0, nn), sc = na > 0 ? sqrtf(nr / na) : 1.0f;
        if (k >= 0) {
            /* (straight still: or the piece ends where it last did, and another goes on from there) */
            float X0[3], X1[3], u[3], v[3];
            at3(Pn, t->ov[k].a, X0), at3(Pn, t->ov[k].b, X1);
            for (int i = 0; i < 3; i++)
                u[i] = X1[i] - X0[i], v[i] = B[i] - X1[i];
            float ul = sqrtf(dot3(u, u)), vl = sqrtf(dot3(v, v));
            if (ul > 8.0f && vl > 1.5f && dot3(u, v) < 0.96f * ul * vl) {
                ent[t->ov[k].e].pinned = 0;
                memcpy(t->ov[k].a, t->ov[k].b, sizeof t->ov[k].a);
                t->ov[k].a_s = t->ov[k].b_s, t->ov[k].a_t = t->ov[k].b_t, t->ov[k].a_w = t->ov[k].b_w;
                if (!(t->ov[k].e = ent_new(t->mesh, c)))
                    continue;
            }
        } else {
            for (int i = 0; i < OVER && k < 0; i++)
                if (!t->ov[i].e)
                    k = i;
            if (k < 0 || !among(Pn, A, ba) || !(t->ov[k].e = ent_new(t->mesh, c)))
                continue;
            t->ov[k].tri = c;
            memcpy(t->ov[k].a, ba, sizeof ba);
            t->ov[k].a_s = t->a_s, t->ov[k].a_t = t->a_t, t->ov[k].a_w = t->a_w;
        }
        memcpy(t->ov[k].b, bb, sizeof bb);
        t->ov[k].b_s = t->s, t->ov[k].b_t = now, t->ov[k].b_w = bw;
        t->ov[k].missed = 0;
        at3(Rn, t->ov[k].a, an), at3(Rn, bb, bn);
        ent_write(t->ov[k].e, an, t->ov[k].a_w, bn, bw, t->ov[k].a_s, t->s, t->ov[k].a_t, now, t->rgb, t->seed, sc);
        ent_show(t->ov[k].e);
    }
    for (int k = 0; k < OVER; k++)
        if (t->ov[k].e && (t->ov[k].missed > 6 || done > 1)) {     /* (no longer under it, or the trickle is done) */
            ent[t->ov[k].e].pinned = 0;
            t->ov[k].e = 0;
        }
    /* its bead: at its end, a little along the way it goes (which way the pixel stage puts its sides) */
    if (t->bead) {
        float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, dl = sqrtf(dot3(d, d)), tip[3];
        float big = bw * (1.5f + 0.6f * (t->still > 1 ? 1 : t->still));
        for (int i = 0; i < 3; i++)
            tip[i] = b[i] + (dl > 0 ? d[i] / dl : 0) * 0.02f * scale;
        ent_write(t->bead, b, big, tip, big, t->s, t->s, now, now, t->rgb | 1u << 24, t->seed, scale);
        ent_show(t->bead);
    }
    if (done) {
        for (int j = 0; j < 4; j++) {
            if (t->ent[j])
                ent[t->ent[j]].pinned = 0;
            t->ent[j] = 0;
        }
    }
}

/* A new piece begun where the trickle is */
static void begin(Trickle *t, float now) {
    memcpy(t->a_bc, t->bc, sizeof t->a_bc);
    t->a_s = t->s, t->a_t = now, t->a_w = width_at(t, t->s);
    memset(t->ent, 0, sizeof t->ent);
    t->ent[0] = ent_new(t->mesh, t->tri);
    if (t->bead && (ent[t->bead].tri != t->tri || !ent[t->bead].used)) {
        ent_free(t->bead);
        t->bead = 0;
    }
    if (!t->bead)
        t->bead = ent_new(t->mesh, t->tri);
}

/* The trickle done: its piece finished, its bead left where it stopped - or gone with the drop that fell */
static void finish_(Trickle *t, const Mesh *m, const TrailSkin *skin, const float (*P)[3], const float (*R)[3], float now, int fell);
static void finish_why(Trickle *t, const Mesh *m, const TrailSkin *skin, const float (*P)[3], const float (*R)[3], float now, int fell, const char *why) {
    if (trails_told) {
        float X[3];
        at3(P, t->bc, X);
        fprintf(stderr, "trickle %d done: %s, on triangle %u at %.0f %.0f %.0f, having run %.0f of %.0f\n", (int)(t - trickles), why, t->tri, (double)X[0],
                (double)X[1], (double)X[2], (double)t->s, (double)t->reach);
    }
    finish_(t, m, skin, P, R, now, fell);
}
static void finish_(Trickle *t, const Mesh *m, const TrailSkin *skin, const float (*P)[3], const float (*R)[3], float now, int fell) {
    if (fell) {
        ent_free(t->bead);
        t->bead = 0;
    }
    piece(t, m, skin, P, R, now, 2);
    if (t->bead)
        ent[t->bead].pinned = 0;
    t->live = 0;
}

static void step(Trickle *t, const Mesh *m, const TrailSkin *skin, float dt, float now) {
    float P[3][3], R[3][3];
    for (int hop = 0; hop < 6 && dt > 1e-4f; hop++) {
        if (!skin->corners(skin->ctx, t->tri, P, R))
            return;                     /* (not drawn now: it waits) */
        float n[3], X[3];
        if (!(normal(P, m->flip[t->tri], n) > 1e-4f)) {
            finish_why(t, m, skin, P, R, now, 0, "no area");
            return;
        }
        at3(P, t->bc, X);
        if (trails_told > 1)
            fprintf(stderr, "trickle %d: triangle %u at %.0f %.0f %.0f, normal %.2f %.2f %.2f, run %.0f, edge %d\n", (int)(t - trickles), t->tri, (double)X[0],
                    (double)X[1], (double)X[2], (double)n[0], (double)n[1], (double)n[2], (double)t->s, t->edge);
        /* down across the skin: down, less its part out of the skin - as long as the skin is steep */
        float down[3] = {n[0] * n[1], n[1] * n[1] - 1.0f, n[2] * n[1]}, steep = sqrtf(dot3(down, down));
        /* under the animal - the skin all but upside down, or level and facing down: it gathers and falls (on skin
         * that only leans over it clings, and runs on down) */
        if ((n[1] < -0.85f || (n[1] < 0 && steep < 0.12f)) && t->s > 3.0f) {
            skin->drop(skin->ctx, X, 2.5f + 3.5f * t->width);
            finish_why(t, m, skin, P, R, now, 1, "it fell from under the animal");
            return;
        }
        float dir[3] = {0, 0, 0};
        int level = steep < 0.12f;
        if (!level) {
            /* (and a little to either side as it goes: no trickle runs straight) */
            float turn = (wander(t->s * 0.035f + t->seed * 31.0f) - 0.5f) * 0.5f, side[3];
            cross3(n, down, side);
            for (int i = 0; i < 3; i++)
                dir[i] = (down[i] * cosf(turn) + side[i] * sinf(turn)) / steep;
        }
        if (t->edge >= 0 && !level) {
            /* along a fold it cannot cross: as the fold runs, as far as that is downhill - free of it once down is
             * away from it again */
            const float *e0 = P[(t->edge + 1) % 3], *e1 = P[(t->edge + 2) % 3];
            float e[3] = {e1[0] - e0[0], e1[1] - e0[1], e1[2] - e0[2]}, el = sqrtf(dot3(e, e)), in[3];
            for (int i = 0; i < 3; i++)
                e[i] /= el > 0 ? el : 1, in[i] = P[t->edge][i] - e0[i];
            float ie = dot3(in, e);
            for (int i = 0; i < 3; i++)
                in[i] -= e[i] * ie;
            if (dot3(dir, in) > 0.02f * sqrtf(dot3(in, in))) {
                t->edge = -1;
            } else {
                float along = dot3(dir, e);
                steep *= fabsf(along);
                level = steep < 0.12f;
                for (int i = 0; i < 3; i++)
                    dir[i] = along < 0 ? -e[i] : e[i];
            }
        }
        if (level) {                    /* level skin: it stays, and spreads a little */
            t->still += dt;
            t->speed = 0;
            piece(t, m, skin, P, R, now, 0);
            if (t->still > 1.5f)
                finish_why(t, m, skin, P, R, now, 0, "level skin");
            return;
        }
        float spent = t->left / t->reach, want = (12.0f + 44.0f * steep) * (0.4f + 0.6f * spent);
        t->speed += (want - t->speed) * (5.0f * dt < 1 ? 5.0f * dt : 1);
        float go = t->speed * dt;
        if (go > t->left)
            go = t->left;
        /* its piece is straight: where it turns from the way the piece began, a new one */
        float A[3], run[3];
        at3(P, t->a_bc, A);
        for (int i = 0; i < 3; i++)
            run[i] = X[i] - A[i];
        float rl = sqrtf(dot3(run, run));
        if (rl > 8.0f && dot3(run, dir) < 0.94f * rl) {
            piece(t, m, skin, P, R, now, 1);
            begin(t, now);
        }
        float Q[3] = {X[0] + dir[0] * go, X[1] + dir[1] * go, X[2] + dir[2] * go}, bq[3];
        if (!among(P, Q, bq)) {
            finish_why(t, m, skin, P, R, now, 0, "it cannot be placed");
            return;
        }
        if (t->edge >= 0)
            bq[t->edge] = t->bc[t->edge];
        float part = 1.0f;
        int out = -1;
        for (int i = 0; i < 3; i++)
            if (bq[i] < 0 && i != t->edge) {
                float p = t->bc[i] / (t->bc[i] - bq[i]);
                if (p < part)
                    part = p, out = i;
            }
        float sum = 0;
        for (int i = 0; i < 3; i++) {
            t->bc[i] += (bq[i] - t->bc[i]) * part;
            t->bc[i] = t->bc[i] < 0 ? 0 : t->bc[i];
            sum += t->bc[i];
        }
        if (out >= 0)
            sum -= t->bc[out], t->bc[out] = 0;
        for (int i = 0; i < 3; i++)
            t->bc[i] /= sum > 0 ? sum : 1;
        t->s += go * part, t->left -= go * part;
        t->idle = go * part > 0.02f ? 0 : t->idle + 1;
        dt -= dt * part;
        if (t->left <= 0.01f || t->idle > 45) {        /* run out (or held where it cannot go on) */
            finish_why(t, m, skin, P, R, now, 0, "run out or held");
            return;
        }
        if (out < 0) {
            piece(t, m, skin, P, R, now, 0);
            return;
        }
        /* at an edge: over it, onto the triangle beyond - unless there is none (the skin's end: it falls), or down
         * on that one is back towards this (a fold: it runs along the edge) */
        int32_t nb = m->nbr[t->tri][out];
        float Pn[3][3], Rn[3][3], nn[3], bn[3];
        at3(P, t->bc, X);
        if (nb < 0) {
            /* (the end of this piece of the skin - a hide is laid in sheets, a part set against another: on to what
             * is just there, below or beside, if anything is) */
            uint32_t on;
            float bo[3];
            if (t->hops < 12 && skin->near && skin->near(skin->ctx, X, t->tri, &on, bo) && on < m->ntri) {
                piece(t, m, skin, P, R, now, 1);
                t->hops++;
                t->tri = on;
                float bs = 0;
                for (int i = 0; i < 3; i++)
                    t->bc[i] = bo[i] < 0.02f ? 0.02f : bo[i], bs += t->bc[i];
                for (int i = 0; i < 3; i++)
                    t->bc[i] /= bs;
                t->edge = -1;
                begin(t, now);
                continue;
            }
            skin->drop(skin->ctx, X, 2.5f + 3.5f * t->width);
            finish_why(t, m, skin, P, R, now, 1, "it fell from the skin's edge");
            return;
        }
        if (!skin->corners(skin->ctx, (uint32_t)nb, Pn, Rn) || !(normal(Pn, m->flip[nb], nn) > 1e-4f) || !among(Pn, X, bn)) {
            finish_why(t, m, skin, P, R, now, 0, "the triangle beyond cannot be had");
            return;
        }
        int back = -1;
        for (int j = 0; j < 3; j++)
            if (m->nbr[nb][j] == (int32_t)t->tri)
                back = j;
        if (back >= 0) {
            const float *e0 = Pn[(back + 1) % 3], *e1 = Pn[(back + 2) % 3];
            float e[3] = {e1[0] - e0[0], e1[1] - e0[1], e1[2] - e0[2]}, el2 = dot3(e, e), in[3], dn[3] = {nn[0] * nn[1], nn[1] * nn[1] - 1.0f, nn[2] * nn[1]};
            for (int i = 0; i < 3; i++)
                in[i] = Pn[back][i] - e0[i];
            float ie = el2 > 0 ? dot3(in, e) / el2 : 0;
            for (int i = 0; i < 3; i++)
                in[i] -= e[i] * ie;
            if (dot3(dn, in) < -0.02f * sqrtf(dot3(in, in)) && nn[1] > -0.85f) {
                t->edge = out;
                continue;
            }
        }
        piece(t, m, skin, P, R, now, 1);
        float bs = 0;
        for (int i = 0; i < 3; i++) {
            t->bc[i] = bn[i] < 1e-4f ? 1e-4f : bn[i];
            bs += t->bc[i];
        }
        for (int i = 0; i < 3; i++)
            t->bc[i] /= bs;
        t->tri = (uint32_t)nb;
        t->edge = -1;
        begin(t, now);
    }
    if (t->live && skin->corners(skin->ctx, t->tri, P, R))
        piece(t, m, skin, P, R, now, 0);
}

static float seconds(double now30) {
    if (epoch < 0) {
        epoch = now30;
    } else if (now30 < last30 - 1 || now30 - epoch > 3.0e6) {           /* (the game's time gone back, a state loaded: all anew) */
        if (trails_told)
            fprintf(stderr, "trails: the time went from %.1f to %.1f: all forgotten\n", last30, now30);
        trails_clear();
        epoch = now30;
    }
    last30 = now30;
    return (float)((now30 - epoch) / 30.0);
}

void trails_start(int mesh, uint32_t tri, const float *bc, float reach, float half_width, const uint8_t *rgb, float seed, double now30) {
    if (mesh < 0 || mesh >= RUN_MESHES || !meshes[mesh].used || tri >= meshes[mesh].ntri)
        return;
    float now = seconds(now30);
    if (!meshes[mesh].used)
        return;
    Trickle *t = NULL;
    for (int k = 0; k < TRICKLES && !t; k++)
        if (!trickles[k].live)
            t = &trickles[k];
    if (!t)
        return;
    memset(t, 0, sizeof *t);
    t->live = 1, t->mesh = mesh, t->tri = tri, t->edge = -1;
    float sum = 0;
    for (int i = 0; i < 3; i++)
        t->bc[i] = bc[i] < 0.02f ? 0.02f : bc[i], sum += t->bc[i];
    for (int i = 0; i < 3; i++)
        t->bc[i] /= sum;
    t->reach = t->left = reach, t->width = half_width, t->seed = seed;
    t->rgb = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16;
    begin(t, now);
}

int trails_step(int mesh, const TrailSkin *skin, double now30) {
    if (mesh < 0 || mesh >= RUN_MESHES || !meshes[mesh].used)
        return 0;
    double before = meshes[mesh].touched;
    float now = seconds(now30);
    if (!meshes[mesh].used)
        return 0;
    float dt = (float)((now30 - before) / 30.0);
    meshes[mesh].touched = now30;
    now_shown = now;
    if (!(dt > 0) || dt > 0.2f)
        dt = dt > 0.2f ? 1.0f / 30 : 0;
    int n = 0;
    for (int k = 0; k < TRICKLES; k++)
        if (trickles[k].live && trickles[k].mesh == mesh) {
            if (dt > 0)
                step(&trickles[k], &meshes[mesh], skin, dt, now);
            n += trickles[k].live;
        }
    return n;
}
