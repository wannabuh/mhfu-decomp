/* The window and input (SDL3): each frame the game flips is shown, scaled to the window; the keyboard and any gamepad
 * are read into the PSP's buttons and analog stick. MHFU_HEADLESS=1 runs without a window (tests, scripted runs).
 *
 * Drawing is the Vulkan renderer's (render.c), into this window or offscreen; with MHFU_RENDERER=soft, or without
 * Vulkan, the software GE draws into VRAM and its frames are shown here through SDL's renderer.
 *
 * The keyboard, the mouse and a gamepad are bound to actions, the actions pressing the PSP's buttons as the game wants
 * them (controls.c: controls.ini). The mouse turns the camera (camera.c), and so does a gamepad's right stick; the
 * wheel goes through the items. While a menu is open (menu.c) the mouse is a pointer instead: over a row it moves the
 * game's cursor there, a left click picks it (cross), a right click goes back (circle). F1 opens the controls screen
 * (overlay.c), the game paused while it is open. */
#include "ticket.h"
#include "hle.h"
#include "render.h"
#include "savestate.h"
#include "controls.h"
#include "perf.h"
#include "capture.h"
#include "inspect.h"
#include "hitview.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>


enum {
    PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40, PSP_LEFT = 0x80,
    PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000,
};

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Gamepad *pad;
static int disabled = 1;
static uint32_t pixels[480 * 272];
static uint32_t device_buttons;
static uint8_t device_lx = 128, device_ly = 128;     /* the stick: the game reads it at frames (present.c) */
static int mouse_captured;
static float mouse_dx, mouse_dy;         /* motion since the game last took it */
/* Camera smoothing, the mouse's and the right stick's apart (MHFU_MOUSE_SMOOTHING, MHFU_PAD_SMOOTHING, ms; either
 * unset: MHFU_CAMERA_SMOOTHING, for both; 0 or none: none): the motion is let through a share at a time, the rest held
 * back, so that the camera eases into a turn and out of it; all of it gets through in the end, so the camera comes to
 * rest where it would have without. The time is how long it takes to let through about two thirds of a motion (e.g. 30
 * light, 80 heavy). The game is given only what is let through, so a recording has the smoothed motion and replays it
 * as it was. */
enum { FROM_MOUSE, FROM_PAD };
static float held_dx[2], held_dy[2];     /* motion not let through yet, by where it came from */
static float smoothing_ms[2] = {-1, -1};
static int want_capture = 1;             /* the player's choice: captured unless F10 or the focus lost let it go */
static int pointer_mode;                 /* a menu is open: the mouse is a pointer (menu.c) */
static int overlay_mode;                 /* the controls screen is open (overlay.c): the mouse a pointer */
static int inspect_mode;                 /* the click inspector is open (inspect.c): the mouse its pointer */
static int ticket_mode;                  /* the ticket's note box is open (ticket.c): the keys type, the mouse is let go */
static float pointer_x, pointer_y;       /* where it is, in the window's points */
static int pointer_moved, pointer_clicks;

/* The mouse drives the camera while it is captured: from the start, toggled with F10, released when the window loses
 * focus (and taken again when it is clicked), and while a menu is open, where it is a pointer */
static void apply_capture(void) {
    if (!window)
        return;
    mouse_captured = want_capture && !pointer_mode && !overlay_mode && !inspect_mode && !ticket_mode;
    if (!mouse_captured)                 /* (a menu opened: the rest of the turn isn't the camera's) */
        memset(held_dx, 0, sizeof held_dx), memset(held_dy, 0, sizeof held_dy);
    SDL_SetWindowRelativeMouseMode(window, mouse_captured);
}

static void capture_mouse(int on) {
    want_capture = on;
    apply_capture();
}

/* The picture's place in the window (points): letterboxed to the PSP's shape, centred (as render.c presents it) */
static int picture(float *x0, float *y0, float *scale) {
    int w, h;
    if (!window || !SDL_GetWindowSize(window, &w, &h) || w <= 0 || h <= 0)
        return 0;
    float vw = (float)w, vh = (float)w * 272.0f / 480.0f;
    if (vh > (float)h) {
        vh = (float)h;
        vw = (float)h * 480.0f / 272.0f;
    }
    *x0 = ((float)w - vw) / 2;
    *y0 = ((float)h - vh) / 2;
    *scale = vw / 480.0f;
    return 1;
}

/* A menu opened (at: the point on the PSP's screen to put the pointer, its cursor) or closed */
void window_pointer_mode(int on, float at_x, float at_y) {
    if (on == pointer_mode)
        return;
    pointer_mode = on;
    pointer_moved = pointer_clicks = 0;
    apply_capture();
    float x0, y0, k;
    if (on && window && want_capture && picture(&x0, &y0, &k)) {
        pointer_x = x0 + at_x * k;
        pointer_y = y0 + at_y * k;
        SDL_WarpMouseInWindow(window, pointer_x, pointer_y);
    }
}

/* The controls screen opened or closed (overlay.c) */
void window_overlay_mode(int on) {
    overlay_mode = on;
    apply_capture();
}

void window_note(const char *text);
/* overlay.c: whether the settings screen can be shown (a window, and not the software renderer's picture: nothing is
 * drawn over that) */
int window_settings_screen(void) {
    if (!window)
        return 0;
    if (renderer) {
        window_note("the settings screen needs the Vulkan renderer: settings.ini, controls.ini");
        return 0;
    }
    return 1;
}

/* settings.c: the whole screen, or a window again */
void window_set_fullscreen(int on) {
    if (window)
        SDL_SetWindowFullscreen(window, on != 0);
}

/* The click inspector opened or closed (inspect.c) */
void window_inspect_mode(int on) {
    inspect_mode = on;
    apply_capture();
}

/* The ticket's note box opened or closed (ticket.c) */
void window_ticket_mode(int on) {
    ticket_mode = on;
    apply_capture();
    if (window) {
        if (on)
            SDL_StartTextInput(window);
        else
            SDL_StopTextInput(window);
    }
}

/* Where the mouse is in the window (its points); 0 if there is no window */
int window_mouse_position(float *x, float *y) {
    if (!window)
        return 0;
    SDL_GetMouseState(x, y);
    return 1;
}

/* A point in the window (its points) on the PSP's screen; 0 if there is no window */
int window_point(float wx, float wy, float *x, float *y) {
    float x0, y0, k;
    if (!picture(&x0, &y0, &k))
        return 0;
    *x = (wx - x0) / k;
    *y = (wy - y0) / k;
    return 1;
}

/* Whether a menu has the pointer (menu.c) */
int window_in_menu(void) { return pointer_mode; }

/* The pointer on the PSP's screen; 0 if there is none (no window, or not in pointer mode) */
int window_pointer(float *x, float *y) {
    float x0, y0, k;
    if (!pointer_mode || !picture(&x0, &y0, &k))
        return 0;
    *x = (pointer_x - x0) / k;
    *y = (pointer_y - y0) / k;
    return 1;
}

/* Whether the pointer moved, and the left clicks, since last asked */
int window_pointer_moved(void) {
    int m = pointer_moved;
    pointer_moved = 0;
    return m;
}

int window_pointer_clicks(void) {
    int n = pointer_clicks;
    pointer_clicks = 0;
    return n;
}

static int enabled(void);

/* The free camera's keys: how fast along each way (units a second) */
void window_free_keys(float *right, float *up, float *front) {
    *right = *up = *front = 0;
    if (!enabled())
        return;
    const bool *k = SDL_GetKeyboardState(NULL);
    float speed = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] ? 2400.0f : 600.0f;
    *right = speed * ((float)k[SDL_SCANCODE_D] - (float)k[SDL_SCANCODE_A]);
    *up = speed * ((float)k[SDL_SCANCODE_E] - (float)k[SDL_SCANCODE_Q]);
    *front = speed * ((float)k[SDL_SCANCODE_W] - (float)k[SDL_SCANCODE_S]);
}

/* A smoothing time (ms) from the environment: name's, else MHFU_CAMERA_SMOOTHING's, else none */
static float smoothing_setting(const char *name) {
    const char *e = getenv(name);
    if (!e)
        e = getenv("MHFU_CAMERA_SMOOTHING");
    float ms = e ? (float)atof(e) : 0;
    return ms > 0 ? ms : 0;
}

/* Motion for the camera, from the mouse or the stick: at once, or held to be let through by smooth_motion */
static void add_motion(int from, float dx, float dy) {
    if (smoothing_ms[from] < 0)
        smoothing_ms[from] = smoothing_setting(from == FROM_MOUSE ? "MHFU_MOUSE_SMOOTHING" : "MHFU_PAD_SMOOTHING");
    if (smoothing_ms[from] > 0) {
        held_dx[from] += dx;
        held_dy[from] += dy;
    } else {
        mouse_dx += dx;
        mouse_dy += dy;
    }
}

/* Each poll: the share of the motion held that the time since the last lets through */
static void smooth_motion(void) {
    static uint64_t last;
    uint64_t now = time_us();
    float dt = last ? (float)(now - last) / 1000.0f : 0;     /* (ms) */
    last = now;
    for (int from = 0; from < 2; from++) {
        if (smoothing_ms[from] <= 0 || (held_dx[from] == 0 && held_dy[from] == 0))
            continue;
        float k = 1.0f - expf(-dt / smoothing_ms[from]);
        if (fabsf(held_dx[from]) + fabsf(held_dy[from]) < 0.05f)
            k = 1;                      /* (the last of it: at once, not forever) */
        mouse_dx += held_dx[from] * k;
        mouse_dy += held_dy[from] * k;
        held_dx[from] -= held_dx[from] * k;
        held_dy[from] -= held_dy[from] * k;
    }
}

void window_mouse_peek(float *dx, float *dy) {
    *dx = mouse_dx;
    *dy = mouse_dy;
}

void window_mouse(float *dx, float *dy) {
    *dx = mouse_dx;
    *dy = mouse_dy;
    mouse_dx = mouse_dy = 0;
}

void window_init(void) {
    int headless = getenv("MHFU_HEADLESS") != NULL;
    const char *r = getenv("MHFU_RENDERER");
    int want_vulkan = !(r && !strcmp(r, "soft"));
    disabled = headless;
    if (!headless || (getenv("MHFU_CONSOLE") && atoi(getenv("MHFU_CONSOLE"))))
        console_start();
    controls_init(!headless);
    if (!headless && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "no window: %s\n", SDL_GetError());
        disabled = headless = 1;
    }
    if (want_vulkan) {
        if (!headless) {
            window = SDL_CreateWindow("Monster Hunter Freedom Unite", 480 * 2, 272 * 2,
                                      SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
            /* MHFU_FULLSCREEN=1: the whole screen from the start (F11 toggles it) */
            if (window && getenv("MHFU_FULLSCREEN") && atoi(getenv("MHFU_FULLSCREEN")))
                SDL_SetWindowFullscreen(window, true);
            /* no MHFU_FPS: the screen's own refresh rate */
            const SDL_DisplayMode *mode = window && !getenv("MHFU_FPS") ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : NULL;
            if (mode && mode->refresh_rate >= 1) {
                char hz[16];
                snprintf(hz, sizeof hz, "%d", (int)(mode->refresh_rate + 0.5f));
                setenv("MHFU_FPS", hz, 0);
            }
        }
        if (render_init(window)) {
            capture_mouse(1);
            return;
        }
        fprintf(stderr, "renderer: no Vulkan, drawing in software\n");
        if (window)
            SDL_DestroyWindow(window);
        window = NULL;
    }
    if (!headless) {
        window = SDL_CreateWindow("Monster Hunter Freedom Unite", 480 * 2, 272 * 2, SDL_WINDOW_RESIZABLE);
        renderer = SDL_CreateRenderer(window, NULL);
        SDL_SetRenderLogicalPresentation(renderer, 480, 272, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
}

float camera_sens(void);
int overlay_open(void);
void overlay_toggle(void);
int overlay_event(const SDL_Event *e);
void overlay_tick(void);

static int enabled(void) { return !disabled; }

/* F9: the game's RAM to build/snap-<time>.bin - for finding the game's variables by comparing snapshots (named by
 * the time, so runs never overwrite each other's) */
static void ram_snapshot(void) {
    static int n;
    char path[64];
    snprintf(path, sizeof path, "build/snap-%llu-%02d.bin", (unsigned long long)time(NULL), ++n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot write %s\n", path);
        return;
    }
    fwrite(MEM(0x08800000u), 1, 0x01800000u, f);
    fclose(f);
    fprintf(stderr, "snapshot %d saved (%s)\n", n, path);
}

/* Read input; returns 0 if the window was closed */
/* MHFU_TEST_MOUSE="frame,vx,vy[,frames][;...]": from the game's frame, move the mouse at vx, vy counts a second (for so
 * many frames), as the window would - for testing the camera without a hand on the mouse */
unsigned long present_frame_number(void);

static void test_mouse(void) {
    static int read, n;
    static struct { unsigned long from, frames; float vx, vy; } list[16];
    static uint64_t last;
    if (!read) {
        read = 1;
        for (const char *e = getenv("MHFU_TEST_MOUSE"); e && *e && n < 16;) {
            list[n].frames = ~0ul;
            if (sscanf(e, "%lu,%f,%f,%lu", &list[n].from, &list[n].vx, &list[n].vy, &list[n].frames) < 3)
                break;
            n++;
            e = strchr(e, ';');
            if (e)
                e++;
        }
    }
    unsigned long f = present_frame_number();
    uint64_t now = time_us();
    for (int i = 0; i < n && last; i++)
        if (f >= list[i].from && f - list[i].from < list[i].frames) {
            float dt = (float)(now - last) / 1e6f;
            add_motion(FROM_MOUSE, list[i].vx * dt, list[i].vy * dt);
        }
    last = now;
}

/* A note in the window's title (the save state slot, a state saved or loaded) */
void window_note(const char *text) {
    render_note(text);
    if (!window)
        return;
    char title[256];
    snprintf(title, sizeof title, "Monster Hunter Freedom Unite - %s", text);
    SDL_SetWindowTitle(window, title);
}

/* Save states: F2 saves into the slot, F4 loads it, F3 the next slot (as PPSSPP has them) */
static int state_slot;

static void state_key(SDL_Scancode key) {
    char note[64];
    if (key == SDL_SCANCODE_F2)
        state_request_save(state_slot);
    else if (key == SDL_SCANCODE_F4)
        state_request_load(state_slot);
    else if (key == SDL_SCANCODE_F3) {
        state_slot = (state_slot + 1) % (ST_SLOTS - 1);
        snprintf(note, sizeof note, "save state slot %d%s", state_slot + 1, state_slot_used(state_slot) ? "" : " (empty)");
        window_note(note);
    }
}

/* The right stick turns the camera as the mouse does (its motion added to the mouse's, by the time since the last) */
static void pad_camera(void) {
    static uint64_t last;
    uint64_t now = SDL_GetTicksNS();
    float dt = last ? (float)(now - last) / 1e9f : 0;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    float dx, dy;
    controls_pad_camera(pad, dt, camera_sens(), &dx, &dy);
    if (mouse_captured || !window)
        add_motion(FROM_PAD, dx, controls_invert_y(1) ? -dy : dy);
}

int window_poll(void) {
    test_mouse();
    if (!enabled()) {                   /* (no devices: the actions a test script holds, MHFU_TEST_ACTION) */
        uint8_t lx = 128, ly = 128;
        device_buttons = controls_buttons(NULL, 0, NULL, pointer_mode, &lx, &ly);
        device_lx = lx;
        device_ly = ly;
        smooth_motion();
        return 1;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT)
            return 0;
        if (ticket_event(&e))           /* (the note box open: the keys are its) */
            continue;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat && !overlay_open()) {
            if (renderer)               /* (the software renderer's picture: nothing drawn over it) */
                window_note("the controls screen needs the Vulkan renderer: controls.ini");
            else
                overlay_toggle();
            continue;
        }
        if (overlay_event(&e))          /* (the controls screen open: its events) */
            continue;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat)
            ram_snapshot();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F5 && !e.key.repeat) {
            if (renderer)               /* (the software renderer's picture: nothing drawn over it) */
                window_note("the performance panel needs the Vulkan renderer");
            else
                perf_toggle();
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F6 && !e.key.repeat) {
            if (renderer)               /* (the software renderer's picture: no box over it) */
                capture_request();
            else
                ticket_key();
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F7 && !e.key.repeat) {
            if (renderer)               /* (the software renderer's picture: nothing drawn over it) */
                window_note("the click inspector needs the Vulkan renderer");
            else
                inspect_toggle();
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F8 && !e.key.repeat)
            inspect_toggle_pause();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_PERIOD)
            inspect_step();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_COMMA)
            inspect_back();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_C && !e.key.repeat)
            inspect_free_camera();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_H && !e.key.repeat)
            inspect_free_hud();
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) {
            if (renderer)               /* (the software renderer's picture: nothing drawn over it) */
                window_note("the hit overlay needs the Vulkan renderer");
            else
                hit_view_toggle();
        }
        if (inspect_event(&e))          /* (the inspector open: the mouse's) */
            continue;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.scancode == SDL_SCANCODE_F2 || e.key.scancode == SDL_SCANCODE_F3 || e.key.scancode == SDL_SCANCODE_F4))
            state_key(e.key.scancode);
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F10 && !e.key.repeat)
            capture_mouse(!mouse_captured);
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat && window)
            SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            render_window_resized();
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
            capture_mouse(0);
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && pointer_mode) {
            if (e.button.button == SDL_BUTTON_LEFT)
                pointer_clicks++;
            pointer_x = e.button.x;
            pointer_y = e.button.y;
            want_capture = 1;           /* (taken when the menu closes) */
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured)
            capture_mouse(1);
        if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_captured) {
            add_motion(FROM_MOUSE, e.motion.xrel, controls_invert_y(0) ? -e.motion.yrel : e.motion.yrel);
        }
        if (e.type == SDL_EVENT_MOUSE_WHEEL && (mouse_captured || pointer_mode))
            controls_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
        /* the device last used (for the hints shown: controls.c) */
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_WHEEL ||
            (e.type == SDL_EVENT_MOUSE_MOTION && (fabsf(e.motion.xrel) + fabsf(e.motion.yrel) > 4)))
            controls_note_device(0);
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.value > 16000 || e.gaxis.value < -16000)))
            controls_note_device(1);
        if (e.type == SDL_EVENT_MOUSE_MOTION && pointer_mode) {
            pointer_x = e.motion.x;
            pointer_y = e.motion.y;
            pointer_moved = 1;
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad)
            pad = SDL_OpenGamepad(e.gdevice.which);
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = NULL;
        }
    }
    /* (what is held as the settings screen closes - the key that closed it, or picked its last row - is not the game's
     * until let go: Enter there would pick the menu's row under it again, Esc close the game's menu too) */
    static int was_open;
    static uint32_t held_over;
    if (overlay_open()) {               /* (the game paused: nothing pressed) */
        overlay_tick();
        was_open = 1;
        device_buttons = 0;
        device_lx = device_ly = 128;
        return 1;
    }
    pad_camera();
    uint8_t lx = 128, ly = 128;
    /* (the inspector open: the mouse's buttons are its own, not the game's) */
    SDL_MouseButtonFlags m = (mouse_captured || pointer_mode) && !inspect_mode ? SDL_GetMouseState(NULL, NULL) : 0;
    /* (in a menu the left button is the pointer's, and the right goes back: circle - menu.c) */
    uint32_t b = controls_buttons(SDL_GetKeyboardState(NULL), pointer_mode ? 0 : m, pad, pointer_mode, &lx, &ly);
    if (pointer_mode && (m & SDL_BUTTON_RMASK))
        b |= PSP_CIRCLE;
    if (was_open) {
        was_open = 0;
        held_over = b;
    }
    held_over &= b;
    b &= ~held_over;
    device_buttons = b;
    device_lx = lx;
    device_ly = ly;
    smooth_motion();
    return 1;
}

/* Show a frame: 480 x 272, ABGR8888 */
void window_show(const uint32_t *frame) {
    if (!enabled() || !renderer)
        return;
    SDL_UpdateTexture(texture, NULL, frame, 480 * 4);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

uint32_t *window_pixels(void) { return pixels; }
uint32_t window_buttons(void) { return device_buttons; }

void window_stick(uint8_t *lx, uint8_t *ly) {
    *lx = device_lx;
    *ly = device_ly;
    /* MHFU_TEST_STICK="frame,x,y[,frames]": the stick held there from the game's frame (for testing) */
    static int read;
    static unsigned long from = ~0ul, frames = ~0ul;
    static int x, y;
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_STICK");
        if (!e || sscanf(e, "%lu,%d,%d,%lu", &from, &x, &y, &frames) < 3)
            from = ~0ul;
    }
    unsigned long f = present_frame_number();
    if (f >= from && f < from + frames) {
        *lx = (uint8_t)x;
        *ly = (uint8_t)y;
    }
}
