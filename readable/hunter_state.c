/* A hunter's states: setting one, going back to the base one, and finding what runs for the state it is in. Rewritten by hand from
 * game_task 09A8B3F0, 09A67F30, 09A68700, 09A67630, 09A678F8, 09A67568 and 09A9A9E0 and EBOOT 088D5C34, the generated code still behind
 * them (MHFU_HAND=0). */
#include "hand.h"
#include "names.h"

#define VSLOT_STATE_SET_FOLLOW_UP 0x54      /* what it does isn't settled */
#define WEAPON_CLASS_KEEPS_CHARGE 9
#define MOTION_AIRBORNE 0x3F9
#define MOTION_AIRBORNE_START_FRAME 8
#define MOTION_LAYER1_ID_OFFSET 100

/* States are (group, state) */
enum {
    GROUP_ALLOWED_WHILE_RESETTING = 4,
    GROUP_REACTION = 2,
    STATE_BASE = 4,                 /* group 0: where a hunter with nothing to do goes */
    BASE_MODE_AIRBORNE = 2,
    BASE_MODE_FLAG_80 = 0x80,       /* only passed along */
};

/* The last argument of the state setter */
enum {
    HOW_KEEP_OVERLAYS = 1,
    HOW_NO_FOLLOW_UP = 2,
    HOW_FOLLOW_UP_6 = 0x10,
    HOW_KEEP_RANDOM_LOW_BYTE = 0x20,
};

#define HUNTER_FLAG_4 4                 /* set while an input may act: not settled */
#define HUNTER_FLAG_MOTION_STARTED 0x180    /* layers 0 and 1 */
#define HUNTER_FLAG_800 0x800
#define MISC_FLAG_STATE_SET_FROM_HERE 4
#define MISC_FLAG_2 2
#define MISC_FLAG_1 1
#define BUILDUP_REACTS_AT 50

#define RNG_POINTER 0x08A632C0u
#define MOTION_BASE_SPEED 2.0f

static int is_local_hunter(Ctx *c, const Hunter *self) {
    return self->instance_index == GAME()->local_hunter_slot;
}

/* game_task 09A8B3F0 */
void hand_hunter_clear_overlays(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->overlay_enabled_0 = self->overlay_enabled_1 = self->overlay_enabled_2 = self->overlay_enabled_3 = 0;
    self->overlay_blend_0 = self->overlay_blend_1 = self->overlay_blend_2 = self->overlay_blend_3 = 0;
    self->overlay_motion_id_0 = self->overlay_motion_id_1 = self->overlay_motion_id_2 = self->overlay_motion_id_3 = 0;
}

/* game_task 09A67F30: both layers' blend set, but only layer 0's motion */
void hand_hunter_start_airborne_motion(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    int16_t blend = (int16_t)((int16_t)c->r[5] / 2);
    self->motion_id = MOTION_AIRBORNE;
    self->motion_blend_frames = blend;
    self->part1_motion_blend_frames = blend;
    self->motion_start_frame = MOTION_AIRBORNE_START_FRAME;
    self->part1_motion_start_frame = MOTION_AIRBORNE_START_FRAME;
    call2(c, object_set_flags_288, GAME_ADDR(self), 1);
}

/* EBOOT 088D5C34: motion id, blend frames (halved, and shared by the layers), start frame */
void hand_hunter_set_motion(Ctx *c) {
    HunterBase *self = OBJ(HunterBase, c->r[4]);
    uint32_t id = c->r[5];
    int32_t half_blend = (int32_t)c->r[6] / 2;
    self->motion_id = (uint16_t)id;
    self->motion_id_layer1 = (uint16_t)(id + MOTION_LAYER1_ID_OFFSET);
    self->motion_blend_frames = (int16_t)half_blend;
    self->part1_motion_blend_frames = (int16_t)half_blend;
    self->motion_start_frame = (uint16_t)c->r[7];
    self->part1_motion_start_frame = (uint16_t)c->r[7];
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_MOTION_STARTED;
}

/* game_task 09A68700 */
void hand_hunter_clear_transient_action_fields(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    self->life_state = 0;
    self->state_step = 0;
    self->state_scratch_a = 0;
    self->state_scratch_b = 0;
    self->hunter_flags &= ~(uint32_t)(HUNTER_FLAG_MOTION_STARTED | HUNTER_FLAG_800);
    self->flagged_ticks = 0;
    self->state_ticks = 0.0f;
    self->flag288_phase1_ticks = 0;
    self->flag288_phase2_ticks = 0;
    self->state_flags &= 0x100;
    self->state_flags_high = 0;
    self->transient_45e = 0;
    self->hit_immune_ticks = 0;
    self->action_count = 0;
    self->transient_282 = 0;
    self->misc_flags &= ~(uint32_t)MISC_FLAG_2;
    if (self->weapon_class != WEAPON_CLASS_KEEPS_CHARGE)
        self->charge_time = 0;
    self->listed_item_ticks = 0;
    self->motion_part0_speed = MOTION_BASE_SPEED;
    self->motion_part1_speed = MOTION_BASE_SPEED;
    self->motion_speed_scale = 1.0f;
}

/* game_task 09A67630: the hunter's vtable +0x6C */
void hand_hunter_set_state(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4], group = c->r[5], state = c->r[6], how_word = c->r[7];
    uint16_t how = (uint16_t)how_word;
    if (is_local_hunter(c, self) && self->reset_place_mode != 0 && (uint16_t)group != GROUP_ALLOWED_WHILE_RESETTING)
        return;
    self->previous_state_group = self->state_group;
    self->previous_state = self->state;
    self->state_group = (uint8_t)group;
    self->state = (uint8_t)state;
    self->state_step = 0;
    self->state_scratch_a = 0;
    self->state_scratch_b = 0;
    if (how & HOW_KEEP_RANDOM_LOW_BYTE) {
        uint8_t low = (uint8_t)self->state_random;
        self->state_random = (uint16_t)((call2(c, math_rng_next, LW(RNG_POINTER), 1) & 0xFF00) | low);
    } else {
        self->state_random = (uint16_t)call2(c, math_rng_next, LW(RNG_POINTER), 1);
    }
    self->misc_flags &= ~(uint32_t)MISC_FLAG_STATE_SET_FROM_HERE;
    self->motion_speed_scale = 1.0f;
    if (!(how & HOW_KEEP_OVERLAYS)) {
        call1(c, hunter_clear_slots_1a8_1b0_1b8, self_addr);
        self->signed_overlay_blend = 0;
    }
    call1(c, hunter_set_fields_10f0_10f1_from_weapon_class, self_addr);
    if (self->state_group == GROUP_REACTION)
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_800;
    call2(c, unknown_sets_field_3f4_from_32c_334, self_addr, 0);
    self->state_flags &= 0x80100;
    self->state_flags_high = 0;
    self->motion_part0_speed = MOTION_BASE_SPEED;
    self->motion_part1_speed = MOTION_BASE_SPEED;
    if (is_local_hunter(c, self)) {
        if (how & HOW_NO_FOLLOW_UP)
            return;
        c->r[8] = 0;
        call_method3(c, self_addr, VSLOT_STATE_SET_FOLLOW_UP, how & HOW_FOLLOW_UP_6 ? 6 : 1, how_word, 0);
    } else if ((uint8_t)call3(c, object_state_is, self_addr, 0, 0) != 0) {
        /* someone else's hunter gone to (0, 0): straight to the base state */
        call4(c, how & HOW_KEEP_OVERLAYS ? f_game_task_09A67A10 : hunter_reset_to_base_state_and_motion, self_addr, 0, STATE_BASE, 0);
    }
}

/* game_task 09A678F8. The game also tests the movement mode for 0xF just after setting it to 0 or 2: never true, so not here. */
void hand_hunter_reset_to_base_state_and_motion(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4], mode = c->r[5], arg2 = c->r[6], arg3 = c->r[7];
    call1(c, hunter_clear_transient_action_fields, self_addr);
    call1(c, hunter_clear_slots_1a8_1b0_1b8, self_addr);
    if (self->attached_part_mode != 0 || self->buildup_state_2_13 >= BUILDUP_REACTS_AT)
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_4;
    else
        self->hunter_flags |= HUNTER_FLAG_4;
    if ((mode & ~(uint32_t)BASE_MODE_FLAG_80) == BASE_MODE_AIRBORNE) {
        self->movement_mode = BASE_MODE_AIRBORNE;
        call2(c, f_game_task_09A67F30, self_addr, (uint32_t)(int32_t)(int16_t)arg2);
    } else {
        call1(c, hunter_clear_velocity_accel_and_flags_288_keep_104_09A68798, self_addr);
        self->movement_mode = 0;
        call4(c, hunter_enter_state_0_0_with_base_motion_pair, self_addr, arg2, arg3, mode & BASE_MODE_FLAG_80);
    }
    call2(c, unknown_sets_field_3f4_from_32c_334, self_addr, 0);
}

/* game_task 09A67568: the hunter's state function, by its group */
void hand_hunter_run_state(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4];
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_4;
    self->misc_flags |= MISC_FLAG_1;
    switch (self->state_group) {
    case 0: call1(c, hunter_state_group0_dispatch, self_addr); break;
    case 1: call1(c, hunter_action_state_dispatch_by_byte_1e8, self_addr); break;
    case 2: call1(c, hunter_reaction_state_dispatch_by_state_299, self_addr); break;
    case 3: call1(c, hunter_faint_state_dispatch, self_addr); break;
    case 4: call1(c, hunter_state_group4_dispatch, self_addr); break;
    case 5: call1(c, f_game_task_09A98DE8, self_addr); break;
    case 6: call1(c, f_game_task_09A8BCB0, self_addr); break;
    default: break;
    }
}

/* game_task 09A9A9E0: group 0's states. The function a state runs, and which of its variants: states not listed go back to the base. */
void hand_hunter_group0_state(Ctx *c) {
    uint32_t self = c->r[4];
    switch (OBJ(Hunter, self)->state) {
    case 0x00: call2(c, hunter_state_idle_select_motion, self, 0); break;
    case 0x01: call2(c, hunter_state_move_motion_variants, self, 0); break;
    case 0x02: call1(c, f_game_task_09A9C0D8, self); break;
    case 0x03: call2(c, hunter_state_move_motion_variants, self, 2); break;
    case 0x04: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 0); break;
    case 0x05: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 1); break;
    case 0x06: call2(c, hunter_state_airborne_launch_and_land, self, 0); break;
    case 0x07: call2(c, hunter_state_move_motion_variants, self, 8); break;
    case 0x08: call2(c, f_game_task_09A9C990, self, 0); break;
    case 0x09: call2(c, hunter_state_airborne_launch_and_land, self, 1); break;
    case 0x0A: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 2); break;
    case 0x0B: call2(c, f_game_task_09A9CC58, self, 1); break;
    case 0x0C: call2(c, f_game_task_09A9CB18, self, 0); break;
    case 0x0D: call2(c, f_game_task_09A9CB18, self, 1); break;
    case 0x0E: call2(c, f_game_task_09A9CC58, self, 0); break;
    case 0x0F: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 3); break;
    case 0x10: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 4); break;
    case 0x11: call2(c, hunter_ledge_grab, self, 0); break;
    case 0x12: call2(c, hunter_state_move_motion_variants, self, 3); break;
    case 0x13: call2(c, hunter_state_move_motion_variants, self, 4); break;
    case 0x14: call2(c, f_game_task_09A9D928, self, 1); break;
    case 0x15: call2(c, hunter_ledge_drop_to_hang, self, 0); break;
    case 0x16: call1(c, f_game_task_09A9D860, self); break;
    case 0x17: call2(c, hunter_state_draw_sheathe_weapon_variants, self, 5); break;
    case 0x18: call2(c, f_game_task_09A9D928, self, 0); break;
    case 0x19: call1(c, f_game_task_09A9DAD0, self); break;
    case 0x1A: call2(c, hunter_state_move_motion_variants, self, 5); break;
    case 0x1B: call2(c, hunter_ledge_grab, self, 1); break;
    case 0x1C: call2(c, hunter_state_evade, self, 0); break;
    case 0x1D: call2(c, f_game_task_09A9C990, self, 1); break;
    case 0x1E: call1(c, f_game_task_09A9E0E0, self); break;
    case 0x1F: call2(c, hunter_state_dash, self, 0); break;
    case 0x20: call2(c, hunter_state_slow_stamina_run, self, 0); break;
    case 0x21: call1(c, f_game_task_09A9EAE8, self); break;
    case 0x22: call2(c, hunter_state_slow_stamina_run, self, 1); break;
    case 0x24: call2(c, hunter_state_move_motion_variants, self, 6); break;
    case 0x25: call2(c, hunter_state_evade, self, 3); break;
    case 0x26: call1(c, hunter_state_motion_13_clear_29c_camera_shake, self); break;
    case 0x27: call1(c, f_game_task_09A9ECB0, self); break;
    case 0x28: call2(c, f_game_task_09A9EDD8, self, 0); break;
    case 0x29: call2(c, f_game_task_09A9EFF8, self, 0); break;
    case 0x2A: call1(c, f_game_task_09A9F1F8, self); break;
    case 0x2B: call1(c, f_game_task_09A9F2A8, self); break;
    case 0x2C: call1(c, hunter_state_stop_from_motion_3, self); break;
    case 0x2D: call2(c, hunter_state_evade, self, 1); break;
    case 0x2E: call1(c, f_game_task_09A9F458, self); break;
    case 0x2F: call1(c, f_game_task_09A9F5B0, self); break;
    case 0x30: call2(c, hunter_ledge_drop_to_hang, self, 1); break;
    case 0x31: call2(c, hunter_state_evade_sideways, self, 0); break;
    case 0x32: call2(c, hunter_state_evade_sideways, self, 1); break;
    case 0x33: call1(c, f_game_task_09A9FCC0, self); break;
    case 0x34: call1(c, f_game_task_09A9FDC0, self); break;
    case 0x35: call1(c, f_game_task_09A9FFC8, self); break;
    case 0x36: call2(c, f_game_task_09AA0090, self, 0); break;
    case 0x37: call1(c, f_game_task_09AA04B0, self); break;
    case 0x38: call2(c, hunter_ledge_drop_to_hang, self, 2); break;
    case 0x39: call1(c, f_game_task_09AA05C8, self); break;
    case 0x3A: call1(c, hunter_state_motion_12_oneshot, self); break;
    case 0x3B: call1(c, f_game_task_09AA0700, self); break;
    case 0x3C: call1(c, f_game_task_09AA0828, self); break;
    case 0x3D: call1(c, f_game_task_09AA0900, self); break;
    case 0x3E: call1(c, f_game_task_09AA0A18, self); break;
    case 0x3F: call1(c, hunter_state_turn_around, self); break;
    case 0x44: call1(c, f_game_task_09AA0B80, self); break;
    case 0x45: call2(c, hunter_state_slow_stamina_run, self, 2); break;
    case 0x46: call2(c, hunter_state_dash, self, 1); break;
    case 0x47: call2(c, hunter_state_timed_collect_action, self, 0); break;
    case 0x48: call1(c, f_game_task_09AA0328, self); break;
    case 0x49: call2(c, hunter_state_move_motion_variants, self, 7); break;
    case 0x4A: call2(c, hunter_state_timed_collect_action, self, 3); break;
    case 0x4B: call2(c, f_game_task_09AA1B60, self, 1); break;
    case 0x4C: call1(c, f_game_task_09AA19F0, self); break;
    case 0x4D: call2(c, f_game_task_09AA1B60, self, 0); break;
    case 0x4E: call2(c, f_game_task_09AA4C00, self, 1); break;
    case 0x4F: call1(c, f_game_task_09AA1D50, self); break;
    case 0x50: call2(c, f_game_task_09AA1C20, self, 1); break;
    case 0x51: call2(c, hunter_state_timed_collect_action, self, 4); break;
    case 0x52: call2(c, hunter_state_timed_collect_action, self, 5); break;
    case 0x53: call1(c, f_game_task_09AA1EA0, self); break;
    case 0x54: call2(c, f_game_task_09AA2378, self, 0); break;
    case 0x55: call1(c, f_game_task_09AA2590, self); break;
    case 0x56: call1(c, f_game_task_09AA2CC0, self); break;
    case 0x57: call2(c, hunter_state_wait_flag_614_credit_item_value_zenny, self, 0); break;
    case 0x58: call2(c, f_game_task_09AA3078, self, 0); break;
    case 0x59: call1(c, f_game_task_09AA3328, self); break;
    case 0x5A: call2(c, f_game_task_09AA3078, self, 1); break;
    case 0x5B: call1(c, f_game_task_09AA3460, self); break;
    case 0x5C: call2(c, hunter_state_timed_collect_action, self, 2); break;
    case 0x5D: call2(c, f_game_task_09AA15C0, self, 0); break;
    case 0x5E: call2(c, f_game_task_09AA35B0, self, 0); break;
    case 0x5F: call2(c, f_game_task_09AA3738, self, 0); break;
    case 0x60: call2(c, f_game_task_09AA15C0, self, 1); break;
    case 0x61: call1(c, f_game_task_09AA3AB0, self); break;
    case 0x62: call2(c, f_game_task_09AA35B0, self, 1); break;
    case 0x63: call2(c, f_game_task_09AA3738, self, 1); break;
    case 0x64: call2(c, f_game_task_09AA2378, self, 1); break;
    case 0x65: call2(c, f_game_task_09AA3C70, self, 0); break;
    case 0x66: call2(c, f_game_task_09AA3C70, self, 1); break;
    case 0x67: call1(c, f_game_task_09AA3E00, self); break;
    case 0x68: call1(c, f_game_task_09AA3F18, self); break;
    case 0x69: call1(c, f_game_task_09AA4090, self); break;
    case 0x6A: call1(c, f_game_task_09AA4180, self); break;
    case 0x6B: call2(c, f_game_task_09AA4270, self, 0); break;
    case 0x6C: call2(c, f_game_task_09AA4270, self, 1); break;
    case 0x6D: call2(c, f_game_task_09AA4270, self, 2); break;
    case 0x6E: call2(c, f_game_task_09AA4270, self, 3); break;
    case 0x6F: call1(c, f_game_task_09AA48E8, self); break;
    case 0x70: call1(c, f_game_task_09AA49F0, self); break;
    case 0x71: call2(c, f_game_task_09AA4B08, self, 1); break;
    case 0x72: call2(c, f_game_task_09A9CC58, self, 2); break;
    case 0x73: call2(c, f_game_task_09AA4C00, self, 0); break;
    case 0x74: call2(c, hunter_state_move_motion_variants, self, 9); break;
    case 0x75: call2(c, f_game_task_09AA4B08, self, 0); break;
    case 0x76: call2(c, hunter_state_idle_select_motion, self, 1); break;
    case 0x77: call2(c, f_game_task_09A9EDD8, self, 1); break;
    case 0x78: call2(c, f_game_task_09A9EFF8, self, 1); break;
    case 0x79: call2(c, f_game_task_09AA15C0, self, 2); break;
    case 0x7A: call2(c, f_game_task_09AA0090, self, 1); break;
    case 0x7B: call2(c, hunter_state_wait_flag_614_credit_item_value_zenny, self, 1); break;
    case 0x7C: call2(c, hunter_state_timed_collect_action, self, 7); break;
    case 0x7D: call2(c, f_game_task_09AA15C0, self, 3); break;
    case 0x7E: call2(c, hunter_state_evade, self, 2); break;
    case 0x7F: call1(c, f_game_task_09AA4D00, self); break;
    default: call4(c, hunter_reset_to_base_state_and_motion, self, 0, STATE_BASE, 0); break;
    }
}
