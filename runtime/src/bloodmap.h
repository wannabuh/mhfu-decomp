/* Blood kept on an animal's skin texel by texel (bloodmap.c; gpu.c gives it the skin and the hits, ge_frag.glsl draws
 * it). The game's own pictures cannot hold it: a model's picture is laid over both its sides and more, a texel 6 to 8
 * units across (a Tigrex: MHFU_UV_DUMP). So each skin kept (trails.h's meshes, RUN_MESHES of them) has a map of its
 * own, BLOODMAP_W x BLOODMAP_H texels, in which every triangle has a place to itself - laid out here, a unit a
 * texel if they fit - and for each triangle what takes a place among its vertices as the game gave them to its place
 * in the map (shared.h RUN_MAPS_AT: two rows). Blood is painted into it where the skin is in the world when it is
 * hit, each texel by its own place there, so what is painted goes on over the triangles' edges though they lie
 * apart in the map. */
#pragma once
#include <stdint.h>

#include "shared.h"
#include "trails.h"

/* A skin's map laid out anew, empty: its ntri triangles as skin gives them now (those not drawn get no place); its
 * blood's colour. How many units of the world a texel came to */
float bloodmap_layout(int mesh, uint32_t ntri, const TrailSkin *skin, const uint8_t *rgb);
void bloodmap_drop(int mesh);
/* (MHFU_BLOODMAP_SHOW: every texel of a map just laid out painted in squares, to look at the layout) */

/* Blood painted where the skin is now. A splash: about at, on skin facing out (unit; skin turned from it gets none,
 * and what lies behind it less the further), so far across, as much (0 to 1; it adds to what is there); long: drawn
 * out that way (a unit vector, or zeros) by so many times. seed: a number of its own */
typedef struct { float at[3], out[3], along[3], radius, much, lengthen, seed; int kind; } BloodSplash;
/* (a cut: the wound itself, radius half its width and lengthen its length over that, much how deep - and a little
 * blood on its lips; a shot's hole; a bruise) */
enum { SPLASH_BLOT, SPLASH_CUT, SPLASH_HOLE, SPLASH_BRUISE };
/* (flip: whether each triangle is wound the other way round, or NULL; now: the game's time in seconds) */
void bloodmap_splash(int mesh, const TrailSkin *skin, const BloodSplash *s, const uint8_t *flip, float now);
/* A stroke from a to b (a trickle's step), half so wide, on triangle tri and those given beside it */
void bloodmap_stroke(int mesh, const TrailSkin *skin, const uint32_t *tris, int ntris, const float *a, const float *b, float half, float much, float now);

/* Each frame of the game's (now: its time in seconds): old blood marked dry */
void bloodmap_frame(float now);

/* The renderer's thread: all the words from RUN_MAPS_AT on (the maps, then the texels); and those changed since this
 * was last asked copied to where the renderer keeps them (to: its copy of the first) - how many words */
const uint32_t *bloodmap_store(void);
uint32_t bloodmap_sync(uint32_t *to);
/* the moment it is, as the texels have when they last got wet (1 to 256) */
float bloodmap_now(void);
int bloodmap_changed(void);            /* (whether any have: not locked, to ask often) */
float bloodmap_unit(int mesh);
