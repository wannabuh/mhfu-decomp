/* The game's button hints (the PSP's circle, cross, triangle, square, L, R, START, SELECT and the D-pad, as it shows
 * them on the item bar and elsewhere) shown as the keys and buttons the actions are bound to (controls.c), those of
 * the device last used: a key's cap with its name, the mouse with its button lit, a gamepad's coloured face buttons.
 *
 * They are sprites of the HUD's atlas (texture 0x8001: sprites 50 circle, 51 cross, 52 triangle, 53 square, 54 L,
 * 55 R, 57 START, 58 SELECT, 59 the D-pad; the sprites' table at 089392C8, 12 bytes each: u, v, width, height,
 * texture), drawn by EBOOT 0882AF70 (at a size given) and 0882A9DC (at their own: the item bar's) - patches.py:
 * rt_sprite. Where the button stands for an action that has an input on the device, the sprite is
 * left out and noted, and the renderer draws the input's picture over its place on the screen, at the window's
 * resolution (render.c, as cleared rectangles). Which action a button stands for depends on where it is shown: on the
 * item bar square is use item, but with the bar's choice open (L held) square is the next item and circle the one
 * before; in a menu, the buttons as they are there (cross confirm, circle back...).
 *
 * The game draws its 2D at 30ths (native.c): the hints noted in a 30th's frame are shown until the next 30th's.
 * MHFU_GLYPH_LOG=1 prints each hint sprite drawn (where from, where on the screen) the first time. */
#include "hle.h"
#include "glyphs.h"
#include "controls.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    PSP_SELECT = 0x1, PSP_START = 0x8, PSP_DPAD = 0xF0, PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000,
    PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000,
};

extern int rt_frame30;
int window_in_menu(void);
int gpu_hud_anchor(int part, float *ax, float *ay, float *scale);
int gpu_hud_part(void);

const float hint_palette[HINT_COLORS][4] = {
    {0.90f, 0.89f, 0.84f, 1}, {0.10f, 0.10f, 0.12f, 1}, {0.96f, 0.62f, 0.16f, 1}, {1, 1, 1, 1},
    {0.32f, 0.72f, 0.28f, 1}, {0.86f, 0.26f, 0.22f, 1}, {0.26f, 0.48f, 0.88f, 1}, {0.92f, 0.76f, 0.20f, 1},
    {0.24f, 0.24f, 0.27f, 1}, {0.56f, 0.56f, 0.60f, 1},
};

/* ---- a 5 x 7 font: capitals, figures, a few signs, and arrows (\1 up, \2 down, \3 left, \4 right) ---- */

const uint8_t *glyph_font(char ch) {
    static const char signs[] = ":()-./+\1\2\3\4%?#_=>@,";
    static const uint8_t letters[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
        {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
        {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
        {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
        {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 17, 10, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}};
    static const uint8_t figures[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},
        {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t sign[19][7] = {{0, 12, 12, 0, 12, 12, 0}, {2, 4, 8, 8, 8, 4, 2}, {8, 4, 2, 2, 2, 4, 8},
                                        {0, 0, 0, 31, 0, 0, 0}, {0, 0, 0, 0, 0, 12, 12}, {1, 1, 2, 4, 8, 16, 16},
                                        {0, 4, 4, 31, 4, 4, 0}, {4, 14, 21, 4, 4, 4, 4}, {4, 4, 4, 4, 21, 14, 4},
                                        {0, 4, 8, 31, 8, 4, 0}, {0, 4, 2, 31, 2, 4, 0},
                                        {24, 25, 2, 4, 8, 19, 3}, {14, 17, 1, 2, 4, 0, 4}, {10, 31, 10, 10, 31, 10, 0},
                                        {0, 0, 0, 0, 0, 0, 31}, {0, 0, 31, 0, 31, 0, 0}, {8, 4, 2, 1, 2, 4, 8},
                                        {14, 17, 23, 21, 23, 16, 14}, {0, 0, 0, 0, 12, 4, 8}};
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    if (ch >= 'A' && ch <= 'Z')
        return letters[ch - 'A'];
    if (ch >= '0' && ch <= '9')
        return figures[ch - '0'];
    const char *at = ch ? strchr(signs, ch) : NULL;
    return at ? sign[at - signs] : NULL;
}

/* ---- the hints noted ---- */

typedef struct { float x, y, w, h; uint32_t button, from; } Hint;
#define MAX_HINTS 32
static Hint building[MAX_HINTS], shown[MAX_HINTS];
static int nbuilding, nshown, pass;     /* pass: a 30th's 2D has been drawn since the last shown */

/* Where the item bar's choice is drawn open (L held: EBOOT 08846xxx), its square and circle the next and previous
 * item (the bar closed: its square from 08845D98, its L from 08845DB4) */
static const uint32_t bar_open_from[] = {0x08846898u, 0x088468D0u};

/* The action a button stands for, drawn from there (-1: none) */
static int action_for(uint32_t button, uint32_t from, int menu) {
    int open = 0;
    for (size_t i = 0; i < sizeof bar_open_from / sizeof bar_open_from[0]; i++)
        open |= bar_open_from[i] && from == bar_open_from[i];
    if (menu)
        switch (button) {
        case PSP_CROSS: return A_CONFIRM;
        case PSP_CIRCLE: return A_BACK;
        case PSP_SQUARE: return A_USE_ITEM;
        case PSP_TRIANGLE: return A_PSP_TRIANGLE;
        case PSP_L: return A_CAMERA_RESET;
        case PSP_R: return A_SPRINT;
        case PSP_START: return A_MENU;
        case PSP_SELECT: return A_MAP;
        case PSP_DPAD: return A_UP;
        }
    switch (button) {
    case PSP_SQUARE: return open ? A_ITEM_NEXT : A_USE_ITEM;
    case PSP_CIRCLE: return open ? A_ITEM_PREV : A_ATTACK2;
    case PSP_CROSS: return A_ROLL;
    case PSP_TRIANGLE: return A_ATTACK;
    case PSP_L: return A_ITEM_NEXT;      /* (the item bar's: hold L to choose) */
    case PSP_R: return A_SPRINT;
    case PSP_START: return A_MENU;
    case PSP_SELECT: return A_MAP;
    case PSP_DPAD: return A_UP;
    }
    return -1;
}

/* The input to show for a button drawn from there: its name, and whether a gamepad's (NULL: none, the PSP's shown) */
static const char *input_for(uint32_t button, uint32_t from, int *pad) {
    int menu = window_in_menu();
    *pad = controls_device();
    if (*pad && menu) {                 /* a gamepad in a menu: as the PSP is laid out */
        switch (button) {
        case PSP_CROSS: return "a";
        case PSP_CIRCLE: return "b";
        case PSP_SQUARE: return "x";
        case PSP_TRIANGLE: return "y";
        case PSP_L: return "leftshoulder";
        case PSP_R: return "rightshoulder";
        case PSP_START: return "start";
        case PSP_SELECT: return "back";
        case PSP_DPAD: return "dpad";
        }
        return NULL;
    }
    int a = action_for(button, from, menu);
    if (a < 0)
        return NULL;
    if (button == PSP_L && !menu) {     /* the item bar's L: either way through the items */
        const char *next = controls_input_name(*pad, A_ITEM_NEXT, 0), *prev = controls_input_name(*pad, A_ITEM_PREV, 0);
        if (next && prev && !strncasecmp(next, "Wheel ", 6) && !strncasecmp(prev, "Wheel ", 6))
            return "Wheel Both";
        if (next && prev && !strncasecmp(next, "dp", 2) && !strncasecmp(prev, "dp", 2))
            return "dpleftright";
    }
    if (button == PSP_DPAD)
        return *pad ? "dpad" : "arrows";     /* (all its arms) */
    const char *name = controls_input_name(*pad, a, 0);
    if (!name && *pad)                  /* (none on the gamepad: the keyboard's) */
        name = controls_input_name(*pad = 0, a, 0);
    return name;
}

static const struct { int index; uint32_t button; } sprites[] = {
    {50, PSP_CIRCLE}, {51, PSP_CROSS}, {52, PSP_TRIANGLE}, {53, PSP_SQUARE}, {54, PSP_L}, {55, PSP_R},
    {57, PSP_START}, {58, PSP_SELECT}, {59, PSP_DPAD},
};

int rt_sprite(Ctx *c, uint32_t index, uint32_t x, uint32_t y, int w, int h) {
    static int logging = -1, off = -1;
    if (logging < 0) {
        logging = getenv("MHFU_GLYPH_LOG") != NULL;
        off = getenv("MHFU_PSP_HINTS") && atoi(getenv("MHFU_PSP_HINTS"));
    }
    uint32_t button = 0;
    for (size_t i = 0; i < sizeof sprites / sizeof sprites[0]; i++)
        if (sprites[i].index == (int)index)
            button = sprites[i].button;
    if (!button || off)
        return 0;
    if (w < 0) {                        /* (its own size, from the table) */
        w = LH(0x089392C8u + 12 * index + 4);
        h = LH(0x089392C8u + 12 * index + 6);
    }
    uint32_t from = c->r[31];
    if (logging) {
        static uint32_t seen[64];
        static int nseen;
        int i = 0;
        while (i < nseen && seen[i] != (from ^ index << 24))
            i++;
        if (i == nseen && nseen < 64) {
            seen[nseen++] = from ^ index << 24;
            fprintf(stderr, "glyph: sprite %u (button %x) from %08x at %d,%d %dx%d%s\n", index, button, from,
                    (int)(int16_t)x, (int)(int16_t)y, w, h, window_in_menu() ? " (menu)" : "");
        }
    }
    int pad;
    if (!input_for(button, from, &pad) || nbuilding >= MAX_HINTS)
        return 0;
    if (!pass) {
        pass = 1;
        nbuilding = 0;
    }
    float hx = (float)(int16_t)x, hy = (float)(int16_t)y, hw = (float)w, hh = (float)h, ax, ay, k;
    if (gpu_hud_anchor(gpu_hud_part(), &ax, &ay, &k)) {     /* (the HUD's part being drawn, made smaller: gpu.c) */
        hx = ax + (hx - ax) * k, hy = ay + (hy - ay) * k;
        hw *= k, hh *= k;
    }
    building[nbuilding++] = (Hint){hx, hy, hw, hh, button, from};
    return 1;
}

/* A game frame shown (present.c): if a 30th's 2D was drawn in it, its hints are those shown from now */
void glyphs_frame_end(void) {
    if (rt_frame30) {
        memcpy(shown, building, sizeof shown);
        nshown = pass ? nbuilding : 0;
        nbuilding = 0;
        pass = 0;
    }
}

/* ---- the pictures, in window pixels ---- */

void pen_box(Pen *p, int x, int y, int w, int h, int color) {
    if (p->n < p->max && w > 0 && h > 0)
        p->out[p->n++] = (HintRect){p->x + x * p->dot, p->y + y * p->dot, w * p->dot, h * p->dot, color};
}

/* A box with its corners cut: the outline in ink, filled */
void pen_rounded(Pen *p, int x, int y, int w, int h, int line, int fill) {
    pen_box(p, x + 1, y, w - 2, h, line);
    pen_box(p, x, y + 1, w, h - 2, line);
    pen_box(p, x + 2, y + 1, w - 4, h - 2, fill);
    pen_box(p, x + 1, y + 2, w - 2, h - 4, fill);
}

int pen_text_width(const char *s) { return (int)strlen(s) * 6 - 1; }

void pen_text(Pen *p, int x, int y, const char *s, int color) {
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = glyph_font(s[i]);
        for (int row = 0; g && row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] >> (4 - col) & 1) {
                    int run = 1;            /* (a row's dots side by side as one) */
                    while (col + run < 5 && (g[row] >> (4 - col - run) & 1))
                        run++;
                    pen_box(p, x + i * 6 + col, y + row, run, 1, color);
                    col += run - 1;
                }
    }
}

static void disc(Pen *p, int cx, int cy, int r, int color) {
    for (int dy = -r; dy < r; dy++) {
        float f = (float)dy + 0.5f;
        int half = (int)floorf(sqrtf((float)(r * r) - f * f) + 0.5f);
        pen_box(p, cx - half, cy + dy, 2 * half, 1, color);
    }
}

/* The short name on a key's cap */
static void key_label(const char *name, char *out, size_t size) {
    static const struct { const char *name, *label; } short_names[] = {
        {"Left Shift", "SHIFT"}, {"Right Shift", "SHIFT"}, {"Left Ctrl", "CTRL"}, {"Right Ctrl", "CTRL"},
        {"Left Alt", "ALT"}, {"Right Alt", "ALT"}, {"Return", "ENTER"}, {"Escape", "ESC"}, {"Backspace", "BKSP"},
        {"Space", "SPACE"}, {"Up", "\1"}, {"Down", "\2"}, {"Left", "\3"}, {"Right", "\4"}, {"Delete", "DEL"},
        {"Insert", "INS"}, {"CapsLock", "CAPS"}, {"PageUp", "PGUP"}, {"PageDown", "PGDN"},
    };
    for (size_t i = 0; i < sizeof short_names / sizeof short_names[0]; i++)
        if (!strcasecmp(name, short_names[i].name)) {
            snprintf(out, size, "%s", short_names[i].label);
            return;
        }
    snprintf(out, size, "%s", name);
    for (char *c = out; *c; c++)
        if (!glyph_font(*c))
            *c = '.';
    if (strlen(out) > 5)
        out[5] = 0;
}

/* The picture of an input, its middle at (cx, cy) in dots: about 14 dots high */
void pen_input(Pen *p, const char *name, int pad, int cx, int cy) {
    if (!pad && !strncasecmp(name, "Mouse ", 6)) {
        int lit = !strcasecmp(name + 6, "Left") ? 0 : !strcasecmp(name + 6, "Right") ? 1 :
                  !strcasecmp(name + 6, "Middle") ? 2 : 3;
        int x = cx - 5, y = cy - 7;
        pen_rounded(p, x, y, 10, 14, C_INK, C_FACE);
        pen_box(p, x + 1, y + 6, 8, 1, C_INK);          /* the buttons' edge */
        pen_box(p, x + 5, y + 1, 1, 5, C_INK);          /* between them */
        if (lit == 0) pen_box(p, x + 2, y + 1, 3, 5, C_LIT), pen_box(p, x + 1, y + 2, 1, 4, C_LIT);
        if (lit == 1) pen_box(p, x + 6, y + 1, 2, 5, C_LIT), pen_box(p, x + 8, y + 2, 1, 4, C_LIT);
        if (lit == 2) pen_box(p, x + 4, y + 2, 3, 3, C_LIT);
        if (lit == 3) pen_text(p, x + 3, y + 7, name[6] == 'X' ? (name[7] == '1' ? "4" : "5") : "?", C_INK);
        return;
    }
    if (!pad && !strncasecmp(name, "Wheel ", 6)) {
        int up = !strcasecmp(name + 6, "Up"), x = cx - 8, y = cy - 7;
        if (!strcasecmp(name + 6, "Both"))      /* (the wheel either way: no arrow, the mouse in the middle) */
            x = cx - 5;
        pen_rounded(p, x, y, 10, 14, C_INK, C_FACE);
        pen_box(p, x + 1, y + 6, 8, 1, C_INK);
        pen_box(p, x + 4, y + 2, 2, 4, C_LIT);          /* the wheel */
        if (!strcasecmp(name + 6, "Both"))
            return;
        for (int i = 0; i < 4; i++)                 /* an arrow beside it: light, outlined */
            pen_box(p, x + 13 - i - 1, y + (up ? 1 + i : 12 - i) - (up ? 1 : -1), 3 + 2 * i, 1, C_INK);
        pen_box(p, x + 12, y + (up ? 5 : 2), 3, 5, C_INK);
        for (int i = 0; i < 3; i++)
            pen_box(p, x + 13 - i, y + (up ? 1 + i : 11 - i), 1 + 2 * i, 1, C_FACE);
        pen_box(p, x + 13, y + (up ? 4 : 5), 1, 4, C_FACE);
        return;
    }
    if (!strcmp(name, "arrows") || !strncasecmp(name, "dp", 2)) {   /* the D-pad (or a key's arrows): its arms lit */
        const char *d = !strcmp(name, "arrows") ? "" : name + 2;
        pen_box(p, cx - 3, cy - 8, 6, 16, C_INK);
        pen_box(p, cx - 8, cy - 3, 16, 6, C_INK);
        pen_box(p, cx - 2, cy - 7, 4, 14, C_GREY);
        pen_box(p, cx - 7, cy - 2, 14, 4, C_GREY);
        int all = !*d || !strcasecmp(d, "ad");
        if (all || !strcasecmp(d, "up")) pen_box(p, cx - 1, cy - 6, 2, 4, C_LIT);
        if (all || !strcasecmp(d, "down")) pen_box(p, cx - 1, cy + 2, 2, 4, C_LIT);
        if (all || !strcasecmp(d, "left") || !strcasecmp(d, "leftright")) pen_box(p, cx - 6, cy - 1, 4, 2, C_LIT);
        if (all || !strcasecmp(d, "right") || !strcasecmp(d, "leftright")) pen_box(p, cx + 2, cy - 1, 4, 2, C_LIT);
        return;
    }
    if (!pad) {                         /* a key */
        char label[16];
        key_label(name, label, sizeof label);
        int tw = pen_text_width(label), w = tw + 6 > 14 ? tw + 6 : 14;
        pen_rounded(p, cx - w / 2, cy - 7, w, 14, C_INK, C_FACE);
        pen_box(p, cx - w / 2 + 2, cy + 4, w - 4, 1, C_GREY);   /* (the cap's lower edge) */
        pen_text(p, cx - tw / 2, cy - 4, label, C_INK);
        return;
    }
    static const struct { const char *name, *label; int color; } face[] = {
        {"a", "A", C_GREEN}, {"b", "B", C_RED}, {"x", "X", C_BLUE}, {"y", "Y", C_YELLOW},
    };
    for (size_t i = 0; i < sizeof face / sizeof face[0]; i++)
        if (!strcasecmp(name, face[i].name)) {
            disc(p, cx, cy, 7, C_INK);
            disc(p, cx, cy, 6, face[i].color);
            pen_text(p, cx - 2, cy - 3, face[i].label, C_WHITE);
            return;
        }
    static const struct { const char *name, *label; } named[] = {
        {"leftshoulder", "LB"}, {"rightshoulder", "RB"}, {"lefttrigger", "LT"}, {"righttrigger", "RT"},
        {"leftstick", "L3"}, {"rightstick", "R3"}, {"start", "START"}, {"back", "BACK"}, {"guide", "HOME"},
    };
    const char *label = name;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcasecmp(name, named[i].name))
            label = named[i].label;
    char l[16];
    key_label(label, l, sizeof l);
    int tw = pen_text_width(l), w = tw + 6;
    pen_rounded(p, cx - w / 2, cy - 6, w, 12, C_INK, C_PAD);
    pen_text(p, cx - tw / 2, cy - 3, l, C_WHITE);
}

/* How wide an input's picture is (dots) */
int pen_input_width(const char *name, int pad) {
    if (!pad && !strncasecmp(name, "Mouse ", 6))
        return 10;
    if (!pad && !strncasecmp(name, "Wheel ", 6))
        return strcasecmp(name + 6, "Both") ? 16 : 10;
    if (!strcmp(name, "arrows") || !strncasecmp(name, "dp", 2))
        return 16;
    char l[16];
    if (!pad) {
        key_label(name, l, sizeof l);
        int w = pen_text_width(l) + 6;
        return w > 14 ? w : 14;
    }
    static const char *faces[] = {"a", "b", "x", "y"};
    for (int i = 0; i < 4; i++)
        if (!strcasecmp(name, faces[i]))
            return 14;
    static const struct { const char *name, *label; } named[] = {
        {"leftshoulder", "LB"}, {"rightshoulder", "RB"}, {"lefttrigger", "LT"}, {"righttrigger", "RT"},
        {"leftstick", "L3"}, {"rightstick", "R3"}, {"start", "START"}, {"back", "BACK"}, {"guide", "HOME"},
    };
    const char *label = name;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcasecmp(name, named[i].name))
            label = named[i].label;
    key_label(label, l, sizeof l);
    return pen_text_width(l) + 6;
}

int glyphs_rects(float x0, float y0, float scale, HintRect *out, int max) {
    Pen p = {out, 0, max, 1, 0, 0};
    for (int i = 0; i < nshown; i++) {
        const Hint *h = &shown[i];
        int pad;
        const char *name = input_for(h->button, h->from, &pad);
        if (!name)
            continue;
        /* the picture's 16 dots as high as the sprite (at least a pixel a dot), its middle on the sprite's */
        float px = h->h * scale;
        p.dot = (int)floorf(px / 16 + 0.25f);
        if (p.dot < 1)
            p.dot = 1;
        p.x = (int)lroundf(x0 + (h->x + h->w / 2) * scale);
        p.y = (int)lroundf(y0 + (h->y + h->h / 2) * scale);
        pen_input(&p, name, pad, 0, 0);
    }
    return p.n;
}
