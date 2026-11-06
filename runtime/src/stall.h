/* The stall recorder: what the game was doing when a frame came late (stall.c) */
#ifndef STALL_H
#define STALL_H

void stall_beat(unsigned long frame);   /* a frame shown (present.c) */
void stall_thread_named(int which);     /* this thread sampled too: 1 the render thread */

#endif
