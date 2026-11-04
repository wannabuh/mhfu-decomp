/* What code rewritten by hand needs: the game's structures, its memory, and calls into the generated code. */
#pragma once
#include "funcs.h"
#include "mhfu_types.h"

/* A game address as a pointer to what is there, and back. Fields that hold pointers hold game addresses. */
#define OBJ(type, addr) ((type *)MEM(addr))
#define GAME_ADDR(p) ((uint32_t)((const uint8_t *)(p) - c->mem))

/* The game's global state: the structure *0x089CC558 points at. */
#define GAME() OBJ(GameState, LW(0x089CC558u))

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
