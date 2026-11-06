/* What happens to a hunter that has been hit: the damage it takes off health, whether it guards, and how it reacts. Rewritten by hand from
 * EBOOT 088D6594 and game_task 09A6FB30, 09A6C968 and 09A6B130, the generated code still behind them (MHFU_HAND=0).
 *
 * The damage arrives worked out already: hit_damage.c puts it in pending_damage_0 (attack, defense and resistance), and the amounts of
 * buildups in pending_buildup_*, and then this runs in the hunter's tick (hunter_run.c). It is here that a guard, or a skill's chance, changes
 * the damage, and that buildups become statuses.
 *
 * Each thing called may change what's read next, so fields are read where the original reads them. */
#include "hand.h"
#include "names.h"

/* Named here by address as function names may change: */
#define active_skill f_EBOOT_088D6A1C
#define hunt_bonus f_EBOOT_088D6A54
#define timed_effect_test f_EBOOT_088DAE98
#define mode_allows_index f_EBOOT_0884FA5C
#define sword_guards f_EBOOT_088D2830               /* weapon class 1, only some of them: 088D2830 */
#define state_flags_test f_EBOOT_08865C88
#define state_flags_set f_EBOOT_08865A64
#define state_flags_clear f_EBOOT_08865A9C
#define angle_within_guard f_game_task_09A6CB50
#define state_is f_EBOOT_08865CB8
#define rng_next f_EBOOT_088900C4
#define bone_position f_EBOOT_08865F24
#define object_sound_mask f_EBOOT_08866284
#define object_sound f_EBOOT_08865F8C
#define sound_pick f_EBOOT_0888553C
#define sound_restore_volume f_EBOOT_08885E9C
#define in_state_for_special_hit f_EBOOT_088DACA4
#define special_hit_cancel f_game_task_09AAD1F0
#define clear_slow_and_freeze f_game_task_09A6C958
#define after_guard_hit f_game_task_09A6CB90
#define start_faint f_game_task_09A6CF38
#define rebuild_timed_effect_mask f_game_task_09A6E3F0
#define add_drain_seconds f_game_task_09A6D0D8
#define stamina_max_add f_game_task_09A6A738
#define state_in_27_to_2b f_EBOOT_088DA324
#define exempt_from_light_reaction f_game_task_09A6D2B8
#define end_mash_status f_game_task_09A6D1F0
#define in_state_2_16_or_2_18 f_game_task_09A6D228
#define reaction_blocked f_game_task_09A6D280            /* reaction_block_weapon_ticks, _item_ticks or misc_flags 0x10 */
#define mash_status_running f_game_task_09AA8958

#define STATE_GROUP_FAINT 3
#define STATE_GROUP_END 4
#define VSLOT_ADD_HEALTH 0x68
#define VSLOT_SET_STATE 0x6C
#define VSLOT_CAN_GUARD 0x7C

#define RNG_POINTER 0x08A632C0u
#define SOUND_2D_POINTER 0x08A62EACu
#define SOUND_STATE_POINTER 0x08A62EB0u

/* game_sub, an overlay not resident at link time: called as the game does */
#define SET_GROUND_POINT_EFFECT 0x09CB1D58u
#define BONE_EFFECT 0x09C60DA0u
#define PLACE_EFFECT 0x09C7BCA8u
#define OWN_BANK_SOUND 0x09C557E0u
#define OWN_BANK_SOUND_DISPATCH 0x09C558E8u
#define HIT_KIND_IN_16_19 0x09C451E0u

static int is_local(Ctx *c, const Hunter *self) {
    return self->instance_index == GAME()->local_hunter_slot;
}

static uint32_t skill(Ctx *c, const Hunter *self, uint32_t id) {
    return call2(c, active_skill, GAME_ADDR(self), id);
}

static int has_skill(Ctx *c, const Hunter *self, uint32_t id) {
    return (uint8_t)skill(c, self, id) == 1;
}

/* game_task 09A6FB30: a slot of the hunter's vtable (+0x2C): an object's hit fields cleared, then the hunter's own, ready for the next hit. */
void hand_hunter_clear_received_hit_fields(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    f_EBOOT_08865EC0(c);
    self->pending_buildup_mash = 0;
    self->pending_item_restrict = 0;
    self->pending_defense_halve = 0;
    self->pending_stamina_debuff = 0;
    self->pending_drain_seconds = 0;
}

/* -- health ------------------------------------------------------------------------------------------------------------------------ */

#define SKILL_SURVIVE_ONE_A 0xC8    /* survive a killing blow, at 1 health, from this much or more */
#define SKILL_SURVIVE_ONE_B 0xD3
#define SURVIVE_ONE_FROM_HEALTH 64
#define SURVIVE_ONE_EFFECT 0x11

/* EBOOT 088D6594: adds to health (a1, a negative amount is damage), within 0 and the maximum, the red part following it up. Only for the
 * local player's hunter. Returns 1 if a skill kept it at 1 health, which the caller goes by. */
void hand_hunter_add_health(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t amount = c->r[5];
    c->r[2] = 0;
    if (!is_local(c, self))
        return;
    /* The game's mode can rule out damage */
    if ((uint8_t)call2(c, mode_allows_index, LW(0x089CC558u), 0) == 1 && (int16_t)amount < 0) {
        c->r[2] = 0;
        return;
    }
    if (self->state_group == STATE_GROUP_FAINT) {
        c->r[2] = 0;
        return;
    }
    if (has_skill(c, self, SKILL_SURVIVE_ONE_A) || has_skill(c, self, SKILL_SURVIVE_ONE_B)) {
        int32_t health = self->health;
        if (health >= SURVIVE_ONE_FROM_HEALTH && health + (int16_t)amount <= 0) {
            self->health = 1;
            self->health_red_limit = 1;
            c->f[12].f = 1.0f;
            call2_at(c, SET_GROUND_POINT_EFFECT, GAME_ADDR(self), SURVIVE_ONE_EFFECT);
            c->r[2] = 1;
            return;
        }
    }
    self->health = (int16_t)(self->health + amount);
    if (self->health <= 0)
        self->health = 0;
    if (self->health >= self->health_max)
        self->health = self->health_max;
    if (self->health >= self->health_red_limit)
        self->health_red_limit = self->health;
    c->r[2] = 0;
}

/* -- guarding ---------------------------------------------------------------------------------------------------------------------- */

#define GUARD_MIN_STAMINA 75
#define HIT_KIND_UNGUARDABLE_A 0xE
#define HIT_KIND_UNGUARDABLE_B 0xF
#define HIT_FLAGS_NEED_SKILL 0xC0
#define SKILL_GUARD_SPECIAL_HITS 0x2C
#define SKILL_GUARD_ANY_DIRECTION 0x2D
#define MISC_FLAG_WEAPON_OUT 8
#define HUNTER_FLAG_GUARD_ANY_DIRECTION 0x20
#define STATE_FLAG_GUARD_DIRECTED 0x8000

/* Weapon classes that can guard: only what the code does with them */
static int weapon_class_can_guard(Ctx *c, Hunter *self) {
    switch (self->weapon_class) {
    case 1:
        return (uint8_t)call1(c, sword_guards, GAME_ADDR(self)) == 1;
    case 0:
    case 3:
    case 4:
    case 9:
        return 1;
    default:
        return 0;
    }
}

static int can_guard(Ctx *c, Hunter *self) {
    if (self->pending_damage_0 < 0)         /* it heals */
        return 0;
    if (self->received_hit_type == HIT_KIND_UNGUARDABLE_A || self->received_hit_type == HIT_KIND_UNGUARDABLE_B)
        return 0;
    if (self->stamina < GUARD_MIN_STAMINA)
        return 0;
    if ((self->pending_hit_flags & HIT_FLAGS_NEED_SKILL) && (uint8_t)skill(c, self, SKILL_GUARD_SPECIAL_HITS) == 0)
        return 0;
    if (!weapon_class_can_guard(c, self))
        return 0;
    if (!(self->misc_flags & MISC_FLAG_WEAPON_OUT))
        return 0;
    if (self->movement_mode >= 2)
        return 0;
    /* The weapon's guard faces the way it faces, with this skill or this one's flag */
    int any_direction = 0;
    if (has_skill(c, self, SKILL_GUARD_ANY_DIRECTION) || (uint8_t)call1(c, sword_guards, GAME_ADDR(self)) == 1)
        any_direction = (self->hunter_flags & HUNTER_FLAG_GUARD_ANY_DIRECTION) != 0;
    if (call2(c, state_flags_test, GAME_ADDR(self), STATE_FLAG_GUARD_DIRECTED) == 0 && !any_direction)
        return 0;
    return (uint8_t)call3(c, angle_within_guard, GAME_ADDR(self), (uint16_t)self->facing, self->received_hit_angle) == 1;
}

/* game_task 09A6C968: a method of the hunter's (slot 0x7C): can it guard the hit it is taking? */
void hand_hunter_can_guard_received_hit(Ctx *c) {
    c->r[2] = (uint32_t)can_guard(c, OBJ(Hunter, c->r[4]));
}

/* -- a hit: the frame ---------------------------------------------------------------------------------------------------------------- */

/* The stack frame: the sounds below take their fifth argument at sp+0 of it, and a place at FRAME_BONE */
#define FRAME 0x40
#define FRAME_BONE 0x30

#define HIT_KIND_ALWAYS_TAKEN 0xD       /* the only kind a fainting hunter takes, and it doesn't make one at 0 health faint */
#define BONE_FOR_EFFECTS 0xA

/* -- a guarded hit --------------------------------------------------------------------------------------------------------------------- */

#define STATE_GUARD_BROKEN 0xA          /* with group 2: in this state a guard doesn't save health */
#define HIT_FLAGS_THROUGH_GUARD 0xC0    /* nor from these: some of the damage gets through */
#define WEAPON_CLASS_GUARD_BETTER_LOSS 1 /* half of it, others a quarter */

#define SKILL_NO_STATUS_ON_GUARD 0x2C   /* the same as lets it guard those hits */
#define SKILL_NO_ITEM_RESTRICT 0xD
#define ITEM_RESTRICT_TICKS 1800
#define DEFENSE_HALVED_TICKS 5400

static void red_limit_at_half_of_loss(Hunter *self, int32_t health_before) {
    int32_t health = self->health;
    self->health_red_limit = (int16_t)(health + (health_before - health) / 2);
}

static void restrict_items_unless_skill(Ctx *c, Hunter *self) {
    if ((uint8_t)skill(c, self, SKILL_NO_ITEM_RESTRICT) == 0)
        self->item_restrict_ticks = ITEM_RESTRICT_TICKS;
}

/* Defense halved a while (hunter_defense.c), and whatever raised it gone */
static void halve_defense(Ctx *c, Hunter *self) {
    self->defense_halved_ticks = DEFENSE_HALVED_TICKS;
    self->defense_bonus = 0;
    self->defense_bonus_timed = 0;
    self->defense_bonus_ticks = 0;
    self->defense_effect_level = 0;
    self->defense_effect_ticks = 0;
    call1(c, rebuild_timed_effect_mask, GAME_ADDR(self));
}

/* Returns 1 if it fainted of it: then nothing else happens */
static int take_guarded_hit(Ctx *c, Hunter *self) {
    call1(c, after_guard_hit, GAME_ADDR(self));
    int32_t health_before = self->health;
    if ((uint8_t)call3(c, state_is, GAME_ADDR(self), 2, STATE_GUARD_BROKEN) != 0 || (self->pending_hit_flags & HIT_FLAGS_THROUGH_GUARD)) {
        int32_t lost = self->weapon_class == WEAPON_CLASS_GUARD_BETTER_LOSS ? self->pending_damage_0 / 2 : self->pending_damage_0 / 4;
        if ((uint8_t)call_method1(c, GAME_ADDR(self), VSLOT_ADD_HEALTH, (uint32_t)(int32_t)(int16_t)-lost) == 1)
            health_before = 1;
        red_limit_at_half_of_loss(self, health_before);
        if (self->health <= 0) {
            call1(c, start_faint, GAME_ADDR(self));
            return 1;
        }
    }
    if ((uint8_t)skill(c, self, SKILL_NO_STATUS_ON_GUARD) != 0)
        return 0;
    if (self->pending_item_restrict > 0)
        restrict_items_unless_skill(c, self);
    if (self->pending_defense_halve > 0)
        halve_defense(c, self);
    return 0;
}

/* -- the damage: chances that change it, then health ------------------------------------------------------------------------------- */

#define SOFTENED 0.7f
#define HARDENED 1.3f
#define EFFECT_MASK_SOFTEN_HALF_THE_TIME 0x40
#define SKILL_SOFTEN_ONE_IN_FOUR 0x58
#define SKILL_HARDEN_ONE_IN_FOUR 0x59
#define BONUS_SOFTEN_ONE_IN_FOUR 0xA
#define BONUS_SOFTEN_ONE_IN_EIGHT 0xB
#define SOUND_SOFTENED 0x72
#define SOUND_HARDENED 0x73

static uint32_t chance(Ctx *c) {
    return call2(c, rng_next, LW(RNG_POINTER), 1) & 0xFFFF;
}

/* What shows that a hit was softened */
static void softened_effect_and_sound(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self), sp = c->r[29];
    c->f[12].f = 4.0f;
    c->r[8] = 0;
    call4_at(c, BONE_EFFECT, self_addr, 2, 0x19, 0xA);
    call3(c, bone_position, self_addr, sp + FRAME_BONE, BONE_FOR_EFFECTS);
    uint32_t mask = call1(c, object_sound_mask, self_addr);
    c->r[8] = 0;
    c->r[9] = sp + FRAME_BONE;
    c->r[10] = 1;
    c->r[11] = 0;
    SW(sp, 0);
    call4(c, object_sound, self_addr, SOUND_SOFTENED, 0, mask);
}

static void soften(Hunter *self) {
    self->pending_damage_0 = (int16_t)rt_trunc_w(SOFTENED * (float)self->pending_damage_0);
}

/* A hit that does damage still does some */
static void at_least_1(Hunter *self) {
    if (self->pending_damage_0 == 0)
        self->pending_damage_0 = 1;
}

static void harden(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self), sp = c->r[29];
    self->pending_damage_0 = (int16_t)rt_trunc_w(HARDENED * (float)self->pending_damage_0);
    call3(c, bone_position, self_addr, sp + FRAME_BONE, BONE_FOR_EFFECTS);
    c->f[12].f = 1.0f;
    c->r[8] = 1;
    c->r[9] = 1;
    call4_at(c, PLACE_EFFECT, self_addr, self_addr + 0x200, 5, 3);
    uint32_t mask = call1(c, object_sound_mask, self_addr);
    c->r[8] = 0;
    c->r[9] = sp + FRAME_BONE;
    c->r[10] = 1;
    c->r[11] = 0;
    SW(sp, 0);
    call4(c, object_sound, self_addr, SOUND_HARDENED, 0, mask);
}

/* The chances, on a hit that does damage, of its being less or more. A hunter already at 0 health takes the first two as they are. */
static void chances_on_damage(Ctx *c, Hunter *self, int32_t health_before) {
    int softened = 0;
    if ((uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_SOFTEN_HALF_THE_TIME) == 1) {
        if (health_before > 0 && (chance(c) & 1)) {
            soften(self);
            softened_effect_and_sound(c, self);
            at_least_1(self);
            softened = 1;
        }
    } else if (health_before > 0 && (chance(c) & 3) == 0) {
        if (has_skill(c, self, SKILL_SOFTEN_ONE_IN_FOUR)) {
            soften(self);
            softened_effect_and_sound(c, self);
            at_least_1(self);
            softened = 1;
        } else if (has_skill(c, self, SKILL_HARDEN_ONE_IN_FOUR)) {
            harden(c, self);
        }
    }
    /* And another chance from a bonus of the hunt's, which may soften it again: the effect is shown only once */
    int32_t odds = 0;
    if (call2(c, hunt_bonus, GAME_ADDR(self), BONUS_SOFTEN_ONE_IN_FOUR) != 0)
        odds = 4;
    else if (call2(c, hunt_bonus, GAME_ADDR(self), BONUS_SOFTEN_ONE_IN_EIGHT) != 0)
        odds = 8;
    if (odds > 0 && (chance(c) & (uint32_t)(odds - 1)) == 0) {
        soften(self);
        at_least_1(self);
        if (!softened)
            softened_effect_and_sound(c, self);
    }
}

/* Health loses the damage; the red part of the gauge falls by half of it, or rises with health */
static void take_damage(Ctx *c, Hunter *self) {
    int16_t damage = self->pending_damage_0;
    int32_t health_before = self->health;
    if (damage > 0)
        chances_on_damage(c, self, health_before);
    if ((uint8_t)call_method1(c, GAME_ADDR(self), VSLOT_ADD_HEALTH, (uint32_t)(int32_t)(int16_t)-self->pending_damage_0) == 1)
        health_before = 1;
    if (self->health < health_before)
        red_limit_at_half_of_loss(self, health_before);
    else if (self->health_red_limit < self->health)
        self->health_red_limit = self->health;
}

/* -- buildups: what a hit carried besides damage, into the meters that become statuses ----------------------------------------------- */

#define SKILL_NO_BUILDUPS 0xD0          /* all of the first three */
#define BUILDUP_2_13_FULL 50              /* buildup_state_2_13 from here on: state 2, 0x13 */
#define BUILDUP_2_13_DECAY_DELAY_TICKS 150
#define SKILL_NO_BUILDUP_2_13 8
#define EFFECT_MASK_NO_BUILDUP_2_13 0x400
#define SKILL_CLEAR_FULL_2_13_HALF_THE_TIME 7
#define SKILL_NO_BUILDUP_2_15 5
#define SKILL_NO_BUILDUP_2_16 2
#define EFFECT_MASK_NO_BUILDUP_2_16 0x800
#define SKILL_NO_STAMINA_DEBUFF_A 0xBB
#define SKILL_NO_STAMINA_DEBUFF_B 0xD6
#define STAMINA_DEBUFF_TICKS 1800
#define STAMINA_DEBUFF_MAX_LOSS -375
#define SKILL_NO_MASH_BUILDUP_A 0xE
#define SKILL_NO_MASH_BUILDUP_B 0xD5
#define EFFECT_MASK_NO_MASH_BUILDUP 0x80
#define STATE_FLAG_AFTER_BUILDUPS_SET 2
#define STATE_FLAG_AFTER_BUILDUPS_CLEAR 0x8000

static void take_buildups(Ctx *c, Hunter *self) {
    /* The hit's direct amount */
    if (self->pending_buildup_direct > 0) {
        if (has_skill(c, self, SKILL_NO_BUILDUP_2_13)) {
            self->pending_buildup_direct = 0;
        } else if (has_skill(c, self, SKILL_NO_BUILDUPS) || (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_NO_BUILDUP_2_13) == 1) {
            self->pending_buildup_direct = 0;
        } else {
            self->buildup_state_2_13 += self->pending_buildup_direct;
            self->buildup_decay_delay_ticks = BUILDUP_2_13_DECAY_DELAY_TICKS;
            if (has_skill(c, self, SKILL_CLEAR_FULL_2_13_HALF_THE_TIME) && self->buildup_state_2_13 >= BUILDUP_2_13_FULL && (chance(c) & 1)) {
                self->buildup_state_2_13 = 0;
                self->pending_buildup_direct = 0;
                self->buildup_decay_delay_ticks = 0;
            }
        }
    }
    /* From the hit's mask 1 */
    if (self->pending_buildup_mask1 > 0) {
        if (has_skill(c, self, SKILL_NO_BUILDUP_2_15) || has_skill(c, self, SKILL_NO_BUILDUPS))
            self->pending_buildup_mask1 = 0;
        else
            self->buildup_state_2_15 += self->pending_buildup_mask1;
    }
    /* From its mask 4 */
    if (self->pending_buildup_mask4 > 0) {
        if (has_skill(c, self, SKILL_NO_BUILDUP_2_16) || has_skill(c, self, SKILL_NO_BUILDUPS)
            || (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_NO_BUILDUP_2_16) == 1)
            self->pending_buildup_mask4 = 0;
        else
            self->buildup_state_2_16 += self->pending_buildup_mask4;
    }
    /* From its mask 2: the drain */
    if (self->pending_buildup_mask2 > 0 || self->pending_drain_seconds > 0)
        call3(c, add_drain_seconds, GAME_ADDR(self), (uint32_t)(int32_t)self->pending_buildup_mask2, (uint32_t)(int32_t)self->pending_drain_seconds);
    if (self->pending_item_restrict > 0)
        restrict_items_unless_skill(c, self);
    if (self->pending_stamina_debuff > 0) {
        if (has_skill(c, self, SKILL_NO_STAMINA_DEBUFF_A) || has_skill(c, self, SKILL_NO_STAMINA_DEBUFF_B)) {
            self->pending_stamina_debuff = 0;
            self->stamina_debuff_ticks = 0;
        } else {
            self->stamina_debuff_ticks = STAMINA_DEBUFF_TICKS;
            call2(c, stamina_max_add, GAME_ADDR(self), (uint32_t)STAMINA_DEBUFF_MAX_LOSS);
        }
    }
    if (self->pending_buildup_mash > 0) {
        if (has_skill(c, self, SKILL_NO_MASH_BUILDUP_A) || has_skill(c, self, SKILL_NO_MASH_BUILDUP_B)
            || (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_NO_MASH_BUILDUP) == 1)
            self->pending_buildup_mash = 0;
        self->buildup_mash_status += self->pending_buildup_mash;
    }
    if (self->pending_defense_halve > 0)
        halve_defense(c, self);
    call2(c, state_flags_set, GAME_ADDR(self), STATE_FLAG_AFTER_BUILDUPS_SET);
    call2(c, state_flags_clear, GAME_ADDR(self), STATE_FLAG_AFTER_BUILDUPS_CLEAR);
}

/* -- the reaction: what state a hit puts the hunter in, by the kind of hit ------------------------------------------------------------- */

/* How a reaction ends: */
typedef enum {
    SOUND_20_21,        /* one of sounds 0x20 and 0x21 */
    SOUND_22,           /* of 0x22, 4 variations */
    NO_SOUND,
    DONE,               /* none of them, and the sound state's volume is restored */
    DONE_UNMOVED,       /* it didn't react at all: not restored either */
} Reaction;

#define GROUP_REACTION 2
#define ACTION_SET_STATE_MODE 0xC       /* a3 to every state set here */
#define IN_THE_AIR 2                    /* movement_mode */
#define HEAVY_POWER 11                  /* received_hit_power from here on makes a heavier reaction */
#define REMOTE_HIT_IMMUNE_TICKS 99          /* a remote hunter's hit_immune_ticks, on kinds 2 to 4 */

/* States of group 2 this sets */
enum {
    REACT_KIND_2_FACING = 0x05, REACT_KIND_2_AWAY = 0x02, REACT_KIND_3_4_FACING = 0x0E, REACT_KIND_3_4_AWAY = 0x07,
    REACT_LIGHT_FACING = 0x0D, REACT_LIGHT_AWAY = 0x09, REACT_HEAVY_FACING = 0x06, REACT_HEAVY_AWAY = 0x00,
    REACT_BUILDUP_2_13_FULL = 0x13, REACT_IN_MASH_STATUS = 0x1B, REACT_KIND_31 = 0x27,
    REACT_WHILE_2_16_2_18 = 0x18,
    REACT_KIND_5 = 0x0F, REACT_KIND_6 = 0x1A, REACT_KIND_7 = 0x04,
    REACT_KIND_19 = 0x25, REACT_KIND_20 = 0x1D,
    REACT_BEFORE_KINDS_5_6 = 0x00,
    REACT_KINDS_22_26 = 0x19, REACT_KINDS_23_27 = 0x12, REACT_KINDS_24_28 = 0x11, REACT_KINDS_OTHER_22_30 = 0x10,
};

static void set_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state) {
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, ACTION_SET_STATE_MODE);
}

static void react(Ctx *c, Hunter *self, uint32_t state) {
    set_state(c, self, GROUP_REACTION, state);
}

static void face_the_hit_angle(Hunter *self) {
    uint32_t angle = self->received_hit_angle;
    self->facing = angle;
    self->facing_target = (uint16_t)angle;
}

static void face_away_from_hit_angle(Hunter *self) {
    uint32_t angle = (uint16_t)(self->received_hit_angle + 0x8000);
    self->facing = angle;
    self->facing_target = (uint16_t)angle;
}

static int buildup_2_13_full(const Hunter *self) {
    return self->buildup_state_2_13 >= BUILDUP_2_13_FULL;
}

static int exempt_from_light_hits(Ctx *c, Hunter *self) {
    return (uint8_t)call1(c, exempt_from_light_reaction, GAME_ADDR(self));
}

static void fly_off(Hunter *self) {
    self->movement_mode = IN_THE_AIR;
    self->attached_part_mode = 0;
}

/* Kinds 3 and 4, and 2 unless exempt: into the air, facing the hit's angle or the other way, as it faced before */
static Reaction flying_reaction(Ctx *c, Hunter *self, uint32_t kind, int facing_hit) {
    if (facing_hit) {
        face_the_hit_angle(self);
        react(c, self, kind == 2 ? REACT_KIND_2_FACING : REACT_KIND_3_4_FACING);
    } else {
        face_away_from_hit_angle(self);
        react(c, self, kind == 2 ? REACT_KIND_2_AWAY : REACT_KIND_3_4_AWAY);
    }
    call1(c, end_mash_status, GAME_ADDR(self));
    fly_off(self);
    return SOUND_22;
}

/* Exempt if light, not at 0 health, and the buildup not full */
static Reaction kind_2(Ctx *c, Hunter *self, int16_t health, uint32_t kind, int facing_hit) {
    if (health > 0 && !buildup_2_13_full(self) && (uint8_t)call2(c, state_in_27_to_2b, GAME_ADDR(self), GAME_ADDR(self)) == 1
        && self->received_hit_power < HEAVY_POWER && (uint8_t)exempt_from_light_hits(c, self) == 1) {
        call1(c, end_mash_status, GAME_ADDR(self));
        fly_off(self);
        return NO_SOUND;
    }
    return flying_reaction(c, self, kind, facing_hit);
}

static Reaction kind_31(Ctx *c, Hunter *self) {
    if (self->state_group == STATE_GROUP_FAINT || self->state_group == STATE_GROUP_END)
        return DONE_UNMOVED;
    react(c, self, REACT_KIND_31);
    call1(c, end_mash_status, GAME_ADDR(self));
    fly_off(self);
    return SOUND_22;
}

/* While reactions are blocked, only a full buildup reacts, or a mash status running */
static Reaction while_reaction_blocked(Ctx *c, Hunter *self) {
    if (buildup_2_13_full(self) && is_local(c, self)) {
        react(c, self, REACT_BUILDUP_2_13_FULL);
        return DONE;
    }
    if ((uint8_t)call1(c, mash_status_running, GAME_ADDR(self)) == 0)
        return DONE_UNMOVED;
    if (is_local(c, self)) {
        react(c, self, REACT_IN_MASH_STATUS);
        return DONE;
    }
    return DONE_UNMOVED;
}

/* Kinds 5, 6 and 7: a skill or a level of timed effect can make the hunter immune */
static int immune_to_kinds_5_6_7(Ctx *c, Hunter *self, uint32_t kind) {
    uint32_t level = (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), 0x10) == 1 ? self->hit_type_5_6_7_immunity_level : 0;
    if (kind == 7)
        return has_skill(c, self, 0x84) || has_skill(c, self, 0xD5) || level >= 3;
    if (has_skill(c, self, 0x83) || has_skill(c, self, 0x84) || has_skill(c, self, 0xD5) || level >= 2)
        return 1;
    return (has_skill(c, self, 0x82) || level > 0) && kind == 6;
}

/* Whether its state keeps it from reacting. Group 0 and 1: some actions do; others, with the weapon out or those flags, are ended first. */
static int state_stops_kinds_5_6(Ctx *c, Hunter *self, uint32_t kind) {
    uint8_t group = self->state_group;
    if (group == STATE_GROUP_FAINT || group == STATE_GROUP_END)
        return 1;
    if (group == 0 || group == 1) {
        if (kind == 7)
            return 0;
        if (self->movement_mode == 1)
            return 1;
        if ((self->weapon_class == 2 || self->weapon_class == 8) && self->charge_time != 0)
            return 1;
        if (call2(c, state_flags_test, GAME_ADDR(self), 0x2000600) != 0 || (self->misc_flags & MISC_FLAG_WEAPON_OUT))
            react(c, self, REACT_BEFORE_KINDS_5_6);
        return 0;
    }
    if (group == GROUP_REACTION) {
        switch (self->state) {
        case 0x1A: case 0xF: case 0xE: case 7: case 6: case 5: case 4: case 2: case 1: case 0:
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

static Reaction kinds_5_6_7(Ctx *c, Hunter *self, uint32_t kind) {
    if ((uint8_t)call1(c, in_state_2_16_or_2_18, GAME_ADDR(self)) == 1) {
        react(c, self, REACT_WHILE_2_16_2_18);
        return SOUND_20_21;
    }
    if ((uint8_t)call1(c, reaction_blocked, GAME_ADDR(self)) == 1 && kind != 7)
        return while_reaction_blocked(c, self);
    if (immune_to_kinds_5_6_7(c, self, kind))
        return DONE_UNMOVED;
    if (state_stops_kinds_5_6(c, self, kind))
        return DONE_UNMOVED;
    face_away_from_hit_angle(self);
    call1(c, end_mash_status, GAME_ADDR(self));
    react(c, self, kind == 7 ? REACT_KIND_7 : kind == 6 ? REACT_KIND_6 : REACT_KIND_5);
    return SOUND_20_21;
}

static Reaction kinds_19_20(Ctx *c, Hunter *self, uint32_t kind) {
    if ((uint8_t)call1(c, in_state_2_16_or_2_18, GAME_ADDR(self)) == 1) {
        react(c, self, REACT_WHILE_2_16_2_18);
        return SOUND_20_21;
    }
    if (has_skill(c, self, 0xB5) || (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), 0x1000) == 1)
        return DONE_UNMOVED;
    call1(c, end_mash_status, GAME_ADDR(self));
    react(c, self, kind == 19 ? REACT_KIND_19 : REACT_KIND_20);
    return SOUND_20_21;
}

#define EFFECT_MASK_IMMUNE_22_30 0x200

static Reaction kinds_22_to_30(Ctx *c, Hunter *self, uint32_t kind) {
    if ((uint8_t)call1(c, in_state_2_16_or_2_18, GAME_ADDR(self)) == 1) {
        react(c, self, REACT_WHILE_2_16_2_18);
        return SOUND_20_21;
    }
    if (has_skill(c, self, 0x5B))
        return DONE_UNMOVED;
    if ((uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_IMMUNE_22_30) == 1 && self->hit_type_16_1e_immunity_level >= 2)
        return DONE_UNMOVED;
    /* Of these, those from 0x16 to 0x19 alone with a lesser skill or effect */
    if ((has_skill(c, self, 0x5A) || (uint8_t)call2(c, timed_effect_test, GAME_ADDR(self), EFFECT_MASK_IMMUNE_22_30) == 1)
        && (uint8_t)call1_at(c, HIT_KIND_IN_16_19, kind & 0xFF) == 1)
        return DONE_UNMOVED;
    face_away_from_hit_angle(self);
    call1(c, end_mash_status, GAME_ADDR(self));
    if (kind == 0x16 || kind == 0x1A)
        react(c, self, REACT_KINDS_22_26);
    else if (kind == 0x17 || kind == 0x1B)
        react(c, self, REACT_KINDS_23_27);
    else if (kind == 0x18 || kind == 0x1C)
        react(c, self, REACT_KINDS_24_28);
    else
        react(c, self, REACT_KINDS_OTHER_22_30);
    return SOUND_20_21;
}

/* Every other kind: by the hit's power and which way the hunter faces it */
static Reaction ordinary_hit(Ctx *c, Hunter *self, int facing_hit) {
    if ((uint8_t)call1(c, in_state_2_16_or_2_18, GAME_ADDR(self)) == 1) {
        react(c, self, REACT_WHILE_2_16_2_18);
        return SOUND_20_21;
    }
    if ((uint8_t)call1(c, reaction_blocked, GAME_ADDR(self)) == 1)
        return while_reaction_blocked(c, self);
    call1(c, end_mash_status, GAME_ADDR(self));
    Reaction reaction = SOUND_20_21;
    if (self->received_hit_power < HEAVY_POWER) {
        if ((uint8_t)exempt_from_light_hits(c, self) != 0) {
            reaction = NO_SOUND;
            if (buildup_2_13_full(self))
                react(c, self, REACT_BUILDUP_2_13_FULL);
        } else if (facing_hit) {
            face_the_hit_angle(self);
            react(c, self, REACT_LIGHT_FACING);
        } else {
            face_away_from_hit_angle(self);
            react(c, self, REACT_LIGHT_AWAY);
        }
    } else if (facing_hit) {
        face_the_hit_angle(self);
        react(c, self, REACT_HEAVY_FACING);
    } else {
        face_away_from_hit_angle(self);
        react(c, self, REACT_HEAVY_AWAY);
    }
    self->movement_mode = 0;
    return reaction;
}

static Reaction reaction_of_kind(Ctx *c, Hunter *self, uint32_t kind, int16_t health, int facing_hit) {
    switch (kind) {
    case 2:
        return kind_2(c, self, health, kind, facing_hit);
    case 3:
    case 4:
        return flying_reaction(c, self, kind, facing_hit);
    case 5:
    case 6:
    case 7:
        return kinds_5_6_7(c, self, kind);
    case 11:
    case 14:
    case 15:
    case 16:
    case 21:
        return DONE_UNMOVED;
    case 13:
        set_state(c, self, 4, 4);
        return DONE_UNMOVED;
    case 19:
    case 20:
        return kinds_19_20(c, self, kind);
    case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
        return kinds_22_to_30(c, self, kind);
    case 31:
        return kind_31(c, self);
    default:
        return ordinary_hit(c, self, facing_hit);
    }
}

/* A remote hunter's own machine decides its reaction: here only this, on kinds 2, 3 and 4 */
static void remote_reaction(Ctx *c, Hunter *self, uint32_t kind) {
    if (kind == 2) {
        if ((uint8_t)call2(c, state_in_27_to_2b, GAME_ADDR(self), GAME_ADDR(self)) == 1 && self->received_hit_power < HEAVY_POWER
            && (uint8_t)exempt_from_light_hits(c, self) == 1)
            return;
    } else if (kind != 3 && kind != 4) {
        return;
    }
    self->hit_immune_ticks = REMOTE_HIT_IMMUNE_TICKS;
}

static void reaction_sound(Ctx *c, Hunter *self, Reaction reaction) {
    uint32_t self_addr = GAME_ADDR(self);
    uint32_t sound;
    if (reaction == SOUND_20_21) {
        c->r[8] = 2;
        sound = call4(c, sound_pick, LW(SOUND_2D_POINTER), 0x20, 2, 0x21);
    } else if (reaction == SOUND_22) {
        c->r[8] = 0;
        sound = call4(c, sound_pick, LW(SOUND_2D_POINTER), 0x22, 4, 0x22);
    } else {
        return;
    }
    c->r[8] = 1;
    c->r[9] = 0;
    call4_at(c, OWN_BANK_SOUND, self_addr, sound, 0, self_addr + 0x200);
    call1_at(c, OWN_BANK_SOUND_DISPATCH, self_addr);
}

/* What kind of hit it counts as: at 0 health, or in the air, most are something simpler */
static uint32_t kind_of_hit(const Hunter *self, int16_t health) {
    uint32_t kind = self->received_hit_type;
    if (health <= 0 && kind != HIT_KIND_ALWAYS_TAKEN)
        kind = 2;
    if (self->movement_mode == IN_THE_AIR) {
        switch (kind) {
        case 4: case 0x1F: case 0x15: case 0x10: case 0xF: case 0xE: case 0xB:
            break;
        default:
            kind = 2;
            break;
        }
    }
    return kind;
}

/* At 0 health it faints, unless the kind says otherwise or it's in the air */
static Reaction react_to_hit(Ctx *c, Hunter *self, int facing_hit) {
    int16_t health = self->health;
    if (health <= 0 && self->received_hit_type != HIT_KIND_ALWAYS_TAKEN && self->movement_mode != IN_THE_AIR) {
        call1(c, start_faint, GAME_ADDR(self));
        return DONE;
    }
    uint32_t kind = kind_of_hit(self, health);
    if (!is_local(c, self)) {
        remote_reaction(c, self, kind);
        return DONE;
    }
    return reaction_of_kind(c, self, kind, health, facing_hit);
}

/* -- the whole of a hit ----------------------------------------------------------------------------------------------------------------- */

#define GAME_FLAGS_WORD 0xF7A57Cu       /* of what RNG_POINTER points at */

/* game_task 09A6B130, in the hunter's tick: the hit that hit_damage.c left pending */
void hand_hunter_process_received_hit(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = GAME_ADDR(self);
    if (self->hit_pending == 0)
        return;
    if (self->state_group == STATE_GROUP_FAINT && self->received_hit_type != HIT_KIND_ALWAYS_TAKEN)
        return;
    c->r[29] -= FRAME;
    if ((LW(LW(RNG_POINTER) + GAME_FLAGS_WORD) & 1) && is_local(c, self) && (uint8_t)call1(c, in_state_for_special_hit, self_addr) == 1)
        call1(c, special_hit_cancel, self_addr);
    call1(c, clear_slow_and_freeze, self_addr);
    Reaction reaction = DONE;
    if ((uint8_t)call_method0(c, self_addr, VSLOT_CAN_GUARD) != 0) {
        if (is_local(c, self) && take_guarded_hit(c, self)) {
            c->r[29] += FRAME;
            return;
        }
    } else {
        int facing_hit = (uint16_t)((uint16_t)(self->facing - self->received_hit_angle) + 0x4000) < 0x8000;
        if (is_local(c, self)) {
            if (self->pending_damage_0 != 0)
                take_damage(c, self);
            take_buildups(c, self);
        }
        reaction = react_to_hit(c, self, facing_hit);
        reaction_sound(c, self, reaction);
    }
    if (reaction != DONE_UNMOVED && self->instance_index == GAME()->local_hunter_slot)
        call1(c, sound_restore_volume, LW(SOUND_STATE_POINTER));
    c->r[29] += FRAME;
}
