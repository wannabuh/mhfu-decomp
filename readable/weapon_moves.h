/* What every weapon class's moves are written with (weapon0_moves.c, weapon4_moves.c). */
#pragma once
#include "hit.h"
#include "names.h"

/* Named here by address as function names may change: */
#define set_motion f_EBOOT_088D5C74
#define frame_reached f_EBOOT_08864408
#define frame_just_reached f_EBOOT_08864348
#define reset_to_base f_game_task_09A678F8
#define set_movement_and_flags f_game_task_09A6F5C0
#define attack_begins f_game_task_09B62F48
#define state_flags_reset_motion f_EBOOT_08865CEC
#define camera_shake f_EBOOT_08812F90
#define can_chain f_game_task_09A8B058
#define before_chain f_game_task_09A8B100
#define clear_chain_flags f_game_task_09A8BC90
#define stick_says f_game_task_09A8B790
#define set_state_noted f_game_task_09A67878

#define HIT_LISTS_POINTER 0x09C190D0u
#define CAMERA_POINTER 0x089CBD04u
#define VSLOT_SET_STATE 0x6C
#define MOTION_PART0 0x80u      /* offset in the hunter of what frames are asked of */
#define QUEST_FLAG_800 0x800
#define QUEST_FLAG_1000 0x1000
#define MISC_FLAG_WEAPON_OUT 8
#define REACTION_BLOCK_TICKS 60

static inline uint32_t set_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state, uint32_t how) {
    return call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, how);
}

static inline void motion(Ctx *c, Hunter *self, uint32_t id, uint32_t blend, uint32_t start_frame) {
    call4(c, set_motion, GAME_ADDR(self), id, blend, start_frame);
}

/* A move's hit records: `which` is a number the game's table goes by */
static inline void attack(Ctx *c, Hunter *self, uint32_t which) {
    call4(c, attack_begins, LW(HIT_LISTS_POINTER), GAME_ADDR(self), which, 0);
}

static inline void back_to_base(Ctx *c, Hunter *self, uint32_t how) {
    call4(c, reset_to_base, GAME_ADDR(self), 0, how, 0);
}

static inline int motion_ended(const Hunter *self) {
    return !(self->motion_part0_flags & 1);
}

static inline int reached(Ctx *c, Hunter *self, float frame) {
    c->f[12].f = frame;
    return (uint8_t)call2(c, frame_reached, GAME_ADDR(self) + MOTION_PART0, 0) != 0;
}

/* From one frame up to another */
static inline int within(Ctx *c, Hunter *self, float from, float to) {
    return reached(c, self, from) && !reached(c, self, to);
}

static inline void shake(Ctx *c, Hunter *self, uint32_t how) {
    call3(c, camera_shake, LW(CAMERA_POINTER), how, GAME_ADDR(self));
}
