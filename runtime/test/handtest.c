/* Code rewritten by hand against the generated code it replaces (recomp/patches.py HAND), on inputs the recordings never reach.
 *
 * Usage: handtest <RAM snapshot from a quest> [iterations] [stubs]
 * Takes the snapshot's hunter and game state, randomises what a function's branches depend on, then runs each version from the same
 * state and compares all of RAM but the stack (dead bytes there are not behaviour), the callee-saved registers and, where the
 * function returns one, v0. The random numbers are fixed, so a run repeats. Build it with HAND_FLAGS for coverage to see what it
 * reached. A new rewrite: a row in `cases`, and its inputs in randomise() if it has new ones.
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
} Case;

static const Case cases[] = {
    {"hunter_stamina_add 09A6A2D0", f_game_task_09A6A2D0, 0, 0, 0x09A6A2D0},
    {"hunter_stamina_use 09A6A370", f_game_task_09A6A370, 0, 0, 0x09A6A370},
    {"hunter_stamina_use_alternate_ticks 09A6A480", f_game_task_09A6A480, 0, 0, 0x09A6A480},
    {"hunter_state_sprint 09A9E288", f_game_task_09A9E288, 0, 1, 0x09A9E288},
};

/* -- the stubs: in a trace build, rt_trace_all is at the top of every generated function --------------------------------------- */
#define MAX_CALLS 64
typedef struct { uint32_t fn, a[4], f[2]; } Call;
static int stubbing;            /* within the function under test */
static uint32_t target;         /* it: not a stub */
static uint32_t results[MAX_CALLS], fresults[MAX_CALLS];
static Call calls[2][MAX_CALLS];
static int ncalls[2], current;

int rt_trace_all(Ctx *c, int module, uint32_t fn) {
    (void)module;
    if (!stubbing || fn == target)
        return 0;
    int n = ncalls[current]++;
    if (n < MAX_CALLS) {
        calls[current][n] = (Call){fn, {c->r[4], c->r[5], c->r[6], c->r[7]}, {c->f[12].u, c->f[13].u}};
        c->r[2] = results[n];
        c->f[0].u = fresults[n];
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
    h->stamina = (int16_t)PICK(0, 1, 2, 5, 75, 76, 150, 299, 300, 301, 32767, -1, -32768);
    h->stamina_max = (int16_t)PICK(0, 50, 150, 300, 32767, -1);
    SH(hunter + 0x1E6, rnd() % 3 == 0 ? rnd() % 4 : g->local_hunter_slot);
    g->tick_30ths = (uint16_t)(rnd() % 8 == 0 ? rnd() : rnd() % 64);
    SH(hunter + 0x622, PICK(0, 0, 0, 1, 5, -1));                /* (stamina loss blocked if positive) */
    SH(hunter + 0x6A0 + 5 * 2, PICK(0, 0, 1, -1));              /* (the other way it is blocked) */
    for (int i = 0; i < 6; i++)                                 /* the active skills */
        SB(hunter + 0x676 + i, PICK(0, 0, 0, 0xA8, 0xA9, 0x10));
    /* the state functions' */
    Hunter *self = (Hunter *)MEM(hunter);
    self->state_step = (uint8_t)PICK(0, 1, 1, 1, 1, 2);
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <RAM snapshot> [iterations] [stubs]\n", argv[0]);
        return 2;
    }
    int iterations = argc > 2 ? atoi(argv[2]) : 3000;
    int stub_mode = argc > 3 && !strcmp(argv[3], "stubs");
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
    uint8_t *start = malloc(SNAP_SIZE), *generated = malloc(SNAP_SIZE);
    int failed = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        int bad = 0;
        for (int i = 0; i < iterations && bad < 5; i++) {
            uint32_t a1;
            int tick30;
            randomise(c, hunter, &a1, &tick30);
            if (cases[k].small_a1)
                a1 = rnd() % 3;
            memcpy(start, c->mem + SNAP_START, SNAP_SIZE);
            uint32_t saved[9], after[2][9], v0[2];
            for (int n = 0; n < MAX_CALLS; n++) {
                results[n] = PICK(0, 0, 1, 1, 2, 3, 3, 5, 5, 0x105, 0x100);     /* (a classifier's, a test's, a low byte's) */
                fresults[n] = rnd();
            }
            ncalls[0] = ncalls[1] = 0;
            for (int mode = 0; mode < 2; mode++) {
                memcpy(c->mem + SNAP_START, start, SNAP_SIZE);
                for (int r = 0; r < 32; r++)
                    c->r[r] = 0xBAD00000u + (uint32_t)r;    /* no version may depend on a dead register */
                c->r[0] = 0;
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
                cases[k].fn(c);
                stubbing = 0;
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
            if (stub_mode && (ncalls[0] != ncalls[1] || memcmp(calls[0], calls[1], sizeof(Call) * (size_t)(ncalls[0] < MAX_CALLS ? ncalls[0] : MAX_CALLS)))) {
                same = 0;
                printf("%s: calls differ\n", cases[k].name);
                for (int m = 0; m < 2; m++)
                    for (int n = 0; n < ncalls[m] && n < MAX_CALLS; n++)
                        printf("  %s %d: %08X(%08X %08X %08X %08X; f12 %08X f13 %08X)\n", m ? "hand" : "generated", n, calls[m][n].fn,
                               calls[m][n].a[0], calls[m][n].a[1], calls[m][n].a[2], calls[m][n].a[3], calls[m][n].f[0], calls[m][n].f[1]);
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
