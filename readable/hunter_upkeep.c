/* The hunter's upkeep once a tick: timers, stamina recovery, health regeneration and the timed effects. Rewritten by hand from
 * game_task 09A68FC8, the generated code still behind it (MHFU_HAND=0).
 *
 * The order of the timers is the original's. They look independent, but calls come between them and the callees can change what
 * follows, so fields are read where the original reads them. */
#include "hand.h"
#include "names.h"

#define HUD_POINTER 0x089CBD0Cu
#define GAME_POINTER 0x089CC558u
#define RNG_POINTER 0x08A632C0u
#define SOUND_2D_POINTER 0x08A62EACu
#define SOUND_STATE_POINTER 0x08A62EB0u
#define AREA_HOLDER_POINTER 0x09A4F1A0u     /* -> -> the object whose method gives the area record */
#define VSLOT_AREA_RECORD 0x28
#define VSLOT_ADD_HEALTH 0x68
#define AREA_RECORD_TYPE 0x3C               /* a byte */

/* Called as the game does: it is in another overlay, not resident at link time */
#define OWN_BANK_SOUND 0x09C557E0u

/* Weapon classes: what the game calls them isn't known from this code */
enum {
    WEAPON_CLASS_NO_RECOVERY_LIST_A = 2,
    WEAPON_CLASS_NO_RECOVERY_LIST_B = 3,
    WEAPON_CLASS_GAUGE = 7,             /* has weapon7_gauge */
    WEAPON_CLASS_CHARGE = 9,            /* counts charge_time down, and takes the flag below with it */
    WEAPON_CLASS_NO_RECOVERY_LIST_C = 10,
};

enum {
    MISC_FLAG_STAMINA_DRAIN = 0x10,     /* stamina falls by 1 instead of recovering */
    QUEST_FLAG_CHARGE_RUNNING = 0x10,
    OBJECT_FLAG_NO_STAMINA_RECOVERY = 0x200,
    EFFECT_MASK_AREA_DRAIN_OFF = 0x100, /* the bit entry 10 of the timed effects sets in +0x6CC: as area_1_2_ward_ticks */
    EFFECT_MASK_REGEN = 0x20,           /* entry 7's */
    STATUS_BIT_REGEN_IN_ANY_AREA = 0x80,
};

/* Messages for the local player, of kind 0, as a timer ends */
enum {
    MESSAGE_AREA_1_2_WARD_ENDS = 0x12,
    MESSAGE_AREA_3_4_WARD_ENDS = 0x13,
    MESSAGE_STAMINA_BLOCK_ENDS = 0x15,
    MESSAGE_REGEN_BOOST_ENDS = 0x16,
    MESSAGE_KIND_ITEM = 1,
    MESSAGE_ITEM_USED_UP = 5,
};

/* Entries of the timed effects that matter here */
enum {
    EFFECT_HEALTH_MAX = 3,
    EFFECT_STAMINA_BLOCK = 5,
    EFFECT_REGEN = 7,
    EFFECT_AREA_3_4_WARD = 9,
    EFFECT_AREA_1_2_WARD = 10,
    EFFECT_COUNT = 21,
};

/* Skills. What the game calls them isn't known from the code, only what they do here. */
enum {
    SKILL_MAP_DETECT = 0x95,
    SKILL_REGEN_IN_ANY_AREA = 0x7B,
    SKILL_REGEN_IN_AREA_1 = 0x7A,
    SKILL_GROUND_1_NO_MATTER_A = 0x0B,
    SKILL_GROUND_1_NO_MATTER_B = 0xD0,
    SKILL_GROUND_1_NO_MATTER_C = 0xD6,
    SKILL_GROUND_2_NO_MATTER_A = 0xB2,
    SKILL_GROUND_2_NO_MATTER_B = 0xD4,
    SKILL_REGEN_EVERY_15 = 0x18,
    SKILL_REGEN_EVERY_30 = 0x17,
    SKILL_REGEN_EVERY_120 = 0x19,
    SKILL_REGEN_EVERY_240 = 0x1A,
};

#define REGEN_INTERVAL 60               /* in the game's count of 30ths */
#define MAP_DETECT_ROLL_TICKS 9000
#define MAP_DETECT_TICKS 300
#define MAP_DETECT_ODDS 3               /* detection unless the number divides by this */
#define GAUGE_INTERVAL 10
#define CHARGE_END_MODE_TICKS 10
#define STAMINA_RECOVERY 2
#define STAMINA_DRAIN -1
#define NEAR_OBJECT_YES 1               /* hunter_near_table_object_kind_40_41's answer that counts as being on ground 2 */
#define LISTED_ITEM_NONE 0xFFFF
#define LISTED_ITEM_LOSS -10
#define LISTED_ITEM_SOUND 0x18FA
#define LISTED_ITEM_DUCK 4
#define CHARGE_END_SOUND 0x15
#define CHARGE_END_SOUND_2D 0x21

#define COUNT_DOWN(timer) do { if ((timer) != 0) (timer)--; } while (0)

static int is_local_hunter(Ctx *c, const Hunter *self) {
    return self->instance_index == GAME()->local_hunter_slot;
}

/* Messages and the sound with them are for the local player's hunter, and not while the game's mode allows index 1 */
static int shows_messages(Ctx *c, const Hunter *self) {
    return is_local_hunter(c, self) && (uint8_t)call2(c, unknown_mode_byte_2e_allows_index, LW(GAME_POINTER), 1) == 0;
}

static void hud_message(Ctx *c, uint32_t kind, uint32_t id, uint32_t arg) {
    call4(c, hud_message_queue_push, LW(HUD_POINTER), kind, id, arg);
}

/* The original tests this call's result three ways: the whole word, the low byte against 0, the low byte against 1. Each stays as it is. */
static uint32_t skill(Ctx *c, const Hunter *self, uint32_t id) {
    return call2(c, hunter_has_active_skill, GAME_ADDR(self), id);
}

static int skill_is_one(Ctx *c, const Hunter *self, uint32_t id) {
    return (uint8_t)skill(c, self, id) == 1;
}

static uint8_t area_type(Ctx *c) {
    uint32_t holder = LW(LW(AREA_HOLDER_POINTER));
    return LBU(call_method0(c, holder, VSLOT_AREA_RECORD) + AREA_RECORD_TYPE);
}

/* Counts down, and below zero goes to zero: only -32768, by wrapping, doesn't */
static void count_down_clamped(int16_t *timer) {
    if (*timer != 0) {
        int16_t left = (int16_t)(*timer - 1);
        *timer = left < 0 ? 0 : left;
    }
}

/* Counts down, positive values only */
static void count_down_positive(int16_t *timer) {
    if (*timer > 0)
        (*timer)--;
}

/* A timer's end is told only if the effect entry doing the same thing isn't still running */
static void tell_timer_ended(Ctx *c, const Hunter *self, const int16_t *effect_ticks, uint32_t message) {
    if (shows_messages(c, self) && *effect_ticks == 0)
        hud_message(c, 0, message, 0);
}

/* A timer that goes faster in the area type it wards against: 2 on even counts, 1 on odd, so 1.5 a tick on average */
static void ward_count_down(Ctx *c, Hunter *self, uint16_t *ticks, uint8_t faster_in, const int16_t *effect_ticks, uint32_t message) {
    if (*ticks == 0)
        return;
    if (area_type(c) == faster_in) {
        int step = (GAME()->tick_30ths & 1) ? 1 : 2;
        *ticks = *ticks < step ? 0 : (uint16_t)(*ticks - step);
    } else {
        (*ticks)--;
    }
    if (*ticks == 0)
        tell_timer_ended(c, self, effect_ticks, message);
}

static void map_detect_skill(Ctx *c, Hunter *self) {
    COUNT_DOWN(self->map_detect_skill_ticks);
    if (self->map_detect_skill_roll_ticks != 0) {
        self->map_detect_skill_roll_ticks--;
        return;
    }
    self->map_detect_skill_roll_ticks = MAP_DETECT_ROLL_TICKS;
    uint32_t number = call2(c, math_rng_next, LW(RNG_POINTER), 1) & 0xFFFF;
    if (number % MAP_DETECT_ODDS != 0)
        self->map_detect_skill_ticks = MAP_DETECT_TICKS;
}

static void timed_bonus_count_down(uint16_t *ticks, int8_t *bonus) {
    if (*ticks != 0) {
        (*ticks)--;
        if (*ticks == 0)
            *bonus = 0;
    }
}

static void count_down_timers(Ctx *c, Hunter *self) {
    call1(c, hunter_tick_countdown_fields, GAME_ADDR(self));
    COUNT_DOWN(self->reaction_block_weapon_ticks);
    COUNT_DOWN(self->push_off_ticks);
    COUNT_DOWN(self->hit_immune_ticks);
    COUNT_DOWN(self->reaction_block_item_ticks);
    COUNT_DOWN(self->menu_camera_lock_ticks);
    count_down_positive(&self->repeat_use_ticks);
    if (self->stamina_loss_block_ticks != 0) {
        self->stamina_loss_block_ticks--;
        if (self->stamina_loss_block_ticks == 0)
            tell_timer_ended(c, self, &self->stamina_block_effect_ticks, MESSAGE_STAMINA_BLOCK_ENDS);
    }
    ward_count_down(c, self, &self->area_1_2_ward_ticks, 2, &self->area_1_2_ward_effect_ticks, MESSAGE_AREA_1_2_WARD_ENDS);
    ward_count_down(c, self, &self->area_3_4_ward_ticks, 4, &self->area_3_4_ward_effect_ticks, MESSAGE_AREA_3_4_WARD_ENDS);
    if (self->health_regen_boost_ticks != 0) {
        self->health_regen_boost_ticks--;
        if (self->health_regen_boost_ticks == 0)
            tell_timer_ended(c, self, &self->health_regen_effect_ticks, MESSAGE_REGEN_BOOST_ENDS);
    }
    COUNT_DOWN(self->map_detect_item_ticks);
    COUNT_DOWN(self->map_detect_ticks);
    if (skill(c, self, SKILL_MAP_DETECT) != 0)
        map_detect_skill(c, self);
    timed_bonus_count_down(&self->attack_bonus_ticks, &self->attack_bonus_timed);
    timed_bonus_count_down(&self->defense_bonus_ticks, &self->defense_bonus_timed);
    COUNT_DOWN(self->mash_status_left);
    COUNT_DOWN(self->weapon_mode_ticks);
    count_down_positive(&self->item_restrict_ticks);
    count_down_clamped(&self->defense_halved_ticks);
    count_down_clamped(&self->stamina_debuff_ticks);
}

/* -- The two weapon classes with a timer of their own ---------------------------------------------------------------------------- */

static void play_charge_end_sounds(Ctx *c, Hunter *self) {
    c->r[4] = GAME_ADDR(self);
    c->r[5] = CHARGE_END_SOUND;
    c->r[6] = 0;
    c->r[7] = GAME_ADDR(self->place);
    c->r[8] = 1;
    c->r[9] = 0;
    rt_call(c, OWN_BANK_SOUND);
    if (!is_local_hunter(c, self))
        return;
    /* eight arguments in registers, and a ninth at the callee's sp+0 */
    c->r[29] -= 0x10;
    SW(c->r[29], 0);
    c->r[4] = LW(SOUND_2D_POINTER);
    c->r[5] = 1;
    c->r[6] = CHARGE_END_SOUND_2D;
    c->r[7] = 0;
    c->r[8] = 0;
    c->r[9] = 0;
    c->r[10] = 0;
    c->r[11] = 0;
    sound_play_2d(c);
    c->r[29] += 0x10;
}

/* Native frame rate: the charge counts in 30ths, so a frame between two of them leaves it where it is. */
static void weapon_class_timers(Ctx *c, Hunter *self) {
    if (self->weapon_class == WEAPON_CLASS_GAUGE && self->weapon_mode_ticks == 0 && self->weapon7_gauge != 0 &&
        GAME()->tick_30ths % GAUGE_INTERVAL == 0)
        self->weapon7_gauge--;
    if (self->weapon_class != WEAPON_CLASS_CHARGE || self->charge_time <= 0)
        return;
    self->charge_time -= rt_tick30;
    if (self->charge_time != 0) {
        self->quest_flags |= QUEST_FLAG_CHARGE_RUNNING;
        return;
    }
    self->quest_flags &= ~(uint32_t)QUEST_FLAG_CHARGE_RUNNING;
    play_charge_end_sounds(c, self);
    self->weapon_mode_ticks = CHARGE_END_MODE_TICKS;
}

/* -- Stamina --------------------------------------------------------------------------------------------------------------------- */

/* Which states stamina recovers in, by state group and sometimes weapon class. */
static int state_lets_stamina_recover(const Hunter *self) {
    uint8_t state = self->state;
    switch (self->state_group) {
    case 3:
        return 0;
    case 2:
        switch (state) {
        case 0x1B: case 0x1C: case 0x1E: case 0x1F: case 0x21: case 0x22: case 0x23: case 0x24:
            return 1;
        default:
            return 0;
        }
    case 1:
        switch (self->weapon_class) {
        case WEAPON_CLASS_NO_RECOVERY_LIST_C:
            return state != 0x12 && state != 0x0A && state != 0x08 && self->charge_time == 0;
        case WEAPON_CLASS_NO_RECOVERY_LIST_B:
            return state != 0x10 && state != 0x05 && state != 0x0E && state != 0x03;
        case WEAPON_CLASS_NO_RECOVERY_LIST_A:
            return state != 0x04 && state != 0x0E && state != 0x0D && state != 0x0C && state != 0x03;
        default:
            return 1;
        }
    case 0:
        switch (state) {
        case 0x1C: case 0x25: case 0x2D: case 0x31: case 0x32: case 0x44: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x77:
        case 0x78: case 0x7E:
            return 0;
        default:
            return 1;
        }
    default:
        return 1;
    }
}

static void stamina_upkeep(Ctx *c, Hunter *self) {
    uint32_t addr = GAME_ADDR(self);
    if (call2(c, object_test_flags_288, addr, OBJECT_FLAG_NO_STAMINA_RECOVERY) == 0 && !(self->misc_flags & MISC_FLAG_STAMINA_DRAIN) &&
        state_lets_stamina_recover(self))
        call2(c, hunter_stamina_add_clamped, addr, STAMINA_RECOVERY);
    if (self->misc_flags & MISC_FLAG_STAMINA_DRAIN) {
        call2(c, hunter_stamina_add_with_skills_a8_a9, addr, (uint32_t)STAMINA_DRAIN);
        if (self->stamina <= 0 && self->state_group != 1)
            self->misc_flags &= ~(uint32_t)MISC_FLAG_STAMINA_DRAIN;
    }
    call1(c, hunter_tick_max_stamina_decay, addr);
}

/* -- Health regeneration --------------------------------------------------------------------------------------------------------- */

/* Area types 1 and 2 hold regeneration back, unless warded or something exempts the hunter. */
static int area_holds_regen_back(Ctx *c, Hunter *self, uint8_t area) {
    if (self->area_1_2_ward_ticks != 0)
        return 0;
    if (skill(c, self, SKILL_REGEN_IN_ANY_AREA) != 0)
        return 0;
    if (call2(c, hunter_flags_6cc_test_mask, GAME_ADDR(self), EFFECT_MASK_AREA_DRAIN_OFF) != 0)
        return 0;
    if (self->status_bits & STATUS_BIT_REGEN_IN_ANY_AREA)
        return 0;
    if (area == 2)
        return 1;
    return area == 1 && skill(c, self, SKILL_REGEN_IN_AREA_1) == 0;
}

/* The ground's effect that holds regeneration back, 0 if none: its kind, or 2 near certain objects; skills can take it away. */
static uint8_t ground_effect(Ctx *c, Hunter *self) {
    uint8_t ground = self->ground_effect_kind;
    if (ground == 0 && (uint8_t)call1(c, hunter_near_table_object_kind_40_41, GAME_ADDR(self)) == NEAR_OBJECT_YES &&
        (uint8_t)skill(c, self, SKILL_GROUND_2_NO_MATTER_A) == 0 && (uint8_t)skill(c, self, SKILL_GROUND_2_NO_MATTER_B) == 0)
        ground = 2;
    if (ground == 1 && (skill_is_one(c, self, SKILL_GROUND_1_NO_MATTER_A) || skill_is_one(c, self, SKILL_GROUND_1_NO_MATTER_B) ||
                        skill_is_one(c, self, SKILL_GROUND_1_NO_MATTER_C)))
        ground = 0;
    if (ground == 2 && (skill_is_one(c, self, SKILL_GROUND_2_NO_MATTER_A) || skill_is_one(c, self, SKILL_GROUND_2_NO_MATTER_B)))
        ground = 0;
    return ground;
}

static int regen_interval(Ctx *c, Hunter *self) {
    if (skill_is_one(c, self, SKILL_REGEN_EVERY_15))
        return 15;
    if (skill_is_one(c, self, SKILL_REGEN_EVERY_30))
        return 30;
    if (skill_is_one(c, self, SKILL_REGEN_EVERY_120))
        return 120;
    if (skill_is_one(c, self, SKILL_REGEN_EVERY_240))
        return 240;
    return REGEN_INTERVAL;
}

/* Up to the red limit, through the object's own method */
static void regen_step(Ctx *c, Hunter *self) {
    if (self->health >= self->health_red_limit)
        return;
    int16_t room = (int16_t)(self->health_red_limit - self->health);
    int16_t amount = self->health_regen_boost_ticks != 0 ? 2 : 1;
    if ((uint8_t)call2(c, hunter_flags_6cc_test_mask, GAME_ADDR(self), EFFECT_MASK_REGEN) == 1)
        amount += self->health_regen_effect_level < 3 ? 1 : 2;
    call_method1(c, GAME_ADDR(self), VSLOT_ADD_HEALTH, (uint32_t)(int32_t)(room < amount ? room : amount));
}

static void regen(Ctx *c, Hunter *self) {
    uint8_t ground = ground_effect(c, self);
    if (self->drain_status_seconds != 0 || ground != 0)
        return;
    if ((int16_t)call1(c, hunter_count_pouch_item_ids_371_372, GAME_ADDR(self)) > 0 &&
        (call3(c, object_state_is, GAME_ADDR(self), 0, 0x5B) != 0 || call3(c, object_state_is, GAME_ADDR(self), 5, 0x0B) != 0))
        return;
    int interval = regen_interval(c, self);
    if (GAME()->tick_30ths % interval == 0)
        regen_step(c, self);
}

static void buildup_decay(Hunter *self) {
    if (self->buildup_decay_delay_ticks > 0) {
        self->buildup_decay_delay_ticks--;
    } else {
        self->buildup_decay_delay_ticks = 0;
        COUNT_DOWN(self->buildup_state_2_13);
    }
}

static void health_upkeep(Ctx *c, Hunter *self) {
    if (self->state_group == 2 || self->state_group == 3)
        return;
    if ((uint8_t)call1(c, hunter_fields_1144_1146_or_listed_states, GAME_ADDR(self)) != 0)
        return;
    uint8_t area = area_type(c);
    if (!area_holds_regen_back(c, self, area))
        regen(c, self);
    buildup_decay(self);
}

/* -- The listed item ------------------------------------------------------------------------------------------------------------- */

/* The states the item isn't used up in, each asked after in turn */
static int in_a_state_keeping_the_listed_item(Ctx *c, Hunter *self) {
    static const uint8_t states[][2] = {
        {0, 0x47}, {0, 0x4A}, {0, 0x51}, {0, 0x52}, {0, 0x5D}, {0, 0x60}, {0, 0x73}, {0, 0x4E}, {0, 0x7C}, {0, 0x7D},
        {5, 0x0A}, {5, 0x0B}, {5, 0x0C}, {5, 0x0D},
    };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++)
        if ((uint8_t)call3(c, object_state_is, GAME_ADDR(self), states[i][0], states[i][1]) != 0)
            return 1;
    return 0;
}

static void use_up_listed_item(Ctx *c, Hunter *self) {
    if (in_a_state_keeping_the_listed_item(c, self))
        return;
    uint32_t item = call1(c, hunter_pouch_find_last_listed_item, GAME_ADDR(self)) & 0xFFFF;
    if (item == LISTED_ITEM_NONE)
        return;
    if (shows_messages(c, self)) {
        call2(c, sound_state_duck_slot_0, LW(SOUND_STATE_POINTER), LISTED_ITEM_DUCK);
        call2(c, sound_state_play_on_slot_1, LW(SOUND_STATE_POINTER), LISTED_ITEM_SOUND);
        hud_message(c, MESSAGE_KIND_ITEM, MESSAGE_ITEM_USED_UP, (uint32_t)(int32_t)(int16_t)item);
    }
    call4(c, pouch_change_item_count, GAME_ADDR(self), item, (uint32_t)LISTED_ITEM_LOSS, 0);
}

static void lockouts_and_listed_item(Ctx *c, Hunter *self) {
    COUNT_DOWN(self->flag_1000_lockout_ticks);
    COUNT_DOWN(self->weapon_selector_lockout_ticks);
    COUNT_DOWN(self->item_use_lockout_ticks);
    if (self->listed_item_ticks != 0 && self->state_group == 5)
        self->listed_item_ticks--;
    else
        use_up_listed_item(c, self);
}

/* -- The timed effects: 21 timers from +0x6A0, each with a byte at +0x5F7 and one at +0x114C ------------------------------------- */

/* Health's maximum falls back: health and its red limit with it, the bar's value too (whichever hunter this is) */
static void restore_health_max(Ctx *c, Hunter *self) {
    int16_t base = self->health_max_base;
    self->health_max = base;
    if (base < self->health) {
        self->health = base;
        OBJ(HudContext, LW(HUD_POINTER))->displayed_health = base;
    }
    if (self->health_max < self->health_red_limit)
        self->health_red_limit = self->health_max;
}

/* The message an effect's end brings, if the plain timer doing the same thing has run out too; 0 for none */
static uint32_t effect_end_message(const Hunter *self, int entry) {
    switch (entry) {
    case EFFECT_AREA_1_2_WARD:
        return self->area_1_2_ward_ticks == 0 ? MESSAGE_AREA_1_2_WARD_ENDS : 0;
    case EFFECT_AREA_3_4_WARD:
        return self->area_3_4_ward_ticks == 0 ? MESSAGE_AREA_3_4_WARD_ENDS : 0;
    case EFFECT_REGEN:
        return self->health_regen_boost_ticks == 0 ? MESSAGE_REGEN_BOOST_ENDS : 0;
    case EFFECT_STAMINA_BLOCK:
        return self->stamina_loss_block_ticks == 0 ? MESSAGE_STAMINA_BLOCK_ENDS : 0;
    default:
        return 0;
    }
}

static void timed_effects(Ctx *c, Hunter *self) {
    /* These arrays' entries have names of their own where they are known; the arrays are reached from their first entries */
    int16_t *ticks = &self->timed_effect_ticks;
    uint8_t *levels = &self->timed_effect_0_level;
    uint8_t *paired = &self->timed_effect_0_paired_byte;
    for (int entry = 0; entry < EFFECT_COUNT; entry++) {
        if (ticks[entry] <= 0)
            continue;
        ticks[entry]--;
        if (ticks[entry] > 0)
            continue;
        levels[entry] = 0;
        ticks[entry] = 0;
        paired[entry] = 0;
        if (entry == EFFECT_HEALTH_MAX) {
            restore_health_max(c, self);
        } else if (shows_messages(c, self)) {
            uint32_t message = effect_end_message(self, entry);
            if (message != 0)
                hud_message(c, 0, message, 0);
        }
    }
    call1(c, hunter_rebuild_flags_6cc_from_short_array_6a0, GAME_ADDR(self));
}

/* game_task 09A68FC8 */
void hand_hunter_tick_upkeep(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    count_down_timers(c, self);
    weapon_class_timers(c, self);
    stamina_upkeep(c, self);
    health_upkeep(c, self);
    lockouts_and_listed_item(c, self);
    timed_effects(c, self);
    COUNT_DOWN(self->ticks_2e6);
}

/* EBOOT 088D6488: the timers the upkeep of any hunter counts down, the lobby's too. To 0 and no further, from either sign. */
void hand_hunter_tick_countdowns(Ctx *c) {
    HunterBase *self = OBJ(HunterBase, c->r[4]);
    if (self->refusal_message_cooldown != 0)
        self->refusal_message_cooldown--;
    if (self->sound_cue_mute_timer != 0)
        self->sound_cue_mute_timer--;
    if (self->state_hold_timer != 0)
        self->state_hold_timer--;
    if (self->ticks_5f0 != 0)
        self->ticks_5f0--;
}
