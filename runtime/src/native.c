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
#include "savestate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* -- what gated code did at the last 30th: the chunks it linked, the converted calls it made -------------------- */

/* In order: a chunk linked (its commands, for the GE to draw again), or a converted call made (a "native" gate: made
 * again on the frames between, where its gate - the one it is under - skips). Each is noted under the gate it was in
 * (site, and key: the object, for an object's gate); within a converted call, under a gate inside it, if any. */
#define MAX_CHUNKS 1024
SAVED static struct {
    uint32_t site, key;             /* the gate it was linked or made under */
    uint32_t mgr, layer;            /* a chunk: the list's (its manager), the layer */
    uint32_t start, end;            /* its commands, while that frame's list is run */
    void (*fn)(Ctx *c);             /* a call: its function (or with none, addr) and its arguments */
    uint32_t addr, a[4];
    uint32_t slots[4];              /* (a call: the game's states as it was made - their objects) */
} chunks[MAX_CHUNKS];

/* The game's states (EBOOT 08895538's four slots, at *0x08A8B308): their objects. A converted call made at a 30th is
 * made again between only while they are the same - not after a state ended (the hunt's objects freed with it) */
static void states_now(uint32_t *out) {
    uint32_t base = LW(0x08A8B308u);
    for (int i = 0; i < 4; i++)
        out[i] = base >= 0x08800000u && base < 0x0A000000u ? LW(base + 4 * (uint32_t)i) : 0;
}
SAVED static int nchunks;
SAVED static uint32_t gate_site, gate_key;    /* the gate being run at a 30th (0: none) */
SAVED static float real_scale = 1;        /* the frame's time scale and rt_tick30, whatever a gate has made them */
SAVED static int real_tick30 = 1, real_ticks60 = 2;

/* EBOOT 088596C8 (patched): a chunk of count words at start is linked into the frame's list, at the end of a layer */
void rt_list_link(uint32_t mgr, uint32_t start, uint32_t count, uint32_t layer) {
    if (!gate_site || nchunks == MAX_CHUNKS)
        return;
    chunks[nchunks].site = gate_site;
    chunks[nchunks].key = gate_key;
    chunks[nchunks].mgr = mgr;
    chunks[nchunks].layer = layer;
    chunks[nchunks].start = start & 0x0FFFFFFFu;
    chunks[nchunks].end = (start & 0x0FFFFFFFu) + 4 * count;
    chunks[nchunks].fn = NULL;
    chunks[nchunks].addr = 0;
    nchunks++;
}

/* ge.c: the chunk (its number) a command is in, if the list run was made at a 30th with gates; else -1 */
SAVED static int by_start[MAX_CHUNKS], nsorted = -1, by_start_n;    /* the chunks (not the calls) by where they start, once linked */

static int cmp_start(const void *a, const void *b) {
    uint32_t x = chunks[*(const int *)a].start, y = chunks[*(const int *)b].start;
    return x < y ? -1 : x > y;
}

int native_chunk_at(uint32_t at) {
    if (!rt_frame30 || !nchunks)
        return -1;
    if (nsorted != nchunks) {           /* (sorted again when more are linked) */
        int n = 0;
        for (int i = 0; i < nchunks; i++)
            if (chunks[i].end > chunks[i].start)
                by_start[n++] = i;
        qsort(by_start, (size_t)n, sizeof *by_start, cmp_start);
        for (int i = n; i < MAX_CHUNKS && i < nchunks; i++)
            by_start[i] = -1;
        nsorted = nchunks;
        by_start_n = n;
    }
    int lo = 0, hi = by_start_n;        /* the last starting at or before it */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (chunks[by_start[mid]].start <= at) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0)
        return -1;
    int i = by_start[lo - 1];
    return at < chunks[i].end ? i : -1;
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

double rt_game_30ths(void);

/* MHFU_NATIVE_TRACE=file: each game frame, the hunter's (the first object ticked with the vtable MHFU_TRACE_VT, by
 * default the hunt's hunter, 089BB3CC - while it has it) position, state function and ticks in it, by the game's time - to compare runs
 * at 30 and at N a second */
static uint32_t traced;
#define LWF(a) (*(float *)MEM(a))
static void trace(void) {
    static FILE *f;
    static int open = -1;
    if (open < 0)
        open = (f = getenv("MHFU_NATIVE_TRACE") ? fopen(getenv("MHFU_NATIVE_TRACE"), "w") : NULL) != NULL;
    if (!open || !traced)
        return;
    uint32_t scene = LW(0x089CBD04u);
    static uint32_t said;
    if (said != (scene ^ traced))
        fprintf(f, "# scene %08x object %08x\n", scene, traced);
    said = scene ^ traced;
    fprintf(f, "%.3f %d %.2f %.2f %.2f %08x %u %.2f", rt_game_30ths(), rt_frame30, (double)LWF(traced + 0x40),
            (double)LWF(traced + 0x44), (double)LWF(traced + 0x48), LW(traced + 0x400), LW(traced + 0x3F4),
            (double)LWF(traced + 0x90));
    if (scene)                      /* the camera: its eye and target */
        for (int i = 0; i < 6; i++)
            fprintf(f, " %.2f", (double)LWF(scene + 0xB40 + 4 * (i % 3) + (i < 3 ? 0 : 0x10)));
    static int w_off = -1, w_n;             /* MHFU_TRACE_WORDS=offset,count (hex): the object's words there */
    if (w_off < 0) {
        w_off = 0;
        if (getenv("MHFU_TRACE_WORDS"))
            sscanf(getenv("MHFU_TRACE_WORDS"), "%x,%x", (unsigned *)&w_off, (unsigned *)&w_n);
    }
    for (int i = 0; i < w_n; i++)
        fprintf(f, " %08x", LW(traced + (uint32_t)w_off + 4 * (uint32_t)i));
    if (scene && getenv("MHFU_TRACE_VIEW"))     /* the view in use, its words */
        for (uint32_t v = scene + 0xB0 + 0x1A0 * LBU(scene + 0xA8D), i = 0; i < 0x1A0; i += 4)
            fprintf(f, " %08x", LW(v + i));
    fputc('\n', f);
    fflush(f);
}

/* A step a game frame of an ease towards a value - by 1/n of the way, x the way left (patches.py: a colour's fade in
 * the render pass) - in game time: at the game's rate as it was, else so that it takes as long */
float rt_ease(float x, float n) {
    if (rt_time_scale == 1 || !(n > 1))
        return x / n;
    return x * (1.0f - powf(1.0f - 1.0f / n, rt_time_scale));
}

/* Whether a 30th fell on each of the last game frames (gpu.c) */
SAVED static uint32_t frames30;           /* bit 0 the latest */

/* back frames before the latest (0: the latest - the one being drawn, as the GE draws it) */
int native_frame30(int back) {
    return back < 32 && (frames30 >> back & 1);
}

/* rt_frame_start (thread.c): a 30th with gates - its gated code will link its chunks anew */
void native_frame_start(void) {
    trace();
    frames30 = frames30 << 1 | (uint32_t)rt_frame30;
    real_scale = rt_time_scale;
    real_tick30 = rt_tick30;
    real_ticks60 = rt_ticks60;
    if (rt_frame30 && rt_time_scale != 1) {
        nchunks = 0;
        nsorted = -1;
        gpu_held_clear();
    }
}

/* -- converted objects --------------------------------------------------------------------------------------------- */

/* The vtables of the objects that tick every frame: the hunt's hunter (089BB3CC); MHFU_NATIVE_VT=hex,... others (0:
 * none) */
static int converted(uint32_t vtable) {
    static uint32_t vt[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("MHFU_NATIVE_VT");
        if (!e)
            vt[n++] = 0x089BB3CCu;
        for (; e && *e && n < 32;) {
            char *end;
            vt[n++] = (uint32_t)strtoul(e, &end, 16);
            e = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++)
        if (vt[i] == vtable)
            return 1;
    return 0;
}

/* -- gates --------------------------------------------------------------------------------------------------------- */

static void run_as_30(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at, uint32_t key) {
    float scale = rt_time_scale;
    int tick30 = rt_tick30, ticks60 = rt_ticks60;
    uint32_t site = gate_site, k = gate_key;
    rt_time_scale = 1;
    rt_tick30 = 1;
    rt_ticks60 = 2;
    if (!site && rt_frame30) {
        gate_site = at;
        gate_key = key;
    }
    fn ? fn(c) : rt_call(c, addr);
    gate_site = site;
    gate_key = k;
    rt_time_scale = scale;
    rt_tick30 = tick30;
    rt_ticks60 = ticks60;
}

/* Converted code: at the frame's own time scale, whatever gate it is under, and none of what it draws kept (it draws
 * every frame) but what gates inside it draw */
static void run_native(Ctx *c, void (*fn)(Ctx *c), uint32_t addr) {
    float scale = rt_time_scale;
    int tick30 = rt_tick30, ticks60 = rt_ticks60;
    uint32_t site = gate_site, k = gate_key;
    rt_time_scale = real_scale;
    rt_tick30 = real_tick30;
    rt_ticks60 = real_ticks60;
    gate_site = gate_key = 0;
    fn ? fn(c) : rt_call(c, addr);
    gate_site = site;
    gate_key = k;
    rt_time_scale = scale;
    rt_tick30 = tick30;
    rt_ticks60 = ticks60;
}

/* A gate skipped, on a frame between 30ths: what its code did at the last one, again - the chunks it linked drawn
 * again, the converted calls it made made again (with the arguments they had) */
static void skipped(Ctx *c, uint32_t at, uint32_t key) {
    for (int i = 0; i < nchunks; i++) {
        if (chunks[i].site != at || chunks[i].key != key)
            continue;
        if (!chunks[i].fn && !chunks[i].addr) {
            link_marker(i);
            continue;
        }
        uint32_t now[4];
        states_now(now);
        if (memcmp(now, chunks[i].slots, sizeof now))
            continue;
        uint32_t a[4] = {c->r[4], c->r[5], c->r[6], c->r[7]};
        for (int j = 0; j < 4; j++)
            c->r[4 + j] = chunks[i].a[j];
        run_native(c, chunks[i].fn, chunks[i].addr);
        for (int j = 0; j < 4; j++)
            c->r[4 + j] = a[j];
    }
}

static void gate(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at, uint32_t key) {
    if ((rt_tick30 && rt_time_scale == 1) || ungated(at)) {
        fn ? fn(c) : rt_call(c, addr);
        return;
    }
    if (rt_frame30)
        run_as_30(c, fn, addr, at, key);
    else
        skipped(c, at, key);
}

/* A gated function or call: as at 30 a second. Within one already, or at 30 a second, it just runs. (fn, or with none
 * a call to addr; at: the call, or the function) */
void rt_gate(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    gate(c, fn, addr, at, 0);
}

/* An object's tick (a0 the object): every frame, if its vtable is converted; otherwise gated, what it drew kept for
 * it alone */
void rt_gate_object(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    static int log = -1;
    static uint32_t trace_vt;
    if (log < 0) {
        log = getenv("MHFU_NATIVE_LOG") != NULL;
        trace_vt = getenv("MHFU_TRACE_VT") ? (uint32_t)strtoul(getenv("MHFU_TRACE_VT"), NULL, 16) : 0x089BB3CCu;
    }
    /* (the one traced: the first object ticked with that vtable, kept while it has it - a herd's animals each the same) */
    if (LW(c->r[4]) == trace_vt && (!traced || LW(traced) != trace_vt))
        traced = c->r[4];
    if (log) {                      /* MHFU_NATIVE_LOG: each vtable ticked, the first time */
        static uint32_t seen[256];
        static int nseen;
        uint32_t vt = LW(c->r[4]);
        int i = 0;
        while (i < nseen && seen[i] != vt)
            i++;
        if (i == nseen && nseen < 256) {
            seen[nseen++] = vt;
            fprintf(stderr, "native: object %08x vtable %08x (tick %08x)\n", c->r[4], vt, fn ? 0 : addr);
        }
    }
    if (real_scale != 1 && converted(LW(c->r[4])))
        run_native(c, fn, addr);
    else
        gate(c, fn, addr, at, c->r[4]);
}

/* A converted call, made by gated code: made at the frame's own rate - noted at a 30th, with its arguments, for the
 * gate it is under to make it again on the frames between */
void rt_gate_native(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    (void)at;
    if (real_scale == 1) {
        fn ? fn(c) : rt_call(c, addr);
        return;
    }
    if (rt_frame30 && gate_site && nchunks < MAX_CHUNKS) {
        chunks[nchunks].site = gate_site;
        chunks[nchunks].key = gate_key;
        chunks[nchunks].start = chunks[nchunks].end = 0;
        chunks[nchunks].fn = fn;
        chunks[nchunks].addr = addr;
        for (int j = 0; j < 4; j++)
            chunks[nchunks].a[j] = c->r[4 + j];
        states_now(chunks[nchunks].slots);
        nchunks++;
    }
    run_native(c, fn, addr);
}

/* RECOMP_TRACE's functions, called: with MHFU_CALL_LOG, the game's time, whether under a gate as at 30, the function,
 * its caller and first arguments */
void rt_trace_call(Ctx *c, uint32_t fn) {
    static int on = -1;
    if (on < 0)
        on = getenv("MHFU_CALL_LOG") != NULL;
    if (on)
        fprintf(stderr, "call: %.2f %s %08x from %08x (%08x %08x %08x)\n", rt_game_30ths(),
                rt_time_scale == 1 && real_scale != 1 ? "30" : "--", fn, c->r[31], c->r[4], c->r[5], c->r[6]);
}

/* A gate of what makes ready for the logic of a 30th: run on the frame before one */
void rt_gate_before(Ctx *c, void (*fn)(Ctx *c), uint32_t addr, uint32_t at) {
    if ((rt_tick30 && rt_time_scale == 1) || ungated(at))
        fn ? fn(c) : rt_call(c, addr);
    else if (rt_next_frame30())
        run_as_30(c, fn, addr, at, 0);
}
