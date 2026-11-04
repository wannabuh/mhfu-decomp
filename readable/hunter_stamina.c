/* The hunter's stamina: spending it and getting it back. Rewritten by hand from game_task 09A6A2D0, 09A6A370 and 09A6A480; the
 * generated code they replace still runs with MHFU_HAND=0, which is how these are checked (docs/readable.md).
 *
 * Only the local player's hunter changes its own stamina here: a remote hunter's arrives over the network. */
#include "hand.h"
#include "names.h"

/* Skills that change stamina use. What the game calls them isn't known from the code. */
enum {
    SKILL_STAMINA_USE_HALVED = 0xA8,    /* costs half, or a cost of 1 every other time */
    SKILL_STAMINA_USE_SPIKES = 0xA9,    /* every fifth tick costs double */
    TIMED_EFFECT_NO_STAMINA_LOSS = 5,   /* in the timed effects, from +0x6A0 */
};

static int is_local_hunter(Ctx *c, const Hunter *self) {
    return self->instance_index == GAME()->local_hunter_slot;
}

static int has_skill(Ctx *c, Hunter *self, uint32_t skill) {
    return (uint8_t)call2(c, hunter_has_active_skill, GAME_ADDR(self), skill) == 1;
}

/* game_task 09A6A808: spending is blocked for a while, or by a timed effect. Only for the local player's hunter. */
void hand_hunter_stamina_loss_blocked(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    if (!is_local_hunter(c, self))
        c->r[2] = 0;
    else if (self->stamina_loss_block_ticks > 0)
        c->r[2] = 1;
    else
        c->r[2] = (uint8_t)call2(c, hunter_short_array_6a0_entry_is_positive, GAME_ADDR(self), TIMED_EFFECT_NO_STAMINA_LOSS) == 1;
}

/* Adds to stamina, kept within 0 and the maximum. Something can block spending altogether. */
static void stamina_add(Ctx *c, Hunter *self, int16_t amount) {
    if (!is_local_hunter(c, self))
        return;
    if (amount < 0 && (uint8_t)call1(c, hunter_stamina_loss_blocked, GAME_ADDR(self)) == 1)
        return;
    self->stamina += amount;
    if (self->stamina < 0)
        self->stamina = 0;
    if (self->stamina > self->stamina_max)
        self->stamina = self->stamina_max;
}

/* A cost changed by the skills, given a count that rises once a tick. Returns 0 if a cost of 1 is skipped this time. Each skill is
 * asked for afresh, and the count is taken from where the caller says: the two callers below don't take it at the same moment. */
static int apply_skills(Ctx *c, Hunter *self, int16_t *amount, const uint16_t *count) {
    if (*amount == -1) {
        if (has_skill(c, self, SKILL_STAMINA_USE_HALVED) && (*count & 1))
            return 0;
    } else if (has_skill(c, self, SKILL_STAMINA_USE_HALVED)) {
        *amount /= 2;
    }
    if (has_skill(c, self, SKILL_STAMINA_USE_SPIKES) && *count % 5 == 0)
        *amount *= 2;
    return 1;
}

/* game_task 09A6A2D0 */
void hand_hunter_stamina_add(Ctx *c) {
    stamina_add(c, OBJ(Hunter, c->r[4]), (int16_t)c->r[5]);
}

/* game_task 09A6A370: so much a game tick, skills applied. Native frame rate: counted at 30ths, as the game's own rate has it. */
void hand_hunter_stamina_use(Ctx *c) {
    if (!rt_tick30)
        return;
    Hunter *self = OBJ(Hunter, c->r[4]);
    int16_t amount = (int16_t)c->r[5];
    if (!is_local_hunter(c, self))
        return;
    if (amount < 0 && !apply_skills(c, self, &amount, &GAME()->tick_30ths))
        return;
    call2(c, hunter_stamina_add_clamped, GAME_ADDR(self), (uint32_t)(int32_t)amount);
}

/* game_task 09A6A480: the same, on every other tick, and with the skills' count going at half speed. */
void hand_hunter_stamina_use_alternate_ticks(Ctx *c) {
    if (!rt_tick30)
        return;
    Hunter *self = OBJ(Hunter, c->r[4]);
    int16_t amount = (int16_t)c->r[5];
    if (!is_local_hunter(c, self))
        return;
    uint16_t tick = GAME()->tick_30ths;
    if (!(tick & 1))
        return;
    uint16_t half_tick = tick >> 1;
    if (amount < 0 && !apply_skills(c, self, &amount, &half_tick))
        return;
    call2(c, hunter_stamina_add_clamped, GAME_ADDR(self), (uint32_t)(int32_t)amount);
}
