/* Instructions the generated code leaves to the runtime (other than the VFPU's). */
#include "runtime.h"

/* Unaligned word loads and stores (little-endian): lwl / swl handle the bytes from the aligned word up to the address,
 * lwr / swr from the address to the end of its word. */
uint32_t rt_lwl(Ctx *c, uint32_t addr, uint32_t old) {
    unsigned shift = (addr & 3) * 8;
    uint32_t mem = LW(addr & ~3u);
    return (old & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
}

uint32_t rt_lwr(Ctx *c, uint32_t addr, uint32_t old) {
    unsigned shift = (addr & 3) * 8;
    uint32_t mem = LW(addr & ~3u);
    return (old & ~(0xFFFFFFFFu >> shift)) | (mem >> shift);
}

void rt_swl(Ctx *c, uint32_t addr, uint32_t v) {
    unsigned shift = (addr & 3) * 8;
    uint32_t mem = LW(addr & ~3u);
    SW(addr & ~3u, (mem & (0xFFFFFF00u << shift)) | (v >> (24 - shift)));
}

void rt_swr(Ctx *c, uint32_t addr, uint32_t v) {
    unsigned shift = (addr & 3) * 8;
    uint32_t mem = LW(addr & ~3u);
    SW(addr & ~3u, (mem & ~(0xFFFFFFFFu << shift)) | (v << shift));
}

/* FPU control: register 31 holds the condition (bit 23) and the rounding mode; 0 is the implementation register. */
uint32_t rt_cfc1(Ctx *c, int reg) {
    if (reg == 31)
        return (c->fcr31 & ~(1u << 23)) | ((uint32_t)(c->fpcond != 0) << 23);
    if (reg == 0)
        return 0x00003351u;     /* what PPSSPP reports */
    return 0;
}

void rt_ctc1(Ctx *c, int reg, uint32_t v) {
    if (reg == 31) {
        c->fcr31 = v & 0x0181FFFFu;
        c->fpcond = (v >> 23) & 1;
    }
}
