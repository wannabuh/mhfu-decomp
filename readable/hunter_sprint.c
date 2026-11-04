/* The hunter's two sprints and being out of breath: rewritten by hand from game_task 09A9E288, 09A9E690 and 09A9EAE8, with the
 * generated code still behind them (MHFU_HAND=0).
 *
 * States here are (group 0, state) as the hunter's state setter takes them. What the game calls each isn't in the code, so they are
 * named for what these functions do with them where that is clear. */
#include "hand.h"
#include "names.h"

enum {
    STATE_BLOCKED_AHEAD = 0x06,     /* also what out of breath ends in */
    STATE_SPRINT_BUTTON_ACTION = 0x1C,
    STATE_SPRINT_LINKED = 0x20,     /* sprinting while a linked monster is out of a quarter turn: costs more, below */
    STATE_OUT_OF_BREATH = 0x21,
    STATE_LOW_STAMINA_EXIT = 0x22,
    STATE_RELEASED = 0x24,
    STATE_LEFT_SPRINT = 0x2C,
    STATE_GROUND_FORCED = 0x3B,
    STATE_LINKED_BUTTON_ACTION = 0x44,
    STATE_SPRINT_UNLINKED = 0x46,   /* back to the ordinary sprint */
};

enum {
    MOTION_SPRINT = 3,
    MOTION_OUT_OF_BREATH_START = 5,
    MOTION_OUT_OF_BREATH_END = 6,
    MOTION_LINKED_SPRINT = 0x26,
    MOTION_LINKED_SPRINT_ENTERED_MOVING = 0x27,
    MOTION_ARG_BLEND = 4,           /* the motion setter's third argument, here; what it does isn't settled */
    MOVE_INPUT_KEEP_GOING = 3,
    MOVE_INPUT_COMMON = 5,
    BUTTON_SPRINT_ACTION = 0x4000,
    QUEST_FLAG_MONSTER_LINKED = 0x100,
    HUNTER_FLAG_MOVING = 4,         /* copied to bit 0x20 a tick; what it means beyond that isn't settled */
    PLAYER_LIST_CHEAPER_STAMINA = 0x18,
};

#define SPRINT_ANIM_SPEED 2.5f
#define SPRINT_HOLD_TICKS 30
#define SPRINT_MIN_STAMINA 76       /* below it, a sprint ends */
#define SPRINT_ACTION_MIN_STAMINA 75
#define SPRINT_PUSH 5.0f            /* units a game frame, forward, on top of the motion's own */
#define LINKED_PUSH 1.5f
#define OUT_OF_BREATH_TICKS 150
#define OUT_OF_BREATH_RECOVERY 4    /* stamina a 30th */
#define STICK_TURNS_AT 40
#define PROBE_AHEAD 85.0f
#define PROBE_HEIGHT -60.0f
#define VSLOT_SET_STATE 0x6C
#define MODE_ENTERED_MOVING 1       /* the states' a1, how they were entered: the costs and exits differ with it */
#define MODE_ENTERED_BLENDED 2

#define set_motion f_EBOOT_088D5C74     /* one instruction's jump to hunter_set_motion_id_both_parts: called as the game does */
#define stamina_use hunter_stamina_add_with_skills_a8_a9
#define stamina_use_alternate_ticks hunter_stamina_add_odd_frames_with_skills_a8_a9

static void set_state(Ctx *c, Hunter *self, uint32_t state) {
    call4(c, hunter_set_state_then_set_flag_410_mask4, GAME_ADDR(self), 0, state, 0);
}

static void set_state_virtual(Ctx *c, Hunter *self, uint32_t state, uint32_t arg) {
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, 0, state, arg);
}

static void common_input(Ctx *c, Hunter *self) {
    call3(c, hunter_common_input_handler, GAME_ADDR(self), 0, 0);
}

static void face_ahead(Hunter *self) {
    self->facing_target = (uint16_t)self->facing;
}

static void turn_to_stick(Ctx *c, Hunter *self) {
    if (self->stick_magnitude >= STICK_TURNS_AT)
        call1(c, hunter_set_angle_320_from_49c_plus_camera_heading, GAME_ADDR(self));
}

static int blocked_ahead(Ctx *c, Hunter *self) {
    c->f[12].f = PROBE_AHEAD;
    c->f[13].f = PROBE_HEIGHT;
    return call2(c, object_compare_probed_height_ahead, GAME_ADDR(self), 0) != 0;
}

/* Is the monster it is linked to more than a quarter turn from where it faces? */
static int linked_monster_off_to_the_side(const Hunter *self) {
    return ((self->linked_monster_angle - self->facing + 0x2000) & 0xFFFF) > 0x4000;
}

/* The push forward. Native frame rate: a game frame's worth, so times the time scale. */
static void push_forward(Hunter *self, float distance) {
    float push[3];
    rotate_by_rows(push, self->rotation, 0, 0, distance);
    self->place[0] += push[0] * rt_time_scale;
    self->place[2] += push[2] * rt_time_scale;
}

static void leave_sprint(Ctx *c, Hunter *self) {
    self->hunter_flags |= HUNTER_FLAG_MOVING;
    set_state(c, self, STATE_LEFT_SPRINT);
}

/* What the input says to do next, for the local player's hunter. Both sprints: only keep_going differs. */
static void follow_input(Ctx *c, Hunter *self, void (*keep_going)(Ctx *, Hunter *)) {
    uint8_t input = (uint8_t)call1(c, hunter_classify_move_input, GAME_ADDR(self));
    if (input == MOVE_INPUT_COMMON) {
        common_input(c, self);
    } else if (self->instance_index != GAME()->local_hunter_slot && (LW(LW(0x08A632C0u) + 0xF7A57Cu) & 1)) {
        common_input(c, self);      /* (it was the local player's a moment ago: only if the classifier changed that) */
    } else if ((uint8_t)call3(c, object_state_is, GAME_ADDR(self), 0, STATE_LOW_STAMINA_EXIT) != 0 && self->stamina <= 0) {
        set_state_virtual(c, self, STATE_OUT_OF_BREATH, 0);
    } else if (input == MOVE_INPUT_KEEP_GOING) {
        keep_going(c, self);
    } else {
        leave_sprint(c, self);
    }
    if (self->ground_forces_state_3b)
        set_state_virtual(c, self, STATE_GROUND_FORCED, 0);
}

/* -- The sprint: game_task 09A9E288, states 0x1F and 0x46 ------------------------------------------------------------------------ */

static void sprint_enter(Ctx *c, Hunter *self, uint32_t mode) {
    self->state_step = 1;
    call4(c, object_set_byte_280_and_flags_288_mask_1_2_8, GAME_ADDR(self), 0, 0, 0);
    call2(c, object_set_flags_288, GAME_ADDR(self), 0x600);
    self->state_ticks = 0;
    self->state_hold_timer = SPRINT_HOLD_TICKS;
    if (mode == MODE_ENTERED_MOVING)
        call4(c, set_motion, GAME_ADDR(self), MOTION_SPRINT, MOTION_ARG_BLEND, SPRINT_HOLD_TICKS);
    else if (self->motion_id != MOTION_SPRINT)
        call4(c, set_motion, GAME_ADDR(self), MOTION_SPRINT, 0, 0);
    self->motion_part0_speed = SPRINT_ANIM_SPEED;
    self->motion_part1_speed = SPRINT_ANIM_SPEED;
    face_ahead(self);
    common_input(c, self);
}

static void sprint_keep_going(Ctx *c, Hunter *self) {
    if (self->state_hold_timer == 0)
        set_state(c, self, STATE_RELEASED);
    else
        common_input(c, self);
}

static void sprint_run(Ctx *c, Hunter *self) {
    self->motion_part0_speed = SPRINT_ANIM_SPEED;
    self->motion_part1_speed = SPRINT_ANIM_SPEED;
    call2(c, stamina_use_alternate_ticks, GAME_ADDR(self), (uint32_t)-1);
    if (self->stamina < SPRINT_MIN_STAMINA) {
        set_state(c, self, STATE_LOW_STAMINA_EXIT);
        return;
    }
    if ((self->buttons_pressed & BUTTON_SPRINT_ACTION) && self->stamina >= SPRINT_ACTION_MIN_STAMINA &&
        (uint8_t)call2(c, unknown_mode_byte_2e_allows_index, LW(0x089CC558u), 1) == 0) {
        face_ahead(self);
        set_state_virtual(c, self, STATE_SPRINT_BUTTON_ACTION, 4);
        return;
    }
    turn_to_stick(c, self);
    if (blocked_ahead(c, self)) {
        face_ahead(self);
        set_state(c, self, STATE_BLOCKED_AHEAD);
        return;
    }
    if ((self->quest_flags & QUEST_FLAG_MONSTER_LINKED) && linked_monster_off_to_the_side(self)) {
        set_state(c, self, STATE_SPRINT_LINKED);
        return;
    }
    push_forward(self, SPRINT_PUSH);
    if (self->instance_index == GAME()->local_hunter_slot)
        follow_input(c, self, sprint_keep_going);
}

void hand_hunter_state_sprint(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t mode = c->r[5];
    self->hunter_flags |= HUNTER_FLAG_MOVING;
    if (self->state_step == 0)
        sprint_enter(c, self, mode);
    else
        sprint_run(c, self);
}

/* -- The sprint with a monster linked: game_task 09A9E690 ------------------------------------------------------------------------ */

static void linked_enter(Ctx *c, Hunter *self, uint32_t mode) {
    self->state_step = 1;
    call4(c, object_set_byte_280_and_flags_288_mask_1_2_8, GAME_ADDR(self), 0, 0, 0);
    call2(c, object_set_flags_288, GAME_ADDR(self), 0x600);
    self->state_ticks = 0;
    if (mode == MODE_ENTERED_BLENDED)
        call4(c, set_motion, GAME_ADDR(self), MOTION_LINKED_SPRINT, MOTION_ARG_BLEND, 0);
    else if (mode == MODE_ENTERED_MOVING)
        call4(c, set_motion, GAME_ADDR(self), MOTION_LINKED_SPRINT_ENTERED_MOVING, 0, 0);
    else
        call4(c, set_motion, GAME_ADDR(self), MOTION_LINKED_SPRINT, 0, 0);
    face_ahead(self);
    common_input(c, self);
}

static int has_cheaper_stamina(Ctx *c, Hunter *self) {
    return call2(c, player_byte_list_contains, GAME_ADDR(self), PLAYER_LIST_CHEAPER_STAMINA) != 0;
}

static void linked_keep_going(Ctx *c, Hunter *self) {
    set_state(c, self, STATE_RELEASED);
}

static void linked_run(Ctx *c, Hunter *self, uint32_t mode) {
    int entered_moving = mode == MODE_ENTERED_MOVING;
    if (entered_moving) {
        if (has_cheaper_stamina(c, self))
            call2(c, stamina_use_alternate_ticks, GAME_ADDR(self), (uint32_t)-1);
        else
            call2(c, stamina_use, GAME_ADDR(self), (uint32_t)-1);
    } else {
        uint32_t cost = has_cheaper_stamina(c, self) ? (uint32_t)-1 : (uint32_t)-2;
        call2(c, stamina_use, GAME_ADDR(self), cost);
        if (self->stamina < SPRINT_MIN_STAMINA) {
            set_state(c, self, STATE_LOW_STAMINA_EXIT);
            return;
        }
        if (!(self->quest_flags & QUEST_FLAG_MONSTER_LINKED) || !linked_monster_off_to_the_side(self)) {
            set_state(c, self, STATE_SPRINT_UNLINKED);
            return;
        }
    }
    if ((self->buttons_pressed & BUTTON_SPRINT_ACTION) && !entered_moving && self->stamina >= SPRINT_ACTION_MIN_STAMINA) {
        face_ahead(self);
        set_state_virtual(c, self, STATE_LINKED_BUTTON_ACTION, 4);
        return;
    }
    turn_to_stick(c, self);
    if (blocked_ahead(c, self) && !entered_moving) {
        face_ahead(self);
        set_state(c, self, STATE_BLOCKED_AHEAD);
        return;
    }
    push_forward(self, LINKED_PUSH);
    if (self->instance_index == GAME()->local_hunter_slot)
        follow_input(c, self, entered_moving ? leave_sprint : linked_keep_going);
}

void hand_hunter_state_sprint_linked(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t mode = c->r[5];
    self->hunter_flags |= HUNTER_FLAG_MOVING;
    if (self->state_step == 0)
        linked_enter(c, self, mode);
    else
        linked_run(c, self, mode);
}

/* -- Out of breath: game_task 09A9EAE8, a timed pause that gets stamina back, then all of it ---------------------------------------- */

/* Native frame rate: the recovery and the timer both go at 30ths. */
void hand_hunter_state_out_of_breath(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    call2(c, hunter_stamina_add_clamped, GAME_ADDR(self), rt_tick30 ? OUT_OF_BREATH_RECOVERY : 0);
    switch (self->state_step) {
    case 0:
        self->state_step = 1;
        self->state_ticks = 0;
        self->misc_flags &= ~8u;
        call4(c, object_set_byte_280_and_flags_288_mask_1_2_8, GAME_ADDR(self), 0, 0, 0);
        call4(c, set_motion, GAME_ADDR(self), MOTION_OUT_OF_BREATH_START, MOTION_ARG_BLEND, 0);
        self->state_step = 2;
        self->action_count = OUT_OF_BREATH_TICKS;
        break;
    case 1:
    case 2:
        self->action_count -= rt_tick30;
        if (self->action_count <= 0) {
            self->state_step++;
            call4(c, set_motion, GAME_ADDR(self), MOTION_OUT_OF_BREATH_END, MOTION_ARG_BLEND, 0);
        }
        break;
    default:
        if (!(self->motion_part0_flags & 1)) {      /* the motion has ended */
            call2(c, hunter_stamina_add_clamped, GAME_ADDR(self), (uint32_t)(int32_t)self->stamina_max);
            call4(c, hunter_reset_to_base_state_and_motion, GAME_ADDR(self), 0, STATE_BLOCKED_AHEAD, 0);
        }
        break;
    }
}
