/* What ran: linked into a build generated with RECOMP_TRACE_ALL=1, never into the game. MHFU_TRACE_ALL=<file> writes, for
 * each span of TRACE_SPAN game frames, every distinct (module, function, return address) that ran in it, the first time
 * in that span, as 16-byte records: frame span's first frame, module, function, return address. It reads no game state
 * and its arguments and writes none, so a replay runs as without it (tools/readable/replaycheck.py checks that).
 *
 * MHFU_TRACE_ARGS=<file> writes what each function was handed: for an argument register a0-a3 holding a pointer into RAM, the
 * word it points at if that could be a vtable and 0 if not, once per (module, function, register, word), as 16-byte records. So a function's
 * `this` is known from what it ran on (tools/readable/classes.py). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp.h"

#define TRACE_SPAN 10
#define SLOTS (1u << 20)

unsigned long present_frame_number(void);

typedef struct { uint32_t fn, ra, module, span; } Slot;     /* span: last span it was seen in, + 1 */
static Slot *slots;
static FILE *out;
static int off;
static uint32_t last_span;

#define ARG_SLOTS (1u << 22)
#define RAM_LOW 0x08800000u
#define RAM_HIGH 0x0A000000u

typedef struct { uint32_t fn, word, module_reg, used; } ArgSlot;
static ArgSlot *arg_slots;
static FILE *arg_out;
static int arg_off;

static void trace_args(Ctx *c, int module, uint32_t fn) {
    if (arg_off)
        return;
    if (!arg_slots) {
        const char *path = getenv("MHFU_TRACE_ARGS");
        if (!path || !(arg_out = fopen(path, "wb"))) {
            arg_off = 1;
            return;
        }
        arg_slots = calloc(ARG_SLOTS, sizeof(ArgSlot));
    }
    for (int r = 4; r < 8; r++) {
        uint32_t p = c->r[r];
        if (p < RAM_LOW || p >= RAM_HIGH || (p & 3))
            continue;
        uint32_t word = LW(p);
        if (word < RAM_LOW || word >= RAM_HIGH || (word & 3))
            word = 0;       /* a pointer to something that can't be an object: once, so helpers aren't taken for methods */
        uint32_t module_reg = (uint32_t)module << 8 | (uint32_t)r;
        uint32_t h = (fn * 2654435761u ^ word * 40503u ^ module_reg * 97u) & (ARG_SLOTS - 1);
        int probes = 0;
        while (arg_slots[h].used && !(arg_slots[h].fn == fn && arg_slots[h].word == word && arg_slots[h].module_reg == module_reg)) {
            h = (h + 1) & (ARG_SLOTS - 1);
            if (++probes > 4096) {      /* full enough: the data words filling it aren't what this is for */
                h = ARG_SLOTS;
                break;
            }
        }
        if (h == ARG_SLOTS || arg_slots[h].used)
            continue;
        arg_slots[h] = (ArgSlot){fn, word, module_reg, 1};
        uint32_t rec[4] = {(uint32_t)module, fn, (uint32_t)r, word};
        fwrite(rec, sizeof rec, 1, arg_out);
    }
}

void rt_trace_all(Ctx *c, int module, uint32_t fn) {
    trace_args(c, module, fn);
    if (off)
        return;
    uint32_t ra = c->r[31];
    if (!slots) {
        const char *path = getenv("MHFU_TRACE_ALL");
        if (!path || !(out = fopen(path, "wb"))) {
            off = 1;
            return;
        }
        slots = calloc(SLOTS, sizeof(Slot));
    }
    uint32_t span = (uint32_t)(present_frame_number() / TRACE_SPAN);
    if (span != last_span) {
        fflush(out);
        if (arg_out)
            fflush(arg_out);
        last_span = span;
    }
    uint32_t h = (fn * 2654435761u ^ ra * 40503u ^ (uint32_t)module * 97u) & (SLOTS - 1);
    while (slots[h].span && !(slots[h].fn == fn && slots[h].ra == ra && slots[h].module == (uint32_t)module))
        h = (h + 1) & (SLOTS - 1);
    if (slots[h].span == span + 1)
        return;
    slots[h] = (Slot){fn, ra, (uint32_t)module, span + 1};
    uint32_t rec[4] = {span * TRACE_SPAN, (uint32_t)module, fn, ra};
    fwrite(rec, sizeof rec, 1, out);
}
