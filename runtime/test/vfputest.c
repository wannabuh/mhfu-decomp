/* Run VFPU instruction sequences through the runtime's VFPU functions, for tools/vfputest.py to compare with PPSSPP,
 * which runs the same sequences.
 *
 * stdin: cases, each: u32 base address, u32 a0, u32 t0, 0x400 bytes of memory at base, u32 count, then count x
 * (u32 instruction, char name[16]). The sequence loads the registers from memory, runs the instructions under test and
 * stores the registers back. stdout: per case, the 0x400 bytes at base afterwards, and CC. */
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPS(X) X(lv_s) X(sv_s) X(lv_q) X(sv_q) X(mtv) X(mfv) X(vpfxs) X(vpfxt) X(vpfxd) X(vadd) X(vsub) X(vmul) \
    X(vdiv) X(vmin) X(vmax) X(vmov) X(vabs) X(vneg) X(vrcp) X(vrsq) X(vsqrt) X(vsin) X(vcos) X(vexp2) X(vlog2) \
    X(vasin) X(vocp) X(vsgn) X(vzero) X(vone) X(vcst) X(vidt) X(vscl) X(vdot) X(vfad) X(vcrsp) X(vsocp) X(vfim) \
    X(vi2f) X(vf2id) X(vf2iu) X(vf2iz) X(vf2in) X(vcmp) X(vcmovt) X(vcmovf) X(vrot) X(vmidt) X(vmmov) X(vmmul) \
    X(vtfm2) X(vtfm3) X(vtfm4)

#define DECL(n) void vfpu_##n(Ctx *c, uint32_t instr);
OPS(DECL)
#undef DECL

static const struct { const char *name; void (*fn)(Ctx *, uint32_t); } ops[] = {
#define ENTRY(n) {#n, vfpu_##n},
    OPS(ENTRY)
#undef ENTRY
};

int main(void) {
    static Ctx ctx;
    Ctx *c = &ctx;
    c->mem = rt_mem_init();
    uint32_t hdr[3];
    while (fread(hdr, 4, 3, stdin) == 3) {
        uint32_t base = hdr[0], n;
        if (fread(MEM(base), 1, 0x400, stdin) != 0x400 || fread(&n, 4, 1, stdin) != 1 || n > 256)
            return 1;
        static uint32_t words[256];
        static char names[256][16];
        for (uint32_t i = 0; i < n; i++)
            if (fread(&words[i], 4, 1, stdin) != 1 || fread(names[i], 16, 1, stdin) != 1)
                return 1;
        memset(c->r, 0, sizeof c->r);
        c->r[4] = hdr[1];
        c->r[8] = hdr[2];
        memset(c->v, 0, sizeof c->v);
        memset(c->vfpu_ctrl, 0, sizeof c->vfpu_ctrl);
        c->vfpu_ctrl[0] = c->vfpu_ctrl[1] = 0xE4;
        for (uint32_t i = 0; i < n; i++) {
            size_t k = 0;
            while (k < sizeof ops / sizeof ops[0] && strcmp(ops[k].name, names[i]))
                k++;
            if (k == sizeof ops / sizeof ops[0]) {
                fprintf(stderr, "unknown op %s\n", names[i]);
                return 1;
            }
            ops[k].fn(c, words[i]);
        }
        fwrite(MEM(base), 1, 0x400, stdout);
        fwrite(&c->vfpu_ctrl[VFPU_CTRL_CC], 4, 1, stdout);
        fflush(stdout);
    }
    return 0;
}
