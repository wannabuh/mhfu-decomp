/* What ran: linked into a build generated with RECOMP_TRACE_ALL=1, never into the game. MHFU_TRACE_ALL=<file> writes, for
 * each span of TRACE_SPAN game frames, every distinct (module, function, return address) that ran in it, the first time
 * in that span, as 16-byte records: frame span's first frame, module, function, return address. It reads no game state
 * but the return address and writes none, so a replay runs as without it (tools/readable/replaycheck.py checks that). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TRACE_SPAN 10
#define SLOTS (1u << 20)

unsigned long present_frame_number(void);

typedef struct { uint32_t fn, ra, module, span; } Slot;     /* span: last span it was seen in, + 1 */
static Slot *slots;
static FILE *out;
static int off;
static uint32_t last_span;

void rt_trace_all(int module, uint32_t fn, uint32_t ra) {
    if (off)
        return;
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
