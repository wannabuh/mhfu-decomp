/* A hunter's update each tick: the state's function, then everything around it. Rewritten by hand from game_task 09A665F0, and EBOOT 088D5C7C
 * which it calls for the motion, the generated code still behind them (MHFU_HAND=0).
 *
 * The update keeps the game's stack frame: callees are given addresses in it. Each thing it calls may change what's read next, so fields are
 * read where the original reads them. */
#include <stddef.h>

#include "hand.h"
#include "names.h"

/* Named here by address as function names may change: */
#define scan_entities f_game_task_09A687C0
#define object_tick_counters f_EBOOT_08865934
#define pouch_selector_input f_game_task_09A6A868
#define pouch_weapon_selector_input f_game_task_09A6ADC8
#define process_received_hit f_game_task_09A6B130
#define classify_status_fields f_EBOOT_088D69F0
#define periodic_health_drains f_game_task_09A6D418
#define run_state_function f_game_task_09A67568
#define check_566_once f_game_task_09AA8FC8
#define turn_toward_target f_EBOOT_088D6B00
#define track_nearest_object f_EBOOT_088D6D10
#define apply_timed_displacement f_EBOOT_088D71B8
#define find_interaction_zone f_game_task_09AA6B18
#define quest_hint_event f_EBOOT_088966C0
#define build_status_word f_game_task_09A6E190
#define tick_freeze_counter f_EBOOT_08865A2C
#define state_is f_EBOOT_08865CB8
#define motion_update f_EBOOT_088D5C7C
#define tick_counters_426_427 f_game_task_09A6FB68
#define in_current_stage f_EBOOT_08865BAC
#define find_area_zone f_game_task_09A6E738
#define update_ground_attribute f_game_task_09A6E8C8
#define field_1144_is_positive f_game_task_09AA8958
#define update_place_260 f_EBOOT_08865BD0
#define weapon_1_5_test f_game_task_09A8BBE0     /* classes 1 and 5: true when a weapon's flag 0x1000 can flip */
#define sound_play_2d f_EBOOT_08883988
#define clear_flag_1000 f_game_task_09AA9228
#define state_keeps_flag_1000 f_EBOOT_088DACA4
#define flag_1000_test f_game_task_09A8BB20
#define camera_says_hidden f_EBOOT_08814048
#define weapon8_test f_game_task_09BB0FE0
#define weapon8_clear f_game_task_09BB0F08
#define update_near_object f_game_task_09A6E9E0
#define flag_8000_action_a f_game_task_09A6EC50
#define flag_8000_action_b f_game_task_09A6EB30

/* In game_sub, an overlay not resident at link time: called as the game does */
#define STATUS_200_HANDLER 0x09CB2540u
#define STATUS_800_HANDLER 0x09CB25D0u
#define STATUS_1000_HANDLER 0x09CB2588u
#define HIT_SWEEP 0x09C417E0u
#define FLOOR_CHECK_MOVING 0x09C3D4E8u
#define FLOOR_FIND 0x09C3E698u
#define HEIGHT_UPDATE 0x09C375D8u

#define GAME_FRAME_COUNTER 0xF7A554u        /* of what RNG_POINTER points at */
#define RNG_POINTER 0x08A632C0u
#define HUD_POINTER 0x089CBD0Cu
#define CAMERA_POINTER 0x089CBD04u
#define SOUND_2D_POINTER 0x08A62EACu
#define VOLUMES_POINTER 0x09A4F1A0u
#define COLLISION_POINTER 0x09D1A160u
#define EFFECT_RATE_SETTING_POINTER 0x09A1DC2Cu     /* -> a byte: 0 every time, 1 every 5th, 2 every 10th */

#define VSLOT_SET_STATE 0x6C
#define VSLOT_TICK_A 0x5C
#define VSLOT_TICK_B 0x60
#define VSLOT_TICK_C 0x64
#define VSLOT_STATE_SET_FOLLOW_UP 0x54
#define VSLOT_STAGE_BUSY 0xA0

/* The stack frame, and what callees get pointers to in it */
#define FRAME 0x50
#define FRAME_ZONE_A 0x30
#define FRAME_ZONE_B 0x4C
#define FRAME_ZONE_C 0x4F
#define FRAME_FLOOR_ARG 0x20
#define FRAME_FLOOR_FOUND 0x48

#define ZONE_HINT_1 0x18
#define ZONE_HINT_3 0x21
#define ZONE_HINT_4 0x1E

#define PLACE 0x200                 /* offsets in the hunter callees take the address of */
#define MOTION_SPHERE 0x290
#define FLOOR_RESULT 0x2A0

#define FALL_START_HEIGHT 30.0f     /* this far above the floor it starts falling */
#define HEIGHT_UPDATE_ARG 40.0f     /* what game_sub's function takes in f12 */

/* Group, as a state's first number */
enum {
    GROUP_BASE = 0,
    GROUP_ACTION = 1,
    GROUP_REACTION = 2,
    GROUP_FAINT = 3,
    GROUP_END = 4,
    GROUP_5 = 5,
    GROUP_6 = 6,
};

#define HUNTER_FLAG_INPUT_OFF 2
#define HUNTER_FLAG_MAY_ACT 4
#define HUNTER_FLAG_8 8
#define HUNTER_FLAG_10 0x10
#define HUNTER_FLAG_WAS_MAY_ACT 0x20
#define HUNTER_FLAG_40 0x40
#define HUNTER_FLAG_800 0x800
#define HUNTER_FLAG_1000 0x1000
#define HUNTER_FLAG_8000 0x8000
#define MISC_FLAG_2 2
#define MISC_FLAG_REPEAT_STATE 4    /* the state function set a new state: run it at once */
#define MISC_FLAG_WEAPON_OUT 8
#define MISC_FLAG_10 0x10
#define OBJECT_FLAG_HIDDEN 4
#define QUEST_FLAG_SEEN 2
#define BUTTON_DASH 0x200

static int is_local(Ctx *c, const Hunter *self) {
    return self->instance_index == GAME()->local_hunter_slot;
}

static int is_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state) {
    return (uint8_t)call3(c, state_is, GAME_ADDR(self), group, state) != 0;
}

static int in_stage(Ctx *c, Hunter *self) {
    return (uint8_t)call1(c, in_current_stage, GAME_ADDR(self)) != 0;
}

static void set_state(Ctx *c, Hunter *self, uint32_t group, uint32_t state) {
    call_method3(c, GAME_ADDR(self), VSLOT_SET_STATE, group, state, 0);
}

/* A method of the hunter's that a state's setting calls too, with t0 as well */
static void state_follow_up(Ctx *c, Hunter *self, uint32_t what, uint32_t t0) {
    c->r[8] = t0;
    call_method3(c, GAME_ADDR(self), VSLOT_STATE_SET_FOLLOW_UP, what, 0, 0);
}

/* -- the parts, in the order the update runs them ---------------------------------------------------------------------------------- */

static void reset_for_the_tick(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    call1(c, scan_entities, self_addr);
    call1(c, object_tick_counters, self_addr);
    self->selector_input_flags = 0;
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_8;
    if (!(self->hunter_flags & HUNTER_FLAG_INPUT_OFF)) {
        call1(c, pouch_selector_input, self_addr);
        call1(c, pouch_weapon_selector_input, self_addr);
    }
    self->place_before[0] = self->place[0];
    self->place_before[1] = self->place[1];
    self->place_before[2] = self->place[2];
    self->place_before_w = self->place_w;
    self->misc_flags &= ~(uint32_t)MISC_FLAG_2;
    self->hunter_flags &= ~(uint32_t)(HUNTER_FLAG_MAY_ACT | HUNTER_FLAG_10);
    /* the count of grabs from a fall survives only those two states, and only for the local player */
    if (!is_local(c, self) || (!is_state(c, self, GROUP_BASE, 9) && !is_state(c, self, GROUP_BASE, 0x1B)))
        self->ledge_grabs_from_fall = 0;
}

static void react_to_status(Ctx *c, Hunter *self) {
    uint32_t status = call1(c, classify_status_fields, GAME_ADDR(self));
    if (status == 2) {
        if (!is_state(c, self, GROUP_REACTION, 0x16) && !is_state(c, self, GROUP_REACTION, 0x18))
            set_state(c, self, GROUP_REACTION, 0x16);
    } else if (status == 1) {
        set_state(c, self, GROUP_REACTION, 0x15);
    }
}

static void run_the_state(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    call1(c, periodic_health_drains, self_addr);
    call1(c, run_state_function, self_addr);
    if (self->misc_flags & MISC_FLAG_REPEAT_STATE) {
        call1(c, run_state_function, self_addr);
        self->misc_flags &= ~(uint32_t)MISC_FLAG_REPEAT_STATE;
    }
    call1(c, check_566_once, self_addr);
    call1(c, turn_toward_target, self_addr);
    call1(c, track_nearest_object, self_addr);
    if (self->state_group != GROUP_END)
        call1(c, apply_timed_displacement, self_addr);
}

static void zone_hint(Ctx *c, Hunter *self) {
    uint32_t sp = c->r[29];
    uint16_t zone = (uint16_t)call4(c, find_interaction_zone, GAME_ADDR(self), sp + FRAME_ZONE_A, sp + FRAME_ZONE_B, sp + FRAME_ZONE_C);
    if (zone == ZONE_HINT_1)
        call2(c, quest_hint_event, LW(HUD_POINTER), 1);
    else if (zone == ZONE_HINT_3)
        call2(c, quest_hint_event, LW(HUD_POINTER), 3);
    else if (zone == ZONE_HINT_4)
        call2(c, quest_hint_event, LW(HUD_POINTER), 4);
}

static void status_handlers(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    if (self->status_bits & 0x200) {
        c->r[4] = self_addr;
        rt_call(c, STATUS_200_HANDLER);
    }
    if (self->status_bits & 0x800) {
        c->r[4] = self_addr;
        rt_call(c, STATUS_800_HANDLER);
    }
    if (self->status_bits & 0x1000) {
        c->r[4] = self_addr;
        rt_call(c, STATUS_1000_HANDLER);
    }
    self->hud_request = 0;
}

static void motion_step(Ctx *c, Hunter *self) {
    /* (asked twice: a callee can't change it between, but the game does ask twice) */
    if (is_state(c, self, GROUP_BASE, 0x27)) {
        if ((uint8_t)call3(c, state_is, GAME_ADDR(self), GROUP_BASE, 0x27) != 1 || self->state_scratch_a != 0)
            return;
    }
    call1(c, motion_update, GAME_ADDR(self));
}

static void floor_and_hits(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self), sp = c->r[29];
    if (!is_state(c, self, GROUP_BASE, 0x1B) || !is_state(c, self, GROUP_BASE, 0x1E)) {
        if (in_stage(c, self)) {
            c->r[4] = self_addr;
            c->r[5] = 0;
            rt_call(c, HIT_SWEEP);
        }
    }
    if (is_local(c, self) && self->state_group == GROUP_BASE && (self->state == 1 || self->state == 3 || self->state == 0x13) && in_stage(c, self)) {
        c->r[4] = LW(COLLISION_POINTER);
        c->r[5] = sp + FRAME_FLOOR_ARG;
        c->r[6] = self_addr;
        c->r[7] = 1;
        rt_call(c, FLOOR_CHECK_MOVING);
    }
    call1(c, find_area_zone, self_addr);
    if (is_local(c, self)) {
        c->r[4] = LW(COLLISION_POINTER);
        c->r[5] = self_addr;
        c->r[6] = self_addr + PLACE;
        c->r[7] = self_addr + MOTION_SPHERE;
        c->r[8] = sp + FRAME_FLOOR_FOUND;
        c->r[9] = self_addr + FLOOR_RESULT;
        c->r[10] = 0;
        rt_call(c, FLOOR_FIND);
        if (c->r[2] == 1)
            SW(self_addr + offsetof(Hunter, floor_y), LW(sp + FRAME_FLOOR_FOUND));
    }
}

/* Moving on ground that forces it, unless a state in the list says otherwise */
static int state_ignores_forced_ground(uint8_t state) {
    switch (state) {
    case 0x08: case 0x09: case 0x0F: case 0x14: case 0x1A: case 0x1C: case 0x1D: case 0x2D: case 0x2E: case 0x2F: case 0x31:
    case 0x32: case 0x3B: case 0x3C: case 0x3D: case 0x44: case 0x59: case 0x5D: case 0x60: case 0x7D: case 0x7E:
        return 1;
    default:
        return 0;
    }
}

static void forced_by_ground(Ctx *c, Hunter *self) {
    if (!in_stage(c, self))
        return;
    call1(c, update_ground_attribute, GAME_ADDR(self));
    if (self->ground_forces_state_3b == 0 || self->movement_mode == 2 || (uint8_t)call1(c, field_1144_is_positive, GAME_ADDR(self)) == 1)
        return;
    uint8_t group = self->state_group;
    if (group == GROUP_5 || group == GROUP_ACTION)
        set_state(c, self, GROUP_BASE, 8);
    else if (group == GROUP_BASE && !state_ignores_forced_ground(self->state))
        set_state(c, self, GROUP_BASE, 8);
}

/* -- walking off an edge: above the floor by enough, a fall begins ------------------------------------------------------------------- */

static void start_falling(Ctx *c, Hunter *self) {
    self->movement_mode = 2;
    uint8_t group = self->state_group;
    if (group == GROUP_5) {
        set_state(c, self, GROUP_5, 6);
    } else if (group == GROUP_REACTION) {
        uint8_t state = self->state;
        if (state == 0x1B || (state >= 0x1E && state <= 0x23))
            set_state(c, self, GROUP_REACTION, 0x24);
        else
            set_state(c, self, GROUP_BASE, 9);
    } else if (group == GROUP_BASE && self->state == 0x7E) {
        set_state(c, self, GROUP_REACTION, 0x24);
    } else {
        set_state(c, self, GROUP_BASE, 9);
    }
}

static void snap_to_floor_or_fall(Ctx *c, Hunter *self) {
    if (self->movement_mode == 2 || self->attached_part_mode != 0 || is_state(c, self, GROUP_BASE, 0x3A) || self->state_group == GROUP_END || !in_stage(c, self))
        return;
    float floor_y = self->floor_y, y = self->place[1];
    if (y < floor_y) {
        self->place[1] = floor_y;
        self->attached_part_mode = 0;
    } else if (y >= floor_y) {      /* not if either is NaN */
        if (y - floor_y < FALL_START_HEIGHT) {
            self->place[1] = floor_y;
            self->attached_part_mode = 0;
        } else if (self->health > 0) {
            start_falling(c, self);
        }
    }
}

/* -- the flag 0x1000 of the hunter: what's done with it, and whether it may hide ------------------------------------------------------ */

static void flag_1000_while_weapon_out(Ctx *c, Hunter *self) {
    uint8_t group = self->state_group;
    if (group == GROUP_BASE) {
        switch (self->state) {
        case 0x3A:
        case 0x1B: self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_1000; break;
        case 0x1C: case 0x67: case 0x68: case 0x69: case 0x6A: break;
        default: self->hunter_flags ^= HUNTER_FLAG_1000; break;
        }
    } else if (group != GROUP_6 && !is_state(c, self, GROUP_ACTION, 3)) {
        self->hunter_flags ^= HUNTER_FLAG_1000;
    }
    if (self->movement_mode == 2)
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_1000;
}

static void flag_1000_sound_unless_busy(Ctx *c, Hunter *self) {
    uint32_t volumes = LW(LW(VOLUMES_POINTER));
    if ((uint8_t)call_method0(c, volumes, VSLOT_STAGE_BUSY) != 0)
        return;
    if ((uint8_t)call1(c, in_current_stage, GAME_ADDR(self)) == 1) {
        SW(c->r[29], 0);
        c->r[4] = LW(SOUND_2D_POINTER);
        c->r[5] = 0;
        c->r[6] = 0x15;
        c->r[7] = 0;
        c->r[8] = 0;
        c->r[9] = 0;
        c->r[10] = 0;
        c->r[11] = 0;
        sound_play_2d(c);
    }
    call1(c, clear_flag_1000, GAME_ADDR(self));
}

static void flag_1000_unless_state_6x(Ctx *c, Hunter *self) {
    if (is_state(c, self, GROUP_BASE, 0x65) || is_state(c, self, GROUP_BASE, 0x66))
        self->hunter_flags |= HUNTER_FLAG_1000;
    else
        call1(c, clear_flag_1000, GAME_ADDR(self));
}

static void flag_1000_and_visibility(Ctx *c, Hunter *self) {
    uint32_t self_addr = GAME_ADDR(self);
    int weapon_decided = 0;
    if (self->misc_flags & MISC_FLAG_WEAPON_OUT) {
        uint8_t weapon_class = self->weapon_class;
        if ((weapon_class == 1 || weapon_class == 5) && call1(c, weapon_1_5_test, self_addr) != 0) {
            flag_1000_while_weapon_out(c, self);
            flag_1000_sound_unless_busy(c, self);
            weapon_decided = 1;
        }
    }
    if (!weapon_decided) {
        if (!(self->misc_flags & MISC_FLAG_WEAPON_OUT)) {
            if ((uint8_t)call1(c, state_keeps_flag_1000, self_addr) == 1) {
                if (self->buttons_pressed & BUTTON_DASH)
                    self->hunter_flags ^= HUNTER_FLAG_1000;
            } else {
                flag_1000_unless_state_6x(c, self);
            }
        } else if (self->weapon_uses_load != 1) {
            flag_1000_unless_state_6x(c, self);
        }
    }
    if (self->hunter_flags & HUNTER_FLAG_1000) {
        if ((uint8_t)call1(c, flag_1000_test, self_addr) == 1) {
            self->flag_1000_lockout_ticks = 10;
            if ((uint8_t)call2(c, camera_says_hidden, LW(CAMERA_POINTER), self_addr) == 1)
                self->flags |= OBJECT_FLAG_HIDDEN;
        }
    } else if (is_local(c, self) || (self->quest_flags & QUEST_FLAG_SEEN)) {
        self->flags &= ~(uint32_t)OBJECT_FLAG_HIDDEN;
    }
}

/* -- what happens every so many game frames --------------------------------------------------------------------------------------- */

static void periodic_effects(Ctx *c, Hunter *self) {
    int32_t frame = (int32_t)LW(LW(RNG_POINTER) + GAME_FRAME_COUNTER);
    if (frame % 30 == 0) {
        state_follow_up(c, self, 2, 0);
        return;
    }
    uint8_t rate = LBU(LW(EFFECT_RATE_SETTING_POINTER));
    if (rate == 1 ? frame % 5 != 0 : rate == 2 ? frame % 10 != 0 : rate != 0)
        return;
    state_follow_up(c, self, 0xB + (uint32_t)(frame & 3), 1);
}

static void flag_8000_actions(Ctx *c, Hunter *self) {
    uint32_t flags = self->hunter_flags;
    if (!is_local(c, self) || (flags & HUNTER_FLAG_1000) || !(flags & HUNTER_FLAG_8000))
        return;
    if ((uint8_t)call1(c, state_keeps_flag_1000, GAME_ADDR(self)) == 1 &&
        ((uint8_t)call3(c, state_is, GAME_ADDR(self), GROUP_BASE, 0x36) != 1 || self->state_scratch_a < 9)) {
        call1(c, flag_8000_action_a, GAME_ADDR(self));
        return;
    }
    uint8_t weapon_class = self->weapon_class;
    if ((weapon_class != 1 && weapon_class != 5) || self->flag_1000_lockout_ticks != 0 || !(self->misc_flags & MISC_FLAG_WEAPON_OUT))
        return;
    if (self->state_group == GROUP_REACTION || self->state_group == GROUP_6)
        return;
    const Camera *camera = OBJ(Camera, LW(CAMERA_POINTER));
    if (camera->hunt_tracking_hunter_facing != 0 || camera->current_view != 0 || GAME()->option_camera_follow_mode == 1)
        call1(c, flag_8000_action_b, GAME_ADDR(self));
}

/* game_task 09A665F0: the hunter's vtable +0x38's state_run */
void hand_hunter_state_run(Ctx *c) {
    Hunter *self = OBJ(Hunter, c->r[4]);
    uint32_t self_addr = c->r[4];
    c->r[29] -= FRAME;
    reset_for_the_tick(c, self);
    call1(c, process_received_hit, self_addr);
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_WAS_MAY_ACT;
    if (self->health > 0)
        react_to_status(c, self);
    call_method0(c, self_addr, VSLOT_TICK_A);
    call_method0(c, self_addr, VSLOT_TICK_B);
    call_method0(c, self_addr, VSLOT_TICK_C);
    if (self->freeze_ticks == 0)
        run_the_state(c, self);
    if (self->hunter_flags & HUNTER_FLAG_MAY_ACT)
        self->hunter_flags |= HUNTER_FLAG_WAS_MAY_ACT;
    else
        self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_WAS_MAY_ACT;
    if (is_local(c, self))
        zone_hint(c, self);
    if (!(self->misc_flags & MISC_FLAG_WEAPON_OUT))
        self->misc_flags &= ~(uint32_t)MISC_FLAG_10;
    call1(c, build_status_word, self_addr);
    status_handlers(c, self);
    self->hunter_flags &= ~(uint32_t)HUNTER_FLAG_40;
    call1(c, tick_freeze_counter, self_addr);
    motion_step(c, self);
    call1(c, tick_counters_426_427, self_addr);
    floor_and_hits(c, self);
    forced_by_ground(c, self);
    if (is_local(c, self))
        snap_to_floor_or_fall(c, self);
    self->hunter_flags |= HUNTER_FLAG_800;
    call1(c, update_place_260, self_addr);
    if (self->state_group != GROUP_REACTION && self->state_group != GROUP_FAINT)
        flag_1000_and_visibility(c, self);
    c->f[12].f = HEIGHT_UPDATE_ARG;
    c->r[4] = self_addr;
    rt_call(c, HEIGHT_UPDATE);
    if (!(self->hunter_flags & HUNTER_FLAG_1000))
        self->flag_1000_lockout_ticks = 0;
    if (self->weapon_class == 8 && (int16_t)call1(c, weapon8_test, self_addr) == 0)
        call1(c, weapon8_clear, self_addr);
    if (in_stage(c, self)) {
        call1(c, update_near_object, self_addr);
        periodic_effects(c, self);
        flag_8000_actions(c, self);
    }
    c->r[29] += FRAME;
}

/* -- EBOOT 088D5C7C: the motion's speed, new motions started, the layers run ------------------------------------------------------------ */

#define MOTION_SPEED_DEFAULT 2.0f
#define MOTION_SPEED_SLOWED 0.2f
#define VSLOT_MOTION_RECORD 0x20
#define HUNTER_FLAG_LAYER0_STARTED 0x80
#define HUNTER_FLAG_LAYER1_STARTED 0x100
#define GAME_MODE_LOBBY 1

static void set_motion_speeds(HunterBase *self, float speed) {
    self->motion_part1_speed = speed;
    self->motion_part0_speed = speed;
}

/* Motions that set no speed, those that are only kept from being slow, and a pair that depends on the mode */
static void motion_speed_by_id(Ctx *c, HunterBase *self) {
    int keep_from_slow = 0;
    switch (self->motion_id) {
    case 0x22: case 0x731: case 3:
        return;
    case 0x195: case 0x196: case 0x197: case 0x19B: case 0x19C: case 0x19D: case 0x19F: case 0x1A0: case 0x1A2: case 0x1A3: case 0x1A6: case 0x1A7:
        keep_from_slow = 1;
        break;
    case 0x587: case 0x58A:
        keep_from_slow = (GAME()->mode_flags & GAME_MODE_LOBBY) != 0;
        break;
    default:
        break;
    }
    if (keep_from_slow) {
        if (self->motion_part0_speed <= MOTION_SPEED_DEFAULT)
            set_motion_speeds(self, MOTION_SPEED_DEFAULT);
    } else {
        set_motion_speeds(self, MOTION_SPEED_DEFAULT * self->motion_speed_scale);
    }
}

void hand_hunter_motion_update(Ctx *c) {
    HunterBase *self = OBJ(HunterBase, c->r[4]);
    uint32_t self_addr = c->r[4], player = self_addr + offsetof(HunterBase, motion_player);
    if (self->freeze_ticks != 0) {
        set_motion_speeds(self, 0.0f);
    } else if (self->motion_slow_timer != 0) {
        self->motion_slow_timer -= rt_tick30;
        set_motion_speeds(self, MOTION_SPEED_SLOWED);
    } else if (self->motion_id != 0x23) {
        motion_speed_by_id(c, self);
    }
    if (!(self->hunter_flags & HUNTER_FLAG_LAYER0_STARTED)) {
        self->hunter_flags |= HUNTER_FLAG_LAYER0_STARTED;
        uint32_t record = call_method1(c, self_addr, VSLOT_MOTION_RECORD, self->motion_id);
        if (record != 0) {
            c->r[8] = 0;
            call4(c, motion_player_change_blended, player, record, self->motion_start_frame, (uint32_t)(int32_t)self->motion_blend_frames);
        }
    }
    if (!(self->hunter_flags & HUNTER_FLAG_LAYER1_STARTED)) {
        self->hunter_flags |= HUNTER_FLAG_LAYER1_STARTED;
        uint32_t record = call_method1(c, self_addr, VSLOT_MOTION_RECORD, self->motion_id_layer1);
        if (record != 0) {
            c->r[8] = 1;
            call4(c, motion_player_change_blended, player, record, self->part1_motion_start_frame, (uint32_t)(int32_t)self->part1_motion_blend_frames);
        }
    }
    call1(c, motion_update_layers, self_addr);
}
