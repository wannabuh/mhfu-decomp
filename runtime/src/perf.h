/* The performance panel (perf.c, F5): what is measured, by whom. Nothing here touches the game's state: host clocks and
 * host counters only (never time_read_us, which moves the game's clock). */
#pragma once
#include <stdint.h>
#include <time.h>

#include "glyphs.h"

extern int perf_on;                     /* the panel is on: the timers below run. Off, each is a branch not taken */

static inline uint64_t perf_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Added to while perf_on, by the thread doing the work (the render thread's and the fence's atomically) */
extern uint64_t perf_ge_ns, perf_draw_ns, perf_flip_ns, perf_present_ns, perf_sync_ns;
extern _Atomic uint64_t perf_render_ns, perf_fence_ns;
extern unsigned long perf_draws, perf_tris;
extern unsigned long perf_skipped;      /* frames left out, the game late: always counted */

void perf_toggle(void);
void perf_game_frame(unsigned long frame);   /* present.c: a game frame done (always: the frame times' ring) */
void perf_shown(void);                  /* present.c: a frame shown (always) */
int perf_rects(float x0, float y0, float scale, HintRect *out, int max);     /* render.c: the panel, if on */
void perf_write_times(void *file, double seconds);   /* capture.c: the last seconds' frame times, as text (a FILE *) */
void perf_summary(double *fps, double *frame_ms, double *worst_ms, double *tick_rate);    /* over the last second, the last 10 */
