/* The hunter's defense value and its five resistances, worked out afresh from equipment, items, skills and timed effects. Rewritten by hand from
 * EBOOT 088D92F8, 088D96BC, 088D9880, 088D9EB8 and 088D9F3C, the generated code still behind them (MHFU_HAND=0).
 *
 * To change defense: the numbers below. What it does to a hit is in hit_damage.c: damage * (1 - d / (d + 80)), d being half the value here.
 * Every skill's id and amount is what the code shows; what the game calls the skills isn't known from it. */
#include "hand.h"
#include "names.h"

/* Named here by address as function names may change: */
#define equipment_stat f_EBOOT_0885D300
#define item_count f_EBOOT_088568AC
#define has_hunt_bonus f_EBOOT_088D6A54
#define timed_effect_test f_EBOOT_088DAE98
#define quest_rule_halves_defense f_EBOOT_08856E34
#define resist_skill_bonuses f_EBOOT_088D9880
#define resist_effect_active f_EBOOT_088DA150
#define resist_add_by_id f_EBOOT_088D9EB8
#define resist_add_quest_bytes f_EBOOT_088D9F3C
#define resist_last_step f_EBOOT_088DA008

#define HUD_EQUIPMENT_POINTER 0x08A62D74u           /* what the equipment's stats are asked of */
#define EQUIPMENT_DEFENSE_SELECTOR 1

/* Items that add while carried, unless the quest's flag rules them out */
#define QUEST_FLAGS_6AF14 0x6AF14u
#define QUEST_FLAG_NO_ITEM_DEFENSE 8
#define ITEM_DEFENSE_A 0x28
#define ITEM_DEFENSE_A_BONUS 16
#define ITEM_DEFENSE_B 0x29
#define ITEM_DEFENSE_B_BONUS 24

/* Skills that add a flat amount: the first found counts, in this order */
static const struct { uint8_t skill; int8_t amount; } flat_skills[] = {
    {0x52, 20}, {0x53, 30}, {0x54, 40}, {0x55, -20}, {0x56, -30}, {0x57, -40},
};

/* Skills that add when health is low */
#define LOW_HEALTH_FRACTION 0.4f
#define SKILL_LOW_HEALTH_90_A 0xA3
#define SKILL_LOW_HEALTH_90_B 0xA4
#define SKILL_LOW_HEALTH_90_C 0xD3
#define SKILL_LOW_HEALTH_42 0xA5
#define LOW_HEALTH_90 90
#define LOW_HEALTH_42 42
#define LOW_HEALTH_DEFAULT 60       /* with none of those skills: all low health gets this */

#define EFFECT_MASK_DEFENSE 4       /* a timed effect in force, from timed_effect_mask */
#define EFFECT_DEFENSE_LEVEL_1 1.15f
#define EFFECT_DEFENSE_LEVEL_2_3 1.2f
#define EFFECT_DEFENSE_LEVEL_4 1.3f

#define VERY_LOW_HEALTH 11          /* below this */
#define BONUS_VERY_LOW_HEALTH 0x15  /* adds half, with a bonus of the hunt's */

static int has_skill(Ctx *c, Hunter *self, uint32_t skill) {
    return (uint8_t)call2(c, f_EBOOT_088D6A1C, GAME_ADDR(self), skill) == 1;
}

static int32_t scaled(float factor, int32_t value) {
    return (int32_t)rt_trunc_w(factor * (float)value);
}

/* EBOOT 088D92F8 */
void hand_hunter_recalc_defense_value(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (self->instance_index != GAME()->local_hunter_slot)
        return;

    /* Equipment */
    int32_t defense = 1 + (int16_t)call4(c, equipment_stat, LW(HUD_EQUIPMENT_POINTER), GAME_ADDR(self), EQUIPMENT_DEFENSE_SELECTOR, 0);

    /* Items carried */
    uint32_t game = LW(0x089CC558u);
    if (!(LW(game + QUEST_FLAGS_6AF14) & QUEST_FLAG_NO_ITEM_DEFENSE)) {
        if ((int32_t)call2(c, item_count, game, ITEM_DEFENSE_A) > 0)
            defense += ITEM_DEFENSE_A_BONUS;
        if ((int32_t)call2(c, item_count, LW(0x089CC558u), ITEM_DEFENSE_B) > 0)
            defense += ITEM_DEFENSE_B_BONUS;
    }

    /* Bonuses, then the skills */
    defense += self->defense_bonus + self->defense_bonus_timed;
    for (size_t i = 0; i < sizeof flat_skills / sizeof flat_skills[0]; i++)
        if (has_skill(c, self, flat_skills[i].skill)) {
            defense += flat_skills[i].amount;
            break;
        }
    if (defense <= 0)
        defense = 1;

    /* Low health */
    float health_fraction = (float)self->health / (float)self->health_max;
    if (health_fraction <= LOW_HEALTH_FRACTION) {
        if (has_skill(c, self, SKILL_LOW_HEALTH_90_A) || has_skill(c, self, SKILL_LOW_HEALTH_90_B) || has_skill(c, self, SKILL_LOW_HEALTH_90_C))
            defense += LOW_HEALTH_90;
        else
            defense += has_skill(c, self, SKILL_LOW_HEALTH_42) ? LOW_HEALTH_42 : LOW_HEALTH_DEFAULT;
    }

    /* The timed effect, by its level */
    if ((uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_DEFENSE) == 1) {
        switch (self->defense_effect_level) {
        case 1: defense = scaled(EFFECT_DEFENSE_LEVEL_1, defense); break;
        case 2:
        case 3: defense = scaled(EFFECT_DEFENSE_LEVEL_2_3, defense); break;
        case 4: defense = scaled(EFFECT_DEFENSE_LEVEL_4, defense); break;
        default: break;
        }
    }
    self->defense_value = (uint16_t)defense;

    if (self->health < VERY_LOW_HEALTH && call2(c, has_hunt_bonus, GAME_ADDR(self), BONUS_VERY_LOW_HEALTH) != 0)
        self->defense_value = (uint16_t)(self->defense_value + (self->defense_value >> 1));

    /* A quest's rule, and being hit by what halves defense for a while */
    if ((uint8_t)call1(c, quest_rule_halves_defense, LW(0x089CC558u)) == 1 && self->defense_halved_ticks > 0)
        self->defense_value = self->defense_value >> 1;

    /* What a hit is worked out with: half the value, at least 1 */
    float for_damage = (float)(int32_t)self->defense_value / 2.0f;
    self->defense_for_damage = for_damage < 1.0f ? 1.0f : for_damage;
}

/* -- the resistances ------------------------------------------------------------------------------------------------------------------------ */

/* Each has the equipment's stat, then skills' bonuses (below), timed effects', bytes of the quest's, and a last step (088DA008).
 * What each is called in the game isn't settled here: hit_damage.c says which bit of a hit chooses which. */
#define RESIST_EFFECT_TABLE 0x089B6610u     /* five s16: the ids of the effects, and the index of their level */
#define RESIST_EFFECTS 5
#define EFFECT_LEVELS 0x5F7u                /* by id */
#define RESIST_LEVEL_1 10.0f
#define RESIST_LEVEL_2_3 15.0f
#define RESIST_LEVEL_4 20.0f
#define QUEST_RESIST_BYTES 0x6993Du         /* of the game's state: five s8 */
#define SKILL_RESIST_04_PLUS_10_B 0x67

static float resist_stat(Ctx *c, Hunter *self, uint32_t selector) {
    return (float)(int32_t)call4(c, equipment_stat, LW(HUD_EQUIPMENT_POINTER), GAME_ADDR(self), selector, 0);
}

/* The skills for one resistance: the first found counts. Which is which element isn't known from this code. */
static void resist_skills(Ctx *c, Hunter *self, float *resist, uint8_t plus_10, uint8_t plus_5, uint8_t minus_10, uint8_t minus_5) {
    if (has_skill(c, self, plus_10))
        *resist = *resist + 10.0f;
    else if (has_skill(c, self, plus_5))
        *resist = *resist + 5.0f;
    else if (has_skill(c, self, minus_10))
        *resist = *resist - 10.0f;
    else if (has_skill(c, self, minus_5))
        *resist = *resist - 5.0f;
}

static void all_resists_add(Hunter *self, float amount) {
    self->resist_pct_hit_bit_04 = self->resist_pct_hit_bit_04 + amount;
    self->resist_pct_hit_bit_10 = self->resist_pct_hit_bit_10 + amount;
    self->resist_pct_hit_bit_40 = self->resist_pct_hit_bit_40 + amount;
    self->resist_pct_hit_bit_20 = self->resist_pct_hit_bit_20 + amount;
    self->resist_pct_hit_bit_80 = self->resist_pct_hit_bit_80 + amount;
}

/* EBOOT 088D9880: a skill for each, then one that moves all five, first found */
void hand_hunter_resist_values_add_skill_bonuses(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    /* The first has a second skill worth the same */
    if (has_skill(c, self, SKILL_RESIST_04_PLUS_10_B))
        self->resist_pct_hit_bit_04 = self->resist_pct_hit_bit_04 + 10.0f;
    else
        resist_skills(c, self, &self->resist_pct_hit_bit_04, 0xD4, 0x66, 0x69, 0x68);
    resist_skills(c, self, &self->resist_pct_hit_bit_10, 0x6B, 0x6A, 0x6D, 0x6C);
    resist_skills(c, self, &self->resist_pct_hit_bit_40, 0x73, 0x72, 0x75, 0x74);
    resist_skills(c, self, &self->resist_pct_hit_bit_20, 0x77, 0x76, 0x79, 0x78);
    resist_skills(c, self, &self->resist_pct_hit_bit_80, 0x6F, 0x6E, 0x71, 0x70);
    if (has_skill(c, self, 0x60))
        all_resists_add(self, 3.0f);
    else if (has_skill(c, self, 0x61))
        all_resists_add(self, 5.0f);
    else if (has_skill(c, self, 0x62))
        all_resists_add(self, 10.0f);
    else if (has_skill(c, self, 0x63))
        all_resists_add(self, -3.0f);
    else if (has_skill(c, self, 0x64))
        all_resists_add(self, -5.0f);
    else if (has_skill(c, self, 0x65))
        all_resists_add(self, -10.0f);
}

/* EBOOT 088D96BC */
void hand_hunter_recalc_resist_values(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = GAME_ADDR(self);
    self->resist_pct_hit_bit_04 = resist_stat(c, self, 2);
    self->resist_pct_hit_bit_10 = resist_stat(c, self, 3);
    self->resist_pct_hit_bit_40 = resist_stat(c, self, 4);
    self->resist_pct_hit_bit_20 = resist_stat(c, self, 5);
    self->resist_pct_hit_bit_80 = resist_stat(c, self, 0xD);
    call1(c, resist_skill_bonuses, self_addr);
    for (uint32_t i = 0; i < RESIST_EFFECTS; i++) {
        int16_t id = (int16_t)LH(RESIST_EFFECT_TABLE + 2 * i);
        if ((uint8_t)call2(c, resist_effect_active, self_addr, (uint32_t)(int32_t)id) != 1)
            continue;
        switch (LBU(self_addr + EFFECT_LEVELS + (uint32_t)(int32_t)id)) {
        case 1: c->f[12].f = RESIST_LEVEL_1; break;
        case 2:
        case 3: c->f[12].f = RESIST_LEVEL_2_3; break;
        case 4: c->f[12].f = RESIST_LEVEL_4; break;
        default: continue;
        }
        call2(c, resist_add_by_id, self_addr, (uint32_t)(int32_t)id);
    }
    call1(c, resist_add_quest_bytes, self_addr);
    call1(c, resist_last_step, self_addr);
}

/* EBOOT 088D9EB8: a1 the id, f12 how much. Ids 0x13 and 0x14 aren't in the order of the fields: that's the game's. */
void hand_hunter_add_resist_value_by_id(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    switch ((int16_t)c->r[5]) {
    case 0x10: self->resist_pct_hit_bit_04 = self->resist_pct_hit_bit_04 + c->f[12].f; break;
    case 0x11: self->resist_pct_hit_bit_10 = self->resist_pct_hit_bit_10 + c->f[12].f; break;
    case 0x12: self->resist_pct_hit_bit_40 = self->resist_pct_hit_bit_40 + c->f[12].f; break;
    case 0x13: self->resist_pct_hit_bit_80 = self->resist_pct_hit_bit_80 + c->f[12].f; break;
    case 0x14: self->resist_pct_hit_bit_20 = self->resist_pct_hit_bit_20 + c->f[12].f; break;
    default: break;
    }
}

/* EBOOT 088D9F3C */
void hand_hunter_resist_values_add_game_bytes_6993d(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->resist_pct_hit_bit_04 = self->resist_pct_hit_bit_04 + (float)(int8_t)LB(LW(0x089CC558u) + QUEST_RESIST_BYTES);
    self->resist_pct_hit_bit_10 = self->resist_pct_hit_bit_10 + (float)(int8_t)LB(LW(0x089CC558u) + QUEST_RESIST_BYTES + 1);
    self->resist_pct_hit_bit_40 = self->resist_pct_hit_bit_40 + (float)(int8_t)LB(LW(0x089CC558u) + QUEST_RESIST_BYTES + 2);
    self->resist_pct_hit_bit_20 = self->resist_pct_hit_bit_20 + (float)(int8_t)LB(LW(0x089CC558u) + QUEST_RESIST_BYTES + 3);
    self->resist_pct_hit_bit_80 = self->resist_pct_hit_bit_80 + (float)(int8_t)LB(LW(0x089CC558u) + QUEST_RESIST_BYTES + 4);
}
