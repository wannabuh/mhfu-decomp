/* Calls by address: the EBOOT's functions, or the functions of whichever overlay is resident in the region the address
 * falls in (see docs/overlays.md). */
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FatalHandler rt_fatal_handler;

void rt_fatal(Ctx *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (rt_fatal_handler)
        rt_fatal_handler(msg);
    fprintf(stderr, "fatal: %s (sp %08X ra %08X)\n", msg, c ? c->r[29] : 0, c ? c->r[31] : 0);
    exit(1);
}

static const RecompModule *resident[NREGIONS];     /* set explicitly (tests); else read from RAM */
static uint32_t region_start[NREGIONS + 1];     /* region i: [region_start[i], region_start[i + 1]) */

static void init_regions(void) {
    if (region_start[0])
        return;
    for (int i = 0; i < recomp_nmodules; i++) {
        const RecompModule *m = &recomp_modules[i];
        if (!region_start[m->region] || m->base < region_start[m->region])
            region_start[m->region] = m->base;
    }
    region_start[NREGIONS] = 0x0A000000u;
    resident[0] = &recomp_modules[0];           /* the EBOOT */
}

void rt_set_resident(int region, const RecompModule *m) {
    init_regions();
    resident[region] = m;
}

const RecompModule *rt_resident(int region) {
    init_regions();
    if (resident[region] || !region || !rt_mem)
        return resident[region];
    /* An overlay is loaded verbatim at its region's base: its header ("MWo3", number) says which it is */
    const uint8_t *h = rt_mem + region_start[region];
    if (memcmp(h, "MWo3", 4))
        return NULL;
    uint32_t number;
    memcpy(&number, h + 4, 4);
    static const RecompModule *by_number[512];
    if (number < 512 && !by_number[number])
        by_number[number] = rt_module_numbered((int)number);
    const RecompModule *m = number < 512 ? by_number[number] : NULL;
    return m && m->region == region ? m : NULL;
}

const RecompModule *rt_module_named(const char *name) {
    for (int i = 0; i < recomp_nmodules; i++)
        if (!strcmp(recomp_modules[i].name, name))
            return &recomp_modules[i];
    return NULL;
}

const RecompModule *rt_module_numbered(int number) {
    for (int i = 0; i < recomp_nmodules; i++)
        if (recomp_modules[i].number == number && recomp_modules[i].region)
            return &recomp_modules[i];
    return NULL;
}

static RecompFunc find(const RecompModule *m, uint32_t addr) {
    int lo = 0, hi = m->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->funcs[mid].addr == addr)
            return m->funcs[mid].func;
        if (m->funcs[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

RecompFunc rt_lookup(uint32_t addr, const RecompModule **in) {
    init_regions();
    addr &= ADDR_MASK;
    for (int r = NREGIONS - 1; r >= 0; r--) {
        if (addr >= region_start[r] && addr < region_start[r + 1]) {
            const RecompModule *m = rt_resident(r);
            if (in)
                *in = m;
            return m ? find(m, addr) : NULL;
        }
    }
    if (in)
        *in = NULL;
    return NULL;
}

void rt_call(Ctx *c, uint32_t addr) {
    const RecompModule *m;
    RecompFunc f = rt_lookup(addr, &m);
    if (!f)
        rt_fatal(c, "call to %08X: no function there in %s", addr, m ? m->name : "(no module resident)");
    f(c);
}

uint32_t rt_resume_pc;

/* The function holding a return point in the module resident there; 0 if it is the start of a function (a call that
 * was a function's last instruction), which is then entered as usual */
static RecompFunc find_return(const RecompModule *m, uint32_t addr) {
    int lo = 0, hi = m->nreturns - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->returns[mid].addr == addr)
            return m->returns[mid].func;
        if (m->returns[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* A thread of a save state, its kernel call just over: the game's code goes on from the return address, and each
 * function it returns from goes on at its own - without the host stack the calls would have made (every register is
 * in c, and a function returns to r31) - until the thread's entry returns to `end`. */
void rt_resume_run(Ctx *c, uint32_t end) {
    for (;;) {
        uint32_t pc = c->r[31] & ADDR_MASK;
        if (pc == end)
            return;
        const RecompModule *m;
        RecompFunc start = rt_lookup(pc, &m);
        RecompFunc f = m ? find_return(m, pc) : NULL;
        if (f) {
            rt_resume_pc = pc;
            f(c);
        } else if (start) {
            start(c);
        } else {
            rt_fatal(c, "a save state's thread: no return point at %08X in %s", pc, m ? m->name : "(no module resident)");
        }
    }
}

void rt_bad_jump(Ctx *c, uint32_t value, uint32_t at) {
    rt_fatal(c, "jump table at %08X: no case for %08X", at, value);
}

void rt_break(Ctx *c, uint32_t at) {
    rt_fatal(c, "break at %08X", at);
}

void rt_unreachable(Ctx *c, uint32_t at) {
    rt_fatal(c, "reached %08X, past the end of a function", at);
}
