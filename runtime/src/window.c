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
 * Gamepad: as laid out (south cross, east circle, west square, north triangle), left stick. */
#include "hle.h"
#include "render.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t ctrl_lx, ctrl_ly;

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
        if (!headless)
            window = SDL_CreateWindow("Monster Hunter Freedom Unite", 480 * 2, 272 * 2,
                                      SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        if (render_init(window))
            return;
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
    SDL_MouseButtonFlags m = SDL_GetMouseState(NULL, NULL);
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
int window_poll(void) {
    if (!enabled())
        return 1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT)
            return 0;
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
    ctrl_lx = lx;
    ctrl_ly = ly;
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
