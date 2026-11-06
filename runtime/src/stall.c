/* The stall recorder. MHFU_STALL=ms (stall in settings.ini; off by default): when frames shown are further apart than that,
 * a record in MHFU_STALL_LOG, or stalls.log in the captures' folder (tools/stalls.py names it):
 *
 *   S <seconds since start> <gap ms> <game frame> <ms on the CPU of the gap> <page faults> <major> <waits> <preempted>
 *   P <thread: 0 game, 1 render> <ms into the gap> <what: t a tick, d a display frame, o other> <address> ...   innermost first
 *
 * Nothing runs at a frame but two clock reads and getrusage. A watchdog thread looks every 2 ms: once a frame is past 40%
 * of the limit it samples the threads' stacks by a signal until the frame comes, and writes the record itself, so
 * writing it doesn't add to what it records. The samples begin late, so a gap's start isn't in them.
 *
 * Reads only: nothing of the game's is written, and the game's clock doesn't see it. */
#define _GNU_SOURCE
#include "stall.h"
#include "capture.h"

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static char log_path[1024];

/* (capture.c) the log, if it's on */
const char *stall_log_path(void) { return log_path[0] ? log_path : NULL; }

#define STALL_DEPTH 32
#define STALL_SAMPLES 512
#define STALL_THREADS 2

extern volatile int present_pumping;

typedef struct {
    uint64_t at_us;
    uint8_t thread, what;
    uint8_t depth;
    void *pc[STALL_DEPTH];
} Sample;

static Sample samples[STALL_SAMPLES];
static atomic_int nsamples;
static atomic_int sampling;

static int limit_us = -1;
static pthread_t threads[STALL_THREADS];
static atomic_int thread_known[STALL_THREADS];
static _Thread_local int my_thread = -1;

static atomic_uint_fast64_t last_beat_us;
static uint64_t started_us;
static struct rusage last_usage;

/* The record at hand, from the frame after the gap to the watchdog */
static struct {
    atomic_int ready;
    uint64_t from_us, gap_us, cpu_us;
    unsigned long frame;
    long faults, major, waits, preempted;
} record;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

#define STALL_SIGNAL (SIGRTMIN + 5)

static void on_signal(int sig) {
    (void)sig;
    if (my_thread < 0 || !atomic_load(&sampling))
        return;
    int i = atomic_fetch_add(&nsamples, 1);
    if (i >= STALL_SAMPLES)
        return;
    Sample *s = &samples[i];
    s->depth = 0;
    s->at_us = now_us();
    s->thread = (uint8_t)my_thread;
    s->what = my_thread ? 'o' : present_pumping ? 'd' : 't';
    int depth = backtrace(s->pc, STALL_DEPTH);
    s->depth = (uint8_t)(depth > 0 ? depth : 0);
}

static void write_record(FILE *f) {
    fprintf(f, "S %.3f %.2f %lu %.2f %ld %ld %ld %ld\n", (double)(record.from_us - started_us) / 1e6, (double)record.gap_us / 1000,
            record.frame, (double)record.cpu_us / 1000, record.faults, record.major, record.waits, record.preempted);
    int n = atomic_load(&nsamples);
    n = n > STALL_SAMPLES ? STALL_SAMPLES : n;
    for (int i = 0; i < n; i++) {
        Sample *s = &samples[i];
        if (!s->depth)
            continue;
        fprintf(f, "P %d %.2f %c", s->thread, (double)((int64_t)s->at_us - (int64_t)record.from_us) / 1000, s->what);
        for (int d = 0; d < s->depth; d++)
            fprintf(f, " %lx", (unsigned long)s->pc[d]);
        fputc('\n', f);
    }
    fflush(f);
}

static void *watchdog(void *arg) {
    FILE *f = arg;
    int after = limit_us * 2 / 5;
    for (;;) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        uint64_t beat = atomic_load(&last_beat_us);
        if (atomic_load(&record.ready)) {       /* (samples stopped by the frame's coming) */
            atomic_store(&sampling, 0);
            write_record(f);
            atomic_store(&nsamples, 0);
            atomic_store(&record.ready, 0);
            continue;
        }
        if (now_us() - beat > (uint64_t)after) {
            atomic_store(&sampling, 1);
            for (int i = 0; i < STALL_THREADS; i++)
                if (atomic_load(&thread_known[i]))
                    pthread_kill(threads[i], STALL_SIGNAL);
        } else if (atomic_load(&sampling)) {    /* (late, but not enough: its samples aren't wanted) */
            atomic_store(&sampling, 0);
            atomic_store(&nsamples, 0);
        }
    }
    return NULL;
}

static void start(void) {
    const char *e = getenv("MHFU_STALL");
    limit_us = e ? (int)(atof(e) * 1000) : 0;
    if (limit_us <= 0) {
        limit_us = 0;
        return;
    }
    char path[1024];
    if (getenv("MHFU_STALL_LOG"))
        snprintf(path, sizeof path, "%s", getenv("MHFU_STALL_LOG"));
    else
        snprintf(path, sizeof path, "%s/stalls.log", capture_dir());
    snprintf(log_path, sizeof log_path, "%s", path);
    FILE *f = fopen(path, "a");
    if (!f) {
        log_path[0] = 0;
        fprintf(stderr, "stalls: can't write %s\n", path);
        limit_us = 0;
        return;
    }
    {   /* (where the libraries are, for naming: tools/stalls.py) */
        FILE *m = fopen("/proc/self/maps", "r");
        char line[512];
        fprintf(f, "# run: frames over %d ms\n", limit_us / 1000);
        while (m && fgets(line, sizeof line, m))
            if (strstr(line, "r-xp"))
                fprintf(f, "M %s", line);
        if (m)
            fclose(m);
        fflush(f);
    }
    void *first[4];
    backtrace(first, 4);                /* (it loads what it needs on the first call: not in the signal handler) */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(STALL_SIGNAL, &sa, NULL);
    started_us = now_us();
    atomic_store(&last_beat_us, started_us);
    getrusage(RUSAGE_THREAD, &last_usage);
    stall_thread_named(0);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, watchdog, f))
        limit_us = 0;
    pthread_attr_destroy(&attr);
}

void stall_thread_named(int which) {
    if (which < 0 || which >= STALL_THREADS)
        return;
    my_thread = which;
    threads[which] = pthread_self();
    atomic_store(&thread_known[which], 1);
}

static int64_t cpu_of(const struct rusage *u) {
    return (int64_t)u->ru_utime.tv_sec * 1000000 + u->ru_utime.tv_usec + (int64_t)u->ru_stime.tv_sec * 1000000 + u->ru_stime.tv_usec;
}

void stall_beat(unsigned long frame) {
    if (limit_us < 0)
        start();
    if (!limit_us)
        return;
    uint64_t now = now_us(), was = atomic_load(&last_beat_us);
    struct rusage u;
    getrusage(RUSAGE_THREAD, &u);
    if (now - was > (uint64_t)limit_us && !atomic_load(&record.ready)) {
        record.from_us = was;
        record.gap_us = now - was;
        record.cpu_us = (uint64_t)(cpu_of(&u) - cpu_of(&last_usage));
        record.frame = frame;
        record.faults = u.ru_minflt - last_usage.ru_minflt;
        record.major = u.ru_majflt - last_usage.ru_majflt;
        record.waits = u.ru_nvcsw - last_usage.ru_nvcsw;
        record.preempted = u.ru_nivcsw - last_usage.ru_nivcsw;
        atomic_store(&record.ready, 1);
    }
    last_usage = u;
    atomic_store(&last_beat_us, now);
}
