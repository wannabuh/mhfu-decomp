/* Save states (savestate.c): the game's whole state - its memory, and the runtime's state of it - copied at a point where
 * every thread waits, and copied back; on disk they load in any later build.
 *
 * The runtime's state of the game is kept where a copy takes it whole: its variables in one section (SAVED, stored by
 * name), what it allocates in one arena (st_malloc...). What is the host's - files, sound streams, decoders, the renderer,
 * the threads' host stacks - is kept apart, and made again after a load (the *_state_loaded hooks). No host address of
 * the program may be in what is copied: no function pointers, no pointers to its variables. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* A variable of the game's state: in the section a save state copies */
#define SAVED __attribute__((section("mhfu_state"), used))     /* (used: kept whole, under its own name) */

void *st_malloc(size_t size);
void *st_calloc(size_t n, size_t size);
void *st_realloc(void *p, size_t size);
void st_free(void *p);

/* A coroutine's host stack (size: at most ST_STACK_SIZE), and its end */
#define ST_STACK_SIZE (2u << 20)
void *st_stack_alloc(void);
void st_stack_free(void *stack);
void st_stacks_reset(void);             /* (a state loaded: none in use) */

/* What the arena's structs look like (thread.c, kernel.c): a state from another layout does not load. Their sizes; a
 * field changed for another of the same size: bump ST_LAYOUT */
#define ST_LAYOUT 1
uint64_t thread_layout(void);
uint64_t kernel_layout(void);

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
void threads_state_loaded(void);        /* (each that waits, a new host stack) */
