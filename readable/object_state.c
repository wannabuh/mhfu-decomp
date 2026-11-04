/* An object's state and its flags, for any object, and how a hunter changes state. Rewritten by hand from EBOOT 08865A64 and 08865CB8
 * and game_task 09A6F5C0 and 09A67878, the generated code still behind them (MHFU_HAND=0). */
#include "hand.h"
#include "names.h"

/* The flags are two words: a mask with this bit is for the second, in the rest of its bits */
#define STATE_FLAGS_SECOND_WORD 0x80000000u

/* What 09A6F5C0's arguments set. What the flags do isn't settled from this code. */
enum {
    STATE_FLAG_ARG2 = 1,
    STATE_FLAG_ARG3 = 2,
    STATE_FLAG_UNLESS_0X8000 = 8,
    OPTION_CLEAR_FLAG_8 = 0x8000,
};

#define VSLOT_SET_STATE 0x6C
#define STATE_GROUP_ALLOWED_WHILE_RESETTING 4
#define MISC_FLAG_STATE_SET_FROM_HERE 4     /* the state setter clears it */

/* EBOOT 08865A64 */
void hand_object_set_state_flags(Ctx *c) {
    Object *self = OBJ(Object, c->r[4]);
    uint32_t mask = c->r[5];
    if (mask & STATE_FLAGS_SECOND_WORD)
        self->state_flags_high |= mask & ~STATE_FLAGS_SECOND_WORD;
    else
        self->state_flags |= mask;
}

/* EBOOT 08865CB8 */
void hand_object_state_is(Ctx *c) {
    const Object *self = OBJ(Object, c->r[4]);
    c->r[2] = self->state_group == (uint8_t)c->r[5] && self->state == (uint8_t)c->r[6];
}

static void set_or_clear_flag(Ctx *c, Object *self, uint32_t flag, int on) {
    call2(c, on ? object_set_flags_288 : object_clear_flags_288, GAME_ADDR(self), flag);
}

/* game_task 09A6F5C0: the movement mode in the low byte of `options`, only 1 and 2 taken, then three flags, in this order. Most state
 * functions start with it. */
void hand_object_set_movement_mode_and_flags(Ctx *c) {
    Object *self = OBJ(Object, c->r[4]);
    uint32_t options = c->r[5];
    int16_t flag_1 = (int16_t)c->r[6], flag_2 = (int16_t)c->r[7];
    uint8_t mode = (uint8_t)options;
    self->movement_mode = mode == 1 || mode == 2 ? mode : 0;
    set_or_clear_flag(c, self, STATE_FLAG_UNLESS_0X8000, !(options & OPTION_CLEAR_FLAG_8));
    set_or_clear_flag(c, self, STATE_FLAG_ARG2, flag_1 != 0);
    set_or_clear_flag(c, self, STATE_FLAG_ARG3, flag_2 != 0);
}

/* game_task 09A67878: a hunter's state set through its vtable (group, state, how it's entered), and a flag noted. The local player's hunter
 * being put back in place takes only group 4 (the setter itself checks that too). */
void hand_hunter_set_state_noted(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t group = c->r[5], state = c->r[6], how = c->r[7];
    if (self->instance_index == GAME()->local_hunter_slot && self->reset_place_mode != 0 &&
        (uint16_t)group != STATE_GROUP_ALLOWED_WHILE_RESETTING)
        return;
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, how);
    self->misc_flags |= MISC_FLAG_STATE_SET_FROM_HERE;
}
