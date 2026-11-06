/* Code rewritten by hand against the generated code it replaces (recomp/patches.py HAND), on inputs the recordings never reach.
 *
 * Usage: handtest <RAM snapshot from a quest> [iterations] [stubs <WORK/signatures.tsv>] [only <part of a case's name>]
 * Takes the snapshot's hunter and game state, randomises what a function's branches depend on, then runs each version from the same
 * state and compares all of RAM but the stack (dead bytes there are not behaviour), the callee-saved registers and, where the
 * function returns one, v0. The random numbers are fixed, so a run repeats. Build it with HAND_FLAGS for coverage to see what it
 * reached. A new rewrite: a row in `cases`, and its inputs in randomise(), or a function of its own (the row's last columns) if they
 * shouldn't disturb the other cases: then its row goes last, as what it sets stays set.
 *
 * In a build generated with RECOMP_TRACE_ALL=1 it also tests each function alone (make build/handtest in that worktree): everything
 * it calls is a stub that logs its arguments and returns the next random result, the same for both versions. So every branch can be
 * reached whatever the game would really return, and the two call sequences are compared too. */
#include "runtime.h"
#include "funcs.h"
#include "mhfu_types.h"
#include "../../readable/hit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_START 0x08800000u
#define SNAP_SIZE 0x01800000u
#define TEST_SP 0x09FF0000u
#define STACK_LOW (TEST_SP - 0x4000u)
#define HUNTER_VTABLE 0x089BB3CCu
#define GAME_POINTER 0x089CC558u

struct Pokes;
typedef struct {
    const char *name;
    void (*fn)(Ctx *);
    int returns;
    int small_a1;       /* a1 is a mode, 0 to 2, not an amount */
    uint32_t addr;
    void (*randomise_more)(Ctx *, uint32_t hunter);     /* also: long chains of results that must be 0 are reached, callees change fields */
    void (*inputs)(Ctx *, uint32_t hunter, uint32_t args[3]);   /* the function's own inputs and a1-a3 (a2, a3 otherwise rubbish). Its row goes last */
    const struct Pokes *pokes;      /* what callees may change, for a function that reads fields again after its calls */
} Case;

static void randomise_upkeep(Ctx *c, uint32_t hunter);
static void inputs_loss_blocked(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_active_skill(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_hunt_bonus(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_face_stick(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_flags(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_state_is(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_movement(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_classify(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_countdowns(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_overlays(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_airborne(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_transient(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_real_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_reset(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_run_state(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_group0(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_tick(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_set_motion(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_motion_copy(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_copy_pad(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_common(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_stamina_scaled(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_motion_update(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_state_run(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_add_health(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_can_guard(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_recalc_defense(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_received_hit(Ctx *c, uint32_t hunter, uint32_t args[3]);
extern const struct Pokes run_pokes, upkeep_pokes, set_state_pokes, reset_pokes, tick_pokes, copy_pad_pokes, common_pokes;
static void inputs_weapon0(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_script(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_script_end_block(Ctx *c, uint32_t hunter, uint32_t args[3]);
/* The cases' inputs for the script's opcodes' handlers: made by opcode number */
static void inputs_script_op(Ctx *c, uint32_t hunter, uint32_t args[3], uint32_t op);
#define OP_INPUTS(n) static void inputs_op_##n(Ctx *c, uint32_t hunter, uint32_t args[3]) { inputs_script_op(c, hunter, args, 0x##n); }
OP_INPUTS(83)
OP_INPUTS(16)
OP_INPUTS(15)
OP_INPUTS(81)
OP_INPUTS(02)
OP_INPUTS(12)
OP_INPUTS(82)
OP_INPUTS(19)
OP_INPUTS(10)
OP_INPUTS(34)
OP_INPUTS(42)
OP_INPUTS(14)
OP_INPUTS(4D)
OP_INPUTS(7C)
OP_INPUTS(77)
OP_INPUTS(76)
OP_INPUTS(3B)
OP_INPUTS(63)
OP_INPUTS(51)
OP_INPUTS(38)
OP_INPUTS(21)
OP_INPUTS(17)
OP_INPUTS(18)
OP_INPUTS(35)
OP_INPUTS(28)
OP_INPUTS(70)
OP_INPUTS(08)
OP_INPUTS(2E)
OP_INPUTS(2A)
OP_INPUTS(29)
OP_INPUTS(3E)
OP_INPUTS(5E)
OP_INPUTS(0F)
OP_INPUTS(43)
OP_INPUTS(71)
OP_INPUTS(13)
OP_INPUTS(06)
OP_INPUTS(53)
OP_INPUTS(2C)
OP_INPUTS(54)
OP_INPUTS(27)
OP_INPUTS(79)
OP_INPUTS(1B)
OP_INPUTS(40)
static void inputs_mask_amounts(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_resist_skills(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_hit_zones(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_recalc_resists(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_add_resist(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_resist_bytes(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_scale_table(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_clear_hit_fields(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_register_hit(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_sum_damage(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_apply_damage(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_test_if_in_map(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_test_target(Ctx *c, uint32_t hunter, uint32_t args[3]);
static void inputs_hit_manager(Ctx *c, uint32_t hunter, uint32_t args[3]);
extern const struct Pokes add_health_pokes, can_guard_pokes, recalc_defense_pokes, received_hit_pokes;
extern const struct Pokes recalc_resists_pokes, hit_zones_pokes, weapon0_pokes, script_pokes;
extern const struct Pokes register_hit_pokes, sum_damage_pokes, apply_damage_pokes, test_if_in_map_pokes, test_target_pokes, hit_manager_pokes;

static const Case cases[] = {
    {"hunter_stamina_add 09A6A2D0", f_game_task_09A6A2D0, 0, 0, 0x09A6A2D0, NULL, NULL, NULL},
    {"hunter_stamina_use 09A6A370", f_game_task_09A6A370, 0, 0, 0x09A6A370, NULL, NULL, NULL},
    {"hunter_stamina_use_alternate_ticks 09A6A480", f_game_task_09A6A480, 0, 0, 0x09A6A480, NULL, NULL, NULL},
    {"hunter_state_sprint 09A9E288", f_game_task_09A9E288, 0, 1, 0x09A9E288, NULL, NULL, NULL},
    {"hunter_state_sprint_linked 09A9E690", f_game_task_09A9E690, 0, 1, 0x09A9E690, NULL, NULL, NULL},
    {"hunter_state_out_of_breath 09A9EAE8", f_game_task_09A9EAE8, 0, 1, 0x09A9EAE8, NULL, NULL, NULL},
    {"hunter_tick_upkeep 09A68FC8", f_game_task_09A68FC8, 0, 0, 0x09A68FC8, randomise_upkeep, NULL, &upkeep_pokes},
    {"hunter_stamina_loss_blocked 09A6A808", f_game_task_09A6A808, 1, 0, 0x09A6A808, NULL, inputs_loss_blocked, NULL},
    {"hunter_has_active_skill 088D6A1C", f_EBOOT_088D6A1C, 1, 0, 0x088D6A1C, NULL, inputs_active_skill, NULL},
    {"hunter_has_hunt_bonus 088D6A54", f_EBOOT_088D6A54, 1, 0, 0x088D6A54, NULL, inputs_hunt_bonus, NULL},
    {"hunter_set_state_noted 09A67878", f_game_task_09A67878, 0, 0, 0x09A67878, NULL, inputs_set_state, NULL},
    {"hunter_face_stick_direction 09A8B578", f_game_task_09A8B578, 0, 0, 0x09A8B578, NULL, inputs_face_stick, NULL},
    {"object_set_state_flags 08865A64", f_EBOOT_08865A64, 0, 0, 0x08865A64, NULL, inputs_set_flags, NULL},
    {"object_state_is 08865CB8", f_EBOOT_08865CB8, 1, 0, 0x08865CB8, NULL, inputs_state_is, NULL},
    {"object_set_movement_mode_and_flags 09A6F5C0", f_game_task_09A6F5C0, 0, 0, 0x09A6F5C0, NULL, inputs_movement, NULL},
    {"hunter_classify_move_input 09AA5FB0", f_game_task_09AA5FB0, 1, 0, 0x09AA5FB0, NULL, inputs_classify, NULL},
    {"hunter_tick_countdowns 088D6488", f_EBOOT_088D6488, 0, 0, 0x088D6488, NULL, inputs_countdowns, NULL},
    {"hunter_clear_overlays 09A8B3F0", f_game_task_09A8B3F0, 0, 0, 0x09A8B3F0, NULL, inputs_overlays, NULL},
    {"hunter_start_airborne_motion 09A67F30", f_game_task_09A67F30, 0, 0, 0x09A67F30, NULL, inputs_airborne, NULL},
    {"hunter_clear_transient_action_fields 09A68700", f_game_task_09A68700, 0, 0, 0x09A68700, NULL, inputs_transient, NULL},
    {"hunter_set_state 09A67630", f_game_task_09A67630, 0, 0, 0x09A67630, NULL, inputs_real_set_state, &set_state_pokes},
    {"hunter_reset_to_base 09A678F8", f_game_task_09A678F8, 0, 0, 0x09A678F8, NULL, inputs_reset, &reset_pokes},
    {"hunter_run_state 09A67568", f_game_task_09A67568, 0, 0, 0x09A67568, NULL, inputs_run_state, NULL},
    {"hunter_group0_state 09A9A9E0", f_game_task_09A9A9E0, 0, 0, 0x09A9A9E0, NULL, inputs_group0, NULL},
    {"hunter_state_function 09A66198", f_game_task_09A66198, 0, 0, 0x09A66198, NULL, inputs_tick, &tick_pokes},
    {"hunter_set_motion 088D5C34", f_EBOOT_088D5C34, 0, 0, 0x088D5C34, NULL, inputs_set_motion, NULL},
    {"hunter_tick_motion_copy 088D1DD0", f_EBOOT_088D1DD0, 0, 0, 0x088D1DD0, NULL, inputs_motion_copy, NULL},
    {"hunter_copy_pad 088D61CC", f_EBOOT_088D61CC, 0, 0, 0x088D61CC, NULL, inputs_copy_pad, &copy_pad_pokes},
    {"hunter_common_input 09AA5270", f_game_task_09AA5270, 0, 0, 0x09AA5270, NULL, inputs_common, &common_pokes},
    {"hunter_stamina_add_scaled 09A6A598", f_game_task_09A6A598, 0, 0, 0x09A6A598, NULL, inputs_stamina_scaled, NULL},
    {"hunter_motion_update 088D5C7C", f_EBOOT_088D5C7C, 0, 0, 0x088D5C7C, NULL, inputs_motion_update, NULL},
    {"hunter_state_run 09A665F0", f_game_task_09A665F0, 0, 0, 0x09A665F0, NULL, inputs_state_run, &run_pokes},
    {"hunter_add_health 088D6594", f_EBOOT_088D6594, 1, 0, 0x088D6594, NULL, inputs_add_health, &add_health_pokes},
    {"hunter_can_guard_received_hit 09A6C968", f_game_task_09A6C968, 1, 0, 0x09A6C968, NULL, inputs_can_guard, &can_guard_pokes},
    {"hunter_recalc_defense_value 088D92F8", f_EBOOT_088D92F8, 0, 0, 0x088D92F8, NULL, inputs_recalc_defense, &recalc_defense_pokes},
    {"hunter_process_received_hit 09A6B130", f_game_task_09A6B130, 0, 0, 0x09A6B130, NULL, inputs_received_hit, &received_hit_pokes},
    {"hunter_recalc_resist_values 088D96BC", f_EBOOT_088D96BC, 0, 0, 0x088D96BC, NULL, inputs_recalc_resists, &recalc_resists_pokes},
    {"hunter_add_resist_value_by_id 088D9EB8", f_EBOOT_088D9EB8, 0, 0, 0x088D9EB8, NULL, inputs_add_resist, NULL},
    {"hunter_resist_values_add_game_bytes 088D9F3C", f_EBOOT_088D9F3C, 0, 0, 0x088D9F3C, NULL, inputs_resist_bytes, NULL},
    {"entity_pending_damage_scale 09AC9318", f_game_task_09AC9318, 0, 0, 0x09AC9318, NULL, inputs_scale_table, NULL},
    {"hunter_clear_received_hit_fields 09A6FB30", f_game_task_09A6FB30, 0, 0, 0x09A6FB30, NULL, inputs_clear_hit_fields, NULL},
    {"hunter_resist_skills 088D9880", f_EBOOT_088D9880, 0, 0, 0x088D9880, NULL, inputs_resist_skills, NULL},
    {"monster_hit_zones 09C43558", f_game_sub_09C43558, 0, 0, 0x09C43558, NULL, inputs_hit_zones, &hit_zones_pokes},
    {"weapon0_dispatch 09BA0AE0", f_game_task_09BA0AE0, 0, 0, 0x09BA0AE0, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_057e 09BA0CD8", f_game_task_09BA0CD8, 0, 0, 0x09BA0CD8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_057f 09BA1108", f_game_task_09BA1108, 0, 0, 0x09BA1108, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_0594 09BA15D8", f_game_task_09BA15D8, 0, 0, 0x09BA15D8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_057c 09BA1E38", f_game_task_09BA1E38, 0, 0, 0x09BA1E38, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_0583 09BA1FB8", f_game_task_09BA1FB8, 0, 0, 0x09BA1FB8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_chain_kind 09BA2E28", f_game_task_09BA2E28, 1, 0, 0x09BA2E28, NULL, inputs_weapon0, NULL},
    {"weapon0_chain_to 09BA2E68", f_game_task_09BA2E68, 0, 0, 0x09BA2E68, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_connected 09BA32E8", f_game_task_09BA32E8, 1, 0, 0x09BA32E8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_charge 09BA2060", f_game_task_09BA2060, 0, 0, 0x09BA2060, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_charged 09BA2608", f_game_task_09BA2608, 0, 0, 0x09BA2608, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_pick_state 09BA2D10", f_game_task_09BA2D10, 0, 0, 0x09BA2D10, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon0_enter_14 09BA31B0", f_game_task_09BA31B0, 0, 0, 0x09BA31B0, NULL, inputs_weapon0, NULL},
    {"weapon0_apply_tier 09BA3200", f_game_task_09BA3200, 0, 0, 0x09BA3200, NULL, inputs_weapon0, NULL},
    {"weapon0_tier 088D1BA8", f_EBOOT_088D1BA8, 1, 0, 0x088D1BA8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_dispatch 09BA7EF0", f_game_task_09BA7EF0, 0, 0, 0x09BA7EF0, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_0579 09BA7FF8", f_game_task_09BA7FF8, 0, 0, 0x09BA7FF8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_057c 09BA8450", f_game_task_09BA8450, 0, 0, 0x09BA8450, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_057d 09BA8700", f_game_task_09BA8700, 0, 0, 0x09BA8700, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_0588 09BA8818", f_game_task_09BA8818, 0, 0, 0x09BA8818, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_057e 09BA88D8", f_game_task_09BA88D8, 0, 0, 0x09BA88D8, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_0580 09BA8BC0", f_game_task_09BA8BC0, 0, 0, 0x09BA8BC0, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_pick 09BA8D40", f_game_task_09BA8D40, 0, 0, 0x09BA8D40, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_evade 09BA8E30", f_game_task_09BA8E30, 0, 0, 0x09BA8E30, NULL, inputs_weapon0, &weapon0_pokes},
    {"weapon4_enter 09BA8E80", f_game_task_09BA8E80, 0, 0, 0x09BA8E80, NULL, inputs_weapon0, NULL},
    {"weapon4_0c_gather 09BA8F40", f_game_task_09BA8F40, 0, 0, 0x09BA8F40, NULL, inputs_weapon0, &weapon0_pokes},
    {"op83 09ABA8F8", f_game_task_09ABA8F8, 1, 0, 0x09ABA8F8, NULL, inputs_op_83, &script_pokes},
    {"op16 09ABD778", f_game_task_09ABD778, 1, 0, 0x09ABD778, NULL, inputs_op_16, &script_pokes},
    {"op15 09ABC600", f_game_task_09ABC600, 1, 0, 0x09ABC600, NULL, inputs_op_15, &script_pokes},
    {"op81 09ABAAF8", f_game_task_09ABAAF8, 1, 0, 0x09ABAAF8, NULL, inputs_op_81, &script_pokes},
    {"op02 09ABA340", f_game_task_09ABA340, 1, 0, 0x09ABA340, NULL, inputs_op_02, &script_pokes},
    {"op12 09ABA440", f_game_task_09ABA440, 1, 0, 0x09ABA440, NULL, inputs_op_12, &script_pokes},
    {"op82 09ABAB38", f_game_task_09ABAB38, 1, 0, 0x09ABAB38, NULL, inputs_op_82, &script_pokes},
    {"op19 09ABCC10", f_game_task_09ABCC10, 1, 0, 0x09ABCC10, NULL, inputs_op_19, &script_pokes},
    {"op10 09ABC9B8", f_game_task_09ABC9B8, 1, 0, 0x09ABC9B8, NULL, inputs_op_10, &script_pokes},
    {"op34 09ABB670", f_game_task_09ABB670, 1, 0, 0x09ABB670, NULL, inputs_op_34, &script_pokes},
    {"op13 09ABA858", f_game_task_09ABA858, 1, 0, 0x09ABA858, NULL, inputs_op_13, &script_pokes},
    {"op06 09AB8E48", f_game_task_09AB8E48, 1, 0, 0x09AB8E48, NULL, inputs_op_06, &script_pokes},
    {"op53 09ABA4B0", f_game_task_09ABA4B0, 1, 0, 0x09ABA4B0, NULL, inputs_op_53, &script_pokes},
    {"op2C 09ABB040", f_game_task_09ABB040, 1, 0, 0x09ABB040, NULL, inputs_op_2C, &script_pokes},
    {"op54 09ABB8B8", f_game_task_09ABB8B8, 1, 0, 0x09ABB8B8, NULL, inputs_op_54, &script_pokes},
    {"op27 09ABB188", f_game_task_09ABB188, 1, 0, 0x09ABB188, NULL, inputs_op_27, &script_pokes},
    {"op79 09AC3798", f_game_task_09AC3798, 1, 0, 0x09AC3798, NULL, inputs_op_79, &script_pokes},
    {"op1B 09ABED10", f_game_task_09ABED10, 1, 0, 0x09ABED10, NULL, inputs_op_1B, &script_pokes},
    {"op40 09ABA888", f_game_task_09ABA888, 1, 0, 0x09ABA888, NULL, inputs_op_40, &script_pokes},
    {"op35 09AC02D0", f_game_task_09AC02D0, 1, 0, 0x09AC02D0, NULL, inputs_op_35, &script_pokes},
    {"op28 09ABBB10", f_game_task_09ABBB10, 1, 0, 0x09ABBB10, NULL, inputs_op_28, &script_pokes},
    {"op70 09ABFCF0", f_game_task_09ABFCF0, 1, 0, 0x09ABFCF0, NULL, inputs_op_70, &script_pokes},
    {"op08 09ABD008", f_game_task_09ABD008, 1, 0, 0x09ABD008, NULL, inputs_op_08, &script_pokes},
    {"op2E 09ABB788", f_game_task_09ABB788, 1, 0, 0x09ABB788, NULL, inputs_op_2E, &script_pokes},
    {"op2A 09ABF9D0", f_game_task_09ABF9D0, 1, 0, 0x09ABF9D0, NULL, inputs_op_2A, &script_pokes},
    {"op29 09ABF8D0", f_game_task_09ABF8D0, 1, 0, 0x09ABF8D0, NULL, inputs_op_29, &script_pokes},
    {"op3E 09AC07C8", f_game_task_09AC07C8, 1, 0, 0x09AC07C8, NULL, inputs_op_3E, &script_pokes},
    {"op5E 09ABBC90", f_game_task_09ABBC90, 1, 0, 0x09ABBC90, NULL, inputs_op_5E, &script_pokes},
    {"op0F 09ABD630", f_game_task_09ABD630, 1, 0, 0x09ABD630, NULL, inputs_op_0F, &script_pokes},
    {"op43 09ABB428", f_game_task_09ABB428, 1, 0, 0x09ABB428, NULL, inputs_op_43, &script_pokes},
    {"op71 09ABBA08", f_game_task_09ABBA08, 1, 0, 0x09ABBA08, NULL, inputs_op_71, &script_pokes},
    {"op42 09ABCC40", f_game_task_09ABCC40, 1, 0, 0x09ABCC40, NULL, inputs_op_42, &script_pokes},
    {"op14 09ABEA38", f_game_task_09ABEA38, 1, 0, 0x09ABEA38, NULL, inputs_op_14, &script_pokes},
    {"op4D 09AC1458", f_game_task_09AC1458, 1, 0, 0x09AC1458, NULL, inputs_op_4D, &script_pokes},
    {"op7C 09AC3550", f_game_task_09AC3550, 1, 0, 0x09AC3550, NULL, inputs_op_7C, &script_pokes},
    {"op77 09AC3CA8", f_game_task_09AC3CA8, 1, 0, 0x09AC3CA8, NULL, inputs_op_77, &script_pokes},
    {"op76 09AC3970", f_game_task_09AC3970, 1, 0, 0x09AC3970, NULL, inputs_op_76, &script_pokes},
    {"op3B 09ABB7B8", f_game_task_09ABB7B8, 1, 0, 0x09ABB7B8, NULL, inputs_op_3B, &script_pokes},
    {"op63 09AC2B78", f_game_task_09AC2B78, 1, 0, 0x09AC2B78, NULL, inputs_op_63, &script_pokes},
    {"op51 09AC1578", f_game_task_09AC1578, 1, 0, 0x09AC1578, NULL, inputs_op_51, &script_pokes},
    {"op38 09AC03D8", f_game_task_09AC03D8, 1, 0, 0x09AC03D8, NULL, inputs_op_38, &script_pokes},
    {"op21 09ABF3B0", f_game_task_09ABF3B0, 1, 0, 0x09ABF3B0, NULL, inputs_op_21, &script_pokes},
    {"op17 09ABD7C0", f_game_task_09ABD7C0, 1, 0, 0x09ABD7C0, NULL, inputs_op_17, &script_pokes},
    {"op18 09ABD818", f_game_task_09ABD818, 1, 0, 0x09ABD818, NULL, inputs_op_18, &script_pokes},
    {"script_next 09AB7858", f_game_task_09AB7858, 1, 0, 0x09AB7858, NULL, inputs_script, &script_pokes},
    {"script_end_block 09AB6788", f_game_task_09AB6788, 1, 0, 0x09AB6788, NULL, inputs_script_end_block, &script_pokes},
    {"script_run 09AB4C68", f_game_task_09AB4C68, 0, 0, 0x09AB4C68, NULL, inputs_script, &script_pokes},
    {"script_pending 09AB7230", f_game_task_09AB7230, 1, 0, 0x09AB7230, NULL, inputs_script, &script_pokes},
    {"script_get_position 09AB7490", f_game_task_09AB7490, 1, 0, 0x09AB7490, NULL, inputs_script, &script_pokes},
    {"script_set_position 09AB74D0", f_game_task_09AB74D0, 0, 0, 0x09AB74D0, NULL, inputs_script, &script_pokes},
    {"script_op07 09AB7510", f_game_task_09AB7510, 1, 0, 0x09AB7510, NULL, inputs_script, &script_pokes},
    {"script_main_entry 09ABAB90", f_game_task_09ABAB90, 1, 0, 0x09ABAB90, NULL, inputs_script, &script_pokes},
    {"script_restart_flagged 09AC8B78", f_game_task_09AC8B78, 0, 0, 0x09AC8B78, NULL, inputs_script, &script_pokes},
    {"script_restart_from 09ABAFF0", f_game_task_09ABAFF0, 0, 0, 0x09ABAFF0, NULL, inputs_script, &script_pokes},
    {"script_sync 09AC9BC8", f_game_task_09AC9BC8, 0, 0, 0x09AC9BC8, NULL, inputs_script, &script_pokes},
    {"script_skip_to 09AB7728", f_game_task_09AB7728, 1, 0, 0x09AB7728, NULL, inputs_script, &script_pokes},
    {"script_past_end 09AB8A38", f_game_task_09AB8A38, 1, 0, 0x09AB8A38, NULL, inputs_script, &script_pokes},
    {"script_op0b 09AB7620", f_game_task_09AB7620, 1, 0, 0x09AB7620, NULL, inputs_script, &script_pokes},
    {"script_op39 09AB7520", f_game_task_09AB7520, 1, 0, 0x09AB7520, NULL, inputs_script, &script_pokes},
    {"script_op01 09ABA218", f_game_task_09ABA218, 1, 0, 0x09ABA218, NULL, inputs_script, &script_pokes},
    {"script_op80 09AB8AE0", f_game_task_09AB8AE0, 1, 0, 0x09AB8AE0, NULL, inputs_script, &script_pokes},
    {"object_apply_hit_mask_28_amounts 09C45008", f_game_sub_09C45008, 0, 0, 0x09C45008, NULL, inputs_mask_amounts, NULL},
    {"hunter_register_hit 09C43000", f_game_sub_09C43000, 0, 0, 0x09C43000, NULL, inputs_register_hit, &register_hit_pokes},
    {"entity_sum_pending_damage 09AC8450", f_game_task_09AC8450, 0, 0, 0x09AC8450, NULL, inputs_sum_damage, &sum_damage_pokes},
    {"entity_apply_pending_damage 09AC7A08", f_game_task_09AC7A08, 1, 0, 0x09AC7A08, NULL, inputs_apply_damage, &apply_damage_pokes},
    {"hit_node_test_if_in_map 09C426E0", f_game_sub_09C426E0, 0, 0, 0x09C426E0, NULL, inputs_test_if_in_map, &test_if_in_map_pokes},
    {"hit_node_test_target 09C42750", f_game_sub_09C42750, 0, 0, 0x09C42750, NULL, inputs_test_target, &test_target_pokes},
    {"hit_manager 09C41F58", f_game_sub_09C41F58, 0, 0, 0x09C41F58, NULL, inputs_hit_manager, &hit_manager_pokes},
};

/* -- the stubs: in a trace build, rt_trace_all is at the top of every generated function --------------------------------------- */
#define MAX_CALLS 128
/* a0-a3 t0-t3, f12-f19: only those the callee reads, others 0; stack0 below; a hash of the hunter as the callee finds it, so a write on
 * the wrong side of a call shows even if the stub does nothing with it */
typedef struct { uint32_t fn, a[8], f[8], stack0, hunter_hash; } Call;
#define HUNTER_HASHED 0x1200u
#define NINE_ARGUMENTS 0x08883988u      /* callees with an argument at their sp+0 */
#define OBJECT_SOUND 0x08865F8Cu
#define STUB_RECORD 0x09FF8000u         /* what a call returning a pointer to a record gets */
#define RECORD_TYPE 0x3C
#define REGEN_EFFECT_TEST 0x088DAE98u
#define SKILL_TEST 0x088D6A1Cu
static int stubbing;            /* within the function under test */
static int running;             /* it is running, stubs or not */
static uint32_t target;         /* it: not a stub */
static uint32_t results[MAX_CALLS], fresults[MAX_CALLS];
static int16_t stub_stamina[MAX_CALLS];     /* what a call leaves stamina at (0x7FFF: it leaves it alone): callees do change it */
static uint8_t stub_slot[MAX_CALLS];        /* the same for the local player's slot (0xFF: alone) */
static uint8_t stub_record_type[MAX_CALLS];
static uint32_t record_method;              /* the method returning that pointer: *(*(*0x09A4F1A0)) slot 0x28 */
typedef struct { uint32_t offset; uint8_t size; uint8_t in_game; } PokeField;
/* What a callee answers more often than the random results would: a function's deep paths may need several answers at once. Ends with fn 0. */
typedef struct { uint32_t fn; int count; int32_t values[16]; } Bias;
struct Pokes { const PokeField *fields; size_t count; const Bias *bias; };
static const PokeField *pokable;   /* a callee may set one of these */
static const Bias *stub_bias;
static uint32_t bias_pick[MAX_CALLS];
#define MAX_POKES 16
static int npokes;                          /* how many at each call, this time */
static uint16_t stub_poke[MAX_CALLS][MAX_POKES];   /* indexes in them */
static int32_t stub_poke_value[MAX_CALLS][MAX_POKES];
static uint32_t stub_hunter;
static uint32_t test_a0;            /* what the function under test gets as a0: the hunter, if its inputs don't say */
static struct { uint32_t at, size; } hashed_more[2];     /* what else is hashed at each call: other objects a function writes */
static uint32_t poke_base[4];       /* what a PokeField's in_game is relative to, from 2 on */
static void (*stub_hook)(Ctx *c, uint32_t fn, int n);    /* what callees write through pointers they are given */
static int stack_pattern;           /* a stack of words all different, so that reading the wrong one shows */
static int compare_vfpu;            /* a function that leaves something in the vector registers */
static int compare_f0;              /* a function returning a float */
static uint32_t test_f12;           /* f12 to give it, as bits */
static uint32_t test_t0, test_t1;   /* t0 and t1 to give it, for those of six arguments */
static Call calls[2][MAX_CALLS];
static int ncalls[2], current;

/* What each function reads, from tools/readable/signatures.py's table: registers it doesn't read aren't arguments, and what they
 * happen to hold differs between versions without mattering. A bit each: a0-a3 t0-t3, then f12-f19. */
static uint16_t *reads;
#define READS(fn) reads[((fn) - 0x08800000u) >> 2]

static void load_signatures(const char *path) {
    reads = malloc(0x01800000u);
    memset(reads, 0xFF, 0x01800000u);       /* not in the table: every register compared */
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(2);
    }
    char line[512];
    static const char *names[16] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19"};
    while (fgets(line, sizeof line, f)) {
        char *module = strtok(line, "\t"), *addr = strtok(NULL, "\t"), *ret = strtok(NULL, "\t"), *args = strtok(NULL, "\t\n");
        (void)module;
        (void)ret;
        uint32_t a = addr ? (uint32_t)strtoul(addr, NULL, 16) : 0;
        if (a < 0x08800000u || a >= 0x0A000000u)
            continue;
        uint16_t mask = 0;
        for (char *r = args ? strtok(args, ",") : NULL; r; r = strtok(NULL, ","))
            for (int i = 0; i < 16; i++)
                if (!strcmp(r, names[i]))
                    mask |= (uint16_t)(1u << i);
        /* an address several overlays share: anything any of them reads (first seen replaces "not in the table") */
        READS(a) = READS(a) == 0xFFFF ? mask : READS(a) | mask;
    }
    fclose(f);
}

/* Callees that reach the kernel, which isn't here, even with no stubs asked for: the sound. Stubbed in any case, their arguments still
 * compared. A bit each for what they read, as in `reads`. In a build without RECOMP_TRACE_ALL a function calling them can't run alone. */
static const struct { uint32_t fn; uint16_t mask; } kernel_bound[] = {
    {0x08883988u, 0x00FF}, {0x088862A8u, 0x0003}, {0x08885E3Cu, 0x0003},
    {0x088A5B50u, 0x0005},      /* the sound's queue: a1 points at the caller's stack, which differs */
    {0x088B87B0u, 0x0003},      /* the file loader's queue */
};

static int kernel_bound_mask(uint32_t fn) {
    for (size_t i = 0; i < sizeof kernel_bound / sizeof kernel_bound[0]; i++)
        if (kernel_bound[i].fn == fn)
            return kernel_bound[i].mask;
    return -1;
}

int rt_trace_all(Ctx *c, int module, uint32_t fn) {
    (void)module;
    int bound = running && !stubbing ? kernel_bound_mask(fn) : -1;
    if (bound < 0 && (!stubbing || fn == target))
        return 0;
    int n = ncalls[current]++;
    if (n < MAX_CALLS) {
        Call *call = &calls[current][n];
        memset(call, 0, sizeof *call);
        call->fn = fn;
        uint16_t mask = bound >= 0 ? (uint16_t)bound : READS(fn);
        for (int i = 0; i < 8; i++) {
            if (mask >> i & 1)
                call->a[i] = c->r[4 + i];
            if (mask >> (8 + i) & 1)
                call->f[i] = c->f[12 + i].u;
        }
        if (fn == NINE_ARGUMENTS || fn == OBJECT_SOUND)
            call->stack0 = LW(c->r[29]);
        if (fn == 0x09C572D0u)      /* the place of an impact, on the stack */
            for (uint32_t o = 0; o < 12; o += 4) {
                uint32_t p = LW(c->r[6] + o);
                p = (p & 0x7FFFFFFFu) > 0x7F800000u ? 0x7FC00000u : p;
                call->stack0 = (call->stack0 ^ p) * 16777619u;
            }
        /* The angle of a hit, from two places it is pointed at. One NaN stands for all: added to another NaN, which comes out is the host compiler's
         * choice of operand order, not the game's (seen: place 0xFFFFFFFF + 0x7FF3EBF0 came out each way, generated and hand-written). */
        if (fn == 0x09C42DA0u)
            for (uint32_t o = 0; o < 16; o += 4) {
                uint32_t p = LW(c->r[6] + o), q = LW(c->r[7] + o);
                p = (p & 0x7FFFFFFFu) > 0x7F800000u ? 0x7FC00000u : p;
                q = (q & 0x7FFFFFFFu) > 0x7F800000u ? 0x7FC00000u : q;
                call->stack0 = (call->stack0 ^ p ^ q * 3u) * 16777619u;
            }
        uint32_t hash = 2166136261u;
        for (uint32_t o = 0; o < HUNTER_HASHED; o += 4)
            hash = (hash ^ LW(stub_hunter + o)) * 16777619u;
        for (int more = 0; more < 2; more++)
            for (uint32_t o = 0; o < hashed_more[more].size; o += 4)
                hash = (hash ^ LW(hashed_more[more].at + o)) * 16777619u;
        call->hunter_hash = hash;
        if (bound >= 0) {
            c->r[2] = 0;
            return 1;
        }
        c->r[2] = results[n];
        c->f[0].u = fresults[n];
        for (const Bias *b = stub_bias; b && b->fn; b++)
            if (b->fn == fn)
                c->r[2] = (uint32_t)b->values[bias_pick[n] % (uint32_t)b->count];
        if (fn == record_method) {
            c->r[2] = STUB_RECORD;
            SB(STUB_RECORD + RECORD_TYPE, stub_record_type[n]);
        }
        for (int i = 0; pokable && i < npokes; i++) {
            uint8_t where = pokable[stub_poke[n][i]].in_game;
            uint32_t at = (where >= 2 ? poke_base[where] : where ? LW(GAME_POINTER) : stub_hunter) + pokable[stub_poke[n][i]].offset;
            int32_t value = stub_poke_value[n][i];
            if (pokable[stub_poke[n][i]].size == 1)
                SB(at, value);
            else if (pokable[stub_poke[n][i]].size == 2)
                SH(at, value);
            else
                SW(at, value);
        }
        /* two tests deep in regeneration, otherwise hardly ever passed */
        if (pokable && ((fn == REGEN_EFFECT_TEST && c->r[5] == 0x20) || (fn == SKILL_TEST && c->r[5] == 0x7A)) && (stub_record_type[n] & 3))
            c->r[2] = 1;
        if (stub_hook)
            stub_hook(c, fn, n);
        if (stub_stamina[n] != 0x7FFF)
            SH(stub_hunter + 0x55A, stub_stamina[n]);
        if (stub_slot[n] != 0xFF)
            SB(LW(0x089CC558u) + 0x28, stub_slot[n]);
    } else if (stub_hook) {     /* a long loop goes on being answered */
        stub_hook(c, fn, n % MAX_CALLS);
    }
    return 1;
}

static uint32_t seed = 12345;
static uint32_t rnd(void) {      /* xorshift: the same on every run */
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static uint32_t pick(const int32_t *from, int n) { return (uint32_t)from[rnd() % (uint32_t)n]; }
#define PICK(...) pick((const int32_t[]){__VA_ARGS__}, sizeof((const int32_t[]){__VA_ARGS__}) / sizeof(int32_t))

/* What the functions under test read, set to something their branches can tell apart */
static void randomise(Ctx *c, uint32_t hunter, uint32_t *a1, int *tick30) {
    HunterBase *h = (HunterBase *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    *a1 = PICK(-1, -1, -2, -3, -4, -5, -7, -10, -300, -301, -32768, 0, 1, 2, 4, 300, 32767, 0x12345, -0x12345);
    *tick30 = rnd() % 4 != 0;
    h->stamina = (int16_t)PICK(0, 1, 2, 5, 75, 76, 77, 150, 150, 299, 300, 300, 301, 32767, -1, -32768);
    h->stamina_max = (int16_t)PICK(0, 50, 150, 300, 32767, -1);
    SH(hunter + 0x1E6, rnd() % 3 == 0 ? rnd() % 4 : g->local_hunter_slot);
    g->tick_30ths = (uint16_t)(rnd() % 8 == 0 ? rnd() : rnd() % 64);
    SH(hunter + 0x622, PICK(0, 0, 0, 1, 5, -1));                /* (stamina loss blocked if positive) */
    SH(hunter + 0x6A0 + 5 * 2, PICK(0, 0, 1, -1));              /* (the other way it is blocked) */
    for (int i = 0; i < 6; i++)                                 /* the active skills */
        SB(hunter + 0x676 + i, PICK(0, 0, 0, 0xA8, 0xA9, 0x10));
    /* the state functions' */
    Hunter *self = (Hunter *)MEM(hunter);
    self->state_step = (uint8_t)PICK(0, 1, 1, 1, 1, 2, 2, 3, 3, 4);
    self->action_count = (int32_t)PICK(150, 2, 1, 0, -1);
    self->misc_flags = PICK(0, 8, 0xFF);
    self->motion_part0_flags = (uint16_t)PICK(0, 1, 2, 3);
    SW(LW(0x08A632C0u) + 0xF7A57Cu, PICK(0, 0, 1, 2));      /* (read by the sprints' input, when the slot has changed) */
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 30);
    self->motion_id = (uint16_t)PICK(3, 3, 0, 0x26);
    self->buttons_pressed = PICK(0, 0, 0x4000, 0x4000, 0x4010, 1, 2);
    self->stick_magnitude = (uint16_t)PICK(0, 39, 40, 127);
    self->quest_flags = PICK(0, 0, 0x100, 0x100);
    self->linked_monster_angle = rnd() & 0xFFFF;
    self->facing = rnd() & 0xFFFF;
    if (rnd() % 3 == 0)         /* just either side of a quarter turn */
        self->linked_monster_angle = (self->facing + PICK(0x2000, 0x2001, 0x1FFF, -0x6000, -0x5FFF, -0x6001)) & 0xFFFF;
    self->ground_forces_state_3b = (uint8_t)PICK(0, 0, 0, 1);
}

/* -- the hunter's upkeep, game_task 09A68FC8 ------------------------------------------------------------------------------------ */
#define T16 PICK(0, 0, 0, 1, 1, 2, 3, -1, 300, 0x7FFF, -0x8000)     /* a 16-bit timer: either sign counts */
#define T8 PICK(0, 0, 1, 1, 2, 0xFF)

/* What the upkeep reads after a call, so a callee may have changed it. All of them, for the versions to be held to reading afresh. */
static const PokeField upkeep_fields[] = {
    {0x1C, 2, 1}, {0x28, 1, 1},
    {0x1E1, 1, 0}, {0x1E6, 2, 0}, {0x1E8, 1, 0}, {0x298, 1, 0}, {0x299, 1, 0}, {0x2BA, 2, 0}, {0x2E4, 2, 0}, {0x2E6, 1, 0}, {0x35E, 2, 0},
    {0x388, 2, 0}, {0x3D6, 2, 0}, {0x3DA, 2, 0}, {0x410, 4, 0}, {0x41C, 2, 0}, {0x41E, 2, 0}, {0x424, 2, 0}, {0x440, 2, 0}, {0x442, 2, 0},
    {0x468, 2, 0}, {0x552, 1, 0}, {0x55A, 2, 0}, {0x564, 1, 0}, {0x5FE, 1, 0}, {0x610, 2, 0}, {0x620, 2, 0}, {0x622, 2, 0}, {0x624, 2, 0},
    {0x626, 2, 0}, {0x628, 2, 0}, {0x62A, 2, 0}, {0x640, 1, 0}, {0x641, 1, 0}, {0x672, 2, 0}, {0x6A0, 2, 0}, {0x6A6, 2, 0}, {0x6AA, 2, 0},
    {0x6AE, 2, 0}, {0x6B2, 2, 0}, {0x6B4, 2, 0}, {0x6C8, 2, 0}, {0x10F4, 2, 0}, {0x10F6, 2, 0}, {0x10F8, 2, 0}, {0x10FA, 2, 0}, {0x110E, 2, 0},
    {0x1112, 2, 0}, {0x111C, 4, 0}, {0x1138, 2, 0}, {0x1140, 2, 0}, {0x1142, 2, 0}, {0x1146, 2, 0}, {0x11B4, 2, 0},
};

const struct Pokes upkeep_pokes = {upkeep_fields, sizeof upkeep_fields / sizeof upkeep_fields[0], NULL};

static void randomise_upkeep(Ctx *c, uint32_t hunter) {
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    if (rnd() % 8 == 0)         /* not the local player, in the part of the word only the whole comparison sees */
        self->instance_index = (uint16_t)(g->local_hunter_slot + 0x100);
    /* a count on the boundaries of what divides it, then as it would be if read signed */
    g->tick_30ths = (uint16_t)(rnd() % 4 == 0 ? rnd() : PICK(0, 1, 2, 3, 10, 11, 15, 30, 60, 120, 240, 480, 241, 0xFFF0, 0xFFFA, 0xFFFF, 0x8000));
    self->reaction_block_weapon_ticks = (int16_t)T16;
    self->push_off_ticks = (uint16_t)T16;
    self->hit_immune_ticks = (uint16_t)T16;
    self->reaction_block_item_ticks = (int16_t)T16;
    self->menu_camera_lock_ticks = (uint8_t)T8;
    self->repeat_use_ticks = (int16_t)T16;
    self->stamina_loss_block_ticks = (int16_t)T16;
    self->area_1_2_ward_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 3, 0x8000, 0xFFFF);
    self->area_3_4_ward_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 3, 0x8000, 0xFFFF);
    self->health_regen_boost_ticks = (uint16_t)PICK(0, 0, 0, 1, 1, 2, 0xFFFF);
    self->map_detect_item_ticks = (uint16_t)T16;
    self->map_detect_ticks = (uint16_t)T16;
    self->map_detect_skill_ticks = (uint16_t)T16;
    self->map_detect_skill_roll_ticks = (uint16_t)PICK(0, 0, 1, 2, 9000);
    self->attack_bonus_ticks = (uint16_t)PICK(0, 0, 1, 1, 2, 0xFFFF);
    self->attack_bonus_timed = (int8_t)PICK(0, 5, -1);
    self->defense_bonus_ticks = (uint16_t)PICK(0, 0, 1, 1, 2, 0xFFFF);
    self->defense_bonus_timed = (int8_t)PICK(0, 5, -1);
    self->mash_status_left = (int16_t)T16;
    self->weapon_mode_ticks = (int16_t)PICK(0, 0, 0, 0, 1, 2, -1, 300);
    self->item_restrict_ticks = (int16_t)T16;
    self->defense_halved_ticks = (int16_t)T16;
    self->stamina_debuff_ticks = (int16_t)T16;
    self->weapon_class = (uint8_t)PICK(2, 2, 3, 3, 7, 7, 9, 9, 10, 10, 0, 1, 0x82);
    self->weapon7_gauge = (int16_t)PICK(0, 1, 100, -1);
    self->charge_time = (int16_t)PICK(0, 0, 1, 1, 2, -1, 300);
    self->quest_flags = PICK(0, 0x10, -1, ~0x10);
    self->misc_flags = PICK(0, 0, 0, 0, 0, 0x10, 0x10, ~0x10, 0xFF, -1);
    self->state_group = (uint8_t)PICK(0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 3, 5, 5, 4, 6, 0xFF);
    /* the states each group and weapon class lists, and their neighbours */
    if (rnd() % 4 == 0)
        self->state = (uint8_t)rnd();
    else if (self->state_group == 0)
        self->state = (uint8_t)PICK(0x25, 0x7E, 0x6E, 0x6D, 0x6C, 0x6B, 0x44, 0x32, 0x31, 0x2D, 0x1C, 0x78, 0x77, 0x76, 0x6A, 0x47, 0x4A, 0x5B);
    else if (self->state_group == 1 && self->weapon_class == 10)
        self->state = (uint8_t)PICK(0x12, 0x0A, 0x08, 0x09, 0x11);
    else if (self->state_group == 1 && self->weapon_class == 3)
        self->state = (uint8_t)PICK(0x10, 0x05, 0x0E, 0x03, 0x04);
    else if (self->state_group == 1 && self->weapon_class == 2)
        self->state = (uint8_t)PICK(0x04, 0x0E, 0x0D, 0x0C, 0x03, 0x05);
    else
        self->state = (uint8_t)PICK(0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x0A, 0x0B, 0x0C, 0x0D);
    self->buildup_decay_delay_ticks = (int16_t)PICK(0, 1, 2, -1, -0x8000);
    self->buildup_state_2_13 = (int16_t)PICK(0, 1, 50, -1);
    self->flag_1000_lockout_ticks = (uint8_t)T8;
    self->weapon_selector_lockout_ticks = (uint8_t)T8;
    self->item_use_lockout_ticks = (uint8_t)T8;
    self->listed_item_ticks = (uint16_t)PICK(0, 0, 1, 2, 0xFFFF, 0x8000);
    self->ticks_2e6 = (uint8_t)T8;
    /* regeneration */
    self->ground_effect_kind = (uint8_t)PICK(0, 0, 0, 0, 1, 2, 3);
    self->status_bits = (uint16_t)PICK(0, 0, 0, 0x80, 0x7F, 0xFF80);
    self->drain_status_seconds = (int16_t)PICK(0, 0, 0, 0, 0, 1, -1);
    self->health_regen_effect_level = (uint8_t)PICK(0, 1, 2, 3, 4, 0xFF);
    int16_t health = (int16_t)PICK(-1, 0, 1, 2, 50, 99, 100, 150, 0x7FFF, -0x8000);
    self->health = health;
    self->health_red_limit = (int16_t)(rnd() % 4 == 0 ? (int32_t)PICK(0, 100, 150, -1, 0x7FFF, -0x8000) : health + (int32_t)PICK(0, 1, 2, 3, 4, 5, -1));
    self->health_max = (int16_t)PICK(100, 150, -1);
    self->health_max_base = (int16_t)(rnd() % 2 ? (int32_t)PICK(0, 100, 150, -1) : (rnd() % 2 ? health : self->health_red_limit) + (int32_t)PICK(0, 1, -1));
    for (int i = 0; i < 6; i++)     /* for the real callee */
        SB(hunter + 0x676 + i, PICK(0, 0, 0x95, 0x7B, 0x7A, 0x0B, 0xD0, 0xD6, 0xB2, 0xD4, 0x18, 0x17, 0x19, 0x1A));
    /* the timed effects */
    for (int i = 0; i < 21; i++) {
        SH(hunter + 0x6A0 + i * 2, PICK(0, 0, 1, 1, 2, -1, 300));
        SB(hunter + 0x5F7 + i, PICK(0, 1, 3));
        SB(hunter + 0x114C + i, PICK(0, 1, 3));
    }
    if (!record_method) {           /* not stubs: what the method really returns, its type our choice */
        uint32_t holder = LW(LW(0x09A4F1A0u));
        c->r[4] = holder;
        c->r[29] = TEST_SP;
        rt_call(c, LW(LW(holder) + 0x28));
        SB(c->r[2] + RECORD_TYPE, PICK(0, 1, 2, 3, 4));
    }
}

/* -- the small functions' inputs ------------------------------------------------------------------------------------------------ */

/* The local player's slot most of the time, or another, or one only the whole 16 bits tell from it */
static uint16_t pick_slot(Ctx *c) {
    uint8_t local = ((GameState *)MEM(LW(GAME_POINTER)))->local_hunter_slot;
    switch (rnd() % 4) {
    case 0: return (uint16_t)(rnd() % 4);
    case 1: return (uint16_t)(local + 0x100);
    default: return local;
    }
}

/* A byte list, and just outside it, with everything but the id; then the id in it, or just outside, or nowhere */
static void plant_id(uint8_t *list, int n, uint8_t id) {
    for (int i = -1; i <= n; i++)
        list[i] = (uint8_t)(id + 1 + rnd() % 200);
    int at = (int)(rnd() % (uint32_t)(2 * n)) - 1;
    if (at <= n)
        list[at] = id;
}

/* an id in the low byte: what's above it isn't to count */
static uint32_t with_high_bits(uint32_t low) { return low | PICK(0, 0, 0x100, 0xFFFFFF00, 0x7F00); }

static void inputs_loss_blocked(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    ((Hunter *)MEM(hunter))->instance_index = pick_slot(c);
}

static void inputs_active_skill(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    uint8_t id = (uint8_t)(rnd() % 4 ? rnd() : 0);
    plant_id(MEM(hunter + 0x676), 6, id);
    args[0] = with_high_bits(id);
}

static void inputs_hunt_bonus(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    uint8_t id = (uint8_t)(rnd() % 4 ? rnd() : 0);
    uint32_t game = LW(GAME_POINTER);
    uint16_t slot = pick_slot(c);
    ((Hunter *)MEM(hunter))->instance_index = slot;
    plant_id(MEM(game + 0x69942), 5, id);
    plant_id(MEM(game + 0xA9 + slot * 0x8Cu), 5, id);
    args[0] = with_high_bits(id);
}

static void inputs_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = pick_slot(c);
    self->reset_place_mode = (uint8_t)PICK(0, 0, 1, 2, 3, 0x80);
    self->misc_flags = rnd();
    args[0] = PICK(0, 0, 1, 2, 4, 4, 5, 0x104, 0x8004, 0x10004, 0xFFFF0004);
    args[1] = rnd() % 0x40;
    args[2] = rnd() % 8;
}

static void inputs_face_stick(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->stick_angle = (uint16_t)rnd();
    self->facing_target = (uint16_t)rnd();
    uint32_t camera = LW(0x089CBD04u);      /* for the real callee */
    SB(camera + 0xDC4, PICK(0, 0, -1));
    SB(camera + 0xA8D, PICK(0, 0, 1));
    SB(camera + 0x1DE, PICK(0, 1, 1));
}

static void inputs_set_flags(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->state_flags = rnd();
    self->state_flags_high = rnd();
    args[0] = PICK(0, 1, 0x80000000, 0x80000001, 0x7FFFFFFF, -1);
    if (rnd() % 2)
        args[0] = rnd();
}

static void inputs_state_is(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->state_group = (uint8_t)(rnd() % 4);
    self->state = (uint8_t)(rnd() % 4);
    args[0] = with_high_bits(rnd() % 2 ? self->state_group : rnd() % 4);
    args[1] = with_high_bits(rnd() % 2 ? self->state : rnd() % 4);
}

static void inputs_movement(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Object *self = (Object *)MEM(hunter);
    self->movement_mode = (uint8_t)PICK(0, 1, 2, 3, 0xFF);
    self->state_flags = rnd();
    self->state_flags_high = rnd();
    args[0] = PICK(0, 1, 2, 3, 4, 0x8000, 0x8001, 0x8002, 0x8003, 0x101, 0x102, 0x7F02, 0xFF01, 0x10000, 0x10002, 0x18000, 0xFF);
    args[1] = PICK(0, 0, 1, -1, 0x8000, 0x10000, 0xFFFF0000);
    args[2] = PICK(0, 0, 1, -1, 0x8000, 0x10000, 0xFFFF0000);
}

static void inputs_classify(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->weapon_class = (uint8_t)(rnd() % 3 == 0 ? PICK(1, 5, 10) : rnd() % 3 == 0 ? rnd() : rnd() % 12);
    self->misc_flags = PICK(0, 0, 8, ~8, -1);
    self->hunter_flags = PICK(0, 0x1000, ~0x1000, -1);
    self->stick_magnitude = (uint16_t)PICK(0, 39, 40, 41, 84, 85, 86, 119, 120, 121, 127, 0x7FFF, 0x8000, 0xFFFF);
    self->movement_mode = (uint8_t)PICK(0, 0, 0, 1, 2, 0x80);
    self->buttons_held = PICK(0, 0x200, 0x200, ~0x200, -1);
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 30, -1);
    self->state_group = (uint8_t)PICK(0, 0, 1);         /* for the real callee */
    self->state = (uint8_t)PICK(0x24, 0x24, 0x25, 0);
}

static void inputs_countdowns(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->refusal_message_cooldown = (uint8_t)PICK(0, 0, 1, 2, 0x80, 0xFF);
    self->sound_cue_mute_timer = (uint8_t)PICK(0, 0, 1, 2, 0x80, 0xFF);
    self->state_hold_timer = (int16_t)PICK(0, 0, 1, 2, 30, -1, -0x8000, 0x7FFF, 0x100);
    self->ticks_5f0 = (int8_t)PICK(0, 0, 1, 2, 5, 50, -1, -0x80, 0x7F);
}

/* -- the hunter's states, its tick and its input -------------------------------------------------------------------------------- */
#define POKES(name, bias, ...) static const PokeField name##_fields[] = {__VA_ARGS__}; \
    const struct Pokes name = {name##_fields, sizeof name##_fields / sizeof name##_fields[0], bias}
static const Bias tick_bias[] = {
    {0x088DE7C8u, 4, {1, 1, 1, 0}},         /* the assets wanted */
    {0x0884FA5Cu, 4, {0, 0, 0, 1}},
    {0x088C7424u, 5, {0, 1, 2, 0xE94F, 0xE950}},    /* the area's asset: added to the base, also "none" and 0 */
    {0x088564F8u, 5, {0, 1, 2, 0xE94F, 0xE950}},
    {0, 0, {0}},
};
static const Bias common_bias[] = {
    {0x09AA5FB0u, 8, {0, 1, 2, 3, 3, 5, 5, 0x105}},     /* the move input */
    {0x08865C88u, 8, {0, 0, 0x200, 0x1000, 0x400, 0x2000000, 0x600, 0x40}},     /* the state flags asked about */
    {0x0884FA5Cu, 8, {0, 0, 0, 0, 0, 0, 1, 0x101}},     /* each answer of 1 ends it */
    {0x09AA8FC8u, 4, {0, 1, 1, 0x101}},
    {0x088D6A54u, 4, {0, 0, 1, 0x100}},
    {0, 0, {0}},
};
POKES(set_state_pokes, NULL, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x298, 1, 0}, {0x322, 2, 0}, {0x410, 4, 0}, {0x614, 4, 0}, {0x288, 4, 0}, {0x28C, 4, 0});
POKES(reset_pokes, NULL, {0x29C, 1, 0}, {0x440, 2, 0}, {0x614, 4, 0}, {0x280, 1, 0});
POKES(tick_pokes, tick_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x4, 4, 0}, {0x298, 1, 0}, {0x29A, 2, 0}, {0x2E4, 2, 0}, {0x6A0, 2, 0}, {0x111C, 4, 0});
POKES(copy_pad_pokes, NULL, {0x614, 4, 0}, {0x480, 4, 0}, {0x484, 4, 0}, {0x488, 4, 0}, {0x61C, 4, 0});
POKES(common_pokes, common_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x2E, 1, 1}, {0x299, 1, 0}, {0x29C, 1, 0}, {0x1E0, 1, 0}, {0x1E8, 1, 0}, {0x2C0, 4, 0}, {0x1F4, 4, 0},
      {0x320, 2, 0}, {0x410, 4, 0}, {0x440, 2, 0}, {0x480, 4, 0}, {0x484, 4, 0}, {0x49E, 2, 0}, {0x55A, 2, 0}, {0x565, 1, 0}, {0x614, 4, 0},
      {0x641, 1, 0}, {0x674, 1, 0}, {0x111C, 4, 0}, {0x1120, 4, 0});

static void inputs_overlays(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    for (uint32_t o = 0x1A8; o < 0x1C0; o++)    /* and those it leaves alone */
        SB(hunter + o, rnd());
}

static void inputs_airborne(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)hunter;
    args[0] = PICK(0, 1, 2, 3, 4, 7, -1, -2, -3, 0x7FFF, 0x8000, 0x10001, 0xFFFF0002);
    (void)c;
}

static void inputs_transient(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->weapon_class = (uint8_t)PICK(0, 8, 9, 9, 10, 0x89);
    self->charge_time = (int16_t)rnd();
    self->hunter_flags = rnd();
    self->state_flags = rnd();
    self->misc_flags = rnd();
    for (uint32_t o = 0x282; o < 0x284; o++)
        SB(hunter + o, rnd());
    self->listed_item_ticks = (uint16_t)rnd();
    self->life_state = (uint8_t)rnd();
    self->flagged_ticks = (uint16_t)rnd();
    self->hit_immune_ticks = (uint16_t)rnd();
    self->transient_45e = (uint16_t)rnd();
    self->flag288_phase1_ticks = (uint8_t)rnd();
    self->flag288_phase2_ticks = (uint8_t)rnd();
    self->action_count = (int32_t)rnd();
}

static void inputs_real_set_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = pick_slot(c);
    self->reset_place_mode = (uint8_t)PICK(0, 0, 0, 1, 3);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3);
    self->state = (uint8_t)rnd();
    self->state_random = (uint16_t)rnd();
    self->misc_flags = rnd();
    self->hunter_flags = rnd();
    self->state_flags = rnd();
    self->signed_overlay_blend = (int8_t)rnd();
    args[0] = PICK(0, 0, 0, 1, 2, 2, 4, 0x104, 0x10004, 0x8002);
    args[1] = rnd() % 4 ? rnd() % 0x80 : rnd();
    args[2] = PICK(0, 1, 2, 3, 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x33, 0x10000, 0x10021, 0xFFFF0010, 4, 0x40);
}

static void inputs_reset(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 1, 0x80);
    self->buildup_state_2_13 = (int16_t)PICK(0, 49, 50, 51, -1, 0x7FFF);
    self->hunter_flags = rnd();
    self->movement_mode = (uint8_t)PICK(0, 2, 0xF);
    args[0] = PICK(0, 1, 2, 2, 3, 0x80, 0x82, 0x81, 0x102, 0x10002, 0xFFFFFF02);
    args[1] = PICK(0, 4, 5, 8, 0x8000, 0x10003, -3);
    args[2] = PICK(0, 1, 4, 0x10000);
    (void)c;
}

static void inputs_run_state(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6, 7, 8, 0x80, 0xFF);
    self->hunter_flags = rnd();
    self->misc_flags = rnd();
    (void)c;
}

static void inputs_group0(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    ((Hunter *)MEM(hunter))->state = (uint8_t)(rnd() % 8 ? rnd() % 0x80 : rnd());
    (void)c;
}

static void inputs_tick(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = pick_slot(c);
    self->area_id = (uint16_t)(rnd() % 8 ? g->stage_id : g->stage_id ^ PICK(1, 0x100, 0x8000));
    self->flags = rnd();
    self->timed_effect_ticks = (int16_t)PICK(0, 1, 2, -1, 300);
    self->quest_flags = PICK(0, 2, ~2, -1);
    self->health = (int16_t)PICK(-1, 0, 1, 100);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3, 3);
    self->reset_place_mode = (uint8_t)rnd();
    uint32_t stage = LW(0x09A44D9Cu), assets = LW(0x09A4F210u);
    SB(stage + 0x5C, PICK(0, 1, 1, 0x80));
    SW(stage + 0x54, rnd());
    SH(assets + 0xFC, PICK(0, 1, 2, 3, 0xFFFC, 0xFFFE));
    SH(assets + 0xFE, PICK(0x16B0, 0x16B1, 0x16B2, 0xFFFF, 0));
    for (uint32_t i = 1; i < 23; i++)   /* one past the last, too */
        SH(assets + 0xFC + i * 12, rnd());
    SW(assets + 0x204, rnd());
    SW(assets + 0x208, rnd());
}

static void inputs_set_motion(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    ((Hunter *)MEM(hunter))->hunter_flags = rnd();
    args[0] = rnd() % 2 ? rnd() % 0x400 : rnd();
    args[1] = PICK(0, 1, 2, 3, 30, -1, -2, -3, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x20001, 0x18000);
    args[2] = PICK(0, 1, 30, -1, 0x10002);
    (void)c;
}

static void inputs_motion_copy(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->motion_id = (uint16_t)rnd();
    self->motion_id_copy = rnd() % 2 ? self->motion_id : (uint16_t)rnd();
    self->effect_cooldown = (uint16_t)PICK(0, 0, 1, 2, 0x8000, 0xFFFF);
    (void)c;
}

static void inputs_copy_pad(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    SW(0x08A62D88u, rnd());
    SW(0x08A62D8Cu, rnd());
    SW(0x08A62D90u, rnd());
    SW(0x08A62D94u, rnd());
    SH(0x08A62D98u, rnd());
    SH(0x08A62D9Au, rnd());
    self->hunter_flags = PICK(0, 0, 0, 1, 1, 2, 0x200, 3, 0x201, ~3 & ~0x200, -1);
    self->l_held_frames = (int32_t)PICK(0, 1, 9, 10, -1);
    self->buttons_word_498 = rnd();
    uint32_t hud = LW(0x089CBD0Cu);
    ((HudContext *)MEM(hud))->hud_flags = PICK(0, 0x20, ~0x20, -1);
}

static void inputs_common(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    g->quest_state = (uint8_t)PICK(0, 0, 0, 0, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 0, 0, 1, 0x80);
    self->misc_flags = PICK(0, 8, ~8, -1);
    self->hunter_flags = rnd() & rnd();
    self->buttons_pressed = rnd() & rnd();
    self->buttons_held = rnd() & rnd();
    self->state = (uint8_t)PICK(0, 0, 0, 0, 0x40, 0x40, 1, 3, 0x12, 0x13, 0x1F, 0x24, 0x2C, 0x3B, 0x3F, 0x49, 0x49, 0x49, 0x74);
    self->ground_forces_state_3b = (uint8_t)PICK(0, 0, 0, 1, 0x80);
    self->hud_request = (uint8_t)PICK(0, 0, 1);
    self->buildup_state_2_13 = (int16_t)PICK(0, 0, 0, 49, 50, -1);
    self->item_use_lockout_ticks = (uint8_t)PICK(0, 0, 1);
    self->selector_input_flags = (uint8_t)rnd();
    self->button_action = PICK(0, 1, 1, 2, 2, 3, 0x101);
    self->quest_flags = PICK(0, 0x100, ~0x100, -1);
    self->stick_magnitude = (uint16_t)PICK(0, 84, 85, 86, 127, 0x8000);
    self->stamina = (int16_t)PICK(0, 74, 75, 76, 300, -1);
    self->weapon_class = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x80);
    self->facing = rnd();
    self->linked_monster_angle = self->facing + PICK(0x2000, 0x2001, 0x1FFF, -0x2000, -0x2001, 0x10000);
    self->facing_target = (uint16_t)(self->facing + PICK(0, 0x5FFF, 0x6000, 0x6001, 0x9FFF, 0xA000, 0xA001, 0xFFFF));
    args[0] = PICK(0, 0, 1, 2, 3, 0x101, 0x102);
    args[1] = PICK(0, 0, 0, 1, 0x100, 0x101);
    SW(LW(0x08A632C0u) + 0xF7A57Cu, PICK(0, 1, 2, 3));
}

/* -- floats: bits on the boundaries a function tests, either side of them, and what compares false either way ------------------------ */
#define FLOAT_NAN 0x7FC00001u
#define FLOAT_SIGNALLING_NAN 0x7F800001u
#define FLOAT_INFINITY 0x7F800000u
static uint32_t float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}
#define SET_FLOAT_BITS(field, bits) memcpy(&(field), &(uint32_t){bits}, sizeof(uint32_t))

static void inputs_stamina_scaled(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    ((Hunter *)MEM(hunter))->instance_index = pick_slot(c);
    args[0] = PICK(-1, -2, -3, -4, -5, -7, -100, -300, 1, 2, 3, 4, 7, 100, 300, 0x7FFF, -0x8000, 0x10000, 0x10003, 0x12345, 0x7FFFFFFF, 0x80000000, 21845, 16777217);
    if (rnd() % 2)      /* large enough that the factor's last bit shows */
        args[0] = (rnd() & 0x3FFFFFF) * PICK(1, -1);
}

static void inputs_motion_update(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    HunterBase *self = (HunterBase *)MEM(hunter);
    self->freeze_ticks = (uint8_t)PICK(0, 0, 0, 0, 1, 0x80);
    self->motion_slow_timer = (uint8_t)PICK(0, 0, 0, 0, 1, 2, 0x80, 0xFF);
    self->motion_id = (uint16_t)PICK(0x23, 0x22, 0x731, 3, 0x1A7, 0x1A6, 0x1A3, 0x1A2, 0x1A0, 0x19F, 0x19D, 0x19C, 0x19B, 0x197, 0x196, 0x195, 0x58A, 0x587,
                                     0x24, 0x21, 2, 4, 0x1A8, 0x1A5, 0x1A4, 0x1A1, 0x19E, 0x19A, 0x198, 0x194, 0x58B, 0x589, 0x588, 0x586, 0x732, 0x730);
    self->motion_id_layer1 = (uint16_t)rnd();
    self->hunter_flags = PICK(0, 0x80, 0x100, 0x180, ~0x180, -1);
    self->motion_start_frame = (uint16_t)rnd();
    self->part1_motion_start_frame = (uint16_t)rnd();
    self->motion_blend_frames = (int16_t)rnd();
    self->part1_motion_blend_frames = (int16_t)rnd();
    SET_FLOAT_BITS(self->motion_part0_speed, PICK(float_bits(2.0f), float_bits(1.9999999f), float_bits(2.0000002f), 0, 0x80000000, float_bits(-1.0f), float_bits(3.5f), FLOAT_NAN, FLOAT_SIGNALLING_NAN, FLOAT_INFINITY));
    SET_FLOAT_BITS(self->motion_part1_speed, rnd());
    SET_FLOAT_BITS(self->motion_speed_scale, PICK(float_bits(1.0f), float_bits(0.5f), float_bits(1.1f), float_bits(0.3f), float_bits(1e-40f), 0x80000000, FLOAT_NAN, FLOAT_INFINITY));
    ((GameState *)MEM(LW(GAME_POINTER)))->mode_flags = PICK(0, 1, 2, 3, ~1, -1);
    if (!record_method) {       /* callees running for real: a motion that isn't one, or a speed that isn't a number, crashes them, whichever version runs */
        self->hunter_flags |= 0x180;
        SET_FLOAT_BITS(self->motion_part0_speed, PICK(float_bits(2.0f), float_bits(1.9999999f), float_bits(2.0000002f), 0, float_bits(3.5f)));
        SET_FLOAT_BITS(self->motion_speed_scale, PICK(float_bits(1.0f), float_bits(0.5f), float_bits(1.1f), float_bits(0.3f)));
    }
}

static const Bias run_bias[] = {
    {0x08865CB8u, 8, {0, 0, 0, 1, 1, 0x100, 0x101, 2}},     /* the state's tests */
    {0x088D69F0u, 6, {0, 1, 2, 3, 0x101, 0x102}},
    {0x08865BACu, 6, {1, 1, 1, 0, 0x100, 0x101}},           /* in the stage being played */
    {0x09AA8958u, 5, {0, 0, 1, 0x100, 2}},
    {0x09A8BBE0u, 3, {0, 1, 0x100}},
    {0x088DACA4u, 5, {0, 1, 1, 0x101, 2}},
    {0x09AA6B18u, 8, {0x18, 0x21, 0x1E, 0x10018, 0x1001E, 0x10021, 0, 0x19}},
    {0x09C3E698u, 4, {0, 1, 2, 0x101}},
    {0x09BB0FE0u, 4, {0, 1, 0x10000, 0x10001}},
    {0x09A8BB20u, 4, {0, 1, 0x100, 0x101}},
    {0x08814048u, 3, {0, 1, 0x101}},
    {0, 0, {0}},
};
POKES(run_pokes, run_bias, {0x4, 4, 0}, {0x28, 1, 1}, {0x1D6, 1, 0}, {0x1E0, 1, 0}, {0x1E6, 2, 0}, {0x1E8, 1, 0}, {0x200, 4, 0}, {0x204, 4, 0}, {0x208, 4, 0}, {0x20C, 4, 0},
      {0x280, 1, 0}, {0x298, 1, 0}, {0x299, 1, 0}, {0x29C, 1, 0}, {0x2CC, 4, 0}, {0x2E4, 2, 0}, {0x33C, 1, 0}, {0x410, 4, 0}, {0x424, 2, 0}, {0x484, 4, 0},
      {0x564, 1, 0}, {0x564, 1, 0}, {0x564, 1, 0}, {0x5F3, 1, 0}, {0x614, 4, 0}, {0x111C, 4, 0});

static void inputs_state_run(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    self->hunter_flags = rnd() & rnd();
    self->misc_flags = rnd() & rnd();
    self->flags = rnd();
    self->health = (int16_t)PICK(1, 1, 1, 100, 0, -1);
    self->freeze_ticks = (uint8_t)PICK(0, 0, 0, 1);
    self->state_group = (uint8_t)PICK(0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7);
    self->state = (uint8_t)(rnd() % 2 ? rnd() % 0x80 : PICK(1, 3, 9, 0x13, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0x23, 0x24, 0x27, 0x3A, 0x36, 0x65, 0x66, 0x67, 0x6A, 0x7E, 0x7D, 0x7F));
    self->state_scratch_a = (uint8_t)PICK(0, 0, 1, 8, 8, 9, 9, 10, 0xFF);
    self->movement_mode = (uint8_t)PICK(0, 0, 0, 2, 2, 1);
    self->attached_part_mode = (uint8_t)PICK(0, 0, 0, 0, 1);
    self->ground_forces_state_3b = (uint8_t)PICK(0, 1, 1, 0x80);
    self->weapon_class = (uint8_t)PICK(0, 1, 1, 5, 5, 8, 8, 9, 10);
    self->weapon_uses_load = (uint8_t)PICK(0, 1, 2);
    self->status_bits = (uint16_t)PICK(0, 0x200, 0x800, 0x1000, 0x1800, ~0x1A00, -1);
    self->buttons_pressed = rnd() & rnd();
    self->quest_flags = PICK(0, 2, ~2, -1);
    self->flag_1000_lockout_ticks = (uint8_t)PICK(0, 0, 1, 10);
    self->ledge_grabs_from_fall = (int8_t)rnd();
    self->hud_request = (uint8_t)PICK(0, 1);
    self->selector_input_flags = (uint8_t)rnd();
    self->place[0] = (float)(int16_t)rnd() / 16.0f;
    self->place[2] = (float)(int16_t)rnd() / 16.0f;
    SET_FLOAT_BITS(self->place_w, rnd());
    /* above or below the floor, around where the fall starts, and NaN each way round */
    float floor_y = (float)(int16_t)rnd() / 8.0f;
    uint32_t y = float_bits(floor_y + (float)PICK(-1, 0, 1, 29, 30, 31, 100) * 1.0f);
    switch (rnd() % 8) {
    case 0: y = float_bits(floor_y + 29.999998f); break;
    case 1: y = float_bits(floor_y + 30.000002f); break;
    case 2: y = FLOAT_NAN; break;
    case 3: y = FLOAT_SIGNALLING_NAN; break;
    default: break;
    }
    SET_FLOAT_BITS(self->place[1], y);
    self->floor_y = floor_y;
    if (rnd() % 16 == 0)
        SET_FLOAT_BITS(self->floor_y, FLOAT_NAN);
    /* the two long ways: starting to fall, and flag 0x1000 with the weapon out */
    if (rnd() % 4 == 0) {
        self->instance_index = g->local_hunter_slot;
        self->movement_mode = 0;
        self->attached_part_mode = 0;
        self->health = 1;
        self->floor_y = 0.0f;
        SET_FLOAT_BITS(self->place[1], PICK(float_bits(29.999998f), float_bits(30.0f), float_bits(30.000002f), float_bits(31.0f)));
        self->state_group = (uint8_t)PICK(0, 0, 2, 2, 2, 5, 1, 6);
        self->state = (uint8_t)PICK(0x7E, 0x7D, 0x7F, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x23, 0x23, 0x23, 0x24, 0x24, 0);
    } else if (rnd() % 3 == 0) {
        self->instance_index = g->local_hunter_slot;
        self->misc_flags |= 8;
        self->weapon_class = (uint8_t)PICK(1, 5);
        self->flag_1000_lockout_ticks = (uint8_t)PICK(0, 0, 0, 1);
        self->state_group = (uint8_t)PICK(0, 0, 0, 0, 1, 6, 5);
        self->state = (uint8_t)PICK(0x3A, 0x1B, 0x1C, 0x67, 0x68, 0x69, 0x6A, 0x66, 0x6B, 0x1A, 0);
        self->hunter_flags = (self->hunter_flags & ~0x9000u) | PICK(0, 0x8000, 0x8000, 0x1000);
    } else if (rnd() % 2 == 0) {     /* forced by the ground, and the states it isn't */
        self->state_group = 0;
        self->state = (uint8_t)PICK(0x08, 0x09, 0x0F, 0x14, 0x1A, 0x1C, 0x1D, 0x2D, 0x2E, 0x2F, 0x31, 0x32, 0x3B, 0x3C, 0x3D, 0x44, 0x59, 0x5D, 0x60, 0x7D, 0x7E, 0x07, 0x0A, 0x15, 0x1E);
        self->ground_forces_state_3b = 1;
        self->movement_mode = 0;
    }
    if (!record_method) {       /* callees running for real: the faint and end states' reach further than this state can hold up */
        if (self->state_group == 3 || self->state_group == 4 || self->state_group > 6)
            self->state_group = 0;
    }
    uint32_t camera = LW(0x089CBD04u);
    SB(camera + 0x1DF, PICK(0, 0, 0, 1, 0x80));
    SB(camera + 0xA8D, PICK(0, 0, 0, 1));
    g->option_camera_follow_mode = (int8_t)PICK(0, 0, 1, 2, -1, 0x41);
    SB(LW(0x09A1DC2Cu), PICK(0, 1, 2, 3));
    SW(LW(0x08A632C0u) + 0xF7A554u, PICK(30, 60, 0, 5, 10, 15, 20, 25, 7, 8, 9, 11, 13, 14, -30, -5, -10, -7, 0x7FFFFFFF));
}

/* -- damage to a hunter ---------------------------------------------------------------------------------------------------------------- */
#define HEALTHS PICK(-1, 0, 0, 1, 2, 10, 11, 12, 40, 41, 63, 64, 65, 100, 150, 0x7FFF, -0x8000)

static const Bias add_health_bias[] = {
    {0x0884FA5Cu, 5, {0, 0, 1, 0x100, 0x101}},
    {0x088D6A1Cu, 5, {0, 0, 1, 0x100, 0x101}},
    {0, 0, {0}},
};
POKES(add_health_pokes, add_health_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x298, 1, 0}, {0x2E4, 2, 0}, {0x41C, 2, 0}, {0x41E, 2, 0});

static void inputs_add_health(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = pick_slot(c);
    self->state_group = (uint8_t)PICK(0, 0, 1, 2, 3, 4);
    self->health = (int16_t)HEALTHS;
    self->health_red_limit = (int16_t)(self->health + PICK(0, 0, 1, -1, 50, -50));
    self->health_max = (int16_t)PICK(100, 150, 150, self->health, self->health + 1, self->health - 1);
    for (int i = 0; i < 6; i++)
        SB(hunter + 0x676 + i, PICK(0, 0, 0xC8, 0xD3, 1));
    args[0] = PICK(0, 1, -1, 5, -5, 30, 100, -100, -63, -64, -65, -0x8000, 0x7FFF, 0x10000, 0x10001, 0xFFFF, -self->health, -self->health + 1, -self->health - 1, 0x10000 - self->health);
}

static const Bias can_guard_bias[] = {
    {0x088D6A1Cu, 4, {0, 1, 0x100, 0x101}},
    {0x088D2830u, 4, {0, 1, 0x100, 0x101}},
    {0x08865C88u, 5, {0, 0, 0, 1, 0x10000}},
    {0, 0, {0}},
};
POKES(can_guard_pokes, can_guard_bias, {0x1E8, 1, 0}, {0x410, 4, 0}, {0x280, 1, 0}, {0x614, 4, 0}, {0x1F4, 4, 0}, {0x360, 2, 0}, {0x1E6, 2, 0});

static void inputs_can_guard(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    (void)c;
    Hunter *self = (Hunter *)MEM(hunter);
    self->pending_damage_0 = (int16_t)PICK(0, 0, 1, 100, -1, 0x7FFF, -0x8000);
    self->received_hit_type = (uint16_t)PICK(0, 2, 2, 2, 0xD, 0xE, 0xF, 0x10, 0x10E, 0x10F, 0xE0E);
    self->stamina = (int16_t)PICK(75, 75, 76, 300, 300, 74, 0, -1);
    self->pending_hit_flags = (uint8_t)PICK(0, 0, 0, 0x40, 0x80, 0xC0, 0x3F, 0xFF);
    self->weapon_class = (uint8_t)PICK(1, 1, 9, 3, 4, 0, 2, 5, 8, 10, 0x81, 0xFF);
    self->misc_flags = PICK(8, 8, 8, ~0, 0, ~8);
    self->movement_mode = (uint8_t)PICK(0, 0, 1, 1, 2, 3, 0xFF);
    self->hunter_flags = PICK(0, 0x20, 0x20, ~0x20, -1);
    self->facing = rnd();
    self->received_hit_angle = (uint16_t)rnd();
}

static const Bias recalc_defense_bias[] = {
    {0x0885D300u, 8, {0, 1, 100, 0x7FFF, 0xFFFF, 0x8000, 0x10005, -1}},
    {0x088568ACu, 6, {0, 0, 1, 2, -1, 0x80000000}},
    {0x088D6A1Cu, 8, {0, 0, 0, 0, 0, 1, 0x100, 0x101}},
    {0x088DAE98u, 4, {0, 1, 0x100, 0x101}},
    {0x088D6A54u, 4, {0, 1, 0x100, 0x10000}},
    {0x08856E34u, 4, {0, 1, 0x100, 0x101}},
    {0, 0, {0}},
};
POKES(recalc_defense_pokes, recalc_defense_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x38B, 1, 0}, {0x3D8, 1, 0}, {0x2E4, 2, 0}, {0x41E, 2, 0}, {0x5F9, 1, 0}, {0x110E, 2, 0}, {0x422, 2, 0});

static void inputs_recalc_defense(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    self->instance_index = rnd() % 8 ? ((GameState *)MEM(LW(GAME_POINTER)))->local_hunter_slot : pick_slot(c);
    SW(LW(GAME_POINTER) + 0x6AF14u, PICK(0, 0, 8, ~8, -1));
    self->defense_bonus = (int8_t)PICK(0, 0, 5, 127, -1, -128);
    self->defense_bonus_timed = (int8_t)PICK(0, 0, 5, 127, -1, -128);
    self->health_max = (int16_t)PICK(100, 150, 0, 1, -1);
    self->health = (int16_t)(rnd() % 2 ? (int32_t)HEALTHS : (self->health_max * (int32_t)PICK(2, 4, 4, 5)) / 10 + (int32_t)PICK(0, 0, 1, -1));
    self->defense_effect_level = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 0x81);
    self->defense_halved_ticks = (int16_t)PICK(0, 1, 100, -1);
    for (int i = 0; i < 6; i++)
        SB(hunter + 0x676 + i, PICK(0, 0, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0xA3, 0xA4, 0xA5, 0xD3));
    self->timed_effect_mask = rnd();
}

#define RARELY_1 16, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x100, 0x101}
static const Bias received_hit_bias[] = {
    {0x09A6C968u, 8, {0, 0, 0, 0, 0, 1, 1, 0x100}},    /* can it guard */
    {0x088D6A1Cu, RARELY_1},
    {0x088DAE98u, RARELY_1},
    {0x088D6A54u, 6, {0, 0, 0, 1, 0x100, 0x10000}},
    {0x088900C4u, 8, {0, 1, 2, 3, 4, 7, 8, 0x10000}},   /* the chances */
    {0x09A6D228u, RARELY_1},
    {0x09A6D280u, 8, {0, 0, 0, 0, 0, 1, 0x100, 0x101}},
    {0x09A6D2B8u, 4, {0, 0, 1, 0x100}},
    {0x088DA324u, 4, {0, 1, 1, 0x100}},
    {0x09AA8958u, 4, {0, 1, 1, 0x100}},
    {0x088DACA4u, 4, {0, 1, 0x100, 0x101}},
    {0x08865CB8u, 4, {0, 0, 1, 0x100}},
    {0x08865C88u, 4, {0, 0, 1, 0x10000}},
    {0x09C451E0u, 4, {0, 1, 1, 0x100}},
    {0, 0, {0}},
};
POKES(received_hit_pokes, received_hit_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x2E4, 2, 0}, {0x3B8, 2, 0}, {0x3CE, 1, 0}, {0x1E8, 1, 0}, {0x3CC, 2, 0}, {0x440, 2, 0}, {0x442, 2, 0}, {0x446, 2, 0}, {0x458, 2, 0},
      {0x394, 2, 0}, {0x38C, 2, 0}, {0x392, 2, 0}, {0x390, 2, 0}, {0x1118, 2, 0}, {0x10FC, 2, 0}, {0x1110, 2, 0}, {0x1100, 2, 0}, {0x1144, 2, 0}, {0x110C, 2, 0}, {0x41C, 2, 0}, {0x298, 1, 0}, {0x299, 1, 0},
      {0x280, 1, 0}, {0x672, 2, 0}, {0x410, 4, 0}, {0x360, 2, 0}, {0x1F4, 4, 0}, {0x5FB, 1, 0}, {0x602, 1, 0}, {0x3CA, 2, 0}, {0x29C, 1, 0});

static void inputs_received_hit(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    self->hit_pending = (uint8_t)(rnd() % 16 ? PICK(2, 2, 1, 0x80) : 0);
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    self->state_group = (uint8_t)PICK(0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 4, 5);
    self->state = (uint8_t)PICK(0x1A, 0xF, 0xE, 7, 6, 5, 4, 3, 2, 1, 0, 0x10, 0x1B, 0x8);
    self->received_hit_type = (uint16_t)(rnd() % 8 ? rnd() % 0x20 : PICK(0x20, 0x21, 0x100, 0x102, 0xFFFF, 0xD));
    if (rnd() % 2)      /* the kinds with the most ways through */
        self->received_hit_type = (uint16_t)PICK(5, 6, 7, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 2);
    self->received_hit_power = (uint16_t)PICK(0, 1, 10, 11, 12, 0x7FFF, 0x8000, 0xFFFF);
    self->pending_damage_0 = (int16_t)PICK(0, 1, 1, 2, 3, 10, 50, 100, 101, -1, -5, 16385, 0x7FFF, -0x8000);
    self->health = (int16_t)PICK(100, 100, 100, 100, 100, 100, 1, 2, 50, 0, -1);
    self->health_red_limit = (int16_t)(self->health + (int32_t)PICK(0, 1, -1, 30));
    self->health_max = 150;
    self->pending_hit_flags = (uint8_t)PICK(0, 0, 0x40, 0x80, 0xC0, 0x3F);
    self->weapon_class = (uint8_t)PICK(0, 1, 1, 2, 3, 8, 9, 0x81);
    self->pending_buildup_direct = (int16_t)PICK(0, 0, 1, 30, 50, -1);
    self->pending_buildup_mask1 = (int16_t)PICK(0, 0, 1, 30, -1);
    self->pending_buildup_mask4 = (int16_t)PICK(0, 0, 1, 30, -1);
    self->pending_buildup_mask2 = (int16_t)PICK(0, 0, 0, 1, 30, -1);
    self->pending_drain_seconds = (int16_t)PICK(0, 0, 0, 1, 30, -1);
    self->pending_item_restrict = (int16_t)PICK(0, 0, 1, 1800, -1);
    self->pending_stamina_debuff = (int16_t)PICK(0, 0, 1, 1800, -1);
    self->pending_buildup_mash = (int16_t)PICK(0, 0, 1, 30, -1);
    self->pending_defense_halve = (int16_t)PICK(0, 0, 1, 30, -1);
    self->buildup_state_2_13 = (int16_t)PICK(0, 0, 49, 50, 51, 20, -1);
    self->buildup_state_2_15 = (int16_t)PICK(0, 7, -1);
    self->buildup_state_2_16 = (int16_t)PICK(0, 7, -1);
    self->buildup_mash_status = (int16_t)PICK(0, 7, -1);
    self->movement_mode = (uint8_t)PICK(0, 0, 0, 0, 0, 1, 2, 3);
    self->attached_part_mode = (uint8_t)PICK(0, 1, 0x80);
    self->facing = rnd();
    self->received_hit_angle = (uint16_t)rnd();
    if (rnd() % 3 == 0)         /* just either side of a quarter turn */
        self->facing = self->received_hit_angle + PICK(0x3FFF, 0x4000, 0x3FFE, -0x4000, -0x4001, -0x3FFF, 0, 0x8000, 0x10000);
    self->facing_target = (uint16_t)rnd();
    self->misc_flags = PICK(0, 8, ~8, -1);
    self->charge_time = (int16_t)PICK(0, 0, 1, -1);
    self->hit_type_5_6_7_immunity_level = (uint8_t)PICK(0, 0, 1, 2, 3, 0xFF);
    self->hit_type_16_1e_immunity_level = (uint8_t)PICK(0, 0, 1, 2, 3, 0xFF);
    self->hit_immune_ticks = (uint16_t)PICK(0, 5);
    self->defense_bonus = (int8_t)PICK(0, 3);
    self->defense_bonus_timed = (int8_t)PICK(0, 3);
    self->defense_bonus_ticks = (uint16_t)PICK(0, 3);
    self->defense_effect_level = (uint8_t)PICK(0, 3);
    self->defense_effect_ticks = (int16_t)PICK(0, 3);
    self->item_restrict_ticks = (int16_t)PICK(0, 3);
    self->defense_halved_ticks = (int16_t)PICK(0, 3);
    self->stamina_debuff_ticks = (int16_t)PICK(0, 3);
    SW(LW(0x08A632C0u) + 0xF7A57Cu, PICK(0, 0, 1, 3, 2, -1));
    if (rnd() % 2) {    /* straight to the reactions with the most tests */
        self->hit_pending = 2;
        self->instance_index = g->local_hunter_slot;
        self->state_group = (uint8_t)PICK(0, 0, 1, 2, 2, 4, 5);
        self->health = 100;
        self->movement_mode = (uint8_t)PICK(0, 0, 0, 1);
        self->received_hit_type = (uint16_t)PICK(5, 5, 5, 6, 6, 6, 7, 19, 20, 22, 26, 27, 28, 30, 31, 2);
        self->weapon_class = (uint8_t)PICK(0, 2, 8, 9);
        self->hit_type_5_6_7_immunity_level = (uint8_t)(self->received_hit_type == 7 ? PICK(2, 3, 3, 4) : PICK(0, 0, 0, 1, 1, 2));
    }
    for (int i = 0; i < 6; i++)     /* for the real callees */
        SB(hunter + 0x676 + i, PICK(0, 0, 0, 0x58, 0x59, 8, 7, 0xD0, 5, 2, 0xBB, 0xD6, 0xE, 0xD5, 0xD, 0x2C, 0x5A, 0x5B, 0x82, 0x83, 0x84, 0xB5, 0xC8));
    if (!record_method && (self->state_group == 3 || self->state_group == 4))   /* callees running for real: not those */
        self->state_group = 0;
}

/* -- hits: objects made up in RAM nothing else uses, the records and shapes the code goes through ----------------------------------- */
#define HIT_RECORD 0x09FF9000u
#define HIT_RECORD_2 0x09FF9100u
#define HIT_RECORD_3 0x09FF9200u
#define HIT_CONTACT_AT 0x09FF9300u
#define HIT_SHAPES 0x09FF9400u
#define HIT_PARTS 0x09FF9600u
#define HIT_OUT 0x09FF9800u
#define HIT_VTABLE 0x09FF9880u       /* a record's and a made-up target's: its slots a stub */
#define HIT_TARGET_A 0x09FFA000u     /* targets: objects of the size the code reads */
#define HIT_TARGET_B 0x09FFC000u
#define HIT_ATTACKER 0x09FFE000u
#define MONSTERS_POINTER 0x09C122B0u
#define HUNTER_OF_SLOT 0x088DCEBCu
#define RECORD_TEST 0x09B67728u
#define MONSTER_TYPE_RECORD(subtype) (0x09BB8AB0u + (uint32_t)(subtype) * 0x1D0u)

static uint32_t first_monster(Ctx *c) {
    uint32_t monsters = LW(MONSTERS_POINTER);
    for (uint32_t i = 0; i < 20; i++)
        if (LW(monsters + 0x1220u + 4 * i) != 0)
            return LW(monsters + 0x1220u + 4 * i);
    fprintf(stderr, "no monster in this snapshot\n");
    exit(2);
}

static void random_record(Ctx *c, uint32_t at) {
    HitRecord *r = (HitRecord *)MEM(at);
    for (uint32_t o = 0; o < 0x80; o += 4)
        SW(at + o, rnd());
    r->owner_kind = (uint8_t)PICK(0, 1, 2, 0xFF);
    r->owner_table = (uint8_t)PICK(0, 0, 1, 2);
    r->state = (uint8_t)PICK(2, 2, 2, 1, 0, 0x82);
    r->owner_slot = (uint8_t)PICK(0, 1, 3, 19, 20, 0x80, 0xFF);
    r->area_id = (uint16_t)PICK(5, 5, 5, 6, 0x105);
    r->hit_id = (uint16_t)PICK(0xFFFF, 0xFFFF, 0x1234, 0x1235, 0x34);
    r->damage = (uint8_t)PICK(0, 1, 2, 10, 50, 127, 128, 255);
    r->kind = (uint8_t)PICK(0xB, 0xE, 0xF, 0x10, 0x15, 0x16, 0x19, 0x1A, 0x1E, 2, 8, 9, 0xC, 0xD, 3, 0x20, 0x80);
    r->power = (uint8_t)PICK(0, 10, 11, 0xFF);
    r->damage_bits = (uint8_t)(rnd() % 2 ? rnd() : 1u << (rnd() % 8));
    r->sound_choice = (uint8_t)PICK(0, 1, 2, 0xFF);
    r->buildup_mask = (uint16_t)(rnd() % 3 ? rnd() : PICK(8, 8, 0, 0x2000, 0xFFFF));
    r->buildup_amount = (uint8_t)PICK(0, 1, 30, 127, 128, 255);
    r->drain_amount = (uint8_t)PICK(0, 1, 0x7F, 0x80, 0x81, 0xFF);
    r->uses = (uint8_t)PICK(0, 0, 1, 2, 0x10, 0x11, 0xF0, 0xFF);
    r->direct_buildup = (uint8_t)PICK(0, 1, 30, 255);
    r->damage_percent = (uint8_t)PICK(0, 0, 0, 1, 50, 100, 127, 128, 255);
}

/* The shapes and part records a record and a target go through. Each can jump, and each ends. */
static void random_shape_list(Ctx *c, uint32_t at, int n) {
    for (int i = 0; i < n; i++) {
        SH(at + 0x28u * (uint32_t)i, PICK(0, 0, 3, 0x7E, 0x7F, 0x7D, 0x7C, 0x80, 0x17D));
        SB(at + 0x28u * (uint32_t)i + 8, rnd() % 3 ? 0 : rnd());
        SB(at + 0x28u * (uint32_t)i + 9, rnd() % 3 ? 0 : rnd());
        SB(at + 0x28u * (uint32_t)i + 0xA, rnd() % 3 ? 0 : rnd());
        SB(at + 0x28u * (uint32_t)i + 0xB, rnd() % 4 ? 0 : rnd());
    }
    SH(at + 0x28u * (uint32_t)n, -1);
}

static int16_t stub_short[MAX_CALLS][2];

/* What the shapes' functions leave in the frame, and the lists' cursors */
static void hit_stub_hook(Ctx *c, uint32_t fn, int n) {
    switch (fn) {
    case 0x09C387A0u: case 0x09C38758u:     /* a list goes on */
        SW(c->r[5], LW(c->r[5]) + 0x28);
        break;
    case 0x09C37A50u: case 0x09C37898u:     /* the type word of a shape */
        SW(c->r[6], stub_short[n][0] & 1);
        break;
    case 0x09C37BC0u:
        SW(c->r[7], stub_short[n][0] & 1);
        break;
    case 0x09C426E0u:                       /* the list ends, as the test ran: is its end read after it? */
        if ((stub_short[n][0] & 0xF) == 0)
            SW(HIT_RECORD + 4, 0);
        break;
    default:
        break;
    }
}

static const Bias hit_shared_bias[] = {
    {0x09C36238u, 4, {0, 1, 2, 0x101}}, {0x09C34718u, 4, {0, 1, 2, 0x101}}, {0x09C34878u, 4, {0, 0, 1, 0x100}}, {0x09C359C8u, 4, {0, 0, 1, 0x100}},
    {RECORD_TEST, 6, {0, 0, 0, 1, 0x10000, -1}},
    {0x09C45190u, 4, {0, 0, 1, 0x100}},
    {0x08865BACu, 4, {0, 1, 1, 0x100}},
    {HUNTER_OF_SLOT, 8, {0, HIT_TARGET_A, HIT_TARGET_A, HIT_TARGET_B, HIT_TARGET_B, HIT_ATTACKER, HIT_ATTACKER, 0}},
    {0, 0, {0}},
};

static void random_target(Ctx *c, uint32_t at, uint32_t kind) {
    Monster *t = (Monster *)MEM(at);
    for (uint32_t o = 0; o < 0x800; o += 4)
        SW(at + o, rnd());
    t->kind = (uint8_t)kind;
    t->area_id = (uint16_t)PICK(5, 5, 5, 6, 0x105);
    t->hit_pending = (uint8_t)PICK(0, 0, 0, 0, 1);
    t->last_hit_record = PICK(0, 0, 0, 0, 1);
    t->hit_immune_ticks = (uint16_t)PICK(0, 0, 0, 0, 1, 0x100);
    t->movement_mode = (uint8_t)PICK(0, 0, 1, 2, 3, 4);
    t->state_group = (uint8_t)PICK(0, 0, 1, 3, 3);
    t->state_flag_cleared_with_3cf = (uint8_t)PICK(0, 0, 0, 1, 0x80);
    t->health = (int16_t)PICK(1, 1, 100, 0, -1);
    t->place[1] = (float)PICK(0, 0, 100, -100) * 1.0f;
    t->part_mask_3_ticks = (int8_t)PICK(0, 1, -1);
    t->part_mask_c_ticks = (int8_t)PICK(0, 1, -1);
    t->locally_owned = (uint8_t)PICK(0, 1, 1, 1);
    t->status_hold_mode = (uint8_t)PICK(0, 1, 2, 3);
    t->subtype = (uint8_t)PICK(77, 35, 2);
    for (int i = 0; i < 16; i++)
        SH(at + 0x33E + 2u * (uint32_t)i, rnd() % 16 ? rnd() : 0x1234);
    SW(MONSTER_TYPE_RECORD(t->subtype) + 0x50, HIT_PARTS);
}

/* 09C42750 reads these again after its calls */
POKES(test_target_pokes, hit_shared_bias, {0x1E9, 1, 0}, {0x635, 1, 0}, {0x634, 1, 0}, {0x6DB, 1, 0}, {0x3CF, 1, 0}, {0x4A0, 4, 0}, {0x4C0, 4, 0}, {0x200, 4, 0}, {0x204, 4, 0}, {0x208, 4, 0}, {0x4B0, 4, 0}, {0x4D0, 4, 0},
      {0x1F, 1, 2}, {0x28, 2, 2}, {0x14, 1, 2});
POKES(test_if_in_map_pokes, hit_shared_bias, {0x14, 1, 2});
POKES(hit_manager_pokes, hit_shared_bias, {0x14, 1, 2}, {0x18, 2, 2}, {0x1A, 2, 2}, {0xE, 1, 2}, {0xF, 1, 2}, {0x15, 1, 2}, {0x1F, 1, 2}, {0x44, 4, 2}, {0x29A, 2, 0}, {0x285, 1, 0}, {0x2F0, 4, 0},
      {0x280, 1, 0}, {0x613, 1, 0}, {0x2E4, 2, 0}, {0x204, 4, 0}, {0x33E, 2, 0}, {0x34C, 2, 0});

static void hit_common(Ctx *c, uint32_t target) {
    stub_hook = hit_stub_hook;
    stack_pattern = 1;
    stub_hunter = target;
    hashed_more[0].at = HIT_RECORD;
    hashed_more[0].size = 0x100;
    hashed_more[1].at = HIT_ATTACKER;
    hashed_more[1].size = 0x400;
    poke_base[2] = HIT_RECORD;
    poke_base[3] = HIT_ATTACKER;
    for (uint32_t slot = 0; slot < 0x100; slot += 4)
        SW(HIT_VTABLE + slot, 0x09C451F0u);
    random_record(c, HIT_RECORD);
    SW(HIT_RECORD, HIT_VTABLE);
    for (int n = 0; n < MAX_CALLS; n++)
        stub_short[n][0] = (int16_t)rnd();
}

static void real_hit(Ctx *c, uint32_t hunter, uint32_t args[3]);

static void inputs_test_if_in_map(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    if (!record_method) {
        hit_common(c, hunter);
        real_hit(c, hunter, args);
        return;
    }
    hit_common(c, HIT_TARGET_A);
    random_target(c, HIT_TARGET_A, 0);
    test_a0 = HIT_RECORD;
    args[0] = HIT_ATTACKER;
    args[1] = HIT_TARGET_A;
}

/* Callees running for real: a record of the kind the game makes, its shape a sphere or capsule at the target, so that it lands: real shapes and targets */
static void real_hit(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    uint32_t monster = first_monster(c), target = rnd() % 2 ? hunter : monster;
    HitRecord *r = (HitRecord *)MEM(HIT_RECORD);
    Monster *t = (Monster *)MEM(target);
    r->state = 2;
    r->owner_kind = (uint8_t)PICK(0, 0, 1, 0xFF);
    r->owner_table = 0;
    r->owner_slot = 0;
    r->area_id = t->area_id;
    r->hit_id = 0xFFFF;
    r->kind = (uint8_t)PICK(0, 2, 3, 4, 8, 9, 0xB, 0xC, 0x16, 0x1A);
    r->uses = (uint8_t)PICK(0, 1, 0x10);
    r->sound_choice = (uint8_t)PICK(0, 1, 0xFF);
    r->shape_a[0] = t->place[0];
    r->shape_a[1] = t->place[1];
    r->shape_a[2] = t->place[2];
    r->shape_a[3] = 0.0f;
    r->shapes = HIT_SHAPES;
    SW(HIT_RECORD + 0x38, HIT_SHAPES);      /* the list 09C422E8 goes through */
    SH(HIT_SHAPES, 0x7F);
    SH(HIT_SHAPES + 2, rnd() % 4 ? 0 : 1);
    SW(HIT_SHAPES + 0xC, float_bits((float)PICK(1, 50, 200, 1000)));
    for (uint32_t o = 0x10; o < 0x28; o += 4)
        SW(HIT_SHAPES + o, 0);
    SH(HIT_SHAPES + 0x28, -1);
    t->hit_pending = 0;
    t->hit_immune_ticks = 0;
    t->state_group = 0;
    test_a0 = HIT_RECORD;
    args[0] = rnd() % 2 ? hunter : monster;
    args[1] = target;
}

static void inputs_test_target(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    if (!record_method) {
        hit_common(c, hunter);
        real_hit(c, hunter, args);
        return;
    }
    hit_common(c, HIT_TARGET_A);
    random_target(c, HIT_TARGET_A, PICK(0, 0, 1, 1, 1, 2));
    if (rnd() % 2)      /* a fainting hunter's kind goes with its state */
        ((Monster *)MEM(HIT_TARGET_A))->state_group = ((HitRecord *)MEM(HIT_RECORD))->kind == 0xD ? 3 : 0;
    ((Monster *)MEM(HIT_TARGET_A))->hit_immune_ticks = (uint16_t)PICK(0, 0, 0, 0, 0, 0, 1);
    random_shape_list(c, HIT_SHAPES, (int)(rnd() % 4));
    random_shape_list(c, HIT_PARTS, (int)(rnd() % 4));
    ((HitRecord *)MEM(HIT_RECORD))->shapes = rnd() % 16 ? HIT_SHAPES : 0;
    test_a0 = HIT_RECORD;
    args[0] = PICK(HIT_ATTACKER, HIT_ATTACKER, 0);
    args[1] = HIT_TARGET_A;
    ((HitRecord *)MEM(HIT_RECORD))->shape_a[1] = (float)PICK(0, 150, 151, 149);
    /* the bits the kinds go by, and the buildup's */
    if (rnd() % 2) {
        ((HitRecord *)MEM(HIT_RECORD))->kind = (uint8_t)PICK(8, 8, 0xC, 9, 9, 0x1E, 0x16, 2, 0x20);
        ((Monster *)MEM(HIT_TARGET_A))->locally_owned = (uint8_t)PICK(0, 1);
        SB(HIT_PARTS + 8, PICK(0, 1, 2, 3, 4, 5, 6, 7));
        SB(HIT_PARTS + 9, PICK(0, 0, 1, 2, 3, 8, 9));
        SB(HIT_PARTS + 0xA, PICK(0, 0, 2, 8, 10));
        SB(HIT_PARTS + 0xB, PICK(0, 0, 0, 1, 0xFE));
    }
}

static void inputs_hit_manager(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    if (!record_method) {       /* the real ones, with a made-up record first, and targets free to take it */
        static uint32_t real_first, seen;       /* what's set here stays set, from one time to the next */
        if (!seen++)
            real_first = LW(LW(0x09C190D0u) + 0x502Cu);
        uint32_t monster = first_monster(c);
        hit_common(c, hunter);
        real_hit(c, hunter, args);
        ((HitRecord *)MEM(HIT_RECORD))->next = real_first;
        SW(LW(0x09C190D0u) + 0x502Cu, HIT_RECORD);
        Monster *m = (Monster *)MEM(monster);
        m->last_hit_record = 0;
        m->hit_pending = 0;
        ((Hunter *)MEM(hunter))->hit_pending = 0;
        ((Hunter *)MEM(hunter))->last_hit_record = 0;
        return;
    }
    (void)hunter;
    hit_common(c, HIT_TARGET_A);
    random_target(c, HIT_TARGET_A, 0);
    random_target(c, HIT_TARGET_B, 1);
    random_target(c, HIT_ATTACKER, 1);
    random_record(c, HIT_RECORD_2);
    random_record(c, HIT_RECORD_3);
    SET_FLOAT_BITS(((HitRecord *)MEM(HIT_RECORD))->shape_a[1], PICK(float_bits(0.0f), float_bits(150.0f), float_bits(150.01f), float_bits(149.99f), float_bits(250.0f), float_bits(-50.0f)));
    SET_FLOAT_BITS(((HitRecord *)MEM(HIT_RECORD_2))->shape_a[1], PICK(float_bits(0.0f), float_bits(150.0f), float_bits(150.01f), float_bits(149.99f), float_bits(250.0f), float_bits(-50.0f)));
    SET_FLOAT_BITS(((HitRecord *)MEM(HIT_RECORD_3))->shape_a[1], PICK(float_bits(150.0f), FLOAT_NAN));
    SW(HIT_RECORD + 4, PICK(0, HIT_RECORD_2, HIT_RECORD_2));
    SW(HIT_RECORD_2 + 4, PICK(0, HIT_RECORD_3));
    SW(HIT_RECORD_3 + 4, 0);
    SW(LW(0x09C190D0u) + 0x502Cu, rnd() % 32 ? HIT_RECORD : 0);
    uint32_t monsters = LW(MONSTERS_POINTER);
    for (uint32_t i = 0; i < 20; i++)
        SW(monsters + 0x1220u + 4 * i, 0);
    SW(monsters + 0x1220u, PICK(0, HIT_TARGET_B, HIT_TARGET_B, HIT_ATTACKER));
    SW(monsters + 0x1220u + 4 * 1, PICK(0, HIT_TARGET_B, HIT_ATTACKER));
    SW(monsters + 0x1220u + 4 * 3, PICK(0, 0, HIT_TARGET_A));
    SW(monsters + 0x1220u + 4 * 19, PICK(0, 0, 0, HIT_TARGET_B));
}

static const Bias register_hit_bias[] = {
    {0x09C44FD8u, 8, {0, 0, 0, 0, 0, 1, 0x100, 0x101}},
    {RECORD_TEST, 5, {0, 0, 1, 0x10000, -1}},
    {0x088D6A1Cu, RARELY_1},
    {0x088DAE98u, 8, {0, 0, 0, 0, 0, 1, 0x100, 0x101}},
    {0x09C45190u, 4, {0, 1, 0x100, 0x101}},
    {0x09C451E0u, 4, {0, 1, 0x100, 0x101}},
    {0x09A6C968u, 4, {0, 0, 1, 0x100}},
    {0, 0, {0}},
};
POKES(register_hit_pokes, register_hit_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x3B8, 2, 0}, {0x384, 4, 0}, {0x394, 2, 0}, {0x398, 1, 0}, {0x38A, 1, 0}, {0x38B, 1, 0}, {0x110E, 2, 0}, {0x602, 1, 0}, {0x3CE, 1, 0},
      {0x39C, 4, 0}, {0x3B0, 1, 0}, {0x1F, 1, 2}, {0x1E, 1, 2}, {0x25, 1, 2}, {0x77, 1, 2}, {0x24, 1, 2}, {0x21, 1, 2}, {0x27, 1, 2}, {0xE, 1, 2}, {0xF, 1, 2}, {0x30, 1, 2}, {0x2C, 1, 2}, {0x380, 4, 3});

#define FLOATS PICK(float_bits(0.0f), float_bits(1.0f), float_bits(0.5f), float_bits(25.0f), float_bits(-50.0f), float_bits(100.0f), float_bits(150.5f), float_bits(-80.0f), float_bits(1.35f), float_bits(1e-40f), FLOAT_NAN, FLOAT_INFINITY)

static void inputs_mask_amounts(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    random_record(c, HIT_RECORD);
    self->kind = (uint8_t)PICK(0, 0, 1, 3);
    self->pending_buildup_mask1 = (int16_t)rnd();
    self->pending_buildup_mask2 = (int16_t)rnd();
    self->pending_buildup_mask4 = (int16_t)rnd();
    self->pending_buildup_mask8 = (int16_t)rnd();
    self->pending_buildup_mash = (int16_t)rnd();
    self->pending_item_restrict = (int16_t)rnd();
    self->pending_stamina_debuff = (int16_t)rnd();
    self->pending_defense_halve = (int16_t)rnd();
    self->pending_drain_seconds = (int16_t)rnd();
    args[0] = HIT_RECORD;
}

static const Bias recalc_resists_bias[] = {
    {0x0885D300u, 8, {0, 1, 5, 25, -10, 0x7FFF, 0x10005, 0x80000000}},
    {0x088DA150u, 6, {0, 1, 1, 1, 0x100, 0x101}},
    {0, 0, {0}},
};
POKES(recalc_resists_pokes, recalc_resists_bias, {0x39C, 4, 0}, {0x3A0, 4, 0}, {0x3A4, 4, 0}, {0x3A8, 4, 0}, {0x3AC, 4, 0}, {0x607, 1, 0}, {0x608, 1, 0}, {0x609, 1, 0}, {0x60A, 1, 0}, {0x60B, 1, 0});

static void inputs_recalc_resists(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    SW(hunter + 0x39C, rnd());
    SW(hunter + 0x3A0, rnd());
    SW(hunter + 0x3A4, rnd());
    SW(hunter + 0x3A8, rnd());
    SW(hunter + 0x3AC, rnd());
    for (uint32_t i = 0; i < 6; i++)
        SB(hunter + 0x5F7 + 0x10 + i, PICK(0, 1, 2, 3, 4, 5, 0x81));
    if (rnd() % 2) {        /* the table as other numbers */
        for (uint32_t i = 0; i < 5; i++)
            SH(0x089B6610u + 2 * i, PICK(0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0));
    } else {
        for (uint32_t i = 0; i < 5; i++)
            SH(0x089B6610u + 2 * i, 0x10 + i);
    }
}

static void inputs_add_resist(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    for (uint32_t o = 0x39C; o <= 0x3AC; o += 4)
        SW(hunter + o, PICK(float_bits(0.0f), float_bits(10.0f), float_bits(-5.5f), float_bits(1e-40f), FLOAT_NAN, FLOAT_INFINITY));
    args[0] = PICK(0x10, 0x11, 0x12, 0x13, 0x14, 0xF, 0x15, 0, 0x10010, 0x10013, 0x8010, 0xFFFF0014);
    test_f12 = PICK(float_bits(10.0f), float_bits(15.0f), float_bits(20.0f), float_bits(-3.25f), float_bits(1e-40f), FLOAT_NAN);
}

static void inputs_resist_bytes(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    for (uint32_t o = 0x39C; o <= 0x3AC; o += 4)
        SW(hunter + o, PICK(float_bits(0.0f), float_bits(10.0f), float_bits(-5.5f), float_bits(1e-40f)));
    for (uint32_t i = 0; i < 6; i++)
        SB(LW(GAME_POINTER) + 0x6993Du + i, PICK(0, 1, 5, 127, -1, -128));
}

static void inputs_scale_table(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    Monster *self = (Monster *)MEM(hunter);
    compare_f0 = 1;
    self->subtype = (uint8_t)PICK(0x59, 0x59, 0x58, 0x5A, 0xD9, 2, 77);
    /* tables made up, so that a wrong index or offset shows: the game's own may run short of quests */
    uint32_t tables = HIT_SHAPES;
    int table = PICK(0, 1, 2);
    SB(0x09BB8AB0u + self->subtype * 0x1D0u + 0x14F, table);
    SW(0x09BC7F80u + 4u * (uint32_t)table, tables);
    SB(LW(0x08A62D9Cu) + 0x116, PICK(0, 1, 2));
    for (uint32_t i = 0; i < 3; i++)
        SW(tables + 4 * i, HIT_PARTS + 0x40 * i);
    for (uint32_t o = 0; o < 0xC0; o += 4)
        SW(HIT_PARTS + o, rnd());
    SB(hunter + 0x4BC, PICK(0, 1, 2));
    SW(LW(0x09BC2DACu) + 0x48, PICK(float_bits(1.0f), float_bits(1.5f), float_bits(0.3f), FLOAT_NAN));
}

static void inputs_clear_hit_fields(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    (void)c;
    Hunter *self = (Hunter *)MEM(hunter);
    self->pending_buildup_mash = 1;
    self->pending_item_restrict = 2;
    self->pending_defense_halve = 3;
    self->pending_stamina_debuff = 4;
    self->pending_drain_seconds = 5;
}

static void inputs_resist_skills(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)args;
    for (uint32_t o = 0x39C; o <= 0x3AC; o += 4)
        SW(hunter + o, float_bits((float)(rnd() % 256) / 8.0f - 10.0f));
    static const uint8_t skills[] = {0x67, 0xD4, 0x66, 0x69, 0x68, 0x6B, 0x6A, 0x6D, 0x6C, 0x73, 0x72, 0x75, 0x74, 0x77, 0x76, 0x79, 0x78, 0x6F, 0x6E, 0x71, 0x70, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x10};
    for (int i = 0; i < 6; i++)     /* real callee, and stubs by chance */
        SB(hunter + 0x676 + i, skills[rnd() % sizeof skills]);
}

/* A hit on a part. In stub mode everything is made up: zone tables, sharpness tables and their sounds, so that a wrong index shows */
#define HIT_ZONE_POINTERS 0x09FF9A00u
#define HIT_ZONES 0x09FF9A20u
static const Bias hit_zones_bias[] = {
    {RECORD_TEST, 8, {0, 0, 1, 1, 1, 1, 0x10000, -1}},
    {0x09C45140u, 8, {0, 0, 0, 0, 1, 2, 3, 0x100}},
    {0x09C44FD8u, 8, {0, 0, 0, 0, 0, 0, 1, 0x100}},
    {0x088D6A1Cu, 8, {0, 0, 0, 0, 0, 1, 0x100, 0x101}},
    {0x088DAE98u, 4, {0, 0, 1, 0x100}},
    {0x088D6A54u, 8, {0, 0, 0, 0, 1, 1, 0x100, 0x101}},
    {0x088900C4u, 4, {0, 1, 2, 0x10000}},
    {0x09A88408u, 4, {0, 1, 2, 0x10001}},
    {0x09C451F0u, 8, {0, 5, 5, 5, 6, 6, 6, 0x105}},     /* the record's method */
    {0, 0, {0}},
};
POKES(hit_zones_pokes, hit_zones_bias, {0x1E9, 1, 0}, {0x1E8, 1, 0}, {0x3B8, 2, 0}, {0x3C6, 2, 0}, {0x394, 2, 0}, {0x398, 1, 0}, {0x3CF, 1, 0}, {0x3D0, 1, 0}, {0x4C0, 1, 0}, {0x3CE, 1, 0}, {0x3B0, 1, 0}, {0x482, 2, 0},
      {0x27, 1, 2}, {0x1E, 1, 2}, {0x1F, 1, 2}, {0x21, 1, 2}, {0x24, 1, 2}, {0x25, 1, 2}, {0x28, 2, 2}, {0x2A, 1, 2}, {0x2C, 1, 2}, {0x2D, 1, 2}, {0x30, 1, 2}, {0x77, 1, 2}, {0xF, 1, 2}, {0x2E, 1, 2},
      {0x1E8, 1, 3}, {0x1E9, 1, 3}, {0x1E6, 2, 3}, {0x397, 1, 3}, {0x33D, 1, 3}, {0x380, 4, 3}, {0x424, 2, 3}, {0x5F2, 1, 3}, {0x5F3, 1, 3}, {0x5F7, 1, 3}, {0x10F0, 1, 3}, {0x10F1, 1, 3}, {0x1114, 4, 3});

static void inputs_hit_zones(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    HitRecord *r = (HitRecord *)MEM(HIT_RECORD);
    if (!record_method) {       /* the real ones: a real monster and its first part, the tables as they are */
        uint32_t monster = first_monster(c);
        Monster *m = (Monster *)MEM(monster);
        hit_common(c, monster);
        real_hit(c, hunter, args);
        uint32_t parts = LW(MONSTER_TYPE_RECORD(m->subtype) + 0x50);
        for (uint32_t skip = rnd() % 3; skip && (int16_t)LH(parts + 0x28) != -1; skip--)
            parts += 0x28;
        args[0] = PICK(hunter, hunter, hunter, monster, 0);
        r->owner_kind = args[0] ? (uint8_t)PICK(0, 0, 1, 0xFF) : 0xFF;
        args[1] = monster;
        args[2] = HIT_SHAPES;
        test_t0 = parts;
        test_t1 = HIT_CONTACT_AT;
        Hunter *h = (Hunter *)MEM(hunter);
        h->weapon_class = (uint8_t)PICK(0, 0, 1, 2, 5);
        h->sharpness_level = (uint8_t)PICK(0, 1, 2, 3, 4);
        r->owner_table = (uint8_t)PICK(0, 0, 0, 1);
        r->sound_choice = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xFF);
        return;
    }
    hit_common(c, HIT_TARGET_A);
    hashed_more[1].size = 0x1200;
    random_target(c, HIT_TARGET_A, PICK(1, 1, 1, 1, 2, 0));
    random_target(c, HIT_ATTACKER, PICK(0, 0, 0, 0, 0, 1));
    SW(HIT_RECORD, HIT_VTABLE);
    SW(HIT_TARGET_A, HIT_VTABLE);
    Monster *t = (Monster *)MEM(HIT_TARGET_A);
    Hunter *a = (Hunter *)MEM(HIT_ATTACKER);
    t->subtype = (uint8_t)PICK(0x1D, 0x1D, 0x1D, 0x55, 0x34, 0x41, 0x40, 77, 2);
    t->hit_zone_table = (int8_t)PICK(0, 1, 2);
    t->status_hold_mode = (uint8_t)PICK(0, 0, 1, 2, 2, 3, 4, 5);
    t->cannot_die_ticks = (int8_t)PICK(0, 0, 1, -1);
    t->hit_mark_flag = (uint8_t)PICK(0, 1, 0xFF);
    t->hit_log_index = (uint8_t)PICK(0, 1, 14, 15, 16, 0xFF);
    for (uint32_t i = 0; i < 8; i++) {
        SB(HIT_TARGET_A + 0x3B0 + i, rnd());
        SH(HIT_TARGET_A + 0x3B8 + 2 * i, rnd());
    }
    SW(MONSTER_TYPE_RECORD(t->subtype) + 0x10C, HIT_ZONE_POINTERS);
    for (uint32_t i = 0; i < 3; i++)
        SW(HIT_ZONE_POINTERS + 4 * i, HIT_ZONES + 0x40 * i);
    for (uint32_t o = 0; o < 0xC0; o++)
        SB(HIT_ZONES + o, PICK(0, 1, 25, 30, 50, 100, 127, 128, 200, 255));
    SH(HIT_PARTS + 4, PICK(0, 1, 2));
    SH(HIT_PARTS + 6, PICK(0, 1, 2, 7, 8, 0x10F, -1));
    SB(HIT_PARTS + 0xB, PICK(0, 8, 0xF7, 0xFF));
    SH(HIT_SHAPES + 4, PICK(0, 1, 2, 0x101));
    a->weapon_class = (uint8_t)PICK(0, 0, 1, 1, 5, 2, 9, 0xA, 0xA, 0xC, 0xC);
    a->sharpness_level = (uint8_t)PICK(0, 1, 2, 3, 4, 5, 6);
    a->attack_percent_a = (uint8_t)PICK(0, 1, 50, 100, 127, 128, 255);
    a->attack_percent_b = (uint8_t)PICK(0, 1, 50, 100, 127, 128, 255);
    a->attack_connected_reaction = (uint8_t)PICK(0, 1, 2, 3);
    a->status_bits = (uint16_t)PICK(0, 0x2000, ~0x2000, -1);
    a->weapon_uses_load = (uint8_t)PICK(0, 1, 1, 2);
    a->timed_effect_0_level = (uint8_t)PICK(0, 1, 2, 3);
    a->instance_index = (uint16_t)rnd();
    SET_FLOAT_BITS(a->attack_multiplier, float_bits(1.0f + (float)(rnd() % 256) / 64.0f));
    SET_FLOAT_BITS(a->attack_growth, PICK(float_bits(1.0f), float_bits(1.01f), float_bits(1.0100001f), float_bits(1.2f), float_bits(0.9f), float_bits(1.05f)));
    r->owner_kind = (uint8_t)PICK(0, 0, 0, 0, 1, 0xFF);
    r->owner_table = (uint8_t)PICK(0, 0, 0, 1);
    r->uses = (uint8_t)PICK(0, 0, 1, 0x10, 0xF0);
    r->kind = (uint8_t)PICK(0xA, 0xA, 0xB, 0x11, 0x11, 2, 0, 3, 0x8A);
    r->sound_choice = (uint8_t)PICK(0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD, 0xFF);
    r->damage_percent = (uint8_t)PICK(0, 0, 1, 50, 127, 128, 255);
    r->damage_bits = (uint8_t)PICK(0, 1, 1, 2, 3, 8, 9, 0xFF);
    r->drain_amount = (uint8_t)PICK(0, 0, 1, 5, 127, 128, 255);
    SB(HIT_RECORD + 0x31, PICK(0, 0, 0, 1, 2, 0xFF));
    SB(HIT_RECORD + 0x32, PICK(0, 0, 1, 2));
    r->buildup_mask = (uint16_t)(rnd() % 2 ? rnd() : PICK(0x10, 0x20, 0x40, 0x80, 0x100, 0));
    r->damage = (uint8_t)PICK(0, 1, 2, 3, 10, 50, 100, 127, 128, 255);
    r->direct_buildup = (uint8_t)PICK(0, 1, 10, 30, 127, 128, 255);
    /* the tables: floats where floats are read, numbers where they aren't */
    for (uint32_t o = 0x09CF97B8u; o < 0x09CF9870u; o += 4)
        SW(o, float_bits((float)(rnd() % 256) / 128.0f));
    for (uint32_t o = 0x09CF9870u; o < 0x09CF9A00u; o += 4)
        SW(o, rnd() % 4 ? rnd() : 0);
    SW(LW(0x08A62D9Cu) + 0x28, PICK(0, 0x40000000, ~0x40000000, -1));
    if (rnd() % 4 == 0) {       /* numbers for which the order of attack, damage and scale shows in the result */
        for (int tries = 0; tries < 500; tries++) {
            float attack = 1.0f + (float)(rnd() % 0x100000) / 0x40000p0f, scale = 0.5f + (float)(rnd() % 0x100000) / 0x100000p0f, damage = (float)(1 + rnd() % 255);
            if (rt_trunc_w(attack * (damage * scale)) == rt_trunc_w((attack * damage) * scale))
                continue;
            SET_FLOAT_BITS(a->attack_multiplier, float_bits(attack));
            r->damage = (uint8_t)damage;
            SW(0x09CF97B8u + a->sharpness_level * 16u, float_bits(1.0f));
            SW(0x09CF9838u + a->sharpness_level * 4u, float_bits(scale));
            SB(HIT_RECORD + 0x31, 0xFF);
            a->attack_percent_a = a->attack_percent_b = 100;
            for (uint32_t o = 0; o < 0xC0; o++)
                SB(HIT_ZONES + o, 100);
            r->damage_percent = 0;
            r->kind = 2;
            r->owner_table = 0;
            r->owner_kind = 0;
            t->status_hold_mode = 0;
            t->last_hit_record = 0;
            break;
        }
    }
    test_a0 = HIT_RECORD;
    args[0] = rnd() % 16 ? HIT_ATTACKER : 0;
    if (args[0] == 0)       /* a record has an owner, or says it has none; otherwise the original reads through 0 */
        r->owner_kind = 0xFF;
    args[1] = HIT_TARGET_A;
    args[2] = HIT_SHAPES;
    test_t0 = HIT_PARTS;
    test_t1 = HIT_CONTACT_AT;
}

/* -- weapon class 0's moves ---------------------------------------------------------------------------------------------------------------- */
static const Bias weapon0_bias[] = {
    {0x08864408u, 6, {0, 0, 1, 1, 0x100, 0x101}},       /* a frame reached */
    {0x08864348u, 4, {0, 1, 1, 0x100}},
    {0x09A8B058u, 5, {0, 1, 1, 1, 0x100}},
    {0x09BA2E28u, 6, {1, 2, 4, 0xFF, 0x101, 0x100}},
    {0x09BA32E8u, 4, {0, 1, 1, 0x101}},
    {0x09A8B790u, 4, {0, 1, 0x100, 0x101}},
    {0x088D6A1Cu, 4, {0, 1, 0x100, 0x101}},
    {0x088D1BA8u, 5, {0, 1, 2, 3, 0x103}},
    {0x09A8B650u, 4, {0, 0, 1, 0x100}},
    {0, 0, {0}},
};
POKES(weapon0_pokes, weapon0_bias, {0x1E6, 2, 0}, {0x28, 1, 1}, {0x60E, 1, 0}, {0x672, 2, 0}, {0x414, 4, 0}, {0x1124, 4, 0}, {0x1149, 1, 0}, {0x484, 4, 0}, {0x55A, 2, 0}, {0x1D5, 1, 0}, {0x1D6, 1, 0}, {0x1D7, 1, 0}, {0xBC, 2, 0}, {0x33D, 1, 0}, {0x558, 1, 0}, {0x410, 4, 0}, {0x614, 4, 0}, {0x1F4, 4, 0}, {0x1120, 4, 0}, {0x397, 1, 0}, {0x111C, 4, 0}, {0x299, 1, 0},
      {0x200, 4, 0}, {0x204, 4, 0}, {0x208, 4, 0}, {0x324, 2, 0}, {0x90, 4, 0});

static void inputs_weapon0(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    compare_vfpu = !!record_method;
    self->state = (uint8_t)(rnd() % 0x22);
    self->selector_input_flags = (uint8_t)rnd();
    self->state_step = (uint8_t)PICK(0, 0, 1, 1, 1, 2, 3, 3, 4, 5, 5, 6, 7, 8, 0xFF);
    self->state_scratch_a = (uint8_t)PICK(0, 0, 1, 2, 3);
    self->state_scratch_b = (uint8_t)PICK(0, 0, 1, 2);
    self->attack_connected = (uint8_t)PICK(0, 0, 1, 0x80);
    self->motion_part0_flags = (uint16_t)PICK(0, 1, 1, 2, 3, 0x100, 0x101);
    self->motion_slow_timer = (uint8_t)PICK(0, 0, 1, 0x80);
    self->hunter_flags = rnd() & rnd();
    self->misc_flags = rnd();
    self->quest_flags = rnd();
    self->button_action = PICK(0, 1, 1, 2, 2, 3, 3, 4, -1, 0x100, 0x101, 0x102, 0x10003);
    self->facing = rnd();
    self->facing_target = (uint16_t)rnd();
    self->attack_connected_reaction = (uint8_t)PICK(0, 1, 2, 3, 4, 0x80);
    self->motion_id = (uint16_t)rnd();
    self->freeze_ticks = (uint8_t)rnd();
    self->freeze_ticks_pending = (uint8_t)rnd();
    self->sound_cue_mute_timer = (uint8_t)rnd();
    self->reaction_block_weapon_ticks = (int16_t)rnd();
    SW(hunter + 0x3F4, rnd());
    SET_FLOAT_BITS(self->motion_part0_frame, float_bits((float)(rnd() % 256) / 2.0f));
    self->instance_index = rnd() % 8 ? ((GameState *)MEM(LW(GAME_POINTER)))->local_hunter_slot : pick_slot(c);
    self->charge_tier = (int8_t)PICK(0, 0, 1, 2, 3, 3, 4, -1);
    self->charge_time = (int16_t)PICK(0, 1, 29, 30, 37, 38, 59, 60, 79, 80, 100, 36, 24, 25, 71, 72, 95, 96, -1, 0x7FFF);
    self->action_count = (int32_t)PICK(90, 2, 1, 0, -1, 72, 108, 0x10001);
    self->button_action_hold = PICK(2, 2, 2, 0, 1, 3, 0x102);
    self->weapon_net_flag = (int8_t)PICK(0, 1, 1, 2, -1);
    self->buttons_pressed = PICK(0, 0, 1, 0x4000, 0x4001, 3, 0xFFFF0000);
    self->stamina = (int16_t)PICK(74, 75, 76, 300, -1);
    self->attack_percent_a = (uint8_t)rnd();
    SET_FLOAT_BITS(self->motion_speed_scale, rnd());
    for (int i = 0; i < 6; i++)     /* for the real callee */
        SB(hunter + 0x676 + i, PICK(0, 0, 0, 0xD7, 0xD8));
    if (record_method) {        /* rotations and places that show which is used */
        for (uint32_t o = 0x10; o < 0x40; o += 4)
            SW(hunter + o, float_bits((float)(rnd() % 256) / 64.0f - 2.0f));
        for (uint32_t o = 0x200; o < 0x20C; o += 4)
            SW(hunter + o, float_bits((float)(rnd() % 4096) / 8.0f - 256.0f));
    }
    args[0] = PICK(0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 6, 7, 0x100, 0x102);
    args[2] = PICK(0, 1, 0x10000, 0xFFFF0001);      /* none of them reads a3 */
    (void)c;
}

/* -- the monster script: bytes made up, each opcode a test or handler, and stubs that go on through them ---------------------------------- */
#define SCRIPT 0x09FFF400u
#define GAME_ADDR_T(p) ((uint32_t)((const uint8_t *)(p) - c->mem))
#define SCRIPT_SIZE 0x400u

static uint32_t script_at(uint32_t at) { return SCRIPT + ((at - SCRIPT) % SCRIPT_SIZE); }
#define FAKE_HUNTERS 0x09FFA000u
#define ID_LIST 0x09FFE000u
#define LINK_AREA 0x09FFE800u    /* what the area table gives: records of a link, each with its points */
#define LINK_RECORDS (LINK_AREA + 0x20u)
#define LINK_POINTS_AT (LINK_AREA + 0x200u)
static uint32_t script_step_two_at;
static uint32_t script_op;      /* the opcode a case is for: what its closing bytes are made for */
static int script_stuck;        /* what goes round and round until something ends it: MAX_STEPS */
static int script_else_then_end;        /* a skip lands on an else whose next instruction is the end: only here does a step go by two */

/* What a handler or a skip returns: somewhere in the bytes; a skip, as a real one does, the next place with its opcode */
static void script_stub_hook(Ctx *c, uint32_t fn, int n) {
    switch (fn) {
    case 0x09AB6778u: case 0x09AB6770u: case 0x09AC9740u: case 0x09AC9BC8u: case 0x09AC8B78u: case 0x09ABAFF0u: case 0x09AB74D0u: case 0x09AC4518u: case 0x09AC8AF0u:
        break;
    case 0x09AB7858u:
        if (c->r[5] == script_step_two_at) {
            c->r[2] = c->r[5] + 2;
            break;
        }
        if (script_else_then_end && c->r[5] >= SCRIPT + 0x390 && c->r[5] < SCRIPT + 0x3C0) {
            c->r[2] = c->r[5] + 2;
            break;
        }
        c->r[2] = script_stuck ? c->r[5] : script_at(c->r[5] + 1);      /* byte by byte: going by more, it may step over what ends a skip, for ever */
        break;
    case 0x09ACC450u:           /* its type in the group or not */
        c->r[2] = (stub_short[n][1] & 3) == 0 ? 0x101u : 0x100u;
        break;
    case 0x09AD1CB8u:           /* an area's table; only one handler can go on without one */
        c->r[2] = script_op == 0xF && (stub_short[n][1] & 7) == 0 ? 0 : LINK_AREA;
        break;
    case 0x09AC8BF0u: {         /* an angle: sometimes just off its facing by a limit the cases use: 90 degrees or one less, or 14 where rounding tells */
        static const uint32_t offs[4] = {0x4000u, 16383u, 2548u, 0x4000u};
        c->r[2] = (stub_short[n][1] & 1) == 0 ? ((const Monster *)MEM(c->r[4]))->facing + offs[(stub_short[n][1] >> 1) & 3] : (uint32_t)(int32_t)stub_short[n][0];
        break;
    }
    case 0x09ABDA78u:           /* a list of ids, each a few ways. If the monster is in them all and it loops, that's for ever, in the game too: later they're elsewhere */
        c->r[2] = ncalls[current] > 200 ? ID_LIST + 0x80 : ID_LIST;
        break;
    case 0x09ABDAA8u:
        c->r[2] = ncalls[current] > 200 ? ID_LIST + 0x80 : ID_LIST + 0x40 + ((uint16_t)stub_short[n][0] & 0xE);
        break;
    case 0x088DCEBCu:           /* a hunter by slot: made up, so that its area and flag can be either way */
        c->r[2] = (stub_short[n][1] & 7) == 0 ? 0 : FAKE_HUNTERS + (c->r[5] & 3) * 0x400;
        break;
    case 0x09AC33F8u:           /* a list of point numbers: a null one would be read from */
        c->r[2] = SCRIPT + ((uint16_t)stub_short[n][0] & 0x1F);
        break;
    case 0x09AB9170u: {         /* the target's position, on the caller's stack */
        const Monster *self = (const Monster *)MEM(c->r[4]);
        int exact = stub_short[n][1] & 3;
        *(float *)MEM(c->r[5]) = self->place[0] + (exact == 0 ? (stub_short[n][0] & 1 ? 500.0f : 500.01f) : exact == 1 ? (stub_short[n][0] & 1 ? 1000.0f : 1000.01f) : (float)(stub_short[n][0] % 1500));
        *(float *)MEM(c->r[5] + 4) = (float)(stub_short[n][1] >> 4);
        *(float *)MEM(c->r[5] + 8) = self->place[2] + (exact < 2 ? 0.0f : (float)(stub_short[n][1] % 1000));
        break;
    }
    case 0x09ABAB90u:           /* a main entry: out of being stuck */
        if (script_stuck) {
            c->r[2] = SCRIPT + 0x3C0;
            break;
        }
        c->r[2] = (stub_short[n][1] & 0xF) == 0 ? 0 : SCRIPT + ((uint16_t)stub_short[n][0] & 0x1F);
        break;
    case 0x09AB7728u: {
        if (script_else_then_end && c->r[5] < SCRIPT + 0x390) {       /* from further on it goes on, or a loop over entries would come back here for ever */
            const uint8_t ops[] = {0xB, 0x39, 1, 0x80, 0xFF, (uint8_t)script_op};
            for (uint32_t i = 0; i < 6; i++)
                if (ops[i] == (uint8_t)c->r[6]) {
                    c->r[2] = SCRIPT + 0x390 + 6 * i;
                    return;
                }
        }
        if (script_op != 0 && (stub_short[n][1] & 0x1F) == 1) {        /* a skip can end on something else, the end of the script */
            c->r[2] = SCRIPT + 0x3E2;
            break;
        }
        uint32_t at = script_at(c->r[5] + 1);
        for (uint32_t i = 0; i < SCRIPT_SIZE && LBU(at) != (uint8_t)c->r[6]; i++)
            at = script_at(at + 1);
        c->r[2] = at;
        break;
    }
    default:
        if (script_stuck)
            c->r[2] = fn == 0x09AB7230u ? 0 : fn == 0x09AB7490u ? SCRIPT + 0x100 : c->r[5] - 1;
        else
            c->r[2] = (stub_short[n][1] & 0xF) == 0 ? 0 : SCRIPT + ((uint16_t)stub_short[n][0] & ((stub_short[n][1] & 0x10) ? 0x3FF : 0x1F));
        break;
    }
}

static const Bias script_bias[] = {
    {0x09AC9740u, 4, {0, 0, 1, 0x10000}},
    {0, 0, {0}},
};
POKES(script_pokes, script_bias, {0x298, 1, 0}, {0x4BB, 1, 0}, {0x4C1, 1, 0}, {0x4C4, 2, 0}, {0x4B9, 1, 0}, {0x6D8, 1, 0}, {0x322, 2, 0}, {0x500, 2, 0}, {0x638, 4, 0}, {0x6DB, 1, 0}, {0x6DE, 1, 0}, {0x4B5, 1, 0});

static void inputs_script(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)hunter;
    uint32_t monster = first_monster(c);
    Monster *self = (Monster *)MEM(monster);
    stub_hunter = test_a0 = monster;
    if (!record_method) {       /* the real script of the real monster */
        self->state_group = 0;
        self->script_active_requests = 0;
        self->script_requests = 0;
        args[0] = self->script_position;
        args[1] = PICK(0xB, 0x39, 1, 0x80);
        args[2] = PICK(0, 2, 0xFF);
        if (args[0] == 0)
            test_a0 = monster;
        return;
    }
    self->state_group = (uint8_t)PICK(0, 0, 0, 1, 2, 4, 5, 6);
    self->script_requests = (uint8_t)(rnd() % 2 ? 0 : rnd());
    self->script_active_requests = (uint8_t)(rnd() % 2 ? 0 : rnd());
    self->end_vfunc_ec_ticks = (int16_t)PICK(0, 0, 1, -1, 0x100);
    self->alert_mode = (uint8_t)PICK(0, 1, 2);
    self->alert_mode_copy = (uint8_t)rnd();
    self->hunters_in_range_mask = (uint8_t)PICK(0, 0, 1, 2, 4, 8, 0x10, 0xF0, 0xFF);
    self->script_random_counter = (uint16_t)rnd();
    self->script_main_entry = (uint16_t)(rnd() % 4);
    self->monster_flags = rnd() % 2 ? PICK(0, 0x10, ~0x10) : rnd();
    self->locally_owned = (uint8_t)PICK(0, 1, 1);
    self->script_restart_sync_kind = (uint8_t)PICK(0, 0, 1, 2, 3);
    self->script_run_requested = (uint8_t)rnd();
    self->script_nest_level = (uint8_t)rnd();
    self->script_delay_ticks = (int16_t)rnd();
    self->point_block_index = (int8_t)rnd();
    self->point_block_goal = (int8_t)rnd();
    self->id_sequence_length = (int8_t)rnd();
    self->id_sequence_position = (int8_t)rnd();
    self->route_final_area = (uint16_t)rnd();
    self->previous_area = (uint16_t)rnd();
    self->script_suspend_arg = (uint8_t)rnd();
    self->script_position = rnd();
    self->script_position_request_20 = rnd();
    self->script_position_request_10 = rnd();
    self->script_position_point_block = rnd();
    self->script_position_request_08 = rnd();
    self->script_position_request_04 = rnd();
    self->script_position_request_02 = rnd();
    stub_hook = script_stub_hook;
    for (int n = 0; n < MAX_CALLS; n++) {
        stub_short[n][0] = (int16_t)rnd();
        stub_short[n][1] = (int16_t)rnd();
    }
    self->script_header = rnd() % 8 ? 0x09FFF3C0u : 0;
    SW(0x09FFF3C0u, 0x09FFF3D0u);
    for (uint32_t i = 0; i < 4; i++)
        SW(0x09FFF3D0u + 4 * i, SCRIPT + 0x10 * i);
    for (uint32_t i = 0; i < SCRIPT_SIZE; i++)
        SB(SCRIPT + i, rnd() % 2 ? (int32_t)PICK(0xB, 0x39, 1, 0x80, 0, 0, 1, 2, 2, 0xFF) : (int32_t)rnd());
    for (uint32_t i = 0; i < 32; i++)       /* in the first bytes, each thing the run tells apart */
        SB(SCRIPT + i, rnd() % 2 ? (int32_t)(uint8_t)rnd() : (int32_t)PICK(5, 1, 7, 0xB, 0x80, 0x7B, 0x84, 0x31, 0x92, 4, 0x23, 0x37, 0x72, 0x6A, 0xFF, 0x95));
    if (rnd() % 2)
        SB(SCRIPT + rnd() % 16, 5);
    /* Blocks that open seldom, so that nesting ends; and each opcode's closing pairs, so that a skip finds them. No 0x95 but the one placed below:
     * inside a block it ends every skip where it stands, for ever (in the game too, if a script had an unclosed block) */
    for (uint32_t i = 0; i + 1 < SCRIPT_SIZE; i++) {
        if (LBU(SCRIPT + i + 1) == 0 && rnd() % 8)
            SB(SCRIPT + i + 1, 3);
        if (LBU(SCRIPT + i) == 0x95)
            SB(SCRIPT + i, 0x94);
    }
    const uint8_t blocks[] = {0xB, 0x39, 1, 0x80, 0xFF, (uint8_t)script_op};
    for (uint32_t i = 0; i < 6; i++) {
        SB(SCRIPT + 0x3C0 + 6 * i, blocks[i]);
        SB(SCRIPT + 0x3C0 + 6 * i + 1, 2);
        SB(SCRIPT + 0x3C0 + 6 * i + 2, blocks[i]);
        SB(SCRIPT + 0x3C0 + 6 * i + 3, 1);
        SB(SCRIPT + 0x3C0 + 6 * i + 4, blocks[i]);
        SB(SCRIPT + 0x3C0 + 6 * i + 5, 0xFF);
        SB(SCRIPT + 0x390 + 6 * i, blocks[i]);
        SB(SCRIPT + 0x390 + 6 * i + 1, 1);
        SB(SCRIPT + 0x390 + 6 * i + 2, blocks[i]);
        SB(SCRIPT + 0x390 + 6 * i + 3, 2);
        SB(SCRIPT + 0x390 + 6 * i + 4, 0x97);
        SB(SCRIPT + 0x390 + 6 * i + 5, 0x97);
    }
    /* what ends a switch on sub-code 3 */
    {   /* spaced: a stub looks from the byte after where it's given */
        const uint8_t closing[] = {2, 1, 3, 0xFF};
        for (uint32_t i = 0; i < 4; i++) {
            SB(SCRIPT + 0x3E0 + 4 * i, script_op);
            SB(SCRIPT + 0x3E1 + 4 * i, closing[i]);
            SB(SCRIPT + 0x3E2 + 4 * i, 0x97);
            SB(SCRIPT + 0x3E3 + 4 * i, 0x97);
        }
    }
    script_stuck = rnd() % 32 == 0;
    SB(SCRIPT + 0x100, 1);
    uint32_t at = SCRIPT + rnd() % 16;
    SB(at, PICK(0, 0, 0, 1, 2, 3, 0xD, 0xE, 0xFF, 0x95));
    SB(at + 1, PICK(0, 1, 2, 3, 5, 8, 0xFF));
    SB(at + 2, PICK(0, 1, 2, 0x20, 0xFF));
    args[0] = at;
    args[1] = PICK(0xB, 0x39, 1, 0x80, 0x1FF, 0x10B);
    args[2] = PICK(2, 2, 0xFF, 1, 0x102);
    script_else_then_end = rnd() % 4 == 0 && !script_stuck && (uint8_t)args[2] == 2;
    if (script_stuck) {         /* where it stands isn't what it's looking for, and it goes on not being */
        SB(at, 0x96);
        args[1] = 0xB;
        args[2] = 2;
        for (uint32_t i = 0; i < 16; i++)
            if (LBU(SCRIPT + i) == 0xB)
                SB(SCRIPT + i, 0x97);
    }
}

/* Every opcode with every byte after it, stubs or not: a wrong length would misread every script from there on, and random bytes may not find it. */
static int script_every_length(Ctx *c, int stub_mode, uint32_t monster) {
    int failed = 0;
    for (uint32_t op = 0; op < 256; op++)
        for (uint32_t sub = 0; sub < 256; sub++) {
            uint32_t v0[2];
            SB(SCRIPT, op);
            SB(SCRIPT + 1, sub);
            SB(SCRIPT + 2, rnd());
            SB(SCRIPT + 3, rnd());
            ncalls[0] = ncalls[1] = 0;
            for (int mode = 0; mode < 2; mode++) {
                c->r[29] = TEST_SP;
                c->r[4] = monster;
                c->r[5] = SCRIPT;
                rt_hand = mode;
                current = mode;
                target = stub_mode ? 0x09AB7858u : 0;
                stubbing = stub_mode;
                running = 1;
                f_game_task_09AB7858(c);
                running = stubbing = 0;
                v0[mode] = c->r[2];
            }
            if (v0[0] != v0[1] || ncalls[0] != ncalls[1]) {
                printf("opcode %02X then %02X: generated %d bytes, %d calls; hand %d bytes, %d calls\n", op, sub, (int)(v0[0] - SCRIPT), ncalls[0], (int)(v0[1] - SCRIPT), ncalls[1]);
                failed++;
            }
        }
    return failed;
}

/* The fields handlers read, each a few ways. A real run keeps what's real but for the state of what it tests. */
static void script_op_real_fields(Ctx *c, Monster *self) {
    (void)c;
    self->point_block_index = (int8_t)PICK(-1, 0, 1, 5);
    self->script_nest_level = (uint8_t)PICK(0, 1, 2);
    self->id_sequence_length = -1;      /* its list wouldn't be there */
    self->target_param2 = (uint16_t)PICK(0, 1, 2, 3, 0xFF);
    self->target_kind = 1;
    self->target_param1 = 0;
}

static void script_op_stub_fields(Ctx *c, Monster *self, uint32_t at) {
    self->state = (uint8_t)PICK(1, 2, 3);
    self->state_group = (uint8_t)PICK(0, 1, 2, 3);
    SB(at + 1, PICK(0, 1, 2, 3, 5, 8));
    SB(at + 2, PICK(0, 1, 2, 3, 0xFF));
    self->chosen_slot = (uint8_t)PICK(0xFF, 0, 1, 2, 3, 0x13);
    self->resolved_target_kind = (uint16_t)PICK(1, 2, 3, 9, 0xD, 0xD, 0x10D);
    self->target_kind = (uint16_t)rnd();
    self->target_param1 = (uint16_t)rnd();
    self->target_param2 = (uint16_t)rnd();
    for (uint32_t i = 0; i < 4; i++) {
        { const float v[] = {-1.0f, 0.0f, 0.01f, 10.0f, 100.0f, 200.0f, 300.0f, 500.0f}; (&self->script_distance_band0)[i] = v[rnd() % 8]; }
        { const float v[] = {-2.0f, 0.0f, 0.005f, 10.0f, 100.0f, 150.0f, 200.0f, 1000.0f}; (&self->distance_to_hunter0)[i] = v[rnd() % 8]; }
    }
    self->distance_to_chosen_slot = (float)rnd();
    uint32_t monsters = LW(MONSTERS_POINTER);
    uint32_t first = 0;
    while (LW(monsters + 0x1220u + 4 * first) == 0)
        first++;
    for (uint32_t i = first + 1; i < 20; i++)
        SW(monsters + 0x1220u + 4 * i, PICK(0, 0, GAME_ADDR_T(self), FAKE_HUNTERS + (i & 3) * 0x400));
    self->area_id = (uint16_t)PICK(0, 1, 2, 3, 0xFFFF);
    SB(at + 3, PICK(0, 1, 2));
    SB(at + 4, PICK(0, 1, 3));
    for (uint32_t id = 0; id < 8; id++) {
        SH(0x09CE5108u + id * 2, (uint16_t)PICK(0, 0, 1));
        SH(0x09CE5320u + id * 2, (uint16_t)PICK(0xFFFF, 0, 1, 2, 3));
    }
    self->hunters_tracked_mask = (uint8_t)PICK(0, 0, 1, 2, 3, 4, 8, 0xC, 0xF, 0x10, 0xF0);
    self->script_table_24_entry = (uint8_t)rnd();
    self->script_level1_entry = (uint16_t)rnd();
    self->script_level2_entry = (uint16_t)rnd();
    self->script_return_table_24 = rnd();
    self->script_return_level1 = rnd();
    self->script_return_level2 = rnd();
    self->script_nest_level = (uint8_t)PICK(0, 1, 2, 3);
    self->script_active_requests = (uint8_t)rnd();
    self->request_bit_8_ticks = (int8_t)rnd();
    self->route_final_area = (uint16_t)rnd();
    self->point_block_order = (uint8_t)PICK(0, 0, 1, 2, 3);
    self->point_block_order_list = (uint8_t)rnd();
    self->point_block_point_count = (int8_t)PICK(-1, 0, 1, 2, 3, 6, 10);
    self->point_block_index = (int8_t)PICK(-2, -1, 0, 1, 2, 5, 10);
    self->spawn_variant = (int16_t)PICK(-1, 0, 1, 2, 3, 0x100);
    self->script_event_active = (uint8_t)PICK(0, 1, 2);
    self->state = (uint8_t)PICK(1, 2, 3);
    for (uint32_t i = 3; i < 10; i++)
        SB(at + i, PICK(0, 1, 2, 3, 0x80));
    SB(at + 1, PICK(0, 1, 2, 3, 5));
    for (uint32_t i = 0; i < 4; i++) {
        SH(FAKE_HUNTERS + i * 0x400 + offsetof(Monster, area_id), rnd() % 4 ? self->area_id : (uint16_t)rnd());
        SW(FAKE_HUNTERS + i * 0x400 + 4, PICK(0, 1, 1, 1, 3));
        (&self->target_points_hunter0)[i] = (int32_t)PICK(0, 0, 0, 1, 2, 29999, 30000, 49999, 50000, -1);
    }
    self->script_random_counter = (uint16_t)rnd();
    self->target_kind = (uint16_t)PICK(1, 1, 1, 1, 2, 0x101);
    self->target_param1 = (uint16_t)PICK(0, 0, 0, 0, 1, 0x100);
    self->target_param2 = (uint16_t)PICK(0, 1, 2, 3, 0xFF, 0x102);
    self->facing = rnd();
    SB(at + 2, PICK(1, 2, 3, 30, 45, 90, 90, 90, 120, 180, 0xFF, 0x80, 0xD3));
    SH(0x09CE5108u + 0xFFFFu * 2, 1);
    SH(0x09CE5320u + 0xFFFFu * 2, 1);
    SW(0x09FC0000u, rnd());
    SW(0x09FC0004u, rnd());
    SW(0x09FC0000u + 15000 * 4, rnd());
    if (script_op == 6)
        SB(at, PICK(0, 1, 2, 3, 4, 5, 6, 7, 0xA, 0xA, 0xD, 0xD));
    if (script_op == 0x3E && rnd() % 2) {
        SB(at + 4, 0xFF);
        SB(at + 5, 0xFF);
    }
    script_step_two_at = 0;
    if (script_op == 0x42 || script_op == 0x14) {       /* an else right after the test passes, and either way what's after that */
        if (rnd() % 2) {
            SB(at + 2, script_op);
            SB(at + 3, PICK(1, 1, 1, 2));
            SB(at + 4, PICK(script_op, script_op, 0x97));
            SB(at + 5, PICK(2, 1));
            script_step_two_at = at + 2;
        }
    }
    for (uint32_t i = 0; i < 4; i++)
        SW(FAKE_HUNTERS + i * 0x400 + offsetof(Monster, monster_flags), PICK(0, 2, 3));
    if (script_op == 0x53) {    /* several hunters to choose among, some just as far */
        self->hunters_tracked_mask = (uint8_t)PICK(3, 5, 7, 0xC, 0xF, 0xF, 0xF);
        for (uint32_t i = 0; i < 4; i++)
            (&self->distance_to_hunter0)[i] = rnd() % 2 ? 10.0f : 20.0f;
    }
    self->paired_monster = PICK(0, 0, 1, 0x09FFA000u);
    SW(GAME_ADDR_T(self) + offsetof(Monster, meter_b_max), PICK(0, 1, 3, 10, 100, 1000, -100));
    SW(GAME_ADDR_T(self) + offsetof(Monster, meter_b), PICK(0, 1, 2, 3, 29, 30, 31, 300, -1));
    self->id_sequence_list = (uint8_t)rnd();
    SB(GAME_ADDR_T(self) + 0x539, rnd());
    self->id_sequence_length = (int8_t)PICK(-1, 0, 1, 2, 3, 8);
    self->id_sequence_position = (int8_t)PICK(0, 0, 1, 2, 7);
    self->id_sequence_loops = (uint8_t)PICK(0, 0, 0, 1, 2, 0x80);
    self->script_sequence_loop_start = rnd();
    for (uint32_t i = 0; i < 16; i++)
        SH(ID_LIST + 2 * i, (uint16_t)(i == 15 ? self->area_id + 1 : PICK(self->area_id, self->area_id, 0xFFFF, 1, 2, 50000, 50001, 49999, 65000)));
    for (uint32_t i = 0; i < 8; i++)
        SH(ID_LIST + 0x40 + 2 * i, (uint16_t)PICK(self->area_id, 1, 2, 0xFFFF));
    for (uint32_t i = 0; i < 128; i++)
        SH(ID_LIST + 0x80 + 2 * i, self->area_id == 0xFFFF ? 1 : 0xFFFF);
    SW(0x09FFD000u + 0x1C, 0x09FC0000u);     /* far enough down that an entry up to 15535 is in memory */
    SH(0x09CE4908u + LW(LW(0x09A4F1A0u) + 0x284) * 2, (uint16_t)PICK(0xFFFF, 0, 1));
    uint32_t mode_byte = LBU(LW(0x089CC558u) + 0x6AF10u);
    SH(0x09CE4908u + mode_byte * 2, (uint16_t)PICK(0xFFFF, 9, 0xA, 0x16, 0x109, 0));
    SB(LW(0x089CC558u) + 0x6AF10u, PICK(mode_byte, 9, 0xA, 0x16));
    self->monster_flags = rnd();
    self->movement_mode = (uint8_t)PICK(0, 0, 1, 2, 4, 5);
    self->alert_area_table_ticks = (int16_t)PICK(-1, 0, 1, 100);
    self->area_table_ticks = (int16_t)PICK(-1, 0, 1, 100);
    self->previous_area = (uint16_t)PICK(0, 1, 2, 3, 0xFFFF, 0xFFFF);
    SB(GAME_ADDR_T(self) + 0x292, PICK(0, 1, 2, 3));
    self->quest_scale_row = (int8_t)PICK(-1, 0, 1, 2, 3);
    self->script_position_point_block = rnd();
    SW(MONSTER_TYPE(self->subtype, 0x158), LINK_AREA + 0x700);
    SW(LINK_AREA + 0x700 + 4, rnd());
    SH(LINK_AREA + 2, (uint16_t)PICK(-1, 0, 1, 2, 3, 5));
    self->script_header = 0x09FFD000u;       /* its tables: anything there, it's only what's returned */
    if (script_op == 0x42 || script_op == 0x14)         /* the limit, in degrees */
        SB(at + 1, PICK(0, 1, 14, 14, 30, 45, 90, 90, 90, 120, 180, 0xD3, 0x80, 0xFF));
    SW(self->script_header + 4, SCRIPT + 0x40);
    for (uint32_t i = 0; i < 0x100; i++)
        SW(self->script_header + 0x3C + 4 * i, SCRIPT + 0x100 + 4 * i);
    (void)c;
}

/* A handler of an opcode: its sub-codes, and the fields the handlers go by */
static void inputs_script_op(Ctx *c, uint32_t hunter, uint32_t args[3], uint32_t op) {
    script_op = op;
    script_step_two_at = 0;
    inputs_script(c, hunter, args);
    uint32_t monster = first_monster(c);
    Monster *self = (Monster *)MEM(monster);
    if (!record_method) {       /* with the real callees: a block they can walk, for they would look for its end for ever otherwise */
        uint32_t at = SCRIPT + 0x10;
        SB(SCRIPT, 0x13);
        SB(at, PICK(0, 0, 0, 1, 2, 3, 4, 5, 0xFF, 0x20));
        for (uint32_t i = 1; i < 8; i++)        /* not the opcode: "op 3" stops a search for the end, for ever */
            SB(at + i, i == 1 ? PICK(1, 2, 3) : op == 3 || op == 4 ? 0x13 : PICK(1, 2, 3, 4, 0x13));
        if (op == 1 || op == 2 || op == 3)
            for (uint32_t i = 1; i < 8; i++)
                SB(at + i, 0x13);
        for (uint32_t i = 8; i < 0x20; i++)
            SB(at + i, 0x13);
        /* An else being skipped from goes to its end: another else first would stop the search for ever */
        const uint8_t closing[] = {LBU(at) == 1 ? 2 : 1, 2, 3, 0xFF, 1, 2, 3, 0xFF};
        for (uint32_t i = 0x20; i < 0x80; i++)  /* with room between: a case's value taken from one wouldn't step over the next */
            SB(at + i, 0x13);
        for (uint32_t i = 0; i < 8; i++) {
            SB(at + 0x20 + 6 * i, op);
            SB(at + 0x21 + 6 * i, closing[i]);
        }
        if (op == 0x82) {       /* a table and entry that are there */
            uint32_t table = 0;
            while (table < 32 && LW(self->script_header + 0x3C + 4 * table) == 0)
                table++;
            SB(at, table);
            SB(at + 1, 0);
        }
        args[0] = at;
        script_op_real_fields(c, self);
        return;
    }
    script_op_stub_fields(c, self, args[0]);
}

/* Opcode FF: each sub-code, a link's records and points close enough for each way of a test, the fields it goes by */
static void inputs_script_end_block(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    script_op = 0;
    script_step_two_at = 0;
    inputs_script(c, hunter, args);
    uint32_t monster = first_monster(c);
    Monster *self = (Monster *)MEM(monster);
    uint32_t sub = PICK(0, 1, 2, 3, 4, 4, 4, 5, 5, 6, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFE, 0xFF);
    if (!record_method) {       /* with the real callees: only where a real table is there to find */
        self->script_active_requests = (uint8_t)PICK(0, 1);
        sub = PICK(1, 2, 3, 0xF8, 0xF9, 0xFB, 0xFC, 0xFD, 0xFF, 0xF4, 0, 0xF5, 0xF6, 0xF7, 5, 4, 0xFE);
        if ((sub == 4 || sub == 5) && LW(MONSTER_TYPE(self->subtype, 0x158)) == 0)
            sub = 1;
        SB(SCRIPT, sub);
        args[0] = SCRIPT;
        return;
    }
    SB(args[0], sub);
    self->subtype = (uint8_t)PICK(0, 1, 2, 7, 0x24, 0x32, 0x37, 0x47, 0x3A, 0x48, 3, 6, 8, 0x23, 0x25, 0x31, 0x33, 0x36, 0x38, 0x46, rnd() & 0x4F);
    self->area_id = (uint16_t)(rnd() % 64);
    self->place[0] = (float)((int)(rnd() % 2048) - 1024);
    self->place[1] = (float)((int)(rnd() % 256) - 128);
    self->place[2] = (float)((int)(rnd() % 2048) - 1024);
    self->place_before[0] = (float)rnd();
    self->facing = rnd();
    self->previous_area = (uint16_t)rnd();
    self->link_record_index = (uint8_t)(rnd() % 8);
    self->link_point_index = (uint8_t)PICK(0, 0, 1, 2, 3, 7);
    self->link_retry_count = (int8_t)PICK(0, 1, 8, 9, 10, -1, 0x7F);
    self->link_script_entry = (uint8_t)rnd();
    self->target_kind = (uint16_t)rnd();
    self->target_param1 = (uint16_t)rnd();
    self->target_param2 = (uint16_t)rnd();
    self->resolved_target_kind = (uint16_t)PICK(9, 9, 9, 1, 0, 0x109);
    self->resolved_target_param1 = (uint16_t)(rnd() % 6);
    self->resolved_target_param2 = (uint16_t)rnd();
    self->script_return_link = rnd();
    self->script_return_level1 = rnd();
    self->script_return_level2 = rnd();
    self->script_return_table_24 = rnd();
    self->script_sequence_loop_start = rnd();
    self->id_sequence_position = (int8_t)PICK(-1, 0, 1, 2, 3, 0x7F);
    self->id_sequence_length = (int8_t)PICK(-1, 0, 1, 2, 3, 4);
    self->id_sequence_loops = (uint8_t)PICK(0, 0, 1, 2, 0x80);
    self->chosen_slot = (uint8_t)PICK(0xFF, 0, 1, 2, 3, 0x13);
    self->hunters_tracked_mask = (uint8_t)rnd();
    for (uint32_t i = 0; i < 4; i++) {
        SH(monster + offsetof(Monster, tracked_ticks_hunter0) + 2 * i, (uint16_t)rnd());
        SW(monster + offsetof(Monster, approach_points_hunter0) + 4 * i, rnd());
        SW(monster + offsetof(Monster, target_points_hunter0) + 4 * i, rnd());
    }
    self->point_block_start_entry = (uint8_t)rnd();
    self->point_block_step_entry = (uint8_t)rnd();
    self->point_block_order = (uint8_t)PICK(0, 0, 1, 2, 3);
    self->point_block_order_list = (uint8_t)rnd();
    self->point_block_visited = (int8_t)PICK(-1, 0, 1, 2, 3);
    self->point_block_goal = (int8_t)PICK(-1, 0, 1, 2, 3);
    self->point_block_index = (int8_t)PICK(-1, 0, 1, 2, 5);
    self->point_block_point_count = (int8_t)PICK(-1, 0, 1, 2, 3, 6, 10);
    self->alert_mode_copy = (uint8_t)rnd();
    SW(MONSTER_TYPE(self->subtype, 0x158), LINK_AREA + 0x700);      /* its table of areas: not every type has one in memory */
    SW(LINK_AREA + 0x700 + 0x18, rnd());
    SH(0x09CE4908u + LW(LW(0x09A4F1A0u) + 0x284) * 2, (uint16_t)PICK(0xFFFF, 0, 1));
    for (uint32_t id = 0; id < 0x102; id++) {
        SH(0x09CE5108u + id * 2, (uint16_t)PICK(0, 1));
        SH(0x09CE5320u + id * 2, (uint16_t)PICK(0xFFFF, 0xFFFF, id + 1));
    }
    /* The area's records and each's points: */
    SH(LINK_AREA + 2, (uint16_t)PICK(1, 2, 3, 5));
    SW(LINK_AREA + 4, rnd() % 8 ? LINK_RECORDS : 0);
    for (uint32_t r = 0; r < 8; r++) {
        uint32_t record = LINK_RECORDS + r * 0x18;
        SH(record, (uint16_t)PICK(-1, 1, 2, 3, r, 0x10, 0x101));
        SH(record + 2, (uint16_t)PICK(1, 2, 4, r + 1));
        SW(record + 4, LINK_POINTS_AT + r * 0x80);
        for (uint32_t o = 8; o <= 0x10; o += 4)
            *(float *)MEM(record + o) = (float)(int16_t)rnd();
        SH(record + 0x14, (uint16_t)rnd());
        for (uint32_t i = 0; i < 6; i++) {
            uint32_t point = LINK_POINTS_AT + r * 0x80 + i * 0x14;
            int exact = rnd() % 2;
            float radius = (float)(rnd() % 1024);
            *(float *)MEM(point) = self->place[0] + (exact ? radius : (float)(rnd() % 2048) - 1024.0f);
            *(float *)MEM(point + 4) = self->place[1] + (exact ? 0.0f : (float)(rnd() % 512));
            *(float *)MEM(point + 8) = self->place[2] + (exact ? 0.0f : (float)(rnd() % 256));
            *(float *)MEM(point + 0xC) = radius;
            SH(point + 0x12, (uint16_t)rnd());
        }
    }
}

static void inputs_register_hit(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    Hunter *self = (Hunter *)MEM(hunter);
    GameState *g = (GameState *)MEM(LW(GAME_POINTER));
    hit_common(c, hunter);
    compare_vfpu = !!record_method;
    random_target(c, HIT_ATTACKER, 0);
    if (!record_method) {       /* the callees reach what a real attacker has */
        memcpy(MEM(HIT_ATTACKER), MEM(hunter), 0x800);
        ((HitRecord *)MEM(HIT_RECORD))->sound_choice = 0xFF;
    }
    self->instance_index = rnd() % 8 ? g->local_hunter_slot : pick_slot(c);
    SET_FLOAT_BITS(((Hunter *)MEM(HIT_ATTACKER))->attack_multiplier, FLOATS);
    SET_FLOAT_BITS(self->defense_for_damage, FLOATS);
    SET_FLOAT_BITS(self->resist_pct_hit_bit_04, FLOATS);
    SET_FLOAT_BITS(self->resist_pct_hit_bit_10, FLOATS);
    SET_FLOAT_BITS(self->resist_pct_hit_bit_40, FLOATS);
    SET_FLOAT_BITS(self->resist_pct_hit_bit_20, FLOATS);
    SET_FLOAT_BITS(self->resist_pct_hit_bit_80, FLOATS);
    if (rnd() % 2) {         /* numbers that aren't peculiar, where rounding shows; the record's too */
        ((HitRecord *)MEM(HIT_RECORD))->damage_percent = (uint8_t)(rnd() % 3 ? 0 : 1 + rnd() % 200);
        SET_FLOAT_BITS(((Hunter *)MEM(HIT_ATTACKER))->attack_multiplier, float_bits(1.0f + (float)(rnd() % 256) / 64.0f));
        SET_FLOAT_BITS(self->defense_for_damage, float_bits(1.0f + (float)(rnd() % 512) / 2.0f));
        SET_FLOAT_BITS(self->resist_pct_hit_bit_04, float_bits((float)(rnd() % 64) - 20.0f));
        SET_FLOAT_BITS(self->resist_pct_hit_bit_10, float_bits((float)(rnd() % 64) - 20.0f));
    }
    self->pending_damage_0 = (int16_t)PICK(0, 0, 1, -1, 32767, -32768, 100);
    self->pending_buildup_direct = (int16_t)PICK(0, 1, 100);
    self->hit_log_index = (uint8_t)PICK(0, 1, 14, 15, 15, 16, 0xFF);
    self->pending_hit_bits_0 = (uint8_t)rnd();
    self->pending_hit_flags = (uint8_t)rnd();
    SB(hunter + 0x286, rnd());      /* cleared */
    self->defense_bonus = (int8_t)PICK(0, 9, 10, 11, -1);
    self->attack_bonus = (int8_t)PICK(0, 2, 3, 4, -1);
    self->defense_halved_ticks = (int16_t)PICK(0, 0, 1, -1);
    self->item_restrict_ticks = (int16_t)PICK(0, 5);
    self->hit_type_16_1e_immunity_level = (uint8_t)PICK(0, 1, 2, 3, 0xFF);
    SW(HIT_CONTACT_AT, rnd());
    SW(HIT_CONTACT_AT + 4, rnd());
    SW(HIT_CONTACT_AT + 8, rnd());
    SW(HIT_CONTACT_AT + 12, rnd());
    test_a0 = HIT_RECORD;
    args[0] = HIT_ATTACKER;
    args[1] = hunter;
    args[2] = HIT_CONTACT_AT;
    ((HitRecord *)MEM(HIT_RECORD))->kind = (uint8_t)PICK(0xB, 0xB, 0xE, 0xF, 0x10, 0x15, 0x16, 0x19, 0x1A, 0x1E, 2, 3, 0x20, 0x80);
    if (record_method && rnd() % 4 == 0) {      /* numbers for which the order of the percent's multiply and divide shows in the result */
        HitRecord *r = (HitRecord *)MEM(HIT_RECORD);
        for (int tries = 0; tries < 500; tries++) {
            float attack = 1.0f + (float)(rnd() % 0x100000) / 0x40000p0f, record_damage = (float)(1 + rnd() % 255), percent = (float)(1 + rnd() % 255);
            float defense = 1.0f + (float)(rnd() % 512) / 2.0f, x = record_damage * attack * (percent / 100.0f), y = record_damage * attack * percent / 100.0f;
            x = x - x * defense / (defense + 80.0f);
            y = y - y * defense / (defense + 80.0f);
            if (rt_trunc_w(x) == rt_trunc_w(y))
                continue;
            SET_FLOAT_BITS(((Hunter *)MEM(HIT_ATTACKER))->attack_multiplier, float_bits(attack));
            SET_FLOAT_BITS(self->defense_for_damage, float_bits(defense));
            r->damage = (uint8_t)record_damage;
            r->damage_percent = (uint8_t)percent;
            r->damage_bits = 0;
            r->kind = 2;
            r->owner_table = 1;
            r->owner_kind = 0;
            break;
        }
    }
}

/* -- damage to a monster ------------------------------------------------------------------------------------------------------------------ */
#define DAMAGES PICK(0, 0, 0, 1, 2, -1, -2, 10, 100, 101, -100, 0x7FFF, -0x8000)

static const Bias apply_damage_bias[] = {
    {0x09AD8688u, 4, {0, 0, 1, 0x100}},
    {0x09AD8458u, 4, {0, 1, 1, 0x100}},
    {0x09AD8310u, 4, {0, 1, 1, 0x100}},
    {0x09AC85F8u, 16, {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 0x100}},
    {0x09C451F0u, 4, {0, 0, 1, 0x100}},
    {0, 0, {0}},
};
POKES(sum_damage_pokes, NULL, {0x638, 4, 0}, {0x4C0, 1, 0}, {0x38C, 2, 0}, {0x38E, 2, 0}, {0x390, 2, 0}, {0x392, 2, 0}, {0x394, 2, 0});
POKES(apply_damage_pokes, apply_damage_bias, {0x2E4, 2, 0}, {0x41E, 2, 0}, {0x384, 4, 0}, {0x3B8, 2, 0}, {0x3BA, 2, 0}, {0x3C6, 2, 0}, {0x3C8, 2, 0}, {0x4C0, 1, 0}, {0x4C2, 1, 0}, {0x1E8, 1, 0}, {0x3CF, 1, 0}, {0x3CA, 2, 0},
      {0x29A, 2, 0}, {0x5F0, 1, 0}, {0x522, 1, 0}, {0x638, 4, 0}, {0x285, 1, 0}, {0x298, 1, 0}, {0x4B5, 1, 0}, {0x577, 1, 0}, {0x580, 2, 0}, {0x582, 1, 0}, {0x5C0, 2, 0}, {0x286, 1, 0}, {0x6DD, 1, 0}, {0x4BA, 1, 0});

static void random_monster_damage(Ctx *c, uint32_t monster) {
    Monster *self = (Monster *)MEM(monster);
    for (int i = 0; i < 9; i++)
        SH(monster + 0x3B8 + 2u * (uint32_t)i, DAMAGES);
    for (int i = 0; i < 8; i++)
        SB(monster + 0x3B0 + (uint32_t)i, PICK(0, 1, 3, 0xFF));
    SET_FLOAT_BITS(self->damage_taken_scale, PICK(float_bits(1.0f), float_bits(0.5f), float_bits(0.001f), float_bits(1.5f), float_bits(0.3f), 0, FLOAT_NAN, FLOAT_INFINITY));
    if (rnd() % 2)
        SET_FLOAT_BITS(self->damage_taken_scale, float_bits((float)(rnd() % 4096) / 1024.0f));
    self->monster_flags = rnd() % 2 ? PICK(0, 0x20, 2, 0x1000, 0x2000) : rnd();
    self->cannot_die_ticks = (int8_t)PICK(0, 0, 0, 1, 5, -1);
    self->pending_buildup_mask1 = (int16_t)rnd();
    self->pending_buildup_mask2 = (int16_t)rnd();
    self->pending_buildup_mask4 = (int16_t)rnd();
    self->pending_buildup_mask8 = (int16_t)rnd();
    self->pending_buildup_direct = (int16_t)rnd();
    self->subtype = (uint8_t)PICK(2, 0x24, 0x47, 7, 0x32, 0x37, 0x4C, 0x58, 77, 35, 1);
    SB(MONSTER_TYPE_RECORD(self->subtype) + 0x14A, PICK(-1, -1, 0, 1, 7, 8));
    SB(MONSTER_TYPE_RECORD(self->subtype) + 0x14B, PICK(0, 1, 3, 0xFF));
    SH(MONSTER_TYPE_RECORD(self->subtype) + 0x148, rnd());
}

/* The sums on the stack are read back: 09AC8450, when it is a stub, leaves something there */
static void monster_stub_hook(Ctx *c, uint32_t fn, int n) {
    if (fn == 0x09AC8450u) {
        SH(c->r[5], LH(c->r[5]) + stub_short[n][0]);     /* adds to what they start at, as it does */
        SH(c->r[6], LH(c->r[6]) + stub_short[n][1]);
    }
}

static void inputs_sum_damage(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)hunter;
    uint32_t monster = first_monster(c);
    stub_hunter = test_a0 = monster;
    random_monster_damage(c, monster);
    SH(HIT_OUT, PICK(0, 0, 1, -1, 0x7FFF, 100));
    SH(HIT_OUT + 2, PICK(0, 0, 1, -1, 0x7FFF, 100));
    args[0] = HIT_OUT;
    args[1] = HIT_OUT + 2;
}

static void inputs_apply_damage(Ctx *c, uint32_t hunter, uint32_t args[3]) {
    (void)hunter;
    uint32_t monster = first_monster(c);
    Monster *self = (Monster *)MEM(monster);
    stub_hunter = test_a0 = monster;
    stub_hook = monster_stub_hook;
    random_monster_damage(c, monster);
    for (int n = 0; n < MAX_CALLS; n++) {
        stub_short[n][0] = (int16_t)PICK(0, 0, 1, 10, 100, 0x7FFF, -0x8000);
        stub_short[n][1] = (int16_t)PICK(0, 0, 0, 1, 10, -1);
    }
    self->monster_flags = rnd() % 2 ? PICK(0, 0, 0, 2, 0x1000, 0x2000) : rnd();
    self->damage_slot_mode = (uint8_t)PICK(1, 1, 8, 8, 9, 9, 0, 2, 0x89);
    self->state_group = (uint8_t)PICK(0, 0, 0, 0, 1, 4, 5);
    self->hit_pending = (uint8_t)PICK(2, 2, 2, 2, 1, 0, 0);
    self->health = (int16_t)PICK(100, 100, 1, 2, 999, 1000, 1001, 0, -1, 0x7FFF);
    self->health_max = (int16_t)PICK(100, 2000, 0x7FFF, 1, -1);
    for (int i = 0; i < 9; i++) {
        SH(monster + 0x580 + 8u * (uint32_t)i, PICK(1, 2, 10, 100, 3999, 4000, 4001, 0, -1, -0x8000));
        SB(monster + 0x582 + 8u * (uint32_t)i, PICK(0, 1, 13, 14, 15, 16, 0xFF));
    }
    if (rnd() % 2)      /* none near breaking */
        for (int i = 0; i < 9; i++)
            SH(monster + 0x580 + 8u * (uint32_t)i, 3000);
    self->slots_zeroed_total_mask = (uint8_t)rnd();
    self->status_hold_mode = (uint8_t)PICK(0, 0, 0, 1, 2, 3, 4);
    self->last_hit_kind = (uint16_t)PICK(0, 8, 8, 9, 9, 0xC, 0xC, 0x1A, 0x1E, 0x109, 0x10C, 0x108);
    self->held_marker_ticks_0 = (int8_t)PICK(0, 0, 0, 1, -1);
    self->status_mask = (uint8_t)rnd();
    self->area_id = (uint16_t)PICK(0xC, 0xC, 5, 5, 6, 0x10C);
    self->script_requests = (uint8_t)rnd();
    self->script_run_requested = (uint8_t)PICK(0, 1);
    self->hunters_engaged_mask = (uint8_t)PICK(0, 1);
    self->alert_mode = (uint8_t)PICK(0, 0, 1, 0x80);
    self->largest_damage_slot = (uint8_t)PICK(0, 1, 7, 8);
    uint32_t game = LW(GAME_POINTER);
    SH(game + 0x6AF0Eu, PICK(5, 5, 6));
    SB(game + 0x6AF10u, PICK(0, 1, 2, 3, 4, 5, 6, 7, 8));
    args[0] = HIT_OUT;
    SB(HIT_OUT, rnd());
    /* the table of quests to one subtype's rule */
    SH(0x09CE4908u + 2u * LBU(game + 0x6AF10u), PICK(0, 1, 2, 0xFFFF, 0x101));
    if (rnd() % 8 == 0) {   /* that rule, all the way */
        self->subtype = 0x37;
        self->area_id = 5;
        self->health = (int16_t)PICK(1, 999, 1000);
        self->cannot_die_ticks = 0;
        self->hit_pending = 2;
        self->state_group = 0;
        SB(game + 0x6AF10u, PICK(1, 2));
        SH(0x09CE4908u + 2u * LBU(game + 0x6AF10u), PICK(1, 2, 0xFFFF, 0xFFFF));
    }
    if (!record_method) {     /* callees running for real: a monster as it is, its state and its script left alone */
        self->damage_slot_mode = (uint8_t)PICK(1, 8, 9);
        self->state_group = 0;
        self->last_hit_kind = (uint16_t)PICK(0, 8, 0xC);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <RAM snapshot> [iterations] [stubs <signatures.tsv>] [only <part of a case's name>]\n", argv[0]);
        return 2;
    }
    int iterations = argc > 2 ? atoi(argv[2]) : 3000;
    int stub_mode = 0;
    const char *only = NULL;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "stubs")) {
            stub_mode = 1;
            load_signatures(argv[i + 1]);
        } else if (!strcmp(argv[i], "only")) {
            only = argv[i + 1];
        }
    }
    static Ctx ctx;
    Ctx *c = &ctx;
    c->mem = rt_mem_init();
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(c->mem + SNAP_START, 1, SNAP_SIZE, f) != SNAP_SIZE) {
        fprintf(stderr, "%s: can't read a snapshot\n", argv[1]);
        return 2;
    }
    fclose(f);
    uint32_t hunter = 0;
    for (uint32_t a = SNAP_START; a < SNAP_START + SNAP_SIZE; a += 4)
        if (LW(a) == HUNTER_VTABLE) {
            hunter = a;
            break;
        }
    if (!hunter) {
        fprintf(stderr, "no hunter in this snapshot\n");
        return 2;
    }
    stub_hunter = hunter;
    if (stub_mode)
        record_method = LW(LW(LW(LW(0x09A4F1A0u))) + 0x28);
    uint8_t *start = malloc(SNAP_SIZE), *generated = malloc(SNAP_SIZE);
    int failed = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        if (only && !strstr(cases[k].name, only))
            continue;
        /* Without stubs these two only run under script_run: given anything else, a real script may not hold what they look for, and they go on for ever */
        if (!stub_mode && (strstr(cases[k].name, "script_skip_to") || strstr(cases[k].name, "script_past_end")))
            continue;
        int bad = 0;
        if (cases[k].fn == f_game_task_09AB7858)
            bad += script_every_length(c, stub_mode, first_monster(c));
        for (int i = 0; i < iterations && bad < 5; i++) {
            uint32_t a1;
            int tick30;
            test_a0 = stub_hunter = hunter;
            hashed_more[0].size = hashed_more[1].size = 0;
            stub_hook = NULL;
            stack_pattern = compare_vfpu = compare_f0 = 0;
            test_f12 = 0;
            test_t0 = 0xBAD00008u;
            test_t1 = 0xBAD00009u;
            randomise(c, hunter, &a1, &tick30);
            /* how rarely a call does anything: a chain of 14 tests all answering 0 isn't reached otherwise */
            uint32_t quiet = 0;
            const struct Pokes *pokes = cases[k].pokes;
            pokable = NULL;
            stub_bias = NULL;
            if (cases[k].randomise_more)
                cases[k].randomise_more(c, hunter);
            if (pokes) {
                quiet = PICK(0, 4, 8, 16, 32, 128);
                pokable = pokes->fields;
                stub_bias = pokes->bias;
                npokes = PICK(0, 0, 1, 4, MAX_POKES);
            }
            if (cases[k].small_a1)
                a1 = rnd() % 3;
            uint32_t args[3] = {a1, 0xBAD00006u, 0xBAD00007u};
            if (cases[k].inputs) {
                cases[k].inputs(c, hunter, args);
                a1 = args[0];
            }
            if (stub_mode)      /* what a version leaves unset on its stack shows, where a call's argument is compared */
                memset(c->mem + STACK_LOW, 0xA5, TEST_SP - STACK_LOW);
            if (stub_mode && stack_pattern)
                for (uint32_t a = STACK_LOW; a < TEST_SP; a += 4)
                    SW(a, a * 2654435761u);
            memcpy(start, c->mem + SNAP_START, SNAP_SIZE);
            uint32_t saved[9], after[2][9], v0[2];
            static float after_v[2][sizeof c->v / sizeof c->v[0]];
            uint32_t after_f0[2];
            for (int n = 0; n < MAX_CALLS; n++) {
                results[n] = PICK(0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 5, 0x105, 0x100, 0x101);   /* (a classifier's, a test's, a low byte's) */
                fresults[n] = rnd();
                bias_pick[n] = rnd();
                stub_stamina[n] = (int16_t)PICK(0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0, 0, -1, 1, 76, 300);
                stub_slot[n] = (uint8_t)PICK(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 2);
                stub_record_type[n] = (uint8_t)PICK(0, 0, 1, 2, 3, 4, 0x82);
                if (pokes) {
                    /* low bytes, low halves and whole words told apart */
                    results[n] = PICK(0, 0, 1, 1, 1, 2, 3, 0x100, 0x101, 0x10000, 0x10002, 0x8000, 0xFFFF, 0x1FFFF, -1, 5, 0x105);
                    if (quiet && rnd() % quiet) {
                        results[n] = 0;
                        stub_stamina[n] = 0x7FFF;
                        stub_slot[n] = 0xFF;
                    }
                    for (int m = 0; m < npokes; m++) {
                        stub_poke[n][m] = (uint16_t)(rnd() % pokes->count);
                        stub_poke_value[n][m] = (int32_t)PICK(0, 0, 1, 2, 3, 10, 100, 150, -1, 0x10);
                    }
                }
            }
            ncalls[0] = ncalls[1] = 0;
            for (int mode = 0; mode < 2; mode++) {
                memcpy(c->mem + SNAP_START, start, SNAP_SIZE);
                for (int r = 0; r < 32; r++)
                    c->r[r] = 0xBAD00000u + (uint32_t)r;    /* no version may depend on a dead register */
                c->r[0] = 0;
                /* and the same in the others: what the last run left there is read by real callees that load registers partly */
                c->hi = c->lo = 0;
                memset(c->f, 0, sizeof c->f);
                c->fcr31 = 0;
                c->fpcond = 0;
                memset(c->v, 0, sizeof c->v);
                memset(c->vfpu_ctrl, 0, sizeof c->vfpu_ctrl);
                c->vfpu_ctrl[0] = c->vfpu_ctrl[1] = 0xE4;      /* no prefix, as after any VFPU instruction: 0 would make the first one read x, x, x */
                c->f[12].u = test_f12;
                c->r[8] = test_t0;
                c->r[9] = test_t1;
                c->r[29] = TEST_SP;
                c->r[4] = test_a0;
                c->r[5] = a1;
                c->r[6] = args[1];
                c->r[7] = args[2];
                for (int r = 0; r < 8; r++)
                    saved[r] = c->r[16 + r];
                saved[8] = c->r[30];
                rt_tick30 = tick30;
                rt_hand = mode;
                current = mode;
                target = stub_mode ? cases[k].addr : 0;
                stubbing = stub_mode;
                running = 1;
                cases[k].fn(c);
                running = stubbing = 0;
                for (int r = 0; r < 8; r++)
                    after[mode][r] = c->r[16 + r];
                after[mode][8] = c->r[30];
                v0[mode] = c->r[2];
                memcpy(after_v[mode], c->v, sizeof after_v[mode]);
                after_f0[mode] = c->f[0].u;
                if (c->r[29] != TEST_SP) {
                    printf("%s: sp not restored (mode %d)\n", cases[k].name, mode);
                    bad++;
                }
                if (mode == 0)
                    memcpy(generated, c->mem + SNAP_START, SNAP_SIZE);
            }
            /* blank the stack in both: what's left there is dead */
            memset(generated + (STACK_LOW - SNAP_START), 0, TEST_SP - STACK_LOW);
            memset(c->mem + STACK_LOW, 0, TEST_SP - STACK_LOW);
            /* Callees running for real: game_sub 09C375D8 stores two positions with vfpu_sv_q, each's fourth word left from whatever was on the stack,
             * which differs with the stack frames above it (docs/readable.md, handdiff.py): dead */
            if (!stub_mode)
                for (int m = 0; m < 2; m++)
                    for (uint32_t o = 0x4AC; o <= 0x4CC; o += 0x20)
                        *(uint32_t *)((m ? c->mem + SNAP_START : generated) + (hunter - SNAP_START) + o) = 0;
            int same = !memcmp(generated, c->mem + SNAP_START, SNAP_SIZE);
            same = same && !memcmp(after[0], saved, sizeof saved) && !memcmp(after[1], saved, sizeof saved);
            same = same && (!cases[k].returns || v0[0] == v0[1]);
            same = same && (!compare_f0 || after_f0[0] == after_f0[1]);
            same = same && (!compare_vfpu || !memcmp(after_v[0], after_v[1], sizeof after_v[0]));
            if ((ncalls[0] != ncalls[1] || memcmp(calls[0], calls[1], sizeof(Call) * (size_t)(ncalls[0] < MAX_CALLS ? ncalls[0] : MAX_CALLS)))) {
                same = 0;
                printf("%s: calls differ\n", cases[k].name);
                for (int m = 0; m < 2; m++)
                    for (int n = 0; n < ncalls[m] && n < MAX_CALLS; n++)
                        printf("  %s %d: %08X(%08X %08X %08X %08X; t0 %08X t1 %08X sp+0 %08X; f12 %08X f13 %08X; hunter %08X)\n", m ? "hand" : "generated", n, calls[m][n].fn,
                               calls[m][n].a[0], calls[m][n].a[1], calls[m][n].a[2], calls[m][n].a[3], calls[m][n].a[4], calls[m][n].a[5], calls[m][n].stack0,
                               calls[m][n].f[0], calls[m][n].f[1], calls[m][n].hunter_hash);
            }
            if (!same) {
                bad++;
                printf("%s: DIFFERS a1=%08X a2=%08X a3=%08X tick30=%d", cases[k].name, a1, args[1], args[2], tick30);
                printf(cases[k].returns ? " v0 generated %08X hand %08X\n" : "\n", v0[0], v0[1]);
                for (uint32_t o = 0; o < SNAP_SIZE; o += 4)
                    if (memcmp(generated + o, c->mem + SNAP_START + o, 4))
                        printf("  %08X generated %08X hand %08X\n", SNAP_START + o, *(uint32_t *)(generated + o),
                               *(uint32_t *)(c->mem + SNAP_START + o));
            }
            memcpy(c->mem + SNAP_START, start, SNAP_SIZE);
        }
        printf("%s: %s\n", cases[k].name, bad ? "FAILED" : "same");
        failed += bad;
    }
    return failed != 0;
}
