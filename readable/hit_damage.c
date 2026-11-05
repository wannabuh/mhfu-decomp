/* A hit's damage to a hunter, worked out as it lands, and its buildups. Rewritten by hand from game_sub 09C43000 and 09C45008, the generated code
 * still behind them (MHFU_HAND=0).
 *
 * THE FORMULA, in hunter_pending_damage() below:
 *     damage = the record's damage * the attacker's attack_multiplier (1 with no attacker)
 *     damage *= the record's damage_percent / 100, if it has one
 *     damage -= damage * d / (d + 80), d being the hunter's defense_for_damage (hunter_defense.c)
 *     damage *= (100 - the resistance chosen by the record's damage_bits) / 100
 * rounded towards 0, and added to pending_damage_0: hunter_damage.c takes it off health in the hunter's tick, where a guard can change it.
 * A hit that lands on a monster doesn't come here: game_sub 09C43558 (not rewritten) goes by the hit zones.
 *
 * What the resistances and the kinds are in the game's terms isn't settled by this code: the five resistances are fields filled from armour and
 * skills (EBOOT 088D96BC), and showing which is which (fire and so on) wants a hit of a known element watched. */
#include "hit.h"
#include "names.h"

/* Named here by address as function names may change: */
#define use_up_record f_game_sub_09C44F70
#define target_is_remote f_game_sub_09C44FD8
#define apply_buildup_amounts f_game_sub_09C45008
#define kind_in_16_1E f_game_sub_09C45190
#define kind_in_16_19 f_game_sub_09C451E0
#define active_skill f_EBOOT_088D6A1C
#define timed_effect_test f_EBOOT_088DAE98
#define object_sound_mask f_EBOOT_08866284
#define object_sound f_EBOOT_08865F8C

/* In the hunt's module, called as the game does */
#define RECORD_TEST_FLAGS 0x09B67728u
#define RECORD_NEW_STATE 0x09B678B8u
#define KIND_10_EFFECT 0x09A704A8u
#define KIND_F_WHILE_HALVED 0x09A6FC78u

#define VSLOT_CAN_GUARD 0x7C

#define FRAME 0x40

/* The record's flags, by the test */
enum {
    RECORD_FLAG_FREEZES_ATTACKER = 0x20,
    RECORD_FLAG_800 = 0x800,
};

/* Kinds that do something beyond a hit, when they land on the local player's hunter */
enum {
    KIND_HEALS = 0xB,               /* takes the damage off, with nothing else working on it */
    KIND_RAISES_ATTACK = 0xE,       /* attack_bonus to 3 if below */
    KIND_RAISES_DEFENSE = 0xF,      /* defense_bonus to 10 if below, or ends the halving */
    KIND_10 = 0x10,
    KIND_ENDS_ITEM_RESTRICT = 0x15,
};
#define KIND_E_ATTACK_BONUS 3
#define KIND_F_DEFENSE_BONUS 10

#define BUILDUP_MASK_CLEARED_IF_NOT_TAKEN 0xDFFF
#define SKILL_IMMUNE_16_1E 0x5B
#define SKILL_IMMUNE_16_19 0x5A
#define EFFECT_MASK_IMMUNE 0x200
#define HIT_LOG_ENTRIES 16

enum { SOUND_CHOICE_1 = 1, SOUND_CHOICE_NONE = 0xFF };
#define SOUND_FOR_CHOICE_1 0x20
#define SOUND_FOR_OTHERS 0x21

static int test_record(Ctx *c, const HitRecord *record, uint32_t mask) {
    return call2_at(c, RECORD_TEST_FLAGS, GAME_ADDR(record), mask) != 0;
}

static int has_skill(Ctx *c, const Hunter *hunter, uint32_t id) {
    return (uint8_t)call2(c, active_skill, GAME_ADDR(hunter), id) == 1;
}

static int has_effect(Ctx *c, const Hunter *hunter, uint32_t mask) {
    return (uint8_t)call2(c, timed_effect_test, GAME_ADDR(hunter), mask) == 1;
}

/* game_sub 09C45008: a1 the hit record, on a0's buildup amounts. A hunter also has those a monster doesn't. */
void hand_object_apply_hit_mask_28_amounts(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    const HitRecord *record = OBJ(HitRecord, c->r[5]);
    if (record->buildup_mask & 2) {
        if ((record->drain_amount & 0x80) && self->kind == 0)
            self->pending_drain_seconds = (int16_t)(self->pending_drain_seconds + (record->drain_amount & 0x7F));
        else
            self->pending_buildup_mask2 = (int16_t)(self->pending_buildup_mask2 + record->buildup_amount);
    }
    if (record->buildup_mask & 4)
        self->pending_buildup_mask4 = (int16_t)(self->pending_buildup_mask4 + record->buildup_amount);
    if (record->buildup_mask & 1)
        self->pending_buildup_mask1 = (int16_t)(self->pending_buildup_mask1 + record->buildup_amount);
    if (record->buildup_mask & 8)
        self->pending_buildup_mask8 = (int16_t)(self->pending_buildup_mask8 + record->buildup_amount);
    if (self->kind != 0)
        return;
    if (record->buildup_mask & 0x200)
        self->pending_buildup_mash = (int16_t)(self->pending_buildup_mash + record->buildup_amount);
    if (record->buildup_mask & 0x400)
        self->pending_item_restrict = 1800;
    if (record->buildup_mask & 0x800)
        self->pending_stamina_debuff = 1800;
    if (record->buildup_mask & 0x1000)
        self->pending_defense_halve = (int16_t)(self->pending_defense_halve + record->buildup_amount);
}

/* -- the damage ----------------------------------------------------------------------------------------------------------------------------- */

/* The first bit that has one chooses. Bits 1, 2 and 8 have no resistance. */
static float resistance_percent(const HitRecord *record, const Hunter *hunter) {
    uint8_t bits = record->damage_bits;
    if (bits & 0x04)
        return hunter->resist_pct_hit_bit_04;
    if (bits & 0x10)
        return hunter->resist_pct_hit_bit_10;
    if (bits & 0x40)
        return hunter->resist_pct_hit_bit_40;
    if (bits & 0x20)
        return hunter->resist_pct_hit_bit_20;
    if (bits & 0x80)
        return hunter->resist_pct_hit_bit_80;
    return 0.0f;
}

static void hunter_pending_damage(const HitRecord *record, Hunter *hunter, float attack) {
    float damage = (float)(int32_t)record->damage * attack;
    if (record->damage_percent != 0)
        damage = damage * ((float)(int32_t)record->damage_percent / 100.0f);
    float defense = hunter->defense_for_damage;
    damage = damage - damage * defense / (defense + 80.0f);
    float resisted = (100.0f - resistance_percent(record, hunter)) / 100.0f;
    hunter->pending_damage_0 = (int16_t)(hunter->pending_damage_0 + (int32_t)rt_trunc_w(damage * resisted));
}

/* The hit's direct buildup, unless a skill or timed effect stops it for the kinds it stops */
static void add_direct_buildup(Ctx *c, const HitRecord *record, Hunter *hunter) {
    if (has_skill(c, hunter, SKILL_IMMUNE_16_1E)
        || (has_effect(c, hunter, EFFECT_MASK_IMMUNE) && hunter->hit_type_16_1e_immunity_level >= 2)) {
        if ((uint8_t)call1(c, kind_in_16_1E, record->kind) == 1)
            return;
    }
    if ((has_skill(c, hunter, SKILL_IMMUNE_16_19) || has_effect(c, hunter, EFFECT_MASK_IMMUNE))
        && (uint8_t)call1(c, kind_in_16_19, record->kind) == 1)
        return;
    hunter->pending_buildup_direct = (int16_t)(hunter->pending_buildup_direct + record->direct_buildup);
}

static void local_hunter_kinds(Ctx *c, const HitRecord *record, Hunter *hunter) {
    switch (record->kind) {
    case KIND_ENDS_ITEM_RESTRICT:
        hunter->item_restrict_ticks = 0;
        break;
    case KIND_10:
        call1_at(c, KIND_10_EFFECT, GAME_ADDR(hunter));
        break;
    case KIND_RAISES_DEFENSE:
        if (hunter->defense_halved_ticks != 0)
            call1_at(c, KIND_F_WHILE_HALVED, GAME_ADDR(hunter));
        else if (hunter->defense_bonus < KIND_F_DEFENSE_BONUS)
            hunter->defense_bonus = KIND_F_DEFENSE_BONUS;
        break;
    case KIND_RAISES_ATTACK:
        if (hunter->attack_bonus < KIND_E_ATTACK_BONUS)
            hunter->attack_bonus = KIND_E_ATTACK_BONUS;
        break;
    default:
        break;
    }
}

/* The attacker, a hunter or a monster, is told it connected. The fields are at the same places. */
static void tell_attacker(Ctx *c, const HitRecord *record, Hunter *attacker, uint32_t target_addr, uint32_t record_addr) {
    attacker->connected_record_params = record_addr + 0x1C;
    attacker->attack_connected = 1;
    if (test_record(c, record, RECORD_FLAG_FREEZES_ATTACKER))
        attacker->freeze_ticks_pending = record->freeze_ticks;
    if (test_record(c, record, RECORD_FLAG_800))
        attacker->connected_flag_3f8 = 1;
    attacker->attack_connected_reaction = 0;
    attacker->connected_target = target_addr;
    attacker->connected_record = record_addr;
}

/* game_sub 09C43000: record a0 from attacker a1 lands on the hunter a2 at the place a3 points at. Only for a hunter: the caller says so. */
void hand_hunter_register_hit_add_defense_scaled_damage(Ctx *c) {
    HitRecord *record = OBJ(HitRecord, c->r[4]);
    Hunter *attacker = OBJ(Hunter, c->r[5]);
    Hunter *hunter = OBJ(Hunter, c->r[6]);
    uint32_t record_addr = c->r[4], attacker_addr = c->r[5], hunter_addr = c->r[6], contact = c->r[7];
    uint16_t hit_id = record->hit_id;
    float attack = 1.0f;
    c->r[29] -= FRAME;

    /* Used up, or one use less */
    if ((record->uses & 0xF) == 0) {
        c->r[8] = 0xFF;
        call4_at(c, RECORD_NEW_STATE, record_addr, 3, 0, 0xFF);
        record->connected = 1;
    } else {
        call1(c, use_up_record, record_addr);
    }
    record->last_target = hunter_addr;
    record->last_target_kind = hunter->kind;
    record->last_target_slot = hunter->manager_slot;
    record->last_target_part = 0;
    record->last_grade = 0;
    if (record->owner_kind != 0xFF) {
        tell_attacker(c, record, attacker, hunter_addr, record_addr);
        attack = attacker->attack_multiplier;
    }

    /* The hit's log, so as not to take it twice */
    SH(hunter_addr + HIT_LOG + 2u * hunter->hit_log_index, hit_id);
    hunter->hit_log_index++;
    if (hunter->hit_log_index >= HIT_LOG_ENTRIES)
        hunter->hit_log_index = 0;
    hunter->pending_hit_bits_0 |= record->damage_bits;

    /* A remote hunter takes it on its own machine */
    if ((uint8_t)call1(c, target_is_remote, hunter_addr) == 1) {
        hunter->last_hit_record = record_addr;
        c->r[29] += FRAME;
        return;
    }
    if (hunter->instance_index == GAME()->local_hunter_slot)
        local_hunter_kinds(c, record, hunter);

    if (record->owner_table != 0 || record->owner_kind == 0xFF || record->kind == KIND_HEALS) {
        call2(c, apply_buildup_amounts, hunter_addr, record_addr);
        if (record->kind == KIND_HEALS) {
            hunter->pending_damage_0 = (int16_t)(hunter->pending_damage_0 - record->damage);
        } else {
            hunter_pending_damage(record, hunter, attack);
            add_direct_buildup(c, record, hunter);
        }
    } else {
        /* A hunter's hit on a hunter does no damage, and no buildup */
        record->buildup_mask &= BUILDUP_MASK_CLEARED_IF_NOT_TAKEN;
    }
    hunter->linked_object = attacker_addr;

    /* What the hunter's tick goes by, in hunter_damage.c */
    c->r[2] = contact;                              /* copied through C000, which it leaves there */
    vfpu_lv_q(c, 0xD8400000u);
    c->r[2] = hunter_addr + HIT_CONTACT;
    vfpu_sv_q(c, 0xF8400000u);
    hunter->hit_pending = 2;
    SB(hunter_addr + 0x286, 0);                     /* a monster's largest_damage_slot */
    hunter->last_hit_record = record_addr;
    hunter->received_hit_type = record->kind;
    hunter->received_hit_power = record->power;
    hunter->pending_hit_flags |= record->flags;

    /* Its sound, unless it can guard it */
    if ((uint8_t)call_method0(c, hunter_addr, VSLOT_CAN_GUARD) == 0 && record->sound_choice != SOUND_CHOICE_NONE) {
        uint32_t sound = record->sound_choice == SOUND_CHOICE_1 ? SOUND_FOR_CHOICE_1 : SOUND_FOR_OTHERS;
        uint32_t mask = call1(c, object_sound_mask, hunter_addr);
        c->r[8] = 0;
        c->r[9] = contact;
        c->r[10] = 1;
        c->r[11] = 0;
        SW(c->r[29], 0);
        call4(c, object_sound, hunter_addr, sound, 0, mask);
    }
    c->r[29] += FRAME;
}
