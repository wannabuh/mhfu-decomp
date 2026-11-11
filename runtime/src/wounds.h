/* Wounds (gpu.c): a hunter's hit on a monster leaves a slash where it landed, staying on the model as it moves.
 * Reads only: nothing of the game's is written or called, and none of its random numbers drawn. MHFU_WOUNDS=0: none. */
#ifndef WOUNDS_H
#define WOUNDS_H
#include <stdint.h>

/* readable/hit_detect.c, as a hit lands on a monster: the hit record, the attacker and the monster (objects), where
 * the game put the contact (a point, in the world) and the attacker's shape (its type word: hit_detect.c's frame) */
/* before the game works the hit out (land_on_monster): the monster, for what the hit then adds to its damage */
/* each tick a record's shape is tried against a target, the shape made (its type word): how the weapon moves */
void wound_swing(uint32_t record, uint32_t shape_id, uint32_t attacker_shape);
void wound_before(uint32_t monster);
void wound_hit(uint32_t record, uint32_t attacker, uint32_t monster, uint32_t contact, uint32_t attacker_shape);

#endif
