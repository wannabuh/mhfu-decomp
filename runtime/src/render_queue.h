/* The render queue (render_queue.c): gpu.c's commands to the renderer, done on a thread of its own */
#pragma once
#include <stddef.h>
#include <stdint.h>

void render_queue_start(void (*run)(const void *cmd, size_t n));   /* (the renderer's commands' runner: gpu.c) */
int render_queue_on(void);
extern int render_queue_on_flag;        /* (render_queue_on's, read as often as it is) */
void *render_queue_reserve(size_t n);   /* a command of n bytes to fill, then commit */
void render_queue_commit(void);
void render_queue_sync(void);           /* wait until all queued is done (the renderer free to call, memory to change) */
