/* Wounds (gpu.c): a hunter's hit on a monster leaves a slash where it landed, staying on the model as it moves.
 * Reads only: nothing of the game's is written or called, and none of its random numbers drawn. MHFU_WOUNDS=0: none. */
#ifndef WOUNDS_H
#define WOUNDS_H
#include <stdint.h>

/* readable/hit_detect.c, as a hit lands on a monster: the hit record, the attacker and the monster (objects), where
 * the game put the contact (a point, in the world) and the attacker's shape (its type word: hit_detect.c's frame) */
void wound_hit(uint32_t record, uint32_t attacker, uint32_t monster, uint32_t contact, uint32_t attacker_shape);

#endif
