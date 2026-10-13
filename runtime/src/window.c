/* The window and input (SDL3): each frame the game flips is shown, scaled to the window; the keyboard and any gamepad
 * are read into the PSP's buttons and analog stick. MHFU_HEADLESS=1 runs without a window (tests, scripted runs).
 *
 * Keyboard: arrows - D-pad; Z cross, X circle, A square, S triangle; Q L, W R; Enter START, Backspace SELECT;
 * I J K L - analog stick. Gamepad: as laid out (south cross, east circle, west square, north triangle), left stick. */
#include "hle.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

extern uint32_t ctrl_buttons;
extern uint8_t ctrl_lx, ctrl_ly;

enum {
    PSP_SELECT = 0x1, PSP_START = 0x8, PSP_UP = 0x10, PSP_RIGHT = 0x20, PSP_DOWN = 0x40, PSP_LEFT = 0x80,
    PSP_L = 0x100, PSP_R = 0x200, PSP_TRIANGLE = 0x1000, PSP_CIRCLE = 0x2000, PSP_CROSS = 0x4000, PSP_SQUARE = 0x8000,
};

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Gamepad *pad;
static int disabled = -1;
static uint32_t pixels[480 * 272];

static int enabled(void) {
    if (disabled < 0) {
        disabled = getenv("MHFU_HEADLESS") != NULL;
        if (!disabled) {
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
                fprintf(stderr, "no window: %s\n", SDL_GetError());
                disabled = 1;
                return 0;
            }
            window = SDL_CreateWindow("Monster Hunter Freedom Unite", 480 * 2, 272 * 2, SDL_WINDOW_RESIZABLE);
            renderer = SDL_CreateRenderer(window, NULL);
            SDL_SetRenderLogicalPresentation(renderer, 480, 272, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
    }
    return !disabled;
}

static uint32_t keyboard_buttons(uint8_t *lx, uint8_t *ly) {
    const bool *k = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    static const struct { SDL_Scancode key; uint32_t button; } map[] = {
        {SDL_SCANCODE_UP, PSP_UP}, {SDL_SCANCODE_DOWN, PSP_DOWN}, {SDL_SCANCODE_LEFT, PSP_LEFT},
        {SDL_SCANCODE_RIGHT, PSP_RIGHT}, {SDL_SCANCODE_Z, PSP_CROSS}, {SDL_SCANCODE_X, PSP_CIRCLE},
        {SDL_SCANCODE_A, PSP_SQUARE}, {SDL_SCANCODE_S, PSP_TRIANGLE}, {SDL_SCANCODE_Q, PSP_L}, {SDL_SCANCODE_W, PSP_R},
        {SDL_SCANCODE_RETURN, PSP_START}, {SDL_SCANCODE_BACKSPACE, PSP_SELECT},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (k[map[i].key])
            b |= map[i].button;
    int x = 128 + (k[SDL_SCANCODE_L] ? 127 : 0) - (k[SDL_SCANCODE_J] ? 128 : 0);
    int y = 128 + (k[SDL_SCANCODE_K] ? 127 : 0) - (k[SDL_SCANCODE_I] ? 128 : 0);
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
    ctrl_buttons = b;
    ctrl_lx = lx;
    ctrl_ly = ly;
    return 1;
}

/* Show a frame: 480 x 272, ABGR8888 */
void window_show(const uint32_t *frame) {
    if (!enabled())
        return;
    SDL_UpdateTexture(texture, NULL, frame, 480 * 4);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

uint32_t *window_pixels(void) { return pixels; }
