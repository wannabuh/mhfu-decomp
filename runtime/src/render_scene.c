/* What is known of the world being drawn (render_internal.h's Scene), gathered from the GE's draws as they come, for the
 * effects: the camera, the sun, the fog, the feet of those walking in it, the moment shown. */
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

Scene scene = {.sun_rgb = {1, 1, 1}, .depth_op = VK_COMPARE_OP_GREATER_OR_EQUAL};

static double clock_s = -1.0;           /* the moment shown, in seconds (gpu.c), or -1: the game's frame */
/* the feet being found this frame (scene.feet: the last's, whole), and the moment shown they were found at */
static Foot found[MAX_FEET];
static int nfound;
static double found_moment = -1, feet_moment = -1;
static float (*lit_pos)[3];             /* the batch's places, if it is lit (its feet are found at flush) */
static size_t lit_cap;

double rt_game_30ths(void);

/* gpu.c: the moment the frame shows, in seconds (-1: the game's own frame, drawn as it runs) */
void render_time(double seconds) {
    clock_s = seconds;
}

double scene_time(void) {
    return clock_s >= 0 ? clock_s : rt_game_30ths() / 30.0;
}

void scene_lit_vertex(size_t i, const float *w) {
    if (i >= lit_cap) {
        lit_cap = lit_cap ? lit_cap * 2 : 65536;
        lit_pos = realloc(lit_pos, lit_cap * sizeof *lit_pos);
    }
    lit_pos[i][0] = w[0];
    lit_pos[i][1] = w[1];
    lit_pos[i][2] = w[2];
}

/* A lit batch's feet (if it has any near the ground): the middle and spread of its lowest corners */
void scene_lit_batch(size_t n) {
    float lo = 1e30f;
    for (size_t i = 0; i < n; i++)
        lo = fminf(lo, lit_pos[i][1]);
    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (size_t i = 0; i < n; i++)
        if (lit_pos[i][1] < lo + 25.0f) {
            x0 = fminf(x0, lit_pos[i][0]);
            x1 = fmaxf(x1, lit_pos[i][0]);
            z0 = fminf(z0, lit_pos[i][2]);
            z1 = fmaxf(z1, lit_pos[i][2]);
        }
    float x = 0.5f * (x0 + x1), z = 0.5f * (z0 + z1), r = 0.5f * fmaxf(x1 - x0, z1 - z0);
    if (!(r <= 250.0f))                 /* (spread too wide to be feet: not something walking) */
        return;
    Foot *f = found;
    for (int i = 0; i < nfound; i++)  /* (one foot drawn in pieces: one) */
        if (fabsf(f[i].x - x) < 10.0f && fabsf(f[i].z - z) < 10.0f && fabsf(f[i].y - lo) < 10.0f)
            return;
    if (nfound < MAX_FEET) {
        if (!nfound)
            found_moment = scene_time();
        f[nfound++] = (Foot){x, lo, z, r, 0, 0};
    }
}

/* The frame's feet, whole: each one's velocity from the nearest of the last frame's (smoothed; the same moment drawn
 * again keeps it). A frame that drew none - the HUD's alone - keeps the last's */
void scene_end_frame(void) {
    if (!nfound)
        return;
    float dt = feet_moment >= 0 ? (float)(found_moment - feet_moment) : 0;
    for (int i = 0; i < nfound; i++) {
        Foot *f = &found[i];
        const Foot *best = NULL;
        float bd = 40.0f * 40.0f;
        for (int j = 0; j < scene.nfeet; j++) {
            float dx = f->x - scene.feet[j].x, dz = f->z - scene.feet[j].z, d = dx * dx + dz * dz;
            if (d < bd && fabsf(f->y - scene.feet[j].y) < 30.0f)
                best = &scene.feet[j], bd = d;
        }
        if (best && dt > 1e-4f && dt < 0.2f) {
            f->vx = 0.6f * best->vx + 0.4f * (f->x - best->x) / dt;
            f->vz = 0.6f * best->vz + 0.4f * (f->z - best->z) / dt;
        } else if (best) {
            f->vx = best->vx;
            f->vz = best->vz;
        }
    }
    feet_moment = found_moment;
    memcpy(scene.feet, found, sizeof found);
    scene.nfeet = nfound;
    nfound = 0;
}
