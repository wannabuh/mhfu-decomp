/* Code rewritten by hand against the generated code it replaces (recomp/patches.py HAND), on inputs the recordings never reach.
 *
 * Usage: handtest <RAM snapshot from a quest> [iterations] [stubs <WORK/signatures.tsv>] [only <part of a case's name>]
 * Takes the snapshot's hunter and game state, randomises what a function's branches depend on, then runs each version from the same
 * state and compares all of RAM but the stack (dead bytes there are not behaviour), the callee-saved registers and, where the
 * function returns one, v0. The random numbers are fixed, so a run repeats. Build it with HAND_FLAGS for coverage to see what it
 * reached. A new rewrite: a row in `cases`, and its inputs in randomise(), or a function of its own (the row's last column) if they
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

typedef struct {
    const char *name;
    void (*fn)(Ctx *);
    int returns;
    int small_a1;       /* a1 is a mode, 0 to 2, not an amount */
    uint32_t addr;
    void (*randomise_more)(Ctx *, uint32_t hunter);     /* also: long chains of results that must be 0 are reached, callees change fields */
} Case;

static void randomise_upkeep(Ctx *c, uint32_t hunter);

static const Case cases[] = {
    {"hunter_stamina_add 09A6A2D0", f_game_task_09A6A2D0, 0, 0, 0x09A6A2D0, NULL},
    {"hunter_stamina_use 09A6A370", f_game_task_09A6A370, 0, 0, 0x09A6A370, NULL},
    {"hunter_stamina_use_alternate_ticks 09A6A480", f_game_task_09A6A480, 0, 0, 0x09A6A480, NULL},
    {"hunter_state_sprint 09A9E288", f_game_task_09A9E288, 0, 1, 0x09A9E288, NULL},
    {"hunter_state_sprint_linked 09A9E690", f_game_task_09A9E690, 0, 1, 0x09A9E690, NULL},
    {"hunter_state_out_of_breath 09A9EAE8", f_game_task_09A9EAE8, 0, 1, 0x09A9EAE8, NULL},
    {"hunter_tick_upkeep 09A68FC8", f_game_task_09A68FC8, 0, 0, 0x09A68FC8, randomise_upkeep},
};

/* -- the stubs: in a trace build, rt_trace_all is at the top of every generated function --------------------------------------- */
#define MAX_CALLS 128
typedef struct { uint32_t fn, a[8], f[8], stack0; } Call;     /* a0-a3 t0-t3, f12-f19: only those the callee reads, others 0; stack0 below */
#define NINE_ARGUMENTS 0x08883988u      /* the one callee so far with an argument at its sp+0 */
#define STUB_RECORD 0x09FF8000u         /* what a call returning a pointer to a record gets */
#define RECORD_TYPE 0x3C
static int stubbing;            /* within the function under test */
static int running;             /* it is running, stubs or not */
static uint32_t target;         /* it: not a stub */
static uint32_t results[MAX_CALLS], fresults[MAX_CALLS];
static int16_t stub_stamina[MAX_CALLS];     /* what a call leaves stamina at (0x7FFF: it leaves it alone): callees do change it */
static uint8_t stub_slot[MAX_CALLS];        /* the same for the local player's slot (0xFF: alone) */
static uint8_t stub_record_type[MAX_CALLS];
static uint32_t record_method;              /* the method returning that pointer: *(*(*0x09A4F1A0)) slot 0x28 */
static const struct { uint16_t offset; uint8_t size; uint8_t in_game; } *pokable;   /* a callee may set one of these */
static int16_t stub_poke[MAX_CALLS];        /* an index in them, or -1 */
static int32_t stub_poke_value[MAX_CALLS];
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
        if (bound >= 0) {
            c->r[2] = 0;
            return 1;
        }
        c->r[2] = results[n];
        c->f[0].u = fresults[n];
        if (fn == record_method) {
            c->r[2] = STUB_RECORD;
            SB(STUB_RECORD + RECORD_TYPE, stub_record_type[n]);
        }
        if (pokable && stub_poke[n] >= 0) {
            uint32_t at = (pokable[stub_poke[n]].in_game ? LW(GAME_POINTER) : stub_hunter) + pokable[stub_poke[n]].offset;
            if (pokable[stub_poke[n]].size == 1)
                SB(at, stub_poke_value[n]);
            else if (pokable[stub_poke[n]].size == 2)
                SH(at, stub_poke_value[n]);
            else
                SW(at, stub_poke_value[n]);
        }
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
static const struct { uint16_t offset; uint8_t size; uint8_t in_game; } upkeep_fields[] = {
    {0x1C, 2, 1}, {0x28, 1, 1},
    {0x1E1, 1, 0}, {0x1E6, 2, 0}, {0x1E8, 1, 0}, {0x298, 1, 0}, {0x299, 1, 0}, {0x2BA, 2, 0}, {0x2E4, 2, 0}, {0x2E6, 1, 0}, {0x35E, 2, 0},
    {0x388, 2, 0}, {0x3D6, 2, 0}, {0x3DA, 2, 0}, {0x410, 4, 0}, {0x41C, 2, 0}, {0x41E, 2, 0}, {0x424, 2, 0}, {0x440, 2, 0}, {0x442, 2, 0},
    {0x468, 2, 0}, {0x552, 1, 0}, {0x55A, 2, 0}, {0x564, 1, 0}, {0x5FE, 1, 0}, {0x610, 2, 0}, {0x620, 2, 0}, {0x622, 2, 0}, {0x624, 2, 0},
    {0x626, 2, 0}, {0x628, 2, 0}, {0x62A, 2, 0}, {0x640, 1, 0}, {0x641, 1, 0}, {0x672, 2, 0}, {0x6A0, 2, 0}, {0x6A6, 2, 0}, {0x6AA, 2, 0},
    {0x6AE, 2, 0}, {0x6B2, 2, 0}, {0x6B4, 2, 0}, {0x6C8, 2, 0}, {0x10F4, 2, 0}, {0x10F6, 2, 0}, {0x10F8, 2, 0}, {0x10FA, 2, 0}, {0x110E, 2, 0},
    {0x1112, 2, 0}, {0x111C, 4, 0}, {0x1138, 2, 0}, {0x1140, 2, 0}, {0x1142, 2, 0}, {0x1146, 2, 0}, {0x11B4, 2, 0},
};

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
    self->state_wait_ticks = (uint8_t)T8;
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
    memset(stub_poke, 0xFF, sizeof stub_poke);
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
            if (cases[k].randomise_more) {
                cases[k].randomise_more(c, hunter);
                quiet = PICK(0, 4, 8, 16, 32, 128);
                pokable = (const void *)upkeep_fields;
            }
            if (cases[k].small_a1)
                a1 = rnd() % 3;
            memcpy(start, c->mem + SNAP_START, SNAP_SIZE);
            uint32_t saved[9], after[2][9], v0[2];
            for (int n = 0; n < MAX_CALLS; n++) {
                results[n] = PICK(0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 5, 0x105, 0x100);   /* (a classifier's, a test's, a low byte's) */
                fresults[n] = rnd();
                stub_stamina[n] = (int16_t)PICK(0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0, 0, -1, 1, 76, 300);
                stub_slot[n] = (uint8_t)PICK(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 2);
                stub_record_type[n] = (uint8_t)PICK(0, 0, 1, 2, 3, 4, 0x82);
                if (cases[k].randomise_more) {
                    /* low bytes, low halves and whole words told apart */
                    results[n] = PICK(0, 0, 1, 1, 1, 2, 3, 0x100, 0x101, 0x10000, 0x10002, 0x8000, 0xFFFF, 0x1FFFF, -1);
                    stub_poke[n] = -1;
                    stub_poke_value[n] = (int32_t)PICK(0, 0, 1, 2, 3, 10, 100, 150, -1, 0x10);
                    if (quiet && rnd() % quiet) {
                        results[n] = 0;
                        stub_stamina[n] = 0x7FFF;
                        stub_slot[n] = 0xFF;
                    } else if (rnd() % 4 == 0) {
                        stub_poke[n] = (int16_t)(rnd() % (sizeof upkeep_fields / sizeof upkeep_fields[0]));
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
            int same = !memcmp(generated, c->mem + SNAP_START, SNAP_SIZE);
            same = same && !memcmp(after[0], saved, sizeof saved) && !memcmp(after[1], saved, sizeof saved);
            same = same && (!cases[k].returns || v0[0] == v0[1]);
            if ((ncalls[0] != ncalls[1] || memcmp(calls[0], calls[1], sizeof(Call) * (size_t)(ncalls[0] < MAX_CALLS ? ncalls[0] : MAX_CALLS)))) {
                same = 0;
                printf("%s: calls differ\n", cases[k].name);
                for (int m = 0; m < 2; m++)
                    for (int n = 0; n < ncalls[m] && n < MAX_CALLS; n++)
                        printf("  %s %d: %08X(%08X %08X %08X %08X; t0 %08X t1 %08X sp+0 %08X; f12 %08X f13 %08X)\n", m ? "hand" : "generated", n, calls[m][n].fn,
                               calls[m][n].a[0], calls[m][n].a[1], calls[m][n].a[2], calls[m][n].a[3], calls[m][n].a[4], calls[m][n].a[5], calls[m][n].stack0,
                               calls[m][n].f[0], calls[m][n].f[1]);
            }
            if (!same) {
                bad++;
                printf("%s: DIFFERS a1=%08X tick30=%d\n", cases[k].name, a1, tick30);
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
