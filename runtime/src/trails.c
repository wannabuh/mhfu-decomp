/* Blood running down an animal: see trails.h. */
#include "trails.h"
#include <math.h>
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
