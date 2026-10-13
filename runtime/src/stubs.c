/* Placeholders for the parts of the runtime not written yet: the imported system functions (HLE). */
#include "runtime.h"

void hle_call(Ctx *c, uint32_t stub) {
    for (int i = 0; i < recomp_nimports; i++)
        if (recomp_imports[i].stub == stub)
            rt_fatal(c, "import %s %08X (stub %08X): no HLE yet", recomp_imports[i].library, recomp_imports[i].nid,
                     stub);
    rt_fatal(c, "call to %08X: not an import stub", stub);
}
