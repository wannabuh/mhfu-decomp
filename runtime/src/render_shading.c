/* Shading made from the geometry (render_internal.h), at flush, from the batch's vertices as put:
 *
 * - the scenery's shading by the sun: it comes lit beforehand, without normals - they are made from its triangles, each
 *   batch's (MHFU_SUN_SHADING=0: none), with relief from its textures' brightness (MHFU_RELIEF=0: none);
 * - the characters' shape normals, for drawing them tessellated (rounder: render.c, ge.tesc, ge.tese);
 * - wind: grass, bushes and leaves sway - the triangles of the scenery whose part of the texture is mostly holes and a
 *   plant's colour, their corners moved by their height above their feet, placed on the screen again (MHFU_SWAY: how
 *   far, 0 none) - and bend away from the feet of those walking through them (render_scene.c). */
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

float sway = -1.0f;
int sun_shading = -1, relief_on = 1;
/* the batch's places in the world and on the screen (the scenery's, for its normals) */
typedef struct { float w[3], sx, sy; } Corner;
static Corner *corners;
static size_t corners_cap;
static GpuVertex *kept;                 /* the batch's vertices as put (the mapped buffer is slow to read) */
static size_t kept_cap;

void shading_settings(void) {
    sun_shading = !getenv("MHFU_SUN_SHADING") || atoi(getenv("MHFU_SUN_SHADING")) != 0;
    sway = getenv("MHFU_SWAY") ? (float)atof(getenv("MHFU_SWAY")) : 1.0f;
    relief_on = !getenv("MHFU_RELIEF") || atoi(getenv("MHFU_RELIEF")) != 0;
}

/* The batch's vertex i as put, kept (swaying or tessellated: written at flush) */
void shading_keep(size_t i, const GpuVertex *g) {
    if (i >= kept_cap) {
        kept_cap = kept_cap ? kept_cap * 2 : 65536;
        kept = realloc(kept, kept_cap * sizeof *kept);
    }
    kept[i] = *g;
}

const GpuVertex *shading_kept(void) {
    return kept;
}

/* The scenery's vertex i: its place in the world and on the screen (PSP pixels) */
void shading_corner(size_t i, const float *w, float sx, float sy) {
    if (i >= corners_cap) {
        corners_cap = corners_cap ? corners_cap * 2 : 65536;
        corners = realloc(corners, corners_cap * sizeof *corners);
    }
    corners[i] = (Corner){{w[0], w[1], w[2]}, sx, sy};
}

/* The batch's normals (the scenery's): each triangle's, turned towards the camera, summed where corners meet (the same
 * place in the world), each corner taking the sum unless it bends more than 60 degrees from its own (an edge) */
void make_normals(size_t n) {
    static uint32_t *table, *owner;     /* a place's first corner + 1 (0: empty); each corner's first at its place */
    static float (*sum)[3], (*face)[3];
    static size_t table_cap, sum_cap;
    if (n > sum_cap) {
        sum_cap = n * 2;
        sum = realloc(sum, sum_cap * sizeof *sum);
        face = realloc(face, sum_cap * sizeof *face);
        owner = realloc(owner, sum_cap * sizeof *owner);
    }
    size_t tsize = 1024;
    while (tsize < n * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
    }
    memset(table, 0, tsize * sizeof *table);
    for (size_t t = 0; t + 2 < n; t += 3) {
        const Corner *a = &corners[t], *b = &corners[t + 1], *c = &corners[t + 2];
        float e1[3] = {b->w[0] - a->w[0], b->w[1] - a->w[1], b->w[2] - a->w[2]};
        float e2[3] = {c->w[0] - a->w[0], c->w[1] - a->w[1], c->w[2] - a->w[2]};
        float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float area = (b->sx - a->sx) * (c->sy - a->sy) - (b->sy - a->sy) * (c->sx - a->sx);
        if (area > 0)                   /* wound the other way on the screen: facing away as wound */
            f[0] = -f[0], f[1] = -f[1], f[2] = -f[2];
        for (int k = 0; k < 3; k++)
            memcpy(face[t + (size_t)k], f, sizeof f);
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t h[3];
        memcpy(h, corners[i].w, sizeof h);
        uint32_t x = (h[0] * 0x9E3779B1u ^ h[1] * 0x85EBCA77u ^ h[2] * 0xC2B2AE3Du);
        x ^= x >> 15;
        size_t j = x & (tsize - 1);
        for (;;) {
            uint32_t e = table[j];
            if (!e) {
                table[j] = (uint32_t)i + 1;
                owner[i] = (uint32_t)i;
                memcpy(sum[i], face[i], sizeof sum[i]);
                break;
            }
            if (!memcmp(corners[e - 1].w, corners[i].w, sizeof h)) {
                for (int k = 0; k < 3; k++)
                    sum[e - 1][k] += face[i][k];
                owner[i] = e - 1;
                break;
            }
            j = (j + 1) & (tsize - 1);
        }
    }
    GpuVertex *g = (GpuVertex *)((uint8_t *)vertices.map + batch_start);
    for (size_t i = 0; i < n; i++) {
        const float *s = sum[owner[i]], *f = face[i];
        float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]), fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (!(fl > 0))
            continue;
        const float *use = sl > 0 && (s[0] * f[0] + s[1] * f[1] + s[2] * f[2]) > 0.5f * sl * fl ? s : f;
        float k = 32767.0f / (use == s ? sl : fl);
        int16_t nn[4] = {(int16_t)(use[0] * k), (int16_t)(use[1] * k), (int16_t)(use[2] * k), 0};
        memcpy(g[i].n, nn, sizeof nn);
    }
}

/* A tessellated batch's shape normals (what curves it, ge.tese): at each place the average of its corners' normals -
 * corners at one place, with normals split for lighting (hard edges, seams), curve alike, and no cracks open between
 * their triangles. Packed into the normal's fourth component (8 + 8 bits, octahedral; 0: none). And its edges that
 * no other of its triangles shares (where another draw goes on: what it curves is not known here) stay straight:
 * in the sun's fourth byte, bit k for the edge facing corner k */
void shape_normals(size_t n) {
    static uint32_t *table, *owner;
    static float (*pos)[3], (*sum)[3];
    static int16_t *packed;
    static struct { uint64_t key; uint32_t uses; } *edges;
    static size_t cap, table_cap;
    if (n > cap) {
        cap = n * 2;
        owner = realloc(owner, cap * sizeof *owner);
        pos = realloc(pos, cap * sizeof *pos);
        sum = realloc(sum, cap * sizeof *sum);
        packed = realloc(packed, cap * sizeof *packed);
    }
    size_t tsize = 1024;
    while (tsize < n * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
        edges = realloc(edges, table_cap * 2 * sizeof *edges);
    }
    memset(table, 0, tsize * sizeof *table);
    GpuVertex *g = kept, *out = (GpuVertex *)((uint8_t *)vertices.map + batch_start);
    /* the places (welded): each corner's first at its place, the normals summed there */
    uint32_t places = 0;
    for (size_t i = 0; i < n; i++) {
        const float *w = &g[i].wx;
        uint32_t hb[3];
        memcpy(hb, w, sizeof hb);
        uint32_t x = (hb[0] * 0x9E3779B1u ^ hb[1] * 0x85EBCA77u ^ hb[2] * 0xC2B2AE3Du);
        x ^= x >> 15;
        size_t j = x & (tsize - 1);
        for (;;) {
            uint32_t e = table[j];
            if (!e) {
                table[j] = ++places;
                memcpy(pos[places - 1], w, sizeof pos[0]);
                sum[places - 1][0] = sum[places - 1][1] = sum[places - 1][2] = 0;
                owner[i] = places - 1;
                break;
            }
            if (!memcmp(pos[e - 1], w, sizeof pos[0])) {
                owner[i] = e - 1;
                break;
            }
            j = (j + 1) & (tsize - 1);
        }
        float *a = sum[owner[i]];
        a[0] += g[i].n[0];
        a[1] += g[i].n[1];
        a[2] += g[i].n[2];
    }
    for (uint32_t p = 0; p < places; p++) {    /* octahedral: onto |x| + |y| + |z| = 1, the lower half folded up */
        const float *a = sum[p];
        float l = fabsf(a[0]) + fabsf(a[1]) + fabsf(a[2]);
        packed[p] = 0;
        if (l > 1e-3f) {
            float ox = a[0] / l, oy = a[1] / l;
            if (a[2] < 0) {
                float tx = (1 - fabsf(oy)) * (ox >= 0 ? 1 : -1), ty = (1 - fabsf(ox)) * (oy >= 0 ? 1 : -1);
                ox = tx;
                oy = ty;
            }
            int raw = (int)lrintf(ox * 127) * 256 + (int)lrintf(oy * 127) + 127;
            packed[p] = (int16_t)(raw ? raw : 1);
        }
    }
    /* the edges, by their two places: counted, then those of one triangle only left straight */
    size_t esize = tsize * 2;
    memset(edges, 0, esize * sizeof *edges);
    size_t ntri = n / 3;
    for (int pass = 0; pass < 2; pass++)
        for (size_t t = 0; t < ntri; t++) {
            const uint32_t *o = &owner[3 * t];
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
                for (int k = 0; k < 3; k++) {
                    GpuVertex v = g[3 * t + (size_t)k];
                    v.n[3] = packed[o[k]];
                    v.sun = (v.sun & 0xFFFFFFu) | open << 24;
                    out[3 * t + (size_t)k] = v;
                }
        }
    for (size_t i = ntri * 3; i < n; i++)
        out[i] = g[i];
}

/* How large the batch (its CPU copy) is on the screen, in PSP pixels (the larger way; 0 if any of it is behind) */
float on_screen_size(size_t n) {
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (size_t i = 0; i < n; i++) {
        const GpuVertex *v = &kept[i];
        if (v->w <= 0)
            return 0;
        float x = v->x / v->w, y = v->y / v->w;
        x0 = fminf(x0, x);
        x1 = fmaxf(x1, x);
        y0 = fminf(y0, y);
        y1 = fmaxf(y1, y);
    }
    return fmaxf(x1 - x0, y1 - y0) * (RT_SIZE / 2.0f);
}

/* The batch's n vertices (triangles) moved by the wind */
void sway_batch(const DrawState *d, size_t n) {
    float m[16];
    if (!world_to_screen(prim_view, prim_world_view, m))
        return;
    double now = scene_time();
    float t = (float)fmod(now, 3600.0), amp = 7.0f * sway;
    const float wind[3] = {0.8f, 0.0f, 0.6f};
    GpuVertex *g = kept, *out = (GpuVertex *)((uint8_t *)vertices.map + batch_start);
    const uint8_t *holes = tex_holes[d->tex_slot];
    float tw = d->tex_w > 0 ? d->tex_w : 1, th = d->tex_h > 0 ? d->tex_h : 1;
    /* the corners at one place (welded: a mesh's triangles share them) move as one: by their height above the lowest
     * foot of their triangles; held where a triangle that is not a plant's meets them (a branch on its trunk) */
    static uint32_t *table, *owner;
    static float *foot;
    static uint8_t *held;
    static size_t cap, table_cap;
    if (n > cap) {
        cap = n * 2;
        owner = realloc(owner, cap * sizeof *owner);
        foot = realloc(foot, cap * sizeof *foot);
        held = realloc(held, cap);
    }
    size_t tsize = 1024;
    while (tsize < n * 2)
        tsize *= 2;
    if (tsize > table_cap) {
        table_cap = tsize;
        table = realloc(table, table_cap * sizeof *table);
    }
    memset(table, 0, tsize * sizeof *table);
    int any = 0;
    for (size_t tri = 0; tri + 3 <= n; tri += 3) {
        /* a plant's: the part of the texture it shows is mostly holes (between blades, leaves) and a plant's colour -
         * not the bark, rope, wood, paper or cloth sharing the texture with them */
        float cu = (g[tri].u + g[tri + 1].u + g[tri + 2].u) / 3, cv = (g[tri].v + g[tri + 1].v + g[tri + 2].v) / 3;
        int share = 0;
        for (int k = -1; k < 3; k++) {
            float u = k < 0 ? cu : 0.6f * g[tri + k].u + 0.4f * cu, v = k < 0 ? cv : 0.6f * g[tri + k].v + 0.4f * cv;
            u /= tw;
            v /= th;
            u -= floorf(u);
            v -= floorf(v);
            int cx = (int)(u * HOLE_GRID), cy = (int)(v * HOLE_GRID);
            share += holes[(cy < HOLE_GRID ? cy : HOLE_GRID - 1) * HOLE_GRID + (cx < HOLE_GRID ? cx : HOLE_GRID - 1)];
        }
        int plant = share >= 4 * 70;    /* (more than about a quarter holes) */
        any |= plant;
        float f = fminf(g[tri].wy, fminf(g[tri + 1].wy, g[tri + 2].wy));
        for (size_t i = tri; i < tri + 3; i++) {
            uint32_t hb[3];
            memcpy(hb, &g[i].wx, sizeof hb);
            uint32_t x = (hb[0] * 0x9E3779B1u ^ hb[1] * 0x85EBCA77u ^ hb[2] * 0xC2B2AE3Du);
            x ^= x >> 15;
            size_t j = x & (tsize - 1);
            for (;;) {
                uint32_t e = table[j];
                if (!e) {
                    table[j] = (uint32_t)i + 1;
                    owner[i] = (uint32_t)i;
                    foot[i] = f;
                    held[i] = !plant;
                    break;
                }
                if (!memcmp(&g[e - 1].wx, &g[i].wx, sizeof hb)) {
                    owner[i] = e - 1;
                    foot[e - 1] = fminf(foot[e - 1], f);
                    held[e - 1] |= !plant;
                    break;
                }
                j = (j + 1) & (tsize - 1);
            }
        }
    }
    if (!any)
        return;
    /* the feet near the batch at all */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < n; i++) {
        const float *w = &g[i].wx;
        for (int k = 0; k < 3; k++) {
            lo[k] = fminf(lo[k], w[k]);
            hi[k] = fmaxf(hi[k], w[k]);
        }
    }
    const Foot *near[MAX_FEET];
    int nnear = 0;
    for (int f = 0; f < scene.nfeet; f++) {
        const Foot *ft = &scene.feet[f];
        float reach = ft->r + 45.0f;
        if (ft->x + reach > lo[0] && ft->x - reach < hi[0] && ft->z + reach > lo[2] && ft->z - reach < hi[2] &&
            ft->y + 60.0f > lo[1] && ft->y - 60.0f < hi[1])
            near[nnear++] = ft;
    }
    for (size_t i = 0; i < n - n % 3; i++) {
        GpuVertex *v = &g[i];
        uint32_t o = owner[i];
        float h = v->wy - foot[o];
        if (held[o] || h <= 0.5f || v->w <= 0)
            continue;
        float wgt = fminf(fmaxf(h - 10.0f, 0.0f) / 80.0f, 1.0f);      /* (small cards hardly) */
        wgt *= sqrtf(wgt);
        float ph = v->wx * 0.0041f + v->wz * 0.0033f;
        float gust = 0.55f + 0.45f * sinf(t * 0.37f + v->wx * 0.0006f - v->wz * 0.0004f);
        float sw = 0.7f * sinf(t * 1.9f + ph * 6.2832f) + 0.3f * sinf(t * 3.7f + ph * 11.0f);
        float d = (sw + 0.4f) * gust * amp * wgt;
        float dw[3] = {wind[0] * d, -0.1f * fabsf(d), wind[2] * d};
        /* bent away from feet among them: out and down, the more the nearer (their foot - not the leaves up a tree) */
        for (int f = 0; f < nnear; f++) {
            const Foot *ft = near[f];
            float dx = v->wx - ft->x, dz = v->wz - ft->z, reach = ft->r + 45.0f;
            if (fabsf(dx) > reach || fabsf(dz) > reach || fabsf(foot[o] - ft->y) > 60.0f)
                continue;
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist >= reach || dist < 1e-3f)
                continue;
            float push = (1.0f - dist / reach) * fminf(h, 60.0f) * 0.6f;
            dw[0] += dx / dist * push;
            dw[2] += dz / dist * push;
            dw[1] -= push * 0.5f;
        }
        float p[4], q[4];
        for (int r = 0; r < 4; r++) {
            p[r] = m[r] * v->wx + m[4 + r] * v->wy + m[8 + r] * v->wz + m[12 + r];
            q[r] = p[r] + m[r] * dw[0] + m[4 + r] * dw[1] + m[8 + r] * dw[2];
        }
        if (p[3] <= 0 || q[3] <= 0)
            continue;
        float w = v->w * q[3] / p[3], z = q[2] / q[3];
        GpuVertex moved = *v;
        moved.x = (q[0] / q[3] / (RT_SIZE / 2.0f) - 1.0f) * w;
        moved.y = (q[1] / q[3] / (RT_SIZE / 2.0f) - 1.0f) * w;
        moved.z = (z < 0 ? 0 : z > 1 ? 1 : z) * w;
        moved.w = w;
        moved.wx += dw[0];              /* (its shadow sways too) */
        moved.wy += dw[1];
        moved.wz += dw[2];
        out[i] = moved;
    }
}
