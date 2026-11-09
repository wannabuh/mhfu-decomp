/* The controls: inputs bound to actions, the actions to the PSP's buttons (controls.c) */
#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

enum {
    A_MOVE_UP, A_MOVE_DOWN, A_MOVE_LEFT, A_MOVE_RIGHT,
    A_ATTACK, A_ATTACK2, A_SPECIAL, A_ROLL, A_SPRINT,
    A_INTERACT, A_USE_ITEM, A_SHEATHE,          /* (in this order: the actions done in steps) */
    A_ITEM_NEXT, A_ITEM_PREV, A_CAMERA_RESET, A_MAP, A_MENU, A_CONFIRM, A_BACK,
    A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_PSP_TRIANGLE, A_PSP_CIRCLE, A_PSP_CROSS, A_PSP_SQUARE, A_PSP_L, A_PSP_R,
    A_FREE_LOOK,
    A_COUNT
};

/* An input's kind */
enum { IN_NONE, IN_KEY, IN_MOUSE, IN_WHEEL, IN_PAD, IN_TRIGGER };
#define MAX_BIND 4

void controls_init(int write_defaults);     /* the bindings: controls.ini, else the defaults (written there) */
void controls_defaults(void);
int controls_save(void);
/* The buttons for the devices' state now (in_menu: a menu has the pointer), and the stick */
uint32_t controls_buttons(const bool *keys, SDL_MouseButtonFlags mouse, SDL_Gamepad *pad, int in_menu, uint8_t *lx,
                          uint8_t *ly);
void controls_wheel(float y);               /* the wheel turned (up +) */
void controls_pad_camera(SDL_Gamepad *pad, float dt, float sens, float *dx, float *dy);
int controls_invert_y(void);
int controls_free_look(void);               /* free look held (the mouse turns the picture alone: camera.c) */
int controls_device(void);                  /* the last used: 0 the keyboard and mouse, 1 a gamepad */
void controls_note_device(int device);
const char *controls_action_name(int action);
const char *controls_action_label(int action);
const char *controls_input_name(int pad, int action, int i);  /* the action's ith input's name (NULL: none) */
void controls_bind(int pad, int action, int add, int kind, int code);    /* (taken from any other action) */
void controls_clear(int pad, int action);
void controls_keep(void);                   /* the bindings as they are, for controls_undo */
void controls_undo(void);
float *controls_pad_camera_speed(int y);   /* (0 across, 1 up and down) */
float *controls_pad_deadzone(void);
int *controls_invert_y_setting(void);
