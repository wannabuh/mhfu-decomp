/* The settings screen (F1, or "PC Settings" in the game's own menu: rt_pc_settings below), in three pages: the
 * graphics settings (settings.c's list: changed at once where they can be - the frame under the screen is drawn again
 * with them - and saved to settings.ini as the screen closes; those that wait for the next start say so), and the
 * controls - the actions and the inputs bound to them, the keyboard and mouse's or the gamepad's, to bind again.
 * Drawn by the runtime over the picture (render.c, as the button hints are: glyphs.c), the game paused
 * while it is screen_open (present.c). What is changed applies at once; closing the screen saves the controls to
 * controls.ini.
 *
 * With the keyboard: up and down (page up, page down) choose a row, Tab or left and right the page (left and right
 * change a setting's value), Enter binds the row's action to the next input pressed (in place of its inputs; + or
 * Insert adds it to them), Delete clears it, Esc or F1 closes. With the mouse: a row pointed at is chosen, a left
 * click binds (a right click adds), the wheel scrolls. With a gamepad: the D-pad, A binds, Y adds, X clears, the
 * shoulders change the set, B closes. While an input is awaited, Esc gives up (the gamepad's set: any button binds;
 * after five seconds it gives up). MHFU_TEST_OVERLAY=set,row draws it (0 the keyboard's, 1 the gamepad's; nothing
 * paused; 2: the graphics page) for testing. */
#include "hle.h"
#include "controls.h"
#include "glyphs.h"
#include "settings.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

void window_overlay_mode(int on);

enum { R_ACTION, R_OPTION, R_BUTTON, R_SETTING, R_HEADING };
enum { O_SPEED, O_SPEED_Y, O_DEADZONE, O_INVERT };
enum { B_DEFAULTS, B_UNDO, B_CLOSE };
typedef struct { int kind, which; } Row;

static int screen_open, set;                   /* set: 0 the keyboard and mouse, 1 the gamepad */
static int graphics;                    /* the graphics page (set: whose keys the foot of the screen names) */
#define TABS 3
#define TAB_W 144
static int tab(void) { return graphics ? 0 : 1 + set; }
static int row, top;                    /* the row chosen; the first shown */
static int awaiting, adding;            /* an input awaited for the row (added, not in place of its inputs) */
static uint64_t awaiting_since;
static Row rows[96];
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
    if (graphics) {
        for (int i = 0; i < settings_count(); i++)
            rows[nrows++] = (Row){settings_at(i)->kind == S_HEADING ? R_HEADING : R_SETTING, i};
        rows[nrows++] = (Row){R_BUTTON, B_UNDO};
        rows[nrows++] = (Row){R_BUTTON, B_CLOSE};
        if (row >= nrows)
            row = nrows - 1;
        return;
    }
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
    settings_save();
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
    graphics = 0;
    row = top = 0;
    build_rows();
    controls_keep();
    settings_keep();
    window_overlay_mode(1);
}

/* The page shown: 0 the graphics, 1 the keyboard and mouse's controls, 2 the gamepad's */
static void show_tab(int t) {
    t = (t % TABS + TABS) % TABS;
    graphics = t == 0;
    if (t)
        set = t - 1;
    row = top = 0;
    build_rows();
    if (rows[row].kind == R_HEADING)
        row++;
}

/* "PC Settings" picked in the game's menu (recomp/patches.py): the screen, on its graphics page */
void rt_pc_settings(void) {
    int window_settings_screen(void);
    int present_replaying(void);
    if (screen_open || present_replaying() || !window_settings_screen())
        return;
    overlay_toggle();
    show_tab(0);
}

/* (by: the way the row was come to, for stepping over a heading) */
static int moved_by;
static void scroll_to_row(void) {
    if (row < 0)
        row = 0;
    if (row >= nrows)
        row = nrows - 1;
    if (rows[row].kind == R_HEADING)
        row += moved_by < 0 && row > 0 ? -1 : 1;
    if (row > 0 && row == top + 1 && rows[top].kind == R_HEADING)
        ;                               /* (a page's first heading stays in sight) */
    else if (row > 0 && rows[row - 1].kind == R_HEADING && row - 1 < top)
        top = row - 1;
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
        *controls_invert_y_setting(set) ^= 1;
}

/* The row chosen, done: an action's input awaited, a setting changed, a button pressed */
static void activate(int add) {
    const Row *r = &rows[row];
    if (r->kind == R_ACTION) {
        awaiting = 1, adding = add;
        awaiting_since = SDL_GetTicksNS();
    } else if (r->kind == R_OPTION)
        change_option(r->which, 1);
    else if (r->kind == R_SETTING)
        settings_step(r->which, add ? -1 : 1);
    else if (r->kind == R_HEADING)
        ;
    else if (r->which == B_DEFAULTS)
        controls_defaults();
    else if (r->which == B_UNDO)
        graphics ? settings_undo() : controls_undo();
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

int window_panel_point(float wx, float wy, float *x, float *y);

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
        else if (k == SDL_SCANCODE_UP) row--, moved_by = -1;
        else if (k == SDL_SCANCODE_DOWN) row++, moved_by = 1;
        else if (k == SDL_SCANCODE_PAGEUP) row -= ROWS_SHOWN, moved_by = -1;
        else if (k == SDL_SCANCODE_PAGEDOWN) row += ROWS_SHOWN, moved_by = 1;
        else if (k == SDL_SCANCODE_HOME) row = 0, moved_by = 1;
        else if (k == SDL_SCANCODE_END) row = nrows - 1, moved_by = -1;
        else if ((k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) && rows[row].kind == R_OPTION)
            change_option(rows[row].which, k == SDL_SCANCODE_RIGHT ? 1 : -1);
        else if ((k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT) && rows[row].kind == R_SETTING)
            settings_step(rows[row].which, k == SDL_SCANCODE_RIGHT ? 1 : -1);
        else if (k == SDL_SCANCODE_TAB)
            show_tab(tab() + ((SDL_GetModState() & SDL_KMOD_SHIFT) ? -1 : 1));
        else if (k == SDL_SCANCODE_LEFT || k == SDL_SCANCODE_RIGHT)
            show_tab(tab() + (k == SDL_SCANCODE_RIGHT ? 1 : -1));
        else if ((k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_SPACE) && !e->key.repeat)
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
        if (window_panel_point(e->motion.x, e->motion.y, &x, &y)) {
            int r = row_at(x, y);
            if (r >= 0 && rows[r].kind != R_HEADING)
                row = r;
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float x, y;
        if (!window_panel_point(e->button.x, e->button.y, &x, &y))
            return 1;
        if (y >= 28 && y < 42 && x >= PANEL_X + 8 && x < PANEL_X + 8 + TABS * TAB_W) {     /* a tab */
            int t = (int)((x - PANEL_X - 8) / TAB_W);
            if (t != tab())
                show_tab(t);
            return 1;
        }
        int r = row_at(x, y);
        if (r >= 0 && rows[r].kind == R_SETTING) {
            row = r;
            if (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_RIGHT)
                settings_step(rows[row].which, e->button.button == SDL_BUTTON_LEFT ? 1 : -1);
        } else if (r >= 0 && rows[r].kind != R_HEADING) {
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
        if (rows[row].kind == R_HEADING)        /* (not on a heading) */
            row += row + 1 < nrows && row + 1 < top + ROWS_SHOWN ? 1 : -1;
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: row--, moved_by = -1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: row++, moved_by = 1; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            if (rows[row].kind == R_OPTION)
                change_option(rows[row].which, e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1);
            else if (rows[row].kind == R_SETTING)
                settings_step(rows[row].which, e->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: show_tab(tab() - 1); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: show_tab(tab() + 1); break;
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
    /* MHFU_TEST_OVERLAY_KEYS="DOWN,RIGHT,TAB,ESC...": pressed one after another while the screen is open, three a
     * second (UP DOWN LEFT RIGHT TAB ENTER ESC), for testing it without a keyboard */
    static const char *keys;
    static int read;
    static uint64_t next;
    if (!read) {
        read = 1;
        keys = getenv("MHFU_TEST_OVERLAY_KEYS");
    }
    if (keys && *keys && screen_open && SDL_GetTicksNS() >= next) {
        static const struct { const char *name; SDL_Scancode code; } names[] = {
            {"UP", SDL_SCANCODE_UP}, {"DOWN", SDL_SCANCODE_DOWN}, {"LEFT", SDL_SCANCODE_LEFT}, {"RIGHT", SDL_SCANCODE_RIGHT},
            {"TAB", SDL_SCANCODE_TAB}, {"ENTER", SDL_SCANCODE_RETURN}, {"ESC", SDL_SCANCODE_ESCAPE}};
        size_t n = strcspn(keys, ",");
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (strlen(names[i].name) == n && !strncmp(names[i].name, keys, n)) {
                SDL_Event e = {0};
                e.type = SDL_EVENT_KEY_DOWN;
                e.key.scancode = names[i].code;
                overlay_event(&e);
            }
        keys += n + (keys[n] == ',');
        next = SDL_GetTicksNS() + 330000000ull;
    }
}

/* ---- drawn ---- */

static void option_text(int which, char *out, size_t size) {
    if (which == O_SPEED || which == O_SPEED_Y)
        snprintf(out, size, "%d", (int)*controls_pad_camera_speed(which == O_SPEED_Y));
    else if (which == O_DEADZONE)
        snprintf(out, size, "%d", (int)(*controls_pad_deadzone() * 100 + 0.5f));
    else
        snprintf(out, size, "%s", *controls_invert_y_setting(set) ? "ON" : "OFF");
}

static const char *option_label(int which) {
    return which == O_SPEED     ? "CAMERA STICK SPEED ACROSS (DEGREES A SECOND)"
           : which == O_SPEED_Y ? "CAMERA STICK SPEED UP AND DOWN (DEGREES A SECOND)"
           : which == O_DEADZONE ? "STICK DEAD ZONE (HUNDREDTHS)"
           : set                 ? "INVERT CAMERA TILT (THE STICK)"
                                 : "INVERT CAMERA TILT (THE MOUSE)";
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
            graphics = set == 2;
            set = set == 1;
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
    pen_text(&p, 0, 0, "SETTINGS", C_WHITE);
    /* the pages, as tabs */
    for (int t = 0; t < TABS; t++) {
        p.x = PX(PANEL_X + 8 + TAB_W * t), p.y = PY(28);
        int w = (int)((TAB_W - 4) * scale) / dot, lit = t == tab();
        pen_rounded(&p, 0, 0, w, 12, lit ? C_LIT : C_GREY, lit ? C_LIT : C_PAD);
        pen_text(&p, 4, 3, t == 0 ? "GRAPHICS" : t == 1 ? "KEYBOARD AND MOUSE" : "GAMEPAD", lit ? C_INK : C_WHITE);
    }
    char value[48];
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
        } else if (r->kind == R_HEADING) {
            pen_text(&p, 0, 0, settings_at(r->which)->label, C_LIT);
            pen_box(&p, 6 * (int)strlen(settings_at(r->which)->label) + 4, 3, (int)(120 * scale) / dot, 1, C_GREY);
        } else if (r->kind == R_SETTING) {
            int waits = settings_shown(r->which, value, sizeof value);
            pen_text(&p, 6, 0, settings_at(r->which)->label, C_WHITE);
            p.x = PX(PANEL_X + 250);
            pen_text(&p, 0, 0, "\3", chosen ? C_LIT : C_GREY);
            pen_text(&p, 10, 0, value, C_WHITE);
            pen_text(&p, 12 + pen_text_width(value), 0, "\4", chosen ? C_LIT : C_GREY);
            if (waits) {
                p.x = PX(PANEL_X + 360);
                pen_text(&p, 0, 0, "NEXT START", C_YELLOW);
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
    pen_text(&p, 0, 0, graphics ? (set ? "D-PAD LEFT/RIGHT CHANGE  LB/RB PAGE  B SAVE AND CLOSE"
                                       : "LEFT/RIGHT OR CLICK CHANGE  TAB PAGE  ESC SAVE AND CLOSE")
                       : set    ? "A BIND  Y ADD  X CLEAR  LB/RB PAGE  B CLOSE"
                                : "ENTER/CLICK BIND  +/RIGHT CLICK ADD  DEL CLEAR  TAB PAGE  ESC CLOSE", C_GREY);
#undef PX
#undef PY
    return p.n;
}

/* ---- "PC Settings" in the game's own menu ----
 *
 * The menu START opens has two pages, and the last row of the second is empty: in the village 7 rows a page (drawn by
 * EBOOT 0883488C from the ids at 09B052A0, the cursor the HUD's byte +0x111C, picked by 08834D3C), in a quest 6 (drawn
 * by 08818C94 from the ids at 089B5600, the cursor +0x57D, picked by 08849120). Both draw the empty row grey and answer
 * a pick of it with the buzz. recomp/patches.py has that row drawn with our words, not grey; picking it opens this
 * screen (with the sound of a pick), and the note under the menu says what it is. The words are C strings as the
 * game's own are, kept in the PSP's memory below the game's (no save state or recording sees them). */
#define PC_TEXT 0x087F8000u             /* (above gpu.c's scratch) */
#define GAME_STATE_PTR 0x089CC558u      /* the game's state: its flags at +0x6AF14, bit 0 the village */

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u - 0x2000u;
}

static uint32_t pc_text(int note) {
    static const char label[] = "PC Settings", about[] = "Graphics and controls of the\nPC version.";
    char *at = (char *)PTR(PC_TEXT);
    if (memcmp(at, label, sizeof label)) {
        memcpy(at, label, sizeof label);
        memcpy(at + 0x40, about, sizeof about);
    }
    return PC_TEXT + (note ? 0x40u : 0);
}

uint32_t rt_pc_menu_label(void) {
    return pc_text(0);
}

/* The note under the menu (EBOOT 08819128, about to draw text for the HUD's menu): ours while the cursor is on the row */
uint32_t rt_pc_menu_note(uint32_t hud, uint32_t text) {
    uint32_t game = LW(GAME_STATE_PTR);
    if (!in_ram(hud) || !in_ram(game))
        return text;
    int village = LW(game + 0x6AF14u) & 1;
    int on_row = village ? LBU(hud + 0x111Du) && !LBU(hud + 0x1118u) && LBU(hud + 0x111Cu) == 13
                         : LBU(hud + 0x52Du) && !LBU(hud + 0x57Cu) && LBU(hud + 0x57Du) == 11;
    return on_row ? pc_text(1) : text;
}

static int pc_picked;

void rt_pc_menu_pick(void) {
    pc_picked = 1;
    rt_pc_settings();
}

/* The sound answering a pick the game refuses: the buzz (0x15) - or a pick's own (0x13), if it was our row */
uint32_t rt_pc_menu_sound(void) {
    int was = pc_picked;
    pc_picked = 0;
    return was ? 0x13u : 0x15u;
}
