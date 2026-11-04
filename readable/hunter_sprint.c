/* The hunter's sprint: rewritten by hand from game_task 09A9E288, with its generated code still behind it (MHFU_HAND=0).
 *
 * States here are (group 0, state) as the hunter's state setter takes them. What the game calls each isn't in the code, so they are
 * named for what this function does with them where that is clear, and left as numbers where it isn't. */
#include "hand.h"
#include "names.h"

enum {
    STATE_SPRINT_LINKED = 0x20,     /* sprint while a linked monster is out of a quarter turn: the other sprint, 09A9E690 */
    STATE_OUT_OF_BREATH = 0x21,
    STATE_LOW_STAMINA_EXIT = 0x22,
    STATE_RELEASED_AFTER_HOLD = 0x24,
    STATE_LEFT_SPRINT = 0x2C,
    STATE_BLOCKED_AHEAD = 0x06,
    STATE_SPRINT_BUTTON_ACTION = 0x1C,
    STATE_GROUND_FORCED = 0x3B,
};

enum {
    MOTION_SPRINT = 3,
    MOVE_INPUT_KEEP_GOING = 3,      /* (leaves once the hold timer has run out) */
    MOVE_INPUT_COMMON = 5,
    BUTTON_SPRINT_ACTION = 0x4000,
    QUEST_FLAG_MONSTER_LINKED = 0x100,
    HUNTER_FLAG_MOVING = 4,         /* copied to bit 0x20 a tick; what it means beyond that isn't settled */
};

#define SPRINT_ANIM_SPEED 2.5f
#define SPRINT_HOLD_TICKS 30
#define SPRINT_MIN_STAMINA 76       /* below it, the sprint ends */
#define SPRINT_ACTION_MIN_STAMINA 75
#define SPRINT_PUSH 5.0f            /* units a game frame, forward, on top of the motion's own */
#define STICK_TURNS_AT 40
#define PROBE_AHEAD 85.0f
#define PROBE_HEIGHT -60.0f
#define VSLOT_SET_STATE 0x6C
#define set_motion f_EBOOT_088D5C74     /* one instruction's jump to hunter_set_motion_id_both_parts: called as the game does */

static void set_state(Ctx *c, Hunter *self, uint32_t state) {
    call4(c, hunter_set_state_then_set_flag_410_mask4, GAME_ADDR(self), 0, state, 0);
}

static void set_state_virtual(Ctx *c, Hunter *self, uint32_t state, uint32_t arg) {
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, 0, state, arg);
}

static void common_input(Ctx *c, Hunter *self) {
    call3(c, hunter_common_input_handler, GAME_ADDR(self), 0, 0);
}

/* Is the monster it is linked to more than a quarter turn from where it faces? */
static int linked_monster_off_to_the_side(const Hunter *self) {
    return ((self->linked_monster_angle - self->facing + 0x2000) & 0xFFFF) > 0x4000;
}

static void enter(Ctx *c, Hunter *self, uint32_t mode) {
    self->state_step = 1;
    call4(c, object_set_byte_280_and_flags_288_mask_1_2_8, GAME_ADDR(self), 0, 0, 0);
    call2(c, object_set_flags_288, GAME_ADDR(self), 0x600);
    self->state_ticks = 0;
    self->state_hold_timer = SPRINT_HOLD_TICKS;
    if (mode == 1)
        call4(c, set_motion, GAME_ADDR(self), MOTION_SPRINT, 4, SPRINT_HOLD_TICKS);
    else if (self->motion_id != MOTION_SPRINT)
        call4(c, set_motion, GAME_ADDR(self), MOTION_SPRINT, 0, 0);
    self->motion_part0_speed = SPRINT_ANIM_SPEED;
    self->motion_part1_speed = SPRINT_ANIM_SPEED;
    self->facing_target = (uint16_t)self->facing;
    common_input(c, self);
}

/* What the input says to do next, for the local player's hunter */
static void follow_input(Ctx *c, Hunter *self) {
    uint8_t input = (uint8_t)call1(c, hunter_classify_move_input, GAME_ADDR(self));
    if (input == MOVE_INPUT_COMMON) {
        common_input(c, self);
    } else if (self->instance_index != GAME()->local_hunter_slot && (LW(LW(0x08A632C0u) + 0xF7A57Cu) & 1)) {
        common_input(c, self);      /* (it was the local player's a moment ago: only if the classifier changed that) */
    } else if ((uint8_t)call3(c, object_state_is, GAME_ADDR(self), 0, STATE_LOW_STAMINA_EXIT) != 0 && self->stamina <= 0) {
        set_state_virtual(c, self, STATE_OUT_OF_BREATH, 0);
    } else if (input == MOVE_INPUT_KEEP_GOING) {
        if (self->state_hold_timer == 0)
            set_state(c, self, STATE_RELEASED_AFTER_HOLD);
        else
            common_input(c, self);
    } else {
        self->hunter_flags |= HUNTER_FLAG_MOVING;
        set_state(c, self, STATE_LEFT_SPRINT);
    }
    if (self->ground_forces_state_3b)
        set_state_virtual(c, self, STATE_GROUND_FORCED, 0);
}

static void run(Ctx *c, Hunter *self) {
    self->motion_part0_speed = SPRINT_ANIM_SPEED;
    self->motion_part1_speed = SPRINT_ANIM_SPEED;
    call2(c, f_game_task_09A6A480, GAME_ADDR(self), (uint32_t)-1);      /* costs 1 stamina every other tick */
    if (self->stamina < SPRINT_MIN_STAMINA) {
        set_state(c, self, STATE_LOW_STAMINA_EXIT);
        return;
    }
    if ((self->buttons_pressed & BUTTON_SPRINT_ACTION) && self->stamina >= SPRINT_ACTION_MIN_STAMINA &&
        (uint8_t)call2(c, unknown_mode_byte_2e_allows_index, LW(0x089CC558u), 1) == 0) {
        self->facing_target = (uint16_t)self->facing;
        set_state_virtual(c, self, STATE_SPRINT_BUTTON_ACTION, 4);
        return;
    }
    if (self->stick_magnitude >= STICK_TURNS_AT)
        call1(c, hunter_set_angle_320_from_49c_plus_camera_heading, GAME_ADDR(self));
    c->f[12].f = PROBE_AHEAD;
    c->f[13].f = PROBE_HEIGHT;
    if (call2(c, object_compare_probed_height_ahead, GAME_ADDR(self), 0) != 0) {
        self->facing_target = (uint16_t)self->facing;
        set_state(c, self, STATE_BLOCKED_AHEAD);
        return;
    }
    if ((self->quest_flags & QUEST_FLAG_MONSTER_LINKED) && linked_monster_off_to_the_side(self)) {
        set_state(c, self, STATE_SPRINT_LINKED);
        return;
    }
    /* The push forward. Native frame rate: a game frame's worth, so times the time scale. */
    float push[3];
    rotate_by_rows(push, self->rotation, 0, 0, SPRINT_PUSH);
    self->place[0] += push[0] * rt_time_scale;
    self->place[2] += push[2] * rt_time_scale;
    if (self->instance_index == GAME()->local_hunter_slot)
        follow_input(c, self);
}

/* game_task 09A9E288: the state's function, a0 the hunter, a1 how it was entered */
void hand_hunter_state_sprint(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t mode = c->r[5];
    self->hunter_flags |= HUNTER_FLAG_MOVING;
    if (self->state_step == 0)
        enter(c, self, mode);
    else
        run(c, self);
}
