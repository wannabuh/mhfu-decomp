/* The window and input (SDL3): each frame the game flips is shown, scaled to the window; the keyboard and any gamepad
 * are read into the PSP's buttons and analog stick. MHFU_HEADLESS=1 runs without a window (tests, scripted runs).
 *
 * Drawing is the Vulkan renderer's (render.c), into this window or offscreen; with MHFU_RENDERER=soft, or without
 * Vulkan, the software GE draws into VRAM and its frames are shown here through SDL's renderer.
 *
 * Keyboard and mouse, laid out for playing on a PC (the PSP buttons they press, with MHFU's default controls):
 *   W A S D      move (the analog stick)          Shift        sprint (R)
 *   Space        dodge / roll (cross)             F            interact: talk, gather, carve (cross)
 *   left mouse   primary attack (triangle)        right mouse  secondary attack (circle)
 *   middle / E   special attack (triangle + circle)   Q        use item / sheathe (square)
 *   Tab          reset the camera (L)             Esc          menu (START)
 *   arrows       D-pad (menus, camera)            Enter / Backspace   confirm / back in menus (cross / circle)
 *   C            SELECT
 * While a menu is open (menu.c) the mouse is a pointer instead: over a row it moves the game's cursor there, a left
 * click picks it (cross), a right click goes back (circle).
 * Gamepad: as laid out (south cross, east circle, west square, north triangle), left stick. */
#include "hle.h"
#include "render.h"
#include "savestate.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
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
static int want_capture = 1;             /* the player's choice: captured unless F10 or the focus lost let it go */
static int pointer_mode;                 /* a menu is open: the mouse is a pointer (menu.c) */
static float pointer_x, pointer_y;       /* where it is, in the window's points */
static int pointer_moved, pointer_clicks;

/* The mouse drives the camera while it is captured: from the start, toggled with F10, released when the window loses
 * focus (and taken again when it is clicked), and while a menu is open, where it is a pointer */
static void apply_capture(void) {
    if (!window)
        return;
    mouse_captured = want_capture && !pointer_mode;
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

static uint32_t keyboard_buttons(uint8_t *lx, uint8_t *ly) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    static const struct { SDL_Scancode key; uint32_t button; } map[] = {
        {SDL_SCANCODE_SPACE, PSP_CROSS}, {SDL_SCANCODE_F, PSP_CROSS}, {SDL_SCANCODE_Q, PSP_SQUARE},
        {SDL_SCANCODE_E, PSP_TRIANGLE | PSP_CIRCLE}, {SDL_SCANCODE_LSHIFT, PSP_R}, {SDL_SCANCODE_RSHIFT, PSP_R},
        {SDL_SCANCODE_TAB, PSP_L}, {SDL_SCANCODE_ESCAPE, PSP_START}, {SDL_SCANCODE_C, PSP_SELECT},
        {SDL_SCANCODE_UP, PSP_UP}, {SDL_SCANCODE_DOWN, PSP_DOWN}, {SDL_SCANCODE_LEFT, PSP_LEFT},
        {SDL_SCANCODE_RIGHT, PSP_RIGHT}, {SDL_SCANCODE_RETURN, PSP_CROSS}, {SDL_SCANCODE_BACKSPACE, PSP_CIRCLE},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (k[map[i].key])
            b |= map[i].button;
    SDL_MouseButtonFlags m = mouse_captured || pointer_mode ? SDL_GetMouseState(NULL, NULL) : 0;
    if (pointer_mode)                   /* the left button is the pointer's (menu.c) */
        m &= SDL_BUTTON_RMASK;
    if (m & SDL_BUTTON_LMASK)
        b |= PSP_TRIANGLE;
    if (m & SDL_BUTTON_RMASK)
        b |= PSP_CIRCLE;
    if (m & SDL_BUTTON_MMASK)
        b |= PSP_TRIANGLE | PSP_CIRCLE;
    int x = 128 + (k[SDL_SCANCODE_D] ? 127 : 0) - (k[SDL_SCANCODE_A] ? 128 : 0);
    int y = 128 + (k[SDL_SCANCODE_S] ? 127 : 0) - (k[SDL_SCANCODE_W] ? 128 : 0);
    *lx = (uint8_t)x;
    *ly = (uint8_t)y;
    return b;
}

static uint32_t gamepad_buttons(uint8_t *lx, uint8_t *ly) {
    if (!pad)
        return 0;
    static const struct { SDL_GamepadButton pad; uint32_t button; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PSP_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, PSP_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PSP_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, PSP_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PSP_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PSP_R},
        {SDL_GAMEPAD_BUTTON_START, PSP_START}, {SDL_GAMEPAD_BUTTON_BACK, PSP_SELECT},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PSP_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PSP_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PSP_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PSP_RIGHT},
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(pad, map[i].pad))
            b |= map[i].button;
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x > 6000 || x < -6000 || y > 6000 || y < -6000) {
        *lx = (uint8_t)((x + 32768) >> 8);
        *ly = (uint8_t)((y + 32768) >> 8);
    }
    return b;
}

/* Read input; returns 0 if the window was closed */
/* MHFU_TEST_MOUSE="frame,vx,vy[,frames]": from the game's frame, move the mouse at vx, vy counts a second (for so many
 * frames), as the window would - for testing the camera without a hand on the mouse */
unsigned long present_frame_number(void);

static void test_mouse(void) {
    static int read;
    static unsigned long from, frames;
    static float vx, vy;
    static uint64_t last;
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_MOUSE");
        frames = ~0ul;
        if (!e || sscanf(e, "%lu,%f,%f,%lu", &from, &vx, &vy, &frames) < 3)
            from = ~0ul;
    }
    unsigned long f = present_frame_number();
    uint64_t now = time_us();
    if (f >= from && f < from + frames && last) {
        float dt = (float)(now - last) / 1e6f;
        mouse_dx += vx * dt;
        mouse_dy += vy * dt;
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

int window_poll(void) {
    test_mouse();
    if (!enabled())
        return 1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT)
            return 0;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F9 && !e.key.repeat)
            ram_snapshot();
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
            mouse_dx += e.motion.xrel;
            mouse_dy += e.motion.yrel;
        }
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
    uint8_t lx = 128, ly = 128;
    uint32_t b = keyboard_buttons(&lx, &ly);
    b |= gamepad_buttons(&lx, &ly);
    device_buttons = b;
    device_lx = lx;
    device_ly = ly;
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
