/* Which generated functions run on which frames (survey only, linked into a RECOMP_TRACE_ALL build, never into the game).
 * MHFU_TICKS_OUT=file MHFU_TICKS_FROM=frame MHFU_TICKS_TO=frame: (module, function, caller, class) -> calls, written
 * whole every 500 game frames and at TO. class 0: on a frame between 30ths; 1: on a 30th under a gate (time scale 1);
 * 2: on a 30th not under a gate (native call, or the render pass). Reads no game state: replays run as without it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp.h"
unsigned long present_frame_number(void);
extern int rt_frame30; extern float rt_time_scale;
#define SLOTS (1u << 22)
typedef struct { uint32_t fn, ra, mc; uint32_t calls; } Slot;
static Slot *s; static const char *path; static int off; static unsigned long from, to, last;
static void dump(void) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    for (uint32_t i = 0; i < SLOTS; i++) if (s[i].calls)
        fprintf(f, "%u %08x %08x %u %u\n", s[i].mc >> 8, s[i].fn, s[i].ra, (s[i].mc & 255) - 1, s[i].calls);
    fclose(f); rename(tmp, path);
}
int rt_trace_all(Ctx *c, int module, uint32_t fn) {
    if (off) return 0;
    if (!s) {
        path = getenv("MHFU_TICKS_OUT");
        if (!path) { off = 1; return 0; }
        from = strtoul(getenv("MHFU_TICKS_FROM") ? getenv("MHFU_TICKS_FROM") : "0", 0, 10);
        to = strtoul(getenv("MHFU_TICKS_TO") ? getenv("MHFU_TICKS_TO") : "999999999", 0, 10);
        s = calloc(SLOTS, sizeof(Slot));
    }
    unsigned long fr = present_frame_number();
    if (fr < from) return 0;
    if (fr > to) { dump(); off = 1; return 0; }
    if (fr >= last + 500) { dump(); last = fr; }
    uint32_t ra = c->r[31];
    uint32_t cls = !rt_frame30 ? 0 : rt_time_scale == 1 ? 1 : 2;
    uint32_t mc = (uint32_t)module << 8 | (cls + 1);
    uint32_t h = (fn * 2654435761u ^ ra * 40503u ^ mc * 97u) & (SLOTS - 1);
    while (s[h].calls && !(s[h].fn == fn && s[h].ra == ra && s[h].mc == mc)) h = (h + 1) & (SLOTS - 1);
    s[h].fn = fn; s[h].ra = ra; s[h].mc = mc; s[h].calls++;
    return 0;
}
