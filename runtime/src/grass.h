/* Ground grass (grass.c): the ground laid down from the scenery the renderer draws, the blades made over it;
 * render_grass.c keeps them in a pool on the GPU and draws them (grass.vert, grass.frag). */
#ifndef GRASS_H
#define GRASS_H

#include <stdint.h>

/* The field's density, by the distance d: full to grass_full(), then as the square of it over d. A blade comes in as
 * that falls to its place in its tile's order, growing from nothing over GRASS_BAND times the distance it is full at
 * (grass.vert); the field shrinks away over the last GRASS_EDGE of the reach. A blade new to a tile grows in over
 * GRASS_GROW seconds */
#define GRASS_BAND 1.35f
#define GRASS_EDGE 0.25f
#define GRASS_GROW 0.5f

/* A blade (grass.vert's instance): its root in the world; its colour (RGB); height (quarter units), half width
 * (64ths), yaw (256ths of a turn), its place in its tile's order (255ths: the distance thins by it); the ground's
 * normal x, z (snorm8) and its wind phase (256ths); when it was first made (extra: a float, the game's seconds
 * modulo 3600, as grass.vert's time) */
typedef struct {
    float x, y, z;
    uint32_t color, shape, normal, extra, pad;
} GrassBlade;

typedef struct {
    int slot, count;                    /* the first count of the slot's blades */
    float box[6];                       /* its bounds: x, y, z low, then high */
} GrassDraw;

/* density: blades a cell (1: 8 tried, the tufts keeping some), radius: the reach, full: the full density's */
void grass_settings(float density, float radius, float full);
float grass_radius(void);
float grass_full(void);
/* The blade pool: near slots of grass_slot_blades(), then far ones of an eighth of that (the first of a tile's: all
 * the distance draws there), as many as the reach needs (known from grass_settings); its size in blades, how many
 * slots, and a slot's first blade */
uint32_t grass_slot_blades(void);
uint32_t grass_pool_blades(void);
int grass_slots(void);
uint32_t grass_slot_base(int slot);
void grass_frame(const float camera[3], double now);    /* (now: the moment shown, seconds of the game's) */
/* A triangle of the scenery: its corners in the world, texture coordinates (0..1 a repeat), vertex colours (0-255);
 * its texture (a small copy, w x h ABGR8888; NULL: none), doubled (the GE's colour doubling) */
void grass_ground(const float p[3][3], const float uv[3][2], const float col[3][3], const uint32_t *tex, int w, int h,
                  int doubled);
/* A tile's blades to upload, if one is ready (one a call): 1 */
int grass_build(int *slot, const GrassBlade **blades, int *count);
int grass_tiles(GrassDraw *out, int max);

/* The trodden grass: a window of GRASS_TRAIL x GRASS_TRAIL cells about the feet near their ground (or the camera) (world-aligned, each GRASS_TRAIL_CELL
 * units; cell x, z at (x & (GRASS_TRAIL - 1)) + (z & (GRASS_TRAIL - 1)) GRASS_TRAIL), each the way and how far its
 * grass is pressed down (snorm16 x, z). Pressed by the feet (x, y, z, radius, velocity x, z: GRASS_FOOT floats each)
 * near their ground, easing in; springing back over a second or so */
#define GRASS_TRAIL 256
#define GRASS_TRAIL_CELL 6.0f
#define GRASS_FOOT 6
void grass_trail(const float *feet, int nfeet, double now, uint32_t *out, int *origin_x, int *origin_z);

#endif
