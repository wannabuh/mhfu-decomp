/* The click inspector (inspect.c, F7). */
#pragma once
#include <SDL3/SDL.h>
#include <stdio.h>

#include "glyphs.h"

int inspect_open(void);
void inspect_toggle(void);
void inspect_toggle_pause(void);
void inspect_step(void);                /* held: forward a frame shown, or one tick on (the . key) */
void inspect_free_camera(void);         /* held: the free camera on or off (the C key) */
void inspect_free_hud(void);            /* its HUD (the H key) */
void inspect_back(void);                /* held: back a frame shown (the , key; MHFU_FRAME_HISTORY) */
int inspect_held(void);                 /* held now, with no side effects */
int inspect_paused(void);               /* present.c: hold the game, as the controls screen does */
int inspect_event(const SDL_Event *e);  /* window.c: 1 if taken */
void inspect_tick(unsigned long frame); /* present.c, each game frame: new pick data asked for, test hooks */
int inspect_rects(float x0, float y0, float scale, HintRect *out, int max);
void inspect_write(FILE *f);            /* the selection as text (inspect.txt, a capture's info.txt) */

/* gpu.c: what the frame shown drew, kept while the inspector is open */
typedef struct {
    uint32_t hash, addr, vaddr, blend_reg;
    int textured, w, h, fmt, through, blend, alpha_test, depth_test, type, count, draw;
    float z;
} GpuPickHit;
void gpu_pick_enable(int on);
void gpu_pick_request(void);            /* recorded frames: the next frame drawn is gone over */
void gpu_pick_frame(uint32_t display_addr);   /* present.c, each game frame */
unsigned gpu_pick_generation(void);     /* changes with new data */
int gpu_pick_at(float x, float y, GpuPickHit *out, int max);   /* top first; how many */
int gpu_pick_project(const float *world, float *x, float *y);  /* a point in the world, on the screen as shown (PSP pixels) */
