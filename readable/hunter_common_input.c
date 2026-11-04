/* What input does in the states that let it: game_task 09AA5270, rewritten by hand, the generated code still behind it (MHFU_HAND=0). Its
 * a1 says how it was called (states pass which variant they are), its a2 says the quest's end doesn't matter here. Whichever state it sets,
 * it goes on to look at what else was pressed: later calls can overrule earlier ones.
 *
 * States are named for the functions group 0's dispatch runs for them (hunter_state.c) where those say; others are only their numbers. */
#include "hand.h"
#include "names.h"

#define VSLOT_SET_STATE 0x6C
#define RNG_POINTER 0x08A632C0u
#define CAMERA_POINTER 0x089CBD04u
#define GAME_FLAG 0xF7A57Cu                 /* of what RNG_POINTER points at: the same flag as the frame's wait's (patches.py GATES) */

#define MISC_FLAG_WEAPON_OUT 8              /* square sheathes while it is up, dashing needs it down (hunter_input.c) */
#define HUNTER_FLAG_MAY_ACT 4               /* read elsewhere: not settled */
#define HUNTER_FLAG_8 8
#define HUNTER_FLAG_40 0x40
#define HUNTER_FLAG_SELECTOR_OPEN 1
#define HUNTER_FLAG_2000 0x2000
#define QUEST_FLAG_MONSTER_LINKED 0x100
#define BUILDUP_REACTS_AT 50
#define BONUS_1A 0x1A

#define BUTTON_SELECT 1
#define SELECTOR_INPUT_ITEM_USED 1
#define BUTTON_L 0x100
#define BUTTON_CIRCLE 0x2000
#define BUTTON_CROSS 0x4000
#define BUTTON_SQUARE 0x8000

/* The state flags asked about (object_test_flags_288) */
#define ASKED_FLAGS 0x2001600u
#define STATE_FLAGS_SHEATHE_A 0x1200u
#define STATE_FLAGS_NO_DASH 0x200u
#define STATE_FLAGS_MAY_EVADE 0x2000655u
#define STATE_FLAGS_WEAPON_INSTEAD 0x2000600u

enum {
    INPUT_NONE = 0,
    INPUT_WALK = 1,
    INPUT_RUN = 3,
    INPUT_DASH = 5,
    CALLED_AS_WALK = 1,
    CALLED_NO_EVADE = 2,
};

enum {
    ACTION_CIRCLE = 1,
    ACTION_TRIANGLE = 2,
};

#define EVADE_MIN_STICK 85
#define EVADE_MIN_STAMINA 75
#define TURN_BACK_FROM 0x6000       /* turning this far from where it faces is a turn around, up to: */
#define TURN_BACK_TO 0xA000
#define LINKED_OFF_TO_THE_SIDE 0x4000

/* States of group 0 */
enum {
    ST_MOVE_1 = 1,
    ST_MOVE_3 = 3,
    ST_BASE = 4,
    ST_SHEATHE_5 = 5,
    ST_08 = 8,
    ST_SHEATHE_A = 0xA,
    ST_MOVE_12 = 0x12,
    ST_MOVE_13 = 0x13,
    ST_EVADE = 0x1C,
    ST_DASH = 0x1F,
    ST_DASH_LINKED = 0x20,
    ST_RELEASED = 0x24,
    ST_STOPPING = 0x2C,
    ST_GROUND_FORCED = 0x3B,
    ST_3E = 0x3E,
    ST_TURN_AROUND = 0x3F,
    ST_MOVE_49 = 0x49,
    ST_67 = 0x67,
    ST_63 = 0x63,
    ST_MOVE_74 = 0x74,
};

static uint8_t quest_state(Ctx *c) { return GAME()->quest_state; }

/* The state set through the vtable */
static void set_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state, uint32_t how) {
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, how);
}

/* The state set, its flag noted */
static void set_state_noted(Ctx *c, Hunter *self, uint32_t group, uint32_t state, uint32_t how) {
    call4(c, hunter_set_state_then_set_flag_410_mask4, GAME_ADDR(self), group, state, how);
}

static int hunt_is_ending(Ctx *c) {
    return (uint8_t)call2(c, unknown_mode_byte_2e_allows_index, LW(0x089CC558u), 1) == 1;
}

static int weapon_is_out(const Hunter *self) { return (self->misc_flags & MISC_FLAG_WEAPON_OUT) != 0; }

/* Square pressed, sheathed, and not with L held */
static int item_button(const Hunter *self) {
    return !weapon_is_out(self) && (self->buttons_pressed & BUTTON_SQUARE) && !(self->buttons_held & BUTTON_L);
}

static int item_button_free(const Hunter *self) {
    return self->item_use_lockout_ticks == 0 && !(self->hunter_flags & HUNTER_FLAG_SELECTOR_OPEN);
}

static void after_move_input(Ctx *c, Hunter *self, uint8_t input) {
    if (input != INPUT_NONE && self->ground_forces_state_3b != 0 && self->state != ST_GROUND_FORCED)
        set_state(c, self, 0, ST_GROUND_FORCED, 0);
}

static void flag_40_state(Ctx *c, Hunter *self) {
    self->hunter_flags = (self->hunter_flags & ~(uint32_t)HUNTER_FLAG_MAY_ACT) | HUNTER_FLAG_8;
    set_state_noted(c, self, 0, ST_67, 0x10);
}

/* What both paths end with */
static void reactions(Ctx *c, Hunter *self) {
    if (self->buildup_state_2_13 >= BUILDUP_REACTS_AT) {
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_MAY_ACT;
        set_state_noted(c, self, 2, 0x13, 0);
    } else if ((uint8_t)call1(c, hunter_field_1144_is_positive, GAME_ADDR(self)) != 0) {
        self->hunter_flags |= HUNTER_FLAG_MAY_ACT;
        set_state_noted(c, self, 2, 0x1B, 0);
    }
}

/* -- while attached to something (+0x29C) -- */
static void while_attached(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_MAY_ACT;
    if ((uint8_t)call1(c, hunter_linked_monster_of_listed_types_has_480, self_addr) == 1) {
        set_state(c, self, 2, 7, 0);
        return;
    }
    if ((int32_t)(uint8_t)call1(c, hunter_classify_move_input, self_addr) > 0 && self->state != ST_MOVE_12) {
        call1(c, hunter_set_angle_320_from_49c_plus_camera_heading, self_addr);
        set_state(c, self, 0, ST_MOVE_12, 0);
    }
    call1(c, f_game_task_09AA6100, self_addr);
    if (hunt_is_ending(c))
        return;
    if (!weapon_is_out(self) && (self->buttons_pressed & BUTTON_CIRCLE)) {
        if ((uint8_t)call1(c, hunter_try_take_item_from_other_hunter, self_addr) == 0) {
            call2(c, hunter_activate_interaction_zone, self_addr, 0);
            call2(c, hunter_try_start_gather, self_addr, 0);
        }
    }
    if (item_button(self) && item_button_free(self)) {
        call2(c, f_game_task_09AA7580, self_addr, 2);
        self->selector_input_flags |= SELECTOR_INPUT_ITEM_USED;
    }
}

/* -- weapon out -- */
static void weapon_out(Ctx *c, Hunter *self, uint32_t called_as, uint32_t state_flags) {
    uint32_t self_addr = GAME_ADDR(self);
    uint8_t input = (uint8_t)call1(c, hunter_classify_move_input, self_addr);
    if ((input == INPUT_WALK || input == 2 || input == INPUT_RUN) && self->state != ST_RELEASED && self->state != ST_MOVE_3 &&
        self->state != ST_MOVE_74 && self->ground_forces_state_3b == 0)
        set_state(c, self, 0, ST_MOVE_3, 0);
    after_move_input(c, self, input);
    if (hunt_is_ending(c))
        return;
    if (self->hud_request != 0)
        call1(c, f_EBOOT_088DAA54, self_addr);
    if (weapon_is_out(self) && (self->buttons_pressed & BUTTON_SQUARE))
        set_state(c, self, 0, state_flags & STATE_FLAGS_SHEATHE_A ? ST_SHEATHE_A : ST_SHEATHE_5, 0);
    call2(c, hunter_snapshot_place_and_dispatch_on_1e8, self_addr, 1);
    if ((uint8_t)call1(c, hunter_test_fields_614_688_689, self_addr) == 0)
        call2(c, hunter_dispatch_on_1e8_if_9a8b058, self_addr, called_as == CALLED_AS_WALK ? 1 : 0);
    if (self->hunter_flags & HUNTER_FLAG_40)
        flag_40_state(c, self);
    reactions(c, self);
}

/* -- sheathed: where it moves to -- */
static int moved_sheathed(Ctx *c, Hunter *self, uint8_t input, uint32_t state_flags) {
    uint8_t state = self->state;
    if (input == INPUT_DASH) {
        if (state_flags & STATE_FLAGS_NO_DASH)
            return 0;
        if ((self->quest_flags & QUEST_FLAG_MONSTER_LINKED) &&
            ((self->linked_monster_angle - self->facing + 0x2000) & 0xFFFF) > LINKED_OFF_TO_THE_SIDE)
            set_state(c, self, 0, ST_DASH_LINKED, 0);
        else
            set_state(c, self, 0, ST_DASH, 0);
        return 1;
    }
    if (input == INPUT_RUN) {
        if (state != ST_MOVE_1 && state != ST_TURN_AROUND && state != ST_MOVE_13 && state != ST_RELEASED && state != ST_DASH && self->ground_forces_state_3b == 0) {
            call1(c, hunter_set_angle_320_from_49c_plus_camera_heading, GAME_ADDR(self));
            uint32_t turn = (self->facing_target + 0x10000u - self->facing) & 0xFFFF;
            if (turn >= TURN_BACK_FROM && turn <= TURN_BACK_TO)
                set_state(c, self, 0, ST_TURN_AROUND, 0x40);
            else
                set_state(c, self, 0, ST_MOVE_1, 0);
        }
    } else if (input == INPUT_WALK && state != ST_MOVE_49 && state != ST_RELEASED && self->ground_forces_state_3b == 0) {
        call1(c, hunter_set_angle_320_from_49c_plus_camera_heading, GAME_ADDR(self));
        set_state(c, self, 0, ST_MOVE_49, 0);
    }
    return 0;
}

static void evade(Ctx *c, Hunter *self, uint8_t input, uint32_t called_as, uint32_t state_flags) {
    if (called_as == CALLED_NO_EVADE) {
        set_state(c, self, 0, ST_08, 0);
        return;
    }
    if ((self->stick_magnitude < EVADE_MIN_STICK || input == INPUT_NONE) && !(state_flags & STATE_FLAGS_MAY_EVADE)) {
        set_state(c, self, 0, ST_08, 0);
        return;
    }
    if (self->stamina >= EVADE_MIN_STAMINA)
        set_state(c, self, 0, ST_EVADE, 4);
}

/* The weapon's own action on triangle, by weapon class */
static void weapon_action(Ctx *c, Hunter *self, uint32_t state_flags) {
    uint32_t self_addr = GAME_ADDR(self);
    uint8_t weapon_class = self->weapon_class;
    if (!(state_flags & STATE_FLAGS_WEAPON_INSTEAD)) {
        if (weapon_class == 1 || weapon_class == 5)
            call2(c, f_game_task_09BA40C0, self_addr, 0);
        else if (!(self->buttons_held & 0x200) || weapon_class == 10)
            set_state(c, self, 0, ST_BASE, 0);
        return;
    }
    switch (weapon_class) {
    case 0: call1(c, hunter_weapon0_enter_state_14_set_111c_mask_800_09BA31B0, self_addr); break;
    case 1:
    case 5: call2(c, f_game_task_09BA40C0, self_addr, 1); break;
    case 2: call1(c, f_game_task_09BA6590, self_addr); break;
    case 3: call1(c, f_game_task_09BA7E98, self_addr); break;
    case 4: call1(c, hunter_weapon4_enter_state_09_set_111c_mask_800, self_addr); break;
    case 6: call1(c, hunter_weapon6_enter_state_0f_set_111c_mask_800, self_addr); break;
    case 7: call1(c, f_game_task_09BAE028, self_addr); break;
    case 8: call1(c, f_game_task_09BB0830, self_addr); break;
    case 9: call1(c, hunter_weapon0_enter_state_14_set_111c_mask_800_09BB4DC0, self_addr); break;
    case 10: call1(c, f_game_task_09BB6AE0, self_addr); break;
    default: break;
    }
}

static void sheathed(Ctx *c, Hunter *self, uint32_t called_as, uint32_t state_flags) {
    uint32_t self_addr = GAME_ADDR(self);
    uint8_t input = (uint8_t)call1(c, hunter_classify_move_input, self_addr);
    if (self->state == ST_STOPPING || self->state == ST_TURN_AROUND)
        input = INPUT_NONE;
    int dashing = moved_sheathed(c, self, input, state_flags);
    if (self->hud_request != 0)
        call1(c, f_EBOOT_088DAA54, self_addr);
    after_move_input(c, self, input);
    if (hunt_is_ending(c))
        return;
    if (self->buttons_pressed & BUTTON_CROSS)
        evade(c, self, input, called_as, state_flags);
    if (item_button(self) && item_button_free(self)) {
        call2(c, hunter_try_start_gather, self_addr, 2);
        call2(c, f_game_task_09AA7580, self_addr, 0);
        self->selector_input_flags |= SELECTOR_INPUT_ITEM_USED;
    }
    /* just started dashing: circle held counts, not only pressed */
    uint16_t buttons = dashing ? (uint16_t)self->buttons_held : (uint16_t)self->buttons_pressed;
    if (!weapon_is_out(self) && (buttons & BUTTON_CIRCLE))
        call1(c, hunter_yaw_from_table_then_probe_ahead_bands, self_addr);
    if (self->buttons_pressed & BUTTON_SELECT)
        set_state(c, self, 1, ST_63, 4);
    if (self->button_action == ACTION_CIRCLE) {
        if ((uint8_t)call1(c, hunter_try_take_item_from_other_hunter, self_addr) == 0) {
            call2(c, hunter_activate_interaction_zone, self_addr, 0);
            call2(c, hunter_try_start_gather, self_addr, 0);
        }
    } else if (self->hunter_flags & HUNTER_FLAG_40) {
        flag_40_state(c, self);
    }
    call2(c, hunter_snapshot_place_and_dispatch_on_1e8, self_addr, 0);
    if (self->button_action == ACTION_TRIANGLE)
        weapon_action(c, self, state_flags);
    if ((uint8_t)call1(c, hunter_check_566_once_via_111c, self_addr) == 1 && (uint8_t)call2(c, player_byte_list_contains, self_addr, BONUS_1A) == 0) {
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_MAY_ACT;
        set_state(c, self, 0, ST_3E, 0);
        call2(c, f_EBOOT_08813188, LW(CAMERA_POINTER), self_addr);
    }
    reactions(c, self);
}

void hand_hunter_common_input(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t called_as = c->r[5];
    uint8_t ignore_quest_end = (uint8_t)c->r[6];
    if (self->instance_index != GAME()->local_hunter_slot) {
        if (LW(LW(RNG_POINTER) + GAME_FLAG) & 1)
            self->hunter_flags |= HUNTER_FLAG_MAY_ACT;
        return;
    }
    if (quest_state(c) == 4 && !(self->hunter_flags & HUNTER_FLAG_2000)) {
        set_state_noted(c, self, 4, 2, 0);
        return;
    }
    self->hunter_flags |= HUNTER_FLAG_MAY_ACT;
    if (self->attached_part_mode != 0) {
        while_attached(c, self);
        return;
    }
    if (ignore_quest_end == 0) {
        switch (quest_state(c)) {
        case 5:
        case 6: set_state_noted(c, self, 4, 3, 0); return;
        case 7: set_state_noted(c, self, 4, 6, 0); return;
        case 8: set_state_noted(c, self, 4, 5, 0); return;
        default: break;
        }
    }
    uint32_t state_flags = call2(c, object_test_flags_288, GAME_ADDR(self), ASKED_FLAGS);
    if (weapon_is_out(self))
        weapon_out(c, self, called_as, state_flags);
    else
        sheathed(c, self, called_as, state_flags);
}
