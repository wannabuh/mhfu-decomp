/* A capture (capture.c, F6): a numbered folder with what is needed to look at a moment again. */
#pragma once
#include <stdint.h>

extern const char *rt_settings_path;    /* main.c: the settings file read */
const char *capture_dir(void);          /* where captures go, made if need be */
void capture_request(void);             /* the key: one more, as soon as there's a frame to take */
int capture_wants_shot(void);           /* present.c: the frame about to be shown is wanted */
void capture_take_shot(uint32_t addr, const uint32_t *soft_pixels);   /* present.c, then (no renderer: its pixels) */
int capture_state_pending(void);        /* a capture's state is still to be taken: the game must get to a state point */
void capture_tick(unsigned long frame);   /* present.c, each game frame: the event list, test hooks */
void capture_note_input(unsigned long frame, uint32_t buttons, int lx, int ly, float mx, float my);
void capture_state_taken(unsigned long frame, int late);   /* savestate.c */
