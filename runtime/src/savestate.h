/* Save states (savestate.c): the game's whole state - its memory, its threads (each one's registers and host stack)
 * and the runtime's own state of the game - copied at a point where every thread waits, and copied back.
 *
 * The runtime's state of the game is kept where a copy takes it whole: its variables in one section (SAVED), what it
 * allocates in one arena (st_malloc...), the threads' host stacks in one slab (st_stack_alloc). What is the host's - files,
 * sound streams, decoders, the renderer - is kept apart, and made again after a load (the *_state_loaded hooks). */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* A variable of the game's state: in the section a save state copies */
#define SAVED __attribute__((section("mhfu_state")))

void *st_malloc(size_t size);
void *st_calloc(size_t n, size_t size);
void *st_realloc(void *p, size_t size);
void st_free(void *p);

/* A coroutine's host stack (size: at most ST_STACK_SIZE), and its end */
#define ST_STACK_SIZE (2u << 20)
void *st_stack_alloc(void);
void st_stack_free(void *stack);

/* The used part of each thread's host stack (thread.c): calls f with each, from its stack pointer to its top */
void thread_stack_spans(void (*f)(void *lo, size_t size));

/* A save or a load asked for (slot 0..ST_SLOTS-1): done at the next point every thread waits (state_point) */
#define ST_SLOTS 10
void state_request_save(int slot);
void state_request_load(int slot);
/* thread.c's scheduler, every thread waiting: a save or load asked for is done now. 1 if a state was loaded */
int state_point(void);
/* Whether a slot holds a state */
int state_slot_used(int slot);

/* After a load: each part made ready again (what is the host's: files, streams, decoders, caches) */
void audio_state_loaded(void);
void atrac_state_loading(void);         /* (before: the decoders that are, freed) */
void atrac_state_loaded(void);
void gpu_state_loaded(void);
void present_state_loaded(void);        /* (a recording of the input ends) */
