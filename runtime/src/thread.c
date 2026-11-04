/* Threads, their synchronisation, timers and interrupts: the PSP kernel's scheduler on coroutines.
 *
 * Each PSP thread is a coroutine (ucontext) with its own host stack and CPU context, on one host thread. The highest-
 * priority ready thread runs (lower numbers first); a thread only stops running inside a kernel call - when it blocks,
 * or when the call readies a thread of higher priority. Interrupt handlers (vblank) and timer handlers run at kernel
 * calls and in the idle loop, on an interrupt context of their own, and may not block. See docs/runtime.md. */
#define _GNU_SOURCE
#include "hle.h"
#include "savestate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

enum { TS_RUNNING = 1, TS_READY = 2, TS_WAITING = 4, TS_SUSPEND = 8, TS_DORMANT = 16, TS_DEAD = 32 };
enum { WAIT_NONE, WAIT_SLEEP, WAIT_DELAY, WAIT_SEMA, WAIT_EVENTFLAG, WAIT_THREADEND };

enum { AFTER_WAIT = 1, AFTER_DELAY, AFTER_AUDIO };

#define HOST_STACK_SIZE ST_STACK_SIZE
#define THREAD_RETURN 0x08000000u      /* ra for a thread's entry: returning from it ends the thread */

typedef struct Thread {
    uint32_t uid;
    char name[32];
    uint32_t entry, init_prio, prio, attr, stack_size, stack, gp;
    int status;
    int wait_type;
    uint32_t wait_uid, wait_bits, wait_mode, wait_out, wait_timeout_ptr;
    int32_t wait_count;
    uint64_t wait_until;               /* 0: no timeout */
    uint32_t wait_result;
    int wakeup_count;
    uint32_t exit_status;
    int after;                         /* a save state's thread: what its kernel call has left to do after the wait */
    int after_callbacks;               /* ... and if it runs its callbacks then */
    uint64_t after_length;             /* ... and AFTER_AUDIO: the buffer's length */
    Ctx ctx;
    ucontext_t uc;
    void *host_stack;
    int has_coroutine;
    struct Thread *next;               /* all threads */
} Thread;

typedef struct {
    uint32_t uid;
    char name[32];
    uint32_t attr;
    int32_t count, max;
} Sema;

typedef struct {
    uint32_t uid;
    char name[32];
    uint32_t attr, bits;
} EventFlag;

typedef struct Callback {
    uint32_t uid;
    char name[32];
    uint32_t func, arg;
    struct Thread *owner;              /* callbacks run on the thread that created them */
    int count;                         /* notifications pending */
    uint32_t notify_arg;
    struct Callback *next;
} Callback;

SAVED static Callback *callbacks;
static void run_callbacks(void);

typedef struct {
    uint32_t uid;
    char name[32];
    int running;
    uint64_t base;                     /* host time it started from (minus its accumulated time) */
    uint64_t schedule;                 /* its time at which the handler runs (0: at once) */
    int armed;                         /* a handler is set and has a time to run */
    uint32_t handler, common;
} VTimer;

SAVED static Thread *threads;
SAVED static Thread *current;
static ucontext_t sched_uc;
SAVED static uint32_t module_gp;
SAVED static int dispatch_enabled = 1;
SAVED static int intr_enabled = 1;
SAVED static int in_interrupt;
SAVED static uint64_t next_vblank;
SAVED static uint64_t vblank_count;
/* the schedule since the rate was last set: vblank number base_count + 1 comes at base_us */
SAVED static uint64_t vblank_base_us, vblank_base_count;
SAVED static unsigned long tick_switch_frame;                 /* MHFU_TICK_HZ=N@frame: the rate from that game frame */
SAVED static int tick_switch_hz;

#define MAX_VTIMERS 16
SAVED static VTimer *vtimers[MAX_VTIMERS];

/* -- time --------------------------------------------------------------------------------------------------------- */

/* The game's clock: virtual, so that a run is the same every time for the same input (MHFU_RECORD / MHFU_REPLAY). It
 * stands still while the game runs and moves only when every thread waits - to what they wait for (a vblank, a timer, a
 * timeout), when the wall clock gets there; fast-forwarding, at once. A slow host slows the game rather than making it
 * skip. Reading it from the game moves it a microsecond, so that waiting by reading the clock still ends. */
SAVED float rt_time_scale = 1;
SAVED int rt_tick30 = 1;
SAVED int rt_ticks60 = 2;
SAVED int rt_frame30 = 1;
SAVED static float tick_scale = 1;        /* 30 / the game's frames a second */
SAVED static float tick30_phase;           /* 30ths of a second gone by, since the last whole one */
SAVED static uint64_t now_us;
static uint64_t wall_base;          /* the wall clock (us) when the game's clock was 0, for pacing */
SAVED static uint64_t epoch_us;           /* the date and time (us since 1970) when the game's clock was 0 */
int fast_forward(void);

static uint64_t wall_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
}

uint64_t wall_time_us(void) { return wall_us(); }

/* How far the game's clock is behind the wall clock (us; negative: ahead) - the game running slower than it should */
int64_t clock_behind_us(void) {
    return fast_forward() ? 0 : (int64_t)wall_us() - (int64_t)(wall_base + now_us);
}

static uint64_t slept_us;           /* for MHFU_FPS_STATS: how long the clock waited for the wall clock */

uint64_t time_slept_us(void) {
    uint64_t s = slept_us;
    slept_us = 0;
    return s;
}

uint64_t time_us(void) {
    return now_us;
}

uint64_t time_read_us(void) {
    return ++now_us;
}

/* The date and time, for the game's real-time clock: from the run's start (recorded, so a replay has the same) */
uint64_t replay_epoch(void);

uint64_t epoch_now_us(void) {
    if (!epoch_us)
        epoch_us = replay_epoch();
    if (!epoch_us) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        epoch_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
    }
    return epoch_us + now_us;
}

uint64_t run_epoch_us(void) {
    return epoch_now_us() - now_us;
}

/* Every thread waits: move the clock to `until`, once the wall clock is there (unless fast-forwarding). Ahead or behind
 * by more than a quarter of a second - after fast-forwarding, or a stall - the pacing starts again from now. */
static void advance_to(uint64_t until) {
    if (until <= now_us)                /* due, but not run (interrupts off): let time go on */
        until = now_us + 1;
    if (!fast_forward()) {
        uint64_t wall = wall_us();
        int64_t ahead = (int64_t)(wall_base + until) - (int64_t)wall;
        if (ahead > 250000 || ahead < -250000) {
            wall_base = wall - until;
            ahead = 0;
        }
        if (ahead > 0) {
            struct timespec ts = {(time_t)(ahead / 1000000), (long)(ahead % 1000000) * 1000};
            nanosleep(&ts, NULL);
            slept_us += (uint64_t)ahead;
        }
    }
    now_us = until;
}

#define VBLANK_US_NUM 1001000u        /* 59.94 Hz: a vblank every 1001000 / 60 us */
SAVED static unsigned vblank_den = 60;      /* MHFU_VBLANK_HZ (an experiment): vblanks a second, times 1.001 */
#define VBLANK_US_DEN vblank_den

uint32_t kernel_gp(void) { return module_gp; }

/* The game's frames hz a second: its vblank handler wakes it every second vblank, so the vblanks come 2 hz a second
 * (from the next, the schedule starting again), and what the patches scale goes by 30 / hz */
void set_tick_hz(int hz) {
    vblank_base_us = next_vblank;
    vblank_base_count = vblank_count;
    vblank_den = (unsigned)(2 * hz);
    tick_scale = 30.0f / (float)hz;
    tick30_phase = 1 - tick_scale;      /* the next frame a 30th's (as the frame before took it to be) */
}

void native_frame_start(void);

/* The game's main loop, each game frame, as it wakes (patches.py): whether a 30th falls on this frame - if, with it,
 * another whole 30th has gone by - and the time scale it runs at */
void rt_frame_start(void) {
    rt_ticks60 = (int)floorf(2 * (tick30_phase + tick_scale) + 0.002f) - (int)floorf(2 * tick30_phase + 0.002f);
    tick30_phase += tick_scale;
    rt_frame30 = tick30_phase >= 0.999f;
    if (rt_frame30)
        tick30_phase -= 1;
    rt_time_scale = tick_scale;
    rt_tick30 = rt_frame30;
    native_frame_start();
}

/* Whether the next game frame is one a 30th falls on (native.c) */
int rt_next_frame30(void) {
    return tick30_phase + tick_scale >= 0.999f;
}

/* The game's time in 30ths of a second (its frames, at its own rate), as of the last frame presented; and the 30ths a
 * frame (the renderer's clocks: render_scene.c, gpu.c) */
SAVED static double game_30ths;

double rt_game_30ths(void) {
    return game_30ths;
}

float rt_frame_30ths(void) {
    return tick_scale;
}

/* present.c, each game frame: the game's time on; the rate switch of MHFU_TICK_HZ=N@frame */
void tick_frame(unsigned long frame) {
    game_30ths += tick_scale;
    if (tick_switch_frame && frame == tick_switch_frame) {
        set_tick_hz(tick_switch_hz);
        fprintf(stderr, "frame %lu: %d game frames a second\n", frame, tick_switch_hz);
    }
}

/* -- interrupts --------------------------------------------------------------------------------------------------- */

#define MAX_SUBINTR 16
SAVED static struct { uint32_t handler, arg; int enabled; } subintr[64][MAX_SUBINTR];
SAVED static Ctx intr_ctx;
SAVED static uint32_t intr_stack_top;

void interrupt_call(uint32_t handler, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0, uint32_t t1,
                    uint32_t t2) {
    Ctx *c = &intr_ctx;
    int was = in_interrupt;
    in_interrupt = 1;
    c->mem = rt_mem;
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    c->r[8] = t0; c->r[9] = t1; c->r[10] = t2;
    c->r[28] = module_gp;
    c->r[29] = intr_stack_top;
    c->r[31] = THREAD_RETURN;
    rt_call(c, handler);
    in_interrupt = was;
}

void intr_register(int intr, int sub, uint32_t handler, uint32_t arg) {
    subintr[intr][sub].handler = handler;
    subintr[intr][sub].arg = arg;
}

static void raise_interrupt(int intr) {
    for (int s = 0; s < MAX_SUBINTR; s++)
        if (subintr[intr][s].handler && subintr[intr][s].enabled)
            interrupt_call(subintr[intr][s].handler, (uint32_t)s, subintr[intr][s].arg, 0, 0, 0, 0, 0);
}

static void wake(Thread *t, uint32_t result);

/* Everything due by now: vblanks, timer handlers, timed-out waits. */
static void process_events(void) {
    if (in_interrupt)
        return;
    uint64_t now = time_us();
    if (intr_enabled) {
        while (now >= next_vblank) {
            vblank_count++;
            next_vblank = vblank_base_us + (vblank_count - vblank_base_count) * VBLANK_US_NUM / VBLANK_US_DEN;
            raise_interrupt(PSP_VBLANK_INT);
        }
        for (int i = 0; i < MAX_VTIMERS; i++) {
            VTimer *v = vtimers[i];
            if (!v || !v->running || !v->armed || !v->handler)
                continue;
            uint64_t vt = now - v->base;
            if (vt < v->schedule)
                continue;
            /* handler(uid, &scheduled time, &actual time, common): the times are 64-bit, passed by pointer (in a
             * scratch area below the interrupt stack) */
            uint64_t sched = v->schedule;
            uint32_t args = intr_stack_top + 0x20;
            SW(args, (uint32_t)sched);
            SW(args + 4, (uint32_t)(sched >> 32));
            SW(args + 8, (uint32_t)vt);
            SW(args + 12, (uint32_t)(vt >> 32));
            interrupt_call(v->handler, v->uid, args, args + 8, v->common, 0, 0, 0);
            uint32_t next = intr_ctx.r[2];          /* the time to the next run, or 0 to stop */
            v->schedule = sched + next;
            v->armed = next != 0;
        }
    }
    for (Thread *t = threads; t; t = t->next)
        if (t->status == TS_WAITING && t->wait_until && now >= t->wait_until)
            wake(t, t->wait_type == WAIT_DELAY ? 0 : SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

static uint64_t next_event(void) {
    uint64_t next = next_vblank;
    for (Thread *t = threads; t; t = t->next)
        if (t->status == TS_WAITING && t->wait_until && t->wait_until < next)
            next = t->wait_until;
    for (int i = 0; i < MAX_VTIMERS; i++) {
        VTimer *v = vtimers[i];
        if (v && v->running && v->armed && v->handler && v->base + v->schedule < next)
            next = v->base + v->schedule;
    }
    return next;
}

/* -- the scheduler ------------------------------------------------------------------------------------------------ */

static Thread *find_thread(uint32_t uid) {
    if (uid == 0)
        return current;
    return kobj_get(uid, OBJ_THREAD);
}

static Thread *highest_ready(void) {
    Thread *best = NULL;
    for (Thread *t = threads; t; t = t->next)
        if (t->status == TS_READY && (!best || t->prio < best->prio))
            best = t;
    return best;
}

/* Back to the scheduler (from the current thread), until it picks this thread again. */
static void reschedule(void) {
    Thread *t = current;
    swapcontext(&t->uc, &sched_uc);
}

/* A wait over: the timeout's time left written back, and its result */
static uint32_t wait_over(Thread *t) {
    if (t->wait_timeout_ptr) {
        uint64_t now = time_us();
        SW(t->wait_timeout_ptr, t->wait_until > now ? (uint32_t)(t->wait_until - now) : 0);
    }
    return t->wait_result;
}

/* Block the current thread; returns the wait's result (0, or an error such as a timeout) once woken. */
static uint32_t wait_current(int type, uint32_t uid, uint32_t timeout_ptr) {
    Thread *t = current;
    t->status = TS_WAITING;
    t->wait_type = type;
    t->wait_uid = uid;
    t->wait_result = 0;
    t->wait_timeout_ptr = timeout_ptr;
    t->wait_until = timeout_ptr ? time_us() + LW(timeout_ptr) : 0;
    t->after = AFTER_WAIT;
    reschedule();
    return wait_over(t);
}

static void wake(Thread *t, uint32_t result) {
    if (t->status != TS_WAITING)
        return;
    t->status = TS_READY;
    t->wait_type = WAIT_NONE;
    t->wait_result = result;
}

/* After a call that may have readied a thread of higher priority: let it run. */
static void maybe_yield(void) {
    if (in_interrupt || !dispatch_enabled || !current)
        return;
    Thread *best = highest_ready();
    if (best && best->prio < current->prio) {
        current->status = TS_READY;
        reschedule();
    }
}

void hle_after_call(Ctx *c) {
    (void)c;
    if (in_interrupt)
        return;
    process_events();
    maybe_yield();
}

Ctx *thread_ctx(void) { return current ? &current->ctx : NULL; }

static void thread_end(Thread *t, uint32_t status);

static void thread_main(void) {
    Thread *t = current;
    rt_call(&t->ctx, t->entry);
    thread_end(t, t->ctx.r[2]);
    /* back to the scheduler - itself, not by returning to glibc's trampoline (uc_link), whose place changes from run to
     * run (a save state's stacks have it: savestate.c) */
    setcontext(&sched_uc);
}

static void make_coroutine(Thread *t, void (*entry)(void)) {
    if (!t->host_stack) {
        t->host_stack = st_stack_alloc();
    }
    getcontext(&t->uc);
    t->uc.uc_stack.ss_sp = t->host_stack;
    t->uc.uc_stack.ss_size = HOST_STACK_SIZE;
    t->uc.uc_link = &sched_uc;
    makecontext(&t->uc, entry, 0);
    t->has_coroutine = 1;
}

/* A thread of a save state, woken: what its kernel call had left to do, and the game's code from where the call returns
 * to. A state holds no host stack: a thread that waits is its registers, its wait and t->after. */
static void thread_resume_main(void) {
    Thread *t = current;
    Ctx *c = &t->ctx;
    switch (t->after) {
    case AFTER_WAIT:
        RET(wait_over(t));
        break;
    case AFTER_DELAY:
        RET(0);
        break;
    case AFTER_AUDIO:
        audio_output_finish(c, t->after_length);
        break;
    default:
        rt_fatal(c, "a save state's thread %s: what it waited in is not known", t->name);
    }
    if (t->after_callbacks) {
        t->after_callbacks = 0;
        run_callbacks();
    }
    hle_after_call(c);
    rt_resume_run(c, THREAD_RETURN);
    thread_end(t, c->r[2]);
    setcontext(&sched_uc);
}

/* A state loaded: no thread has a host stack now; each that waits gets one, to go on from its wait */
void threads_state_loaded(void) {
    st_stacks_reset();
    intr_ctx.mem = rt_mem;
    for (Thread *t = threads; t; t = t->next) {
        t->ctx.mem = rt_mem;
        t->host_stack = NULL;
        t->has_coroutine = 0;
        if (t->status == TS_WAITING)
            make_coroutine(t, thread_resume_main);
    }
}

/* What a state's arena depends on (savestate.c) */
uint64_t thread_layout(void) {
    return ((uint64_t)sizeof(Thread) << 32) ^ ((uint64_t)sizeof(Sema) << 24) ^ ((uint64_t)sizeof(EventFlag) << 16) ^
           ((uint64_t)sizeof(Callback) << 8) ^ sizeof(VTimer);
}

void scheduler_run(void) {
    for (;;) {
        process_events();
        Thread *t = highest_ready();
        if (!t) {
            if (state_point())              /* (every thread waits: a save state saved or loaded here) */
                continue;
            uint64_t next = next_event(), display = present_pump();
            advance_to(display < next ? display : next);
            continue;
        }
        current = t;
        t->status = TS_RUNNING;
        if (!t->has_coroutine)
            make_coroutine(t, thread_main);
        swapcontext(&sched_uc, &t->uc);
        if (current && current->status == TS_RUNNING)
            current->status = TS_READY;
        current = NULL;
    }
}

/* -- thread creation ---------------------------------------------------------------------------------------------- */

static Thread *new_thread(const char *name, uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t attr) {
    Thread *t = st_calloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->entry = entry;
    t->init_prio = t->prio = prio;
    t->attr = attr;
    t->stack_size = (stack_size + 0xFF) & ~0xFFu;
    t->stack = kmem_alloc(t->stack_size, 1, 0x100, 0);
    t->gp = module_gp;
    t->status = TS_DORMANT;
    t->uid = kobj_new(OBJ_THREAD, t);
    t->next = threads;
    threads = t;
    return t;
}

/* Reset a dormant thread's context to start at its entry, with its arguments copied to the top of its stack. */
static void start_thread(Thread *t, uint32_t argsize, const void *argp) {
    Ctx *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->mem = rt_mem;
    for (int i = 0; i < 3; i++)
        c->vfpu_ctrl[i] = i < 2 ? 0xE4 : 0;
    /* k0: a block at the top of the stack, as the PSP's kernel lays it out */
    uint32_t top = t->stack + t->stack_size;
    uint32_t k0 = top - 0x100;
    memset(MEM(k0), 0, 0x100);
    SW(k0 + 0xC0, t->uid);
    SW(k0 + 0xC8, t->stack);
    SW(k0 + 0xF8, 0xFFFFFFFFu);
    SW(k0 + 0xFC, 0xFFFFFFFFu);
    uint32_t sp = k0;
    c->r[4] = argsize;
    if (argp && argsize) {
        sp -= (argsize + 0xF) & ~0xFu;
        memcpy(MEM(sp), argp, argsize);
        c->r[5] = sp;
    }
    c->r[26] = k0;
    c->r[28] = t->gp;
    c->r[29] = sp - 0x40;
    c->r[31] = THREAD_RETURN;
    t->prio = t->init_prio;
    t->wakeup_count = 0;
    t->status = TS_READY;
    t->has_coroutine = 0;
}

uint32_t thread_create_main(Ctx *proto, uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t argsize,
                            const void *argp) {
    module_gp = proto->r[28];
    Thread *t = new_thread("root", entry, prio, stack_size, 0x80000000u);
    start_thread(t, argsize, argp);
    return t->uid;
}

/* MHFU_THREADS=1: at exit, each thread's state and what it waits on */
static void dump_threads(void) {
    static const char *waits[] = {"", "sleep", "delay", "semaphore", "event flag", "thread end"};
    for (Thread *t = threads; t; t = t->next)
        fprintf(stderr, "thread %-24s prio %3u %s%s %08X pc~ra %08X\n", t->name, t->prio,
                t->status == TS_WAITING ? "waiting: " : t->status == TS_READY ? "ready" :
                t->status == TS_RUNNING ? "running" : "dormant", t->status == TS_WAITING ? waits[t->wait_type] : "",
                t->wait_uid, t->ctx.r[31]);
}

void threads_init(void) {
    if (getenv("MHFU_VBLANK_HZ"))
        vblank_den = (unsigned)atoi(getenv("MHFU_VBLANK_HZ"));
    /* MHFU_TICK_HZ=N[@frame] (an experiment, towards the native frame rate): the game's frames N a second (from that
     * game frame: getting somewhere at 30, by a recording, and carrying on at N) - see set_tick_hz */
    if (getenv("MHFU_TICK_HZ")) {
        const char *e = getenv("MHFU_TICK_HZ"), *at = strchr(e, '@');
        int hz = atoi(e);
        if (hz >= 30 && hz <= 480) {
            if (at)
                tick_switch_frame = strtoul(at + 1, NULL, 10), tick_switch_hz = hz;
            else
                set_tick_hz(hz);
        }
    }
    if (getenv("MHFU_THREADS")) {
        atexit(dump_threads);
        at_quick_exit(dump_threads);
    }
    intr_stack_top = kmem_alloc(0x4000, 1, 0x100, 0) + 0x4000 - 0x40;
    next_vblank = vblank_base_us = VBLANK_US_NUM / VBLANK_US_DEN;
    time_us();
}

static void thread_end(Thread *t, uint32_t status) {
    t->status = TS_DORMANT;
    t->exit_status = status;
    for (Thread *w = threads; w; w = w->next)
        if (w->status == TS_WAITING && w->wait_type == WAIT_THREADEND && w->wait_uid == t->uid)
            wake(w, 0);
}

static void thread_delete(Thread *t) {
    kobj_free(t->uid);
    for (Thread **p = &threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    kmem_free(t->stack);
    /* the host stack of a thread that is running is freed by nothing: it is the one we are on */
    if (t != current && t->host_stack)
        st_stack_free(t->host_stack);
    if (t != current)
        st_free(t);
}

/* -- HLE: threads ------------------------------------------------------------------------------------------------- */

void hle_sceKernelCreateThread(Ctx *c) {
    const char *name = hle_string(c, ARG0);
    uint32_t prio = ARG2;
    if (prio < 1 || prio > 127) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    Thread *t = new_thread(name, ARG1, prio, ARG3 < 0x200 ? 0x200 : ARG3, ARG4);
    t->gp = c->r[28];
    HLE_LOG("%u=sceKernelCreateThread(%s, %08X, %u, %X)", t->uid, name, ARG1, prio, ARG3);
    RET(t->uid);
}

void hle_sceKernelStartThread(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    if (t->status != TS_DORMANT) {
        RET(SCE_KERNEL_ERROR_NOT_DORMANT);
        return;
    }
    start_thread(t, ARG1, ARG2 ? PTR(ARG2) : NULL);
    HLE_LOG("sceKernelStartThread(%s)", t->name);
    RET(0);
    maybe_yield();
}

void hle_sceKernelExitThread(Ctx *c) {
    thread_end(current, ARG0);
    reschedule();               /* never comes back */
}

void hle_sceKernelExitDeleteThread(Ctx *c) {
    Thread *t = current;
    thread_end(t, ARG0);
    thread_delete(t);
    reschedule();
}

void hle_sceKernelDeleteThread(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    if (t->status != TS_DORMANT) {
        RET(SCE_KERNEL_ERROR_NOT_DORMANT);
        return;
    }
    thread_delete(t);
    RET(0);
}

void hle_sceKernelTerminateThread(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t || t == current) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    thread_end(t, 0x800201ACu);
    RET(0);
}

void hle_sceKernelTerminateDeleteThread(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t || t == current) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    thread_end(t, 0x800201ACu);
    thread_delete(t);
    RET(0);
}

void hle_sceKernelGetThreadId(Ctx *c) { RET(current->uid); }

void hle_sceKernelGetThreadExitStatus(Ctx *c) {
    Thread *t = find_thread(ARG0);
    RET(!t ? SCE_KERNEL_ERROR_UNKNOWN_THID : t->status == TS_DORMANT ? t->exit_status : SCE_KERNEL_ERROR_NOT_DORMANT);
}

void hle_sceKernelGetThreadCurrentPriority(Ctx *c) { RET(current->prio); }

void hle_sceKernelChangeThreadPriority(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    uint32_t prio = ARG1 ? ARG1 : current->prio;
    if (prio < 1 || prio > 127) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    t->prio = prio;
    RET(0);
    maybe_yield();
}

void hle_sceKernelChangeCurrentThreadAttr(Ctx *c) {
    current->attr = (current->attr & ~ARG0) | ARG1;
    RET(0);
}

void hle_sceKernelDelayThread(Ctx *c) {
    uint32_t us = ARG0 < 200 ? 200 : ARG0;      /* the kernel does not delay for less */
    current->status = TS_WAITING;
    current->wait_type = WAIT_DELAY;
    current->wait_until = time_us() + us;
    current->wait_timeout_ptr = 0;
    current->after = AFTER_DELAY;
    reschedule();
    RET(0);
}

/* A kernel call that runs the thread's callbacks before and after its wait */
static void with_callbacks(Ctx *c, void (*call)(Ctx *c)) {
    Thread *t = current;
    run_callbacks();
    t->after_callbacks = 1;
    call(c);
    t->after_callbacks = 0;
    run_callbacks();
}

void hle_sceKernelDelayThreadCB(Ctx *c) { with_callbacks(c, hle_sceKernelDelayThread); }

void thread_delay_until(uint64_t us, uint64_t audio_length) {
    if (in_interrupt || !current || us <= time_us())
        return;
    current->status = TS_WAITING;
    current->wait_type = WAIT_DELAY;
    current->wait_until = us;
    current->wait_timeout_ptr = 0;
    current->after = AFTER_AUDIO;       /* (audio.c's the only caller: a state's thread goes on with what it does next) */
    current->after_length = audio_length;
    reschedule();
}

void hle_sceKernelSleepThread(Ctx *c) {
    if (current->wakeup_count > 0) {
        current->wakeup_count--;
        RET(0);
        return;
    }
    RET(wait_current(WAIT_SLEEP, 0, 0));
}

void hle_sceKernelSleepThreadCB(Ctx *c) { with_callbacks(c, hle_sceKernelSleepThread); }

void hle_sceKernelWakeupThread(Ctx *c) {
    Thread *t = find_thread(ARG0);
    if (!t) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
        return;
    }
    if (t->status == TS_WAITING && t->wait_type == WAIT_SLEEP)
        wake(t, 0);
    else
        t->wakeup_count++;
    RET(0);
    maybe_yield();
}

void hle_sceKernelSuspendDispatchThread(Ctx *c) {
    if (!intr_enabled) {
        RET(0x80020064u);       /* SCE_KERNEL_ERROR_CPUDI */
        return;
    }
    RET(dispatch_enabled);
    dispatch_enabled = 0;
}

void hle_sceKernelResumeDispatchThread(Ctx *c) {
    if (!intr_enabled) {
        RET(0x80020064u);
        return;
    }
    dispatch_enabled = ARG0 != 0;
    RET(0);
    maybe_yield();
}

/* -- HLE: interrupts ---------------------------------------------------------------------------------------------- */

void hle_sceKernelCpuSuspendIntr(Ctx *c) {
    RET(intr_enabled);
    intr_enabled = 0;
}

void hle_sceKernelCpuResumeIntr(Ctx *c) {
    intr_enabled = ARG0 != 0;
    RET(0);
}

void hle_sceKernelRegisterSubIntrHandler(Ctx *c) {
    if (ARG0 >= 64 || ARG1 >= MAX_SUBINTR) {
        RET(0x80020065u);
        return;
    }
    intr_register((int)ARG0, (int)ARG1, ARG2, ARG3);
    RET(0);
}

void hle_sceKernelReleaseSubIntrHandler(Ctx *c) {
    if (ARG0 < 64 && ARG1 < MAX_SUBINTR)
        subintr[ARG0][ARG1].handler = 0;
    RET(0);
}

void hle_sceKernelEnableSubIntr(Ctx *c) {
    if (ARG0 < 64 && ARG1 < MAX_SUBINTR)
        subintr[ARG0][ARG1].enabled = 1;
    RET(0);
}

/* -- HLE: semaphores ---------------------------------------------------------------------------------------------- */

void hle_sceKernelCreateSema(Ctx *c) {
    Sema *s = st_calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", hle_string(c, ARG0));
    s->attr = ARG1;
    s->count = (int32_t)ARG2;
    s->max = (int32_t)ARG3;
    s->uid = kobj_new(OBJ_SEMA, s);
    RET(s->uid);
}

static void sema_wake(Sema *s) {
    for (;;) {
        Thread *best = NULL;
        for (Thread *t = threads; t; t = t->next)
            if (t->status == TS_WAITING && t->wait_type == WAIT_SEMA && t->wait_uid == s->uid &&
                t->wait_count <= s->count && (!best || ((s->attr & 0x100) && t->prio < best->prio)))
                best = t;
        if (!best)
            return;
        s->count -= best->wait_count;
        wake(best, 0);
    }
}

void hle_sceKernelDeleteSema(Ctx *c) {
    Sema *s = kobj_get(ARG0, OBJ_SEMA);
    if (!s) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
        return;
    }
    for (Thread *t = threads; t; t = t->next)
        if (t->status == TS_WAITING && t->wait_type == WAIT_SEMA && t->wait_uid == s->uid)
            wake(t, SCE_KERNEL_ERROR_WAIT_DELETE);
    kobj_free(s->uid);
    st_free(s);
    RET(0);
    maybe_yield();
}

void hle_sceKernelSignalSema(Ctx *c) {
    Sema *s = kobj_get(ARG0, OBJ_SEMA);
    if (!s) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
        return;
    }
    if (s->count + (int32_t)ARG1 > s->max) {
        RET(SCE_KERNEL_ERROR_SEMA_OVF);
        return;
    }
    s->count += (int32_t)ARG1;
    sema_wake(s);
    RET(0);
    maybe_yield();
}

static void wait_sema(Ctx *c) {
    Sema *s = kobj_get(ARG0, OBJ_SEMA);
    if (!s) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
        return;
    }
    int32_t n = (int32_t)ARG1;
    if (s->count >= n) {
        s->count -= n;
        RET(0);
        return;
    }
    current->wait_count = n;
    RET(wait_current(WAIT_SEMA, s->uid, ARG2));
}

void hle_sceKernelWaitSema(Ctx *c) { wait_sema(c); }
void hle_sceKernelWaitSemaCB(Ctx *c) { with_callbacks(c, wait_sema); }

void hle_sceKernelPollSema(Ctx *c) {
    Sema *s = kobj_get(ARG0, OBJ_SEMA);
    if (!s) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
        return;
    }
    if (s->count >= (int32_t)ARG1) {
        s->count -= (int32_t)ARG1;
        RET(0);
    } else {
        RET(SCE_KERNEL_ERROR_SEMA_ZERO);
    }
}

/* -- HLE: event flags --------------------------------------------------------------------------------------------- */

#define EVF_OR 1
#define EVF_CLEARALL 0x10
#define EVF_CLEAR 0x20

static int evf_match(EventFlag *e, uint32_t bits, uint32_t mode) {
    return (mode & EVF_OR) ? (e->bits & bits) != 0 : (e->bits & bits) == bits;
}

static void evf_take(EventFlag *e, uint32_t bits, uint32_t mode, uint32_t out) {
    if (out)
        SW(out, e->bits);
    if (mode & EVF_CLEARALL)
        e->bits = 0;
    else if (mode & EVF_CLEAR)
        e->bits &= ~bits;
}

void hle_sceKernelCreateEventFlag(Ctx *c) {
    EventFlag *e = st_calloc(1, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", hle_string(c, ARG0));
    e->attr = ARG1;
    e->bits = ARG2;
    e->uid = kobj_new(OBJ_EVENTFLAG, e);
    RET(e->uid);
}

void hle_sceKernelDeleteEventFlag(Ctx *c) {
    EventFlag *e = kobj_get(ARG0, OBJ_EVENTFLAG);
    if (!e) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
        return;
    }
    for (Thread *t = threads; t; t = t->next)
        if (t->status == TS_WAITING && t->wait_type == WAIT_EVENTFLAG && t->wait_uid == e->uid)
            wake(t, SCE_KERNEL_ERROR_WAIT_DELETE);
    kobj_free(e->uid);
    st_free(e);
    RET(0);
    maybe_yield();
}

void hle_sceKernelSetEventFlag(Ctx *c) {
    EventFlag *e = kobj_get(ARG0, OBJ_EVENTFLAG);
    if (!e) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
        return;
    }
    e->bits |= ARG1;
    for (Thread *t = threads; t; t = t->next) {
        if (t->status == TS_WAITING && t->wait_type == WAIT_EVENTFLAG && t->wait_uid == e->uid &&
            evf_match(e, t->wait_bits, t->wait_mode)) {
            evf_take(e, t->wait_bits, t->wait_mode, t->wait_out);
            wake(t, 0);
        }
    }
    RET(0);
    maybe_yield();
}

void hle_sceKernelClearEventFlag(Ctx *c) {
    EventFlag *e = kobj_get(ARG0, OBJ_EVENTFLAG);
    if (!e) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
        return;
    }
    e->bits &= ARG1;
    RET(0);
}

static void wait_evf(Ctx *c) {
    EventFlag *e = kobj_get(ARG0, OBJ_EVENTFLAG);
    if (!e) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
        return;
    }
    if (evf_match(e, ARG1, ARG2)) {
        evf_take(e, ARG1, ARG2, ARG3);
        RET(0);
        return;
    }
    current->wait_bits = ARG1;
    current->wait_mode = ARG2;
    current->wait_out = ARG3;
    RET(wait_current(WAIT_EVENTFLAG, e->uid, ARG4));
}

void hle_sceKernelWaitEventFlag(Ctx *c) { wait_evf(c); }
void hle_sceKernelWaitEventFlagCB(Ctx *c) { with_callbacks(c, wait_evf); }

void hle_sceKernelPollEventFlag(Ctx *c) {
    EventFlag *e = kobj_get(ARG0, OBJ_EVENTFLAG);
    if (!e) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
        return;
    }
    if (evf_match(e, ARG1, ARG2)) {
        evf_take(e, ARG1, ARG2, ARG3);
        RET(0);
    } else {
        if (ARG3)
            SW(ARG3, e->bits);
        RET(SCE_KERNEL_ERROR_EVF_COND);
    }
}

/* -- HLE: callbacks --------------------------------------------------------------------------------------------- */

/* Callbacks are notified by the system (the Memory Stick being inserted, say) and run on the thread that created
 * them, when it waits in one of the kernel's ...CB calls: func(notifications, argument, common argument). */

void hle_sceKernelCreateCallback(Ctx *c) {
    Callback *cb = st_calloc(1, sizeof *cb);
    snprintf(cb->name, sizeof cb->name, "%s", hle_string(c, ARG0));
    cb->func = ARG1;
    cb->arg = ARG2;
    cb->owner = current;
    cb->uid = kobj_new(OBJ_CALLBACK, cb);
    cb->next = callbacks;
    callbacks = cb;
    RET(cb->uid);
}

void callback_notify(uint32_t uid, uint32_t arg) {
    Callback *cb = kobj_get(uid, OBJ_CALLBACK);
    if (!cb)
        return;
    cb->count++;
    cb->notify_arg = arg;
}

/* Run the current thread's pending callbacks, on its own context (saved around them) */
static void run_callbacks(void) {
    if (in_interrupt || !current)
        return;
    for (Callback *cb = callbacks; cb; cb = cb->next) {
        if (cb->owner != current || !cb->count)
            continue;
        int n = cb->count;
        cb->count = 0;
        Ctx *c = &current->ctx;
        Ctx saved = *c;
        c->r[4] = (uint32_t)n;
        c->r[5] = cb->notify_arg;
        c->r[6] = cb->arg;
        c->r[29] -= 0x40;
        c->r[31] = THREAD_RETURN;
        rt_no_resume++;
        rt_call(c, cb->func);
        rt_no_resume--;
        *c = saved;
    }
}

/* -- HLE: time and virtual timers --------------------------------------------------------------------------------- */

void hle_sceKernelGetSystemTimeLow(Ctx *c) { RET((uint32_t)time_read_us()); }
void hle_sceKernelGetSystemTimeWide(Ctx *c) { RET64(time_read_us()); }

void hle_sceKernelSysClock2USecWide(Ctx *c) {
    uint64_t clock = ARG0 | ((uint64_t)ARG1 << 32);
    if (ARG2)
        SW(ARG2, (uint32_t)(clock / 1000000));
    if (ARG3)
        SW(ARG3, (uint32_t)(clock % 1000000));
    RET(0);
}

void hle_sceKernelCreateVTimer(Ctx *c) {
    VTimer *v = st_calloc(1, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", hle_string(c, ARG0));
    v->uid = kobj_new(OBJ_VTIMER, v);
    for (int i = 0; i < MAX_VTIMERS; i++)
        if (!vtimers[i]) {
            vtimers[i] = v;
            break;
        }
    RET(v->uid);
}

void hle_sceKernelStartVTimer(Ctx *c) {
    VTimer *v = kobj_get(ARG0, OBJ_VTIMER);
    if (!v) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    if (v->running) {
        RET(1);
        return;
    }
    v->running = 1;
    v->base = time_us();
    RET(0);
}

/* (uid, time: a2:a3, handler, common) - the handler runs when the timer reaches time */
void hle_sceKernelSetVTimerHandlerWide(Ctx *c) {
    VTimer *v = kobj_get(ARG0, OBJ_VTIMER);
    if (!v) {
        RET(SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    v->schedule = ARG2 | ((uint64_t)ARG3 << 32);
    v->handler = ARG4;
    v->common = ARG5;
    v->armed = ARG4 != 0;
    RET(0);
}
