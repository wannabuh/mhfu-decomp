/* The controls: the keyboard, the mouse and a gamepad, bound to actions - and the actions to the PSP's buttons, as the
 * game wants them at the moment (window.c reads the devices; what comes out is the buttons the game sees, recorded as
 * such: a recording replays as it was whatever the bindings).
 *
 * Most actions are a button or two held while their input is (attack: triangle; special: triangle + circle; sprint
 * and guard: R...). Where the PSP has one button do several things, an action of its own does one of them, the
 * buttons pressed for it by what the hunter is doing (his object, native.c's rt_local_player):
 *   - use item: square - with the weapon drawn (+0x410 bit 8) it only puts it away, as square does (the sword and
 *     shield: uses the item drawn), the item used with the next press;
 *   - sheathe: square, only with the weapon drawn (else it would use an item);
 *   - interact (talk, gather, carve, open a box): circle with the weapon away - drawn, it is put away first as for an
 *     item (circle with it drawn is an attack);
 *   - next and previous item: L held (the bar's choice opened at once: item_buttons), then square (next) or circle
 *     (previous) with it - the item bar's own chord, a step for each turn of the wheel.
 * The game takes a button only at its 30ths (native.c): what is pressed for an action is held for whole 30ths.
 *
 * In a menu (window.c's pointer, menu.c) the actions are the buttons they are made of (use item: square, interact:
 * circle, the wheel: up and down), and a gamepad is laid out as the PSP is (A cross, B circle, X square, Y triangle,
 * the shoulders L and R, the D-pad).
 *
 * The bindings: controls.ini, in the working directory or else $XDG_CONFIG_HOME/mhfu-recomp/ (~/.config/...) - written
 * there with the defaults when there is none. "[keyboard]" and "[gamepad]" sections of "action = input, input...":
 * keys by SDL's names ("W", "Left Shift", "Space"), "Mouse Left" / "Mouse Right" / "Mouse Middle" / "Mouse X1" /
 * "Mouse X2", "Wheel Up" / "Wheel Down"; a gamepad's buttons by SDL's names ("a", "b", "x", "y", "leftshoulder",
 * "rightshoulder", "dpup", "start", "back", "leftstick"...) and its triggers ("lefttrigger", "righttrigger"). An empty
 * value unbinds. "[options]": pad_camera_speed_x and pad_camera_speed_y (degrees a second the right stick turns the camera
 * fully pushed, across and up and down; pad_camera_speed sets both), pad_deadzone (0-1), mouse_invert_y and pad_invert_y
 * (the camera's tilt inverted, the mouse's and the stick's; invert_y sets both).
 *
 * The controls screen (F1, overlay.c) binds them again in the game, and saves them here.
 *
 * MHFU_TEST_ACTION="frame,action[,frames][;...]": the action's input held from the game's frame (for frames; default
 * 2) - for testing without a window. MHFU_CONTROLS_LOG=1 prints the buttons pressed. */
#include "hle.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "controls.h"

enum {
    PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40, PSP_LEFT = 0x80,
    PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000,
};

double rt_game_30ths(void);
unsigned long present_frame_number(void);
extern uint32_t rt_local_player;

/* The actions: their names (in controls.ini), and the buttons of those that are buttons held */
static const struct { const char *name, *label; uint32_t buttons; } actions[A_COUNT] = {
    [A_MOVE_UP] = {"move_up", "Move forward", 0},
    [A_MOVE_DOWN] = {"move_down", "Move back", 0},
    [A_MOVE_LEFT] = {"move_left", "Move left", 0},
    [A_MOVE_RIGHT] = {"move_right", "Move right", 0},
    [A_ATTACK] = {"attack", "Attack", PSP_TRIANGLE},
    [A_ATTACK2] = {"attack2", "Attack 2", PSP_CIRCLE},
    [A_SPECIAL] = {"special", "Special attack", PSP_TRIANGLE | PSP_CIRCLE},
    [A_ROLL] = {"roll", "Roll / dodge", PSP_CROSS},
    [A_SPRINT] = {"sprint", "Sprint / guard", PSP_R},
    [A_INTERACT] = {"interact", "Interact", 0},
    [A_USE_ITEM] = {"use_item", "Use item", 0},
    [A_SHEATHE] = {"sheathe", "Sheathe", 0},
    [A_ITEM_NEXT] = {"item_next", "Next item", 0},
    [A_ITEM_PREV] = {"item_prev", "Previous item", 0},
    [A_CAMERA_RESET] = {"camera_reset", "Reset camera", PSP_L},
    [A_MAP] = {"map", "Map (SELECT)", PSP_SELECT},
    [A_MENU] = {"menu", "Menu (START)", PSP_START},
    [A_CONFIRM] = {"confirm", "Confirm (menus)", PSP_CROSS},
    [A_BACK] = {"back", "Back (menus)", PSP_CIRCLE},
    [A_UP] = {"up", "D-pad up", PSP_UP},
    [A_DOWN] = {"down", "D-pad down", PSP_DOWN},
    [A_LEFT] = {"left", "D-pad left", PSP_LEFT},
    [A_RIGHT] = {"right", "D-pad right", PSP_RIGHT},
    [A_PSP_TRIANGLE] = {"psp_triangle", "PSP triangle", PSP_TRIANGLE},
    [A_PSP_CIRCLE] = {"psp_circle", "PSP circle", PSP_CIRCLE},
    [A_PSP_CROSS] = {"psp_cross", "PSP cross", PSP_CROSS},
    [A_PSP_SQUARE] = {"psp_square", "PSP square", PSP_SQUARE},
    [A_PSP_L] = {"psp_l", "PSP L", PSP_L},
    [A_PSP_R] = {"psp_r", "PSP R", PSP_R},
    [A_FREE_LOOK] = {"free_look", "Free look", 0},
};

/* An input: a key, a mouse button, the wheel, a gamepad's button or trigger (IN_*: controls.h) */
typedef struct { uint8_t kind; uint16_t code; } Input;

static Input bind[2][A_COUNT][MAX_BIND];     /* [0] the keyboard and mouse's, [1] the gamepad's */

typedef struct { int action; const char *inputs; } Default;

static const Default kb_defaults[] = {
    {A_MOVE_UP, "W"}, {A_MOVE_DOWN, "S"}, {A_MOVE_LEFT, "A"}, {A_MOVE_RIGHT, "D"},
    {A_ATTACK, "Mouse Left"}, {A_ATTACK2, "Mouse Right"}, {A_SPECIAL, "Left Ctrl"},
    {A_ROLL, "Space"}, {A_SPRINT, "Left Shift, Right Shift"}, {A_INTERACT, "F"}, {A_USE_ITEM, "R"},
    {A_SHEATHE, "X"}, {A_ITEM_NEXT, "Wheel Up"}, {A_ITEM_PREV, "Wheel Down"}, {A_CAMERA_RESET, "Tab"},
    {A_MAP, "M, C"}, {A_MENU, "Escape"}, {A_CONFIRM, "Return"}, {A_BACK, "Backspace"},
    {A_UP, "Up"}, {A_DOWN, "Down"}, {A_LEFT, "Left"}, {A_RIGHT, "Right"}, {A_PSP_R, "Mouse Middle"},
    {A_FREE_LOOK, "Left Alt"},
};

static const Default pad_defaults[] = {
    {A_ATTACK, "y"}, {A_ATTACK2, "b"}, {A_SPECIAL, "rightshoulder"}, {A_ROLL, "a"},
    {A_SPRINT, "righttrigger"}, {A_INTERACT, "lefttrigger"}, {A_USE_ITEM, "x"}, {A_SHEATHE, "dpdown"},
    {A_ITEM_NEXT, "dpright"}, {A_ITEM_PREV, "dpleft"}, {A_CAMERA_RESET, "leftshoulder, leftstick"},
    {A_MAP, "back, dpup"}, {A_MENU, "start"},
};

static float pad_speed_x = 200, pad_speed_y = 200, pad_deadzone = 0.2f;
static int invert_y[2];                 /* the mouse's, the stick's */

/* ---- names ---- */

static const char *mouse_names[] = {NULL, "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};

static int parse_input(const char *s, int pad, Input *in) {
    in->kind = IN_NONE;
    if (!pad) {
        for (int i = 1; i < 6; i++)
            if (!strcasecmp(s, mouse_names[i])) {
                *in = (Input){IN_MOUSE, (uint16_t)i};
                return 1;
            }
        if (!strcasecmp(s, "Wheel Up") || !strcasecmp(s, "Wheel Down")) {
            *in = (Input){IN_WHEEL, (uint16_t)(strcasecmp(s, "Wheel Up") != 0)};
            return 1;
        }
        SDL_Scancode k = SDL_GetScancodeFromName(s);
        if (k != SDL_SCANCODE_UNKNOWN) {
            *in = (Input){IN_KEY, (uint16_t)k};
            return 1;
        }
        return 0;
    }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s);
    if (b != SDL_GAMEPAD_BUTTON_INVALID) {
        *in = (Input){IN_PAD, (uint16_t)b};
        return 1;
    }
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s);
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        *in = (Input){IN_TRIGGER, (uint16_t)a};
        return 1;
    }
    return 0;
}

const char *controls_input_name(int pad, int action, int i) {
    const Input *in = &bind[pad][action][i];
    switch (in->kind) {
    case IN_KEY: return SDL_GetScancodeName((SDL_Scancode)in->code);
    case IN_MOUSE: return mouse_names[in->code];
    case IN_WHEEL: return in->code ? "Wheel Down" : "Wheel Up";
    case IN_PAD: return SDL_GetGamepadStringForButton((SDL_GamepadButton)in->code);
    case IN_TRIGGER: return SDL_GetGamepadStringForAxis((SDL_GamepadAxis)in->code);
    }
    return NULL;
}

const char *controls_action_name(int action) { return actions[action].name; }
const char *controls_action_label(int action) { return actions[action].label; }

/* An action's inputs from "input, input..." (replacing those it had) */
static void bind_list(int pad, int action, const char *list) {
    memset(bind[pad][action], 0, sizeof bind[pad][action]);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    int n = 0;
    for (char *tok = strtok(buf, ","); tok && n < MAX_BIND; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char *e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
            *--e = 0;
        if (!*tok)
            continue;
        if (parse_input(tok, pad, &bind[pad][action][n]))
            n++;
        else
            fprintf(stderr, "controls: no %s input \"%s\" (for %s)\n", pad ? "gamepad" : "keyboard or mouse", tok,
                    actions[action].name);
    }
}

void controls_defaults(void) {
    memset(bind, 0, sizeof bind);
    for (size_t i = 0; i < sizeof kb_defaults / sizeof kb_defaults[0]; i++)
        bind_list(0, kb_defaults[i].action, kb_defaults[i].inputs);
    for (size_t i = 0; i < sizeof pad_defaults / sizeof pad_defaults[0]; i++)
        bind_list(1, pad_defaults[i].action, pad_defaults[i].inputs);
    pad_speed_x = pad_speed_y = 200, pad_deadzone = 0.2f, invert_y[0] = invert_y[1] = 0;
}

/* Bind an input to an action (in place of its inputs, or added to them), taken from any other action of the same set
 * that had it */
void controls_bind(int pad, int action, int add, int kind, int code) {
    Input in = {(uint8_t)kind, (uint16_t)code};
    for (int a = 0; a < A_COUNT; a++)
        for (int i = 0; i < MAX_BIND; i++)
            if (bind[pad][a][i].kind == in.kind && bind[pad][a][i].code == in.code) {
                memmove(&bind[pad][a][i], &bind[pad][a][i + 1], (MAX_BIND - 1 - i) * sizeof(Input));
                memset(&bind[pad][a][MAX_BIND - 1], 0, sizeof(Input));
                i--;
            }
    if (!add)
        memset(bind[pad][action], 0, sizeof bind[pad][action]);
    int i = 0;
    while (i < MAX_BIND && bind[pad][action][i].kind)
        i++;
    if (i == MAX_BIND) {                /* (full: the last replaced) */
        i = MAX_BIND - 1;
    }
    bind[pad][action][i] = in;
}

void controls_clear(int pad, int action) { memset(bind[pad][action], 0, sizeof bind[pad][action]); }

/* The bindings as they were (the rebinding screen's undo) */
static Input kept[2][A_COUNT][MAX_BIND];
static float kept_speed_x, kept_speed_y, kept_deadzone;
static int kept_invert[2];
void controls_keep(void) {
    memcpy(kept, bind, sizeof kept);
    kept_speed_x = pad_speed_x, kept_speed_y = pad_speed_y, kept_deadzone = pad_deadzone, memcpy(kept_invert, invert_y, sizeof kept_invert);
}
void controls_undo(void) {
    memcpy(bind, kept, sizeof bind);
    pad_speed_x = kept_speed_x, pad_speed_y = kept_speed_y, pad_deadzone = kept_deadzone, memcpy(invert_y, kept_invert, sizeof invert_y);
}

float *controls_pad_camera_speed(int y) { return y ? &pad_speed_y : &pad_speed_x; }
float *controls_pad_deadzone(void) { return &pad_deadzone; }
int *controls_invert_y_setting(int pad) { return &invert_y[pad != 0]; }

/* ---- controls.ini ---- */

static char ini_path[4096];

static int config_path(char *path, size_t size) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(path, size, "%s/mhfu-recomp/controls.ini", xdg);
    else if (home)
        snprintf(path, size, "%s/.config/mhfu-recomp/controls.ini", home);
    else
        return 0;
    return 1;
}

static int load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[1024];
    int section = -1;                   /* 0 keyboard, 1 gamepad, 2 options */
    while (fgets(line, sizeof line, f)) {
        char *l = line;
        while (*l == ' ' || *l == '\t')
            l++;
        if (!*l || *l == '#' || *l == ';' || *l == '\n' || *l == '\r')
            continue;
        if (*l == '[') {
            section = !strncasecmp(l, "[keyboard]", 10) ? 0 : !strncasecmp(l, "[gamepad]", 9) ? 1
                    : !strncasecmp(l, "[options]", 9) ? 2 : -1;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq || section < 0)
            continue;
        *eq = 0;
        char *key = l, *value = eq + 1, *e = eq;
        while (e > key && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (section == 2) {
            if (!strcasecmp(key, "pad_camera_speed"))       /* (both: as written before they were apart) */
                pad_speed_x = pad_speed_y = (float)atof(value);
            else if (!strcasecmp(key, "pad_camera_speed_x"))
                pad_speed_x = (float)atof(value);
            else if (!strcasecmp(key, "pad_camera_speed_y"))
                pad_speed_y = (float)atof(value);
            else if (!strcasecmp(key, "pad_deadzone"))
                pad_deadzone = (float)atof(value);
            else if (!strcasecmp(key, "invert_y"))          /* (both: as written before they were apart) */
                invert_y[0] = invert_y[1] = atoi(value) != 0;
            else if (!strcasecmp(key, "mouse_invert_y"))
                invert_y[0] = atoi(value) != 0;
            else if (!strcasecmp(key, "pad_invert_y"))
                invert_y[1] = atoi(value) != 0;
            continue;
        }
        int a = 0;
        while (a < A_COUNT && strcasecmp(key, actions[a].name))
            a++;
        if (a == A_COUNT)
            fprintf(stderr, "controls: %s: no action \"%s\"\n", path, key);
        else
            bind_list(section, a, value);
    }
    fclose(f);
    return 1;
}

int controls_save(void) {
    if (!*ini_path && !config_path(ini_path, sizeof ini_path))
        return 0;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", ini_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        SDL_CreateDirectory(dir);
    }
    FILE *f = fopen(ini_path, "w");
    if (!f) {
        fprintf(stderr, "controls: cannot write %s\n", ini_path);
        return 0;
    }
    fprintf(f, "# MHFU's controls (runtime/src/controls.c): action = input, input... An empty value unbinds.\n"
               "# Keys by SDL's names (\"W\", \"Left Shift\", \"Space\"), Mouse Left/Right/Middle/X1/X2, Wheel Up/Down;\n"
               "# gamepad buttons a b x y leftshoulder rightshoulder lefttrigger righttrigger leftstick rightstick\n"
               "# dpup dpdown dpleft dpright start back. F1 in the game opens the controls screen.\n");
    for (int pad = 0; pad < 2; pad++) {
        fprintf(f, "\n[%s]\n", pad ? "gamepad" : "keyboard");
        for (int a = 0; a < A_COUNT; a++) {
            if (pad && a <= A_MOVE_RIGHT)
                continue;               /* (the left stick moves) */
            fprintf(f, "%s =", actions[a].name);
            for (int i = 0, n = 0; i < MAX_BIND; i++) {
                const char *name = controls_input_name(pad, a, i);
                if (name)
                    fprintf(f, "%s %s", n++ ? "," : "", name);
            }
            fputc('\n', f);
        }
    }
    fprintf(f, "\n[options]\npad_camera_speed_x = %g\npad_camera_speed_y = %g\npad_deadzone = %g\n"
            "mouse_invert_y = %d\npad_invert_y = %d\n", (double)pad_speed_x, (double)pad_speed_y, (double)pad_deadzone,
            invert_y[0], invert_y[1]);
    fclose(f);
    fprintf(stderr, "controls: saved %s\n", ini_path);
    return 1;
}

void controls_init(int write_defaults) {
    controls_defaults();
    if (load("controls.ini")) {
        snprintf(ini_path, sizeof ini_path, "controls.ini");
        return;
    }
    if (!config_path(ini_path, sizeof ini_path))
        return;
    if (!load(ini_path) && write_defaults)
        controls_save();
}

/* ---- the devices' state, as window.c gives it ---- */

static const bool *keys;
static SDL_MouseButtonFlags mouse;
static SDL_Gamepad *pad;
static int wheel_steps[2];              /* the wheel's turns not yet taken: up, down */

static int controls_input_held(int padset, const Input *in) {
    switch (in->kind) {
    case IN_KEY: return keys && keys[in->code];
    case IN_MOUSE: return (mouse & SDL_BUTTON_MASK(in->code)) != 0;
    case IN_PAD: return pad && SDL_GetGamepadButton(pad, (SDL_GamepadButton)in->code);
    case IN_TRIGGER: return pad && SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)in->code) > 12000;
    }
    (void)padset;
    return 0;
}

/* MHFU_TEST_ACTION: the actions held by the script at this frame */
static uint64_t test_actions(void) {
    static int read;
    static struct { unsigned long from, frames; int action; } list[32];
    static int n;
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_ACTION");
        for (const char *p = e; p && *p && n < 32;) {
            char name[64];
            unsigned long from, frames = 2;
            int got = sscanf(p, "%lu,%63[a-z0-9_],%lu", &from, name, &frames);
            if (got < 2)
                break;
            int a = 0;
            while (a < A_COUNT && strcmp(name, actions[a].name))
                a++;
            if (a < A_COUNT)
                list[n].from = from, list[n].frames = frames, list[n++].action = a;
            else
                fprintf(stderr, "MHFU_TEST_ACTION: no action %s\n", name);
            p = strchr(p, ';');
            p = p ? p + 1 : NULL;
        }
    }
    uint64_t held = 0;
    unsigned long f = present_frame_number();
    for (int i = 0; i < n; i++)
        if (f >= list[i].from && f < list[i].from + list[i].frames)
            held |= 1ull << list[i].action;
    return held;
}

/* ---- the hunter ---- */

static int hunter_drawn(uint32_t h) { return h && (LW(h + 0x410) & 8) != 0; }
static int hunter_free(uint32_t h) { return !h || (!hunter_drawn(h) && LBU(h + 0x685) == 0); }

/* ---- the actions done in steps (in the game's 30ths) ---- */

typedef struct {
    int on;                             /* 0: none */
    int stage;
    double t0;                          /* when the stage began */
    uint32_t last;                      /* (what it pressed: for a final press on the right button) */
} Steps;

static Steps item, smart;               /* the item bar's chord; use item / interact / sheathe */
static int item_queue;                  /* the item bar's steps to go: + next, - previous */
static int smart_action;

/* The item bar's chord: L, and once the game has seen it pressed its count of how long L is held (the hunter's +0x61C,
 * counted from 0 at the press: at 10 the bar's choice opens) set to 9 - a poke, recorded (present.c) - so that the
 * choice opens at the next 30th, not a third of a second later; then square (next) or circle (previous) with it for a
 * step - a 30th pressed, a 30th let go - for each step queued; L kept a little after the last (more turns of the wheel
 * go on at once), then let go */
#define ITEM_POKE 1
#define ITEM_LEAD 3
#define ITEM_PRESS 1
#define ITEM_LINGER 8
void present_poke_hunter(uint32_t offset, char type, uint32_t value);
static uint32_t item_buttons(double now, uint32_t h) {
    if (!item.on) {
        if (!item_queue)
            return 0;
        item.on = 1, item.stage = 0, item.t0 = now;     /* stage 0: the lead; 1: a step; 2: between, or lingering */
        item.last = 0;
    }
    double t = now - item.t0;
    switch (item.stage) {
    case 0:
        if (t >= ITEM_POKE && !item.last && h) {
            item.last = 1;              /* (poked) */
            if (LW(h + 0x61C) < 9)
                present_poke_hunter(0x61C, 'w', 9);
        }
        if (t < (h ? ITEM_LEAD : 11))
            return PSP_L;
        item.stage = 2, item.t0 = now - ITEM_PRESS, t = ITEM_PRESS;
        /* fall through */
    case 2:
        if (t < ITEM_PRESS)
            return PSP_L;
        if (item_queue) {
            item.last = item_queue > 0 ? PSP_SQUARE : PSP_CIRCLE;
            item_queue += item_queue > 0 ? -1 : 1;
            item.stage = 1, item.t0 = now;
            return PSP_L | item.last;
        }
        if (t < ITEM_PRESS + ITEM_LINGER)
            return PSP_L;
        item.on = 0;
        return 0;
    case 1:
        if (t < ITEM_PRESS)
            return PSP_L | item.last;
        item.stage = 2, item.t0 = now;
        return PSP_L;
    }
    item.on = 0;
    return 0;
}

/* Use item, interact, sheathe: stage 0 the weapon put away (square, two 30ths), 1 waiting for it to be away (or, for
 * use item, for the item to have been used drawn: it stays out), 2 the press for the action (two 30ths) */
static uint32_t smart_buttons(double now, uint32_t h) {
    if (!smart.on)
        return 0;
    double t = now - smart.t0;
    uint32_t final = smart_action == A_INTERACT ? PSP_CIRCLE : PSP_SQUARE;
    switch (smart.stage) {
    case 0:
        if (t < 2)
            return PSP_SQUARE;
        if (smart_action == A_SHEATHE) {
            smart.on = 0;
            return 0;
        }
        smart.stage = 1, smart.t0 = now;
        return 0;
    case 1:
        if (hunter_free(h)) {
            smart.stage = 2, smart.t0 = now;
            return final;
        }
        /* still drawn after a while: the item used with it out (or, a gun, reloaded) - nothing more */
        if ((hunter_drawn(h) && t > 15) || t > 75)
            smart.on = 0;
        return 0;
    case 2:
        if (t < 2)
            return final;
        smart.on = 0;
        return 0;
    }
    smart.on = 0;
    return 0;
}

static void smart_start(int action, double now, uint32_t h) {
    if (smart.on)
        return;
    if (action == A_USE_ITEM && hunter_drawn(h))
        action = A_SHEATHE;             /* (drawn: put away only - the next press uses the item) */
    smart_action = action;
    smart.on = 1, smart.t0 = now;
    if (action == A_SHEATHE)
        smart.stage = 0;
    else
        smart.stage = hunter_drawn(h) ? 0 : 2;
    if (action == A_SHEATHE && !hunter_drawn(h))
        smart.on = 0;
}

/* ---- each poll ---- */

static int last_device;                 /* 0 the keyboard and mouse, 1 a gamepad: the last used */
int controls_device(void) {
    static int test = -2;               /* MHFU_TEST_DEVICE=0/1: as if that device were the last used (for testing) */
    if (test == -2)
        test = getenv("MHFU_TEST_DEVICE") ? atoi(getenv("MHFU_TEST_DEVICE")) : -1;
    return test >= 0 ? test : last_device;
}
void controls_note_device(int d) { last_device = d; }

/* Out of the hunt (the village, its shops and counters, the title's menus) the gamepad's D-pad is the PSP's: what is
 * bound to it is the hunt's (sheathe, the item bar, the map), and there it was all the D-pad did wherever menu.c
 * did not know a menu for one - a shop's first choices, the quest list: down did nothing, left backed out (#51) */
static int dpad_plain;

static int on_dpad(const Input *in) {
    return in->kind == IN_PAD && in->code >= SDL_GAMEPAD_BUTTON_DPAD_UP && in->code <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}

static int held_by(int padset, int action) {
    for (int i = 0; i < MAX_BIND; i++)
        if (bind[padset][action][i].kind && bind[padset][action][i].kind != IN_WHEEL &&
            !(padset && dpad_plain && on_dpad(&bind[padset][action][i])) &&
            controls_input_held(padset, &bind[padset][action][i]))
            return 1;
    return 0;
}

/* The wheel's turns bound to an action (taken) */
static int wheel_turns(int action) {
    int n = 0;
    for (int i = 0; i < MAX_BIND; i++)
        if (bind[0][action][i].kind == IN_WHEEL) {
            n += wheel_steps[bind[0][action][i].code];
            wheel_steps[bind[0][action][i].code] = 0;
        }
    return n;
}

void controls_wheel(float y) {
    static float part;
    part += y;
    while (part >= 1) wheel_steps[0]++, part -= 1;
    while (part <= -1) wheel_steps[1]++, part += 1;
}

/* The gamepad, in a menu: as the PSP is laid out */
static uint32_t pad_menu_buttons(void) {
    static const struct { SDL_GamepadButton pad; uint32_t button; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PSP_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, PSP_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PSP_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, PSP_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PSP_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PSP_R},
        {SDL_GAMEPAD_BUTTON_START, PSP_START}, {SDL_GAMEPAD_BUTTON_BACK, PSP_SELECT},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PSP_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PSP_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PSP_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PSP_RIGHT},
    };
    uint32_t b = 0;
    if (pad)
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GetGamepadButton(pad, map[i].pad))
                b |= map[i].button;
    return b;
}

static int free_look;
int controls_free_look(void) { return free_look; }

uint32_t controls_buttons(const bool *k, SDL_MouseButtonFlags m, SDL_Gamepad *p, int in_menu, uint8_t *lx,
                          uint8_t *ly) {
    keys = k, mouse = m, pad = p;
    double now = rt_game_30ths();
    uint32_t h = rt_local_player;
    if (h && LW(h) != 0x089BB3CCu)      /* (the hunt's hunter, while there is one) */
        h = 0;
    uint64_t held = test_actions();
    dpad_plain = !h && !in_menu;
    for (int a = 0; a < A_COUNT; a++)
        if (held_by(0, a) || (!in_menu && held_by(1, a)))
            held |= 1ull << a;
    static uint64_t before;
    uint64_t pressed = held & ~before;
    before = held;
    free_look = !in_menu && (held >> A_FREE_LOOK & 1);
    int next = wheel_turns(A_ITEM_NEXT) + (int)((pressed >> A_ITEM_NEXT) & 1);
    int prev = wheel_turns(A_ITEM_PREV) + (int)((pressed >> A_ITEM_PREV) & 1);

    uint32_t b = 0;
    for (int a = 0; a < A_COUNT; a++)
        if (held >> a & 1)
            b |= actions[a].buttons;
    if (in_menu) {
        /* the actions as the buttons they are made of; the wheel steps the cursor */
        if (held >> A_USE_ITEM & 1 || held >> A_SHEATHE & 1)
            b |= PSP_SQUARE;
        if (held >> A_INTERACT & 1)
            b |= PSP_CIRCLE;
        static int tap, tap_frames;     /* (the cursor's step: two frames held, one let go) */
        if (!tap_frames && (next || prev)) {
            tap = next ? PSP_DOWN : PSP_UP, tap_frames = 3;
        }
        if (tap_frames > 0) {
            if (--tap_frames > 0)
                b |= (uint32_t)tap;
        }
        b |= pad_menu_buttons();
        item.on = smart.on = 0;
        item_queue = 0;
    } else {
        if (next || prev) {
            item_queue += next - prev;
            if (item_queue > 6) item_queue = 6;
            if (item_queue < -6) item_queue = -6;
        }
        for (int a = A_INTERACT; a <= A_SHEATHE; a++)
            if (pressed >> a & 1)
                smart_start(a, now, h);
        b |= item_buttons(now, h) | smart_buttons(now, h);
        if (dpad_plain && pad)
            b |= (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) ? PSP_UP : 0) |
                 (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? PSP_DOWN : 0) |
                 (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? PSP_LEFT : 0) |
                 (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? PSP_RIGHT : 0);
    }

    /* the stick: the keys, then the gamepad's left stick */
    int x = 128 + (held >> A_MOVE_RIGHT & 1 ? 127 : 0) - (held >> A_MOVE_LEFT & 1 ? 128 : 0);
    int y = 128 + (held >> A_MOVE_DOWN & 1 ? 127 : 0) - (held >> A_MOVE_UP & 1 ? 128 : 0);
    if (pad) {
        int sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        float r = sqrtf((float)sx * sx + (float)sy * sy) / 32767.0f;
        if (r > pad_deadzone) {
            x = (sx + 32768) >> 8;
            y = (sy + 32768) >> 8;
        }
    }
    *lx = (uint8_t)x;
    *ly = (uint8_t)y;
    static int logging = -1;            /* MHFU_CONTROLS_LOG=1: the buttons pressed, as they change */
    static uint32_t logged;
    if (logging < 0)
        logging = getenv("MHFU_CONTROLS_LOG") != NULL;
    if (logging && b != logged)
        fprintf(stderr, "controls: frame %lu (%.2f), buttons %x%s\n", present_frame_number(), now, b,
                h ? (hunter_drawn(h) ? " (drawn)" : hunter_free(h) ? " (free)" : " (busy)") : " (no hunter)");
    logged = b;
    return b;
}

/* The right stick's turn of the camera this long (seconds), in mouse counts (camera.c's sensitivity: degrees a count) */
void controls_pad_camera(SDL_Gamepad *p, float dt, float sens, float *dx, float *dy) {
    *dx = *dy = 0;
    if (!p || sens <= 0)
        return;
    float sx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
    float sy = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
    float r = sqrtf(sx * sx + sy * sy);
    if (r <= pad_deadzone)
        return;
    float k = (r - pad_deadzone) / (1 - pad_deadzone) / r;    /* (from the dead zone's edge, the turn's speed rising) */
    k *= k * r;                                                /* (squared: fine turns near the middle) */
    *dx = sx * k * pad_speed_x * dt / sens;
    *dy = sy * k * pad_speed_y * dt / sens;
}

int controls_invert_y(int pad) { return invert_y[pad != 0]; }
