/* The GE (graphics engine): display lists queued by the game, walked as the hardware would - jumps, calls, returns,
 * the stall address, FINISH and SIGNAL raising the game's callbacks - each command going to the renderer (gpu.c). */
#include "hle.h"
#include "savestate.h"
#include "render_queue.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LISTS 64
#define MAX_CALLBACKS 16

typedef struct {
    int used, active;
    uint32_t id, pc, stall, cbid, arg;
    uint32_t base, offset;
    uint32_t stack[32];
    int depth;
    int alone;                      /* a chunk run by itself (ge_run_chunk): no callbacks, no gated chunks */
    int chunk[11];                  /* each call's: the gated chunk it was made from (native.c), or -1 */
} GeList;

int native_chunk_at(uint32_t at);
void gpu_held_chunk(int chunk);
void gpu_held_draw(int chunk);
extern int shadow_extras_due;       /* shadow_extras.c */
void shadow_extras_at(uint32_t at);
extern uint32_t gpu_cmd_at;         /* the command being run's address (gpu.c: the HUD's draws told by it) */

SAVED static GeList lists[MAX_LISTS];
SAVED static uint32_t next_id = 0x35000000u;     /* as the PSP numbers them */
SAVED static struct { int used; uint32_t signal_func, signal_arg, finish_func, finish_arg; } callbacks[MAX_CALLBACKS];
SAVED static uint32_t edram_translation = 0x400;

static unsigned long cmd_count[256], prim_count[8];
static int ge_stats;

static void print_stats(void) {
    fprintf(stderr, "GE commands:");
    for (int i = 0; i < 256; i++)
        if (cmd_count[i])
            fprintf(stderr, " %02X:%lu", i, cmd_count[i]);
    fprintf(stderr, "\nGE primitives:");
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " %d:%lu", i, prim_count[i]);
    fprintf(stderr, "\n");
}

void ge_init(void) {
    ge_stats = getenv("MHFU_GESTATS") != NULL;
    if (ge_stats)
        atexit(print_stats);
}

static uint32_t relative(GeList *l, uint32_t addr) {
    return (((l->base & 0x0F000000u) | addr) + l->offset) & 0x0FFFFFFFu;
}

static void callback(GeList *l, int finish, uint32_t value) {
    render_queue_sync();                /* (the game's code runs: it may write what the renderer has yet to read) */
    if (l->cbid >= MAX_CALLBACKS || !callbacks[l->cbid].used)
        return;
    uint32_t func = finish ? callbacks[l->cbid].finish_func : callbacks[l->cbid].signal_func;
    uint32_t arg = finish ? callbacks[l->cbid].finish_arg : callbacks[l->cbid].signal_arg;
    if (func)
        interrupt_call(func, value, arg, 0, 0, 0, 0, 0);
}

/* Walk a list up to its stall address or its end */
static void run_list_on(GeList *l);

/* Walk a list as far as it goes; the renderer done with it before the game runs on (render_queue.c) */
static void run_list(GeList *l) {
    run_list_on(l);
    shadow_extras_due = 0;              /* (made ready for this list, which has run: nothing of it left for another) */
    render_queue_sync();
}

static void run_list_on(GeList *l) {
    while (l->active) {
        if (l->stall && l->pc == l->stall)
            return;
        if (shadow_extras_due && !l->alone)
            shadow_extras_at(l->pc);        /* (at the end of a layer: what is drawn apart, in its place) */
        uint32_t cmd = LW(l->pc), op = cmd >> 24, data = cmd & 0xFFFFFF;
        uint32_t at = l->pc;
        gpu_cmd_at = at;
        l->pc += 4;
        /* drawn by gated code at a 30th (its draws kept for the frames between: native.c), or a chunk's in its place */
        int chunk = l->alone ? -1 : native_chunk_at(at);
        if (chunk < 0 && l->depth >= 3)
            chunk = l->chunk[l->depth / 3 - 1];
        gpu_held_chunk(chunk);
        if (op == 0x00 && (data >> 16) == 0xA5)
            gpu_held_draw((int)(data & 0xFFFF));
        cmd_count[op]++;
        if (op == 0x04)
            prim_count[(data >> 16) & 7]++;
        switch (op) {
        case 0x08:                                  /* JUMP */
            l->pc = relative(l, data & 0xFFFFFC);
            break;
        case 0x0A:                                  /* CALL */
            if (l->depth + 3 <= 32) {
                l->chunk[l->depth / 3] = chunk;
                l->stack[l->depth++] = l->pc;
                l->stack[l->depth++] = l->base;
                l->stack[l->depth++] = l->offset;
            }
            l->pc = relative(l, data & 0xFFFFFC);
            break;
        case 0x0B:                                  /* RET */
            if (l->depth >= 3) {
                l->offset = l->stack[--l->depth];
                l->base = l->stack[--l->depth];
                l->pc = l->stack[--l->depth];
            }
            break;
        case 0x0C:                                  /* END */
            l->active = 0;
            break;
        case 0x0E:                                  /* SIGNAL (followed by END) */
            if ((data >> 16) <= 3 && !l->alone)
                callback(l, 0, data & 0xFFFF);
            l->pc += 4;
            break;
        case 0x0F:                                  /* FINISH (followed by END) */
            if (!l->alone)
                callback(l, 1, data & 0xFFFF);
            break;
        case 0x10:                                  /* BASE */
            l->base = (data << 8) & 0x0F000000u;
            break;
        case 0x13:                                  /* OFFSETADDR */
            l->offset = data << 8;
            break;
        case 0x14:                                  /* ORIGIN */
            l->offset = at;
            break;
        default:
            break;
        }
        if (l->alone && op == 0xEA)         /* (a block transfer writes the game's memory: not for what is drawn apart) */
            continue;
        gpu_command(cmd, relative(l, data));
    }
}

/* shadow_extras.c: the commands from start up to end, as if linked into the frame's list (its link sets the base) */
void ge_run_chunk(uint32_t start, uint32_t end) {
    GeList l = {0};
    l.used = l.active = l.alone = 1;
    l.pc = start;
    l.stall = end;
    l.base = start & 0x0F000000u;
    run_list_on(&l);
}

void ge_process(void) {
    for (int i = 0; i < MAX_LISTS; i++)
        if (lists[i].used && lists[i].active)
            run_list(&lists[i]);
}

static GeList *find(uint32_t id) {
    for (int i = 0; i < MAX_LISTS; i++)
        if (lists[i].used && lists[i].id == id)
            return &lists[i];
    return NULL;
}

/* (list, stall, cbid, arg) */
void hle_sceGeListEnQueue(Ctx *c) {
    GeList *l = NULL;
    for (int i = 0; i < MAX_LISTS; i++)
        if (!lists[i].used || !lists[i].active) {
            l = &lists[i];
            break;
        }
    if (!l) {
        RET(0x80000022u);
        return;
    }
    memset(l, 0, sizeof *l);
    l->used = l->active = 1;
    l->id = next_id++;
    l->pc = ARG0 & 0x0FFFFFFFu;
    l->stall = ARG1 & 0x0FFFFFFFu;
    l->cbid = ARG2;
    l->arg = ARG3;
    HLE_LOG("%08X=sceGeListEnQueue(%08X, stall %08X)", l->id, l->pc, l->stall);
    RET(l->id);
    run_list(l);
}

void hle_sceGeListUpdateStallAddr(Ctx *c) {
    GeList *l = find(ARG0);
    if (!l) {
        RET(0x80000100u);
        return;
    }
    l->stall = ARG1 & 0x0FFFFFFFu;
    RET(0);
    run_list(l);
}

/* Lists run to their stall at once, so a list is done unless stalled */
void hle_sceGeListSync(Ctx *c) {
    GeList *l = find(ARG0);
    RET(!l || !l->active ? 0 : 1);
}

void hle_sceGeDrawSync(Ctx *c) {
    for (int i = 0; i < MAX_LISTS; i++)
        if (lists[i].used && lists[i].active) {
            RET(1);
            return;
        }
    RET(0);
}

void hle_sceGeSetCallback(Ctx *c) {
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (!callbacks[i].used) {
            callbacks[i].used = 1;
            callbacks[i].signal_func = LW(ARG0);
            callbacks[i].signal_arg = LW(ARG0 + 4);
            callbacks[i].finish_func = LW(ARG0 + 8);
            callbacks[i].finish_arg = LW(ARG0 + 12);
            RET(i);
            return;
        }
    RET(0x80000022u);
}

void hle_sceGeUnsetCallback(Ctx *c) {
    if (ARG0 < MAX_CALLBACKS)
        callbacks[ARG0].used = 0;
    RET(0);
}

void hle_sceGeEdramGetAddr(Ctx *c) { RET(0x04000000u); }
void hle_sceGeEdramGetSize(Ctx *c) { RET(0x00200000u); }

void hle_sceGeEdramSetAddrTranslation(Ctx *c) {
    RET(edram_translation);
    edram_translation = ARG0;
}
