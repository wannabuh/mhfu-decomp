/* What a hunter does each tick around its state: rewritten by hand from game_task 09A66198 and EBOOT 088D1DD0, the generated code still
 * behind them (MHFU_HAND=0). */
#include "hand.h"
#include "names.h"

#define CAMERA_POINTER 0x089CBD04u
#define FILE_LOADER_POINTER 0x089CC554u
#define STAGE_POINTER 0x09A44D9Cu
#define VOLUMES_POINTER 0x09A4F1A0u
#define ASSETS_POINTER 0x09A4F210u
#define STAGE_CHECKS_VIEW 0x5C              /* a byte */
#define STAGE_FLAGS 0x54                    /* a word */
#define VSLOT_LOADER_BUSY 0x40
#define VSLOT_MOTION_ENDED 0x74
#define PLACE 0x200                         /* of the hunter: its callees take the address */

/* Asset slots, in what ASSETS_POINTER points at */
#define ASSET_SLOTS 0xFC
#define ASSET_SLOT_SIZE 12
#define ASSET_SLOT_COUNT 22
#define ASSET_SLOT_LOADED 3                 /* of the first's flags */
#define ASSET_SLOT_WANTED 2
#define ASSET_FIRST_ID 0xFE                 /* the first's id */
#define ASSETS_WANTED_FROM 0x204
#define ASSETS_WANTED_FROM_TO 0x210
#define ASSETS_WANTED_TO 0x208
#define ASSETS_WANTED_TO_TO 0x20C
#define ASSET_NONE 0xFFFF
#define AREA_ASSET_BASE 0x16B0

#define OBJECT_FLAG_HIDDEN 4        /* set while it is out of the stage, or out of view: see below */
#define QUEST_FLAG_SEEN 2           /* someone else's hunter, shown even with the camera not on it */
#define STATE_GROUP_FAINTED 3

static void assets_clear_wanted(Ctx *c) {
    uint32_t assets = LW(ASSETS_POINTER);
    SW(assets + ASSETS_WANTED_FROM_TO, LW(assets + ASSETS_WANTED_FROM));
    SW(assets + ASSETS_WANTED_TO_TO, LW(assets + ASSETS_WANTED_TO));
    for (uint32_t i = 0; i < ASSET_SLOT_COUNT; i++)
        SH(assets + ASSET_SLOTS + i * ASSET_SLOT_SIZE, LHU(assets + ASSET_SLOTS + i * ASSET_SLOT_SIZE) & ~(uint32_t)ASSET_SLOT_WANTED);
    call2(c, file_load_queue_cancel_by_tag, LW(FILE_LOADER_POINTER), 1);
}

/* The local player's: what the stage's edges do, and the area's assets being loaded */
static void local_stage_upkeep(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    if (call1(c, game_task_hunter_in_stage_volume_check, LW(STAGE_POINTER)) != 0) {
        self->reset_place_mode = 1;
        SW(LW(STAGE_POINTER) + STAGE_FLAGS, LW(LW(STAGE_POINTER) + STAGE_FLAGS) | 1);
    } else if (self->health <= 0 || (uint8_t)call2(c, unknown_mode_byte_2e_allows_index, LW(0x089CC558u), 1) == 1) {
        if (call2(c, stage_find_volume_containing_point, LW(VOLUMES_POINTER), self_addr + PLACE) != 0)
            call1(c, f_game_task_09A636D8, LW(STAGE_POINTER));
    }
    if (call1(c, game_flag_6addd_set_and_480_clear, LW(ASSETS_POINTER)) == 0 || call2(c, unknown_mode_byte_2e_allows_index, LW(0x089CC558u), 1) != 0) {
        assets_clear_wanted(c);
        return;
    }
    uint32_t area_asset;
    if (self->state_group != STATE_GROUP_FAINTED)
        area_asset = (uint16_t)call3(c, f_EBOOT_088C7424, LW(VOLUMES_POINTER), self->area_id, self_addr + PLACE);
    else
        area_asset = (uint16_t)call1(c, game_lookup_id_from_6af0e_or_table, LW(0x089CC558u));
    uint32_t assets = LW(ASSETS_POINTER);
    uint32_t loaded = LHU(assets + ASSET_SLOTS) & ASSET_SLOT_LOADED ? LHU(assets + ASSET_FIRST_ID) : ASSET_NONE;
    if (loaded == ((area_asset + AREA_ASSET_BASE) & 0xFFFF))
        return;
    assets_clear_wanted(c);
    call1(c, asset_slots_update, LW(ASSETS_POINTER));
    if (call_method1(c, LW(FILE_LOADER_POINTER), VSLOT_LOADER_BUSY, 1) == 0)
        call2(c, f_EBOOT_088DE6EC, LW(ASSETS_POINTER), area_asset);
}

/* game_task 09A66198: the hunter's vtable +0x38 */
void hand_hunter_state_function(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4];
    if (self->area_id != GAME()->stage_id) {        /* not in the stage being played */
        self->flags |= OBJECT_FLAG_HIDDEN;
        call1(c, hunter_scan_entity_list_for_target_angle, self_addr);
        if (self->timed_effect_ticks > 0)
            self->timed_effect_ticks--;
        call1(c, object_set_260_place_plus_table_09cdf040, self_addr);
        return;
    }
    self->flags &= ~(uint32_t)OBJECT_FLAG_HIDDEN;
    if (LBU(LW(STAGE_POINTER) + STAGE_CHECKS_VIEW) == 0) {
        self->flags |= OBJECT_FLAG_HIDDEN;
    } else if (self->instance_index == GAME()->local_hunter_slot) {
        if ((uint8_t)call2(c, unknown_camera_checks_961_621_arg_564, LW(CAMERA_POINTER), self_addr) == 1)
            self->flags |= OBJECT_FLAG_HIDDEN;
        else if (self->instance_index != GAME()->local_hunter_slot && !(self->quest_flags & QUEST_FLAG_SEEN))
            self->flags |= OBJECT_FLAG_HIDDEN;      /* (it was the local player's a moment ago: only if the camera's check changed that) */
    } else if (!(self->quest_flags & QUEST_FLAG_SEEN)) {
        self->flags |= OBJECT_FLAG_HIDDEN;
    }
    if (self->instance_index == GAME()->local_hunter_slot)
        local_stage_upkeep(c, self);
    call1(c, hunter_clear_flag_111c_mask8_unless_480_mask200, self_addr);
    call1(c, hunter_tick_counter_11b0_clear_111c_mask_2000, self_addr);
    call1(c, hunter_update_flag_614_mask8000_from_buttons, self_addr);
    call1(c, hunter_decode_buttons_480_484_to_action_1120, self_addr);
    call1(c, hunter_state_run, self_addr);
    uint32_t slot = call2(c, hunter_weapon_attach_slot_for_motion, self_addr, 0);
    self->weapon_attach_slot = (int16_t)slot;
    call2(c, hunter_update_field_612_by_byte_1e8, self_addr, (uint32_t)(int32_t)(int16_t)slot);
}

/* EBOOT 088D1DD0: the vtable's +0x1C */
void hand_hunter_tick_motion_copy(Ctx *c) {
    HunterBase *self = OBJ(HunterBase, c->r[4]);
    uint32_t self_addr = c->r[4];
    self->motion_id_copy = self->motion_id;
    if (self->effect_cooldown != 0)
        self->effect_cooldown--;
    if ((uint8_t)call1(c, object_29a_matches_game_6af0e, self_addr) == 1)
        call_method0(c, self_addr, VSLOT_MOTION_ENDED);
}
