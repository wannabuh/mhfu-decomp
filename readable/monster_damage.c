/* Damage to a monster: what the hits of a tick add up to, and what the monster does about it. Rewritten by hand from game_task 09AC8450,
 * 09AC7A08 and 09AC9318, the generated code still behind them (MHFU_HAND=0).
 *
 * The hits have put their damage in pending_damage_N, N being the slot of the part hit (game_sub 09C43558 works out how much from the hit zone).
 * Here each slot's damage is scaled by damage_taken_scale, and taken off health and off the slot's own meter, which breaks the part when it
 * gets to 0. Where a monster's damage is to be changed as a whole, that scale, or slot_damage() below, is the place.
 *
 * Each thing called may change what's read next, so fields are read where the original reads them. */
#include "hit.h"
#include "names.h"

typedef union { uint32_t u; float f; } Float32Bits;

/* Named here by address as function names may change: */
#define add_meter_by_health_band f_game_task_09AD38F8
#define add_buildup_44c f_game_task_09ACC170
#define add_buildup_446 f_game_task_09ACC1F8
#define add_buildup_388 f_game_task_09ACC280
#define add_buildup_458 f_game_task_09ACC330
#define add_buildup_440 f_game_task_09ACC3B8
#define pick_code_from_status_mask f_game_task_09AC85F8
#define sum_pending_damage f_game_task_09AC8450
#define reset_slot_meter f_game_task_09AD3C10
#define run_broken_slots f_game_task_09AD4A40
#define call_e8_for_damaged_slots f_game_task_09AD4C58
#define set_alert_mode f_game_task_09AC93B0
#define held_markers_set f_game_task_09AD8688
#define health_in_low_band f_game_task_09AD8458
#define test_8310 f_game_task_09AD8310
#define set_type_block_644 f_game_task_09AC7478
#define load_type_block_648 f_game_task_09AC74B0
#define restart_script f_game_task_09ABAFF0
#define state_flags_clear f_EBOOT_08865A9C
#define clear_listed_area_fields f_game_task_09AD4D80

#define HIT_KIND_IN_1A_1E 0x09C451F0u       /* in game_sub, not resident at link time: called as the game does */

#define VSLOT_CAN_BREAK_SLOT_8 0xE0
#define VSLOT_STATUS_FLAG_1000 0xE4

#define PENDING_DAMAGE 0x3B8u
#define SLOT_METER 0x580u                   /* each slot's, 8 bytes on */
#define SLOT_BREAKS 0x582u
#define SLOT_METER_STRIDE 8u
#define SLOT_METER_MAX 4000
#define SLOT_BREAKS_MAX 15

/* subtypes */
enum {
    SUBTYPE_2 = 2, SUBTYPE_24 = 0x24, SUBTYPE_47 = 0x47,
    SUBTYPE_KEPT_ABOVE_1000_A = 7, SUBTYPE_KEPT_ABOVE_1000_B = 0x32, SUBTYPE_KEPT_ABOVE_1000_C = 0x37,
    SUBTYPE_4C = 0x4C, SUBTYPE_58 = 0x58,
};
#define AREA_NOT_KEPT 0xC           /* area_id 0xC: the rule below doesn't apply */
#define KEPT_ABOVE_HEALTH 1000

#define GAME_QUEST_BYTE 0x6AF10u
#define GAME_AREA 0x6AF0Eu
#define QUEST_TABLE_FOR_C 0x09CE4908u

enum {
    MONSTER_FLAG_NO_METER = 0x20,
    MONSTER_FLAG_2 = 2,
    MONSTER_FLAG_1000 = 0x1000,
    MONSTER_FLAG_2000 = 0x2000,
};

/* What the hit makes of it: what the monster's state function is told */
enum {
    RESULT_NOTHING = 0,
    RESULT_KILLED = 1,
    RESULT_DEAD_AND_NOT_HIT = 2,
    RESULT_KIND_C = 3,
    RESULT_KIND_8 = 4,
    RESULT_CODE_1 = 5,
    RESULT_CODE_4 = 6,
    RESULT_CODE_2_LOW_HEALTH = 7,
    RESULT_CODE_3 = 8,
    RESULT_FLAG_1000 = 9,
    RESULT_HOLD_MODE_2 = 0xA,
    RESULT_FLAG_2000 = 0xB,
    RESULT_SLOT_BROKE = 0xC,
    RESULT_HOLD_MODE_3 = 0xD,
    RESULT_PLAIN_HIT = 0xE,
    RESULT_KIND_1A_1E = 0xF,
    RESULT_KIND_1A_1E_SLOT_BROKE = 0x10,
    RESULT_CODE_5 = 0x11,
    RESULT_KIND_9_TEST_8310 = 0x12,
};

#define SCALE_TABLES 0x09BC7F80u            /* -> by the type record's +0x14F, -> by the quest's byte, -> records of 16 bytes by the monster's +0x4BC */
#define MONSTER_TYPE_SCALE_TABLE 0x14F
#define QUEST_POINTER 0x08A62D9Cu           /* its +0x116, s8, chooses */
#define SCALE_ON_RECORD 8
#define SUBTYPE_SCALED_AGAIN 0x59
#define BOOST_SCALES_POINTER 0x09BC2DACu    /* -> +0x48 */

/* game_task 09AC9318: the scale a monster's damage goes through, by quest. Returned in f0: what calls this stores it in damage_taken_scale.
 * To change how tough monsters are in a quest: here, or those tables. */
void hand_entity_pending_damage_scale_from_quest_table(Ctx *c) {
    const Monster *self = OBJ(Monster, c->r[4]);
    int32_t table = (int8_t)LB(MONSTER_TYPE(self->subtype, MONSTER_TYPE_SCALE_TABLE));
    int32_t quest = (int8_t)LB(LW(QUEST_POINTER) + 0x116);
    uint32_t records = LW(LW(SCALE_TABLES + 4u * (uint32_t)table) + 4u * (uint32_t)quest);
    uint32_t record = records + 16u * (uint32_t)(int32_t)(int8_t)LB(GAME_ADDR(self) + 0x4BCu);
    c->f[0].u = LW(record + SCALE_ON_RECORD);
    if (self->subtype == SUBTYPE_SCALED_AGAIN)
        c->f[0].f = c->f[0].f * ((Float32Bits){.u = LW(LW(BOOST_SCALES_POINTER) + 0x48)}).f;
}

/* A slot's damage, scaled: a positive amount never comes to nothing */
static int32_t slot_damage(Ctx *c, const Monster *self, int slot) {
    int16_t raw = (int16_t)LH(GAME_ADDR(self) + PENDING_DAMAGE + 2u * (uint32_t)slot);
    int32_t scaled = (int16_t)rt_trunc_w((float)raw * self->damage_taken_scale);
    return raw > 0 && scaled == 0 ? 1 : scaled;
}

/* game_task 09AC8450: a1 -> the sum of the damage, a2 -> the sum of what heals, both s16; the buildups pass on to their meters */
void hand_entity_sum_pending_damage_and_add_buildups(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = GAME_ADDR(self), damage_at = c->r[5], healed_at = c->r[6];
    int32_t largest = 0;
    uint8_t largest_slot = 0;
    for (int slot = 0; slot < 8; slot++) {
        int32_t amount = slot_damage(c, self, slot);
        if (amount < 0) {
            SH(healed_at, LH(healed_at) - amount);
        } else {
            SH(damage_at, LH(damage_at) + amount);
            int32_t raw = (int16_t)LH(self_addr + PENDING_DAMAGE + 2u * (uint32_t)slot);
            if ((int16_t)largest < raw) {
                largest = raw;
                largest_slot = (uint8_t)slot;
            }
        }
    }
    /* The type table can name a slot: its damage is noted if its hit bits have all the table's */
    int8_t masked_slot = (int8_t)LB(MONSTER_TYPE(self->subtype, MONSTER_TYPE_MASKED_SLOT));
    if (masked_slot != -1) {
        uint8_t mask = LBU(MONSTER_TYPE(self->subtype, MONSTER_TYPE_SLOT_MASK));
        if ((mask & LBU(self_addr + 0x3B0u + (uint32_t)(int32_t)masked_slot)) == mask) {
            int16_t raw = (int16_t)LH(self_addr + PENDING_DAMAGE + 2u * (uint32_t)(int32_t)masked_slot);
            self->pending_damage_of_type_slot = raw < 0 ? 0 : raw;
        } else {
            self->pending_damage_of_type_slot = 0;
        }
    }
    if (!(self->monster_flags & MONSTER_FLAG_NO_METER)) {
        int16_t damage = (int16_t)LH(damage_at);
        if (damage > 0)
            call2(c, add_meter_by_health_band, self_addr, (uint32_t)(int32_t)damage);
    }
    self->largest_damage_slot = largest_slot;
    if (self->cannot_die_ticks > 0)
        return;
    call2(c, add_buildup_44c, self_addr, (uint32_t)(int32_t)self->pending_buildup_mask8);
    call2(c, add_buildup_446, self_addr, (uint32_t)(int32_t)self->pending_buildup_mask1);
    call2(c, add_buildup_388, self_addr, (uint32_t)(int32_t)self->pending_buildup_mask2);
    call2(c, add_buildup_458, self_addr, (uint32_t)(int32_t)self->pending_buildup_mask4);
    call2(c, add_buildup_440, self_addr, (uint32_t)(int32_t)self->pending_buildup_direct);
}

/* -- the slots' meters, by damage_slot_mode ---------------------------------------------------------------------------------------------- */

static int16_t slot_meter(Ctx *c, Monster *self, int slot, int32_t damage) {
    uint32_t at = GAME_ADDR(self) + SLOT_METER + SLOT_METER_STRIDE * (uint32_t)slot;
    SH(at, LH(at) - damage);
    if ((int16_t)LH(at) > SLOT_METER_MAX)
        SH(at, SLOT_METER_MAX);
    return (int16_t)LH(at);
}

static void count_break(Ctx *c, Monster *self, int slot) {
    uint32_t at = GAME_ADDR(self) + SLOT_BREAKS + SLOT_METER_STRIDE * (uint32_t)slot;
    SB(at, LBU(at) + 1);
    if (LBU(at) >= SLOT_BREAKS_MAX)
        SB(at, SLOT_BREAKS_MAX);
}

/* Each returns whether a slot broke. Mode 1: every slot's damage on one meter, slot 0's */
static int one_meter(Ctx *c, Monster *self) {
    int broke = 0;
    for (int slot = 0; slot < 8; slot++) {
        if (slot_meter(c, self, 0, slot_damage(c, self, slot)) <= 0) {
            broke = 1;
            call2(c, reset_slot_meter, GAME_ADDR(self), 0);
            uint8_t bit = (uint8_t)(1u << slot);
            self->slots_zeroed_mask |= bit;
            self->slots_zeroed_total_mask |= bit;
            count_break(c, self, 0);
        }
    }
    return broke;
}

/* Mode 8: a meter for each of eight */
static int eight_meters(Ctx *c, Monster *self) {
    int broke = 0;
    for (int slot = 0; slot < 8; slot++) {
        if (slot_meter(c, self, slot, slot_damage(c, self, slot)) <= 0) {
            broke = 1;
            call2(c, reset_slot_meter, GAME_ADDR(self), (uint32_t)slot);
            uint8_t bit = (uint8_t)(1u << slot);
            if (!(self->slots_zeroed_mask & bit)) {
                self->slots_zeroed_mask |= bit;
                count_break(c, self, slot);
            }
        }
    }
    return broke;
}

/* Mode 9: nine, and the slot with the most damage among those that broke is the one noted. The ninth breaks only if a method says so. */
static int nine_meters(Ctx *c, Monster *self) {
    int broke = 0, any = 0;
    self->monster_flags &= ~(uint32_t)MONSTER_FLAG_2000;
    for (int slot = 0; slot < 9; slot++) {
        uint32_t damage_at = GAME_ADDR(self) + PENDING_DAMAGE + 2u * (uint32_t)slot;
        if (slot_meter(c, self, slot, slot_damage(c, self, slot)) > 0)
            continue;
        if (slot == 8) {
            if ((uint8_t)call_method0(c, GAME_ADDR(self), VSLOT_CAN_BREAK_SLOT_8) == 0 || self->cannot_die_ticks != 0) {
                SH(GAME_ADDR(self) + SLOT_METER + SLOT_METER_STRIDE * 8u, 1);
                continue;
            }
            self->monster_flags |= MONSTER_FLAG_2000;
        } else {
            uint8_t bit = (uint8_t)(1u << slot);
            if (!(self->slots_zeroed_mask & bit)) {
                self->slots_zeroed_mask |= bit;
                count_break(c, self, slot);
            }
            broke = 1;
        }
        call2(c, reset_slot_meter, GAME_ADDR(self), (uint32_t)slot);
        if (!any) {
            self->largest_damage_slot = (uint8_t)slot;
            any = 1;
        } else if (slot < 8 && (int16_t)LH(GAME_ADDR(self) + PENDING_DAMAGE + 2u * self->largest_damage_slot) < (int16_t)LH(damage_at)) {
            self->largest_damage_slot = (uint8_t)slot;
        }
    }
    return broke;
}

/* -- what the monster does about it -------------------------------------------------------------------------------------------------------- */

#define FRAME 0x40
#define FRAME_DAMAGE 0x3E
#define FRAME_HEALED 0x3C

static void alert(Ctx *c, Monster *self) {
    call3(c, set_alert_mode, GAME_ADDR(self), 1, (uint32_t)(int32_t)(int16_t)LH(MONSTER_TYPE(self->subtype, MONSTER_TYPE_ALERT_ARG)));
}

static int held_marker(Ctx *c, Monster *self) {
    return (uint8_t)call1(c, held_markers_set, GAME_ADDR(self));
}

static void keep_above_1000(Monster *self) {
    if (self->health < KEPT_ABOVE_HEALTH)
        self->health = KEPT_ABOVE_HEALTH;
}

/* Three subtypes' health stops at 1000 outside area 0xC, one of them only in some quests */
static void subtype_keeps_health(Ctx *c, Monster *self) {
    if (self->subtype == SUBTYPE_KEPT_ABOVE_1000_A || self->subtype == SUBTYPE_KEPT_ABOVE_1000_B) {
        if (self->area_id != AREA_NOT_KEPT)
            keep_above_1000(self);
    }
    if (self->subtype == SUBTYPE_KEPT_ABOVE_1000_C && self->area_id != AREA_NOT_KEPT) {
        uint32_t quest = LBU(LW(0x089CC558u) + GAME_QUEST_BYTE);
        uint32_t value = LHU(QUEST_TABLE_FOR_C + quest * 2);
        if (value == 0xFFFF)
            value = quest;
        if ((value & 0xFFFF) == 1)
            keep_above_1000(self);
    }
}

/* Returns 0 if it isn't killed: the others have a result */
static int killed(Ctx *c, Monster *self) {
    if (self->health > 0)
        return 0;
    self->script_run_requested = 0;
    if (!(self->monster_flags & MONSTER_FLAG_2)) {
        self->health = 0;
        self->hunters_engaged_mask = 0;
        return 1;
    }
    if (self->area_id == LHU(LW(0x089CC558u) + GAME_AREA)) {
        self->health = 0;
        self->hunters_engaged_mask = 0;
        return 1;
    }
    self->health = 1;
    return 0;
}

/* What's left to do when nothing about health decides it. Returns the result; *ended says to return it at once. */
static uint32_t result_of_hit(Ctx *c, Monster *self, int slot_broke, uint32_t out_at, int *ended) {
    uint32_t self_addr = GAME_ADDR(self);
    uint8_t code = (uint8_t)call1(c, pick_code_from_status_mask, self_addr);
    if (code != 0 || (self->monster_flags & MONSTER_FLAG_1000))
        alert(c, self);
    if (code == 1)
        return RESULT_CODE_1;
    if (code == 4)
        return RESULT_CODE_4;
    if (code == 2 && held_marker(c, self) == 1 && (uint8_t)call1(c, health_in_low_band, self_addr) == 1)
        return RESULT_CODE_2_LOW_HEALTH;
    if (held_marker(c, self) == 0 || (uint8_t)call1(c, health_in_low_band, self_addr) == 0)
        self->status_mask &= 0xFE;
    if (code == 3) {
        self->monster_flags &= ~(uint32_t)MONSTER_FLAG_1000;
        return RESULT_CODE_3;
    }
    if (code == 5) {
        self->monster_flags &= ~(uint32_t)MONSTER_FLAG_1000;
        return RESULT_CODE_5;
    }
    if (self->monster_flags & MONSTER_FLAG_1000) {
        self->monster_flags &= ~(uint32_t)MONSTER_FLAG_1000;
        call_method0(c, self_addr, VSLOT_STATUS_FLAG_1000);
        return RESULT_FLAG_1000;
    }
    if (self->hit_pending == 0) {
        *ended = 1;
        return RESULT_NOTHING;
    }
    if (self->status_hold_mode == 2)
        return RESULT_HOLD_MODE_2;
    if (self->status_hold_mode != 4 && held_marker(c, self) == 0)
        self->status_hold_mode = 0;
    if (self->monster_flags & MONSTER_FLAG_2000)
        return RESULT_FLAG_2000;
    if (self->state_group == 4) {
        *ended = 1;
        return RESULT_NOTHING;
    }
    if ((uint8_t)call1_at(c, HIT_KIND_IN_1A_1E, (uint8_t)self->last_hit_kind) == 1)
        return slot_broke ? RESULT_KIND_1A_1E_SLOT_BROKE : RESULT_KIND_1A_1E;
    if (slot_broke)
        return RESULT_SLOT_BROKE;
    if (self->status_hold_mode == 3)
        return RESULT_HOLD_MODE_3;
    if (held_marker(c, self) == 1) {
        *ended = 1;
        return RESULT_NOTHING;
    }
    call2(c, set_type_block_644, self_addr, 0);
    call2(c, load_type_block_648, self_addr, 0);
    if (LBU(out_at) == 0 || self->status_hold_mode != 0) {
        call1(c, restart_script, self_addr);
        self->script_run_requested = 1;
    }
    return RESULT_PLAIN_HIT;
}

/* The result's aftermath, whatever it was */
static void after_result(Ctx *c, Monster *self, uint32_t result) {
    uint32_t self_addr = GAME_ADDR(self);
    if (result != RESULT_PLAIN_HIT && !((self->subtype == SUBTYPE_4C || self->subtype == SUBTYPE_58) && result == RESULT_KIND_1A_1E)) {
        self->script_run_requested = 0;
        call1(c, restart_script, self_addr);
        call2(c, state_flags_clear, self_addr, 0x20000);
    }
    call1(c, clear_listed_area_fields, self_addr);
}

static uint32_t apply_pending(Ctx *c, Monster *self, uint32_t out_at) {
    uint32_t self_addr = GAME_ADDR(self), sp = c->r[29];
    int slot_broke = 0, ended = 0;
    SH(sp + FRAME_DAMAGE, 0);
    SH(sp + FRAME_HEALED, 0);
    SB(out_at, self->alert_mode);
    if (self->state_group == 5)
        return 0;
    self->slots_zeroed_mask = 0;
    if (self->health <= 0 && self->hit_pending == 0) {
        self->script_run_requested = 0;
        self->hunters_engaged_mask = 0;
        return RESULT_DEAD_AND_NOT_HIT;
    }
    if (self->hit_pending != 0) {
        self->script_requests &= 0x8D;
        call3(c, sum_pending_damage, self_addr, sp + FRAME_DAMAGE, sp + FRAME_HEALED);
        if (self->health > 0) {
            int16_t health = (int16_t)(self->health - (int16_t)LH(sp + FRAME_DAMAGE) + (int16_t)LH(sp + FRAME_HEALED));
            self->health = health;
            if (self->health_max < health)
                self->health = self->health_max;
        }
        switch (self->damage_slot_mode) {
        case 9: slot_broke = nine_meters(c, self); break;
        case 8: slot_broke = eight_meters(c, self); break;
        case 1: slot_broke = one_meter(c, self); break;
        default: break;
        }
        call2(c, run_broken_slots, self_addr, self->slots_zeroed_mask);
        call1(c, call_e8_for_damaged_slots, self_addr);
        if (self->cannot_die_ticks != 0) {
            if (self->health <= 0)
                self->health = 1;
            if (self->status_hold_mode != 2)
                alert(c, self);
            if (self->subtype != SUBTYPE_2 && self->subtype != SUBTYPE_24 && self->subtype != SUBTYPE_47)
                return 0;
            if (self->last_hit_kind != 0xC && self->last_hit_kind != 8)
                return 0;
        } else {
            subtype_keeps_health(c, self);
            if (killed(c, self))
                return RESULT_KILLED;
            alert(c, self);
        }
        /* Reactions to a kind come first */
        if (held_marker(c, self) != 1 && self->last_hit_kind == 9 && (uint8_t)call1(c, test_8310, self_addr) == 1) {
            after_result(c, self, RESULT_KIND_9_TEST_8310);
            return RESULT_KIND_9_TEST_8310;
        }
        if (self->held_marker_ticks_0 == 0) {
            if (self->last_hit_kind == 0xC) {
                after_result(c, self, RESULT_KIND_C);
                return RESULT_KIND_C;
            }
            if (self->last_hit_kind == 8) {
                after_result(c, self, RESULT_KIND_8);
                return RESULT_KIND_8;
            }
        }
    }
    uint32_t result = result_of_hit(c, self, slot_broke, out_at, &ended);
    if (ended)
        return result;
    after_result(c, self, result);
    return result;
}

/* game_task 09AC7A08: a1 -> a byte told the monster's alert mode from before. Returns the result, for its state function. */
void hand_entity_apply_pending_damage_and_choose_result_code(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t out_at = c->r[5];
    c->r[29] -= FRAME;
    uint32_t result = apply_pending(c, self, out_at);
    c->r[29] += FRAME;
    c->r[2] = result;
}
