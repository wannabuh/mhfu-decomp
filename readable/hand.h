/* What code rewritten by hand needs: the game's structures, its memory, and calls into the generated code. */
#pragma once
#include "funcs.h"
#include "mhfu_types.h"

/* A game address as a pointer to what is there, and back. Fields that hold pointers hold game addresses. */
#define OBJ(type, addr) ((type *)MEM(addr))
#define GAME_ADDR(p) ((uint32_t)((const uint8_t *)(p) - c->mem))

/* The game's global state: the structure *0x089CC558 points at. */
#define GAME() OBJ(GameState, LW(0x089CC558u))

/* The VFPU's vtfm3 on a matrix kept as rows of 4 floats, transposed as the game loads them. Each sum starts from 0 and multiplies by
 * the zeros too, as it does: a result can only be exact if it is worked out the same way. */
static inline void rotate_by_rows(float out[3], const float *rows, float x, float y, float z) {
    for (int i = 0; i < 3; i++)
        out[i] = 0.0f + rows[i] * x + rows[4 + i] * y + rows[8 + i] * z;
}

/* A generated function called: arguments in the argument registers, its result from v0. Nothing else carries over a call. */
static inline uint32_t call1(Ctx *c, void (*fn)(Ctx *), uint32_t a0) {
    c->r[4] = a0;
    fn(c);
    return c->r[2];
}
static inline uint32_t call2(Ctx *c, void (*fn)(Ctx *), uint32_t a0, uint32_t a1) {
    c->r[4] = a0;
    c->r[5] = a1;
    fn(c);
    return c->r[2];
}
static inline uint32_t call3(Ctx *c, void (*fn)(Ctx *), uint32_t a0, uint32_t a1, uint32_t a2) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    fn(c);
    return c->r[2];
}
static inline uint32_t call4(Ctx *c, void (*fn)(Ctx *), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    fn(c);
    return c->r[2];
}

/* A virtual method called: the function in slot `slot` of the vtable at +0 of the object at `self` (a game address) */
static inline uint32_t call_method0(Ctx *c, uint32_t self, uint32_t slot) {
    c->r[4] = self;
    rt_call(c, LW(LW(self) + slot));
    return c->r[2];
}
static inline uint32_t call_method1(Ctx *c, uint32_t self, uint32_t slot, uint32_t a1) {
    c->r[4] = self;
    c->r[5] = a1;
    rt_call(c, LW(LW(self) + slot));
    return c->r[2];
}
static inline uint32_t call_method3(Ctx *c, uint32_t self, uint32_t slot, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = self;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    rt_call(c, LW(LW(self) + slot));
    return c->r[2];
}

/* A generated function in an overlay not resident at link time, called by its address as the game does */
static inline uint32_t call1_at(Ctx *c, uint32_t addr, uint32_t a0) {
    c->r[4] = a0;
    rt_call(c, addr);
    return c->r[2];
}
static inline uint32_t call2_at(Ctx *c, uint32_t addr, uint32_t a0, uint32_t a1) {
    c->r[4] = a0;
    c->r[5] = a1;
    rt_call(c, addr);
    return c->r[2];
}
static inline uint32_t call3_at(Ctx *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    rt_call(c, addr);
    return c->r[2];
}
static inline uint32_t call4_at(Ctx *c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    rt_call(c, addr);
    return c->r[2];
}
