/* Opcode 0xFF of the monster script: how a block ends. Rewritten by hand from game_task 09AB6788, the generated code still behind it (MHFU_HAND=0).
 * Its sub-code says which: back to where a nested table entry was called from, the next step of a walk over link points or over a table of points,
 * the end of an id sequence, or the end of an interrupt. Each returns where the script goes on. */
#include <stddef.h>

#include "hand.h"
#include "hit.h"
#include "names.h"
#include "monster_script.h"

#define main_entry f_game_task_09ABAB90
#define set_restart_sync f_game_task_09AC9BC8
#define saved_position f_game_task_09AB7490
#define table_08_entry f_game_task_09ABDB28
#define table_30_entry f_game_task_09AC33C8
#define point_order_list f_game_task_09AC33F8
#define area_table_find_entry f_game_task_09AD1CB8
#define resolve_target_position f_game_task_09AB9170
#define set_alert_mode f_game_task_09AC93B0
#define set_type_block_644 f_game_task_09AC7478
#define FLOOR_QUERY 0x09C3EE98u
#define FLOOR_QUERY_CONTEXT 0x09D1A160u
#define FLOOR_QUERY_RESULT_290 0x290u

#define MONSTER_TYPE_AREA_TABLES 0x158          /* -> a structure, +0x18 of it the table of areas for map_area_table_find_entry */
#define TARGET_KIND_LINK_POINT 9
#define MONSTER_FLAG_LINK_MOVES 2
#define MONSTER_FLAG_200 0x200                  /* cleared at each round of an id sequence */
#define MAX_LINK_RETRIES 10
#define POINT_REACHED_RADIUS 500.0f
#define POINT_REACHED_RADIUS_WIDE 1000.0f

/* The stack, as the game's: a callee gets the address of a position in it, and the list of other points goes on it */
#define FRAME 0xA0u
#define F_TARGET_POSITION 0x60u
#define F_OTHER_POINTS 0x94u

enum { END_TO_MAIN = 0, END_RETURN_LEVEL1 = 1, END_RETURN_LEVEL2 = 2, END_RETURN_TABLE_24 = 3, END_LINK_MOVE = 4, END_LINK_STEP = 5,
       END_MAIN_F5 = 0xF5, END_MAIN_F6 = 0xF6, END_MAIN_CLEAR_SLOT = 0xF7, END_REQUEST_08 = 0xF8, END_REQUEST_04 = 0xF9, END_REQUEST_02 = 0xFA,
       END_ID_SEQUENCE = 0xFB, END_ALERT = 0xFC, END_REQUEST_10 = 0xFD, END_POINT_STEP = 0xFE, END_POINT_BLOCK = 0xFF };
enum { POINT_ORDER_RANDOM_OTHER = 0, POINT_ORDER_FROM_LIST = 1, POINT_ORDER_NEXT = 2 };

/* Link records: per area, 0x18 bytes each */
#define LINK_RECORD(records, index) ((records) + (uint32_t)(index) * 0x18u)
enum { LINK_TO_AREA = 0, LINK_POINT_COUNT = 2, LINK_POINTS = 4, LINK_ARRIVE_PLACE = 8, LINK_ARRIVE_FACING = 0x14 };
/* A link's points: 0x14 bytes each */
#define LINK_POINT(points, index) ((points) + (uint32_t)(index) * 0x14u)
enum { POINT_RADIUS = 0xC, POINT_SCRIPT_ENTRY = 0x12 };

/* The script from main entry 0. Sub-codes 0, F5, F6 do just this. */
static uint32_t to_main_entry_0(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    self->script_main_entry = 0;
    uint32_t at = call1(c, main_entry, self_addr);
    self->script_active_requests = 0;
    call2(c, set_restart_sync, self_addr, 2);
    return at;
}

/* F7: also forgets the hunter that was the target */
static uint32_t to_main_entry_0_forget_chosen_slot(Ctx *c, Monster *self) {
    uint32_t at = to_main_entry_0(c, self), self_addr = GAME_ADDR(self);
    uint8_t chosen = self->chosen_slot;
    if (chosen == 0xFF)
        return at;
    uint32_t slot = chosen & 0xFu;
    self->hunters_tracked_mask &= (uint8_t)~(1u << slot);
    SH(self_addr + offsetof(Monster, tracked_ticks_hunter0) + 2 * slot, 0);
    SW(self_addr + offsetof(Monster, approach_points_hunter0) + 4 * slot, 0);
    SW(self_addr + offsetof(Monster, target_points_hunter0) + 4 * slot, 0);
    return at;
}

/* The end of an interrupt's handling, when something else may be running: the point block goes on if that was running, otherwise main entry 0. */
static uint32_t after_interrupt(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    if (self->script_active_requests & 1)
        return call2(c, table_08_entry, self_addr, self->point_block_step_entry);
    self->script_main_entry = 0;
    uint32_t at = call1(c, main_entry, self_addr);
    call2(c, set_restart_sync, self_addr, 2);
    return at;
}

/* FD: as the others, but marks the restart itself, not going by what the mark was before */
static uint32_t end_request_10(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    self->script_active_requests &= 0xEF;
    if (self->script_active_requests & 1)
        return call2(c, table_08_entry, self_addr, self->point_block_step_entry);
    self->script_main_entry = 0;
    uint32_t at = call1(c, main_entry, self_addr);
    self->script_restart_sync_kind = 2;
    return at;
}

/* FA: main_entry is called first and what it returns isn't used; it does clear the nest level */
static uint32_t end_request_02(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    call1(c, main_entry, self_addr);
    self->script_active_requests &= 0xFD;
    call2(c, set_type_block_644, self_addr, 0);
    return after_interrupt(c, self);
}

/* FB: one more round of the id sequence, from where it loops; at its end it starts again, or is over */
static uint32_t end_id_sequence_round(Monster *self) {
    self->monster_flags &= ~(uint32_t)MONSTER_FLAG_200;
    uint32_t at = self->script_sequence_loop_start;
    self->id_sequence_position++;
    if (self->id_sequence_position < self->id_sequence_length)
        return at;
    if (self->id_sequence_loops != 0) {
        self->id_sequence_position = 0;
    } else {
        self->id_sequence_position = -1;
        self->id_sequence_length = -1;
    }
    return at;
}

/* FC: if this machine runs the monster, alert it with its type's value; then main entry 0 */
static uint32_t end_alert(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    if (self->locally_owned != 0) {
        call3(c, set_alert_mode, self_addr, 1, (uint32_t)(int32_t)LH(MONSTER_TYPE(self->subtype, MONSTER_TYPE_ALERT_ARG)));
        self->alert_mode_copy = self->alert_mode;
    }
    return to_main_entry_0(c, self);
}

/* FF: the point block has gone round: from its start again until enough points are visited, then back to what was running before it */
static uint32_t end_point_block(Ctx *c, Monster *self) {
    uint32_t self_addr = GAME_ADDR(self);
    if (self->point_block_visited < self->point_block_goal)
        return call2(c, table_08_entry, self_addr, self->point_block_start_entry);
    self->script_active_requests &= 0xFE;
    return call1(c, saved_position, self_addr);
}

/* A random point of the area's table other than the current one. The list goes on the stack, for all it may be longer than the room there: as the game does. */
static void choose_random_other_point(Ctx *c, Monster *self, uint32_t sp) {
    int8_t others = 0;
    int8_t current = self->point_block_index;
    for (int8_t i = 0; i < self->point_block_point_count; i++) {
        if (i != current) {
            SB(sp + F_OTHER_POINTS + (uint32_t)(int32_t)others, (uint8_t)i);
            others++;
        }
    }
    if (others == 0)
        self->point_block_index = 0;
    else
        self->point_block_index = (int8_t)LBU(sp + F_OTHER_POINTS + (uint32_t)(int32_t)(int8_t)(self->script_random_counter % others));
}

/* FE: if the target point is reached (within 500 across the ground, 1000 for some types), count it and choose the next. Then on with the block's step. */
static uint32_t end_point_step(Ctx *c, Monster *self, uint32_t sp) {
    uint32_t self_addr = GAME_ADDR(self), target = sp + F_TARGET_POSITION;
    call2(c, resolve_target_position, self_addr, target);
    float there = distance_flat((const float *)MEM(target), self->place);
    float radius;
    switch (self->subtype) {
    case 2: case 7: case 0x24: case 0x32: case 0x37: case 0x47:
        radius = POINT_REACHED_RADIUS_WIDE;
        break;
    default:
        radius = POINT_REACHED_RADIUS;
        break;
    }
    if (there <= radius) {
        self->point_block_visited++;
        switch (self->point_block_order) {
        case POINT_ORDER_NEXT:
            self->point_block_index++;
            break;
        case POINT_ORDER_FROM_LIST: {
            uint32_t list = call2(c, point_order_list, self_addr, self->point_block_order_list);
            self->point_block_index = (int8_t)LBU(list + (uint32_t)(int32_t)self->point_block_visited);
            break;
        }
        case POINT_ORDER_RANDOM_OTHER:
            choose_random_other_point(c, self, sp);
            break;
        }
    }
    return call2(c, table_08_entry, self_addr, self->point_block_step_entry);
}

/* What a link point's script entry and the target say: on towards this point */
static uint32_t go_to_link_point(Ctx *c, Monster *self, uint32_t points) {
    self->target_kind = TARGET_KIND_LINK_POINT;
    self->target_param1 = self->link_record_index;
    self->target_param2 = self->link_point_index;
    self->resolved_target_kind = self->target_kind;
    self->resolved_target_param1 = self->target_param1;
    self->resolved_target_param2 = self->target_param2;
    self->link_script_entry = (uint8_t)LHU(LINK_POINT(points, self->link_point_index) + POINT_SCRIPT_ENTRY);
    return call2(c, table_30_entry, GAME_ADDR(self), self->link_script_entry);
}

/* The script goes back to where the link move was called from; nothing is the target any more */
static uint32_t end_link_walk(Monster *self) {
    uint32_t at = self->script_return_link;
    self->script_return_link = 0;
    self->target_kind = 0;
    self->resolved_target_kind = 0;
    return at;
}

/* Sub-code 4, the last link point reached: it goes through. Place, facing and area are the link's other end, and the floor is found again there. */
static void arrive_through_link(Ctx *c, Monster *self, uint32_t records) {
    uint32_t self_addr = GAME_ADDR(self);
    uint32_t record = LINK_RECORD(records, self->link_record_index);
    for (int i = 0; i < 3; i++)
        SW(self_addr + offsetof(Monster, place) + 4u * (uint32_t)i, LW(record + LINK_ARRIVE_PLACE + 4u * (uint32_t)i));
    for (int i = 0; i < 3; i++)
        SW(self_addr + offsetof(Monster, place_before) + 4u * (uint32_t)i, LW(self_addr + offsetof(Monster, place) + 4u * (uint32_t)i));
    self->facing = LHU(LINK_RECORD(records, self->resolved_target_param1) + LINK_ARRIVE_FACING);
    self->previous_area = self->area_id;
    self->area_id = area_by_remap_flag(c, LHU(LINK_RECORD(records, self->resolved_target_param1) + LINK_TO_AREA));
    call_method0(c, self_addr, 0x48);
    call2(c, set_restart_sync, self_addr, 1);
    c->r[4] = LW(FLOOR_QUERY_CONTEXT);
    c->r[5] = self_addr;
    c->r[6] = self_addr + offsetof(Monster, place);
    c->r[7] = self_addr + FLOOR_QUERY_RESULT_290;
    c->r[8] = self_addr + offsetof(Monster, floor_y);
    c->r[9] = self_addr + offsetof(Monster, surface_height);
    rt_call(c, FLOOR_QUERY);
}

/* Sub-codes 4 and 5: a walk along the points of a link record of the area, towards point 0. The monster is sent to the point, and when near enough to the next
 * one on; after MAX_LINK_RETRIES tries at one it goes straight for point 0. At point 0 the walk ends; with sub-code 4, and the flag, the monster also goes through. */
static uint32_t end_link_walk_step(Ctx *c, Monster *self, uint32_t script_byte) {
    uint32_t self_addr = GAME_ADDR(self);
    uint32_t type_tables = LW(MONSTER_TYPE(self->subtype, MONSTER_TYPE_AREA_TABLES));
    uint32_t area = call3(c, area_table_find_entry, self_addr, LW(type_tables + 0x18), (uint32_t)(int32_t)(int16_t)self->area_id);
    uint32_t records = LW(area + 4);
    if (records == 0)
        return end_link_walk(self);
    int32_t record_count = LH(area + 2);
    if ((int32_t)self->link_record_index >= record_count)
        self->link_record_index = (uint8_t)(record_count - 1);
    uint32_t record = LINK_RECORD(records, self->link_record_index);
    int32_t point_count = LH(records + LINK_POINT_COUNT);       /* of the first record, whichever this is: as the game has it */
    uint32_t points = LW(record + LINK_POINTS);
    if ((int32_t)self->link_point_index >= point_count)
        self->link_point_index = (uint8_t)(point_count - 1);
    if (self->locally_owned == 0) {     /* a remote copy only follows */
        if (self->resolved_target_kind == TARGET_KIND_LINK_POINT) {
            self->link_script_entry = (uint8_t)LHU(LINK_POINT(points, self->link_point_index) + POINT_SCRIPT_ENTRY);
            return call2(c, table_30_entry, self_addr, self->link_script_entry);
        }
        self->script_main_entry = 0;
        return call1(c, main_entry, self_addr);
    }
    uint32_t point = LINK_POINT(points, self->link_point_index);
    const float *where = (const float *)MEM(point);
    float there = self->subtype == 0x3A || self->subtype == 0x48 ? distance_3d(self->place, where) : distance_flat(self->place, where);
    if (there > *(const float *)MEM(LINK_POINT(points, self->link_point_index) + POINT_RADIUS)) {
        self->link_retry_count++;
        if (self->link_retry_count < MAX_LINK_RETRIES)
            return go_to_link_point(c, self, points);
        if (self->link_point_index != 0) {
            self->link_point_index = 0;
            self->link_retry_count = 0;
            return go_to_link_point(c, self, points);
        }
    } else if (self->link_point_index != 0) {
        self->link_retry_count = 0;
        self->link_point_index--;
        return go_to_link_point(c, self, points);
    }
    /* At point 0, or given up there */
    if (self->resolved_target_kind == TARGET_KIND_LINK_POINT && LBU(script_byte) == END_LINK_MOVE && LH(LINK_RECORD(records, self->resolved_target_param1) + LINK_TO_AREA) != -1 &&
        (self->monster_flags & MONSTER_FLAG_LINK_MOVES) != 0)
        arrive_through_link(c, self, records);
    return end_link_walk(self);
}

/* game_task 09AB6788, opcode 0xFF. a1 is the byte after the opcode. A sub-code that isn't listed: goes on from that byte. */
void hand_monster_script_op_ff_end_block(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t at = c->r[5];
    uint32_t sp = c->r[29] - FRAME;
    c->r[29] = sp;
    uint8_t sub = LBU(at);
    switch (sub) {
    case END_TO_MAIN:
    case END_MAIN_F5:
    case END_MAIN_F6:
        at = to_main_entry_0(c, self);
        break;
    case END_MAIN_CLEAR_SLOT:
        at = to_main_entry_0_forget_chosen_slot(c, self);
        break;
    case END_RETURN_LEVEL1:
        self->script_nest_level = 0;
        at = self->script_return_level1;
        break;
    case END_RETURN_LEVEL2:
        self->script_nest_level = 1;
        at = self->script_return_level2;
        break;
    case END_RETURN_TABLE_24:
        at = self->script_return_table_24;
        break;
    case END_LINK_MOVE:
    case END_LINK_STEP:
        at = end_link_walk_step(c, self, at);
        break;
    case END_REQUEST_08:
        self->script_active_requests &= 0xF7;
        at = after_interrupt(c, self);
        break;
    case END_REQUEST_04:
        self->script_active_requests &= 0xFB;
        at = after_interrupt(c, self);
        break;
    case END_REQUEST_02:
        at = end_request_02(c, self);
        break;
    case END_ID_SEQUENCE:
        at = end_id_sequence_round(self);
        break;
    case END_ALERT:
        at = end_alert(c, self);
        break;
    case END_REQUEST_10:
        at = end_request_10(c, self);
        break;
    case END_POINT_STEP:
        at = end_point_step(c, self, sp);
        break;
    case END_POINT_BLOCK:
        at = end_point_block(c, self);
        break;
    default:
        break;
    }
    c->r[29] = sp + FRAME;
    c->r[2] = at;
}
