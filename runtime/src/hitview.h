/* The hit overlay (hitview.c, F12): what the hit test (readable/hit_detect.c) tested, as it tested it. */
#pragma once
#include <stdint.h>

#include "glyphs.h"

extern int hit_view_on;

/* Why a target wasn't tested for a record, before any shape */
enum { HIT_SKIP_AREA, HIT_SKIP_RECORD_FLAG, HIT_SKIP_OWN, HIT_SKIP_LOGGED, HIT_SKIP_PENDING, HIT_SKIP_LAST_HIT, HIT_SKIP_KIND, HIT_SKIP_IMMUNE,
       HIT_SKIPS };

/* Addresses in the game's memory; `shape` is where the shape's type word is (the shape from +0x10) */
void hit_view_skip(uint32_t record, uint32_t target, int why);
void hit_view_attacker(uint32_t record, uint32_t shape);
void hit_view_part(uint32_t record, uint32_t target, uint32_t part, uint32_t attacker_shape, uint32_t target_shape, int can_take, int touched);

void hit_view_toggle(void);
void hit_view_tick(unsigned long frame);        /* present.c, each game frame */
int hit_view_rects(float x0, float y0, float scale, HintRect *out, int max);
