/* The stick and the dash button, read for moving. Rewritten by hand from game_task 09AA5FB0 and 09A8B578, the generated code still behind
 * them (MHFU_HAND=0). */
#include "hand.h"
#include "names.h"

#define CAMERA_POINTER 0x089CBD04u

/* What hunter_classify_move_input says */
enum {
    MOVE_INPUT_NONE = 0,
    MOVE_INPUT_WALK = 1,
    MOVE_INPUT_RUN = 3,
    MOVE_INPUT_DASH = 5,
};

#define STICK_WALK_FROM 40
#define STICK_RUN_FROM 120
#define BUTTON_DASH 0x200               /* the PSP's R */
#define STATE_DASH_RELEASED 0x24        /* group 0: dashing again has to wait for state_hold_timer */
#define MISC_FLAG_NO_DASH 8             /* many other uses: not settled */
#define HUNTER_FLAG_HOLDS_STILL 0x1000  /* only for the classes below; many other uses: not settled */

/* The classes with no movement at all while both of those flags are up. What the game calls them isn't known from this code. */
static int class_stops_on_flags(uint8_t weapon_class) {
    return weapon_class == 1 || weapon_class == 5 || weapon_class == 10;
}

/* game_task 09AA5FB0 */
void hand_hunter_classify_move_input(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (class_stops_on_flags(self->weapon_class) && (self->misc_flags & MISC_FLAG_NO_DASH) && (self->hunter_flags & HUNTER_FLAG_HOLDS_STILL)) {
        c->r[2] = MOVE_INPUT_NONE;
        return;
    }
    uint32_t input = MOVE_INPUT_NONE;
    if (self->stick_magnitude >= STICK_RUN_FROM)    /* the game also compares with 85, for the same answer either way */
        input = MOVE_INPUT_RUN;
    else if (self->stick_magnitude >= STICK_WALK_FROM)
        input = MOVE_INPUT_WALK;
    if (input != MOVE_INPUT_NONE && self->movement_mode == 0 && !(self->misc_flags & MISC_FLAG_NO_DASH) && (self->buttons_held & BUTTON_DASH)) {
        if ((uint8_t)call3(c, object_state_is, GAME_ADDR(self), 0, STATE_DASH_RELEASED) == 0 || self->state_hold_timer == 0)
            input = MOVE_INPUT_DASH;
    }
    c->r[2] = input;
}

/* game_task 09A8B578: to turn towards where the stick points as the camera sees it, unless the camera says not to */
void hand_hunter_face_stick_direction(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if ((uint8_t)call1(c, unknown_camera_checks_dc4_a8d_1de, LW(CAMERA_POINTER)) == 1)
        return;
    uint16_t heading = (uint16_t)call1(c, camera_get_heading_angle16, LW(CAMERA_POINTER));
    self->facing_target = (uint16_t)(self->stick_angle + heading);
}

/* -- EBOOT 088D61CC: the pad's buttons, copied to the hunter and narrowed ---------------------------------------------------------- */

#define PAD_HELD 0x08A62D88u
#define PAD_PRESSED 0x08A62D8Cu
#define PAD_RELEASED 0x08A62D90u
#define PAD_STICK_ANGLE 0x08A62D98u
#define PAD_STICK_MAGNITUDE 0x08A62D9Au
#define HUD_POINTER 0x089CBD0Cu
#define BUTTON_L 0x100
#define BUTTON_SELECT 1
#define BUTTONS_ALWAYS_THROUGH 0xF00000u    /* above the PSP's own */
#define HUNTER_FLAG_SELECTOR_OPEN 1
#define HUNTER_FLAG_INPUT_OFF_A 2
#define HUNTER_FLAG_INPUT_OFF_B 0x200
#define HUD_FLAG_NO_SELECT 0x20
#define OVERLAY_KEEPS_L 1           /* what ui_is_overlay_active says for an overlay that L still works in */

/* Native frame rate: the pad is read at 30ths, so what was pressed or released is seen at a 30th and not again on the frames between, and
 * L's hold is counted in 30ths. */
void hand_hunter_copy_pad(Ctx *c) {
    HunterBase *self = OBJ(HunterBase, c->r[4]);
    uint32_t pressed = rt_tick30 ? LW(PAD_PRESSED) : 0, released = rt_tick30 ? LW(PAD_RELEASED) : 0;
    self->buttons_held = LW(PAD_HELD);
    self->buttons_pressed = pressed;
    self->buttons_released = released;
    self->stick_angle = LHU(PAD_STICK_ANGLE);
    self->stick_magnitude = LHU(PAD_STICK_MAGNITUDE);
    self->buttons_held_unnarrowed = LW(PAD_HELD);
    self->buttons_pressed_unnarrowed = pressed;
    self->buttons_released_raw = released;
    if ((uint8_t)call1(c, ui_is_overlay_active, LW(HUD_POINTER)) != 0 || (self->hunter_flags & (HUNTER_FLAG_INPUT_OFF_A | HUNTER_FLAG_INPUT_OFF_B))) {
        if ((uint8_t)call1(c, ui_is_overlay_active, LW(HUD_POINTER)) == OVERLAY_KEEPS_L) {
            self->buttons_held &= BUTTONS_ALWAYS_THROUGH | BUTTON_L;
            self->buttons_pressed &= BUTTONS_ALWAYS_THROUGH | BUTTON_L;
            self->buttons_released &= BUTTONS_ALWAYS_THROUGH | BUTTON_L;
        } else {
            self->buttons_held = 0;
            self->buttons_pressed = 0;
            self->buttons_released = 0;
        }
        self->buttons_held_unnarrowed = 0;
        self->buttons_pressed_unnarrowed = 0;
        self->buttons_released_raw = 0;
        self->buttons_word_498 = 0;
    } else if (self->hunter_flags & HUNTER_FLAG_SELECTOR_OPEN) {
        self->buttons_held &= BUTTONS_ALWAYS_THROUGH | BUTTON_DASH;
        self->buttons_pressed &= BUTTONS_ALWAYS_THROUGH;
        self->buttons_released &= BUTTONS_ALWAYS_THROUGH;
    }
    if (self->buttons_pressed & BUTTON_L)
        self->l_held_frames = 0;
    else if (self->buttons_held & BUTTON_L)
        self->l_held_frames += rt_tick30;
    if (OBJ(HudContext, LW(HUD_POINTER))->hud_flags & HUD_FLAG_NO_SELECT)
        self->buttons_pressed &= ~(uint32_t)BUTTON_SELECT;
}
