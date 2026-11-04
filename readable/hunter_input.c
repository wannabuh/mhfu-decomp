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
