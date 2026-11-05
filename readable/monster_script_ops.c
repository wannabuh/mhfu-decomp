/* The monster script's opcodes, by hand: those the recordings ran, those the most monster types' scripts use first. The generated code is still behind each (MHFU_HAND=0).
 * Each handler gets a0 the monster and a1 the byte after its opcode, and returns where the script goes on. What each tests or does is from reading its code;
 * the game's own words for these aren't known. How scripts run: monster_script.c. */
#include <stddef.h>

#include "hand.h"
#include "hit.h"
#include "names.h"
#include "monster_script.h"

#define table_24_enter f_game_task_09ABD790
#define restart_script f_game_task_09ABAFF0
#define point_order_list f_game_task_09AC33F8
#define hunter_by_slot f_EBOOT_088DCEBC
#define set_alert_mode f_game_task_09AC93B0
#define type_in_group f_game_task_09ACC450
#define save_position f_game_task_09AB74D0
#define table_08_entry f_game_task_09ABDB28
#define area_table_find_entry f_game_task_09AD1CB8
#define set_type_block_644 f_game_task_09AC7478
#define global_bit_test f_game_task_09AD8438
#define resolve_target_position f_game_task_09AB9170
#define angle_to_place f_game_task_09AC8BF0
#define hunter_point f_game_task_09ABA0C8
#define point_in_area f_EBOOT_08865C2C
#define condition_09ac04f0 f_game_task_09AC04F0
#define id_sequence_list_of f_game_task_09ABDA78
#define id_lookup f_game_task_09ABDAA8
#define id_sequence_entry_enter f_game_task_09ABDBB8
#define HUNTER_MANAGER_POINTER 0x09A4F204u
#define GAME_POINTER_ADDR 0x089CC558u
#define GAME_MODE_BYTE 0x6AF10u
#define MONSTER_FLAG_200 0x200
#define ID_SEQUENCE_ENTRY 0x539u
#define ID_LOOKED_UP 50000
#define MONSTER_FLAG_BOOST 0x20
#define MONSTER_TYPE_AREA_TABLES 0x158          /* -> a structure: +4 tables of points, +0x18 tables of links, by area */
#define FIELD_292 0x292u
#define TARGET_KIND_HUNTERS_AREA 0xA
#define POINTS_TOP_TIER 50000
#define POINTS_SECOND_TIER 30000
#define MONSTERS_POINTER 0x09C122B0u
#define MONSTERS_LIST 0x1220u                   /* where its monsters are listed */
#define TARGET_KIND_OTHER_MONSTER 0xD
#define TARGET_KIND_HUNTER_SLOT 1
#define TARGET_KIND_LIST_POINT 2
#define TARGET_KIND_AREA 3
#define NO_DISTANCE -1.0f

enum { OP_IF_HUNTERS_TRACKED = 2, OP_IF_STATE = 0x34, OP_SWITCH_AREA = 0x15, OP_SWITCH_DISTANCE = 0x83, OP_IF_CHOSEN_HUNTER_HERE = 0x54, OP_IF_EVENT = 0x1B,
       OP_SWITCH_TYPE_GROUP = 0x2C, OP_SWITCH_VARIANT = 0x27, OP_SWITCH_METHOD_DC = 0x79, OP_IF_BOOST = 0x35, OP_IF_ANY_HUNTER_HERE = 0x28, OP_SWITCH_TYPE = 0x70, OP_IF_ON_FLOOR = 8,
       OP_IF_ALERT_TICKS_OUT = 0x2A, OP_IF_AREA_TICKS_OUT = 0x29, OP_SWITCH_PREVIOUS_AREA = 0x3E, OP_IF_292 = 0x5E, OP_IF_GLOBAL_BIT = 0x43, OP_IF_SCALE_ROW = 0x71, OP_IF_TARGET_ANGLE = 0x42, OP_IF_HUNTER_ANGLE = 0x14, OP_IF_GAME_MODE = 0x7C, OP_IF_REMAP_ENTRY = 0x77, OP_SWITCH_ZERO = 0x76, OP_IF_PAIRED = 0x3B,
       OP_IF_VARIANT_BITS = 0x63, OP_IF_MODE_4 = 0x51, OP_IF_09AC04F0 = 0x38, OP_IF_METER_LOW = 0x21 };
enum { SWITCH_DISTANCE_END = 0xFF, SWITCH_AREA_END = 3, POINT_ORDER_RANDOM = 0, POINT_ORDER_FROM_LIST = 1, POINT_ORDER_FIRST = 2 };

/* -- which way to go ---------------------------------------------------------------------------------------------------------------------------- */

/* How far the target is: the hunter's distance as kept on the monster, or another monster's worked out. NO_DISTANCE if there's no target. */
static float distance_to_chosen(Ctx *c, Monster *self) {
    uint8_t chosen = self->chosen_slot;
    if (chosen == 0xFF)
        return NO_DISTANCE;
    uint32_t slot = chosen & 0xFu;
    if (self->resolved_target_kind != TARGET_KIND_OTHER_MONSTER)
        return (&self->distance_to_hunter0)[slot];
    uint32_t other = LW(LW(MONSTERS_POINTER) + 4 * slot + MONSTERS_LIST);
    if (other == 0)
        return NO_DISTANCE;
    return distance_3d(self->place, OBJ(Monster, other)->place);
}

/* game_task 09ABA8F8, opcode 0x83: goes to the first branch whose distance band holds how far the target is. "83 0 count .. ..", then branches each beginning "83 n",
 * then "83 FF". The bands are script_distance_band0..3. With no target, or past them all, it takes the "count"th. A branch that runs into the next goes past the end. */
void hand_monster_script_op_83_switch_on_distance_to_chosen_slot(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    at++;
    if (sub == 0) {
        float distance = distance_to_chosen(c, self);
        uint8_t count = LBU(at);
        int32_t branch = 0;
        at++;
        if (distance == NO_DISTANCE) {
            branch = count;
        } else {
            self->distance_to_chosen_slot = distance;
            const float *bands = &self->script_distance_band0;
            while (branch < count && !(self->distance_to_chosen_slot <= bands[branch] && bands[branch] >= 0.0f))
                branch++;
        }
        for (int32_t i = 0; i < branch; i++) {
            at = next_of(c, self_addr, at);
            at = call4(c, skip_to, self_addr, at, OP_SWITCH_DISTANCE, SWITCH_DISTANCE_END);
        }
        c->r[2] = next_of(c, self_addr, at);
    } else if (sub >= 1 && sub <= 5) {
        for (;;) {
            uint32_t entry = call4(c, skip_to, self_addr, at, OP_SWITCH_DISTANCE, SWITCH_DISTANCE_END);
            at = entry + 2;
            if (LBU(entry + 1) == SWITCH_DISTANCE_END)
                break;
        }
        c->r[2] = at;
    } else {
        c->r[2] = at;
    }
}

/* game_task 09ABC600, opcode 0x15: a switch on which area the monster is in. "15 0 count .. .." then for each case a big-endian area id, its branch, and "15 n" for the next;
 * "15 2" or "15 3" ends it. Ids and the monster's area are compared as remapped. A case that matches goes on from there; one that runs into the next goes past the end ("15 3"). */
void hand_monster_script_op_15_switch_on_field_29a_remapped(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (switch_not_the_first_case(c, self_addr, at, OP_SWITCH_AREA, 2))
        return;
    int32_t count = LBU(at + 1);
    at += 4;
    for (int32_t i = 0; i < count; i++) {
        uint16_t id = (uint16_t)(LBU(at) << 8 | LBU(at + 1));
        at += 2;
        if (area_as_remapped(c, self->area_id) == area_as_remapped(c, id) || !to_next_case(c, self_addr, &at, OP_SWITCH_AREA))
            break;
    }
    c->r[2] = at;
}

/* -- entering other parts of the script --------------------------------------------------------------------------------------------------------- */

/* game_task 09ABD778, opcode 0x16: enters an entry of the table at header +0x24; sub-code 3 of FF comes back here. */
void hand_monster_script_op_16_enter_table_24_entry(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5];
    self->script_table_24_entry = LBU(at);
    self->script_return_table_24 = at + 1;
    call2(c, table_24_enter, c->r[4], self->script_table_24_entry);
}

/* game_task 09ABAAF8, opcode 0x81: enters an entry of the table at header +4, one level in. Only from level 0 does it note where to come back to: entering from inside
 * replaces the running one, and comes back to the first's caller. */
void hand_monster_script_op_81_enter_table_04_entry(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5];
    self->script_level1_entry = LBU(at);
    if (self->script_nest_level == 0)
        self->script_return_level1 = at + 1;
    self->script_nest_level = 1;
    c->r[2] = LW(LW(self->script_header + 4) + 4u * self->script_level1_entry);
}

/* game_task 09ABAB38, opcode 0x82: "82 table entry": enters an entry of one of the tables from header +0x3C, a level further in. Notes where to come back to only from level 1. */
void hand_monster_script_op_82_enter_table_3c_entry(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5];
    uint32_t table = LBU(at);
    self->script_level2_entry = LBU(at + 1);
    if (self->script_nest_level == 1)
        self->script_return_level2 = at + 2;
    self->script_nest_level = 2;
    c->r[2] = LW(LW(self->script_header + 4 * table + 0x3C) + 4u * self->script_level2_entry);
}

/* -- tests -------------------------------------------------------------------------------------------------------------------------------------- */

/* game_task 09ABA340, opcode 2: if any hunter is tracked. If none, also no hunter is the target. Unlike most, a byte it doesn't know isn't passed. */
void hand_monster_script_op_02_if_slot_mask_503_nonzero(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    if (sub == IF_END) {
        c->r[2] = at + 1;
    } else if (sub == IF_ELSE) {
        c->r[2] = call3(c, skip_past_end, self_addr, at + 1, OP_IF_HUNTERS_TRACKED);
    } else if (sub == IF_TEST) {
        at++;
        if (self->hunters_tracked_mask != 0) {
            c->r[2] = at;
        } else {
            self->chosen_slot = 0xFF;
            c->r[2] = to_else_or_end(c, self_addr, at, OP_IF_HUNTERS_TRACKED);
        }
    } else {
        c->r[2] = at;
    }
}

/* game_task 09ABB670, opcode 0x34: "34 0 group state": if the monster's state is this one */
void hand_monster_script_op_34_if_bytes_298_299_equal(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (if_not_the_test(c, self_addr, at, OP_IF_STATE))
        return;
    uint8_t group = LBU(at + 1), state = LBU(at + 2);
    if_test_answer(c, self_addr, at + 3, OP_IF_STATE, self->state_group == group && self->state == state);
}

/* -- targets ------------------------------------------------------------------------------------------------------------------------------------ */

/* game_task 09ABA440, opcode 0x12: the target is the highest-numbered tracked hunter. No hunter tracked: the slot stays as it was, kind and first parameter are still set. */
void hand_monster_script_op_12_target_highest_slot_in_mask_503(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    for (uint32_t slot = 0; slot < 4; slot++) {
        if (self->hunters_tracked_mask & (1u << slot)) {
            self->target_param2 = (uint16_t)slot;
            self->chosen_slot = (uint8_t)self->target_param2;
        }
    }
    self->target_kind = TARGET_KIND_HUNTER_SLOT;
    self->target_param1 = 0;
    if (self->locally_owned != 0)
        self->request_bit_8_ticks = 2;
    self->script_active_requests &= 0xEE;
    c->r[2] = c->r[5];
}

/* game_task 09ABCC10, opcode 0x19: the target is the area route_final_area */
void hand_monster_script_op_19_target_kind_3_from_field_540(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    self->target_kind = TARGET_KIND_AREA;
    self->target_param2 = self->route_final_area;
    self->target_param1 = self->route_final_area;
    c->r[2] = c->r[5];
}

/* A random point of the area's table, for all the list on the stack may be longer than the room there. No points at all: index 0 and the script starts over. */
static void first_point_random(Ctx *c, Monster *self, uint32_t sp) {
    int8_t count = self->point_block_point_count;
    for (int8_t i = 0; i < count; i++)
        SB(sp + 0x14 + (uint32_t)(int32_t)i, (uint8_t)i);
    if (count <= 0) {
        self->point_block_index = 0;
        call1(c, restart_script, GAME_ADDR(self));
    } else {
        self->point_block_index = (int8_t)LBU(sp + 0x14 + (uint32_t)(int32_t)(int8_t)(self->script_random_counter % count));
    }
}

/* game_task 09ABC9B8, opcode 0x10: the target is the point block's current point. If there's none yet, or it's past the last, one is chosen by point_block_order: 0 at random,
 * 1 the first of a list, 2 the first. */
void hand_monster_script_op_10_target_point_index_534(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5], sp = c->r[29] - 0x20;
    c->r[29] = sp;
    uint8_t order = self->point_block_order;
    if (order <= POINT_ORDER_FIRST && (self->point_block_index == -1 || self->point_block_index >= self->point_block_point_count)) {
        if (order == POINT_ORDER_FIRST)
            self->point_block_index = 0;
        else if (order == POINT_ORDER_FROM_LIST)
            self->point_block_index = (int8_t)LBU(call2(c, point_order_list, GAME_ADDR(self), self->point_block_order_list));
        else
            first_point_random(c, self, sp);
    }
    self->target_kind = TARGET_KIND_LIST_POINT;
    self->target_param1 = 1;
    self->target_param2 = (uint16_t)(int16_t)self->point_block_index;
    c->r[29] = sp + 0x20;
    c->r[2] = at;
}

/* A hunter by slot, or 0 */
static uint32_t hunter_of_slot(Ctx *c, uint32_t slot) {
    return call2(c, hunter_by_slot, LW(HUNTER_MANAGER_POINTER), slot);
}

/* game_task 09ABA858, opcode 0x13: the target is the chosen hunter, or slot 0xFF if there's none */
void hand_monster_script_op_13_target_chosen_slot_511(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    self->target_kind = TARGET_KIND_HUNTER_SLOT;
    self->target_param1 = 0;
    uint8_t chosen = self->chosen_slot;
    self->target_param2 = chosen == 0xFF ? 0xFF : chosen & 0xFu;
    c->r[2] = c->r[5];
}

/* Going to an area: nothing further after it */
static void target_area_set(Monster *self) {
    self->target_param1 = self->target_param2;
    self->route_next_area = self->target_param2;
    self->route_final_area = 0xFFFF;
}

/* game_task 09AB8E48, opcode 6: "06 kind a b": sets the target. Mostly kind and two parameters. Kind 3: an area, whose big-endian id follows. Kind 0xA: the area the chosen hunter
 * is in, as kind 3. Kind 0xD: the first other live monster in the same area with monster_flags 2, or 0xFF. */
void hand_monster_script_op_06_set_target_spec(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint16_t kind = LBU(at);
    self->target_kind = kind;
    if (kind == TARGET_KIND_AREA) {
        self->target_param1 = LBU(at + 1);
        self->target_param2 = (uint16_t)(LBU(at + 2) << 8 | LBU(at + 3));
        self->target_param2 = area_by_remap_flag(c, self->target_param2);
        target_area_set(self);
        c->r[2] = at + 4;
        return;
    }
    self->target_param1 = LBU(at + 1);
    self->target_param2 = LBU(at + 2);
    c->r[2] = at + 3;
    if (kind == TARGET_KIND_HUNTERS_AREA) {
        self->target_kind = TARGET_KIND_AREA;
        uint8_t chosen = self->chosen_slot;
        if (chosen == 0xFF) {
            self->target_param2 = self->area_id;
        } else {
            uint32_t hunter = hunter_of_slot(c, chosen);
            self->target_param2 = hunter != 0 ? OBJ(Monster, hunter)->area_id : self->area_id;
        }
        target_area_set(self);
        c->r[2] = at + 3;
    } else if (kind == TARGET_KIND_OTHER_MONSTER) {
        uint32_t monsters = LW(MONSTERS_POINTER), slot;
        for (slot = 0; slot < 20; slot++) {
            uint32_t other = LW(monsters + MONSTERS_LIST + 4 * slot);
            if (other != 0 && (LW(other + 4) & 1) && OBJ(Monster, other)->area_id == self->area_id && other != self_addr && (OBJ(Monster, other)->monster_flags & 2))
                break;
        }
        self->target_param2 = slot < 20 ? (uint16_t)slot : 0xFF;
    }
}

/* Among hunters whose points are at least these: returns how many, listed. */
static int hunters_with_points(const Monster *self, uint8_t usable, int32_t points, uint8_t list[4]) {
    int n = 0;
    for (uint32_t slot = 0; slot < 4; slot++)
        if ((&self->target_points_hunter0)[slot] >= points && (usable & (1u << slot)))
            list[n++] = (uint8_t)slot;
    return n;
}

/* Of those listed, the farthest */
static uint8_t farthest_listed(const Monster *self, const uint8_t *list, int n) {
    const float *values = &self->distance_to_hunter0;
    uint8_t best = list[0];
    for (int i = 1; i < n; i++)
        if (values[best] < values[list[i]])
            best = list[i];
    return best;
}

/* game_task 09ABA4B0, opcode 0x53: chooses which hunter to go for, among those tracked, live and in its area. Those with 50000 points or more first, then 30000 or more: alone, the one,
 * or if more the farthest. Otherwise the one with most points; if all have none (or the first has none and none has more), at random
 * by script_random_counter. Nobody: chosen_slot 0xFF. Points are target_points_hunter0.. */
void hand_monster_script_op_53_choose_hunter_slot_by_field_554_tiers(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5];
    uint8_t usable = 0;
    if (self->hunters_tracked_mask != 0) {
        for (uint32_t slot = 0; slot < 4; slot++) {
            uint32_t hunter = hunter_of_slot(c, slot);
            if (hunter != 0 && (self->hunters_tracked_mask & (1u << slot)) && self->area_id == OBJ(Monster, hunter)->area_id && (LW(hunter + 4) & 1))
                usable |= (uint8_t)(1u << slot);
        }
    }
    uint8_t list[4], chosen = 0xFF;
    int n;
    if (usable == 0) {
        chosen = 0xFF;
    } else if ((n = hunters_with_points(self, usable, POINTS_TOP_TIER, list)) != 0) {
        chosen = n == 1 ? list[0] : farthest_listed(self, list, n);
    } else if ((n = hunters_with_points(self, usable, POINTS_SECOND_TIER, list)) != 0) {
        chosen = n == 1 ? list[0] : farthest_listed(self, list, n);
    } else {
        n = 0;
        for (uint32_t slot = 0; slot < 4; slot++)
            if (usable & (1u << slot))
                list[n++] = (uint8_t)slot;
        const int32_t *points = &self->target_points_hunter0;
        chosen = list[0];
        if (n != 1) {
            int nobody_has_more = points[chosen] == 0;
            for (int i = 1; i < n; i++)
                if (points[chosen] < points[list[i]]) {
                    chosen = list[i];
                    nobody_has_more = 0;
                }
            if (nobody_has_more)
                chosen = list[self->script_random_counter % n];
        }
    }
    self->chosen_slot = chosen;
    c->r[2] = at;
}

/* game_task 09ABB8B8, opcode 0x54: if the chosen hunter is live and in the monster's area */
void hand_monster_script_op_54_if_chosen_hunter_active_same_field_29a(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (if_not_the_test(c, self_addr, at, OP_IF_CHOSEN_HUNTER_HERE))
        return;
    int here = 0;
    uint8_t chosen = self->chosen_slot;
    if (chosen != 0xFF) {
        uint32_t hunter = hunter_of_slot(c, chosen);
        here = hunter != 0 && self->area_id == OBJ(Monster, hunter)->area_id && (LW(hunter + 4) & 1);
    }
    if_test_answer(c, self_addr, at + 1, OP_IF_CHOSEN_HUNTER_HERE, here);
}

/* game_task 09ABED10, opcode 0x1B: "1B 0 value": if script_event_active is the value */
void hand_monster_script_op_1b_if_field_544_equals(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (if_not_the_test(c, self_addr, at, OP_IF_EVENT))
        return;
    uint8_t value = LBU(at + 1);
    if_test_answer(c, self_addr, at + 2, OP_IF_EVENT, value == self->script_event_active);
}

/* game_task 09ABA888, opcode 0x40: "40 0" ends alert, "40 n" begins it, with the value from the monster's type. */
void hand_monster_script_op_40_set_mode_4b9_zero_or_one(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (LBU(at) == 0)
        call3(c, set_alert_mode, self_addr, 0, 0);
    else
        call3(c, set_alert_mode, self_addr, 1, (uint32_t)(int32_t)LH(MONSTER_TYPE(self->subtype, MONSTER_TYPE_ALERT_ARG)));
    c->r[2] = at + 1;
}

/* game_task 09ABB040, opcode 0x2C: a switch on the monster's type, by a list test: 09ACC450 given its type and the case's byte. */
void hand_monster_script_op_2c_switch_on_field_1e8_group(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (switch_not_the_first_case(c, self_addr, at, OP_SWITCH_TYPE_GROUP, 1))
        return;
    int32_t count = LBU(at + 1);
    at += 4;
    for (int32_t i = 0; i < count; i++) {
        uint8_t value = LBU(at);
        at++;
        if ((call3(c, type_in_group, self_addr, self->subtype, value) & 0xFF) != 0 || !to_next_case(c, self_addr, &at, OP_SWITCH_TYPE_GROUP))
            break;
    }
    c->r[2] = at;
}

/* game_task 09ABB188, opcode 0x27: a switch on spawn_variant, cases in order */
void hand_monster_script_op_27_switch_on_field_4ae_sorted(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (switch_not_the_first_case(c, self_addr, at, OP_SWITCH_VARIANT, 1))
        return;
    c->r[2] = sorted_cases(c, self_addr, at + 4, OP_SWITCH_VARIANT, LBU(at + 1), (int16_t)self->spawn_variant);
}

/* game_task 09AC3798, opcode 0x79: "79 0 count key .. ..": a switch on what the monster's method +0xDC says of the key, cases in order. */
void hand_monster_script_op_79_switch_on_vfunc_dc(Ctx *c) {
    uint32_t self_addr = c->r[4], at = c->r[5];
    if (switch_not_the_first_case(c, self_addr, at, OP_SWITCH_METHOD_DC, 1))
        return;
    uint8_t count = LBU(at + 1);
    int32_t value = call_method1(c, self_addr, 0xDC, LBU(at + 2)) & 0xFF;
    c->r[2] = sorted_cases(c, self_addr, at + 5, OP_SWITCH_METHOD_DC, count, value);
}
