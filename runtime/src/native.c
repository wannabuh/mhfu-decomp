/* Towards the native frame rate (docs/native.md): what has not been converted yet runs as at 30 frames a second, behind
 * gates (recomp/patches.py's GATES), whatever the game's rate (MHFU_TICK_HZ).
 *
 * A gate runs its code on the frames a 30th falls on (rt_frame30), with the time scale 1 - and skips it on the others.
 * What the code drew is kept for those: the game draws into display list chunks linked into the frame's list by layer
 * (EBOOT 088596C8, patched to tell us); a chunk linked from gated code is noted, the GE keeps the draws it runs from it
 * (gpu.c), and on the frames between, where the gate skips, a small chunk is linked in its place that has the GE draw
 * them again (a NOP marking it: ge.c). So what is drawn by the logic itself - the HUD, much of the 2D - stays on
 * screen, as it was at its last 30th. */
#include "hle.h"

#include <stdlib.h>

int rt_next_frame30(void);
void gpu_held_clear(void);

/* MHFU_UNGATE=addr,... (hex): gates left open - what they gate runs every frame, at the game's rate (to try one) */
static int ungated(uint32_t at) {
    static uint32_t open[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (const char *e = getenv("MHFU_UNGATE"); e && *e && n < 32;) {
            char *end;
            open[n++] = (uint32_t)strtoul(e, &end, 16);
            e = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++)
        if (open[i] == at)
            return 1;
    return 0;
}

/* -- the chunks gated code linked, at the last 30th ---------------------------------------------------------------- */

#define MAX_CHUNKS 256
static struct {
    uint32_t site;                  /* the gate it was linked under */
    uint32_t mgr, layer;            /* the list's (its manager), the layer */
    uint32_t start, end;            /* its commands, while that frame's list is run */
} chunks[MAX_CHUNKS];
static int nchunks;
static uint32_t gate_site;          /* the gate being run at a 30th (0: none) */

/* EBOOT 088596C8 (patched): a chunk of count words at start is linked into the frame's list, at the end of a layer */
void rt_list_link(uint32_t mgr, uint32_t start, uint32_t count, uint32_t layer) {
    if (!gate_site || nchunks == MAX_CHUNKS)
        return;
    chunks[nchunks].site = gate_site;
    chunks[nchunks].mgr = mgr;
    chunks[nchunks].layer = layer;
    chunks[nchunks].start = start & 0x0FFFFFFFu;
    chunks[nchunks].end = (start & 0x0FFFFFFFu) + 4 * count;
    nchunks++;
}

/* ge.c: the chunk (its number) a command is in, if the list run was made at a 30th with gates; else -1 */
int native_chunk_at(uint32_t at) {
    if (!rt_frame30)
        return -1;
    for (int i = 0; i < nchunks; i++)
        if (at >= chunks[i].start && at < chunks[i].end)
            return i;
    return -1;
}

/* A chunk of three words linked in a chunk's place: the GE draws again what it drew (a NOP: 0x00A5 and its number) */
static void link_marker(int i) {
    uint32_t mgr = chunks[i].mgr, layer = chunks[i].layer;
    if (LW(0x089CC120u))                /* a chunk is being written (it would be written over): left out */
        return;
    uint32_t start = LW(mgr + 0x80198u);    /* the list's next free word (EBOOT 088597FC) */
    SW(start, 0x00A50000u | (uint32_t)i);
    SW(mgr + 0x80198u, start + 12);
    /* linked as EBOOT 088596C8 does: the layer's end (a BASE and JUMP onwards) moved to the chunk's last two words,
     * the old end made a jump to the chunk */
    uint32_t tail = LW(mgr + 0x148u + 4 * layer);
    SW(start + 4, LW(tail));
    SW(start + 8, LW(tail + 4));
    SW(tail, ((start & 0xFF000000u) >> 8) | 0x10000000u);
    SW(tail + 4, ((start << 8) >> 8) | 0x08000000u);
    SW(mgr + 0x148u + 4 * layer, start + 4);
}

/* rt_frame_start (thread.c): a 30th with gates - its gated code will link its chunks anew */
void native_frame_start(void) {
    if (rt_frame30 && rt_time_scale != 1) {
        nchunks = 0;
        gpu_held_clear();
    }
}

/* -- gates --------------------------------------------------------------------------------------------------------- */

static void run_as_30(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    float scale = rt_time_scale;
    int tick30 = rt_tick30;
    uint32_t site = gate_site;
    rt_time_scale = 1;
    rt_tick30 = 1;
    if (!site && rt_frame30)
        gate_site = at;
    fn ? fn(c) : rt_call(c, addr);
    gate_site = site;
    rt_time_scale = scale;
    rt_tick30 = tick30;
}

/* A gated function or call: as at 30 a second. Within one already, or at 30 a second, it just runs. (fn, or with none
 * a call to addr; at: the call, or the function) */
void rt_gate(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    if ((rt_tick30 && rt_time_scale == 1) || ungated(at)) {
        fn ? fn(c) : rt_call(c, addr);
        return;
    }
    if (rt_frame30) {
        run_as_30(c, fn, addr, at);
        return;
    }
    for (int i = 0; i < nchunks; i++)
        if (chunks[i].site == at)
            link_marker(i);
}

/* A gate of what makes ready for the logic of a 30th: run on the frame before one */
void rt_gate_before(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    if ((rt_tick30 && rt_time_scale == 1) || ungated(at))
        fn ? fn(c) : rt_call(c, addr);
    else if (rt_next_frame30())
        run_as_30(c, fn, addr, at);
}
