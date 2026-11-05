/* The monster script interpreter: how a monster's script picks and runs its actions. Rewritten by hand from game_task 09AB4C68 and the functions
 * below it, the generated code still behind them (MHFU_HAND=0). The opcodes' handlers (their names below) are generated, each monster's own states too.
 *
 * A script is bytes. Each instruction is an opcode byte, then its own bytes; the handler is given the byte after the opcode and returns where to go on.
 * script_header (+0x4C8) holds tables of entry points by number; script_main_entry says which to begin from. Three things make up what a script does:
 *   - Run: monster_script_run below, each time the monster's state asks (script_run_requested, then its tick): from wherever it left off, until opcode 5,
 *     which resolves the target (+0x5D0), starts its action by the monster's vtable +0x88 (game_task 09AC8AF0), saves the position, and stops. So one call
 *     of this picks one action; the monster's own state functions run it.
 *   - Blocks: many opcodes come in threes, "op 0 ..test..", "op 1", "op 2": if, else, end if (to_else_or_end and opcodes 1, 0xB, 0x39 below). A weighted choice
 *     (opcode 0x80 below) picks among branches by script_random_counter. Skipping forward uses a table of each instruction's length
 *     (next_instruction, game_task 09AB7858, still generated).
 *   - Interrupts: other code sets a bit of script_requests; the next run takes the entry for it instead of going on (take_pending_entry). Who sets each, all only if
 *     09AC9740 doesn't block it (a running interrupt of that bit or above):
 *       0x02  09AC5C30, for some types, not alert (or alert in types 0x34, 0x4B, 0x51, 0x55): 09AD21E8 finds a point of list +0x88 at 09C122B0 near, and the check 09C3E3D0 passes
 *       0x04  in some classes' ticks (09AF7980, 09AFEB90, 09B07530, 09B153D8, 09B400E8): mostly not alert, 09AC77C8 finds a point of the list at 09C122B0 near that its type answers to
 *       0x08  09AD24D0: the boost begins, attack and damage scaled up, from boost-meter-full (monster_flags 0x400)
 *       0x10  09ACCB18: a hunter in sight range has approach points over the threshold, when not alert
 *       0x20  09AC5C30: not alert, and a hunter is tracked (seen, hunters_tracked_mask); never for types 4, 9
 *       0x40  09AC5C30: not alert, 09AD25A0 finds a point of list +0x108 at 09C122B0 near, with flag 2
 *       0x80  09AD3228, 09AD51E8: rejoin_restart_ticks out, in the game's area; 09B14F18, type 0x50: 5000 or more from the area's point
 *     None of these tests being hit or health itself: reactions to hits go through the code from 09AC7A08 into states. What the lists at 09C122B0 hold isn't worked out.
 *     Each has a saved position of its own, and script_active_requests says which is running, so that the main script can go on after.
 *
 * Opcodes' names are from reading their handlers: a test of a field, or a target chosen. What the game calls them isn't known. */
#include <stddef.h>

#include "hand.h"
#include "names.h"
#include "monster_script.h"

/* Named here by address as function names may change: */
#define on_byte f_game_task_09AB6778                /* does nothing with the byte it's given: called all the same */
#define no_op f_game_task_09AB6770
#define main_entry f_game_task_09ABAB90
#define restart_if_flagged f_game_task_09AC8B78
#define restart_script f_game_task_09ABAFF0
#define take_pending_entry f_game_task_09AB7230
#define saved_position f_game_task_09AB7490
#define save_position f_game_task_09AB74D0
#define request_blocked f_game_task_09AC9740
#define set_restart_sync f_game_task_09AC9BC8
#define choose_action f_game_task_09AC8AF0
#define op_7b_action f_game_task_09AC4518
#define op_31 f_game_task_09ABCC30
#define op_92 f_game_task_09AC3210

/* Opcodes run here or tested */
enum { OP_IF_ALERT_MODE = 0xB, OP_IF_4C4 = 0x39, OP_IF_HUNTERS_IN_RANGE = 1, OP_WEIGHTED = 0x80, OP_CHOOSE_ACTION = 5, OP_MAIN_ENTRY_RESET = 4, OP_ACTION_7B = 0x7B, OP_ACTION_84 = 0x84, OP_31 = 0x31, OP_92 = 0x92, OP_END_OF_SCRIPT = 0x95 };
enum { OP_SET_TARGET_SPEC = 6, OP_3E = 0x3E, OP_SWITCH_DISTANCE = 0x83, OP_85 = 0x85 };
#define WEIGHTED_ENTRY_END 0xFF
#define MAX_STEPS 1000          /* in a run, and in a skip */
#define STATE_GROUP_NO_SCRIPT_A 5
#define STATE_GROUP_NO_SCRIPT_B 6

/* Where each interrupt's handling begins, in the order they're looked at. */
#define REQUEST_MAIN_40 0x40
#define REQUEST_MAIN_80 0x80
#define ACTIVE_POINT_BLOCK 1
#define MONSTER_FLAG_SCRIPT_RESTART 0x10

/* -- the entry points --------------------------------------------------------------------------------------------------------------------- */

/* game_task 09ABAB90: where script_main_entry begins, at nest level 0; 0 if the monster has no script */
void hand_monster_script_main_entry_clear_nest_level(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    self->script_nest_level = 0;
    c->r[2] = self->script_header == 0 ? 0 : LW(LW(self->script_header) + 4u * self->script_main_entry);
}

/* game_task 09AB7510, opcode 7: a jump to a main entry */
void hand_monster_script_op_07_jump_main_entry(Ctx *c) {
    OBJ(Monster, c->r[4])->script_main_entry = LBU(c->r[5]);
    main_entry(c);
}

/* game_task 09AC9BC8: a mark for what a remote machine is told of a restart. 1 wins over 2. */
void hand_entity_set_script_restart_sync(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint8_t kind = (uint8_t)c->r[5];
    if (kind == 1 || (kind == 2 && self->script_restart_sync_kind != 1))
        self->script_restart_sync_kind = (uint8_t)c->r[5];
}

/* game_task 09AC8B78: back to the start of main entry 0, if a restart is flagged */
void hand_entity_script_restart_if_flagged(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    if (!(self->monster_flags & MONSTER_FLAG_SCRIPT_RESTART))
        return;
    self->script_run_requested = 0;
    self->monster_flags &= ~(uint32_t)MONSTER_FLAG_SCRIPT_RESTART;
    self->script_active_requests = 0;
    self->script_delay_ticks = 0;
    self->script_main_entry = 0;
    self->script_position = call1(c, main_entry, GAME_ADDR(self));
    self->point_block_index = -1;
    self->point_block_goal = -1;
    self->id_sequence_position = -1;
    self->id_sequence_length = -1;
    self->route_final_area = 0xFFFF;
    self->previous_area = 0xFFFF;
}

/* game_task 09ABAFF0: from anywhere, a restart */
void hand_monster_script_restart_from_main_entry(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    if (self->locally_owned != 0 && self->script_restart_sync_kind == 0)
        call2(c, set_restart_sync, GAME_ADDR(self), 2);
    self->monster_flags |= MONSTER_FLAG_SCRIPT_RESTART;
    call1(c, restart_if_flagged, GAME_ADDR(self));
}

/* -- where the script is ------------------------------------------------------------------------------------------------------------------ */

/* The position for what's running: an interrupt's, the point block's, or the main script's. */
static uint32_t *position_of(Monster *self) {
    uint8_t active = self->script_active_requests;
    if (active & 0x20)
        return &self->script_position_request_20;
    if (active & 0x10)
        return &self->script_position_request_10;
    if (active & ACTIVE_POINT_BLOCK)
        return &self->script_position_point_block;
    return &self->script_position;
}

/* game_task 09AB7490 */
void hand_monster_script_get_saved_position(Ctx *c) {
    c->r[2] = *position_of(OBJ(Monster, c->r[4]));
}

/* game_task 09AB74D0 */
void hand_monster_script_set_saved_position(Ctx *c) {
    *position_of(OBJ(Monster, c->r[4])) = c->r[5];
}

/* An interrupt that has a position of its own, when requests come in: the bits of script_requests it leaves */
static const struct { uint8_t bit; void (*entry)(Ctx *); uint16_t position; uint8_t requests_kept; } interrupts[] = {
    {0x20, f_game_task_09ABDB88, offsetof(Monster, script_position_request_20), 0xCE},
    {0x10, f_game_task_09ABDB58, offsetof(Monster, script_position_request_10), 0xEF},
    {0x08, f_game_task_09AC3468, offsetof(Monster, script_position_request_08), 0xF7},
    {0x04, f_game_task_09AC3498, offsetof(Monster, script_position_request_04), 0xC8},
    {0x02, f_game_task_09AC34C8, offsetof(Monster, script_position_request_02), 0xCC},
};

/* game_task 09AB7230: if an interrupt is requested and allowed, where its handling begins. The first takes the rest with it, in this order. */
void hand_monster_script_take_pending_entry(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], next = 0;
    if (self->script_requests == 0) {
        c->r[2] = 0;
        return;
    }
    /* Two that have only the main position, and clear all the others */
    if ((self->script_requests & REQUEST_MAIN_40) && call2(c, request_blocked, self_addr, REQUEST_MAIN_40) == 0) {
        next = call1(c, f_game_task_09AC3448, self_addr);
        self->script_requests = 0;
        self->script_active_requests = REQUEST_MAIN_40;
    }
    if ((self->script_requests & REQUEST_MAIN_80) && call2(c, request_blocked, self_addr, REQUEST_MAIN_80) == 0 && next == 0) {
        next = call1(c, f_game_task_09AC3428, self_addr);
        self->script_requests = 0;
        self->script_active_requests = REQUEST_MAIN_80;
    }
    for (size_t i = 0; i < sizeof interrupts / sizeof interrupts[0]; i++) {
        if ((self->script_requests & interrupts[i].bit) && call2(c, request_blocked, self_addr, interrupts[i].bit) == 0) {
            uint32_t entry = call2(c, interrupts[i].entry, self_addr, 0);
            SW(self_addr + interrupts[i].position, entry);
            if (next == 0)
                next = LW(self_addr + interrupts[i].position);
            self->script_requests &= interrupts[i].requests_kept;
            self->script_active_requests |= interrupts[i].bit;
        }
    }
    c->r[2] = next;
}

/* -- skipping forward --------------------------------------------------------------------------------------------------------------------- */

/* An instruction's length in bytes, by opcode and by the byte after it: for that byte being 0, 1, 2, anything else. Used only for skipping forward; a
 * handler that runs moves on by itself. "If" opcodes: "op 0 <test bytes>", "op 1", "op 2". Switches: "op 0 ..", "op 1 ..". A length of 0 is what an opcode
 * not listed has, 0 and 0x95 among them: a skip stepping onto one stays where it is. */
typedef struct { uint8_t sub0, sub1, sub2, other; } InstructionLength;
#define FIXED(n) {n, n, n, n}
#define IF(test) {test, 2, 2, 1}
#define SWITCH(first, second) {first, second, 2, 2}
static const InstructionLength instruction_lengths[256] = {
    [0x01] = FIXED(2), [0x02] = FIXED(2), [0x03] = FIXED(2), [0x04] = FIXED(1), [0x05] = FIXED(4), [0x07] = FIXED(2),
    [0x08] = FIXED(2), [0x09] = FIXED(2), [0x0A] = FIXED(2), [0x0B] = IF(3), [0x0C] = FIXED(3), [0x0D] = FIXED(2),
    [0x0E] = IF(4), [0x0F] = FIXED(6), [0x10] = FIXED(1), [0x11] = FIXED(1), [0x12] = FIXED(1), [0x13] = FIXED(1),
    [0x14] = IF(3), [0x15] = SWITCH(7, 4), [0x16] = FIXED(2), [0x17] = FIXED(5), [0x18] = FIXED(1), [0x19] = FIXED(1),
    [0x1A] = FIXED(3), [0x1B] = IF(3), [0x1C] = SWITCH(6, 3), [0x1D] = SWITCH(6, 3), [0x1E] = FIXED(1), [0x1F] = FIXED(2),
    [0x20] = SWITCH(6, 3), [0x21] = IF(2), [0x22] = IF(3), [0x24] = {3, 2, 1, 1}, [0x25] = FIXED(1), [0x26] = FIXED(2),
    [0x27] = SWITCH(6, 3), [0x28] = FIXED(2), [0x29] = FIXED(2), [0x2A] = FIXED(2), [0x2B] = IF(4), [0x2C] = SWITCH(6, 3),
    [0x2D] = FIXED(1), [0x2E] = FIXED(2), [0x2F] = FIXED(2), [0x30] = FIXED(1), [0x31] = FIXED(1), [0x32] = IF(3),
    [0x33] = SWITCH(6, 3), [0x34] = IF(4), [0x35] = FIXED(2), [0x36] = IF(3), [0x38] = FIXED(2), [0x39] = FIXED(2),
    [0x3A] = FIXED(2), [0x3B] = FIXED(2), [0x3C] = FIXED(2), [0x3D] = IF(4), [0x3E] = {7, 4, 2, 1}, [0x3F] = FIXED(4),
    [0x40] = FIXED(2), [0x41] = FIXED(2), [0x42] = IF(3), [0x43] = FIXED(2), [0x44] = FIXED(2), [0x45] = FIXED(2),
    [0x46] = IF(4), [0x47] = FIXED(2), [0x48] = FIXED(2), [0x49] = FIXED(2), [0x4A] = FIXED(2), [0x4B] = FIXED(1),
    [0x4C] = FIXED(1), [0x4D] = FIXED(1), [0x4E] = FIXED(2), [0x4F] = FIXED(2), [0x50] = FIXED(2), [0x51] = FIXED(2),
    [0x52] = FIXED(1), [0x53] = FIXED(1), [0x54] = FIXED(2), [0x55] = FIXED(2), [0x56] = SWITCH(6, 3), [0x57] = SWITCH(6, 3),
    [0x58] = FIXED(1), [0x59] = FIXED(2), [0x5A] = IF(3), [0x5B] = FIXED(2), [0x5C] = FIXED(2), [0x5D] = FIXED(2),
    [0x5E] = IF(3), [0x5F] = FIXED(1), [0x60] = FIXED(2), [0x61] = FIXED(1), [0x62] = FIXED(2), [0x63] = IF(3),
    [0x64] = IF(3), [0x65] = FIXED(1), [0x66] = FIXED(2), [0x67] = FIXED(2), [0x68] = FIXED(1), [0x69] = IF(3),
    [0x70] = SWITCH(6, 3), [0x71] = IF(3), [0x75] = SWITCH(6, 3), [0x76] = SWITCH(6, 3), [0x77] = FIXED(2), [0x78] = IF(4),
    [0x79] = SWITCH(7, 3), [0x7A] = SWITCH(6, 3), [0x7B] = FIXED(1), [0x7C] = FIXED(2), [0x7D] = FIXED(2), [0x7E] = SWITCH(6, 3),
    [0x81] = FIXED(2), [0x82] = FIXED(3), [0x84] = FIXED(1), [0x86] = FIXED(3), [0x87] = IF(4), [0x90] = FIXED(2),
    [0x91] = FIXED(2), [0x92] = FIXED(1), [0x93] = FIXED(1), [0x94] = SWITCH(5, 3), [0x99] = FIXED(2), [0xFF] = FIXED(2),
};

/* The few that don't fit the table: the length goes by a range of sub-codes, or a further one */
static uint32_t instruction_length(uint8_t op, uint8_t sub) {
    switch (op) {
    case OP_SET_TARGET_SPEC:
        return sub == 3 ? 5 : 4;
    case OP_WEIGHTED:
    case OP_85:
        if (sub == 0)
            return op == OP_WEIGHTED ? 6 : 7;
        if (sub >= 1 && sub <= 0xD)
            return 3;
        return sub == WEIGHTED_ENTRY_END ? 2 : 1;
    case OP_SWITCH_DISTANCE:
        if (sub == 0)
            return 5;
        return (sub >= 1 && sub <= 5) || sub == 0xFF ? 2 : 1;
    case OP_3E:
        if (sub == 3)
            return 2;
        break;
    }
    const InstructionLength *length = &instruction_lengths[op];
    return sub == 0 ? length->sub0 : sub == 1 ? length->sub1 : sub == 2 ? length->sub2 : length->other;
}

/* game_task 09AB7858: where the instruction at `at` ends. A wrong length here would misread a whole script: handtest checks every opcode with every second byte. */
void hand_monster_script_next_instruction(Ctx *c) {
    uint32_t self = c->r[4], at = c->r[5];
    call2(c, on_byte, self, at);
    if (LBU(at) != 0)
        call2(c, on_byte, self, at + 1);
    c->r[2] = at + instruction_length(LBU(at), LBU(at + 1));
}

/* game_task 09AB7728: forward to the next `op` that isn't its own block's start: blocks of the same op, in between, are stepped over whole.
 * If it goes on too long, the script starts over from its main entry 0. */
void hand_monster_script_skip_to_opcode_subcode(Ctx *c) {
    uint32_t self = c->r[4], at = c->r[5], op_arg = c->r[6], sub_arg = c->r[7];
    uint8_t op = (uint8_t)op_arg, sub = (uint8_t)sub_arg;
    int16_t steps = 0;
    Monster *monster = OBJ(Monster, self);
    for (;;) {
        if (LBU(at) == OP_END_OF_SCRIPT) {
            c->r[2] = at;
            return;
        }
        while (LBU(at) != op) {
            at = next_of(c, self, at);
            steps++;
            if (steps >= MAX_STEPS) {
                monster->script_main_entry = 0;
                at = call1(c, main_entry, self);
            }
        }
        if (LBU(at + 1) != 0) {
            c->r[2] = at;
            return;
        }
        /* A block of the same op opens: to its end */
        at = next_of(c, self, at);
        for (;;) {
            uint8_t here = LBU(at);
            if (here == op && LBU(at + 1) == sub)
                break;
            if (here == op)
                at = next_of(c, self, at);
            at = call4(c, skip_to, self, at, op_arg, sub_arg);
        }
        at = next_of(c, self, at);
    }
}

/* game_task 09AB8A38: an "else" reached by running into it: past its end */
void hand_monster_script_skip_past_block_end(Ctx *c) {
    uint32_t self = c->r[4], at = c->r[5], op_arg = c->r[6];
    uint8_t op = (uint8_t)op_arg;
    while (LBU(at) != op || LBU(at + 1) != IF_END)
        at = call4(c, skip_to, self, at, op_arg, IF_END);
    c->r[2] = next_of(c, self, at);
}

/* -- blocks ----------------------------------------------------------------------------------------------------------------------------------- */

/* game_task 09AB7620, opcode 0xB: if alert_mode is the byte that follows */
void hand_monster_script_op_0b_if_field_4b9_equals(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    at++;
    if (sub == IF_END) {
        c->r[2] = at;
    } else if (sub == IF_ELSE) {
        c->r[2] = call3(c, skip_past_end, self_addr, at, OP_IF_ALERT_MODE);
    } else if (sub == IF_TEST) {
        uint8_t wanted = LBU(at);
        self->alert_mode_copy = self->alert_mode;
        uint8_t mode = self->alert_mode;
        at++;
        c->r[2] = mode == wanted ? at : to_else_or_end(c, self_addr, at, OP_IF_ALERT_MODE);
    } else {
        c->r[2] = at;
    }
}

/* game_task 09AB7520, opcode 0x39: if end_vfunc_ec_ticks is running */
void hand_monster_script_op_39_if_field_4c4_nonzero(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    at++;
    if (sub == IF_END) {
        c->r[2] = at;
    } else if (sub == IF_ELSE) {
        c->r[2] = call3(c, skip_past_end, self_addr, at, OP_IF_4C4);
    } else if (sub == IF_TEST) {
        c->r[2] = self->end_vfunc_ec_ticks != 0 ? at : to_else_or_end(c, self_addr, at, OP_IF_4C4);
    } else {
        c->r[2] = at;
    }
}

/* game_task 09ABA218, opcode 1: if any of the first four hunters is in range */
void hand_monster_script_op_01_if_field_6d8_low_bits_any(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    if (sub == IF_END) {
        c->r[2] = at + 1;
    } else if (sub == IF_ELSE) {
        c->r[2] = call3(c, skip_past_end, self_addr, at + 1, OP_IF_HUNTERS_IN_RANGE);
    } else if (sub == IF_TEST) {
        at++;
        c->r[2] = (self->hunters_in_range_mask & 0xF) != 0 ? at : to_else_or_end(c, self_addr, at, OP_IF_HUNTERS_IN_RANGE);
    } else {
        c->r[2] = at;       /* unlike the others: the byte isn't passed */
    }
}

/* -- the choice ---------------------------------------------------------------------------------------------------------------------------------- */

/* Past the rest of a weighted choice, from a branch that has run: entries until the end one */
static uint32_t past_weighted_entries(Ctx *c, uint32_t self, uint32_t at) {
    for (;;) {
        uint32_t entry = call4(c, skip_to, self, at, OP_WEIGHTED, 0xFF);
        if (LBU(entry + 1) == WEIGHTED_ENTRY_END)
            return entry + 2;
        at = entry + 3;
    }
}

/* game_task 09AB8AE0, opcode 0x80, how a script picks among actions at random. "0x80 0 count", then entries "0x80 n weight" each followed by its branch, then "0x80 0xFF".
 * The number is script_random_counter & 0x1F: the first entry whose running sum of weights is above it. To change how often a monster does something: its weights. */
void hand_monster_script_op_80_weighted_branch(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4], at = c->r[5];
    uint8_t sub = LBU(at);
    if (sub == WEIGHTED_ENTRY_END) {
        c->r[2] = at + 1;
    } else if (sub >= 1 && sub <= 0xD) {
        c->r[2] = past_weighted_entries(c, self_addr, at + 2);
    } else if (sub == 0) {
        int32_t count = LBU(at + 1), number = self->script_random_counter & 0x1F;
        int32_t weights = 0;
        at += 2;
        for (int32_t i = 0; i < count; i = (i + 1) & 0xFFFF) {
            uint32_t entry = call4(c, skip_to, self_addr, at, OP_WEIGHTED, 0xFF);
            uint8_t weight = LBU(entry + 2);
            at = entry + 3;
            if (weight == 0)
                continue;
            if (weight == 0xFF)
                break;
            weights = (weights + weight) & 0xFF;
            if (number < weights)
                break;
        }
        c->r[2] = at;
    } else {
        c->r[2] = at;
    }
}

/* -- the run -------------------------------------------------------------------------------------------------------------------------------------- */

/* Each opcode's handler: given a0 the monster, a1 the next byte; returns where to go on. Not listed: those handled in the run, and an unknown one restarts. */
static void (*const handlers[256])(Ctx *) = {
    [0x01] = f_game_task_09ABA218,     /* if_field_6d8_low_bits_any */
    [0x02] = f_game_task_09ABA340,     /* if_slot_mask_503_nonzero */
    [0x03] = f_game_task_09ABCE90,
    [0x06] = f_game_task_09AB8E48,     /* set_target_spec */
    [0x07] = f_game_task_09AB7510,     /* jump_main_entry */
    [0x08] = f_game_task_09ABD008,     /* if_field_280_zero */
    [0x09] = f_game_task_09ABD208,
    [0x0A] = f_game_task_09ABD308,
    [0x0B] = f_game_task_09AB7620,     /* if_field_4b9_equals */
    [0x0C] = f_game_task_09ABD318,
    [0x0D] = f_game_task_09ABC858,
    [0x0E] = f_game_task_09ABD480,
    [0x0F] = f_game_task_09ABD630,     /* begin_block_over_point_table */
    [0x10] = f_game_task_09ABC9B8,     /* target_point_index_534 */
    [0x11] = f_game_task_09AC2DE8,
    [0x12] = f_game_task_09ABA440,     /* target_highest_slot_in_mask_503 */
    [0x13] = f_game_task_09ABA858,     /* target_chosen_slot_511 */
    [0x14] = f_game_task_09ABEA38,     /* if_hunter_target_angle_off_field_1f4_at_least */
    [0x15] = f_game_task_09ABC600,     /* switch_on_field_29a_remapped */
    [0x16] = f_game_task_09ABD778,     /* enter_table_24_entry */
    [0x17] = f_game_task_09ABD7C0,     /* set_id_sequence */
    [0x18] = f_game_task_09ABD818,     /* enter_next_sequence_id_differing_from_field_29a */
    [0x19] = f_game_task_09ABCC10,     /* target_kind_3_from_field_540 */
    [0x1A] = f_game_task_09ABDBD8,
    [0x1B] = f_game_task_09ABED10,     /* if_field_544_equals */
    [0x1C] = f_game_task_09ABEE10,
    [0x1D] = f_game_task_09ABEF68,
    [0x1E] = f_game_task_09ABF108,
    [0x1F] = f_game_task_09ABD108,
    [0x20] = f_game_task_09ABF140,
    [0x21] = f_game_task_09ABF3B0,     /* if_field_498_at_most_30pct_of_field_4a0 */
    [0x22] = f_game_task_09ABF4D0,
    [0x24] = f_game_task_09ABF7C0,
    [0x25] = f_game_task_09ABF8B0,
    [0x26] = f_game_task_09ABF8C0,
    [0x27] = f_game_task_09ABB188,     /* switch_on_field_4ae_sorted */
    [0x28] = f_game_task_09ABBB10,     /* if_any_hunter_active_same_field_29a */
    [0x29] = f_game_task_09ABF8D0,     /* if_field_54e_not_positive */
    [0x2A] = f_game_task_09ABF9D0,     /* if_field_550_not_positive */
    [0x2B] = f_game_task_09ABFAD0,
    [0x2C] = f_game_task_09ABB040,     /* switch_on_field_1e8_group */
    [0x2D] = f_game_task_09ABFE90,
    [0x2E] = f_game_task_09ABB788,     /* set_field_644_record */
    [0x2F] = f_game_task_09ABFEB0,
    [0x30] = f_game_task_09AC0018,
    [0x32] = f_game_task_09AC00B0,
    [0x33] = f_game_task_09AC01B0,
    [0x34] = f_game_task_09ABB670,     /* if_bytes_298_299_equal */
    [0x35] = f_game_task_09AC02D0,     /* if_field_638_mask_20 */
    [0x36] = f_game_task_09ABF658,
    [0x38] = f_game_task_09AC03D8,     /* if_cond_9ac04f0 */
    [0x39] = f_game_task_09AB7520,     /* if_field_4c4_nonzero */
    [0x3A] = f_game_task_09ABB328,
    [0x3B] = f_game_task_09ABB7B8,     /* if_field_5c4_nonzero */
    [0x3C] = f_game_task_09ABC4D0,
    [0x3D] = f_game_task_09AC0568,
    [0x3E] = f_game_task_09AC07C8,     /* switch_on_field_53e_remapped */
    [0x3F] = f_game_task_09ABBD90,     /* target_link_point_toward_id_and_enter_table_30 */
    [0x40] = f_game_task_09ABA888,     /* set_mode_4b9_zero_or_one */
    [0x41] = f_game_task_09AC0A30,
    [0x42] = f_game_task_09ABCC40,     /* if_target_angle_off_field_1f4_at_least */
    [0x43] = f_game_task_09ABB428,     /* if_global_08a62d9c_mask_20000000 */
    [0x44] = f_game_task_09ABB548,
    [0x45] = f_game_task_09AC0DC8,
    [0x46] = f_game_task_09AC1000,
    [0x47] = f_game_task_09AC1158,
    [0x48] = f_game_task_09AC1270,
    [0x49] = f_game_task_09AC1280,
    [0x4A] = f_game_task_09AC12E0,
    [0x4B] = f_game_task_09AC13F8,
    [0x4C] = f_game_task_09AC1428,
    [0x4D] = f_game_task_09AC1458,     /* compute_position_into_5d0 */
    [0x4E] = f_game_task_09AC1488,
    [0x4F] = f_game_task_09AC14D8,
    [0x50] = f_game_task_09AC1528,
    [0x51] = f_game_task_09AC1578,     /* if_byte_280_is_4 */
    [0x52] = f_game_task_09AC1678,
    [0x53] = f_game_task_09ABA4B0,     /* choose_hunter_slot_by_field_554_tiers */
    [0x54] = f_game_task_09ABB8B8,     /* if_chosen_hunter_active_same_field_29a */
    [0x55] = f_game_task_09AC1A08,
    [0x56] = f_game_task_09AC1B20,
    [0x57] = f_game_task_09AC1CC0,
    [0x58] = f_game_task_09AC1E88,
    [0x59] = f_game_task_09AC1EC0,
    [0x5A] = f_game_task_09AC1FC0,
    [0x5B] = f_game_task_09AC2120,
    [0x5C] = f_game_task_09AC2178,
    [0x5D] = f_game_task_09AC22C0,
    [0x5E] = f_game_task_09ABBC90,     /* if_field_292_equals */
    [0x5F] = f_game_task_09AC2410,
    [0x60] = f_game_task_09AC2840,
    [0x61] = f_game_task_09AC2958,
    [0x62] = f_game_task_09AC2980,
    [0x63] = f_game_task_09AC2B78,     /* if_field_4ae_has_operand_mask */
    [0x64] = f_game_task_09AC2C80,
    [0x65] = f_game_task_09AC2DD0,
    [0x66] = f_game_task_09AC2E40,
    [0x67] = f_game_task_09AC2FA8,
    [0x68] = f_game_task_09AC30C0,
    [0x69] = f_game_task_09AC0B70,
    [0x70] = f_game_task_09ABFCF0,     /* switch_on_field_1e8_sorted */
    [0x71] = f_game_task_09ABBA08,     /* if_field_4bc_at_least */
    [0x75] = f_game_task_09AC3B08,
    [0x76] = f_game_task_09AC3970,     /* switch_with_selector_0 */
    [0x77] = f_game_task_09AC3CA8,     /* if_global_a4f1a0_id_has_remap_entry */
    [0x78] = f_game_task_09AC3FE0,
    [0x79] = f_game_task_09AC3798,     /* switch_on_vfunc_dc */
    [0x7A] = f_game_task_09AC3DD0,
    [0x7C] = f_game_task_09AC3550,     /* if_game_6af10_remapped_is_09_0a_or_16 */
    [0x7D] = f_game_task_09AC36A0,
    [0x7E] = f_game_task_09AC4278,
    [0x80] = f_game_task_09AB8AE0,     /* weighted_branch */
    [0x81] = f_game_task_09ABAAF8,     /* enter_table_04_entry */
    [0x82] = f_game_task_09ABAB38,     /* enter_table_3c_entry */
    [0x83] = f_game_task_09ABA8F8,     /* switch_on_distance_to_chosen_slot */
    [0x85] = f_game_task_09AB8C88,
    [0x86] = f_game_task_09AC4660,
    [0x87] = f_game_task_09AC4528,
    [0x90] = f_game_task_09AC30E8,
    [0x91] = f_game_task_09AC3160,
    [0x93] = f_game_task_09AC3218,
    [0x94] = f_game_task_09AC3220,
    [0x99] = f_game_task_09AC31F8,
    [0xFF] = f_game_task_09AB6788,     /* end_block */
};

static uint32_t run_handler(Ctx *c, uint32_t self, uint32_t at, void (*handler)(Ctx *)) {
    call2(c, on_byte, self, at);
    call2(c, on_byte, self, at + 1);
    return call2(c, handler, self, at + 1);
}

/* game_task 09AB4C68: until opcode 5 chooses an action and the position is saved for next time, or MAX_STEPS */
void hand_monster_script_run(Ctx *c) {
    Monster *self = OBJ(Monster, c->r[4]);
    uint32_t self_addr = c->r[4];
    if (self->state_group == STATE_GROUP_NO_SCRIPT_A || self->state_group == STATE_GROUP_NO_SCRIPT_B)
        return;
    self->script_requests_blocked = 0;
    call1(c, restart_if_flagged, self_addr);
    uint32_t at = call1(c, take_pending_entry, self_addr);
    if (at == 0)
        at = call1(c, saved_position, self_addr);
    for (int step = 1; step < MAX_STEPS; step++) {
        if (at == 0) {
            call1(c, restart_script, self_addr);
            return;
        }
        uint8_t op = LBU(at);
        if (op == OP_CHOOSE_ACTION) {
            uint32_t a1 = LBU(at + 1), a2 = LBU(at + 2);
            self->script_suspend_arg = LBU(at + 3);
            at += 4;
            if (self->locally_owned != 0)
                call4(c, choose_action, self_addr, a1, a2, 1);
            call2(c, save_position, self_addr, at);
            call1(c, no_op, self_addr);
            return;
        }
        if (op == OP_MAIN_ENTRY_RESET) {
            call2(c, on_byte, self_addr, at);
            self->script_main_entry = 0;
            at = call1(c, main_entry, self_addr);
        } else if (op == OP_ACTION_7B || op == OP_ACTION_84) {
            call2(c, on_byte, self_addr, at);
            call1(c, op_7b_action, self_addr);
            at++;
        } else if (op == OP_31) {
            call2(c, on_byte, self_addr, at);
            at = call2(c, op_31, self_addr, at + 1);
        } else if (op == OP_92) {
            at = run_handler(c, self_addr, at, op_92);
            call1(c, no_op, self_addr);
        } else if (handlers[op] != NULL) {
            at = run_handler(c, self_addr, at, handlers[op]);
        } else {
            call1(c, restart_script, self_addr);
            return;
        }
    }
}
