/* Frame pacing: what the display really did with the frames shown (VK_EXT_present_timing, where the driver has it), a
 * log of it, and the figures the performance panel gives. See pacing.c. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

extern int pacing_watch;                /* something wants the display's reports: the log, the panel. Off, a branch */

void pacing_init(void);                 /* the settings: first thing, before the renderer */
void pacing_watch_panel(int on);        /* perf.c: the panel is on */
VkPresentModeKHR pacing_present_mode(const VkPresentModeKHR *modes, uint32_t n);  /* vk.c: of the surface's */
void pacing_swapchain_created(void);    /* vk.c: its timing set up (a new one's reports start again) */

/* present.c, for a frame about to be shown: when it was due, and what moment of the game it shows */
void pacing_frame(uint64_t due_wall_us, uint64_t issue_wall_us, int64_t content_us, unsigned long game_frame, float t);
void pacing_drawn(uint64_t wall_us);    /* present.c: its draws are queued */
void pacing_tick(unsigned long frame);  /* present.c: a game frame done */

/* render.c: the present's own chain (its id, what to report), and what came of it */
const void *pacing_present_chain(void);
void pacing_presented(VkResult r);

typedef struct {
    int have;                           /* the display's reports have come in, over the last second */
    double refresh_hz;                  /* the driver's, or else from the reports */
    unsigned long shown;                /* frames that reached the screen */
    unsigned long replaced;             /* frames that never did */
    unsigned long repeats;              /* refreshes that showed the frame before again */
    double step_ms, step_min_ms, step_max_ms;  /* the game's time from one refresh to the next, the eye's motion */
} PacingStats;
void pacing_stats(PacingStats *out);    /* perf.c */
