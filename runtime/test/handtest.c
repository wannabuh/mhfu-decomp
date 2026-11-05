/* Code rewritten by hand against the generated code it replaces (recomp/patches.py HAND), on inputs the recordings never reach.
 *
 * Usage: handtest <RAM snapshot from a quest> [iterations] [stubs <WORK/signatures.tsv>] [only <part of a case's name>]
 * Takes the snapshot's hunter and game state, randomises what a function's branches depend on, then runs each version from the same
 * state and compares all of RAM but the stack (dead bytes there are not behaviour), the callee-saved registers and, where the
 * function returns one, v0. The random numbers are fixed, so a run repeats. Build it with HAND_FLAGS for coverage to see what it
 * reached. A new rewrite: a row in `cases`, and its inputs in randomise(), or a function of its own (the row's last columns) if they
 * shouldn't disturb the other cases: then its row goes last, as what it sets stays set.
 *
 * In a build generated with RECOMP_TRACE_ALL=1 it also tests each function alone (make build/handtest in that worktree): everything
 * it calls is a stub that logs its arguments and returns the next random result, the same for both versions. So every branch can be
 * reached whatever the game would really return, and the two call sequences are compared too. */
#include "runtime.h"
#include "funcs.h"
#include "mhfu_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_START 0x08800000u
#define SNAP_SIZE 0x01800000u
#define TEST_SP 0x09FF0000u
#define STACK_LOW (TEST_SP - 0x4000u)
#define HUNTER_VTABLE 0x089BB3CCu
#define GAME_POINTER 0x089CC558u

struct Pokes;
typedef struct {
    const char *name;
    void (*fn)(Ctx *);
    int returns;
    int small_a1;       /* a1 is a mode, 0 to 2, not an amount */
    uint32_t addr;
    void (*randomise_more)(Ctx *, uint32_t hunter);     /* also: long chains of results that must be 0 are reached, callees change fields */
    void (*inputs)(Ctx *, uint32_t hunter, uint32_t args[3]);   /* the function's own inputs and a1-a3 (a2, a3 otherwise rubbish). Its row goes last */
    const struct Pokes *pokes;      /* what callees may change, for a function that reads fields again after its calls */
} Case;

static void randomise_upkeep(Ctx *c, uint32_t hunter);
static void inputs_loss_blocked(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_active_skill(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_hunt_bonus(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_face_stick(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_flags(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_state_is(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_movement(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_classify(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_countdowns(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_overlays(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_airborne(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_transient(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_real_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_reset(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_run_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_group0(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_tick(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_motion(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_motion_copy(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_copy_pad(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_common(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_stamina_scaled(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_motion_update(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_state_run(Ctx *c, uint32_t hunter, uint32_t args[3]);
extern const struct Pokes run_pokes, upkeep_pokes, set_state_pokes, reset_pokes, tick_pokes, copy_pad_pokes, common_pokes;

static const Case cases[] = {
    {"hunter_stamina_add 09A6A2D0", f_game_task_09A6A2D0, 0, 0, 0x09A6A2D0, NULL, NULL, NULL},
    {"hunter_stamina_use 09A6A370", f_game_task_09A6A370, 0, 0, 0x09A6A370, NULL, NULL, NULL},
    {"hunter_stamina_use_alternate_ticks 09A6A480", f_game_task_09A6A480, 0, 0, 0x09A6A480, NULL, NULL, NULL},
    {"hunter_state_sprint 09A9E288", f_game_task_09A9E288, 0, 1, 0x09A9E288, NULL, NULL, NULL},
    {"hunter_state_sprint_linked 09A9E690", f_game_task_09A9E690, 0, 1, 0x09A9E690, NULL, NULL, NULL},
    {"hunter_state_out_of_breath 09A9EAE8", f_game_task_09A9EAE8, 0, 1, 0x09A9EAE8, NULL, NULL, NULL},
    {"hunter_tick_upkeep 09A68FC8", f_game_task_09A68FC8, 0, 0, 0x09A68FC8, randomise_upkeep, NULL, &upkeep_pokes},
    {"hunter_stamina_loss_blocked 09A6A808", f_game_task_09A6A808, 1, 0, 0x09A6A808, NULL, inputs_loss_blocked, NULL},
    {"hunter_has_active_skill 088D6A1C", f_EBOOT_088D6A1C, 1, 0, 0x088D6A1C, NULL, inputs_active_skill, NULL},
    {"hunter_has_hunt_bonus 088D6A54", f_EBOOT_088D6A54, 1, 0, 0x088D6A54, NULL, inputs_hunt_bonus, NULL},
    {"hunter_set_state_noted 09A67878", f_game_task_09A67878, 0, 0, 0x09A67878, NULL, inputs_set_state, NULL},
    {"hunter_face_stick_direction 09A8B578", f_game_task_09A8B578, 0, 0, 0x09A8B578, NULL, inputs_face_stick, NULL},
    {"object_set_state_flags 08865A64", f_EBOOT_08865A64, 0, 0, 0x08865A64, NULL, inputs_set_flags, NULL},
    {"object_state_is 08865CB8", f_EBOOT_08865CB8, 1, 0, 0x08865CB8, NULL, inputs_state_is, NULL},
    {"object_set_movement_mode_and_flags 09A6F5C0", f_game_task_09A6F5C0, 0, 0, 0x09A6F5C0, NULL, inputs_movement, NULL},
    {"hunter_classify_move_input 09AA5FB0", f_game_task_09AA5FB0, 1, 0, 0x09AA5FB0, NULL, inputs_classify, NULL},
    {"hunter_tick_countdowns 088D6488", f_EBOOT_088D6488, 0, 0, 0x088D6488, NULL, inputs_countdowns, NULL},
    {"hunter_clear_overlays 09A8B3F0", f_game_task_09A8B3F0, 0, 0, 0x09A8B3F0, NULL, inputs_overlays, NULL},
    {"hunter_start_airborne_motion 09A67F30", f_game_task_09A67F30, 0, 0, 0x09A67F30, NULL, inputs_airborne, NULL},
    {"hunter_clear_transient_action_fields 09A68700", f_game_task_09A68700, 0, 0, 0x09A68700, NULL, inputs_transient, NULL},
    {"hunter_set_state 09A67630", f_game_task_09A67630, 0, 0, 0x09A67630, NULL, inputs_real_set_state, &set_state_pokes},
    {"hunter_reset_to_base 09A678F8", f_game_task_09A678F8, 0, 0, 0x09A678F8, NULL, inputs_reset, &reset_pokes},
    {"hunter_run_state 09A67568", f_game_task_09A67568, 0, 0, 0x09A67568, NULL, inputs_run_state, NULL},
    {"hunter_group0_state 09A9A9E0", f_game_task_09A9A9E0, 0, 0, 0x09A9A9E0, NULL, inputs_group0, NULL},
    {"hunter_state_function 09A66198", f_game_task_09A66198, 0, 0, 0x09A66198, NULL, inputs_tick, &tick_pokes},
    {"hunter_set_motion 088D5C34", f_EBOOT_088D5C34, 0, 0, 0x088D5C34, NULL, inputs_set_motion, NULL},
    {"hunter_tick_motion_copy 088D1DD0", f_EBOOT_088D1DD0, 0, 0, 0x088D1DD0, NULL, inputs_motion_copy, NULL},
    {"hunter_copy_pad 088D61CC", f_EBOOT_088D61CC, 0, 0, 0x088D61CC, NULL, inputs_copy_pad, &copy_pad_pokes},
    {"hunter_common_input 09AA5270", f_game_task_09AA5270, 0, 0, 0x09AA5270, NULL, inputs_common, &common_pokes},
    {"hunter_stamina_add_scaled 09A6A598", f_game_task_09A6A598, 0, 0, 0x09A6A598, NULL, inputs_stamina_scaled, NULL},
    {"hunter_motion_update 088D5C7C", f_EBOOT_088D5C7C, 0, 0, 0x088D5C7C, NULL, inputs_motion_update, NULL},
    {"hunter_state_run 09A665F0", f_game_task_09A665F0, 0, 0, 0x09A665F0, NULL, inputs_state_run, &run_pokes},
};

/* -- the stubs: in a trace build, rt_trace_all is at the top of every generated function --------------------------------------- */
#define MAX_CALLS 128
/* a0-a3 t0-t3, f12-f19: only those the callee reads, others 0; stack0 below; a hash of the hunter as the callee finds it, so a write on
 * the wrong side of a call shows even if the stub does nothing with it */
typedef struct { uint32_t fn, a[8], f[8], stack0, hunter_hash; } Call;
#define HUNTER_HASHED 0x1200u
#define NINE_ARGUMENTS 0x08883988u      /* the one callee so far with an argument at its sp+0 */
#define STUB_RECORD 0x09FF8000u         /* what a call returning a pointer to a record gets */
#define RECORD_TYPE 0x3C
#define REGEN_EFFECT_TEST 0x088DAE98u
#define SKILL_TEST 0x088D6A1Cu
static int stubbing;            /* within the function under test */
static int running;             /* it is running, stubs or not */
static uint32_t target;         /* it: not a stub */
static uint32_t results[MAX_CALLS], fresults[MAX_CALLS];
static int16_t stub_stamina[MAX_CALLS];     /* what a call leaves stamina at (0x7FFF: it leaves it alone): callees do change it */
static uint8_t stub_slot[MAX_CALLS];        /* the same for the local player's slot (0xFF: alone) */
static uint8_t stub_record_type[MAX_CALLS];
static uint32_t record_method;              /* the method returning that pointer: *(*(*0x09A4F1A0)) slot 0x28 */
typedef struct { uint32_t offset; uint8_t size; uint8_t in_game; } PokeField;
/* What a callee answers more often than the random results would: a function's deep paths may need several answers at once. Ends with fn 0. */
typedef struct { uint32_t fn; int count; int32_t values[8]; } Bias;
struct Pokes { const PokeField *fields; size_t count; const Bias *bias; };
static const PokeField *pokable;   /* a callee may set one of these */
static const Bias *stub_bias;
static uint32_t bias_pick[MAX_CALLS];
#define MAX_POKES 16
static int npokes;                          /* how many at each call, this time */
static uint16_t stub_poke[MAX_CALLS][MAX_POKES];   /* indexes in them */
static int32_t stub_poke_value[MAX_CALLS][MAX_POKES];
static uint32_t stub_hunter;
static Call calls[2][MAX_CALLS];
static int ncalls[2], current;

/* What each function reads, from tools/readable/signatures.py's table: registers it doesn't read aren't arguments, and what they
 * happen to hold differs between versions without mattering. A bit each: a0-a3 t0-t3, then f12-f19. */
static uint16_t *reads;
#define READS(fn) reads[((fn) - 0x08800000u) >> 2]

static void load_signatures(const char *path) {
    reads = malloc(0x01800000u);
    memset(reads, 0xFF, 0x01800000u);       /* not in the table: every register compared */
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    char line[512];
    static const char *names[16] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19"};
    while (fgets(line, sizeof line, f)) {
        char *module = strtok(line, "\t"), *addr = strtok(NULL, "\t"), *ret = strtok(NULL, "\t"), *args = strtok(NULL, "\t\n");
        (void)module;
        (void)ret;
        uint32_t a = addr ? (uint32_t)strtoul(addr, NULL, 16) : 0;
        if (a < 0x08800000u || a >= 0x0A000000u)
            continue;
        uint16_t mask = 0;
        for (char *r = args ? strtok(args, ",") : NULL; r; r = strtok(NULL, ","))
            for (int i = 0; i < 16; i++)
                if (!strcmp(r, names[i]))
                    mask |= (uint16_t)(1u << i);
        /* an address several overlays share: anything any of them reads (first seen replaces "not in the table") */
        READS(a) = READS(a) == 0xFFFF ? mask : READS(a) | mask;
    }
    fclose(f);
}

/* Callees that reach the kernel, which isn't here, even with no stubs asked for: the sound. Stubbed in any case, their arguments still
 * compared. A bit each for what they read, as in `reads`. In a build without RECOMP_TRACE_ALL a function calling them can't run alone. */
static const struct { uint32_t fn; uint16_t mask; } kernel_bound[] = {
    {0x08883988u, 0x00FF}, {0x088862A8u, 0x0003}, {0x08885E3Cu, 0x0003},
    {0x088A5B50u, 0x0005},      /* the sound's queue: a1 points at the caller's stack, which differs */
    {0x088B87B0u, 0x0003},      /* the file loader's queue */
};

static int kernel_bound_mask(uint32_t fn) {
    for (size_t i = 0; i < sizeof kernel_bound / sizeof kernel_bound[0]; i++)
        if (kernel_bound[i].fn == fn)
            return kernel_bound[i].mask;
    return -1;
}

int rt_trace_all(Ctx *c, int module, uint32_t fn) {
    (void)module;
    int bound = running && !stubbing ? kernel_bound_mask(fn) : -1;
    if (bound < 0 && (!stubbing || fn == target))
        return 0;
    int n = ncalls[current]++;
    if (n < MAX_CALLS) {
        Call *call = &calls[current][n];
        memset(call, 0, sizeof *call);
        call->fn = fn;
        uint16_t mask = bound >= 0 ? (uint16_t)bound : READS(fn);
        for (int i = 0; i < 8; i++) {
            if (mask >> i & 1)
                call->a[i] = c->r[4 + i];
            if (mask >> (8 + i) & 1)
                call->f[i] = c->f[12 + i].u;
        }
        if (fn == NINE_ARGUMENTS)
            call->stack0 = LW(c->r[29]);
        uint32_t hash = 2166136261u;
        for (uint32_t o = 0; o < HUNTER_HASHED; o += 4)
            hash = (hash ^ LW(stub_hunter + o)) * 16777619u;
        call->hunter_hash = hash;
        if (bound >= 0) {
            c->r[2] = 0;
            return 1;
        }
        c->r[2] = results[n];
        c->f[0].u = fresults[n];
        for (const Bias *b = stub_bias; b && b->fn; b++)
            if (b->fn == fn)
                c->r[2] = (uint32_t)b->values[bias_pick[n] % (uint32_t)b->count];
        if (fn == record_method) {
            c->r[2] = STUB_RECORD;
            SB(STUB_RECORD + RECORD_TYPE, stub_record_type[n]);
        }
        for (int i = 0; pokable && i < npokes; i++) {
            uint32_t at = (pokable[stub_poke[n][i]].in_game ? LW(GAME_POINTER) : stub_hunter) + pokable[stub_poke[n][i]].offset;
            int32_t value = stub_poke_value[n][i];
            if (pokable[stub_poke[n][i]].size == 1)
                SB(at, value);
            else if (pokable[stub_poke[n][i]].size == 2)
                SH(at, value);
            else
                SW(at, value);
        }
        /* two tests deep in regeneration, otherwise hardly ever passed */
        if (pokable && ((fn == REGEN_EFFECT_TEST && c->r[5] == 0x20) || (fn == SKILL_TEST && c->r[5] == 0x7A)) && (stub_record_type[n] & 3))
            c->r[2] = 1;
        if (stub_stamina[n] != 0x7FFF)
            SH(stub_hunter + 0x55A, stub_stamina[n]);
        if (stub_slot[n] != 0xFF)
            SB(LW(0x089CC558u) + 0x28, stub_slot[n]);
    }
    return 1;
}

static uint32_t seed = 12345;
static uint32_t rnd(void) {      /* xorshift: the same on every run */
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static uint32_t pick(const int32_t *from, int n) { return (uint32_t)from[rnd() % (uint32_t)n]; }
#define PICK(...) pick((const int32_t[]){__VA_ARGS__}, sizeof((const int32_t[]){__VA_ARGS__}) / sizeof(int32_t))

/* What the functions under test read, set to something their branches can tell apart */
static void randomise(Ctx *c, uint32_t hunter, uint32_t *a1, int *tick30) {
    HunterBase *h = (HunterBase *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    *a1 = PICK(-1, -1, -2, -3, -4, -5, -7, -10, -300, -301, -32768, 0, 1, 2, 4, 300, 32767, 0x12345, -0x12345);
    *tick30 = rnd() % 4 != 0;
    h->stamina = (int16_t)PICK(0, 1, 2, 5, 75, 76, 77, 150, 150, 299, 300, 300, 301, 32767, -1, -32768);
    h->stamina_max = (int16_t)PICK(0, 50, 150, 300, 32767, -1);
    SH(hunter + 0x1E6, rnd() % 3 == 0 ? rnd() % 4 : g->local_hunter_slot);
    g->tick_30ths = (uint16_t)(rnd() % 8 == 0 ? rnd() : rnd() % 64);
    SH(hunter + 0x622, PICK(0, 0, 0, 1, 5, -1));                /* (stamina loss blocked if positive) */
    SH(hunter + 0x6A0 + 5 * 2, PICK(0, 0, 1, -1));              /* (the other way it is blocked) */
    for (int i = 0; i < 6; i++)                                 /* the active skills */
        SB(hunter + 0x676 + i, PICK(0, 0, 0, 0xA8, 0xA9, 0x10));
    /* the state functions' */
    Hunter *self = (Hunter *)MEM(hunter);
    self->state_step = (uint8_t)PICK(0, 1, 1, 1, 1, 2, 2, 3, 3, 4);
    self->action_count = (int32_t)PICK(150, 2, 1, 0, -1);
    self->misc_flags = PICK(0, 8, 0xFF);
    self->motion_part0_flags = (uint16_t)PICK(0, 1, 2, 3);
    SW(LW(0x08A632C0u) + 0xF7A57Cu, PICK(0, 0, 1, 2));      /* (read by the sprints' input, when the slot has changed) */
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 30);
    self->motion_id = (uint16_t)PICK(3, 3, 0, 0x26);
    self->buttons_pressed = PICK(0, 0, 0x4000, 0x4000, 0x4010, 1, 2);
    self->stick_magnitude = (uint16_t)PICK(0, 39, 40, 127);
    self->quest_flags = PICK(0, 0, 0x100, 0x100);
    self->linked_monster_angle = rnd() & 0xFFFF;
    self->facing = rnd() & 0xFFFF;
    if (rnd() % 3 == 0)         /* just either side of a quarter turn */
        self->linked_monster_angle = (self->facing + PICK(0x2000, 0x2001, 0x1FFF, -0x6000, -0x5FFF, -0x6001)) & 0xFFFF;
    self->ground_forces_state_3b = (uint8_t)PICK(0, 0, 0, 1);
}

/* -- the hunter's upkeep, game_task 09A68FC8 ------------------------------------------------------------------------------------ */
#define T16 PICK(0, 0, 0, 1, 1, 2, 3, -1, 300, 0x7FFF, -0x8000)     /* a 16-bit timer: either sign counts */
#define T8 PICK(0, 0, 1, 1, 2, 0xFF)

/* What the upkeep reads after a call, so a callee may have changed it. All of them, for the versions to be held to reading afresh. */
static const PokeField upkeep_fields[] = {
    {0x1C, 2, 1}, {0x28, 1, 1},
    {0x1E1, 1, 0}, {0x1E6, 2, 0}, {0x1E8, 1, 0}, {0x298, 1, 0}, {0x299, 1, 0}, {0x2BA, 2, 0}, {0x2E4, 2, 0}, {0x2E6, 1, 0}, {0x35E, 2, 0},
    {0x388, 2, 0}, {0x3D6, 2, 0}, {0x3DA, 2, 0}, {0x410, 4, 0}, {0x41C, 2, 0}, {0x41E, 2, 0}, {0x424, 2, 0}, {0x440, 2, 0}, {0x442, 2, 0},
    {0x468, 2, 0}, {0x552, 1, 0}, {0x55A, 2, 0}, {0x564, 1, 0}, {0x5FE, 1, 0}, {0x610, 2, 0}, {0x620, 2, 0}, {0x622, 2, 0}, {0x624, 2, 0},
    {0x626, 2, 0}, {0x628, 2, 0}, {0x62A, 2, 0}, {0x640, 1, 0}, {0x641, 1, 0}, {0x672, 2, 0}, {0x6A0, 2, 0}, {0x6A6, 2, 0}, {0x6AA, 2, 0},
    {0x6AE, 2, 0}, {0x6B2, 2, 0}, {0x6B4, 2, 0}, {0x6C8, 2, 0}, {0x10F4, 2, 0}, {0x10F6, 2, 0}, {0x10F8, 2, 0}, {0x10FA, 2, 0}, {0x110E, 2, 0},
    {0x1112, 2, 0}, {0x111C, 4, 0}, {0x1138, 2, 0}, {0x1140, 2, 0}, {0x1142, 2, 0}, {0x1146, 2, 0}, {0x11B4, 2, 0},
};

const struct Pokes upkeep_pokes = {upkeep_fields, sizeof upkeep_fields / sizeof upkeep_fields[0], NULL};

static void randomise_upkeep(Ctx *c, uint32_t hunter) {
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    if (rnd() % 8 == 0)         /* not the local player, in the part of the word only the whole comparison sees */
        self->instance_index = (uint16_t)(g->local_hunter_slot + 0x100);
    /* a count on the boundaries of what divides it, then as it would be if read signed */
    g->tick_30ths = (uint16_t)(rnd() % 4 == 0 ? rnd() : PICK(0, 1, 2, 3, 10, 11, 15, 30, 60, 120, 240, 480, 241, 0xFFF0, 0xFFFA, 0xFFFF, 0x8000));
    self->reaction_block_weapon_ticks = (int16_t)T16;
    self->push_off_ticks = (uint16_t)T16;
    self->hit_immune_ticks = (uint16_t)T16;
    self->reaction_block_item_ticks = (int16_t)T16;
    self->menu_camera_lock_ticks = (uint8_t)T8;
    self->repeat_use_ticks = (int16_t)T16;
    self->stamina_loss_block_ticks = (int16_t)T16;
    self->area_1_2_ward_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 3, 0x8000, 0xFFFF);
    self->area_3_4_ward_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 3, 0x8000, 0xFFFF);
    self->health_regen_boost_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 0xFFFF);
    self->map_detect_item_ticks = (uint16_t)T16;
    self->map_detect_ticks = (uint16_t)T16;
    self->map_detect_skill_ticks = (uint16_t)T16;
    self->map_detect_skill_roll_ticks = (uint16_t)PICK(0, 0, 1, 2, 9000);
    self->attack_bonus_ticks = (uint16_t)PICK(0, 0, 1, 1, 2, 0xFFFF);
    self->attack_bonus_timed = (int8_t)PICK(0, 5, -1);
    self->defense_bonus_ticks = (uint16_t)PICK(0, 0, 1, 1, 2, 0xFFFF);
    self->defense_bonus_timed = (int8_t)PICK(0, 5, -1);
    self->mash_status_left = (int16_t)T16;
    self->weapon_mode_ticks = (int16_t)PICK(0, 0, 0, 0, 1, 2, -1, 300);
    self->item_restrict_ticks = (int16_t)T16;
    self->defense_halved_ticks = (int16_t)T16;
    self->stamina_debuff_ticks = (int16_t)T16;
    self->weapon_class = (uint8_t)PICK(2, 2, 3, 3, 7, 7, 9, 9, 10, 10, 0, 1, 0x82);
    self->weapon7_gauge = (int16_t)PICK(0, 1, 100, -1);
    self->charge_time = (int16_t)PICK(0, 0, 1, 1, 2, -1, 300);
    self->quest_flags = PICK(0, 0x10, -1, ~0x10);
    self->misc_flags = PICK(0, 0, 0, 0, 0, 0x10, 0x10, ~0x10, 0xFF, -1);
    self->state_group = (uint8_t)PICK(0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 3, 5, 5, 4, 6, 0xFF);
    /* the states each group and weapon class lists, and their neighbours */
    if (rnd() % 4 == 0)
        self->state = (uint8_t)rnd();
    else if (self->state_group == 0)
        self->state = (uint8_t)PICK(0x25, 0x7E, 0x6E, 0x6D, 0x6C, 0x6B, 0x44, 0x32, 0x31, 0x2D, 0x1C, 0x78, 0x77, 0x76, 0x6A, 0x47, 0x4A, 0x5B);
    else if (self->state_group == 1 && self->weapon_class == 10)
        self->state = (uint8_t)PICK(0x12, 0x0A, 0x08, 0x09, 0x11);
    else if (self->state_group == 1 && self->weapon_class == 3)
        self->state = (uint8_t)PICK(0x10, 0x05, 0x0E, 0x03, 0x04);
    else if (self->state_group == 1 && self->weapon_class == 2)
        self->state = (uint8_t)PICK(0x04, 0x0E, 0x0D, 0x0C, 0x03, 0x05);
    else
        self->state = (uint8_t)PICK(0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x0A, 0x0B, 0x0C, 0x0D);
    self->buildup_decay_delay_ticks = (int16_t)PICK(0, 1, 2, -1, -0x8000);
    self->buildup_state_2_13 = (int16_t)PICK(0, 1, 50, -1);
    self->flag_1000_lockout_ticks = (uint8_t)T8;
    self->weapon_selector_lockout_ticks = (uint8_t)T8;
    self->item_use_lockout_ticks = (uint8_t)T8;
    self->listed_item_ticks = (uint16_t)PICK(0, 0, 1, 2, 0xFFFF, 0x8000);
    self->ticks_2e6 = (uint8_t)T8;
    /* regeneration */
    self->ground_effect_kind = (uint8_t)PICK(0, 0, 0, 0, 1, 2, 3);
    self->status_bits = (uint16_t)PICK(0, 0, 0, 0x80, 0x7F, 0xFF80);
    self->drain_status_seconds = (int16_t)PICK(0, 0, 0, 0, 0, 1, -1);
    self->health_regen_effect_level = (uint8_t)PICK(0, 1, 2, 3, 4, 0xFF);
    int16_t health = (int16_t)PICK(-1, 0, 1, 2, 50, 99, 100, 150, 0x7FFF, -0x8000);
    self->health = health;
    self->health_red_limit = (int16_t)(rnd() % 4 == 0 ? (int32_t)PICK(0, 100, 150, -1, 0x7FFF, -0x8000) : health + (int32_t)PICK(0, 1, 2, 3, 4, 5, -1));
    self->health_max = (int16_t)PICK(100, 150, -1);
    self->health_max_base = (int16_t)(rnd() % 2 ? (int32_t)PICK(0, 100, 150, -1) : (rnd() % 2 ? health : self->health_red_limit) + (int32_t)PICK(0, 1, -1));
    for (int i = 0; i < 6; i++)     /* for the real callee */
        SB(hunter + 0x676 + i, PICK(0, 0, 0x95, 0x7B, 0x7A, 0x0B, 0xD0, 0xD6, 0xB2, 0xD4, 0x18, 0x17, 0x19, 0x1A));
    /* the timed effects */
    for (int i = 0; i < 21; i++) {
        SH(hunter + 0x6A0 + i * 2, PICK(0, 0, 1, 1, 2, -1, 300));
        SB(hunter + 0x5F7 + i, PICK(0, 1, 3));
        SB(hunter + 0x114C + i, PICK(0, 1, 3));
    }
    if (!record_method) {           /* not stubs: what the method really returns, its type our choice */
        uint32_t holder = LW(LW(0x09A4F1A0u));
        c->r[4] = holder;
        c->r[29] = TEST_SP;
        rt_call(c, LW(LW(holder) + 0x28));
        SB(c->r[2] + RECORD_TYPE, PICK(0, 1, 2, 3, 4));
    }
}

/* -- the small functions' inputs ------------------------------------------------------------------------------------------------ */

/* The local player's slot most of the time, or another, or one only the whole 16 bits tell from it */
static uint16_t pick_slot(Ctx *c) {
    uint8_t local = ((GameState *)MEM(LW(GAME_POINTER)))->local_hunter_slot;
    switch (rnd() % 4) {
    case 0: return (uint16_t)(rnd() % 4);
    case 1: return (uint16_t)(local + 0x100);
    default: return local;
    }
}

/* A byte list, and just outside it, with everything but the id; then the id in it, or just outside, or nowhere */
static void plant_id(uint8_t *list, int n, uint8_t id) {
    for (int i = -1; i <= n; i++)
        list[i] = (uint8_t)(id + 1 + rnd() % 200);
    int at = (int)(rnd() % (uint32_t)(2 * n)) - 1;
    if (at <= n)
        list[at] = id;
}

/* an id in the low byte: what's above it isn't to count */
static uint32_t with_high_bits(uint32_t low) { return low | PICK(0, 0, 0x100, 0xFFFFFF00, 0x7F00); }

static void inputs_loss_blocked(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    ((Hunter *)MEM(hunter))->instance_index = pick_slot(c);
}

static void inputs_active_skill(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    uint8_t id = (uint8_t)(rnd() % 4 ? rnd() : 0);
    plant_id(MEM(hunter + 0x676), 6, id);
    args[0] = with_high_bits(id);
}

static void inputs_hunt_bonus(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    uint8_t id = (uint8_t)(rnd() % 4 ? rnd() : 0);
    uint32_t game = LW(GAME_POINTER);
    uint16_t slot = pick_slot(c);
    ((Hunter *)MEM(hunter))->instance_index = slot;
    plant_id(MEM(game + 0x69942), 5, id);
    plant_id(MEM(game + 0xA9 + slot * 0x8Cu), 5, id);
    args[0] = with_high_bits(id);
}

static void inputs_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = pick_slot(c);
    self->reset_place_mode = (uint8_t)PICK(0, 0, 1, 2, 3, 0x80);
    self->misc_flags = rnd();
    args[0] = PICK(0, 0, 1, 2, 4, 4, 5, 0x104, 0x8004, 0x10004, 0xFFFF0004);
    args[1] = rnd() % 0x40;
    args[2] = rnd() % 8;
}

static void inputs_face_stick(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->stick_angle = (uint16_t)rnd();
    self->facing_target = (uint16_t)rnd();
    uint32_t camera = LW(0x089CBD04u);      /* for the real callee */
    SB(camera + 0xDC4, PICK(0, 0, -1));
    SB(camera + 0xA8D, PICK(0, 0, 1));
    SB(camera + 0x1DE, PICK(0, 1, 1));
}

static void inputs_set_flags(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->state_flags = rnd();
    self->state_flags_high = rnd();
    args[0] = PICK(0, 1, 0x80000000, 0x80000001, 0x7FFFFFFF, -1);
    if (rnd() % 2)
        args[0] = rnd();
}

static void inputs_state_is(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->state_group = (uint8_t)(rnd() % 4);
    self->state = (uint8_t)(rnd() % 4);
    args[0] = with_high_bits(rnd() % 2 ? self->state_group : rnd() % 4);
    args[1] = with_high_bits(rnd() % 2 ? self->state : rnd() % 4);
}

static void inputs_movement(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->movement_mode = (uint8_t)PICK(0, 1, 2, 3, 0xFF);
    self->state_flags = rnd();
    self->state_flags_high = rnd();
    args[0] = PICK(0, 1, 2, 3, 4, 0x8000, 0x8001, 0x8002, 0x8003, 0x101, 0x102, 0x7F02, 0xFF01, 0x10000, 0x10002, 0x18000, 0xFF);
    args[1] = PICK(0, 0, 1, -1, 0x8000, 0x10000, 0xFFFF0000);
    args[2] = PICK(0, 0, 1, -1, 0x8000, 0x10000, 0xFFFF0000);
}

static void inputs_classify(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->weapon_class = (uint8_t)(rnd() % 3 == 0 ? PICK(1, 5, 10) : rnd() % 3 == 0 ? rnd() : rnd() % 12);
    self->misc_flags = PICK(0, 0, 8, ~8, -1);
    self->hunter_flags = PICK(0, 0x1000, ~0x1000, -1);
    self->stick_magnitude = (uint16_t)PICK(0, 39, 40, 41, 84, 85, 86, 119, 120, 121, 127, 0x7FFF, 0x8000, 0xFFFF);
    self->movement_mode = (uint8_t)PICK(0, 0, 0, 1, 2, 0x80);
    self->buttons_held = PICK(0, 0x200, 0x200, ~0x200, -1);
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 30, -1);
    self->state_group = (uint8_t)PICK(0, 0, 1);         /* for the real callee */
    self->state = (uint8_t)PICK(0x24, 0x24, 0x25, 0);
}

static void inputs_countdowns(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->refusal_message_cooldown = (uint8_t)PICK(0, 0, 1, 2, 0x80, 0xFF);
    self->sound_cue_mute_timer = (uint8_t)PICK(0, 0, 1, 2, 0x80, 0xFF);
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 2, 30, -1, -0x8000, 0x7FFF, 0x100);
    self->ticks_5f0 = (int8_t)PICK(0, 0, 1, 2, 5, 50, -1, -0x80, 0x7F);
}

/* -- the hunter's states, its tick and its input -------------------------------------------------------------------------------- */
#define POKES(name, bias, ...) static const PokeField name##_fields[] = {__VA_ARGS__}; \
    const struct Pokes name = {name##_fields, sizeof name##_fields / sizeof name##_fields[0], bias}
static const Bias tick_bias[] = {
    {0x088DE7C8u, 4, {1, 1, 1, 0}},         /* the assets wanted */
    {0x0884FA5Cu, 4, {0, 0, 0, 1}},
    {0x088C7424u, 5, {0, 1, 2, 0xE94F, 0xE950}},    /* the area's asset: added to the base, also "none" and 0 */
    {0x088564F8u, 5, {0, 1, 2, 0xE94F, 0xE950}},
    {0, 0, {0}},
};
static const Bias common_bias[] = {
    {0x09AA5FB0u, 8, {0, 1, 2, 3, 3, 5, 5, 0x105}},     /* the move input */
    {0x08865C88u, 8, {0, 0, 0x200, 0x1000, 0x400, 0x2000000, 0x600, 0x40}},     /* the state flags asked about */
    {0x0884FA5Cu, 8, {0, 0, 0, 0, 0, 0, 1, 0x101}},     /* each answer of 1 ends it */
    {0x09AA8FC8u, 4, {0, 1, 1, 0x101}},
    {0x088D6A54u, 4, {0, 0, 1, 0x100}},
    {0, 0, {0}},
};
POKES(set_state_pokes, NULL, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x298, 1, 0}, {0x322, 2, 0}, {0x410, 4, 0}, {0x614, 4, 0}, {0x288, 4, 0}, {0x28C, 4, 0});
POKES(reset_pokes, NULL, {0x29C, 1, 0}, {0x440, 2, 0}, {0x614, 4, 0}, {0x280, 1, 0});
POKES(tick_pokes, tick_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x4, 4, 0}, {0x298, 1, 0}, {0x29A, 2, 0}, {0x2E4, 2, 0}, {0x6A0, 2, 0}, {0x111C, 4, 0});
POKES(copy_pad_pokes, NULL, {0x614, 4, 0}, {0x480, 4, 0}, {0x484, 4, 0}, {0x488, 4, 0}, {0x61C, 4, 0});
POKES(common_pokes, common_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x2E, 1, 1}, {0x299, 1, 0}, {0x29C, 1, 0}, {0x1E0, 1, 0}, {0x1E8, 1, 0}, {0x2C0, 4, 0}, {0x1F4, 4, 0},
      {0x320, 2, 0}, {0x410, 4, 0}, {0x440, 2, 0}, {0x480, 4, 0}, {0x484, 4, 0}, {0x49E, 2, 0}, {0x55A, 2, 0}, {0x565, 1, 0}, {0x614, 4, 0},
      {0x641, 1, 0}, {0x674, 1, 0}, {0x111C, 4, 0}, {0x1120, 4, 0});

static void inputs_overlays(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    for (uint32_t o = 0x1A8; o < 0x1C0; o++)    /* and those it leaves alone */
        SB(hunter + o, rnd());
}

static void inputs_airborne(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)hunter;
    args[0] = PICK(0, 1, 2, 3, 4, 7, -1, -2, -3, 0x7FFF, 0x8000, 0x10001, 0xFFFF0002);
    (void)c;
}

static void inputs_transient(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->weapon_class = (uint8_t)PICK(0, 8, 9, 9, 10, 0x89);
    self->charge_time = (int16_t)rnd();
    self->hunter_flags = rnd();
    self->state_flags = rnd();
    self->misc_flags = rnd();
    for (uint32_t o = 0x282; o < 0x284; o++)
        SB(hunter + o, rnd());
    self->listed_item_ticks = (uint16_t)rnd();
    self->life_state = (uint8_t)rnd();
    self->flagged_ticks = (uint16_t)rnd();
    self->hit_immune_ticks = (uint16_t)rnd();
    self->transient_45e = (uint16_t)rnd();
    self->flag288_phase1_ticks = (uint8_t)rnd();
    self->flag288_phase2_ticks = (uint8_t)rnd();
    self->action_count = (int32_t)rnd();
}

static void inputs_real_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = pick_slot(c);
    self->reset_place_mode = (uint8_t)PICK(0, 0, 0, 1, 3);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3);
    self->state = (uint8_t)rnd();
    self->state_random = (uint16_t)rnd();
    self->misc_flags = rnd();
    self->hunter_flags = rnd();
    self->state_flags = rnd();
    self->signed_overlay_blend = (int8_t)rnd();
    args[0] = PICK(0, 0, 0, 1, 2, 2, 4, 0x104, 0x10004, 0x8002);
    args[1] = rnd() % 4 ? rnd() % 0x80 : rnd();
    args[2] = PICK(0, 1, 2, 3, 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x33, 0x10000, 0x10021, 0xFFFF0010, 4, 0x40);
}

static void inputs_reset(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 1, 0x80);
    self->buildup_state_2_13 = (int16_t)PICK(0, 49, 50, 51, -1, 0x7FFF);
    self->hunter_flags = rnd();
    self->movement_mode = (uint8_t)PICK(0, 2, 0xF);
    args[0] = PICK(0, 1, 2, 2, 3, 0x80, 0x82, 0x81, 0x102, 0x10002, 0xFFFFFF02);
    args[1] = PICK(0, 4, 5, 8, 0x8000, 0x10003, -3);
    args[2] = PICK(0, 1, 4, 0x10000);
    (void)c;
}

static void inputs_run_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6, 7, 8, 0x80, 0xFF);
    self->hunter_flags = rnd();
    self->misc_flags = rnd();
    (void)c;
}

static void inputs_group0(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    ((Hunter *)MEM(hunter))->state = (uint8_t)(rnd() % 8 ? rnd() % 0x80 : rnd());
    (void)c;
}

static void inputs_tick(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = pick_slot(c);
    self->area_id = (uint16_t)(rnd() % 8 ? g->stage_id : g->stage_id ^ PICK(1, 0x100, 0x8000));
    self->flags = rnd();
    self->timed_effect_ticks = (int16_t)PICK(0, 1, 2, -1, 300);
    self->quest_flags = PICK(0, 2, ~2, -1);
    self->health = (int16_t)PICK(-1, 0, 1, 100);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3, 3);
    self->reset_place_mode = (uint8_t)rnd();
    uint32_t stage = LW(0x09A44D9Cu), assets = LW(0x09A4F210u);
    SB(stage + 0x5C, PICK(0, 1, 1, 0x80));
    SW(stage + 0x54, rnd());
    SH(assets + 0xFC, PICK(0, 1, 2, 3, 0xFFFC, 0xFFFE));
    SH(assets + 0xFE, PICK(0x16B0, 0x16B1, 0x16B2, 0xFFFF, 0));
    for (uint32_t i = 1; i < 23; i++)   /* one past the last, too */
        SH(assets + 0xFC + i * 12, rnd());
    SW(assets + 0x204, rnd());
    SW(assets + 0x208, rnd());
}

static void inputs_set_motion(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    ((Hunter *)MEM(hunter))->hunter_flags = rnd();
    args[0] = rnd() % 2 ? rnd() % 0x400 : rnd();
    args[1] = PICK(0, 1, 2, 3, 30, -1, -2, -3, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x20001, 0x18000);
    args[2] = PICK(0, 1, 30, -1, 0x10002);
    (void)c;
}

static void inputs_motion_copy(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->motion_id = (uint16_t)rnd();
    self->motion_id_copy = rnd() % 2 ? self->motion_id : (uint16_t)rnd();
    self->effect_cooldown = (uint16_t)PICK(0, 0, 1, 2, 0x8000, 0xFFFF);
    (void)c;
}

static void inputs_copy_pad(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    SW(0x08A62D88u, rnd());
    SW(0x08A62D8Cu, rnd());
    SW(0x08A62D90u, rnd());
    SW(0x08A62D94u, rnd());
    SH(0x08A62D98u, rnd());
    SH(0x08A62D9Au, rnd());
    self->hunter_flags = PICK(0, 0, 0, 1, 1, 2, 0x200, 3, 0x201, ~3 & ~0x200, -1);
    self->l_held_frames = (int32_t)PICK(0, 1, 9, 10, -1);
    self->buttons_word_498 = rnd();
    uint32_t hud = LW(0x089CBD0Cu);
    ((HudContext *)MEM(hud))->hud_flags = PICK(0, 0x20, ~0x20, -1);
}

static void inputs_common(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    g->quest_state = (uint8_t)PICK(0, 0, 0, 0, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 0, 0, 1, 0x80);
    self->misc_flags = PICK(0, 8, ~8, -1);
    self->hunter_flags = rnd() & rnd();
    self->buttons_pressed = rnd() & rnd();
    self->buttons_held = rnd() & rnd();
    self->state = (uint8_t)PICK(0, 0, 0, 0, 0x40, 0x40, 1, 3, 0x12, 0x13, 0x1F, 0x24, 0x2C, 0x3B, 0x3F, 0x49, 0x49, 0x49, 0x74);
    self->ground_forces_state_3b = (uint8_t)PICK(0, 0, 0, 1, 0x80);
    self->hud_request = (uint8_t)PICK(0, 0, 1);
    self->buildup_state_2_13 = (int16_t)PICK(0, 0, 0, 49, 50, -1);
    self->item_use_lockout_ticks = (uint8_t)PICK(0, 0, 1);
    self->selector_input_flags = (uint8_t)rnd();
    self->button_action = PICK(0, 1, 1, 2, 2, 3, 0x101);
    self->quest_flags = PICK(0, 0x100, ~0x100, -1);
    self->stick_magnitude = (uint16_t)PICK(0, 84, 85, 86, 127, 0x8000);
    self->stamina = (int16_t)PICK(0, 74, 75, 76, 300, -1);
    self->weapon_class = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x80);
    self->facing = rnd();
    self->linked_monster_angle = self->facing + PICK(0x2000, 0x2001, 0x1FFF, -0x2000, -0x2001, 0x10000);
    self->facing_target = (uint16_t)(self->facing + PICK(0, 0x5FFF, 0x6000, 0x6001, 0x9FFF, 0xA000, 0xA001, 0xFFFF));
    args[0] = PICK(0, 0, 1, 2, 3, 0x101, 0x102);
    args[1] = PICK(0, 0, 0, 1, 0x100, 0x101);
    SW(LW(0x08A632C0u) + 0xF7A57Cu, PICK(0, 1, 2, 3));
}

/* -- floats: bits on the boundaries a function tests, either side of them, and what compares false either way ------------------------ */
#define FLOAT_NAN 0x7FC00001u
#define FLOAT_SIGNALLING_NAN 0x7F800001u
#define FLOAT_INFINITY 0x7F800000u
static uint32_t float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}
#define SET_FLOAT_BITS(field, bits) memcpy(&(field), &(uint32_t){bits}, sizeof(uint32_t))

static void inputs_stamina_scaled(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    ((Hunter *)MEM(hunter))->instance_index = pick_slot(c);
    args[0] = PICK(-1, -2, -3, -4, -5, -7, -100, -300, 1, 2, 3, 4, 7, 100, 300, 0x7FFF, -0x8000, 0x10000, 0x10003, 0x12345, 0x7FFFFFFF, 0x80000000, 21845, 16777217);
    if (rnd() % 2)      /* large enough that the factor's last bit shows */
        args[0] = (rnd() & 0x3FFFFFF) * PICK(1, -1);
}

static void inputs_motion_update(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->freeze_ticks = (uint8_t)PICK(0, 0, 0, 0, 1, 0x80);
    self->motion_slow_timer = (uint8_t)PICK(0, 0, 0, 0, 1, 2, 0x80, 0xFF);
    self->motion_id = (uint16_t)PICK(0x23, 0x22, 0x731, 3, 0x1A7, 0x1A6, 0x1A3, 0x1A2, 0x1A0, 0x19F, 0x19D, 0x19C, 0x19B, 0x197, 0x196, 0x195, 0x58A, 0x587,
                                     0x24, 0x21, 2, 4, 0x1A8, 0x1A5, 0x1A4, 0x1A1, 0x19E, 0x19A, 0x198, 0x194, 0x58B, 0x589, 0x588, 0x586, 0x732, 0x730);
    self->motion_id_layer1 = (uint16_t)rnd();
    self->hunter_flags = PICK(0, 0x80, 0x100, 0x180, ~0x180, -1);
    self->motion_start_frame = (uint16_t)rnd();
    self->part1_motion_start_frame = (uint16_t)rnd();
    self->motion_blend_frames = (int16_t)rnd();
    self->part1_motion_blend_frames = (int16_t)rnd();
    SET_FLOAT_BITS(self->motion_part0_speed, PICK(float_bits(2.0f), float_bits(1.9999999f), float_bits(2.0000002f), 0, 0x80000000, float_bits(-1.0f), float_bits(3.5f), FLOAT_NAN, FLOAT_SIGNALLING_NAN, FLOAT_INFINITY));
    SET_FLOAT_BITS(self->motion_part1_speed, rnd());
    SET_FLOAT_BITS(self->motion_speed_scale, PICK(float_bits(1.0f), float_bits(0.5f), float_bits(1.1f), float_bits(0.3f), float_bits(1e-40f), 0x80000000, FLOAT_NAN, FLOAT_INFINITY));
    ((GameState *)MEM(LW(GAME_POINTER)))->mode_flags = PICK(0, 1, 2, 3, ~1, -1);
    if (!record_method) {       /* callees running for real: a motion that isn't one, or a speed that isn't a number, crashes them, whichever version runs */
        self->hunter_flags |= 0x180;
        SET_FLOAT_BITS(self->motion_part0_speed, PICK(float_bits(2.0f), float_bits(1.9999999f), float_bits(2.0000002f), 0, float_bits(3.5f)));
        SET_FLOAT_BITS(self->motion_speed_scale, PICK(float_bits(1.0f), float_bits(0.5f), float_bits(1.1f), float_bits(0.3f)));
    }
}

static const Bias run_bias[] = {
    {0x08865CB8u, 8, {0, 0, 0, 1, 1, 0x100, 0x101, 2}},     /* the state's tests */
    {0x088D69F0u, 6, {0, 1, 2, 3, 0x101, 0x102}},
    {0x08865BACu, 6, {1, 1, 1, 0, 0x100, 0x101}},           /* in the stage being played */
    {0x09AA8958u, 5, {0, 0, 1, 0x100, 2}},
    {0x09A8BBE0u, 3, {0, 1, 0x100}},
    {0x088DACA4u, 5, {0, 1, 1, 0x101, 2}},
    {0x09AA6B18u, 8, {0x18, 0x21, 0x1E, 0x10018, 0x1001E, 0x10021, 0, 0x19}},
    {0x09C3E698u, 4, {0, 1, 2, 0x101}},
    {0x09BB0FE0u, 4, {0, 1, 0x10000, 0x10001}},
    {0x09A8BB20u, 4, {0, 1, 0x100, 0x101}},
    {0x08814048u, 3, {0, 1, 0x101}},
    {0, 0, {0}},
};
POKES(run_pokes, run_bias, {0x4, 4, 0}, {0x28, 1, 1}, {0x1D6, 1, 0}, {0x1E0, 1, 0}, {0x1E6, 2, 0}, {0x1E8, 1, 0}, {0x200, 4, 0}, {0x204, 4, 0}, {0x208, 4, 0}, {0x20C, 4, 0},
      {0x280, 1, 0}, {0x298, 1, 0}, {0x299, 1, 0}, {0x29C, 1, 0}, {0x2CC, 4, 0}, {0x2E4, 2, 0}, {0x33C, 1, 0}, {0x410, 4, 0}, {0x424, 2, 0}, {0x484, 4, 0},
      {0x564, 1, 0}, {0x564, 1, 0}, {0x564, 1, 0}, {0x5F3, 1, 0}, {0x614, 4, 0}, {0x111C, 4, 0});

static void inputs_state_run(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    self->hunter_flags = rnd() & rnd();
    self->misc_flags = rnd() & rnd();
    self->flags = rnd();
    self->health = (int16_t)PICK(1, 1, 1, 100, 0, -1);
    self->freeze_ticks = (uint8_t)PICK(0, 0, 0, 1);
    self->state_group = (uint8_t)PICK(0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7);
    self->state = (uint8_t)(rnd() % 2 ? rnd() % 0x80 : PICK(1, 3, 9, 0x13, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0x23, 0x24, 0x27, 0x3A, 0x36, 0x65, 0x66, 0x67, 0x6A, 0x7E, 0x7D, 0x7F));
    self->state_scratch_a = (uint8_t)PICK(0, 0, 1, 8, 8, 9, 9, 10, 0xFF);
    self->movement_mode = (uint8_t)PICK(0, 0, 0, 2, 2, 1);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 0, 0, 1);
    self->ground_forces_state_3b = (uint8_t)PICK(0, 1, 1, 0x80);
    self->weapon_class = (uint8_t)PICK(0, 1, 1, 5, 5, 8, 8, 9, 10);
    self->weapon_uses_load = (uint8_t)PICK(0, 1, 2);
    self->status_bits = (uint16_t)PICK(0, 0x200, 0x800, 0x1000, 0x1800, ~0x1A00, -1);
    self->buttons_pressed = rnd() & rnd();
    self->quest_flags = PICK(0, 2, ~2, -1);
    self->flag_1000_lockout_ticks = (uint8_t)PICK(0, 0, 1, 10);
    self->ledge_grabs_from_fall = (int8_t)rnd();
    self->hud_request = (uint8_t)PICK(0, 1);
    self->selector_input_flags = (uint8_t)rnd();
    self->place[0] = (float)(int16_t)rnd() / 16.0f;
    self->place[2] = (float)(int16_t)rnd() / 16.0f;
    SET_FLOAT_BITS(self->place_w, rnd());
    /* above or below the floor, around where the fall starts, and NaN each way round */
    float floor_y = (float)(int16_t)rnd() / 8.0f;
    uint32_t y = float_bits(floor_y + (float)PICK(-1, 0, 1, 29, 30, 31, 100) * 1.0f);
    switch (rnd() % 8) {
    case 0: y = float_bits(floor_y + 29.999998f); break;
    case 1: y = float_bits(floor_y + 30.000002f); break;
    case 2: y = FLOAT_NAN; break;
    case 3: y = FLOAT_SIGNALLING_NAN; break;
    default: break;
    }
    SET_FLOAT_BITS(self->place[1], y);
    self->floor_y = floor_y;
    if (rnd() % 16 == 0)
        SET_FLOAT_BITS(self->floor_y, FLOAT_NAN);
    /* the two long ways: starting to fall, and flag 0x1000 with the weapon out */
    if (rnd() % 4 == 0) {
        self->instance_index = g->local_hunter_slot;
        self->movement_mode = 0;
        self->attached_part_mode = 0;
        self->health = 1;
        self->floor_y = 0.0f;
        SET_FLOAT_BITS(self->place[1], PICK(float_bits(29.999998f), float_bits(30.0f), float_bits(30.000002f), float_bits(31.0f)));
        self->state_group = (uint8_t)PICK(0, 0, 2, 2, 2, 5, 1, 6);
        self->state = (uint8_t)PICK(0x7E, 0x7D, 0x7F, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x23, 0x23, 0x23, 0x24, 0x24, 0);
    } else if (rnd() % 3 == 0) {
        self->instance_index = g->local_hunter_slot;
        self->misc_flags |= 8;
        self->weapon_class = (uint8_t)PICK(1, 5);
        self->flag_1000_lockout_ticks = (uint8_t)PICK(0, 0, 0, 1);
        self->state_group = (uint8_t)PICK(0, 0, 0, 0, 1, 6, 5);
        self->state = (uint8_t)PICK(0x3A, 0x1B, 0x1C, 0x67, 0x68, 0x69, 0x6A, 0x66, 0x6B, 0x1A, 0);
        self->hunter_flags = (self->hunter_flags & ~0x9000u) | PICK(0, 0x8000, 0x8000, 0x1000);
    } else if (rnd() % 2 == 0) {     /* forced by the ground, and the states it isn't */
        self->state_group = 0;
        self->state = (uint8_t)PICK(0x08, 0x09, 0x0F, 0x14, 0x1A, 0x1C, 0x1D, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x3B, 0x3C, 0x3D, 0x44, 0x59, 0x5D, 0x60, 0x7D, 0x7E, 0x07, 0x0A, 0x15, 0x1E);
        self->ground_forces_state_3b = 1;
        self->movement_mode = 0;
    }
    if (!record_method) {       /* callees running for real: the faint and end states' reach further than this state can hold up */
        if (self->state_group == 3 || self->state_group == 4 || self->state_group > 6)
            self->state_group = 0;
    }
    uint32_t camera = LW(0x089CBD04u);
    SB(camera + 0x1DF, PICK(0, 0, 0, 1, 0x80));
    SB(camera + 0xA8D, PICK(0, 0, 0, 1));
    g->option_camera_follow_mode = (int8_t)PICK(0, 0, 1, 2, -1, 0x41);
    SB(LW(0x09A1DC2Cu), PICK(0, 1, 2, 3));
    SW(LW(0x08A632C0u) + 0xF7A554u, PICK(30, 60, 0, 5, 10, 15, 20, 25, 7, 8, 9, 11, 13, 14, -30, -5, -10, -7, 0x7FFFFFFF));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <RAM snapshot> [iterations] [stubs <signatures.tsv>] [only <part of a case's name>]\n", argv[0]);
        return 2;
    }
    int iterations = argc > 2 ? atoi(argv[2]) : 3000;
    int stub_mode = 0;
    const char *only = NULL;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "stubs")) {
            stub_mode = 1;
            load_signatures(argv[i + 1]);
        } else if (!strcmp(argv[i], "only")) {
            only = argv[i + 1];
        }
    }
    static Ctx ctx;
    Ctx *c = &ctx;
    c->mem = rt_mem_init();
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(c->mem + SNAP_START, 1, SNAP_SIZE, f) != SNAP_SIZE) {
        fprintf(stderr, "%s: can't read a snapshot\n", argv[1]);
        return 2;
    }
    fclose(f);
    uint32_t hunter = 0;
    for (uint32_t a = SNAP_START; a < SNAP_START + SNAP_SIZE; a += 4)
        if (LW(a) == HUNTER_VTABLE) {
            hunter = a;
            break;
        }
    if (!hunter) {
        fprintf(stderr, "no hunter in this snapshot\n");
        return 2;
    }
    stub_hunter = hunter;
    if (stub_mode)
        record_method = LW(LW(LW(LW(0x09A4F1A0u))) + 0x28);
    uint8_t *start = malloc(SNAP_SIZE), *generated = malloc(SNAP_SIZE);
    int failed = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        if (only && !strstr(cases[k].name, only))
            continue;
        int bad = 0;
        for (int i = 0; i < iterations && bad < 5; i++) {
            uint32_t a1;
            int tick30;
            randomise(c, hunter, &a1, &tick30);
            /* how rarely a call does anything: a chain of 14 tests all answering 0 isn't reached otherwise */
            uint32_t quiet = 0;
            const struct Pokes *pokes = cases[k].pokes;
            pokable = NULL;
            stub_bias = NULL;
            if (cases[k].randomise_more)
                cases[k].randomise_more(c, hunter);
            if (pokes) {
                quiet = PICK(0, 4, 8, 16, 32, 128);
                pokable = pokes->fields;
                stub_bias = pokes->bias;
                npokes = PICK(0, 0, 1, 4, MAX_POKES);
            }
            if (cases[k].small_a1)
                a1 = rnd() % 3;
            uint32_t args[3] = {a1, 0xBAD00006u, 0xBAD00007u};
            if (cases[k].inputs) {
                cases[k].inputs(c, hunter, args);
                a1 = args[0];
            }
            if (stub_mode)      /* what a version leaves unset on its stack shows, where a call's argument is compared */
                memset(c->mem + STACK_LOW, 0xA5, TEST_SP - STACK_LOW);
            memcpy(start, c->mem + SNAP_START, SNAP_SIZE);
            uint32_t saved[9], after[2][9], v0[2];
            for (int n = 0; n < MAX_CALLS; n++) {
                results[n] = PICK(0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 5, 0x105, 0x100, 0x101);   /* (a classifier's, a test's, a low byte's) */
                fresults[n] = rnd();
                bias_pick[n] = rnd();
                stub_stamina[n] = (int16_t)PICK(0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0, 0, -1, 1, 76, 300);
                stub_slot[n] = (uint8_t)PICK(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 2);
                stub_record_type[n] = (uint8_t)PICK(0, 0, 1, 2, 3, 4, 0x82);
                if (pokes) {
                    /* low bytes, low halves and whole words told apart */
                    results[n] = PICK(0, 0, 1, 1, 1, 2, 3, 0x100, 0x101, 0x10000, 0x10002, 0x8000, 0xFFFF, 0x1FFFF, -1, 5, 0x105);
                    if (quiet && rnd() % quiet) {
                        results[n] = 0;
                        stub_stamina[n] = 0x7FFF;
                        stub_slot[n] = 0xFF;
                    }
                    for (int m = 0; m < npokes; m++) {
                        stub_poke[n][m] = (uint16_t)(rnd() % pokes->count);
                        stub_poke_value[n][m] = (int32_t)PICK(0, 0, 1, 2, 3, 10, 100, 150, -1, 0x10);
                    }
                }
            }
            ncalls[0] = ncalls[1] = 0;
            for (int mode = 0; mode < 2; mode++) {
                memcpy(c->mem + SNAP_START, start, SNAP_SIZE);
                for (int r = 0; r < 32; r++)
                    c->r[r] = 0xBAD00000u + (uint32_t)r;    /* no version may depend on a dead register */
                c->r[0] = 0;
                /* and the same in the others: what the last run left there is read by real callees that load registers partly */
                c->hi = c->lo = 0;
                memset(c->f, 0, sizeof c->f);
                c->fcr31 = 0;
                c->fpcond = 0;
                memset(c->v, 0, sizeof c->v);
                memset(c->vfpu_ctrl, 0, sizeof c->vfpu_ctrl);
                c->r[29] = TEST_SP;
                c->r[4] = hunter;
                c->r[5] = a1;
                c->r[6] = args[1];
                c->r[7] = args[2];
                for (int r = 0; r < 8; r++)
                    saved[r] = c->r[16 + r];
                saved[8] = c->r[30];
                rt_tick30 = tick30;
                rt_hand = mode;
                current = mode;
                target = stub_mode ? cases[k].addr : 0;
                stubbing = stub_mode;
                running = 1;
                cases[k].fn(c);
                running = stubbing = 0;
                for (int r = 0; r < 8; r++)
                    after[mode][r] = c->r[16 + r];
                after[mode][8] = c->r[30];
                v0[mode] = c->r[2];
                if (c->r[29] != TEST_SP) {
                    printf("%s: sp not restored (mode %d)\n", cases[k].name, mode);
                    bad++;
                }
                if (mode == 0)
                    memcpy(generated, c->mem + SNAP_START, SNAP_SIZE);
            }
            /* blank the stack in both: what's left there is dead */
            memset(generated + (STACK_LOW - SNAP_START), 0, TEST_SP - STACK_LOW);
            memset(c->mem + STACK_LOW, 0, TEST_SP - STACK_LOW);
            /* Callees running for real: game_sub 09C375D8 stores two positions with vfpu_sv_q, each's fourth word left from whatever was on the stack,
             * which differs with the stack frames above it (docs/readable.md, handdiff.py): dead */
            if (!stub_mode)
                for (int m = 0; m < 2; m++)
                    for (uint32_t o = 0x4AC; o <= 0x4CC; o += 0x20)
                        *(uint32_t *)((m ? c->mem + SNAP_START : generated) + (hunter - SNAP_START) + o) = 0;
            int same = !memcmp(generated, c->mem + SNAP_START, SNAP_SIZE);
            same = same && !memcmp(after[0], saved, sizeof saved) && !memcmp(after[1], saved, sizeof saved);
            same = same && (!cases[k].returns || v0[0] == v0[1]);
            if ((ncalls[0] != ncalls[1] || memcmp(calls[0], calls[1], sizeof(Call) * (size_t)(ncalls[0] < MAX_CALLS ? ncalls[0] : MAX_CALLS)))) {
                same = 0;
                printf("%s: calls differ\n", cases[k].name);
                for (int m = 0; m < 2; m++)
                    for (int n = 0; n < ncalls[m] && n < MAX_CALLS; n++)
                        printf("  %s %d: %08X(%08X %08X %08X %08X; t0 %08X t1 %08X sp+0 %08X; f12 %08X f13 %08X; hunter %08X)\n", m ? "hand" : "generated", n, calls[m][n].fn,
                               calls[m][n].a[0], calls[m][n].a[1], calls[m][n].a[2], calls[m][n].a[3], calls[m][n].a[4], calls[m][n].a[5], calls[m][n].stack0,
                               calls[m][n].f[0], calls[m][n].f[1], calls[m][n].hunter_hash);
            }
            if (!same) {
                bad++;
                printf("%s: DIFFERS a1=%08X a2=%08X a3=%08X tick30=%d", cases[k].name, a1, args[1], args[2], tick30);
                printf(cases[k].returns ? " v0 generated %08X hand %08X\n" : "\n", v0[0], v0[1]);
                for (uint32_t o = 0; o < SNAP_SIZE; o += 4)
                    if (memcmp(generated + o, c->mem + SNAP_START + o, 4))
                        printf("  %08X generated %08X hand %08X\n", SNAP_START + o, *(uint32_t *)(generated + o),
                               *(uint32_t *)(c->mem + SNAP_START + o));
            }
            memcpy(c->mem + SNAP_START, start, SNAP_SIZE);
        }
        printf("%s: %s\n", cases[k].name, bad ? "FAILED" : "same");
        failed += bad;
    }
    return failed != 0;
}
