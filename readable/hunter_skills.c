/* What a hunter has in effect: the skills and the bonuses it hunts with. Rewritten by hand from EBOOT 088D6A1C and 088D6A54, the generated
 * code still behind them (MHFU_HAND=0). Both are called often: plain loops over a few bytes.
 *
 * Ids of 0 mean an empty entry, and these don't treat them specially: asked for 0, they answer for any empty entry. */
#include <stddef.h>

#include "hand.h"
#include "names.h"

enum {
    ACTIVE_SKILLS = 6,
    HUNT_BONUSES = 5,
    HUNT_BONUS_RECORD_SIZE = 0x8C,      /* a player's record in the game state */
};

static inline int list_has(const uint8_t *list, int count, uint8_t id) {
    for (int i = 0; i < count; i++)
        if (list[i] == id)
            return 1;
    return 0;
}

/* EBOOT 088D6A1C */
void hand_hunter_has_active_skill(Ctx *c) {
    const uint8_t *hunter = MEM(c->r[4]);
    c->r[2] = list_has(hunter + offsetof(HunterBase, active_skill_id_0), ACTIVE_SKILLS, (uint8_t)c->r[5]);
}

/* EBOOT 088D6A54: the local player's bonuses are in the game state, the others' in their records */
void hand_hunter_has_hunt_bonus(Ctx *c) {
    uint16_t slot = OBJ(HunterBase, c->r[4])->instance_index;
    const GameState *game = GAME();
    const uint8_t *bonuses = (const uint8_t *)game;
    if (slot == game->local_hunter_slot)
        bonuses += offsetof(GameState, hunt_bonus_id_0);
    else
        bonuses += offsetof(GameState, record_hunt_bonus_id_0) + (uint32_t)slot * HUNT_BONUS_RECORD_SIZE;
    c->r[2] = list_has(bonuses, HUNT_BONUSES, (uint8_t)c->r[5]);
}
