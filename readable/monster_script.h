/* What the monster script's interpreter and its opcodes' handlers share: walking over instructions, the three-part "if", distances. See monster_script.c. */
#pragma once
#include <math.h>

#include "hand.h"

#define next_instruction f_game_task_09AB7858
#define skip_to f_game_task_09AB7728
#define skip_past_end f_game_task_09AB8A38

enum { IF_TEST = 0, IF_ELSE = 1, IF_END = 2 };

static inline uint32_t next_of(Ctx *c, uint32_t self, uint32_t at) {
    return call2(c, next_instruction, self, at);
}

/* A test that failed: to the else, or past the end */
static inline uint32_t to_else_or_end(Ctx *c, uint32_t self, uint32_t at, uint8_t op) {
    while (LBU(at) != op || (LBU(at + 1) != IF_ELSE && LBU(at + 1) != IF_END))
        at = call4(c, skip_to, self, at, op, IF_END);
    uint32_t next = next_of(c, self, at);
    if (LBU(next) == op && LBU(next + 1) == IF_END)
        return next_of(c, self, next);
    return next;
}

/* An "if" opcode's byte after it, when it isn't the test: "op 2" goes on, "op 1", reached by running into it, goes past the end. Sets where to go on, and says it has.
 * Any other byte is passed over like the end. */
static inline int if_not_the_test(Ctx *c, uint32_t self, uint32_t at, uint8_t op) {
    uint8_t sub = LBU(at);
    if (sub == IF_TEST)
        return 0;
    c->r[2] = sub == IF_ELSE ? call3(c, skip_past_end, self, at + 1, op) : at + 1;
    return 1;
}

/* What the test's answer means */
static inline void if_test_answer(Ctx *c, uint32_t self, uint32_t after_test, uint8_t op, int passed) {
    c->r[2] = passed ? after_test : to_else_or_end(c, self, after_test, op);
}

/* As the VFPU works it out: a dot of a vector with itself from 0, then the root */
static inline float distance_3d(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(0.0f + x * x + y * y + z * z);
}

static inline float distance_flat(const float a[3], const float b[3]) {
    float x = a[0] - b[0], z = a[2] - b[2];
    return sqrtf(0.0f + x * x + z * z);
}

/* Some area ids stand for another. An id that isn't 0 in the first table goes by the second, unless that says 0xFFFF. */
#define AREA_REMAP_KIND 0x09CE5108u             /* u16 by area id */
#define AREA_REMAP_OTHER 0x09CE5320u            /* u16 by area id */
static inline uint16_t area_as_remapped(Ctx *c, uint16_t id) {
    if (id == 0xFFFF || LHU(AREA_REMAP_KIND + 2u * id) == 0)
        return id;
    uint16_t other = LHU(AREA_REMAP_OTHER + 2u * id);
    return other == 0xFFFF ? id : other;
}

/* The same with a flag: an id goes as it is when the first table's value is what the second flag table says, which goes by +0x284 of what POINTER_09A4F1A0 points at. */
#define POINTER_09A4F1A0 0x09A4F1A0u
#define AREA_REMAP_FLAG 0x09CE4908u             /* u16 by that +0x284: 0xFFFF or not picks which ids stand as they are */
static inline uint16_t area_by_remap_flag(Ctx *c, uint16_t id) {
    if (id == 0xFFFF)
        return id;
    uint32_t flag = LHU(AREA_REMAP_FLAG + LW(LW(POINTER_09A4F1A0) + 0x284) * 2) != 0xFFFF;
    if (flag == LHU(AREA_REMAP_KIND + 2u * id))
        return id;
    uint16_t other = LHU(AREA_REMAP_OTHER + 2u * id);
    return other == 0xFFFF ? id : other;
}

/* A switch opcode's other bytes than the first case's: "op 3" goes on, "op 1", reached by running into it, skips its value and goes past the end, "op 2" likewise. Any other byte is
 * passed over. Sets where to go on, and says it has. */
static inline uint32_t past_switch_end(Ctx *c, uint32_t self, uint32_t at, uint8_t op) {
    int more;
    do {
        uint32_t entry = call4(c, skip_to, self, at, op, 3);
        more = !(LBU(entry) == op && LBU(entry + 1) == 3);
        at = next_of(c, self, entry);
    } while (more);
    return at;
}

static inline int switch_not_the_first_case(Ctx *c, uint32_t self, uint32_t at, uint8_t op, uint32_t value_bytes) {
    uint8_t sub = LBU(at);
    if (sub == 0)
        return 0;
    if (sub == 1)
        c->r[2] = past_switch_end(c, self, at + 1 + value_bytes, op);
    else if (sub == 2)
        c->r[2] = past_switch_end(c, self, at + 1, op);
    else
        c->r[2] = at + 1;
    return 1;
}

/* A case not taken: on to the next, says whether there is one */
static inline int to_next_case(Ctx *c, uint32_t self, uint32_t *at, uint8_t op) {
    uint32_t entry = call4(c, skip_to, self, *at, op, 3);
    uint8_t found = LBU(entry + 1);
    *at = entry + 2;
    return found != 2 && found != 3;
}

/* A switch whose cases' values are in order, by the value of one byte each: goes on from the case that's the value; a greater one than the value ends the search past the end,
 * which here also an "op 2" stops */
static inline uint32_t sorted_cases(Ctx *c, uint32_t self, uint32_t at, uint8_t op, int32_t count, int32_t value) {
    for (int32_t i = 0; i < count; i++) {
        int32_t case_value = LBU(at);
        at++;
        if (value == case_value)
            return at;
        if (case_value >= value) {
            int more;
            do {
                uint32_t entry = call4(c, skip_to, self, at, op, 3);
                more = !(LBU(entry) == op && (LBU(entry + 1) == 2 || LBU(entry + 1) == 3));
                at = next_of(c, self, entry);
            } while (more);
            return at;
        }
        if (!to_next_case(c, self, &at, op))
            return at;
    }
    return at;
}
