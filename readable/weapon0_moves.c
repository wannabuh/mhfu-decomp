/* The moves of weapon class 0 (the greatsword, by the recordings gs-charge and the fights): the state functions of hunter group 1. Rewritten by hand
 * from game_task 09BA0AE0 and the functions below it, the generated code still behind them (MHFU_HAND=0). Not yet: 09BA2060 (the charge, with a
 * count at 30ths) and 09BA2608, which stay generated.
 *
 * Each state function goes by state_step, once a tick. The first step sets the motion and marks the attack; then it waits on frames of the motion:
 * a window of frames in which a button chains to another state (chain_in_window), and the motion's end. Frames and motions are the moves' own,
 * so that's where a move's timing can be changed. What the game calls each move isn't known from this code: only motion numbers.
 *
 * Each thing called may change what's read next, so fields are read where the original reads them. */
#include "hit.h"
#include "names.h"

/* Named here by address as function names may change: */
#define set_motion f_EBOOT_088D5C74
#define frame_reached f_EBOOT_08864408
#define frame_just_reached f_EBOOT_08864348
#define reset_to_base f_game_task_09A678F8
#define set_movement_and_flags f_game_task_09A6F5C0
#define attack_begins f_game_task_09B62F48
#define state_flags_reset_motion f_EBOOT_08865CEC
#define camera_shake f_EBOOT_08812F90
#define can_chain f_game_task_09A8B058
#define before_chain f_game_task_09A8B100
#define clear_chain_flags f_game_task_09A8BC90
#define stick_says f_game_task_09A8B790
#define chain_kind f_game_task_09BA2E28
#define chain_to_state f_game_task_09BA2E68
#define connected_reaction f_game_task_09BA32E8
#define move_a f_game_task_09BA0CD8
#define move_b f_game_task_09BA1108
#define move_c f_game_task_09BA15D8
#define move_d f_game_task_09BA1E38
#define move_e f_game_task_09BA1FB8
#define charge_move f_game_task_09BA2060
#define charged_release f_game_task_09BA2608

/* In game_sub, not resident at link time: called as the game does */
#define SOUND_EFFECT 0x09C5F7E0u
#define IMPACT_EFFECT 0x09C572D0u

#define HIT_LISTS_POINTER 0x09C190D0u
#define CAMERA_POINTER 0x089CBD04u
#define VSLOT_SET_STATE 0x6C

#define MOTION_PART0 0x80u      /* offset in the hunter of what frames are asked of */
#define HUNTER_FLAG_ATTACKING 0x800
#define MISC_FLAG_WEAPON_OUT 8
#define QUEST_FLAG_1000 0x1000
#define QUEST_FLAG_800 0x800
#define REACTION_BLOCK_TICKS 60
#define AFTER_CONNECTED_SOUND 0x1C
#define WEAK_CONNECT_SOUND 0xF
#define IMPACT_Y 15.0f
#define TURN 0x2000

enum { CAMERA_SHAKE = 0, CAMERA_SHAKE_LONG = 1, CAMERA_SHAKE_OTHER = 2 };

static uint32_t set_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state, uint32_t how) {
    return call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, how);
}

static void motion(Ctx *c, Hunter *self, uint32_t id, uint32_t blend, uint32_t start_frame) {
    call4(c, set_motion, GAME_ADDR(self), id, blend, start_frame);
}

static void attack(Ctx *c, Hunter *self, uint32_t which) {
    call4(c, attack_begins, LW(HIT_LISTS_POINTER), GAME_ADDR(self), which, 0);
}

static void turn(Hunter *self, int32_t by) {
    uint32_t facing = self->facing + (uint32_t)by;
    self->facing = facing & 0xFFFF;
    self->facing_target = (uint16_t)facing;
}

static void back_to_base(Ctx *c, Hunter *self, uint32_t how) {
    call4(c, reset_to_base, GAME_ADDR(self), 0, how, 0);
}

static int motion_ended(const Hunter *self) {
    return !(self->motion_part0_flags & 1);
}

static int reached(Ctx *c, Hunter *self, float frame) {
    c->f[12].f = frame;
    return (uint8_t)call2(c, frame_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0;
}

/* From one frame up to another */
static int within(Ctx *c, Hunter *self, float from, float to) {
    return reached(c, self, from) && !reached(c, self, to);
}

static void chain_in_window(Ctx *c, Hunter *self, float from, float to, uint32_t move) {
    if (within(c, self, from, to))
        call2(c, chain_to_state, GAME_ADDR(self), move);
}

static void shake(Ctx *c, Hunter *self, uint32_t how) {
    call3(c, camera_shake, LW(CAMERA_POINTER), how, GAME_ADDR(self));
}

/* The last steps, the same in most moves */
static void until_recovered(Ctx *c, Hunter *self) {
    if (self->motion_slow_timer > 0)
        return;
    self->state_step++;
    call4_at(c, SOUND_EFFECT, GAME_ADDR(self), AFTER_CONNECTED_SOUND, 0, 0);
}

static void last_step(Ctx *c, Hunter *self, float from, float to, uint32_t move) {
    if (motion_ended(self))
        back_to_base(c, self, 6);
    else
        chain_in_window(c, self, from, to, move);
}

/* Its first connection, in a step */
static void first_connection(Ctx *c, Hunter *self, uint32_t how, uint32_t shake_then) {
    self->state_scratch_b = 1;
    shake(c, self, CAMERA_SHAKE);
    if ((uint8_t)call2(c, connected_reaction, GAME_ADDR(self), how) == 1) {
        self->state_step++;
        shake(c, self, shake_then);
    }
}

/* -- helpers ----------------------------------------------------------------------------------------------------------------------------------- */

/* game_task 09BA2E28: what button_action chains to */
void hand_hunter_weapon0_chain_kind(Ctx *c) {
    switch (OBJ(Hunter, c->r[4])->button_action) {
    case 1: c->r[2] = 1; break;
    case 2: c->r[2] = 4; break;
    case 3: c->r[2] = 2; break;
    default: c->r[2] = 0xFF; break;
    }
}

/* game_task 09BA2E68: a1 the move it is in. A button that says another move goes to that: which one by the stick */
void hand_hunter_weapon0_chain_to_state(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4], move = c->r[5];
    if ((uint8_t)call1(c, can_chain, self_addr) == 0)
        return;
    call1(c, before_chain, self_addr);
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
    call1(c, clear_chain_flags, self_addr);
    uint8_t kind = (uint8_t)call1(c, chain_kind, self_addr);
    uint32_t stick_mask, if_stick, otherwise;
    if (kind == 0 || kind == move)
        return;
    switch (kind) {
    case 4:
        if (move == 1) {
            stick_mask = 0x800000; if_stick = 0x18; otherwise = 0x16;
        } else if (move == 2) {
            stick_mask = 0x200000; if_stick = 0x19; otherwise = 0x15;
        } else {
            return;
        }
        break;
    case 2:
        if (move == 1) {
            stick_mask = 0x200000; if_stick = 0xC; otherwise = 0xA;
        } else if (move == 4 || move == 3) {
            stick_mask = 0x200000; if_stick = 0xB; otherwise = 9;
        } else {
            return;
        }
        break;
    case 1:
        if (move == 2) {
            stick_mask = 0x800000; if_stick = 8; otherwise = 6;
        } else if (move == 4 || move == 3) {
            stick_mask = 0x800000; if_stick = 7; otherwise = 5;
        } else {
            return;
        }
        break;
    default:
        return;
    }
    uint32_t state = (uint8_t)call2(c, stick_says, self_addr, stick_mask) == 1 ? if_stick : otherwise;
    set_state(c, self, 1, state, 4);
}

/* game_task 09BA32E8: what connecting does, by the grade hit_damage gave. Returns 1 when the motion is held for it. */
void hand_hunter_weapon0_connected_reaction(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4], how = c->r[5];
    switch (self->attack_connected_reaction) {
    case 3:
    case 2:
        call4(c, set_motion, self_addr, self->motion_id, 0, (uint32_t)(int32_t)(int16_t)rt_trunc_w(self->motion_part0_frame - 2.0f));
        self->sound_cue_mute_timer = 2;
        c->r[2] = 1;
        return;
    case 1:
        self->freeze_ticks = 0;
        self->freeze_ticks_pending = 0;
        if (how == 0)
            self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_ATTACKING;
        set_state(c, self, 1, 3, 0);
        break;
    case 0:
        call4_at(c, SOUND_EFFECT, self_addr, WEAK_CONNECT_SOUND, 0, 0);
        break;
    default:
        break;
    }
    c->r[2] = 0;
}

/* -- a flash at the point of impact: offset by the hunter's rotation, on the game's stack, which the effect reads --------------------------------- */

static void impact_effect(Ctx *c, Hunter *self, uint32_t offset_at, uint32_t rotated_at, uint32_t x_bits, uint32_t z_bits) {
    SW(offset_at, x_bits);
    SW(offset_at + 8, z_bits);
    SW(offset_at + 4, 0);
    /* As the game does it, with the vector registers it leaves as they are */
    c->r[6] = offset_at;
    c->r[2] = GAME_ADDR(self) + 0x10;
    c->r[3] = rotated_at;
    vfpu_lv_q(c, 0xD8C40000u);
    vfpu_lv_q(c, 0xD8480000u);
    vfpu_lv_q(c, 0xD8490010u);
    vfpu_lv_q(c, 0xD84A0020u);
    vfpu_vmov(c, 0xD0006460u);
    vfpu_vtfm3(c, 0xF104A800u);
    vfpu_sv_q(c, 0xF8600000u);
    Float32Bits x = {.u = LW(rotated_at)}, y = {.u = LW(rotated_at + 4)}, z = {.u = LW(rotated_at + 8)};
    Float32Bits out_x = {.f = self->place[0] + x.f};
    SW(offset_at, out_x.u);
    Float32Bits out_y = {.f = IMPACT_Y + (self->place[1] + y.f)};
    SW(offset_at + 4, out_y.u);
    Float32Bits out_z = {.f = self->place[2] + z.f};
    SW(offset_at + 8, out_z.u);
    call4_at(c, IMPACT_EFFECT, GAME_ADDR(self), 0, offset_at, 1);
    shake(c, self, CAMERA_SHAKE);
}

/* -- the moves --------------------------------------------------------------------------------------------------------------------------------- */

/* game_task 09BA0CD8, for states 0 and 5 to 8: motion 0x57E, a1 the way into it */
void hand_hunter_weapon0_state_motion_057e(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    switch (self->state_step) {
    case 0:
        self->hunter_flags |= HUNTER_FLAG_ATTACKING;
        self->state_scratch_b = 0;
        self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        if (way_in < 1 || way_in > 4) {
            self->state_step++;
            motion(c, self, 0x57E, 4, 0);
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            attack(c, self, 2);
            call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
            return;
        }
        if (self->state_scratch_a == 0) {
            self->state_scratch_a = 1;
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            switch (way_in) {
            case 4: turn(self, TURN); motion(c, self, 0x593, 0, 0); break;
            case 3: turn(self, TURN); motion(c, self, 0x58F, 0, 0); break;
            case 2: motion(c, self, 0x58D, 4, 0); break;
            default: motion(c, self, 0x589, 4, 0); break;
            }
            return;
        }
        if (motion_ended(self)) {
            self->state_step++;
            if (way_in == 4)
                motion(c, self, 0x57E, 0, 0x2C);
            else if (way_in == 2)
                motion(c, self, 0x57E, 2, 0x2C);
            else
                motion(c, self, 0x57E, 2, 0x1A);
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            attack(c, self, 2);
        }
        return;
    case 1:
        if (motion_ended(self)) {
            back_to_base(c, self, 4);
            return;
        }
        if (self->attack_connected != 0 && self->state_scratch_b == 0) {
            first_connection(c, self, 0, CAMERA_SHAKE_OTHER);
            return;
        }
        chain_in_window(c, self, 72.0f, 98.0f, 1);
        return;
    case 2:
        until_recovered(c, self);
        return;
    default:
        last_step(c, self, 78.0f, 104.0f, 1);
        return;
    }
}

/* game_task 09BA1108, for states 1 and 9 to 12: motion 0x57F, a1 the way into it */
void hand_hunter_weapon0_state_motion_057f(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    c->r[29] -= 0x30;
    uint32_t sp = c->r[29];
    switch (self->state_step) {
    case 0:
        self->hunter_flags |= HUNTER_FLAG_ATTACKING;
        self->state_scratch_b = 0;
        self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        if (way_in < 1 || way_in > 4) {
            self->state_step++;
            motion(c, self, 0x57F, 4, 0);
            SW(GAME_ADDR(self) + 0x3F4u, 0);
            attack(c, self, 1);
            call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
            break;
        }
        if (self->state_scratch_a == 0) {
            self->state_scratch_a = 1;
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            switch (way_in) {
            case 4: turn(self, -TURN); motion(c, self, 0x591, 0, 0); break;
            case 3: turn(self, -TURN); motion(c, self, 0x58E, 0, 0); break;
            case 2: motion(c, self, 0x58B, 4, 0); break;
            default: motion(c, self, 0x588, 4, 0); break;
            }
            break;
        }
        if (motion_ended(self)) {
            self->state_step++;
            if (way_in == 1)
                motion(c, self, 0x57F, 2, 0x40);
            else
                motion(c, self, 0x57F, 0, 0x42);
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            attack(c, self, 1);
        }
        break;
    case 1:
        if (motion_ended(self)) {
            back_to_base(c, self, 4);
            break;
        }
        if (self->attack_connected != 0 && self->state_scratch_b == 0) {
            first_connection(c, self, 0, CAMERA_SHAKE_OTHER);
            break;
        }
        if (self->state_scratch_b == 0) {
            c->f[12].f = 106.0f;
            if ((uint8_t)call2(c, frame_just_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0)
                impact_effect(c, self, sp + 0x20, sp + 0x10, 0x41F00000u /* 30 */, 0xC3160000u /* -150 */);
        }
        chain_in_window(c, self, 116.0f, 192.0f, 2);
        break;
    case 2:
        until_recovered(c, self);
        break;
    default:
        last_step(c, self, 116.0f, 192.0f, 2);
        break;
    }
    c->r[29] = sp + 0x30;
}

/* The two steps of 09BA15D8 with the motion's connection: they differ in how much of the stack */
static void motion_0594_connects(Ctx *c, Hunter *self, uint32_t offset_at, uint32_t rotated_at) {
    if (motion_ended(self)) {
        back_to_base(c, self, 8);
        return;
    }
    if (self->attack_connected != 0 && self->state_scratch_b == 0) {
        first_connection(c, self, 1, CAMERA_SHAKE_LONG);
        return;
    }
    if (self->state_scratch_b == 0) {
        c->f[12].f = 42.0f;
        if ((uint8_t)call2(c, frame_just_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0)
            impact_effect(c, self, offset_at, rotated_at, 0, 0x43160000u /* 150 */);
    }
    chain_in_window(c, self, 48.0f, 102.0f, 3);
}

#define MOTION_0594_ENTRY 0x09C0BBA0u   /* two s8 by the way into it: blend, first frame */

static void motion_0594_entry(Ctx *c, Hunter *self, uint32_t way_in) {
    int32_t blend = (int8_t)LB(MOTION_0594_ENTRY + way_in * 2), start = (int8_t)LB(MOTION_0594_ENTRY + 1 + way_in * 2);
    self->state_step++;
    motion(c, self, 0x594, (uint32_t)blend, (uint32_t)start);
    call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
    attack(c, self, 3);
}

/* game_task 09BA15D8, for states 2 and 14 to 18: motion 0x594, after one of several motions first */
void hand_hunter_weapon0_state_motion_0594(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    c->r[29] -= 0x50;
    uint32_t sp = c->r[29];
    switch (self->state_step) {
    case 0:
        self->state_step++;
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        self->misc_flags |= MISC_FLAG_WEAPON_OUT;
        self->hunter_flags |= HUNTER_FLAG_ATTACKING;
        switch (way_in) {
        case 2: motion(c, self, 0x58C, 4, 0); break;
        case 3: motion(c, self, 0x58A, 4, 0); break;
        case 4:
            self->state_step++;
            motion(c, self, 0x57D, 4, 0);
            break;
        case 5: turn(self, TURN); motion(c, self, 0x590, 0, 0); break;
        case 6: turn(self, -TURN); motion(c, self, 0x592, 0, 0); break;
        default:
            self->state_step++;
            motion(c, self, 0x579, 4, 0);
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            break;
        }
        break;
    case 1:
        if (way_in == 6) {
            if (motion_ended(self)) {
                self->state_step++;
                motion(c, self, 0x579, 0, 0x34);
            }
        } else if (way_in == 5) {
            if (reached(c, self, 26.0f)) {
                self->state_step++;
                motion(c, self, 0x596, 2, 0);
            }
        } else if (way_in == 3) {
            if (reached(c, self, 24.0f)) {
                self->state_step++;
                motion(c, self, 0x595, 2, 0);
            }
        } else if (motion_ended(self)) {
            self->state_step++;
            motion(c, self, 0x579, 2, 0x32);
        }
        break;
    case 2:
        /* Ways 1, 3, 4 and 5 go on at the motion's end, others at frame 58 */
        if (way_in == 1 || way_in == 3 || way_in == 4 || way_in == 5) {
            if (motion_ended(self)) {
                motion_0594_entry(c, self, way_in);
                if (way_in == 4)
                    self->quest_flags |= QUEST_FLAG_1000;
            }
        } else if (reached(c, self, 58.0f)) {
            motion_0594_entry(c, self, way_in);
        }
        break;
    case 3:
        motion_0594_connects(c, self, sp + 0x40, sp + 0x30);
        break;
    case 5:
        motion_0594_connects(c, self, sp + 0x20, sp + 0x10);
        break;
    case 4:
    case 6:
        until_recovered(c, self);
        break;
    default:
        last_step(c, self, 48.0f, 102.0f, 3);
        break;
    }
    c->r[29] = sp + 0x50;
}

/* game_task 09BA1E38, state 3: motion 0x57C; a button in a window of it chains on to state 1, 0x10 */
void hand_hunter_weapon0_state_motion_057c(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->state_step == 0) {
        self->state_step = 1;
        self->state_scratch_a = 0;
        self->state_scratch_b = 0;
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 0, 0);
        motion(c, self, 0x57C, 0, 0);
        return;
    }
    if (self->state_scratch_a != 0) {
        if (reached(c, self, 18.0f))
            set_state(c, self, 1, 0x10, 0xC);
    } else if (within(c, self, 2.0f, 60.0f) && self->state_scratch_a == 0 && !(self->hunter_flags & 1) && self->button_action - 1 < 3) {
        self->state_scratch_a = 1;
        self->state_scratch_b = 0;
    }
    if (motion_ended(self))
        back_to_base(c, self, 0xA);
}

/* game_task 09BA1FB8, state 4: motion 0x583 */
void hand_hunter_weapon0_state_motion_0583(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->state_step == 0) {
        self->state_step = 1;
        motion(c, self, 0x583, 0, 0);
        attack(c, self, 4);
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
    } else if (motion_ended(self)) {
        back_to_base(c, self, 4);
    }
}

/* -- the dispatch ------------------------------------------------------------------------------------------------------------------------------ */

/* game_task 09BA0AE0, for group 1 with this weapon class. States 13 and 23 do nothing. */
void hand_hunter_weapon0_state_dispatch(Ctx *c) {
    uint32_t self = c->r[4];
    uint8_t state = OBJ(Hunter, self)->state;
    static const struct { void (*fn)(Ctx *); uint8_t way_in; } states[0x1E] = {
        {move_a, 0}, {move_b, 0}, {move_c, 0}, {move_d, 0}, {move_e, 0},
        {move_a, 1}, {move_a, 2}, {move_a, 3}, {move_a, 4},
        {move_b, 1}, {move_b, 2}, {move_b, 3}, {move_b, 4},
        {NULL, 0},
        {move_c, 2}, {move_c, 3}, {move_c, 4}, {move_c, 5}, {move_c, 6},
        {charge_move, 0}, {charge_move, 1}, {charge_move, 2}, {charge_move, 3},
        {NULL, 0},
        {charge_move, 5}, {charge_move, 6},
        {charged_release, 0}, {charged_release, 1}, {charged_release, 2}, {charged_release, 3},
    };
    if (state >= 0x1E || states[state].fn == NULL)
        return;
    if (states[state].fn != move_d && states[state].fn != move_e)
        c->r[5] = states[state].way_in;
    states[state].fn(c);
}
