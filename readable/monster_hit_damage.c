/* A hit landing on a monster: how much damage it does, by the part it hit. Rewritten by hand from game_sub 09C43558, the generated code still
 * behind it (MHFU_HAND=0). Called from hit_detect.c, when a record's shape touches a part; monster_damage.c takes what this leaves off health.
 *
 * THE FORMULA, for a hit of a hunter's whose record has flag 0x40 (melee_scale, final_damage below):
 *     scale   = sharpness (a table by sharpness_level and the record) * zone
 *     zone    = the larger of  attack_percent_a% * the part's byte 1%  and  attack_percent_b% * its byte 2%     <- HIT ZONES: which_zone()
 *     physical = attack_multiplier * (the record's damage * damage_percent% * status factor * scale)
 *     element = element scale (by sharpness) * sum over five kinds of (the record's buildup_amount, if its mask says, * the part's bytes 4 to 8%)
 *     damage  = physical (a quarter for a monster's own record; at least 1 if the record has any damage) + element
 * added to pending_damage[the part's slot]. Other records don't look at attack: scale is 1, or the part's byte 3% with flag 8.
 *
 * Where to change it:
 *     hit zones           the zone tables (monster type +0x10C), and which_zone()
 *     sharpness           SHARPNESS_* tables, read in melee_scale()
 *     element             element_damage(), mask to the part's byte
 *     status factors      status_factor()
 *     stun-like buildup   direct_buildup(), the part's byte 9%
 *     what a part breaks  monster_damage.c
 *
 * Game words are not settled by this code: which of byte 1 and 2 is cut or impact, and which of the five kinds is which element, want a hit of a
 * known weapon or element watched. Each thing called may change what's read next, so fields are read where the original reads them. */
#include "hit.h"
#include "names.h"

/* Named here by address as function names may change: */
#define use_up_record f_game_sub_09C44F70
#define part_timer_bits f_game_sub_09C45140         /* 1: part +0xA & 3 with part_mask_3_ticks running; 2: & 0xC with part_mask_c_ticks */
#define target_is_remote f_game_sub_09C44FD8
#define apply_buildup_amounts f_game_sub_09C45008
#define active_skill f_EBOOT_088D6A1C
#define hunt_bonus f_EBOOT_088D6A54
#define timed_effect_test f_EBOOT_088DAE98
#define rng_next f_EBOOT_088900C4
#define object_sound_mask f_EBOOT_08866284
#define object_sound f_EBOOT_08865F8C
#define state_flags_set f_EBOOT_08865A64
#define state_flags_clear f_EBOOT_08865A9C

/* In the hunt's module, called as the game does */
#define RECORD_TEST_FLAGS 0x09B67728u
#define RECORD_NEW_STATE 0x09B678B8u
#define PART_SOUND_EFFECT 0x09ACB4E0u
#define SHARPNESS_LOSS 0x09BAE2F8u
#define ATTACKER_REACTS 0x09A67F68u
#define WEAPON_C_SOUND_COLUMN 0x09A88408u
#define KIND_11_ON_MONSTER 0x09AD88E8u

#define RNG_POINTER 0x08A632C0u
#define RULES_POINTER 0x08A62D9Cu       /* its +0x28, bit 0x40000000: a lower limit for the lowest grade */

#define VSLOT_RECORD_KIND 0x18
#define VSLOT_TARGET_F8 0xF8

#define FRAME 0x90

/* The record's flags, by the test */
enum {
    RECORD_FLAG_1 = 1,
    RECORD_FLAG_USES_8 = 8,
    RECORD_FLAG_10 = 0x10,
    RECORD_FLAG_FREEZES_ATTACKER = 0x20,
    RECORD_FLAG_GOES_BY_ATTACK = 0x40,
    RECORD_FLAG_800 = 0x800,
};

/* Tables, in game_sub's data */
#define SHARPNESS_BY_COLUMN 0x09CF97B8u     /* floats, 4 to a sharpness level */
#define SHARPNESS_SCALE 0x09CF9838u         /* a float by level */
#define SHARPNESS_ELEMENT 0x09CF9854u       /* a float by level */
#define SHARPNESS_LOSS_BY_GRADE 0x09CF9828u /* a float by grade */
#define REACTION_BY_GRADE 0x09CF9870u       /* s16 by grade */
#define HIT_SOUNDS_BY_CLASS 0x09CF9878u     /* two s16 by weapon class * 8 and grade */
#define HIT_SOUNDS_CLASS_C 0x09CF9928u      /* the same by the weapon's own column */
#define SOUND_OF_CHOICE_7 0x09CF98C4u
#define PART_EFFECT_ARG_A 0x089AA2E8u       /* s8 by subtype */
#define PART_EFFECT_ARG_B 0x089AB8BCu

/* The part's bytes in its entry of the zone table */
enum { ZONE_A = 1, ZONE_B = 2, ZONE_FLAG_8 = 3, ZONE_MASK_10 = 4, ZONE_MASK_20 = 5, ZONE_MASK_80 = 6, ZONE_MASK_40 = 7, ZONE_MASK_100 = 8, ZONE_BUILDUP = 9 };
#define ZONE_ENTRY_SIZE 10u

enum {
    WEAPON_CLASS_A = 0xA,       /* its sharpness doesn't go by the tables: the record's method says */
    WEAPON_CLASS_C = 0xC,
    SUBTYPE_1D = 0x1D,          /* several rules below are only for this subtype */
    SUBTYPE_F8_A = 0x55, SUBTYPE_F8_B = 0x34, SUBTYPE_STATE_9_A = 0x41, SUBTYPE_STATE_9_B = 0x40,
    KIND_MONSTER = 1,
};
enum { GRADE_LOW = 1, GRADE_MID = 2, GRADE_HIGH = 3 };  /* attack_connected_reaction: 0 is between low and mid */
#define GRADE_HIGH_FROM 1.0f
#define GRADE_MID_FROM 0.45f
#define GRADE_LOW_LIMIT 0.25f
#define GRADE_LOW_LIMIT_OTHER 0.29f

enum {
    BONUS_GROWTH = 0x2D, BONUS_REACTION = 0x17, BONUS_TIMES_10 = 0x2B, BONUS_MORE_BUILDUP = 0x30,
    SKILL_NO_REACTION_HALF_THE_TIME = 0x1B, SKILL_SAVES_LOW_GRADE = 0x1F,
    EFFECT_MASK_SAVES_LOW_GRADE = 1,
};
#define GROWTH_PER_HIT 1.05f
#define STATUS_BIT_GROWTH 0x2000
#define WEAK_PART_LIMIT 0.1f
#define OTHER_RECORD_ATTACK 1.15f
#define HIT_MARK_TICKS 18000
#define NO_HIT_MARK 0xFF
#define KIND_HIT_MARK 0xA
#define RECORD_KIND_HIT_MARK 6
#define KIND_HEALS 0xB
#define KIND_11 0x11
#define BUILDUP_MASK_SAVED_BY_PART 0x2000

/* All that a hit goes by, as it is worked out */
typedef struct {
    HitRecord *record;
    Hunter *attacker;           /* a hunter's or a monster's: the fields read are at the same places. 0: none */
    Monster *target;
    uint32_t record_addr, attacker_addr, target_addr, shape, part, contact, zone;
    float scale, element_scale, attack, element;
    float grade_limit;
    float amount_10, amount_20, amount_40, amount_80, amount_100;
    int no_damage;              /* a timed part's: the record is told, but no damage */
    int goes_by_attack;         /* the bonus times 10 goes by this */
    int weak_part;
    uint8_t hit_bit_1, hit_bit_2;
} Hit;

static float percent(uint8_t byte) {
    return (float)(int32_t)byte / 100.0f;
}

static float zone_percent(Ctx *c, const Hit *hit, uint32_t byte) {
    return percent(LBU(hit->zone + byte));
}

static int test_record(Ctx *c, const Hit *hit, uint32_t mask) {
    return call2_at(c, RECORD_TEST_FLAGS, hit->record_addr, mask) != 0;
}

static int has_skill(Ctx *c, const Hit *hit, uint32_t id) {
    return (uint8_t)call2(c, active_skill, hit->attacker_addr, id) == 1;
}

static void sound(Ctx *c, const Hit *hit, uint32_t id) {
    uint32_t mask = call1(c, object_sound_mask, hit->target_addr);
    c->r[8] = 0;
    c->r[9] = hit->contact;
    c->r[10] = 1;
    c->r[11] = 0;
    SW(c->r[29], 0);
    call4(c, object_sound, hit->target_addr, id, 0, mask);
}

static void set_record_state(Ctx *c, const Hit *hit, uint32_t state) {
    c->r[8] = 0xFF;
    call4_at(c, RECORD_NEW_STATE, hit->record_addr, state, 0, 0xFF);
}

static int target_is_subtype_1D(const Hit *hit) {
    return hit->target->kind == KIND_MONSTER && hit->target->subtype == SUBTYPE_1D;
}

/* -- the record used up ------------------------------------------------------------------------------------------------------------------ */

/* Its last use. A part with its timer running can take no damage, and the record goes to another state. */
static void last_use(Ctx *c, Hit *hit) {
    if (test_record(c, hit, RECORD_FLAG_10) && ((uint8_t)call2(c, part_timer_bits, hit->target_addr, hit->part) & 1)) {
        switch (hit->target->subtype) {
        case SUBTYPE_F8_A:
        case SUBTYPE_F8_B:
            call_method0(c, hit->target_addr, VSLOT_TARGET_F8);
            set_record_state(c, hit, 5);
            break;
        case SUBTYPE_STATE_9_A:
        case SUBTYPE_STATE_9_B:
            set_record_state(c, hit, 9);
            break;
        default:
            set_record_state(c, hit, 5);
            break;
        }
        hit->no_damage = 1;
    } else if (test_record(c, hit, RECORD_FLAG_1) && ((uint8_t)call2(c, part_timer_bits, hit->target_addr, hit->part) & 2)) {
        set_record_state(c, hit, 5);
        hit->no_damage = 1;
    } else {
        set_record_state(c, hit, 3);
    }
    hit->record->connected = 1;
}

/* -- a hunter's hit: sharpness and the part ----------------------------------------------------------------------------------------------- */

/* Which column of the sharpness table: the record's bytes +0x31 and +0x32, and the shape's id */
static uint32_t sharpness_column(Ctx *c, const Hit *hit) {
    uint8_t byte_31 = LBU(hit->record_addr + 0x31);
    if (byte_31 == 0xFF)
        return 0;
    if (byte_31 != 0)
        return 1;
    if (LBU(hit->record_addr + 0x32) == 0)
        return 0;
    return (int16_t)LH(hit->shape + 4) == 1 ? 3 : 2;
}

/* HIT ZONES: the larger of the two, and whether it was the first. Both are read whichever is used. */
static void which_zone(Ctx *c, const Hit *hit, float *a, float *b) {
    *a = percent(hit->attacker->attack_percent_a);
    *b = percent(hit->attacker->attack_percent_b);
    *a = *a * zone_percent(c, hit, ZONE_A);
    *b = *b * zone_percent(c, hit, ZONE_B);
}

/* The growth a bonus gives, each hit, while the status lasts */
static void growth(Ctx *c, Hit *hit) {
    if ((uint8_t)call2(c, hunt_bonus, hit->attacker_addr, BONUS_GROWTH) == 1 && (hit->attacker->status_bits & STATUS_BIT_GROWTH)) {
        float grown = hit->attacker->attack_growth * GROWTH_PER_HIT;
        hit->scale = hit->scale * grown;
        hit->attacker->attack_growth = grown;
    }
}

/* How well it bit, from the scale: what the attacker's reaction goes by */
static void grade(Ctx *c, Hit *hit) {
    Hunter *attacker = hit->attacker;
    if (hit->scale >= GRADE_HIGH_FROM) {
        attacker->attack_connected_reaction = GRADE_HIGH;
        attacker->freeze_ticks_pending = (uint8_t)(hit->record->freeze_ticks << 1);
    } else if (hit->scale >= GRADE_MID_FROM) {
        attacker->attack_connected_reaction = GRADE_MID;
        attacker->freeze_ticks_pending = 0;
        attacker->motion_slow_timer = hit->record->freeze_ticks;
    } else {
        attacker->attack_connected_reaction = hit->scale > hit->grade_limit ? 0 : GRADE_LOW;
        attacker->freeze_ticks_pending = 0;
    }
    hit->record->last_grade = attacker->attack_connected_reaction;
    (void)c;
}

/* The lowest grade can be saved by an effect, or a skill. Returns whether it was. */
static int save_low_grade(Ctx *c, Hit *hit) {
    int saved = 0;
    if (hit->attacker->attack_connected_reaction != GRADE_LOW)
        return 0;
    if ((uint8_t)call2(c, timed_effect_test, hit->attacker_addr, EFFECT_MASK_SAVES_LOW_GRADE) == 1 && hit->attacker->timed_effect_0_level >= 2)
        saved = 1;
    if (hit->attacker->weapon_uses_load != 1 && (uint8_t)call2(c, active_skill, hit->attacker_addr, SKILL_SAVES_LOW_GRADE) != 0)
        saved = 1;
    if (saved)
        hit->attacker->attack_connected_reaction = 0;
    return saved;
}

static void sharpness_loss(Ctx *c, const Hit *hit) {
    uint32_t amount = hit->record->drain_amount;
    if ((int32_t)amount > 0) {
        float by_grade = ((Float32Bits){.u = LW(SHARPNESS_LOSS_BY_GRADE + 4u * hit->attacker->attack_connected_reaction)}).f;
        call2_at(c, SHARPNESS_LOSS, hit->attacker_addr, rt_trunc_w((float)(int32_t)amount * by_grade));
    }
}

/* What the attacker does about the grade: a number from a table, which skills and bonuses change, unless its class is one of two */
static void attacker_reaction(Ctx *c, const Hit *hit, int saved, int weak_part) {
    uint8_t class = hit->attacker->weapon_class;
    if (class == WEAPON_CLASS_A || class == WEAPON_CLASS_C)
        return;
    uint32_t arg = 0;
    int32_t reaction;
    if (saved && hit->attacker->attack_connected_reaction == 0)
        reaction = (int16_t)LH(REACTION_BY_GRADE + 2);
    else
        reaction = (int16_t)LH(REACTION_BY_GRADE + 2u * hit->attacker->attack_connected_reaction);
    if (weak_part)
        reaction = (int16_t)(reaction * 5);
    if (call2(c, hunt_bonus, hit->attacker_addr, BONUS_REACTION) != 0) {
        uint8_t now = hit->attacker->attack_connected_reaction;
        if (now == GRADE_LOW || (saved && now == 0)) {
            reaction = -1;
            arg = 1;
        }
    }
    if (has_skill(c, hit, SKILL_NO_REACTION_HALF_THE_TIME) && hit->scale > hit->grade_limit && (call2(c, rng_next, LW(RNG_POINTER), 1) & 0xFFFF & 1))
        reaction = 0;
    call3_at(c, ATTACKER_REACTS, hit->attacker_addr, (uint32_t)reaction, arg);
}

/* Its sounds, by the record's choice: 0 and others from tables of two by class and grade */
static void melee_sound(Ctx *c, const Hit *hit, int saved) {
    uint8_t choice = hit->record->sound_choice;
    if (choice == 1) {
        sound(c, hit, 0x20);
        return;
    }
    if (choice == 0xA) {
        if (hit->attacker->attack_connected_reaction != GRADE_LOW)
            sound(c, hit, 0x82);
        return;
    }
    if (choice == 0xFF)
        return;
    uint32_t row;
    if (saved && hit->attacker->attack_connected_reaction == 0) {
        row = 1;
    } else if (target_is_subtype_1D(hit) && hit->attacker->attack_growth > 1.01f && hit->scale / hit->attacker->attack_growth <= hit->grade_limit) {
        row = 1;
    } else {
        row = hit->attacker->attack_connected_reaction;
    }
    int32_t second;
    if (hit->attacker->weapon_class == WEAPON_CLASS_C) {
        uint32_t column = (uint32_t)(int32_t)(int16_t)call1_at(c, WEAPON_C_SOUND_COLUMN, hit->attacker_addr);
        int32_t first = (int16_t)LH(HIT_SOUNDS_CLASS_C + (row * 2 + column * 8) * 2);
        sound(c, hit, (uint32_t)first);
        column = (uint32_t)(int32_t)(int16_t)call1_at(c, WEAPON_C_SOUND_COLUMN, hit->attacker_addr);
        second = (int16_t)LH(HIT_SOUNDS_CLASS_C + 2 + (row * 2 + column * 8) * 2);
    } else {
        int32_t first = (int16_t)LH(HIT_SOUNDS_BY_CLASS + (hit->attacker->weapon_class * 8u + row * 2) * 2);
        sound(c, hit, (uint32_t)first);
        second = (int16_t)LH(HIT_SOUNDS_BY_CLASS + 2 + (hit->attacker->weapon_class * 8u + row * 2) * 2);
    }
    if (second != 0)
        sound(c, hit, (uint32_t)second);
}

/* A record that goes by attack. Sets the scale, the element's scale and the attack; the grade, sharpness and reaction go with it. */
static void melee_scale(Ctx *c, Hit *hit) {
    const HitRecord *record = hit->record;
    Hunter *attacker = hit->attacker;
    if (record->damage_bits & 1)
        hit->hit_bit_1 = 1;
    uint32_t column = sharpness_column(c, hit);
    hit->attack = attacker->attack_multiplier;
    if (attacker->weapon_class == WEAPON_CLASS_A) {
        hit->scale = call_method0(c, hit->record_addr, VSLOT_RECORD_KIND) == 5 ? 1.3f : 1.0f;
        hit->element_scale = 1.0f;
    } else {
        uint32_t level = attacker->sharpness_level;
        float by_column = ((Float32Bits){.u = LW(SHARPNESS_BY_COLUMN + level * 16 + column * 4)}).f;
        float by_level = ((Float32Bits){.u = LW(SHARPNESS_SCALE + level * 4)}).f;
        hit->element_scale = ((Float32Bits){.u = LW(SHARPNESS_ELEMENT + level * 4)}).f;
        hit->scale = by_column * by_level;
    }
    float a, b;
    which_zone(c, hit, &a, &b);
    hit->scale = hit->scale * (a >= b ? a : b);
    if (record->damage_bits & 1) {      /* two kinds of damage in one record: only the better one's bit is noted */
        if (a >= b)
            hit->hit_bit_1 = 0;
        else
            hit->hit_bit_2 = 0;
    }
    int weak_part = hit->weak_part;
    if (weak_part && hit->scale > WEAK_PART_LIMIT)
        hit->scale = WEAK_PART_LIMIT;
    growth(c, hit);
    grade(c, hit);
    int saved = save_low_grade(c, hit);
    sharpness_loss(c, hit);
    attacker_reaction(c, hit, saved, weak_part);
    melee_sound(c, hit, saved);
    hit->goes_by_attack = 1;
}

/* -- a hunter's hit of another kind -------------------------------------------------------------------------------------------------------- */

static void other_record_sound(Ctx *c, Hit *hit) {
    switch (hit->record->sound_choice) {
    case 0xC:
        if (!target_is_subtype_1D(hit))
            sound(c, hit, 0x84);
        break;
    case 0xB:
        if (!target_is_subtype_1D(hit))
            sound(c, hit, 0x83);
        break;
    case 0xA:
        sound(c, hit, 0x82);
        break;
    case 9:
        if (!target_is_subtype_1D(hit))
            sound(c, hit, 0x81);
        hit->goes_by_attack = 1;
        break;
    case 8:
        if (!target_is_subtype_1D(hit))
            sound(c, hit, 0x80);
        hit->goes_by_attack = 1;
        break;
    case 7:
        if (!target_is_subtype_1D(hit))
            sound(c, hit, (uint32_t)(int32_t)(int16_t)LH(SOUND_OF_CHOICE_7));
        break;
    case 6: sound(c, hit, 0x4D); break;
    case 4: sound(c, hit, 0x5C); break;
    case 3:
    case 2: sound(c, hit, 0x5D); break;
    case 1: sound(c, hit, 0x20); break;
    default: break;
    }
}

/* No attack in it: scale 1, or the part's byte 3 with flag 8 */
static void other_record_scale(Ctx *c, Hit *hit) {
    hit->scale = 1.0f;
    if (hit->no_damage) {
        SB(hit->record_addr + 0x2B, 9);
        hit->record->sound_choice = 5;
    }
    if (test_record(c, hit, RECORD_FLAG_USES_8)) {
        hit->scale = hit->scale * zone_percent(c, hit, ZONE_FLAG_8);
        if (hit->attacker->weapon_class == 1 || hit->attacker->weapon_class == 5)
            hit->attack = OTHER_RECORD_ATTACK;
        hit->goes_by_attack = 1;
    }
    other_record_sound(c, hit);
}

/* -- the element --------------------------------------------------------------------------------------------------------------------------- */

static void element_amounts(Hit *hit) {
    uint16_t mask = hit->record->buildup_mask;
    float amount = (float)(int32_t)hit->record->buildup_amount;
    if (mask & 0x10)
        hit->amount_10 = amount;
    if (mask & 0x20)
        hit->amount_20 = amount;
    if (mask & 0x80)
        hit->amount_80 = amount;
    if (mask & 0x40)
        hit->amount_40 = amount;
    if (mask & 0x100)
        hit->amount_100 = amount;
}

static void element_damage(Ctx *c, Hit *hit) {
    float amount_10 = hit->amount_10 * zone_percent(c, hit, ZONE_MASK_10);
    float amount_20 = hit->amount_20 * zone_percent(c, hit, ZONE_MASK_20);
    float amount_80 = hit->amount_80 * zone_percent(c, hit, ZONE_MASK_80);
    float amount_40 = hit->amount_40 * zone_percent(c, hit, ZONE_MASK_40);
    float amount_100 = hit->amount_100 * zone_percent(c, hit, ZONE_MASK_100);
    float sum = amount_10 + amount_20;
    sum = amount_40 + sum;
    sum = amount_100 + sum;
    sum = amount_80 + sum;
    hit->element = hit->element_scale * sum;
}

/* -- the attacker told --------------------------------------------------------------------------------------------------------------------- */

static void tell_attacker(Ctx *c, Hit *hit) {
    Hunter *attacker = hit->attacker;
    attacker->connected_record_params = hit->record_addr + 0x1C;
    attacker->attack_connected = (uint8_t)((int16_t)LH(hit->part + 4) + 1);
    if (test_record(c, hit, RECORD_FLAG_FREEZES_ATTACKER)) {
        attacker->freeze_ticks_pending = hit->record->freeze_ticks;
        attacker->connected_target = hit->target_addr;
        attacker->connected_record = hit->record_addr;
    }
    if (test_record(c, hit, RECORD_FLAG_800)) {
        attacker->freeze_ticks_pending = hit->record->freeze_ticks;
        attacker->connected_target = hit->target_addr;
        attacker->connected_record = hit->record_addr;
        attacker->connected_flag_3f8 = 1;
    }
    if ((uint8_t)call2(c, part_timer_bits, hit->target_addr, hit->part) & 2) {
        attacker->attack_connected |= 0x80;
        hit->weak_part = 1;
        uint32_t mask = call1(c, object_sound_mask, hit->target_addr);
        uint8_t subtype = hit->target->subtype;
        c->r[8] = 0;
        c->r[9] = hit->contact;
        c->r[10] = (uint32_t)(int32_t)(int8_t)LB(PART_EFFECT_ARG_B + subtype);
        call4_at(c, PART_SOUND_EFFECT, hit->target_addr, 0x2D, (uint32_t)(int32_t)(int8_t)LB(PART_EFFECT_ARG_A + subtype), mask);
    }
}

static void attacker_part(Ctx *c, Hit *hit) {
    tell_attacker(c, hit);
    if (hit->attacker->kind != 0) {     /* a monster's: no more than a sound */
        sound(c, hit, 0x60);
        return;
    }
    element_amounts(hit);
    if (test_record(c, hit, RECORD_FLAG_GOES_BY_ATTACK))
        melee_scale(c, hit);
    else
        other_record_scale(c, hit);
    element_damage(c, hit);
}

/* -- the damage ----------------------------------------------------------------------------------------------------------------------------- */

/* A monster in a status takes more: 0x3CF is status_hold_mode */
static float status_factor(const Monster *target, float damage) {
    switch (target->status_hold_mode) {
    case 3:
    case 4:
        return damage * 1.1f;
    case 2:
        return target->cannot_die_ticks > 0 ? damage : damage * 3.0f;
    default:
        return damage;
    }
}

/* What the hit does to health. Returns it as an s16 */
static int32_t final_damage(const Hit *hit, uint8_t raw) {
    const HitRecord *record = hit->record;
    float damage = (float)(int32_t)raw;
    if (record->damage_percent != 0)
        damage = damage * percent(record->damage_percent);
    damage = status_factor(hit->target, damage);
    int32_t total;
    if (record->kind == KIND_HEALS)
        total = (int16_t)rt_trunc_w(-1.0f * (float)(int32_t)raw);
    else
        total = (int16_t)rt_trunc_w(hit->attack * (damage * hit->scale));
    if (record->owner_table != 0 && hit->target->subtype != SUBTYPE_1D)     /* a monster's own record, on another */
        total = (int16_t)(total / 4);
    if (total == 0 && raw != 0)
        total = 1;
    return (int16_t)(total + (int32_t)rt_trunc_w(hit->element));
}

/* The part's slot: which bits its record had, for what breaks it */
static void note_hit_bits(Ctx *c, const Hit *hit) {
    const HitRecord *record = hit->record;
    uint32_t bits_at = hit->target_addr + 0x3B0u + ((uint32_t)(int32_t)(int16_t)LH(hit->part + 6) & 7);
    uint8_t bits = LBU(bits_at);
    if (hit->hit_bit_1)
        bits |= 1;
    if ((record->damage_bits & 2) && hit->hit_bit_2)
        bits |= 2;
    if (record->damage_bits & 8)
        bits |= 8;
    if (record->buildup_mask & 0x10)
        bits |= 4;
    if (record->buildup_mask & 0x20)
        bits |= 0x10;
    if (record->buildup_mask & 0x80)
        bits |= 0x80;
    if (record->buildup_mask & 0x40)
        bits |= 0x40;
    if (record->buildup_mask & 0x100)
        bits |= 0x20;
    SB(bits_at, bits);
}

static int32_t direct_buildup(Ctx *c, const Hit *hit) {
    return (int16_t)rt_trunc_w((float)(int32_t)hit->record->direct_buildup * zone_percent(c, hit, ZONE_BUILDUP));
}

/* Whether the record's next hit keeps a bit, by the part's own */
static void buildup_mask_by_part(Ctx *c, const Hit *hit) {
    if (LBU(hit->part + 0xB) & 8)
        hit->record->buildup_mask |= BUILDUP_MASK_SAVED_BY_PART;
    else
        hit->record->buildup_mask &= (uint16_t)~BUILDUP_MASK_SAVED_BY_PART;
}

static void add_direct_buildup(Ctx *c, const Hit *hit) {
    int32_t amount = direct_buildup(c, hit);
    if (amount <= 0)
        return;
    if (hit->attacker_addr != 0 && (uint8_t)call2(c, hunt_bonus, hit->attacker_addr, BONUS_MORE_BUILDUP) == 1)
        amount = (int16_t)rt_trunc_w(1.1f * (float)amount);
    hit->target->pending_buildup_direct = (int16_t)(hit->target->pending_buildup_direct + amount);
    buildup_mask_by_part(c, hit);
}

static void hit_mark(Ctx *c, const Hit *hit) {
    if (hit->record->kind != KIND_HIT_MARK) {
        if (hit->attacker_addr == 0 || hit->attacker->weapon_class != WEAPON_CLASS_A
            || call_method0(c, hit->record_addr, VSLOT_RECORD_KIND) != RECORD_KIND_HIT_MARK)
            return;
    }
    if (hit->target->hit_mark_flag != NO_HIT_MARK && (uint8_t)call1(c, target_is_remote, hit->target_addr) == 0 && !hit->no_damage) {
        hit->target->hit_mark_flag = 1;
        hit->target->hit_mark_ticks = HIT_MARK_TICKS;
    }
}

/* game_sub 09C43558: record a0 from attacker a1 (0: none) on the monster a2, its shape a3, the part t0, at the place t1 points at */
void hand_entity_apply_hit_hitzone_damage_and_attacker_sharpness(Ctx *c) {
    Hit hit = {0};
    hit.record_addr = c->r[4];
    hit.attacker_addr = c->r[5];
    hit.target_addr = c->r[6];
    hit.shape = c->r[7];
    hit.part = c->r[8];
    hit.contact = c->r[9];
    hit.record = OBJ(HitRecord, hit.record_addr);
    hit.attacker = OBJ(Hunter, hit.attacker_addr);
    hit.target = OBJ(Monster, hit.target_addr);
    hit.scale = hit.element_scale = hit.attack = 1.0f;
    hit.hit_bit_2 = 1;
    hit.grade_limit = (LW(LW(RULES_POINTER) + 0x28) & 0x40000000u) ? GRADE_LOW_LIMIT_OTHER : GRADE_LOW_LIMIT;
    c->r[29] -= FRAME;

    HitRecord *record = hit.record;
    Monster *target = hit.target;
    record->last_grade = 0;
    /* The part's entry in the zone table: by subtype, the monster's +0x481, and the part's +4 */
    uint32_t zones = LW(LW(MONSTER_TYPE(target->subtype, MONSTER_TYPE_HIT_ZONES)) + 4u * (uint32_t)(int32_t)target->hit_zone_table);
    hit.zone = zones + ZONE_ENTRY_SIZE * (uint32_t)(int32_t)(int16_t)LH(hit.part + 4);
    uint16_t hit_id = record->hit_id;

    if ((record->uses & 0xF) == 0)
        last_use(c, &hit);
    else
        call1(c, use_up_record, hit.record_addr);
    record->last_target = hit.target_addr;
    record->last_target_kind = target->kind;
    record->last_target_slot = target->manager_slot;
    record->last_target_part = hit.part;
    if (record->owner_kind != 0xFF && hit.attacker_addr != 0)
        attacker_part(c, &hit);
    hit_mark(c, &hit);

    /* The hit's log, so as not to take it twice */
    SH(hit.target_addr + HIT_LOG + 2u * target->hit_log_index, hit_id);
    target->hit_log_index++;
    if (target->hit_log_index >= 16)
        target->hit_log_index = 0;

    /* A monster on another machine: only the record is told */
    if ((uint8_t)call1(c, target_is_remote, hit.target_addr) == 1) {
        target->last_hit_record = hit.record_addr;
        if (record->owner_table == 0 && direct_buildup(c, &hit) > 0)
            buildup_mask_by_part(c, &hit);
        c->r[29] += FRAME;
        return;
    }

    uint8_t raw = record->damage;
    int32_t damage = final_damage(&hit, raw);
    if (hit.no_damage) {
        c->r[29] += FRAME;
        return;
    }
    if (hit.goes_by_attack && hit.attacker_addr != 0 && (uint8_t)call2(c, hunt_bonus, hit.attacker_addr, BONUS_TIMES_10) == 1 && target_is_subtype_1D(&hit))
        damage = (int16_t)(damage * 10);
    note_hit_bits(c, &hit);
    uint32_t slot = (uint32_t)(int32_t)(int16_t)LH(hit.part + 6) & 7;
    SH(hit.target_addr + 0x3B8u + 2 * slot, LH(hit.target_addr + 0x3B8u + 2 * slot) + damage);
    call2(c, apply_buildup_amounts, hit.target_addr, hit.record_addr);
    if (record->owner_table == 0)
        add_direct_buildup(c, &hit);

    /* Who did how much, by hunter */
    if (record->owner_kind != 0xFF && hit.attacker->kind == 0) {
        uint32_t at = hit.target_addr + 0x482u + 2u * (hit.attacker->instance_index & 3u);
        SH(at, LH(at) + damage);
    }
    target->freeze_ticks_pending = record->freeze_ticks;
    target->hit_pending = 2;
    target->largest_damage_slot = (uint8_t)LH(hit.part + 6);
    if (test_record(c, &hit, RECORD_FLAG_FREEZES_ATTACKER))
        call2(c, state_flags_clear, hit.target_addr, 0x80000001u);
    else
        call2(c, state_flags_set, hit.target_addr, 0x80000001u);
    target->last_hit_record = hit.record_addr;
    target->last_hit_kind = record->kind;
    SH(hit.target_addr + 0x3CCu, record->power);
    target->pending_hit_flags |= record->flags;
    if (record->kind == KIND_11)
        call1_at(c, KIND_11_ON_MONSTER, hit.target_addr);
    c->r[29] += FRAME;
}
