/* The performance panel (F5; MHFU_TEST_PERF=1 turns it on from the start, for a dump to show it): frames shown and
 * their times, the game's tick rate, where the time goes, draws, and how full the game's memory is. Drawn over the
 * picture as the controls screen is (glyphs.c, render.c), so nothing of it reaches a MHFU_DUMP's frames, only what is
 * presented (MHFU_DUMP_PRESENT, a capture's screenshot).
 *
 * What is measured, on the wall clock, over the last second:
 *   FPS, frame times  between frames handed to the window (present.c), whatever the swapchain then does with them
 *   LOGIC             the game's thread, what is left when the rest below is taken from the wall clock: game code and
 *                     HLE, and anything unaccounted for. Not measured directly: the game's code has no timers in it
 *   GE                the game's thread walking display lists: the vertex stage, recording; less its waits for the
 *                     render thread (RT WAIT)
 *   DRAW              the game's thread replaying a recorded frame into the render queue (MHFU_FPS only)
 *   FLIP              render_flip on the game's thread, less RT WAIT: ending the frame's passes, submitting, the
 *                     GPU WAIT and ACQ+PRES within it
 *   RT WAIT           the game's thread waiting for the render thread to empty its queue
 *   GPU WAIT          vkWaitForFences for the frame before, on whichever thread submits (so not all of it inside FLIP)
 *   ACQ+PRES          vkAcquireNextImage and vkQueuePresent, where a full swapchain's wait shows up
 *   RENDER THREAD     running its queue's commands: clipping, batches, textures, Vulkan calls
 *   IDLE              the game's thread asleep until the game's clock is due (thread.c)
 *   DISPLAY...        the driver's reports of when frames really went out, over a second (pacing.c; not on every driver):
 *                     SHOWN frames that reached the screen, REPEATS refreshes that showed a frame again, REPLACED
 *                     frames that never reached it, LATE those after the refresh they were for, STEP the least and most
 *                     game time (ms) from one refresh to the next, LOCK how long before its refresh a frame begins
 * DRAWS and TRIS are as the game issues them a tick (every primitive, render targets included, sprites as two
 * triangles), not the GPU's draw calls, which are fewer (batches, groups).
 * RAM is the kernel's view (kernel.c): the EBOOT's image and the blocks allocated from the user partition, out of the
 * 24 MB from 0x08800000 (the rest of the PSP's 32 is the kernel's: the runtime keeps scratch there). The game's own
 * allocators work inside those blocks, so it says how much the game took, not how much of that it uses. */
#include "perf.h"
#include "hle.h"
#include "pacing.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int perf_on;
uint64_t perf_ge_ns, perf_draw_ns, perf_flip_ns, perf_present_ns, perf_sync_ns;
_Atomic uint64_t perf_render_ns, perf_fence_ns;
unsigned long perf_draws, perf_tris, perf_skipped;

uint64_t wall_time_us(void);
uint64_t time_slept_total_us(void);
void kmem_usage(uint32_t *used, uint32_t *total);

/* -- the rings: always kept (a capture wants the last 10 s, whether the panel was on or not) ------------------------ */

#define RING 8192                       /* 10 s at up to 800 a second */
static uint64_t shown_at[RING];         /* (wall clock, us) */
static unsigned long shown_n;
static struct { uint64_t at; unsigned long frame; } ticks[RING];
static unsigned long ticks_n;

void perf_shown(void) {
    shown_at[shown_n++ % RING] = wall_time_us();
}

static void update_every_second(uint64_t now);
static void test_read(void);

void perf_game_frame(unsigned long frame) {
    uint64_t now = wall_time_us();
    ticks[ticks_n % RING].at = now;
    ticks[ticks_n % RING].frame = frame;
    ticks_n++;
    if (ticks_n == 1)
        test_read();
    if (perf_on)
        update_every_second(now);
}

static void test_read(void) {
    if (getenv("MHFU_TEST_PERF") && atoi(getenv("MHFU_TEST_PERF"))) {
        perf_on = 1;
        pacing_watch_panel(1);
    }
}

/* Over the last `seconds`: how many frames shown, their longest interval */
static unsigned long shown_within(uint64_t now, double seconds, double *worst_ms, uint64_t *oldest) {
    uint64_t from = now - (uint64_t)(seconds * 1e6);
    unsigned long n = 0;
    uint64_t worst = 0, first = now;
    unsigned long have = shown_n < RING ? shown_n : RING;
    for (unsigned long i = 0; i + 1 < have; i++) {
        uint64_t a = shown_at[(shown_n - 1 - i) % RING], b = shown_at[(shown_n - 2 - i) % RING];
        if (a < from)
            break;
        n++;
        first = b;
        if (a - b > worst)
            worst = a - b;
    }
    if (worst_ms)
        *worst_ms = (double)worst / 1000;
    if (oldest)
        *oldest = first;
    return n;
}

void perf_summary(double *fps, double *frame_ms, double *worst_ms, double *tick_rate) {
    uint64_t now = wall_time_us(), first;
    unsigned long n = shown_within(now, 1, NULL, &first);
    double secs = (double)(now - first) / 1e6;
    *fps = n && secs > 0 ? (double)n / secs : 0;
    *frame_ms = n ? (double)(now - first) / 1000 / (double)n : 0;
    shown_within(now, 10, worst_ms, NULL);
    unsigned long have = ticks_n < RING ? ticks_n : RING, t = 0;
    for (unsigned long i = 0; i < have && now - ticks[(ticks_n - 1 - i) % RING].at < 1000000; i++)
        t++;
    *tick_rate = (double)t;
}

/* The rings' last seconds, as text: when (seconds before the last), what (ms since the one before) */
void perf_write_times(void *file, double seconds) {
    FILE *f = file;
    uint64_t now = wall_time_us(), from = now - (uint64_t)(seconds * 1e6);
    fprintf(f, "# frames shown: seconds before the capture, ms since the frame before\n");
    unsigned long have = shown_n < RING ? shown_n : RING, n = 0;
    while (n < have && shown_at[(shown_n - 1 - n) % RING] >= from)
        n++;
    for (unsigned long i = n; i-- > 0;) {
        uint64_t a = shown_at[(shown_n - 1 - i) % RING];
        double ms = i + 1 < have ? (double)(a - shown_at[(shown_n - 2 - i) % RING]) / 1000 : 0;
        fprintf(f, "shown -%.3f %.2f\n", (double)(now - a) / 1e6, ms);
    }
    fprintf(f, "# game frames: seconds before the capture, the frame's number, ms since the frame before\n");
    have = ticks_n < RING ? ticks_n : RING, n = 0;
    while (n < have && ticks[(ticks_n - 1 - n) % RING].at >= from)
        n++;
    for (unsigned long i = n; i-- > 0;) {
        uint64_t a = ticks[(ticks_n - 1 - i) % RING].at;
        double ms = i + 1 < have ? (double)(a - ticks[(ticks_n - 2 - i) % RING].at) / 1000 : 0;
        fprintf(f, "tick -%.3f %lu %.2f\n", (double)(now - a) / 1e6, ticks[(ticks_n - 1 - i) % RING].frame, ms);
    }
}

/* -- the last second's split ----------------------------------------------------------------------------------------- */

typedef struct {
    uint64_t wall, slept, ge, draw, flip, present, sync, render, fence;
    unsigned long ticks, shown, draws, tris, skipped;
} Snap;
static Snap last_snap;
static struct {
    int ready;
    double tick_rate, logic, ge, draw, flip, sync, fence, present, render, idle, draws, tris;
    unsigned long skipped;
    uint32_t ram_used, ram_total;
} split;

static Snap snap_now(uint64_t now) {
    Snap s = {now, time_slept_total_us(), perf_ge_ns, perf_draw_ns, perf_flip_ns, perf_present_ns, perf_sync_ns,
              atomic_load_explicit(&perf_render_ns, memory_order_relaxed),
              atomic_load_explicit(&perf_fence_ns, memory_order_relaxed), ticks_n, shown_n, perf_draws, perf_tris,
              perf_skipped};
    return s;
}

static void update_every_second(uint64_t now) {
    if (!last_snap.wall) {
        last_snap = snap_now(now);
        return;
    }
    if (now - last_snap.wall < 1000000)
        return;
    Snap s = snap_now(now);
    const Snap *o = &last_snap;
    double secs = (double)(s.wall - o->wall) / 1e6, ms_ticks = (double)(s.ticks - o->ticks), ms_shown = (double)(s.shown - o->shown);
    if (ms_ticks < 1) ms_ticks = 1;
    if (ms_shown < 1) ms_shown = 1;
    double ge = (double)(s.ge - o->ge) / 1e6, draw = (double)(s.draw - o->draw) / 1e6, flip = (double)(s.flip - o->flip) / 1e6;
    double sync = (double)(s.sync - o->sync) / 1e6, slept = (double)(s.slept - o->slept) / 1e3, wall = (double)(s.wall - o->wall) / 1e3;
    double logic = wall - slept - ge - draw - flip - sync;
    split.tick_rate = (double)(s.ticks - o->ticks) / secs;
    split.logic = (logic < 0 ? 0 : logic) / ms_ticks;
    split.ge = ge / ms_ticks;
    split.draw = draw / ms_shown;
    split.flip = flip / ms_shown;
    split.sync = sync / ms_shown;
    split.fence = (double)(s.fence - o->fence) / 1e6 / ms_shown;
    split.present = (double)(s.present - o->present) / 1e6 / ms_shown;
    split.render = (double)(s.render - o->render) / 1e6 / ms_shown;
    split.idle = 100 * slept / wall;
    split.draws = (double)(s.draws - o->draws) / ms_ticks;
    split.tris = (double)(s.tris - o->tris) / ms_ticks;
    split.skipped = s.skipped - o->skipped;
    kmem_usage(&split.ram_used, &split.ram_total);
    split.ready = 1;
    last_snap = s;
}

void perf_toggle(void) {
    perf_on = !perf_on;
    pacing_watch_panel(perf_on);
    last_snap.wall = 0;                 /* (what it measures starts again from now: the counters stood still) */
    split.ready = 0;
}

/* -- drawn ------------------------------------------------------------------------------------------------------------- */

#define GRAPH_N 208
#define GRAPH_H 28
#define PANEL_DOTS 216                  /* its width: GRAPH_N and a margin */
#define LINE 9

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int perf_rects(float x0, float y0, float scale, HintRect *out, int max) {
    if (!perf_on)
        return 0;
    int dot = (int)(scale / 2) < 1 ? 1 : (int)(scale / 2);
    int lines = 11;
    int height = 4 + GRAPH_H + 3 + lines * LINE + 2;
    int picture_w = (int)(480 * scale);
    Pen p = {out, 0, max, dot, (int)x0 + picture_w - (PANEL_DOTS + 3) * dot, (int)y0 + 3 * dot};
    pen_box(&p, 0, 0, PANEL_DOTS, height, C_INK);
    /* the graph, the latest at the right: how long each frame shown took, spikes marked */
    uint32_t ms[GRAPH_N], sorted[GRAPH_N];
    int n = 0;
    unsigned long have = shown_n < RING ? shown_n : RING;
    for (unsigned long i = 0; i + 1 < have && n < GRAPH_N; i++, n++)
        ms[n] = (uint32_t)(shown_at[(shown_n - 1 - i) % RING] - shown_at[(shown_n - 2 - i) % RING]);
    memcpy(sorted, ms, sizeof(uint32_t) * (size_t)n);
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_u32);
    uint32_t median = n ? sorted[n / 2] : 16667;
    double top = (double)median * 3;                 /* a frame at the usual time a third of the way up */
    if (top < 8000) top = 8000;
    int gx = 4, gy = 3;
    pen_box(&p, gx, gy, GRAPH_N, GRAPH_H, C_PAD);
    pen_box(&p, gx, gy + GRAPH_H - GRAPH_H / 3, GRAPH_N, 1, C_GREY);
    for (int i = 0; i < n; i++) {
        double h = (double)ms[i] / top * GRAPH_H;
        int bar = h < 1 ? 1 : h > GRAPH_H ? GRAPH_H : (int)h;
        int color = (double)ms[i] > 2.0 * (double)median ? C_RED : (double)ms[i] > 1.5 * (double)median ? C_YELLOW : C_GREEN;
        pen_box(&p, gx + GRAPH_N - 1 - i, gy + GRAPH_H - bar, 1, bar, color);
        if (color == C_RED)                 /* (a spike: marked above the bars, whatever its height) */
            pen_box(&p, gx + GRAPH_N - 1 - i, gy - 2, 1, 2, C_RED);
    }
    double fps, frame_ms, worst, tick;
    perf_summary(&fps, &frame_ms, &worst, &tick);
    char t[64];
    int y = gy + GRAPH_H + 3;
#define TEXT(color, ...) do { snprintf(t, sizeof t, __VA_ARGS__); pen_text(&p, 4, y, t, color); y += LINE; } while (0)
    TEXT(C_WHITE, "FPS %.1f  %.2f MS  WORST 10S %.1f", fps, frame_ms, worst);
    TEXT(C_GREY, "GRAPH TOP %.0f MS  RED > 2X %.1f", top / 1000, (double)median / 1000);
    if (!split.ready) {
        TEXT(C_GREY, "THE REST AFTER A SECOND");
        return p.n;
    }
    TEXT(C_WHITE, "TICK %.1f/S  SKIPPED %lu  IDLE %.0f%%", split.tick_rate, split.skipped, split.idle);
    TEXT(C_FACE, "TICK MS: LOGIC %.1f  GE %.1f", split.logic, split.ge);
    TEXT(C_FACE, "FRAME MS: DRAW %.1f  FLIP %.1f", split.draw, split.flip);
    TEXT(C_FACE, " RT WAIT %.1f  GPU WAIT %.1f", split.sync, split.fence);
    TEXT(C_FACE, " ACQ+PRES %.1f  RENDER THR %.1f", split.present, split.render);
    TEXT(C_WHITE, "TICK: DRAWS %.0f  TRIS %.0f", split.draws, split.tris);
    TEXT(C_WHITE, "RAM %.1f/%.1f MB  KERNEL BLOCKS", (double)split.ram_used / 1048576, (double)split.ram_total / 1048576);
    PacingStats ps;
    pacing_stats(&ps);
    if (ps.have) {
        if (ps.lead_ms > 0)
            TEXT(C_WHITE, "DISPLAY %.2f HZ  SHOWN %lu  LOCK %.1f MS", ps.refresh_hz, ps.shown, ps.lead_ms);
        else
            TEXT(C_WHITE, "DISPLAY %.2f HZ  SHOWN %lu  TIMER", ps.refresh_hz, ps.shown);
        TEXT(ps.repeats || ps.replaced || ps.late ? C_YELLOW : C_WHITE, "REPEAT %lu REPLACED %lu LATE %lu STEP %.1f-%.1f",
             ps.repeats, ps.replaced, ps.late, ps.step_min_ms, ps.step_max_ms);
    } else {
        TEXT(C_GREY, "DISPLAY: NOT REPORTED");
        y += LINE;
    }
#undef TEXT
    return p.n;
}
