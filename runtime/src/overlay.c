/* The controls screen (F1): the actions and the inputs bound to them, the keyboard and mouse's or the gamepad's, to
 * bind again - drawn by the runtime over the picture (render.c, as the button hints are: glyphs.c), the game paused
 * while it is screen_open (present.c). What is changed applies at once; closing the screen saves it to controls.ini.
 *
 * With the keyboard: up and down (page up, page down) choose a row, Tab or left and right the set (left and right
 * change a setting's value), Enter binds the row's action to the next input pressed (in place of its inputs; + or
 * Insert adds it to them), Delete clears it, Esc or F1 closes. With the mouse: a row pointed at is chosen, a left
 * click binds (a right click adds), the wheel scrolls. With a gamepad: the D-pad, A binds, Y adds, X clears, the
 * shoulders change the set, B closes. While an input is awaited, Esc gives up (the gamepad's set: any button binds;
 * after five seconds it gives up). MHFU_TEST_OVERLAY=set,row draws it (0 the keyboard's, 1 the gamepad's; nothing
 * paused) for testing. */
#include "hle.h"
#include "controls.h"
#include "glyphs.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

void window_overlay_mode(int on);

enum { R_ACTION, R_OPTION, R_BUTTON };
enum { O_SPEED, O_SPEED_Y, O_DEADZONE, O_INVERT };
enum { B_DEFAULTS, B_UNDO, B_CLOSE };
typedef struct { int kind, which; } Row;

static int screen_open, set;                   /* set: 0 the keyboard and mouse, 1 the gamepad */
static int row, top;                    /* the row chosen; the first shown */
static int awaiting, adding;            /* an input awaited for the row (added, not in place of its inputs) */
static uint64_t awaiting_since;
static Row rows[64];
static int nrows;

/* The layout, in PSP pixels */
#define PANEL_X 16
#define PANEL_Y 8
#define PANEL_W 448
#define PANEL_H 256
#define ROWS_Y 50
#define ROW_H 16
#define ROWS_SHOWN 12

static void build_rows(void) {
    nrows = 0;
    for (int a = 0; a < A_COUNT; a++)
        if (!(set && (a <= A_MOVE_RIGHT || a == A_CONFIRM || a == A_BACK)))   /* (a gamepad in a menu: as the PSP) */
            rows[nrows++] = (Row){R_ACTION, a};
    if (set) {
        rows[nrows++] = (Row){R_OPTION, O_SPEED};
        rows[nrows++] = (Row){R_OPTION, O_SPEED_Y};
        rows[nrows++] = (Row){R_OPTION, O_DEADZONE};
    }
    rows[nrows++] = (Row){R_OPTION, O_INVERT};
    rows[nrows++] = (Row){R_BUTTON, B_DEFAULTS};
    rows[nrows++] = (Row){R_BUTTON, B_UNDO};
    rows[nrows++] = (Row){R_BUTTON, B_CLOSE};
    if (row >= nrows)
        row = nrows - 1;
}

int overlay_open(void) { return screen_open; }

static void close_screen(void) {
    screen_open = awaiting = 0;
    controls_save();
    window_overlay_mode(0);
}

void overlay_toggle(void) {
    if (screen_open) {
        close_screen();
        return;
    }
    screen_open = 1;
    awaiting = 0;
    set = controls_device();
    row = top = 0;
    build_rows();
    controls_keep();
    window_overlay_mode(1);
}

static void scroll_to_row(void) {
    if (row < 0)
        row = 0;
    if (row >= nrows)
        row = nrows - 1;
    if (row < top)
        top = row;
    if (row >= top + ROWS_SHOWN)
        top = row - ROWS_SHOWN + 1;
}

static void change_option(int which, int dir) {
    if (which == O_SPEED || which == O_SPEED_Y) {
        float *v = controls_pad_camera_speed(which == O_SPEED_Y);
        *v += 20.0f * (float)dir;
        if (*v < 20) *v = 20;
        if (*v > 720) *v = 720;
    } else if (which == O_DEADZONE) {
        float *v = controls_pad_deadzone();
        *v += 0.05f * (float)dir;
        if (*v < 0.05f) *v = 0.05f;
        if (*v > 0.6f) *v = 0.6f;
    } else
        *controls_invert_y_setting() ^= 1;
}

/* The row chosen, done: an action's input awaited, a setting changed, a button pressed */
static void activate(int add) {
    const Row *r = &rows[row];
    if (r->kind == R_ACTION) {
        awaiting = 1, adding = add;
        awaiting_since = SDL_GetTicksNS();
    } else if (r->kind == R_OPTION)
        change_option(r->which, 1);
    else if (r->which == B_DEFAULTS)
        controls_defaults();
    else if (r->which == B_UNDO)
        controls_undo();
    else
        close_screen();
}

static void bind_awaited(int kind, int code) {
    controls_bind(set, rows[row].which, adding, kind, code);
    awaiting = 0;
}

/* The row at a point (PSP pixels; -1: none) */
static int row_at(float x, float y) {
    if (x < PANEL_X || x >= PANEL_X + PANEL_W || y < ROWS_Y || y >= ROWS_Y + ROWS_SHOWN * ROW_H)
        return -1;
    int r = top + (int)((y - ROWS_Y) / ROW_H);
    return r < nrows ? r : -1;
}

int window_point(float wx, float wy, float *x, float *y);

/* An event from the window while the screen is open: 1 if taken */
int overlay_event(const SDL_Event *e) {
    if (!screen_open)
        return 0;
    if (awaiting) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            if (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_F1)
                awaiting = 0;
            else if (!set)
                bind_awaited(IN_KEY, e->key.scancode);
            return 1;
        }
        /* (the click that asked for an input is let go of first: its button comes up before another goes down) */
        if (!set && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && SDL_GetTicksNS() - awaiting_since > 200000000u) {
            bind_awaited(IN_MOUSE, e->button.button);
            return 1;
        }
        if (!set && e->type == SDL_EVENT_MOUSE_WHEEL && e->wheel.y != 0) {
            float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
            bind_awaited(IN_WHEEL, y > 0 ? 0 : 1);
            return 1;
        }
        if (set && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            bind_awaited(IN_PAD, e->gbutton.button);
            return 1;
        }
        if (set && e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.value > 16000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            bind_awaited(IN_TRIGGER, e->gaxis.axis);
            return 1;
        }
        return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               e->type == SDL_EVENT_MOUSE_BUTTON_UP || e->type == SDL_EVENT_MOUSE_WHEEL ||
               e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode k = e->key.scancode;
        if (k == SDL_SCANCODE_ESCAPE || (k == SDL_SCANCODE_F1 && !e->key.repeat))
            close_screen();
        else if (k == SDL_SCANCODE_UP) row--;
        else if (k == SDL_SCANCODE_DOWN) row++;
        else if (k == SDL_SCANCODE_PAGEUP) row -= ROWS_SHOWN;
        else if (k == SDL_SCANCODE_PAGEDOWN) row += ROWS_SHOWN;
        else if (k == SDL_SCANCODE_HOME) row = 0;
        else if (k == SDL_SCANCODE_END) row = nrows - 1;
        else if ((k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) && rows[row].kind == R_OPTION)
            change_option(rows[row].which, k == SDL_SCANCODE_RIGHT ? 1 : -1);
        else if (k == SDL_SCANCODE_TAB || k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) {
            set ^= 1;
            build_rows();
        } else if ((k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE) && !e->key.repeat)
            activate(0);
        else if ((k == SDL_SCANCODE_INSERT || k == SDL_SCANCODE_EQUALS || k == SDL_SCANCODE_KP_PLUS) && !e->key.repeat &&
                 rows[row].kind == R_ACTION)
            activate(1);
        else if ((k == SDL_SCANCODE_DELETE || k == SDL_SCANCODE_BACKSPACE) && rows[row].kind == R_ACTION)
            controls_clear(set, rows[row].which);
        scroll_to_row();
        return 1;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float x, y;
        if (window_point(e->motion.x, e->motion.y, &x, &y)) {
            int r = row_at(x, y);
            if (r >= 0)
                row = r;
            /* (the tabs) */
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x, y;
        if (!window_point(e->button.x, e->button.y, &x, &y))
            return 1;
        if (y >= 28 && y < 42 && x >= PANEL_X + 8 && x < PANEL_X + 8 + 2 * 150) {     /* a tab */
            int s = (int)((x - PANEL_X - 8) / 150);
            if (s != set) {
                set = s;
                build_rows();
                scroll_to_row();
            }
            return 1;
        }
        int r = row_at(x, y);
        if (r >= 0) {
            row = r;
            if (e->button.button == SDL_BUTTON_LEFT)
                activate(0);
            else if (e->button.button == SDL_BUTTON_RIGHT && rows[row].kind == R_ACTION)
                activate(1);
            else if (e->button.button == SDL_BUTTON_RIGHT && rows[row].kind == R_OPTION)
                change_option(rows[row].which, -1);
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        top -= (int)(e->wheel.y * (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
        if (top > nrows - ROWS_SHOWN) top = nrows - ROWS_SHOWN;
        if (top < 0) top = 0;
        if (row < top) row = top;
        if (row >= top + ROWS_SHOWN) row = top + ROWS_SHOWN - 1;
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: row--; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: row++; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            if (rows[row].kind == R_OPTION)
                change_option(rows[row].which, e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: set ^= 1; build_rows(); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: activate(0); break;
        case SDL_GAMEPAD_BUTTON_NORTH: if (rows[row].kind == R_ACTION) activate(1); break;
        case SDL_GAMEPAD_BUTTON_WEST: if (rows[row].kind == R_ACTION) controls_clear(set, rows[row].which); break;
        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START: close_screen(); break;
        }
        scroll_to_row();
        return 1;
    case SDL_EVENT_KEY_UP: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return 1;
    }
    return 0;
}

/* Each moment while open: an input awaited too long given up */
void overlay_tick(void) {
    if (awaiting && SDL_GetTicksNS() - awaiting_since > 5000000000ull)
        awaiting = 0;
}

/* ---- drawn ---- */

static void option_text(int which, char *out, size_t size) {
    if (which == O_SPEED || which == O_SPEED_Y)
        snprintf(out, size, "%d", (int)*controls_pad_camera_speed(which == O_SPEED_Y));
    else if (which == O_DEADZONE)
        snprintf(out, size, "%d", (int)(*controls_pad_deadzone() * 100 + 0.5f));
    else
        snprintf(out, size, "%s", *controls_invert_y_setting() ? "ON" : "OFF");
}

static const char *option_label(int which) {
    return which == O_SPEED     ? "CAMERA STICK SPEED ACROSS (DEGREES A SECOND)"
           : which == O_SPEED_Y ? "CAMERA STICK SPEED UP AND DOWN (DEGREES A SECOND)"
           : which == O_DEADZONE ? "STICK DEAD ZONE (HUNDREDTHS)"
                                 : "INVERT CAMERA TILT";
}

static const char *button_label(int which) {
    return which == B_DEFAULTS ? "RESET TO THE DEFAULTS" : which == B_UNDO ? "UNDO THE CHANGES" : "SAVE AND CLOSE";
}

/* The screen over a picture at (x0, y0), scale window pixels a PSP pixel: into out (at most max); how many */
int overlay_rects(float x0, float y0, float scale, HintRect *out, int max) {
    static int test = -1;               /* MHFU_TEST_OVERLAY=set,row: the screen drawn (not open: nothing paused) */
    if (test < 0) {
        test = getenv("MHFU_TEST_OVERLAY") != NULL;
        if (test) {
            sscanf(getenv("MHFU_TEST_OVERLAY"), "%d,%d", &set, &row);
            build_rows();
            scroll_to_row();
        }
    }
    if (!screen_open && !test)
        return 0;
    int dot = (int)scale < 1 ? 1 : (int)scale;
#define PX(v) ((int)(x0 + (float)(v) * scale))
#define PY(v) ((int)(y0 + (float)(v) * scale))
    Pen p = {out, 0, max, dot, PX(PANEL_X), PY(PANEL_Y)};
    int pw = (int)(PANEL_W * scale) / dot, ph = (int)(PANEL_H * scale) / dot;
    pen_rounded(&p, 0, 0, pw, ph, C_GREY, C_INK);
    p.x = PX(PANEL_X + 8), p.y = PY(PANEL_Y + 6);
    pen_text(&p, 0, 0, "CONTROLS", C_WHITE);
    /* the sets, as tabs */
    for (int s = 0; s < 2; s++) {
        p.x = PX(PANEL_X + 8 + 150 * s), p.y = PY(28);
        int w = (int)(146 * scale) / dot;
        pen_rounded(&p, 0, 0, w, 12, s == set ? C_LIT : C_GREY, s == set ? C_LIT : C_PAD);
        pen_text(&p, 4, 3, s ? "GAMEPAD" : "KEYBOARD AND MOUSE", s == set ? C_INK : C_WHITE);
    }
    char value[32];
    for (int i = 0; i < ROWS_SHOWN && top + i < nrows; i++) {
        const Row *r = &rows[top + i];
        int y = ROWS_Y + i * ROW_H, chosen = top + i == row;
        p.x = PX(PANEL_X + 4), p.y = PY(y);
        if (chosen)
            pen_rounded(&p, 0, 0, (int)((PANEL_W - 8) * scale) / dot, (int)(ROW_H * scale) / dot - 1,
                        awaiting ? C_LIT : C_PAD, C_PAD);
        p.x = PX(PANEL_X + 10), p.y = PY(y) + ((int)(ROW_H * scale) / dot - 7) / 2 * dot;
        if (r->kind == R_ACTION) {
            char label[64];
            snprintf(label, sizeof label, "%s", controls_action_label(r->which));
            pen_text(&p, 0, 0, label, C_WHITE);
            p.x = PX(PANEL_X + 200);
            if (chosen && awaiting) {
                pen_text(&p, 0, 0, set ? "PRESS A BUTTON ..." : "PRESS A KEY OR MOUSE BUTTON ... (ESC: NO)", C_LIT);
                continue;
            }
            /* the inputs' pictures, side by side */
            int cy = (int)(ROW_H * scale) / dot / 2, x = 0, any = 0;
            p.y = PY(y);
            for (int k = 0; k < MAX_BIND; k++) {
                const char *name = controls_input_name(set, r->which, k);
                if (!name)
                    continue;
                int w = pen_input_width(name, set);
                pen_input(&p, name, set, x + w / 2, cy);
                x += w + 4;
                any = 1;
            }
            if (!any) {
                p.y = PY(y) + ((int)(ROW_H * scale) / dot - 7) / 2 * dot;
                pen_text(&p, 0, 0, "-", C_GREY);
            }
        } else if (r->kind == R_OPTION) {
            pen_text(&p, 0, 0, option_label(r->which), C_WHITE);
            option_text(r->which, value, sizeof value);
            p.x = PX(PANEL_X + 380);
            pen_text(&p, 0, 0, "\3", chosen ? C_LIT : C_GREY);
            pen_text(&p, 10, 0, value, C_WHITE);
            pen_text(&p, 12 + pen_text_width(value), 0, "\4", chosen ? C_LIT : C_GREY);
        } else
            pen_text(&p, 0, 0, button_label(r->which), chosen ? C_LIT : C_FACE);
    }
    /* the scroll's place, and what the keys do */
    if (nrows > ROWS_SHOWN) {
        int track = ROWS_SHOWN * ROW_H, bar = track * ROWS_SHOWN / nrows;
        p.x = PX(PANEL_X + PANEL_W - 6), p.y = PY(ROWS_Y + track * top / nrows);
        pen_box(&p, 0, 0, 2, (int)(bar * scale) / dot, C_GREY);
    }
    p.x = PX(PANEL_X + 8), p.y = PY(PANEL_Y + PANEL_H - 14);
    pen_text(&p, 0, 0, set ? "A BIND  Y ADD  X CLEAR  LB/RB SET  B CLOSE"
                           : "ENTER/CLICK BIND  +/RIGHT CLICK ADD  DEL CLEAR  TAB SET  ESC CLOSE", C_GREY);
#undef PX
#undef PY
    return p.n;
}
