/* The moves of weapon class 4: the state functions of hunter group 1. Rewritten by hand from game_task 09BA7EF0 and the functions below it, the
 * generated code still behind them (MHFU_HAND=0). What game weapon the class is isn't settled by this code: only motion numbers, and that it can
 * guard (hunter_damage.c). As in weapon0_moves.c, each state goes by state_step; button_action in a window of frames picks the next move, which starts
 * at a later frame; state_scratch_a remembers what was picked. */
#include "weapon_moves.h"

#define dodge_or_stop_cancel f_game_task_09BA8E30
#define swing_a f_game_task_09BA7FF8
#define swing_b f_game_task_09BA8450
#define swing_c f_game_task_09BA8700
#define swing_d f_game_task_09BA8818
#define swing_e f_game_task_09BA88D8
#define swing_f f_game_task_09BA8BC0
#define weapon_can_cancel f_game_task_09A8B650
#define button_4_a f_game_task_09AA6E70
#define button_4_b f_game_task_09AA7580

#define SOUND_EFFECT 0x09C5F7E0u        /* in game_sub, not resident at link time: called as the game does */

enum { BUTTON_MOVE_NEXT = 1, BUTTON_MOVE_OTHER = 2, BUTTON_3 = 3, BUTTON_4 = 4 };   /* button_action */
enum { PICKED_NEXT = 1, PICKED_OTHER = 2 };     /* state_scratch_a */
#define CANCEL_MODE 5
#define MIN_STAMINA_FOR_B 75
#define BUTTON_FIRST_MOVE_B 0x4000
#define SELECTOR_FLAG_BUTTON_4 1

static void cancel(Ctx *c, Hunter *self) {
    call1(c, dodge_or_stop_cancel, GAME_ADDR(self));
}

/* Stops if the motion is over, back to standing, or a weapon-independent cancel took it */
static int moves_on(Ctx *c, Hunter *self, uint32_t how) {
    if (motion_ended(self)) {
        back_to_base(c, self, how);
        return 0;
    }
    return (uint8_t)call2(c, weapon_can_cancel, GAME_ADDR(self), CANCEL_MODE) != 1;
}

/* A button in the window, noted */
static void note_button(Ctx *c, Hunter *self, float from, float to) {
    if (within(c, self, from, to)) {
        if (self->button_action == BUTTON_MOVE_NEXT)
            self->state_scratch_a = PICKED_OTHER;
        else if (self->button_action == BUTTON_MOVE_OTHER)
            self->state_scratch_a = PICKED_NEXT;
    }
}

/* What was noted, at the frame it counts: PICKED_NEXT goes to this state, PICKED_OTHER to state 2; nothing, a cancel until `until` */
static void follow_on(Ctx *c, Hunter *self, uint32_t next_state, float until) {
    if (self->state_scratch_a == 0) {
        if (!reached(c, self, until))
            cancel(c, self);
    } else if (self->state_scratch_a == PICKED_NEXT) {
        set_state(c, self, 1, next_state, 4);
    } else {
        set_state(c, self, 1, 2, 4);
    }
}

/* game_task 09BA7FF8: states 0, 6, 7, 8: motions 0x579 to 0x57B by the way in */
void hand_hunter_weapon4_state_motion_0579(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    if (self->state_step == 0) {
        self->state_step = 1;
        self->state_scratch_b = 0;
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        switch (way_in) {
        case 3:
            motion(c, self, 0x579, 2, 0xA);
            attack(c, self, 0x15);
            call4_at(c, SOUND_EFFECT, GAME_ADDR(self), 8, 1, 0);
            break;
        case 2:
            motion(c, self, 0x57B, 2, 0);
            attack(c, self, 0x1A);
            break;
        case 1:
            motion(c, self, 0x57A, 2, 0);
            attack(c, self, 0x19);
            break;
        default:
            motion(c, self, 0x579, 4, 0);
            attack(c, self, 0x15);
            break;
        }
        return;
    }
    if (self->state_scratch_a == 0) {
        if (way_in == 2)
            note_button(c, self, 30.0f, 105.0f);
        else
            note_button(c, self, 10.0f, 50.0f);
    }
    if (!moves_on(c, self, 6))
        return;
    if (way_in == 2) {
        if (reached(c, self, 64.0f)) {
            if (self->state_scratch_a == PICKED_OTHER)
                set_state(c, self, 1, 2, 4);
            else if (!reached(c, self, 92.0f))
                cancel(c, self);
        }
    } else if (way_in == 1) {
        if (reached(c, self, 20.0f))
            follow_on(c, self, 7, 50.0f);
    } else if (reached(c, self, 24.0f)) {
        follow_on(c, self, 6, 50.0f);
    }
    c->f[12].f = 52.0f;
    if ((uint8_t)call2(c, frame_just_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0 && way_in == 2)
        attack(c, self, 0x1B);
}

/* game_task 09BA8450: states 1, 9, 10: motion 0x57C */
void hand_hunter_weapon4_state_motion_057c(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    if (self->state_step == 0) {
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        self->state_scratch_a = 0;
        self->state_scratch_b = 0;
        if (way_in == 2) {
            self->state_step = 2;
            motion(c, self, 0x57C, 4, 0);
            attack(c, self, 0x16);
            self->quest_flags |= QUEST_FLAG_1000;
        } else if (way_in == 1) {
            self->state_step++;
            self->misc_flags |= MISC_FLAG_WEAPON_OUT;
            motion(c, self, 0x3F2, 2, 0);
            self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        } else {
            self->state_step = 2;
            motion(c, self, 0x57C, 2, 0);
            attack(c, self, 0x16);
            self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        }
        return;
    }
    if (self->state_step == 1) {
        if (motion_ended(self)) {
            self->state_step++;
            motion(c, self, 0x57C, 0, 8);
            attack(c, self, 0x16);
            self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
        }
        return;
    }
    if (self->state_scratch_a == 0 && within(c, self, 20.0f, 60.0f) && self->button_action == BUTTON_MOVE_OTHER)
        self->state_scratch_a = 1;
    if (!moves_on(c, self, 4))
        return;
    if (reached(c, self, 34.0f)) {
        if (self->state_scratch_a != 0) {
            call1(c, clear_chain_flags, GAME_ADDR(self));
            set_state(c, self, 1, 4, 4);
        } else if (!reached(c, self, 60.0f)) {
            cancel(c, self);
        }
    }
}

/* game_task 09BA8700: state 2: motion 0x57D */
void hand_hunter_weapon4_state_motion_057d(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->state_step == 0) {
        self->state_step = 1;
        self->state_scratch_b = 0;
        motion(c, self, 0x57D, 4, 0);
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        attack(c, self, 0x17);
        return;
    }
    if (moves_on(c, self, 6) && reached(c, self, 20.0f) && reached(c, self, 40.0f))
        cancel(c, self);
}

/* game_task 09BA8818: state 3: motion 0x588, then a reaction's state */
void hand_hunter_weapon4_state_motion_0588(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->state_step == 0) {
        self->state_step = 1;
        self->state_scratch_b = 0;
        motion(c, self, 0x588, 4, 0);
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        attack(c, self, 0x18);
        return;
    }
    if (motion_ended(self))
        call4(c, set_state_noted, GAME_ADDR(self), 2, 0xC, 0);
    else
        call2(c, weapon_can_cancel, GAME_ADDR(self), CANCEL_MODE);
}

/* game_task 09BA88D8: states 4, 11, 12: motion 0x57E */
void hand_hunter_weapon4_state_motion_057e(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    if (self->state_step == 0) {
        self->state_step = 1;
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        self->state_scratch_a = 0;
        self->state_scratch_b = 0;
        if (way_in == 2) {
            motion(c, self, 0x589, 2, 0);
        } else {
            self->state_step = 2;
            motion(c, self, 0x57E, 2, way_in == 1 ? 8 : 0);
            attack(c, self, 0x1E);
        }
        return;
    }
    if (self->state_step == 1) {
        if (motion_ended(self)) {
            self->state_step++;
            self->state_scratch_a = 0;
            motion(c, self, 0x57E, 0, 0x10);
        } else if (reached(c, self, 18.0f) && self->state_scratch_a == 0) {
            self->state_scratch_a = 1;
            attack(c, self, 0x64);
        }
        return;
    }
    if (self->state_scratch_a == 0)
        note_button(c, self, 26.0f, 60.0f);
    if (moves_on(c, self, 6) && reached(c, self, 26.0f)) {
        if (self->state_scratch_a == PICKED_NEXT)
            set_state(c, self, 1, 8, 4);
        else if (self->state_scratch_a == PICKED_OTHER)
            set_state(c, self, 1, 2, 4);
        else if (!reached(c, self, 60.0f))
            cancel(c, self);
    }
}

/* game_task 09BA8BC0: state 5: motion 0x580; a button in a window goes on to state 0xA */
void hand_hunter_weapon4_state_motion_0580(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->state_step == 0) {
        self->state_step = 1;
        motion(c, self, 0x580, 4, 0);
        shake(c, self, 0);
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 0, 0);
        return;
    }
    if (self->state_scratch_a != 0) {
        if (reached(c, self, 32.0f))
            call4(c, set_state_noted, GAME_ADDR(self), 1, 0xA, 0xC);
    } else if (within(c, self, 14.0f, 50.0f) && self->state_scratch_a == 0 && !(self->hunter_flags & 1) && self->button_action - 1 < 3) {
        self->state_scratch_a = 1;
    }
    if (motion_ended(self))
        back_to_base(c, self, 6);
}

/* game_task 09BA8D40: the first move, by the button */
void hand_hunter_weapon4_pick_state_from_field_1120(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
    call1(c, clear_chain_flags, GAME_ADDR(self));
    switch (self->button_action) {
    case 1: set_state(c, self, 1, 2, 4); return;
    case 2: set_state(c, self, 1, 0, 4); return;
    case 3: set_state(c, self, 1, 1, 4); return;
    default: break;
    }
    if ((self->buttons_pressed & BUTTON_FIRST_MOVE_B) && self->stamina >= MIN_STAMINA_FOR_B)
        set_state(c, self, 0, 0x1C, 4);
}

/* game_task 09BA8E30 */
void hand_hunter_weapon4_try_evade_clear_111c_mask_800(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if ((uint8_t)call1(c, can_chain, GAME_ADDR(self)) == 0)
        return;
    call1(c, before_chain, GAME_ADDR(self));
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
    call1(c, clear_chain_flags, GAME_ADDR(self));
}

/* game_task 09BA8E80 */
void hand_hunter_weapon4_enter_state_09(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->quest_flags |= QUEST_FLAG_800;
    call1(c, clear_chain_flags, GAME_ADDR(self));
    set_state(c, self, 1, 9, 4);
}

/* game_task 09BA8F40 */
void hand_hunter_weapon4_state_0c_or_button_4(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
    call1(c, clear_chain_flags, GAME_ADDR(self));
    if (self->button_action == 2) {
        set_state(c, self, 1, 0xC, 4);
    } else if (self->button_action == 4) {
        call2(c, button_4_a, GAME_ADDR(self), 2);
        call2(c, button_4_b, GAME_ADDR(self), 0);
        self->selector_input_flags |= SELECTOR_FLAG_BUTTON_4;
    }
}

/* game_task 09BA7EF0: for group 1 with this weapon class */
void hand_hunter_weapon4_state_dispatch(Ctx *c) {
    uint8_t state = OBJ(Hunter, c->r[4])->state;
    static const struct { void (*fn)(Ctx *); int8_t way_in; } states[0xD] = {
        {swing_a, 0}, {swing_b, 0}, {swing_c, -1}, {swing_d, -1}, {swing_e, 0}, {swing_f, -1},
        {swing_a, 1}, {swing_a, 2}, {swing_a, 3},
        {swing_b, 1}, {swing_b, 2},
        {swing_e, 1}, {swing_e, 2},
    };
    if (state >= 0xD)
        return;
    if (states[state].way_in >= 0)
        c->r[5] = (uint32_t)states[state].way_in;
    states[state].fn(c);
}
