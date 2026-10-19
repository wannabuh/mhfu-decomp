/* The render queue: what gpu.c gives the renderer (render.c), done on a thread of its own. The GE's vertex stage stays
 * on the game's thread; each draw's state and its vertices after it go into a ring of commands, which the render thread
 * takes in order - clipping, the renderer's batches, textures, Vulkan - while the game's thread goes on to the next.
 * It is all one GE's work as before, only in two halves at once.
 *
 * The renderer reads PSP memory (textures, palettes) as it goes, so what it reads must not change before it does: the
 * game's thread waits for the queue to be done (render_queue_sync) before anything can write it - the list's run ends
 * (the game runs on), a callback of the list's (game code), a block transfer - and before anything else calls the
 * renderer (present.c, the window): the renderer is the render thread's alone while there is work queued, the game's
 * thread's when there is none.
 *
 * MHFU_RENDER_THREAD=0: all on the game's thread, as before. */
#include "render_queue.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RQ_BYTES (64u << 20)            /* the ring: a frame's vertices and more */
#define ALIGN(n) (((n) + 15) & ~(size_t)15)

typedef struct {
    uint32_t size;                      /* the command's bytes after this header; 0: the ring goes on from its start */
    uint32_t pad[3];
} Header;

static uint8_t *ring;
/* bytes ever written (given the render thread), ever done - positions in the ring, modulo RQ_BYTES; each on a cache
 * line of its own (the two threads write them) */
static _Alignas(64) _Atomic size_t written;
static _Alignas(64) _Atomic size_t done;
static _Alignas(64) size_t made, given;  /* (the game's thread's) bytes made so far, and given at the last publish */
static size_t reserving;                /* the command being made: where */
#define PUBLISH_BYTES (64u << 10)       /* given the render thread in batches of this much */
static void (*run)(const void *cmd, size_t n);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work = PTHREAD_COND_INITIALIZER, emptied = PTHREAD_COND_INITIALIZER;
static atomic_int asleep, waiting;
static int on = -1;

static void *render_thread(void *arg) {
    (void)arg;
    for (;;) {
        size_t at = atomic_load_explicit(&done, memory_order_relaxed);
        for (int spins = 0; spins < 200 && at == atomic_load_explicit(&written, memory_order_acquire); spins++)
            __builtin_ia32_pause();
        if (at == atomic_load_explicit(&written, memory_order_acquire)) {
            pthread_mutex_lock(&lock);
            if (atomic_load(&waiting))
                pthread_cond_broadcast(&emptied);
            atomic_store(&asleep, 1);
            atomic_thread_fence(memory_order_seq_cst);
            while (at == atomic_load(&written))
                pthread_cond_wait(&work, &lock);
            atomic_store(&asleep, 0);
            pthread_mutex_unlock(&lock);
            continue;
        }
        const Header *h = (const Header *)(ring + at % RQ_BYTES);
        if (!h->size) {                 /* to the ring's start */
            atomic_store_explicit(&done, at + (RQ_BYTES - at % RQ_BYTES), memory_order_release);
            continue;
        }
        run(h + 1, h->size);
        atomic_store_explicit(&done, at + sizeof(Header) + ALIGN(h->size), memory_order_release);
    }
    return NULL;
}

int render_queue_on_flag;
static void render_queue_stop(void);

int render_queue_on(void) {
    return on > 0;
}

void render_queue_start(void (*runner)(const void *cmd, size_t n)) {
    on = !getenv("MHFU_RENDER_THREAD") || atoi(getenv("MHFU_RENDER_THREAD")) != 0;
    render_queue_on_flag = on;
    if (!on)
        return;
    ring = aligned_alloc(64, RQ_BYTES);
    run = runner;
    pthread_t t;
    atexit(render_queue_stop);
    if (!ring || pthread_create(&t, NULL, render_thread, NULL)) {
        fprintf(stderr, "renderer: no thread of its own\n");
        on = render_queue_on_flag = 0;
        return;
    }
    pthread_detach(t);
}

/* What has been made given the render thread (woken, if it sleeps) */
static void publish(void) {
    if (made == given)
        return;
    given = made;
    atomic_store_explicit(&written, made, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);  /* (seen before asleep is read: the render thread does the same) */
    if (atomic_load_explicit(&asleep, memory_order_relaxed)) {
        pthread_mutex_lock(&lock);
        pthread_cond_signal(&work);
        pthread_mutex_unlock(&lock);
    }
}

/* Wait for the ring to have n bytes free from the write position on */
static void room(size_t n) {
    if (made + n - atomic_load_explicit(&done, memory_order_acquire) <= RQ_BYTES)
        return;
    publish();
    for (int spins = 0; made + n - atomic_load_explicit(&done, memory_order_acquire) > RQ_BYTES; spins++)
        if (spins > 64)
            sched_yield();
}

void *render_queue_reserve(size_t n) {
    size_t need = sizeof(Header) + ALIGN(n);
    if (made % RQ_BYTES + need > RQ_BYTES) {   /* not at the ring's end: go on from its start */
        room(RQ_BYTES - made % RQ_BYTES);
        ((Header *)(ring + made % RQ_BYTES))->size = 0;
        made += RQ_BYTES - made % RQ_BYTES;
    }
    room(need);
    Header *h = (Header *)(ring + made % RQ_BYTES);
    h->size = (uint32_t)n;
    reserving = made;
    return h + 1;
}

void render_queue_commit(void) {
    const Header *h = (const Header *)(ring + reserving % RQ_BYTES);
    made = reserving + sizeof(Header) + ALIGN(h->size);
    if (made - given >= PUBLISH_BYTES)
        publish();
}

void render_queue_sync(void) {
    if (on <= 0)
        return;
    publish();
    size_t target = made;
    for (int spins = 0; spins < 2000; spins++)
        if (atomic_load_explicit(&done, memory_order_acquire) == target)
            return;
    pthread_mutex_lock(&lock);
    atomic_store(&waiting, 1);
    while (atomic_load(&done) != target) {
        pthread_cond_signal(&work);
        pthread_cond_wait(&emptied, &lock);
    }
    atomic_store(&waiting, 0);
    pthread_mutex_unlock(&lock);
}

/* At exit: what is queued done, and the render thread left waiting - the renderer the exiting thread's alone (its
 * teardown) */
static void render_queue_stop(void) {
    if (on <= 0)
        return;
    render_queue_sync();
    on = render_queue_on_flag = 0;
}
