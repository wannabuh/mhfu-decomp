/* What is just out of view, for the renderer's shadows - without the game knowing.
 *
 * The game draws only what passes its view test (EBOOT 088160F0), and the shadows are made from what is drawn. So that
 * the shadows of what is just out of view do not come and go as the camera turns, a model within rt_cull_margin of the
 * view is drawn too - but drawing writes the game's memory (the display list, the matrices, the random numbers effects
 * draw from, what a model keeps): the game must not see it. The test itself says what the game would say; those it
 * fails that the margin would pass are noted here, and drawn on the side:
 *
 * - just before the frame's list runs, its GE state put aside (gpu.c), all the game's memory is made read-only: the
 *   first write to a page keeps a copy of it (on_fault);
 * - the game's own code then queues what was noted and runs its render pass over only that, and the chunks it links
 *   (native.c rt_list_link) go to the GE by themselves, in the order of their layers - for the shadow maps only, not
 *   the picture (render.c render_shadow_only: what is out of view is not in it, and its effects go by what is drawn when);
 * - the pages and the CPU's registers are put back, as is the GE. Nothing of it remains in the game's memory, whatever
 *   the drawing wrote, so the game runs exactly as with no shadows at all.
 *
 * It all happens on the game's thread between two instructions: no kernel call runs in between (hle_table.c), no other
 * PSP thread, no interrupt. */
#define _GNU_SOURCE
#include "hle.h"
#include "render_queue.h"
#include "savestate.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_NOTED 512
#define MAX_CHUNKS 512

typedef struct {
    uint32_t add;                       /* the function that queues it (it passes the view test here): 0 a model drawn
                                         * already, some of whose instances were out of view */
    uint32_t mgr, layer, object, forced;
    uint32_t vtable;                    /* (as when noted: to tell it is still there) */
} Noted;

/* EBOOT 0884C9B8, 0884CB24: the logic queues a model; 09CC39A0, 09CCD510 (game_sub): a queued model's draw tests its
 * instances, each by itself. Told by where the test returns to */
#define RET_ADD_OBJECT 0x0884CAA4u
#define RET_ADD_AT_40 0x0884CB7Cu
#define ADD_OBJECT 0x0884C9B8u
#define ADD_AT_40 0x0884CB24u
#define RET_INSTANCES_A 0x09CC3A94u
#define RET_INSTANCES_B 0x09CCD60Cu
#define RENDER_MANAGER 0x089CC11Cu      /* (holds the address of the render manager) */
#define INSERT 0x0884CBD8u              /* render_queue_insert_depth_sorted */
#define QUEUES_CLEAR 0x0884BB00u
#define LAYER_STATES 0x0884C22Cu
#define RENDER_PASS 0x0884BB58u

SAVED static Noted queued[MAX_NOTED];   /* what the logic queues would have, while the queues are kept */
SAVED static int nqueued;
SAVED static Noted drawn[MAX_NOTED];    /* those of the frame's render pass, and its models with instances out of view */
SAVED static int ndrawn;
static struct { uint32_t start, count, layer; } chunks[MAX_CHUNKS];
static int nchunks;
int rt_shadow_extras_running;

static unsigned long stat_frames, stat_models, stat_pages, stat_chunks, stat_us;
static int stats_on = -1;

/* -- the game's memory, read-only for a while ------------------------------------------------------------------------ */

#define PAGE 4096u
static const struct { uint32_t start, size; } kept[] = {    /* (below 0x08800000: the renderer's scratch, not the game's) */
    {SCRATCHPAD_START, SCRATCHPAD_SIZE},
    {VRAM_START, VRAM_SIZE},
    {0x08800000u, RAM_START + RAM_SIZE - 0x08800000u},
};
#define NKEPT (sizeof kept / sizeof kept[0])
static uint8_t *copies;                 /* the pages' copies, in the order written */
static uint32_t *copied, ncopied;       /* each one's address */
static volatile int keeping;
static atomic_flag fault_lock = ATOMIC_FLAG_INIT;

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *at = si->si_addr;
    if (keeping && at >= rt_mem) {
        uint64_t psp = (uint64_t)(at - rt_mem);
        for (size_t i = 0; i < NKEPT; i++)
            if (psp >= kept[i].start && psp < (uint64_t)kept[i].start + kept[i].size) {
                int e = errno;
                while (atomic_flag_test_and_set(&fault_lock))
                    ;
                uint32_t page = (uint32_t)psp & ~(PAGE - 1);
                int seen = 0;           /* (another thread's write to it just now: the render thread's, if ever) */
                if (ncopied && copied[ncopied - 1] == page)
                    seen = 1;
                if (!seen) {
                    memcpy(copies + (size_t)ncopied * PAGE, rt_mem + page, PAGE);
                    copied[ncopied++] = page;
                }
                mprotect(rt_mem + page, PAGE, PROT_READ | PROT_WRITE);
                atomic_flag_clear(&fault_lock);
                errno = e;
                return;
            }
    }
    signal(sig, SIG_DFL);               /* (not ours: as if there were no handler) */
}

static void keep_memory(void) {
    static int ready;
    if (!ready) {
        ready = 1;
        size_t pages = 0;
        for (size_t i = 0; i < NKEPT; i++)
            pages += kept[i].size / PAGE;
        copies = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        copied = malloc(pages * sizeof *copied);
        struct sigaction sa = {0};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGSEGV, &sa, NULL);
    }
    ncopied = 0;
    keeping = 1;
    for (size_t i = 0; i < NKEPT; i++)
        mprotect(rt_mem + kept[i].start, kept[i].size, PROT_READ);
}

static void restore_memory(void) {
    keeping = 0;
    for (uint32_t i = 0; i < ncopied; i++)      /* (each writable already) */
        memcpy(rt_mem + copied[i], copies + (size_t)i * PAGE, PAGE);
    for (size_t i = 0; i < NKEPT; i++)
        mprotect(rt_mem + kept[i].start, kept[i].size, PROT_READ | PROT_WRITE);
}

/* -- what the game's view test would say ----------------------------------------------------------------------------- */

static float rf(Ctx *c, uint32_t a) {
    (void)c;
    uint32_t u = LW(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* EBOOT 088160F0's test (a sphere against the planes of the camera at cam; its far limit given), by another radius */
static int in_view(Ctx *c, uint32_t cam, uint32_t centre, float radius, float far) {
    float x = rf(c, centre), y = rf(c, centre + 4), z = rf(c, centre + 8);
#define ROW(at) (rf(c, cam + (at)) * x + rf(c, cam + (at) + 0x10) * y + rf(c, cam + (at) + 0x20) * z + rf(c, cam + (at) + 0x30))
    float vz = ROW(0xB88);
    if (vz - radius > rf(c, cam + 0xD90) || vz + radius < far)
        return 0;
    float vx = ROW(0xB80);
    if (vx * rf(c, cam + 0xD50) + vz * rf(c, cam + 0xD58) > radius ||
        vx * rf(c, cam + 0xD60) + vz * rf(c, cam + 0xD68) > radius)
        return 0;
    float vy = ROW(0xB84);
    if (vy * rf(c, cam + 0xD74) + vz * rf(c, cam + 0xD78) > radius ||
        vy * rf(c, cam + 0xD84) + vz * rf(c, cam + 0xD88) > radius)
        return 0;
#undef ROW
    return 1;
}

static void note(Noted *list, int *n, uint32_t add, uint32_t mgr, uint32_t layer, uint32_t object, uint32_t forced,
                 uint32_t vtable) {
    if (*n && !add && list[*n - 1].object == object && !list[*n - 1].add)
        return;                         /* (a model's instances, one after another) */
    if (*n < MAX_NOTED)
        list[(*n)++] = (Noted){add, mgr, layer, object, forced, vtable};
}

/* EBOOT 088160F0 (patched): 1 if the test's result is given here, else it goes on as it would (the camera's +0xAAA clear:
 * it passes anything) */
int rt_view_test(Ctx *c) {
    float margin = rt_cull_margin;
    uint32_t back = c->r[31];
    int instances = back == RET_INSTANCES_A || back == RET_INSTANCES_B;
    if (rt_shadow_extras_running) {     /* on the side: those noted are drawn; of a model's instances, those it left out */
        c->r[2] = 1;
        if (instances && LBU(c->r[4] + 0xAAAu))
            c->r[2] = in_view(c, c->r[4], c->r[5], c->f[12].f + margin, c->f[13].f) &&
                      !in_view(c, c->r[4], c->r[5], c->f[12].f, c->f[13].f);
        return 1;
    }
    if (margin <= 0 || !LBU(c->r[4] + 0xAAAu))
        return 0;
    if (in_view(c, c->r[4], c->r[5], c->f[12].f, c->f[13].f) ||
        !in_view(c, c->r[4], c->r[5], c->f[12].f + margin, c->f[13].f))
        return 0;
    if (back == RET_ADD_OBJECT || back == RET_ADD_AT_40) {
        /* (those functions' registers, at their call of the test: the manager, layer, object, whether forced) */
        note(queued, &nqueued, back == RET_ADD_OBJECT ? ADD_OBJECT : ADD_AT_40, c->r[19], c->r[18], c->r[17], c->r[16],
             LW(c->r[17]));
    } else if (instances) {
        uint32_t object = back == RET_INSTANCES_A ? c->r[19] : c->r[17], mgr = LW(RENDER_MANAGER);
        note(drawn, &ndrawn, 0, mgr, LBU(mgr + 0x20Du), object, 0, LW(object));   /* (+0x20D: the layer being drawn) */
    }
    return 0;
}

/* EBOOT 0884BB00 (patched): the render queues emptied */
void rt_queues_cleared(void) {
    if (!rt_shadow_extras_running)
        nqueued = 0;
}

/* EBOOT 0884BB58 (patched): the render pass begins: what it draws of those noted is what is noted now */
void rt_render_pass_begin(void) {
    if (rt_shadow_extras_running)
        return;
    memcpy(drawn, queued, (size_t)nqueued * sizeof *queued);
    ndrawn = nqueued;
}

/* -- on the side -------------------------------------------------------------------------------------------------- */

/* native.c rt_list_link: a chunk of the pass on the side: for the GE here, not by the frame's list */
void shadow_extras_chunk(uint32_t start, uint32_t count, uint32_t layer) {
    if (nchunks < MAX_CHUNKS) {
        chunks[nchunks].start = start;
        chunks[nchunks].count = count;
        chunks[nchunks].layer = layer;
        nchunks++;
    }
}

static int cmp_layer(const void *a, const void *b) {
    const typeof(chunks[0]) *x = a, *y = b;
    if (x->layer != y->layer)
        return x->layer < y->layer ? -1 : 1;
    return x->start < y->start ? -1 : x->start > y->start;
}

void ge_run_chunk(uint32_t start, uint32_t end);
extern int gpu_skip_draw;               /* (a frame not drawn: present.c) */
void gpu_state_put_aside(void);
void gpu_state_put_back(void);

static uint64_t now_us(void) {     /* (the real time: time_us is the game's) */
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void call(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    c->r[8] = t0;
    rt_call(c, fn);
}

/* EBOOT 08859728 (patched): the frame's list is about to run; drawn before it, on the side */
void rt_shadow_extras(Ctx *c) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = !getenv("MHFU_SHADOW_EXTRAS") || atoi(getenv("MHFU_SHADOW_EXTRAS")) != 0;
        stats_on = getenv("MHFU_SHADOW_EXTRAS_STATS") != NULL;
    }
    if (!ndrawn || !enabled || rt_cull_margin <= 0 || gpu_skip_draw) {
        ndrawn = 0;
        return;
    }
    uint64_t t0 = stats_on ? now_us() : 0;
    render_queue_sync();                /* (it is about to find the memory read-only) */
    Ctx saved = *c;
    gpu_state_put_aside();
    keep_memory();
    rt_shadow_extras_running = 1;
    nchunks = 0;
    uint32_t mgr = LW(RENDER_MANAGER);
    call(c, QUEUES_CLEAR, mgr, 0, 0, 0, 0);
    for (int i = 0; i < ndrawn; i++) {
        const Noted *n = &drawn[i];
        if (LW(n->object) != n->vtable)
            continue;                   /* (gone since it was noted) */
        if (n->add)
            call(c, n->add, n->mgr, n->layer, n->object, n->forced, 0);
        else
            call(c, INSERT, n->mgr, n->layer, n->object, n->object + 0x40, 0);
    }
    call(c, LAYER_STATES, mgr, 0, 0, 0, 0);
    call(c, RENDER_PASS, mgr, 0, 0, 0, 0);
    qsort(chunks, (size_t)nchunks, sizeof chunks[0], cmp_layer);
    for (int i = 0; i < nchunks; i++)
        if (chunks[i].count > 2)        /* (its last two words: the link onwards, the frame's list's) */
            ge_run_chunk(chunks[i].start & 0x0FFFFFFFu, (chunks[i].start & 0x0FFFFFFFu) + 4 * (chunks[i].count - 2));
    render_queue_sync();                /* (what is queued reads the memory as it is now) */
    rt_shadow_extras_running = 0;
    if (stats_on) {
        stat_frames++;
        stat_models += (unsigned long)ndrawn;
        stat_pages += ncopied;
        stat_chunks += (unsigned long)nchunks;
    }
    restore_memory();
    gpu_state_put_back();
    *c = saved;
    ndrawn = 0;
    if (stats_on) {
        stat_us += now_us() - t0;
        if (stat_frames % 300 == 0)
            fprintf(stderr, "shadow extras: %lu frames, %.1f models, %.1f pages, %.1f chunks, %.0f us a frame\n", stat_frames,
                    (double)stat_models / (double)stat_frames, (double)stat_pages / (double)stat_frames,
                    (double)stat_chunks / (double)stat_frames, (double)stat_us / (double)stat_frames);
    }
}
