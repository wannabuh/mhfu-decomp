/* The ticket key (ticket.c, F6): holds the game, takes a capture, and asks for a note. */
#pragma once
#include "glyphs.h"

void ticket_key(void);                  /* window.c: F6 */
int ticket_event(const void *sdl_event);   /* window.c: the box open, what it takes (1: nothing else is to see it) */
void ticket_poll(void);                 /* present.c: each pass, held or not: a note waiting for its capture */
void ticket_test(unsigned long frame);  /* capture.c, each game frame: MHFU_TEST_TICKET */
int ticket_rects(float x0, float y0, float scale, HintRect *out, int max);   /* render.c: the box */
const char *ticket_dir(void);           /* where tickets go, made if need be */
void console_start(void);               /* window.c: what the run prints kept, for a capture's console.txt */
char *console_copy(size_t *n);          /* capture.c: the last of it */
