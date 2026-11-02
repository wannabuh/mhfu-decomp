/* The PSP's button hints shown as the keys and buttons bound (glyphs.c) */
#pragma once
#include <stdint.h>

#include "recomp.h"

/* A rectangle of the hints, in the window's pixels, in one of the palette's colours */
typedef struct { int x, y, w, h, color; } HintRect;

extern const float hint_palette[][4];
enum { HINT_COLORS = 10 };
enum { C_FACE, C_INK, C_LIT, C_WHITE, C_GREEN, C_RED, C_BLUE, C_YELLOW, C_PAD, C_GREY };

/* Drawing in rectangles: at (x, y) window pixels, in dots of dot pixels */
typedef struct { HintRect *out; int n, max, dot, x, y; } Pen;
void pen_box(Pen *p, int x, int y, int w, int h, int color);
void pen_rounded(Pen *p, int x, int y, int w, int h, int line, int fill);     /* (its corners cut) */
int pen_text_width(const char *s);
void pen_text(Pen *p, int x, int y, const char *s, int color);               /* (7 dots high) */
int pen_input_width(const char *name, int pad);                              /* (dots) */
void pen_input(Pen *p, const char *name, int pad, int cx, int cy);           /* an input's picture, about 14 dots
                                                                                 high, its middle at (cx, cy) */

const uint8_t *glyph_font(char ch);     /* a 5 x 7 letter: 7 rows of 5 bits (NULL: none) */
int rt_sprite(Ctx *c, uint32_t sprite, uint32_t x, uint32_t y, int w, int h);  /* (recomp.h) */
void glyphs_frame_end(void);            /* a game frame shown: its hints are those shown from now */
/* The hints over a picture at (x0, y0), scale window pixels a PSP pixel: into out (at most max); how many */
int glyphs_rects(float x0, float y0, float scale, HintRect *out, int max);
