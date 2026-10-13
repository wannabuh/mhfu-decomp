/* Run one recompiled function from a CPU and memory state captured in PPSSPP, and write the state it leaves - for
 * tools/difftest.py to compare with what PPSSPP did from the same state.
 *
 * Usage: difftest <in.state> <out.state> <function address>
 * State file: the StateHeader below, then the scratchpad, then RAM. The header's status and message are set on output:
 * 0 the function returned, 1 it hit a fatal error (an import, the VFPU, a call to no function...). */
#include "runtime.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char magic[8];                  /* "MHFUST01" */
    uint32_t gpr[32], hi, lo;
    uint32_t fpr[32], fcr31;
    uint32_t vfpu[128], vfpu_ctrl[16];
    char resident[NREGIONS][32];    /* the module resident in each region ("" for the runtime to find it) */
    uint32_t status;
    char message[252];
} StateHeader;

static jmp_buf bail;
static char bail_msg[252];

static void on_fatal(const char *msg) {
    snprintf(bail_msg, sizeof bail_msg, "%s", msg);
    longjmp(bail, 1);
}

static void read_exact(FILE *f, void *p, size_t n, const char *what) {
    if (fread(p, 1, n, f) != n) {
        fprintf(stderr, "short read: %s\n", what);
        exit(2);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.state> <out.state> <function address>\n", argv[0]);
        return 2;
    }
    uint32_t addr = (uint32_t)strtoul(argv[3], NULL, 16);
    static Ctx ctx;
    Ctx *c = &ctx;
    c->mem = rt_mem_init();

    StateHeader h;
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    read_exact(f, &h, sizeof h, "header");
    if (memcmp(h.magic, "MHFUST01", 8)) {
        fprintf(stderr, "%s: not a state file\n", argv[1]);
        return 2;
    }
    read_exact(f, c->mem + SCRATCHPAD_START, SCRATCHPAD_SIZE, "scratchpad");
    read_exact(f, c->mem + RAM_START, RAM_SIZE, "RAM");
    fclose(f);

    memcpy(c->r, h.gpr, sizeof c->r);
    c->r[0] = 0;
    c->hi = h.hi;
    c->lo = h.lo;
    for (int i = 0; i < 32; i++)
        c->f[i].u = h.fpr[i];
    c->fcr31 = h.fcr31;
    c->fpcond = (h.fcr31 >> 23) & 1;
    memcpy(c->v, h.vfpu, sizeof c->v);
    memcpy(c->vfpu_ctrl, h.vfpu_ctrl, sizeof c->vfpu_ctrl);
    /* The runtime finds the resident overlays from their headers in RAM; a name in the state file overrides that */
    for (int r = 1; r < NREGIONS; r++) {
        if (h.resident[r][0])
            rt_set_resident(r, rt_module_named(h.resident[r]));
        const RecompModule *m = rt_resident(r);
        snprintf(h.resident[r], sizeof h.resident[r], "%s", m ? m->name : "");
    }

    rt_fatal_handler = on_fatal;
    h.status = 0;
    h.message[0] = 0;
    if (setjmp(bail)) {
        h.status = 1;
        snprintf(h.message, sizeof h.message, "%s", bail_msg);
    } else {
        const RecompModule *m;
        RecompFunc fn = rt_lookup(addr, &m);
        if (!fn) {
            h.status = 1;
            snprintf(h.message, sizeof h.message, "no function at %08X in %s", addr, m ? m->name : "(none)");
        } else {
            fn(c);
        }
    }

    memcpy(h.gpr, c->r, sizeof h.gpr);
    h.hi = c->hi;
    h.lo = c->lo;
    for (int i = 0; i < 32; i++)
        h.fpr[i] = c->f[i].u;
    h.fcr31 = (c->fcr31 & ~(1u << 23)) | ((uint32_t)(c->fpcond != 0) << 23);
    memcpy(h.vfpu, c->v, sizeof h.vfpu);
    memcpy(h.vfpu_ctrl, c->vfpu_ctrl, sizeof h.vfpu_ctrl);
    f = fopen(argv[2], "wb");
    if (!f) {
        perror(argv[2]);
        return 2;
    }
    fwrite(&h, sizeof h, 1, f);
    fwrite(c->mem + SCRATCHPAD_START, 1, SCRATCHPAD_SIZE, f);
    fwrite(c->mem + RAM_START, 1, RAM_SIZE, f);
    fclose(f);
    return 0;
}
