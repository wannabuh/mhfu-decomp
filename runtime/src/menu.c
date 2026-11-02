/* The mouse in the game's menus: pointing at an item moves the game's cursor there, a left click picks it.
 *
 * What the game draws of its menus is noted as it draws them (recomp/patches.py), each game frame, as groups of items
 * (rectangles on the PSP's screen, in the order the cursor goes down them), one of them the cursor's:
 *   - the list windows (the menus, shops, the choices people offer: EBOOT 0882F580, rt_menu_list), from their layout
 *     (halfwords: +0x10 the rows' pitch, +0x12 how many rows show, +0x1C, +0x1E the cursor's bar's place on the first
 *     row, +0x20, +0x22 its size) and the row the bar is on - and, where a window opens over another (an item's
 *     choices over the item pouch, a yes or no over those), whether it is the one in use: those behind have their bar
 *     grey (+0x18), and the windows are drawn in no telling order. The Options' window, drawn with no bar, is noted
 *     with its cursor's row by its own drawer (EBOOT 08839824);
 *   - lists drawn an item at a time (the quest lists' plates: rt_menu_item), each item as it is drawn;
 *   - grids of slots laid out evenly, their cursor a byte (the supply box's items, EBOOT 08847CB0: rt_menu_grid);
 *   - the cursor's bar alone, where nothing else is known of the list (EBOOT 0882F4A4, 08831F3C: rt_menu_bar).
 * While a group is drawn the mouse is a pointer (window.c), put on the cursor to begin with. Moved over another item
 * of the group in front (the last drawn of those in use), the game's cursor goes there at once: the menus' cursor
 * routine (EBOOT 0881C2E4, rt_menu_cursor) is made to step to it from the row before, or where the cursor is kept is
 * known (the quest lists) it is put there and the step's sound made. Otherwise it presses down or up a step at a time
 * until the game's cursor is there - the game moves its own cursor, scrolling, sounding and skipping what it skips as
 * it would; for a bar alone it
 * presses towards the pointer (up or down where the pointer is above or below it, left or right beside it) until the
 * bar is under it. A step that does not bring the cursor nearer (the list ends there, or wraps round) is undone and
 * the pointer given up on until it moves again. A left click on an item goes there and presses cross (a few frames
 * after the cursor got there: what slides as the cursor comes takes no cross until it has). A left click on one of
 * the arrows either side of a page number or a setting (EBOOT 0882E6F0, rt_menu_arrows) presses left or right. The
 * devices' D-pad wins over the pointer.
 *
 * MHFU_TEST_POINTER="frame,x,y[,click][;frame,x,y[,click]...]": the pointer there (PSP pixels) from the game's frame,
 * clicking if click is 1 (for testing without a window). MHFU_MENU_LOG=1 prints the groups seen. */
#include "hle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void window_pointer_mode(int on, float at_x, float at_y);
int present_pointer(float *x, float *y, int *moved, int *clicks);
unsigned long present_frame_number(void);

#define PSP_UP 0x10
#define PSP_RIGHT 0x20
#define PSP_DOWN 0x40
#define PSP_LEFT 0x80
#define PSP_DPAD 0xF0
#define PSP_CROSS 0x4000

#define MAX_ITEMS 64
#define MAX_GROUPS 8

typedef struct { int x, y, w, h; } Rect;

typedef struct {
    int kind;                           /* 0 a list window, 1 items drawn one by one, 2 a bar alone */
    int n, sel;                         /* items; the cursor's (-1: none yet) */
    int active;                         /* the one the keys move (a window behind another has its bar grey) */
    uint32_t cursor;                    /* where its cursor is kept, as the row (a halfword; 0: not known) */
    int byte;                           /* (a byte there) */
    int sound;                          /* the sound of its cursor's step (EBOOT 08883988's), for the mouse's */
    Rect item[MAX_ITEMS];
} Group;

static Group drawn[MAX_GROUPS];         /* this game frame's */
static int ndrawn;
static Rect arrow[MAX_GROUPS][2];       /* this game frame's arrows, left and right */
static int narrows;

static Group *new_group(int kind) {
    if (ndrawn >= MAX_GROUPS)
        return NULL;
    Group *g = &drawn[ndrawn++];
    g->kind = kind;
    g->active = 1;
    g->cursor = 0;
    g->byte = 0;
    g->sound = 0x17;
    g->n = 0;
    g->sel = -1;
    return g;
}

void rt_menu_list(uint32_t layout, uint32_t row) {
    if ((int32_t)row < 0 || layout < 0x08800000u || layout >= 0x0A000000u - 0x24)
        return;
    int x = (int16_t)LH(layout + 0x1C), y = (int16_t)LH(layout + 0x1E), w = (int16_t)LH(layout + 0x20),
        pitch = (int16_t)LH(layout + 0x10), rows = (int16_t)LH(layout + 0x12);
    if (pitch <= 0 || rows <= 0 || rows > MAX_ITEMS || w <= 0 || (int)row >= rows)
        return;
    Group *g = new_group(0);
    if (!g)
        return;
    for (int i = 0; i < rows; i++)
        g->item[i] = (Rect){x, y + i * pitch, w, pitch};
    g->n = rows;
    g->sel = (int)row;
    /* the bar's colour (+0x18, red, green, blue): grey on the windows behind the one in use (an item's choices in
     * front of the item pouch, the yes or no in front of them...), which are drawn in no telling order */
    g->active = !(LBU(layout + 0x18) == LBU(layout + 0x19) && LBU(layout + 0x19) == LBU(layout + 0x1A));
}

/* An item of a list drawn one by one: the items drawn in a row (no other group between) are one list; its cursor (the
 * row it is on) kept at cursor */
void rt_menu_item(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int selected, uint32_t cursor) {
    Group *g = ndrawn && drawn[ndrawn - 1].kind == 1 ? &drawn[ndrawn - 1] : new_group(1);
    if (!g || g->n >= MAX_ITEMS)
        return;
    g->cursor = cursor;
    if (selected)
        g->sel = g->n;
    g->item[g->n++] = (Rect){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

/* A grid of slots, n of them in rows of cols, the first at x, y, each w x h and pitch_x, pitch_y from the next; the
 * cursor (the slot it is on) a byte at cursor, its step's sound the game's sound */
void rt_menu_grid(int x, int y, int pitch_x, int pitch_y, int w, int h, int cols, int n, uint32_t cursor, int sound) {
    int sel = (int)LBU(cursor);
    if (n <= 0 || n > MAX_ITEMS || cols <= 0 || sel >= n)
        return;
    Group *g = new_group(1);
    if (!g)
        return;
    for (int i = 0; i < n; i++)
        g->item[i] = (Rect){x + i % cols * pitch_x, y + i / cols * pitch_y, w, h};
    g->n = n;
    g->sel = sel;
    g->cursor = cursor;
    g->byte = 1;
    g->sound = sound;
}

void rt_menu_bar(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    Rect r = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    if (r.w <= 0 || r.h <= 0)
        return;
    /* a list window's own bar (drawn as part of it) is the window's */
    if (ndrawn && drawn[ndrawn - 1].kind == 0) {
        const Rect *s = &drawn[ndrawn - 1].item[drawn[ndrawn - 1].sel];
        if (s->x == r.x && s->y == r.y)
            return;
    }
    Group *g = new_group(2);
    if (!g)
        return;
    g->item[0] = r;
    g->n = 1;
    g->sel = 0;
}

/* A left and right arrow (a page's, a setting's), the left at x, y, the right w on: the arrows' sprite's size is the
 * game's (EBOOT 0882E604: halfwords +4, +6 of 0x08939970) */
void rt_menu_arrows(uint32_t x, uint32_t y, uint32_t w) {
    if (narrows >= MAX_GROUPS)
        return;
    int sw = (int16_t)LH(0x08939974u), sh = (int16_t)LH(0x08939976u);
    arrow[narrows][0] = (Rect){(int16_t)x, (int16_t)y, sw, sh};
    arrow[narrows][1] = (Rect){(int16_t)x + (int16_t)w, (int16_t)y, sw, sh};
    narrows++;
}

static int inside(const Rect *r, float x, float y) {
    return x >= (float)r->x && x < (float)(r->x + r->w) && y >= (float)r->y && y < (float)(r->y + r->h);
}

/* The same menu as before: as many items, the first where it was (its cursor may have moved, or slid it) */
static int same_group(const Group *a, const Group *b) {
    if (a->kind != b->kind || a->n != b->n)
        return 0;
    if (a->kind == 2)
        return a->item[0].w == b->item[0].w && a->item[0].h == b->item[0].h;
    return a->item[0].y == b->item[0].y && abs(a->item[0].x - b->item[0].x) <= 16;
}

/* MHFU_TEST_POINTER: the pointer from a frame on (PSP pixels), and a click there; several separated by ';' */
int menu_test_pointer(float *x, float *y, int *moved, int *clicks) {
    static int read, n;
    static struct { unsigned long from; float x, y; int click; } at[16];
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_POINTER");
        while (e && n < 16) {
            at[n].click = 0;
            if (sscanf(e, "%lu,%f,%f,%d", &at[n].from, &at[n].x, &at[n].y, &at[n].click) < 3)
                break;
            n++;
            e = strchr(e, ';');
            e = e ? e + 1 : NULL;
        }
    }
    unsigned long f = present_frame_number();
    int i = n - 1;
    while (i >= 0 && f < at[i].from)
        i--;
    if (i < 0)
        return 0;
    static int done = -1;               /* (each point taken once, even a frame late) */
    *x = at[i].x;
    *y = at[i].y;
    *moved = i > done;
    *clicks = i > done && at[i].click;
    done = i;
    return 1;
}

/* How far the cursor is from where it is going: in items, or (a bar alone) in pixels from the pointer */
static float distance(const Group *g, int target, float px, float py) {
    if (g->kind != 2)
        return (float)abs(target - g->sel);
    const Rect *r = &g->item[0];
    float dx = px < (float)r->x ? (float)r->x - px : px >= (float)(r->x + r->w) ? px - (float)(r->x + r->w) : 0;
    float dy = py < (float)r->y ? (float)r->y - py : py >= (float)(r->y + r->h) ? py - (float)(r->y + r->h) : 0;
    return sqrtf(dx * dx + dy * dy);
}

/* The direction to press to bring the cursor nearer; 0 when it is there (or the pointer is not in line with a bar) */
static uint32_t direction(const Group *g, int target, float px, float py) {
    if (g->kind != 2)
        return target > g->sel ? PSP_DOWN : target < g->sel ? PSP_UP : 0;
    const Rect *r = &g->item[0];
    if (px >= (float)r->x && px < (float)(r->x + r->w))
        return py < (float)r->y ? PSP_UP : py >= (float)(r->y + r->h) ? PSP_DOWN : 0;
    if (py >= (float)r->y && py < (float)(r->y + r->h))
        return px < (float)r->x ? PSP_LEFT : PSP_RIGHT;
    return 0;
}

static uint32_t opposite(uint32_t b) {
    return b == PSP_UP ? PSP_DOWN : b == PSP_DOWN ? PSP_UP : b == PSP_LEFT ? PSP_RIGHT : PSP_LEFT;
}

static int logging = -1;

/* A jump asked of the menus' cursor routine: from the row the cursor is on to the row pointed at, of a menu so many
 * rows long (wait: frames left to see it done) */
static struct { int from, to, rows, wait, done; } jump;

/* EBOOT 0881C2E4, a menu's cursor moved by up or down: the cursor's byte, how many rows, the buttons. Where it is the
 * menu in front (as many rows, the cursor where it is seen) and a jump is asked, the cursor is put a row short of the
 * one pointed at and the step to it is pressed - the routine moves it, sounds as for a step and says it moved */
static int sound_due;                   /* a cursor put elsewhere by menu_step: the step's sound to make (its number) */

/* The sound of the cursor stepping (as EBOOT 0881C2E4 makes it: 08883988 with the sound player, 0x17; the supply
 * box's 0x16), from the game's own context in the middle of its code: all of it as it was afterwards */
void f_EBOOT_08883988(Ctx *c);

void rt_menu_sound(Ctx *c) {
    if (!sound_due)
        return;
    int sound = sound_due;
    sound_due = 0;
    Ctx saved = *c;
    c->r[29] -= 0x20;
    SW(c->r[29], 0);
    c->r[4] = LW(0x08A62EACu);
    c->r[6] = (uint32_t)sound;
    c->r[5] = c->r[7] = c->r[8] = c->r[9] = c->r[10] = c->r[11] = 0;
    f_EBOOT_08883988(c);
    *c = saved;
}

/* The routine's last run (every frame for the menus it serves): the frame, how many rows - a jump is asked only of a
 * menu it is seen serving, the others (a yes or no...) stepped at once */
static struct { unsigned long frame; int rows; } routine = {(unsigned long)-1, 0};

void rt_menu_cursor(uint32_t cursor, uint32_t rows, uint32_t *buttons) {
    routine.frame = present_frame_number();
    routine.rows = (int)(rows & 0xFF);
    if (jump.wait <= 0 || jump.done || (int)(rows & 0xFF) != jump.rows || (int)LBU(cursor) != jump.from)
        return;
    SB(cursor, (uint32_t)(jump.to > jump.from ? jump.to - 1 : jump.to + 1));
    *buttons = jump.to > jump.from ? PSP_DOWN : PSP_UP;
    jump.done = 1;
    jump.wait = 3;                      /* (for the menu to be drawn so) */
}

/* Each game frame, with the input the game will see next (present.c): the buttons pressed for the pointer added */
static void menu_step(uint32_t *buttons);

void menu_input(uint32_t *buttons) {
    uint32_t before = *buttons;
    menu_step(buttons);
    if (logging > 0 && *buttons != before)
        fprintf(stderr, "menu: frame %lu, pressed %x\n", present_frame_number(), *buttons & ~before);
}

static void menu_step(uint32_t *buttons) {
    static Group menu;                  /* the group in front, as last drawn */
    static int open, absent;
    static int going, target, click;    /* going to the item target (or, a bar alone, to the pointer); then cross */
    static float tx, ty;                /* (the pointer, then) */
    static int pressing, press_frames, press_sel, rest, confirm, undo;
    static float press_from;            /* the distance when the step was pressed */
    static Rect press_rect;             /* where the cursor was then */
    static Rect settle;                 /* (where it was the frame before, a click waiting for it to stop) */
    static uint32_t press_button, step;  /* what is held; the last step towards the pointer */
    static int moved_here;              /* steps were made to get there */
    static int jump_tried;              /* the jump was asked for this target (else steps are pressed) */
    static uint32_t tap;                /* an arrow clicked: left or right, pressed for tap_frames more */
    static int tap_frames;
    if (logging < 0)
        logging = getenv("MHFU_MENU_LOG") != NULL;

    int front = -1;                     /* the last drawn of the groups in use, else the last drawn */
    for (int i = ndrawn - 1; i >= 0; i--)
        if (drawn[i].sel >= 0 && drawn[i].n > 0 && (front < 0 || (drawn[i].active && !drawn[front].active)))
            front = i;
    int have = front >= 0;
    if (have) {
        const Group *g = &drawn[front];
        const Rect *s = &g->item[g->sel];
        if (logging && (!open || !same_group(g, &menu) || g->sel != menu.sel))
            fprintf(stderr, "menu: frame %lu, %s of %d, the cursor on %d at %d,%d %dx%d\n", present_frame_number(),
                    g->kind == 0 ? "a window" : g->kind == 1 ? "a list" : "a bar", g->n, g->sel, s->x, s->y, s->w,
                    s->h);
        if (open && !same_group(g, &menu))
            going = click = pressing = undo = jump.wait = 0;
        menu = *g;
    }
    Rect arrows[MAX_GROUPS][2];
    int narrow = narrows;
    memcpy(arrows, arrow, sizeof arrows);
    ndrawn = narrows = 0;
    if (have) {
        absent = 0;
        if (!open) {
            open = 1;
            going = click = pressing = rest = confirm = undo = tap_frames = 0;
            const Rect *r = &menu.item[menu.sel];
            window_pointer_mode(1, (float)r->x + (float)r->w / 2, (float)r->y + (float)r->h / 2);
        }
    } else if (open && ++absent > 15) {     /* (a menu can go a few frames without being drawn, between screens) */
        open = 0;
        window_pointer_mode(0, 0, 0);
    }
    if (!open)
        return;

    float x, y;
    int moved, clicks, point = present_pointer(&x, &y, &moved, &clicks);
    if (point && clicks) {              /* an arrow clicked (a little round it too: they are small) */
        for (int i = 0; i < narrow && !tap_frames; i++)
            for (int k = 0; k < 2; k++) {
                const Rect *a = &arrows[i][k];
                Rect r = {a->x - 3, a->y - 3, a->w + 6, a->h + 6};
                if (inside(&r, x, y)) {
                    tap = k ? PSP_RIGHT : PSP_LEFT;
                    tap_frames = 2;
                    going = click = pressing = undo = 0;
                    break;
                }
            }
        if (tap_frames)
            moved = clicks = 0;
    }
    if (point && (moved || clicks)) {
        int over = -1;
        for (int i = 0; i < menu.n && over < 0; i++)
            if (inside(&menu.item[i], x, y))
                over = i;
        if (menu.kind == 2 && over < 0 && direction(&menu, 0, x, y))
            over = 0;                   /* (a bar alone: towards the pointer) */
        if (over >= 0) {
            going = 1;
            moved_here = jump_tried = 0;
            target = over;
            tx = x, ty = y;
            click = clicks > 0;
        }
    }
    if (*buttons & PSP_DPAD)            /* the keys or the pad move the cursor themselves */
        going = click = pressing = undo = tap_frames = jump.wait = 0;
    if (tap_frames > 0) {
        tap_frames--;
        *buttons |= tap;
        rest = 1;
        return;
    }

    if (confirm > 0) {                  /* cross, held for two frames once the cursor has settled (its slide done) */
        if (confirm > 2 && have) {
            const Rect *c = &menu.item[menu.sel];
            int still = c->x == settle.x && c->y == settle.y && c->w == settle.w && c->h == settle.h;
            settle = *c;
            confirm = still ? 1 : confirm - 1;
        } else
            confirm--;
        if (confirm < 2)
            *buttons |= PSP_CROSS;
        return;
    }
    if (!have)
        return;
    const Rect *cur = &menu.item[menu.sel];
    if (pressing) {
        /* held until the cursor moves (or the game has had time to move it), then let go */
        int still = cur->x == press_rect.x && cur->y == press_rect.y && menu.sel == press_sel;
        if (still && ++press_frames < 6) {
            *buttons |= press_button;
            return;
        }
        pressing = 0;
        rest = 1;
        if (going && distance(&menu, target, tx, ty) >= press_from) {
            going = click = 0;          /* no nearer: the list ends or wraps there */
            undo = !still;              /* (wrapped: back where it was) */
        }
    }
    if (rest > 0) {
        rest--;
        return;
    }
    if (jump.wait > 0) {                /* a jump asked: the cursor's routine to do it, the menu to show it */
        if (jump.done && menu.sel == jump.to)
            jump.wait = 0;
        else if (--jump.wait > 0 && going)
            return;
    }
    uint32_t dir = 0;
    if (going && !undo && !jump_tried && menu.kind != 2 && menu.sel != target) {
        jump_tried = 1;
        jump.from = menu.sel, jump.to = target, jump.rows = menu.n, jump.done = 0;
        if (menu.cursor && (menu.byte ? LBU(menu.cursor) : LH(menu.cursor)) == (uint32_t)menu.sel) {
            if (menu.byte)              /* the cursor's place known: put there */
                SB(menu.cursor, (uint32_t)target);
            else
                SH(menu.cursor, (uint32_t)target);
            sound_due = menu.sound;
            moved_here = 1;
            jump.done = 1;
            jump.wait = 3;
            return;
        }
        if (routine.frame + 2 >= present_frame_number() && routine.rows == menu.n) {
            moved_here = 1;
            jump.wait = 2;
            return;
        }                               /* (else stepped there) */
    }
    if (undo)
        dir = opposite(step), undo = 0;
    else if (going && (dir = direction(&menu, target, tx, ty)) != 0) {
        press_from = distance(&menu, target, tx, ty);
        step = dir;
        moved_here = 1;
    }
    if (dir) {
        press_button = dir;
        press_rect = *cur;
        press_sel = menu.sel;
        press_frames = 0;
        pressing = 1;
        *buttons |= dir;
        return;
    }
    if (going) {                        /* there */
        going = 0;
        if (click) {
            click = 0;
            confirm = moved_here ? 8 : 2;
            settle = *cur;
        }
    }
}

