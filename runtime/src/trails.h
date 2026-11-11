/* Blood running down an animal (trails.c; gpu.c gives it the animal's skin as posed, and draws what it leaves): the
 * skin's triangles and which lies across each one's edges. */
#pragma once
#include <stdint.h>

/* What a skin came to: its triangles, the places its vertices are at (welded), its edges with no triangle across them
 * and with one, those more than two triangles meet at, its triangles with no area, and how many pieces it is in (the
 * largest's triangles) */
typedef struct { uint32_t ntri, places, open, inner, many, flat, islands, largest; } TrailStats;

/* The ntri triangles (tri: three of the nv vertices each) of a skin whose vertices are at pos: nbr[t][k] the triangle
 * across t's edge opposite its corner k, or -1 - vertices within weld of each other being one place (a seam in the
 * picture, a join between two pieces of the model, a vertex given twice) */
void trail_topology(const float (*pos)[3], uint32_t nv, const uint32_t *tri, uint32_t ntri, float weld, int32_t (*nbr)[3],
                    TrailStats *stats);
