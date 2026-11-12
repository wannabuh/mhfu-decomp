/* Blood running down an animal (trails.c; gpu.c gives it the animal's skin as posed, and draws what it leaves): the
 * skin's triangles and which lies across each one's edges. */
#pragma once
#include <stdint.h>

#include "shared.h"

/* What a skin came to: its triangles, the places its vertices are at (welded), its edges with no triangle across them
 * and with one, those more than two triangles meet at, its triangles with no area, and how many pieces it is in (the
 * largest's triangles) */
typedef struct { uint32_t ntri, places, open, inner, many, flat, islands, largest; } TrailStats;

/* The ntri triangles (tri: three of the nv vertices each) of a skin whose vertices are at pos: nbr[t][k] the triangle
 * across t's edge opposite its corner k, or -1 - vertices within weld of each other being one place (a seam in the
 * picture, a join between two pieces of the model, a vertex given twice) */
void trail_topology(const float (*pos)[3], uint32_t nv, const uint32_t *tri, uint32_t ntri, float weld, int32_t (*nbr)[3],
                    TrailStats *stats);
/* Which of those triangles are in a piece of the skin of at least so many (big[t]: 1), and which piece each is in
 * (which[t]: a number the piece's triangles share; or NULL) */
void trail_pieces(int32_t (*nbr)[3], uint32_t ntri, uint32_t least, uint8_t *big, uint32_t *which);

/* -- the blood itself ---------------------------------------------------------------------------------------------
 * A trickle starts on a triangle of an animal's skin and runs down it, from triangle to triangle - which way is down
 * read again each step from how the animal stands - slower on skin that slopes less, staying where the skin is level,
 * going on from the end of one piece of the skin to another lying there, falling off where the skin turns under or ends. What it has run over is left on the skin: straight pieces, each kept
 * with the triangle it lies on (and those beside it that it reaches into) where it is among that triangle's vertices
 * as the game gave them, so it stays on the skin however the animal moves; the pixel stage draws a triangle's pieces
 * (ge_frag.glsl, shared.h RUN_). They stay as long as the animal's skin is kept, the oldest gone first when there is
 * no room. */

/* The skin as it is now (gpu.c): triangle tri's corners where they are in the world and as the game gave them - 0 if
 * it is not drawn now; and a drop leaving the skin at a place, so big */
typedef struct {
    void *ctx;
    int (*corners)(void *ctx, uint32_t tri, float posed[3][3], float rest[3][3]);
    void (*drop)(void *ctx, const float *at, float size);
    /* at an edge of triangle from with nothing across it: the triangle of another piece of the skin that is just
     * there, not above - which, and where on it; 0 if there is none (or NULL: never) */
    int (*near)(void *ctx, const float *at, uint32_t from, uint32_t *tri, float *bc);
    /* what a trickle has just run over, if its trail is kept by the skin's keeper and not here in pieces (bloodmap.c;
     * or NULL): from a to b, half so wide, on these triangles - its own and those across its edges */
    void (*stroke)(void *ctx, const uint32_t *tris, int ntris, const float *a, const float *b, float half);
} TrailSkin;

/* An animal's skin kept (owner: what tells it from another's): which of the RUN_MESHES it is, or -1; made - its
 * triangles' neighbours (trail_topology's), whether each is wound the other way round, and which triangles lie over
 * each (fur drawn over the skin: over_at[t] to over_at[t + 1] of over; or none) - all kept, to be freed here - in place
 * of the one longest unused; forgotten with its blood; all forgotten */
int trails_mesh(uint64_t owner);
int trails_mesh_make(uint64_t owner, uint32_t ntri, int32_t (*nbr)[3], uint8_t *flip, uint32_t *over_at, uint32_t *over);
void trails_mesh_drop(int mesh);
void trails_clear(void);
/* A trickle from triangle tri of it, at bc among its corners: how far it can run and half how wide it is (units of
 * the world), its colour, a number of its own (0 to 1); now, in 30ths of the game's second */
void trails_start(int mesh, uint32_t tri, const float *bc, float reach, float half_width, const uint8_t *rgb, float seed, double now30);
/* A step of the game's: its trickles moved. How many are still running */
int trails_step(int mesh, const TrailSkin *skin, double now30);
/* Whether trails are kept in pieces here (as they are unless told): not, when the skins' strokes keep them */
void trails_pieces(int on);

/* The renderer's thread: the words for the pixel stage (shared.h RUN_: all of them), and which of them changed since
 * this was last asked (from, to: none if equal); the moment the blood is at, in the seconds its pieces are timed in */
const uint32_t *trails_words(uint32_t *from, uint32_t *to);
float trails_now(void);
extern int trails_told;            /* (MHFU_TRAIL_LOG: what each trickle does; 2: every step) */
