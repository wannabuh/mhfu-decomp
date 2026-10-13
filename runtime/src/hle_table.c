/* The imported functions: dispatch from their stubs (by NID) to their HLE implementations. */
#include "hle.h"

#include <stdlib.h>
#include <string.h>

int hle_enabled;

/* -- dispatch ----------------------------------------------------------------------------------------------------- */

/* Every import has a weak hle_<name>; those implemented are defined somewhere in the runtime */
#define X(lib, nid, name) void hle_##name(Ctx *c) __attribute__((weak));
#include "nids.inc"
#undef X

typedef struct { const char *library; uint32_t nid; const char *name; void (*func)(Ctx *c); int warned; } HleFunc;
static HleFunc hle_funcs[] = {
#define X(lib, nid, name) {#lib, nid, #name, hle_##name, 0},
#include "nids.inc"
#undef X
};

static HleFunc **by_stub;           /* recomp_imports index -> function */

static HleFunc *lookup(uint32_t stub) {
    if (!by_stub) {
        by_stub = calloc((size_t)recomp_nimports, sizeof *by_stub);
        for (int i = 0; i < recomp_nimports; i++)
            for (size_t k = 0; k < sizeof hle_funcs / sizeof hle_funcs[0]; k++)
                if (hle_funcs[k].nid == recomp_imports[i].nid && !strcmp(hle_funcs[k].library, recomp_imports[i].library))
                    by_stub[i] = &hle_funcs[k];
    }
    int lo = 0, hi = recomp_nimports - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (recomp_imports[mid].stub == stub)
            return by_stub[mid];
        if (recomp_imports[mid].stub < stub)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

void hle_call(Ctx *c, uint32_t stub) {
    HleFunc *f = lookup(stub);
    if (!f)
        rt_fatal(c, "call to %08X: not an import stub", stub);
    if (!hle_enabled)
        rt_fatal(c, "import %s (%s %08X): no HLE here", f->name, f->library, f->nid);
    if (hle_trace)
        fprintf(stderr, "[%u] %s(%08X, %08X, %08X, %08X)\n", (unsigned)time_us(), f->name, ARG0, ARG1, ARG2, ARG3);
    if (f->func) {
        f->func(c);
    } else {
        if (!f->warned++)
            HLE_WARN("%s not implemented (returning 0)", f->name);
        RET(0);
    }
    hle_after_call(c);
}
