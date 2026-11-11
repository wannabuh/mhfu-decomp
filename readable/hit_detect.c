/* Hit detection: which of the hit records in play reaches what, each tick. Rewritten by hand from game_sub 09C41F58, 09C426E0 and 09C42750, the
 * generated code still behind them (MHFU_HAND=0). 09C41E78 and 09C41EA0, which clear the hits before this, hold frame-rate gates and stay generated.
 *
 * Order: for every record (hit_manager_test_nodes_against_targets), every monster and then every hunter that could take it; then every shape of
 * the record against the target's shapes (hit_node_test_target_and_apply). The first pair of shapes to touch lands the hit: on a hunter,
 * hit_damage.c; on a monster, game_sub 09C43558. A target hit once this tick isn't tested again, as its hit_pending is set.
 *
 * To change who can be hit: invulnerability is hit_immune_ticks, tested first in hit_node_test_target_and_apply; what makes a target skip a record
 * is in can_take_at_all() and test_a_monster() below; a monster's parts are picked in monster_part_can_take().
 *
 * The shapes are the game's structures on its stack, which the math in game_sub reads and writes, so they stay there. */
#include <string.h>

#include "hit.h"
#include "names.h"
#include "hitview.h"
#include "wounds.h"

/* Named here by address as function names may change: */
#define in_current_stage f_EBOOT_08865BAC
#define hunter_of_slot f_EBOOT_088DCEBC
#define test_target_in_stage f_game_sub_09C426E0
#define test_target f_game_sub_09C42750
#define mark_overlapping_records f_game_sub_09C422E8
#define land_on_hunter f_game_sub_09C43000
#define land_on_monster f_game_sub_09C43558
#define after_landing f_game_sub_09C2C370
#define set_hit_angle f_game_sub_09C42DA0
#define kind_in_16_1E f_game_sub_09C45190
#define shape_from_place f_game_sub_09C37A50
#define shape_from_two_places f_game_sub_09C37BC0
#define shape_from_bone f_game_sub_09C37898
#define shape_list_jump_attacker f_game_sub_09C387A0
#define shape_list_jump_target f_game_sub_09C38758
#define capsule_prepare f_game_sub_09C345D8
#define capsule_sphere_contact f_game_sub_09C36238
#define spheres_contact f_game_sub_09C34718
#define capsules_contact f_game_sub_09C34878
#define sphere_capsule_contact f_game_sub_09C359C8

/* In the hunt's module, called as the game does */
#define RECORD_TEST_FLAGS 0x09B67728u

#define HIT_LISTS_POINTER 0x09C190D0u       /* -> +0x502C the first record */
#define HIT_LISTS_FIRST 0x502Cu
#define MONSTERS_POINTER 0x09C122B0u        /* -> +0x1220 the monsters */
#define MONSTERS 0x1220u
#define MONSTER_COUNT 20
#define HUNTER_COUNT 4
#define HUNTER_MANAGER_POINTER 0x09A4F204u
#define AFTER_LANDING_POINTER 0x09D19860u

#define HIT_LOG_ENTRIES 16
#define STATE_TESTS_TARGETS 2
#define STATE_GROUP_FAINT 3
#define OWNER_NONE 0xFF
#define NO_HIT_ID 0xFFFF

/* The record's flags, by the test */
enum {
    RECORD_FLAG_HITS_OWN = 4,           /* can hit its own owner */
    RECORD_FLAG_HITS_MONSTER_HIDDEN_PARTS = 0x10,     /* part bit +0xA & 2, while part_mask_3_ticks */
    RECORD_FLAG_NOT_MONSTERS = 0x80,
    RECORD_FLAG_NOT_HUNTERS = 0x400,
};

/* Three kinds that reach only monsters, on some conditions; one a fainting hunter can take */
enum { KIND_8 = 8, KIND_9 = 9, KIND_C = 0xC, KIND_D = 0xD, KIND_1E = 0x1E };
#define HEIGHT_REACH 150.0f     /* kinds 8, 9, 0xC don't reach a monster this far below the record's shape */
#define MODE_LYING_4 4          /* movement_mode, for kind 9 */
#define MODE_IN_THE_AIR 2

/* The stack frame of 09C42750 */
#define FRAME 0x190
#define F_MID_POINT 0x20
#define F_TARGET_TYPE 0x30          /* a word, 0: its shape is a capsule, otherwise a sphere; the shape itself from +0x10 */
#define F_TARGET_SHAPE 0x40
#define F_TARGET_RADIUS 0x50
#define F_ATTACKER_TYPE 0x70
#define F_ATTACKER_SHAPE 0x80
#define F_ATTACKER_RADIUS 0x90
#define F_TARGET_CAPSULE 0xB0
#define F_TARGET_CONTACT 0xF0
#define F_ATTACKER_CAPSULE 0x110
#define F_ATTACKER_CONTACT 0x150
#define F_CONTACT 0x170
#define F_TARGET_CURSOR 0x188       /* its part records, a word */
#define F_ATTACKER_CURSOR 0x18C

#define SHAPE_NEXT_STRIDE 0x28
#define SHAPE_END -1
#define SHAPE_JUMP 0x7D             /* the first short: the list goes on elsewhere */
#define SHAPE_TWO_PLACES 0x7E       /* the record's shape_a and shape_b */
#define SHAPE_PLACE 0x7F            /* the record's shape_a */

#define HUNTER_SPHERES 2
#define HUNTER_SPHERE_STRIDE 0x20u
#define SPHERE_A_CENTRE 0x4A0u
#define SPHERE_B_CENTRE 0x4C0u

static int test_record(Ctx *c, const HitRecord *record, uint32_t mask) {
    return call2_at(c, RECORD_TEST_FLAGS, GAME_ADDR(record), mask) != 0;
}

/* -- which targets ----------------------------------------------------------------------------------------------------------------------- */

/* The tests every target goes through; `flag_not` is the flag that rules out its class */
static int can_take_at_all(Ctx *c, const HitRecord *record, uint32_t owner, uint32_t target_addr, uint32_t flag_not) {
    const Hunter *target = OBJ(Hunter, target_addr);
#define SKIP(why) do { if (hit_view_on) hit_view_skip(GAME_ADDR(record), target_addr, why); return 0; } while (0)
    if (target->area_id != record->area_id)
        SKIP(HIT_SKIP_AREA);
    if (test_record(c, record, flag_not))
        SKIP(HIT_SKIP_RECORD_FLAG);
    if (record->owner_kind != OWNER_NONE && owner == target_addr && !test_record(c, record, RECORD_FLAG_HITS_OWN))
        SKIP(HIT_SKIP_OWN);
    if (record->hit_id != NO_HIT_ID)
        for (uint32_t i = 0; i < HIT_LOG_ENTRIES; i++)
            if (LHU(target_addr + HIT_LOG + 2 * i) == record->hit_id)
                SKIP(HIT_SKIP_LOGGED);
    if (target->hit_pending != 0)
        SKIP(HIT_SKIP_PENDING);
    return 1;
#undef SKIP
}

static void test_a_monster(Ctx *c, const HitRecord *record, uint32_t owner, const Monster *monster) {
    if (monster->last_hit_record != 0) {
        if (hit_view_on)
            hit_view_skip(GAME_ADDR(record), GAME_ADDR(monster), HIT_SKIP_LAST_HIT);
        return;
    }
    switch (record->kind) {
    case KIND_9:
        if (monster->movement_mode == MODE_LYING_4 || monster->state_flag_cleared_with_3cf != 0)
            return;
        /* fall through */
    case KIND_8:
    case KIND_C:
        if (monster->movement_mode == MODE_IN_THE_AIR || monster->health <= 0)
            return;
        if (monster->place[1] < record->shape_a[1] - HEIGHT_REACH)
            return;
        break;
    default:
        break;
    }
    call3(c, test_target_in_stage, GAME_ADDR(record), owner, GAME_ADDR(monster));
}

static void test_monsters(Ctx *c, const HitRecord *record, uint32_t owner) {
    for (uint32_t i = 0; i < MONSTER_COUNT; i++) {
        uint32_t monster = LW(LW(MONSTERS_POINTER) + MONSTERS + 4 * i);
        if (monster != 0 && can_take_at_all(c, record, owner, monster, RECORD_FLAG_NOT_MONSTERS))
            test_a_monster(c, record, owner, OBJ(Monster, monster));
    }
}

static void test_hunters(Ctx *c, const HitRecord *record, uint32_t owner) {
    for (uint32_t i = 0; i < HUNTER_COUNT; i++) {
        uint32_t hunter = call2(c, hunter_of_slot, LW(HUNTER_MANAGER_POINTER), i);
        if (hunter != 0 && can_take_at_all(c, record, owner, hunter, RECORD_FLAG_NOT_HUNTERS)
            && record->kind != KIND_9 && record->kind != KIND_C && record->kind != KIND_8)
            call3(c, test_target_in_stage, GAME_ADDR(record), owner, hunter);
    }
}

/* The object a record belongs to: 0 when it has none */
static uint32_t owner_of(Ctx *c, const HitRecord *record) {
    if (record->owner_table == 0)
        return call2(c, hunter_of_slot, LW(HUNTER_MANAGER_POINTER), record->owner_slot);
    uint32_t monsters = LW(MONSTERS_POINTER);
    int32_t slot = record->owner_slot;
    return slot >= 0 && slot < MONSTER_COUNT ? LW(monsters + MONSTERS + 4u * (uint32_t)slot) : 0;
}

/* game_sub 09C41F58, in the hunt's tick */
void hand_hit_manager_test_nodes_against_targets(Ctx *c) {
    uint32_t at = LW(LW(HIT_LISTS_POINTER) + HIT_LISTS_FIRST);
    while (at != 0) {
        HitRecord *record = OBJ(HitRecord, at);
        record->connected = 0;
        if (record->state == STATE_TESTS_TARGETS)
            call1(c, mark_overlapping_records, at);
        uint32_t owner = 0;
        if (record->owner_kind == OWNER_NONE || (owner = owner_of(c, record)) != 0) {
            test_monsters(c, record, owner);
            test_hunters(c, record, owner);
        }
        at = record->next;
    }
}

/* game_sub 09C426E0 */
void hand_hit_node_test_target_if_in_current_map_and_state_2(Ctx *c) {
    uint32_t record = c->r[4], attacker = c->r[5], target = c->r[6];
    if ((uint8_t)call1(c, in_current_stage, target) != 0 && OBJ(HitRecord, record)->state == STATE_TESTS_TARGETS)
        call3(c, test_target, record, attacker, target);
}

/* -- the shapes ---------------------------------------------------------------------------------------------------------------------------- */

static void land_and_tell(Ctx *c, uint32_t record, uint32_t attacker, uint32_t target, uint32_t sp) {
    c->r[8] = sp + F_CONTACT;
    call4(c, after_landing, LW(AFTER_LANDING_POINTER), record, attacker, target);
}

/* A hunter has two spheres. The first that touches lands the hit, angled from the middle of them both. */
static int test_hunter_spheres(Ctx *c, uint32_t record, uint32_t attacker, Hunter *hunter, uint32_t sp) {
    uint32_t hunter_addr = GAME_ADDR(hunter);
    for (uint32_t i = 0; i < HUNTER_SPHERES; i++) {
        uint32_t sphere = hunter_addr + HUNTER_SPHERE_STRIDE * i;
        uint32_t touched;
        if (LW(sp + F_ATTACKER_TYPE) == 0) {
            c->f[12].u = LW(sphere + 0x4B0u);
            touched = call3(c, capsule_sphere_contact, sp + F_ATTACKER_CAPSULE, sphere + SPHERE_A_CENTRE, sp + F_CONTACT);
        } else {
            c->f[12].u = LW(sp + F_ATTACKER_RADIUS);
            c->f[13].u = LW(sphere + 0x4B0u);
            touched = call3(c, spheres_contact, sp + F_ATTACKER_SHAPE, sphere + SPHERE_A_CENTRE, sp + F_CONTACT);
        }
        if ((uint8_t)touched != 1)
            continue;
        const float *a = (const float *)MEM(hunter_addr + SPHERE_A_CENTRE), *b = (const float *)MEM(hunter_addr + SPHERE_B_CENTRE);
        float mid[3];
        for (int axis = 0; axis < 3; axis++)
            mid[axis] = hunter->place[axis] + 0.5f * (a[axis] - b[axis]);
        memcpy(MEM(sp + F_MID_POINT), mid, sizeof mid);
        SW(sp + F_MID_POINT + 12, 0);
        uint32_t from = LW(sp + F_ATTACKER_TYPE) == 0 ? sp + F_ATTACKER_CONTACT : sp + F_ATTACKER_SHAPE;
        call4(c, set_hit_angle, record, hunter_addr, sp + F_MID_POINT, from);
        call4(c, land_on_hunter, record, attacker, hunter_addr, sp + F_CONTACT);
        land_and_tell(c, record, attacker, hunter_addr, sp);
        return 1;
    }
    return 0;
}

/* Whether this part record can take this hit: the record's and part's bits, then the kind's own rules. What the bits mean to the game isn't
 * known; +0xA's 2 and 8 go by two of the monster's timers. The part is read again where the original reads it again: from the stack. */
static int monster_part_can_take(Ctx *c, const HitRecord *record, const Monster *monster, uint32_t sp) {
    if (LBU(LW(sp + F_TARGET_CURSOR) + 0xA) & 2) {
        if (monster->part_mask_3_ticks == 0 || !test_record(c, record, RECORD_FLAG_HITS_MONSTER_HIDDEN_PARTS))
            return 0;
    }
    uint32_t part = LW(sp + F_TARGET_CURSOR);
    if ((LBU(part + 0xA) & 8) && monster->part_mask_c_ticks == 0)
        return 0;
    uint8_t bits9 = LBU(part + 9);
    if ((bits9 & 4) || (LBU(part + 0xB) & 1))
        return 0;
    uint8_t bits8 = LBU(part + 8);
    if ((bits8 & 4) && (monster->locally_owned == 0 || (record->kind != KIND_8 && record->kind != KIND_C)))
        return 0;
    if ((bits9 & 2) && (monster->locally_owned == 0 || record->kind != KIND_9))
        return 0;
    uint8_t kind = record->kind;
    if ((kind == KIND_8 || kind == KIND_C) && (!(bits8 & 1) || monster->locally_owned == 0))
        return 0;
    if (kind == KIND_9 && (!(bits9 & 1) || monster->locally_owned == 0))
        return 0;
    if ((uint8_t)call1(c, kind_in_16_1E, kind) == 1 && !(LBU(LW(sp + F_TARGET_CURSOR) + 8) & 2))
        return 0;
    if (record->kind == KIND_1E && monster->status_hold_mode == 2)
        return 0;
    return !(LBU(LW(sp + F_TARGET_CURSOR) + 9) & 8) || record->buildup_mask == 8;
}

/* The target's shape against the attacker's, each a sphere or a capsule. Returns whether they touch: the place is at F_CONTACT. */
static int monster_shapes_touch(Ctx *c, uint32_t sp) {
    uint32_t touched;
    if (LW(sp + F_ATTACKER_TYPE) == 0) {
        if (LW(sp + F_TARGET_TYPE) == 0) {
            touched = call3(c, capsules_contact, sp + F_ATTACKER_CAPSULE, sp + F_TARGET_CAPSULE, sp + F_CONTACT);
        } else {
            c->f[12].u = LW(sp + F_TARGET_RADIUS);
            touched = call3(c, capsule_sphere_contact, sp + F_ATTACKER_CAPSULE, sp + F_TARGET_SHAPE, sp + F_CONTACT);
        }
    } else {
        c->f[12].u = LW(sp + F_ATTACKER_RADIUS);
        if (LW(sp + F_TARGET_TYPE) == 0) {
            touched = call3(c, sphere_capsule_contact, sp + F_ATTACKER_SHAPE, sp + F_TARGET_CAPSULE, sp + F_CONTACT);
        } else {
            c->f[13].u = LW(sp + F_TARGET_RADIUS);
            touched = call3(c, spheres_contact, sp + F_ATTACKER_SHAPE, sp + F_TARGET_SHAPE, sp + F_CONTACT);
        }
    }
    return (uint8_t)touched != 0;
}

/* A monster has its part records (the monster type table's). The first that can take the hit and touches lands it. */
static int test_monster_parts(Ctx *c, const HitRecord *record, uint32_t record_addr, uint32_t attacker, Monster *monster, uint32_t sp) {
    uint32_t monster_addr = GAME_ADDR(monster);
    SW(sp + F_TARGET_CURSOR, LW(MONSTER_TYPE(monster->subtype, MONSTER_TYPE_PARTS)));
    for (;;) {
        uint32_t part = LW(sp + F_TARGET_CURSOR);
        int32_t id = (int16_t)LH(part);
        if (id == SHAPE_END)
            return 0;
        if (id == SHAPE_JUMP) {
            call2(c, shape_list_jump_target, monster_addr, sp + F_TARGET_CURSOR);
            continue;
        }
        if (id == SHAPE_TWO_PLACES || id == SHAPE_PLACE)
            call3(c, shape_from_place, monster_addr + 0x200, part, sp + F_TARGET_TYPE);
        else
            call3(c, shape_from_bone, monster_addr, part, sp + F_TARGET_TYPE);
        int can_take = monster_part_can_take(c, record, monster, sp);
        if (hit_view_on && !can_take)
            hit_view_part(record_addr, monster_addr, LW(sp + F_TARGET_CURSOR), sp + F_ATTACKER_TYPE, sp + F_TARGET_TYPE, 0, 0);
        if (can_take) {
            if (LW(sp + F_TARGET_TYPE) == 0)
                call2(c, capsule_prepare, sp + F_TARGET_SHAPE, sp + F_TARGET_CAPSULE);
            int touched = monster_shapes_touch(c, sp);
            if (hit_view_on)
                hit_view_part(record_addr, monster_addr, LW(sp + F_TARGET_CURSOR), sp + F_ATTACKER_TYPE, sp + F_TARGET_TYPE, 1, touched);
            if (touched) {
                uint32_t from = LW(sp + F_ATTACKER_TYPE) == 0 ? sp + F_ATTACKER_CONTACT : sp + F_ATTACKER_SHAPE;
                uint32_t to = LW(sp + F_TARGET_TYPE) == 0 ? sp + F_TARGET_CONTACT : sp + F_TARGET_SHAPE;
                call4(c, set_hit_angle, record_addr, monster_addr, to, from);
                c->r[8] = LW(sp + F_TARGET_CURSOR);
                c->r[9] = sp + F_CONTACT;
                wound_before(monster_addr);     /* (the runtime's: reads only) */
                call4(c, land_on_monster, record_addr, attacker, monster_addr, LW(sp + F_ATTACKER_CURSOR));
                wound_hit(record_addr, attacker, monster_addr, sp + F_CONTACT, sp + F_ATTACKER_TYPE);  /* (the runtime's: reads only) */
                land_and_tell(c, record_addr, attacker, monster_addr, sp);
                return 1;
            }
        }
        SW(sp + F_TARGET_CURSOR, LW(sp + F_TARGET_CURSOR) + SHAPE_NEXT_STRIDE);
    }
}

/* Every shape of the record, against the target */
static void test_shapes(Ctx *c, uint32_t record_addr, uint32_t attacker, uint32_t target_addr, uint32_t sp) {
    const HitRecord *record = OBJ(HitRecord, record_addr);
    Hunter *target = OBJ(Hunter, target_addr);
    for (;;) {
        uint32_t shape = LW(sp + F_ATTACKER_CURSOR);
        int32_t id = (int16_t)LH(shape);
        if (id == SHAPE_END)
            return;
        if (id == SHAPE_JUMP) {
            call2(c, shape_list_jump_attacker, record_addr, sp + F_ATTACKER_CURSOR);
            continue;
        }
        if (id == SHAPE_TWO_PLACES)
            call4(c, shape_from_two_places, record_addr + 0x40, record_addr + 0x50, shape, sp + F_ATTACKER_TYPE);
        else if (id == SHAPE_PLACE)
            call3(c, shape_from_place, record_addr + 0x40, shape, sp + F_ATTACKER_TYPE);
        else
            call3(c, shape_from_bone, attacker, shape, sp + F_ATTACKER_TYPE);
        if (hit_view_on)
            hit_view_attacker(record_addr, sp + F_ATTACKER_TYPE);
        if (LW(sp + F_ATTACKER_TYPE) == 0)
            call2(c, capsule_prepare, sp + F_ATTACKER_SHAPE, sp + F_ATTACKER_CAPSULE);
        int landed = target->kind == 0 ? test_hunter_spheres(c, record_addr, attacker, target, sp)
                                       : test_monster_parts(c, record, record_addr, attacker, OBJ(Monster, target_addr), sp);
        if (landed)
            return;
        SW(sp + F_ATTACKER_CURSOR, LW(sp + F_ATTACKER_CURSOR) + SHAPE_NEXT_STRIDE);
    }
}

/* game_sub 09C42750: record a0 from attacker a1 (0: none) against target a2, a hunter or a monster. A hunter fainting takes only kind 0xD, and
 * only then does it take it. */
void hand_hit_node_test_target_and_apply(Ctx *c) {
    uint32_t record_addr = c->r[4], attacker = c->r[5], target_addr = c->r[6];
    const HitRecord *record = OBJ(HitRecord, record_addr);
    const Hunter *target = OBJ(Hunter, target_addr);
    if (target->hit_immune_ticks != 0) {
        if (hit_view_on)
            hit_view_skip(record_addr, target_addr, HIT_SKIP_IMMUNE);
        return;
    }
    uint32_t shapes = record->shapes;
    if (shapes == 0)
        return;
    if (target->kind == 0 && (target->state_group == STATE_GROUP_FAINT) != (record->kind == KIND_D))
        return;
    uint32_t sp = c->r[29] - FRAME;
    c->r[29] = sp;
    SW(sp + F_ATTACKER_CURSOR, shapes);
    test_shapes(c, record_addr, attacker, target_addr, sp);
    c->r[29] = sp + FRAME;
}
