/* The moves of weapon class 0 (the greatsword, by the recordings gs-charge and the fights): the state functions of hunter group 1. Rewritten by hand
 * from game_task 09BA0AE0 and the functions below it, and EBOOT 088D1BA8, the generated code still behind them (MHFU_HAND=0).
 *
 * Each state function goes by state_step, once a tick. The first step sets the motion and marks the attack; then it waits on frames of the motion:
 * a window of frames in which a button chains to another state (chain_in_window), and the motion's end. Frames and motions are the moves' own,
 * so that's where a move's timing can be changed. What the game calls each move isn't known from this code: only motion numbers.
 *
 * Each thing called may change what's read next, so fields are read where the original reads them. */
#include "weapon_moves.h"

/* Named here by address as function names may change: */
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
#define tier_applied f_game_task_09BA3200
#define charge_tier_of f_EBOOT_088D1BA8
#define active_skill f_EBOOT_088D6A1C

/* In game_sub, not resident at link time: called as the game does */
#define SOUND_EFFECT 0x09C5F7E0u
#define IMPACT_EFFECT 0x09C572D0u
#define HUNTER_EFFECT 0x09CB1D58u


#define HUNTER_FLAG_ATTACKING 0x800
#define AFTER_CONNECTED_SOUND 0x1C
#define WEAK_CONNECT_SOUND 0xF
#define IMPACT_Y 15.0f
#define TURN 0x2000

enum { CAMERA_SHAKE = 0, CAMERA_SHAKE_LONG = 1, CAMERA_SHAKE_OTHER = 2 };

static void turn(Hunter *self, int32_t by) {
    uint32_t facing = self->facing + (uint32_t)by;
    self->facing = facing & 0xFFFF;
    self->facing_target = (uint16_t)facing;
}

static void chain_in_window(Ctx *c, Hunter *self, float from, float to, uint32_t move) {
    if (within(c, self, from, to))
        call2(c, chain_to_state, GAME_ADDR(self), move);
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

static void impact_place(Ctx *c, Hunter *self, uint32_t offset_at, uint32_t rotated_at, uint32_t x_bits, uint32_t z_bits) {
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
}

static void impact_effect(Ctx *c, Hunter *self, uint32_t offset_at, uint32_t rotated_at, uint32_t x_bits, uint32_t z_bits) {
    impact_place(c, self, offset_at, rotated_at, x_bits, z_bits);
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

/* -- the charge ------------------------------------------------------------------------------------------------------------------------------- */

#define CHARGE_TICKS 90             /* action_count: how long a charge can be held, in 30ths */
#define SKILL_CHARGE_QUICKER 0xD7
#define SKILL_CHARGE_SLOWER 0xD8
#define CHARGE_QUICKER 0.8f         /* of the ticks; and the tier below goes by the count times 1.25 */
#define CHARGE_SLOWER 1.2f
#define CHARGE_MOTION_SPEED 0.5f    /* motion_speed_scale while it holds */
#define TIER_1_FROM 30
#define TIER_2_FROM 60
#define TIER_3_FROM 80
#define BUTTON_HOLD_CHARGES 2       /* button_action_hold */
#define CHARGE_EFFECT 8
#define TIER_3_EFFECT 0xA
#define TIER_3_START_EFFECT 4

static void hunter_effect(Ctx *c, Hunter *self, uint32_t which) {
    c->f[12].f = 1.0f;
    call2_at(c, HUNTER_EFFECT, GAME_ADDR(self), which);
}

/* EBOOT 088D1BA8: the tier the charge's count has come to, by skill; the local player's is noted in charge_tier. Returns it, a remote one's as noted. */
void hand_hunter_field_672_tier(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4];
    int32_t count = self->charge_time;
    if ((uint8_t)call2(c, active_skill, self_addr, SKILL_CHARGE_QUICKER) == 1)
        count = (int16_t)rt_trunc_w(1.25f * (float)count);
    else if ((uint8_t)call2(c, active_skill, self_addr, SKILL_CHARGE_SLOWER) == 1)
        count = (int16_t)rt_trunc_w(0.8333333f * (float)count);
    int8_t tier = count < TIER_1_FROM ? 0 : count < TIER_2_FROM ? 1 : count < TIER_3_FROM ? 2 : 3;
    if (self->instance_index == GAME()->local_hunter_slot) {
        self->charge_tier = tier;
        c->r[2] = (uint32_t)(uint8_t)tier;
    } else {
        c->r[2] = (uint8_t)self->charge_tier;
    }
}

/* game_task 09BA3200: what the tier does to the hit: attack_percent_a, which the hit zones go by (monster_hit_damage.c), and the attack's record */
void hand_hunter_weapon0_apply_tier(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    switch (self->charge_tier) {
    case 3:
        self->attack_percent_a = 130;
        attack(c, self, 0x5D);
        break;
    case 2:
        self->attack_percent_a = 120;
        attack(c, self, 0x5C);
        break;
    case 1:
        self->attack_percent_a = 110;
        attack(c, self, 0x5B);
        break;
    default:
        self->attack_percent_a = 100;
        attack(c, self, 3);
        break;
    }
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
}

/* The first tick of a charge: the motion it starts with, by the way in, and how long it lasts */
static void charge_starts(Ctx *c, Hunter *self, uint32_t way_in) {
    self->state_step++;
    self->state_scratch_a = 0;
    self->state_scratch_b = 0;
    call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
    self->misc_flags |= MISC_FLAG_WEAPON_OUT;
    self->hunter_flags |= HUNTER_FLAG_ATTACKING;
    self->charge_time = 0;
    self->charge_tier = 0;
    self->action_count = CHARGE_TICKS;
    if ((uint8_t)call2(c, active_skill, GAME_ADDR(self), SKILL_CHARGE_QUICKER) == 1)
        self->action_count = (int16_t)rt_trunc_w(CHARGE_QUICKER * (float)self->action_count);
    else if ((uint8_t)call2(c, active_skill, GAME_ADDR(self), SKILL_CHARGE_SLOWER) == 1)
        self->action_count = (int16_t)rt_trunc_w(CHARGE_SLOWER * (float)self->action_count);
    self->weapon_net_flag = 0;
    switch (way_in) {
    case 1: motion(c, self, 0x597, 2, 0); break;
    case 2: motion(c, self, 0x58C, 4, 0); break;
    case 3:
        self->state_step++;
        motion(c, self, 0x58A, 4, 0);
        break;
    case 5:
        self->state_step++;
        turn(self, TURN);
        motion(c, self, 0x590, 0, 0);
        break;
    case 6: turn(self, -TURN); motion(c, self, 0x592, 0, 0); break;
    default:
        self->state_step++;
        motion(c, self, 0x579, 4, 0);
        call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
        break;
    }
}

/* Waiting for the motion that leads to the charge. Letting go early, the local player's, goes straight to state 0x1D. */
static void charge_lead_in(Ctx *c, Hunter *self, uint32_t way_in) {
    if (way_in == 6) {
        if (motion_ended(self)) {
            self->state_step++;
            motion(c, self, 0x579, 0, 0x34);
        }
        return;
    }
    if (way_in == 2) {
        if (motion_ended(self)) {
            self->state_step++;
            motion(c, self, 0x579, 2, 0x32);
        }
        return;
    }
    if (motion_ended(self)) {
        self->state_step++;
        motion(c, self, 0x579, 0, 0x38);
        return;
    }
    if (self->instance_index != GAME()->local_hunter_slot)
        return;
    if (self->state_scratch_a == 0 && !reached(c, self, 10.0f) && self->button_action_hold != BUTTON_HOLD_CHARGES)
        self->state_scratch_a = 2;
    if (self->state_scratch_a == 2 && reached(c, self, 10.0f)) {
        self->charge_tier = 0;
        self->motion_speed_scale = 1.0f;
        call4(c, set_state_noted, GAME_ADDR(self), 1, 0x1D, 4);
    }
}

/* A remote hunter's charge only shows its effect, when told to */
static void charge_remote(Ctx *c, Hunter *self, float frame) {
    if (reached(c, self, frame) && self->state_scratch_b == 0 && self->weapon_net_flag == 1) {
        self->state_scratch_b++;
        hunter_effect(c, self, CHARGE_EFFECT);
    }
}

/* The count: while the button is held and the motion at the holding frame. The gate in it is the frame rate's: the count is at 30ths (recomp/patches.py COUNTED). */
static void charge_holds(Ctx *c, Hunter *self) {
    if (self->button_action_hold != BUTTON_HOLD_CHARGES)
        self->state_scratch_a = 1;
    if (self->state_scratch_b == 0 && self->state_scratch_a == 0) {
        self->state_scratch_b++;
        hunter_effect(c, self, CHARGE_EFFECT);
        self->weapon_net_flag = 1;
    }
    if (self->state_scratch_a != 0) {
        self->motion_speed_scale = 1.0f;
        return;
    }
    self->motion_speed_scale = CHARGE_MOTION_SPEED;
    self->charge_time = (int16_t)(self->charge_time + rt_tick30);
    self->action_count -= rt_tick30;
    if (self->action_count <= 0)
        self->state_scratch_a = 1;
}

/* At the frame it's let go: the tier, and on to the release */
static void charge_released(Ctx *c, Hunter *self, uint32_t way_in) {
    self->charge_tier = (int8_t)call1(c, charge_tier_of, GAME_ADDR(self));
    self->motion_speed_scale = 1.0f;
    call4(c, set_state_noted, GAME_ADDR(self), 1, way_in == 5 ? 0x1C : way_in == 3 ? 0x1B : 0x1A, 4);
}

/* game_task 09BA2060: states 19 to 25. a1 the way in. */
void hand_hunter_weapon0_state_charge(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    float hold_from, release_from;
    self->reaction_block_weapon_ticks = 2;
    if (self->state_step == 0) {
        charge_starts(c, self, way_in);
        return;
    }
    if (self->state_step == 1) {
        charge_lead_in(c, self, way_in);
        return;
    }
    if (way_in == 3) {
        hold_from = 0.0f;
        release_from = 24.0f;
    } else if (way_in == 5) {
        hold_from = 2.0f;
        release_from = 26.0f;
    } else {
        hold_from = 34.0f;
        release_from = 58.0f;
    }
    if (self->instance_index != GAME()->local_hunter_slot) {
        charge_remote(c, self, hold_from);
        return;
    }
    if (reached(c, self, hold_from))
        charge_holds(c, self);
    if (reached(c, self, release_from) && self->state_scratch_a == 1)
        charge_released(c, self, way_in);
}

/* -- the release ------------------------------------------------------------------------------------------------------------------------------- */

/* The release's own chain: it ends the tier and count with it */
static void release_chain(Ctx *c, Hunter *self) {
    if (within(c, self, 48.0f, 102.0f)) {
        call2(c, chain_to_state, GAME_ADDR(self), 4);
        self->charge_time = 0;
        self->charge_tier = 0;
    }
}

/* Connecting, in either of the two steps that can */
static void release_connected(Ctx *c, Hunter *self) {
    self->state_scratch_b = 1;
    shake(c, self, CAMERA_SHAKE);
    if ((uint8_t)call2(c, connected_reaction, GAME_ADDR(self), 1) == 1) {
        self->state_step++;
        shake(c, self, CAMERA_SHAKE_LONG);
    }
    self->charge_time = 0;
    self->charge_tier = 0;
}

/* The first of those steps: tier 3 has an effect of its own at the impact */
static void release_step_a(Ctx *c, Hunter *self, uint32_t sp) {
    if (motion_ended(self)) {
        back_to_base(c, self, 8);
        return;
    }
    if (self->attack_connected != 0 && self->state_scratch_b == 0) {
        release_connected(c, self);
        return;
    }
    if (self->state_scratch_b == 0) {
        c->f[12].f = 42.0f;
        if ((uint8_t)call2(c, frame_just_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0) {
            impact_place(c, self, sp + 0x40, sp + 0x30, 0, 0x43160000u /* 150 */);
            if (self->charge_tier == 3)
                hunter_effect(c, self, TIER_3_EFFECT);
            else
                call4_at(c, IMPACT_EFFECT, GAME_ADDR(self), 0, sp + 0x40, 1);
            shake(c, self, CAMERA_SHAKE);
        }
    }
    release_chain(c, self);
}

static void release_step_b(Ctx *c, Hunter *self, uint32_t sp) {
    if (motion_ended(self)) {
        back_to_base(c, self, 8);
        return;
    }
    if (self->attack_connected != 0 && self->state_scratch_b == 0) {
        release_connected(c, self);
        return;
    }
    if (self->state_scratch_b == 0) {
        c->f[12].f = 42.0f;
        if ((uint8_t)call2(c, frame_just_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0)
            impact_effect(c, self, sp + 0x20, sp + 0x10, 0, 0x43160000u /* 150 */);
    }
    release_chain(c, self);
}

/* Which way the release's turned, a little, by the stick */
static void release_aim(Ctx *c, Hunter *self) {
    if (self->charge_tier >= 3)
        hunter_effect(c, self, TIER_3_START_EFFECT);
    if (self->charge_tier <= 0)
        return;
    if ((uint8_t)call2(c, stick_says, GAME_ADDR(self), 0x800000) == 1)
        self->facing_target = (uint16_t)(self->facing + 0x1000);
    else if ((uint8_t)call2(c, stick_says, GAME_ADDR(self), 0x200000) == 1)
        self->facing_target = (uint16_t)(self->facing - 0x1000);
}

/* game_task 09BA2608: states 26 to 29: the charged swing. a1 the way in. */
void hand_hunter_weapon0_state_motion_0594_charged(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t way_in = c->r[5];
    c->r[29] -= 0x50;
    uint32_t sp = c->r[29];
    switch (self->state_step) {
    case 0:
        self->state_step++;
        self->state_scratch_b = 0;
        call4(c, set_movement_and_flags, GAME_ADDR(self), 0, 1, 0);
        self->reaction_block_weapon_ticks = REACTION_BLOCK_TICKS;
        self->hunter_flags |= HUNTER_FLAG_ATTACKING;
        switch (way_in) {
        case 3: motion(c, self, 0x582, 0, 0); break;
        case 2: motion(c, self, 0x596, 0, 0); break;
        case 1: motion(c, self, 0x595, 0, 0); break;
        default:
            self->state_step = 2;
            motion(c, self, 0x594, 2, 0);
            break;
        }
        call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
        if (way_in < 1 || way_in > 3)
            call1(c, tier_applied, GAME_ADDR(self));
        release_aim(c, self);
        break;
    case 1:
        if (motion_ended(self)) {
            self->state_step++;
            call2(c, state_flags_reset_motion, GAME_ADDR(self), 0);
            if (way_in == 1)
                motion(c, self, 0x594, 2, 8);
            else if (way_in == 3)
                motion(c, self, 0x594, 0, 0x20);
            else
                motion(c, self, 0x594, 0, 0xA);
            call1(c, tier_applied, GAME_ADDR(self));
        }
        break;
    case 2:
        release_step_a(c, self, sp);
        break;
    case 4:
        release_step_b(c, self, sp);
        break;
    case 3:
    case 5:
        until_recovered(c, self);
        break;
    default:
        if (motion_ended(self))
            back_to_base(c, self, 6);
        else
            release_chain(c, self);
        break;
    }
    c->r[29] = sp + 0x50;
}

/* -- entering a move: reached from the hunter's common input ------------------------------------------------------------------------------------- */

#define BUTTON_FIRST_MOVE_A 1       /* buttons_pressed */
#define BUTTON_FIRST_MOVE_B 0x4000
#define MIN_STAMINA_FOR_B 75

/* game_task 09BA2D10: from standing, the first move, by the button */
void hand_hunter_weapon0_pick_state_from_field_1120(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4];
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_800;
    call1(c, clear_chain_flags, self_addr);
    switch ((uint8_t)call1(c, chain_kind, self_addr)) {
    case 4:
        set_state(c, self, 1, 0x13, 4);
        return;
    case 2:
        set_state(c, self, 1, 1, 4);
        return;
    case 1:
        set_state(c, self, 1, 0, 4);
        return;
    default:
        break;
    }
    uint32_t pressed = self->buttons_pressed;
    if (pressed & BUTTON_FIRST_MOVE_A)
        set_state(c, self, 1, 4, 4);
    else if ((pressed & BUTTON_FIRST_MOVE_B) && self->stamina >= MIN_STAMINA_FOR_B)
        set_state(c, self, 0, 0x1C, 4);
}

/* game_task 09BA31B0 */
void hand_hunter_weapon0_enter_state_14(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->quest_flags |= QUEST_FLAG_800;
    call1(c, clear_chain_flags, c->r[4]);
    set_state(c, self, 1, 0x14, 4);
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
