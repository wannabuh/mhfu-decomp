/* Blood off the animal (blood.c): the drops in the air and the ground's blood, a painted map of it */
#ifndef BLOOD_H
#define BLOOD_H

#include <stdint.h>

#include "shared.h"

/* The game's thread (gpu.c): a drop leaves a wound - where, how fast, how high the ground is taken to be if nothing
 * better is known, how big, its colour; the area is left; where the eye is */
void blood_spawn(const float *at, const float *speed, float floor_y, float size, const uint8_t *rgb, float fall);   /* (fall: times gravity) */
void blood_clear(void);
void blood_eye(const float *eye);

/* The renderer's thread: the map's copy on the GPU (BLOOD_N * BLOOD_N uint32s, zeroed), and each frame (now: the
 * moment shown, seconds of the game's) what the pixel stage needs besides - the drops in the air (where, w how wide;
 * colour; streak), the map's colours, the eye, whether there is any blood */
typedef struct {
    int ndrops, any;
    float drop_at[4 * BLOOD_DROP_SETS][4], drop_color[4 * BLOOD_DROP_SETS][4], drop_streak[4 * BLOOD_DROP_SETS][4];
    float palette[16][3], eye[3];
} BloodFrame;
void blood_init(uint32_t *gpu_map);
void blood_step(double now, BloodFrame *out);

#endif
