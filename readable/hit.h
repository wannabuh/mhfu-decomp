/* What the hit code passes around that isn't in the game's classes: the hit record ("node" in the function names) and the monster type table.
 * Offsets from the code that reads them: game_sub 09C41F58, 09C42750, 09C43000, 09C43558, 09C45008. Names are for what the code does with
 * them. What +0x25's bits and part records' bytes are in the game's terms isn't settled: see hit_damage.c. */
#pragma once
#include <stddef.h>
#include "hand.h"

typedef struct HitRecord {
    uint8_t pad_00[0x04];
    uint32_t next;              /* +0x04 the next one, 0 at the end */
    uint8_t pad_08[0x06];
    uint8_t owner_kind;         /* +0x0E 0xFF: no owner */
    uint8_t owner_table;        /* +0x0F 0: a hunter's slot; otherwise the monster table's */
    uint8_t pad_10[0x04];
    uint8_t state;              /* +0x14 2: tests targets */
    uint8_t owner_slot;         /* +0x15 */
    uint8_t connected;          /* +0x16 */
    uint8_t pad_17;
    uint16_t area_id;           /* +0x18 only targets in this area */
    uint16_t hit_id;            /* +0x1A a target logs it, so as not to take it twice; 0xFFFF: not logged */
    uint8_t pad_1C[0x02];       /* the attacker is told +0x1C's address */
    uint8_t damage;             /* +0x1E before attack, defense and resistance */
    uint8_t kind;               /* +0x1F to the target's received_hit_type */
    uint8_t pad_20;
    uint8_t flags;              /* +0x21 to pending_hit_flags */
    uint8_t pad_22[0x02];
    uint8_t power;              /* +0x24 to received_hit_power */
    uint8_t damage_bits;        /* +0x25 0x04 0x10 0x40 0x20 0x80 choose the resistance; to pending_hit_bits */
    uint8_t pad_26;
    uint8_t sound_choice;       /* +0x27 */
    uint16_t buildup_mask;      /* +0x28 */
    uint8_t buildup_amount;     /* +0x2A */
    uint8_t pad_2B;
    uint8_t freeze_ticks;       /* +0x2C to the attacker's freeze_ticks_pending */
    uint8_t drain_amount;       /* +0x2D bit 0x80: goes to the drain's other amount, low 7 bits */
    uint8_t uses;               /* +0x2E low 4 bits: uses left */
    uint8_t pad_2F;
    uint8_t direct_buildup;     /* +0x30 */
    uint8_t pad_31[0x03];
    uint32_t shapes;            /* +0x34 the attacker's shape records, 0x28 bytes each, ended by -1 */
    uint8_t pad_38[0x08];
    float shape_a[4];           /* +0x40 [1] is its height, the test of a target's height */
    float shape_b[4];           /* +0x50 */
    uint8_t pad_60[0x08];
    uint32_t last_target;       /* +0x68 */
    uint8_t last_target_kind;   /* +0x6C */
    uint8_t last_target_slot;   /* +0x6D */
    uint8_t pad_6E[0x02];
    uint32_t last_target_part;  /* +0x70 */
    uint8_t last_grade;         /* +0x74 */
    uint8_t pad_75[0x02];
    uint8_t damage_percent;     /* +0x77 0: all of it */
} HitRecord;
_Static_assert(offsetof(HitRecord, next) == 0x04 && offsetof(HitRecord, owner_kind) == 0x0E && offsetof(HitRecord, state) == 0x14
               && offsetof(HitRecord, area_id) == 0x18 && offsetof(HitRecord, damage) == 0x1E && offsetof(HitRecord, flags) == 0x21
               && offsetof(HitRecord, power) == 0x24 && offsetof(HitRecord, sound_choice) == 0x27 && offsetof(HitRecord, buildup_mask) == 0x28
               && offsetof(HitRecord, buildup_amount) == 0x2A && offsetof(HitRecord, freeze_ticks) == 0x2C && offsetof(HitRecord, uses) == 0x2E
               && offsetof(HitRecord, direct_buildup) == 0x30 && offsetof(HitRecord, shapes) == 0x34 && offsetof(HitRecord, shape_a) == 0x40
               && offsetof(HitRecord, shape_b) == 0x50 && offsetof(HitRecord, last_target) == 0x68 && offsetof(HitRecord, last_target_kind) == 0x6C
               && offsetof(HitRecord, last_target_part) == 0x70 && offsetof(HitRecord, last_grade) == 0x74 && offsetof(HitRecord, damage_percent) == 0x77,
               "HitRecord");

/* In a target */
#define HIT_LOG 0x33Eu              /* the ids of the last 16 hits, as u16, at hit_log_index */
#define HIT_CONTACT 0x370u          /* 4 words: where it was hit */

/* The monster type table: a record per subtype (+0x1E8) */
#define MONSTER_TYPE_TABLE 0x09BB8AB0u
#define MONSTER_TYPE_SIZE 0x1D0u
#define MONSTER_TYPE(subtype, offset) (MONSTER_TYPE_TABLE + (uint32_t)(subtype) * MONSTER_TYPE_SIZE + (offset))
#define MONSTER_TYPE_PARTS 0x50             /* its part records, 0x28 bytes each, ended by -1 */
#define MONSTER_TYPE_HIT_ZONES 0x10C        /* -> a table by +0x481 of the monster, -> 10 bytes per part: game_sub 09C43558 */
#define MONSTER_TYPE_ALERT_ARG 0x148        /* s16 */
#define MONSTER_TYPE_MASKED_SLOT 0x14A      /* s8: a damage slot, or -1 */
#define MONSTER_TYPE_SLOT_MASK 0x14B
