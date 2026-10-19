"""Changes to the game's code, made as it is translated: (module, address) -> (the instruction there, as disassembled,
checked so that a patch never lands on something else; the C to run instead; why).

The native frame rate: the game counts time in its frames (30 a second). With MHFU_TICK_HZ=N it runs N frames a
second and rt_time_scale is 30 / N; what advances by so much a frame is patched to advance by that much times
rt_time_scale. A count of frames in an integer cannot go in fractions: it counts only on the frames where a frame at
30 a second would fall (rt_tick30, 1 or 0) - the same time, in steps of a 30th. At the game's own rate the scale is 1
and every frame a 30th, so the patches change nothing then. docs/native.md keeps the list.
"""

# The motion player (EBOOT 088637C4): each layer's frame (+0x10) advances by its speed (+0x14) a game frame - 2.0 for
# the 60 frames a second the motions are made at. The speed is loaded in four places; each load is scaled.
MOTION = "the motion player: a layer's speed (frames a game frame), times the time scale"
# The root motion's step (EBOOT 08863B70, before the player): the motion's displacement from a layer's frame to the
# frame its speed takes it to - each load of the speed scaled, so the step is the one the player then takes
ROOT = "the root motion's step: over a layer's speed (frames a game frame) times the time scale"
# A character's tick (EBOOT 08865934, before its state's function): the ticks in its state (+0x3F4) and another
# count (+0x45C) go up by 2 a game frame (in 60ths), a byte (+0x276) counts game frames down
COUNTS = "a character's counts: in 60ths of a second (rt_ticks60) and 30ths (rt_tick30), not game frames"
# The hunter's attack buttons (game_task 09A68DB0): one pressed waits two game frames (+0x1128) for another to make a
# combination - counted in 30ths
COMBINE = "the attack buttons' wait for a second one: in 30ths of a second (rt_tick30), not game frames"
# The local player's buttons (EBOOT 088D61CC: the pad's, *0x08A62D88 held, +4 pressed, +8 released, copied into
# the player): the pad is read at 30ths, so its pressed and released are seen at a 30th only - not again on the frames
# between, as a second press
EDGES = "the buttons pressed and released, seen at 30ths only (the pad is read at 30ths: not pressed again between)"
# A character's turn to where it is going (EBOOT 088D6B00): its facing (+0x1F4) steps by so much a game frame, its lean
# (+0x41A) by 0x80 - times the time scale
TURN = "a character's turn and lean: their steps a game frame times the time scale"
# The hunter's sprint (game_task 09A9E288): a push of 5 units forward a game frame on top of the motion's, and a
# point of stamina a game frame
SPRINT = "the sprint's push (units a game frame) times the time scale, its stamina a 30th"
# A motion's frame reached (EBOOT 08864348: frame N in [a layer's frame, its frame + its speed) - the step the player
# takes next): the speed times the time scale, or a sound or event at frame N comes again each game frame until the
# frame passes it
REACHED = "a motion frame reached: within the player's next step (its speed times the time scale)"
BLEND = "the motion player: a blend's weight step times the time scale, its count of frames at 30 a second"
# The hunter in the air (game_task 09A9C530 falling, 09A905F0 and 09A99928 the other leaps and flights: its velocity
# +0x240 gains its acceleration +0x250 a game frame, its place +0x200 the velocity turned to its facing) - both steps
# times the time scale; and an action's count of game frames (+0x414: how long it has fallen, how long a state has
# lasted) counted in 30ths
AIR = "the hunter in the air: its velocity's and its place's steps a game frame times the time scale"
ACTION = "a hunter's action's count of game frames (+0x414): counted in 30ths"

# The hunt's camera (EBOOT 08886E50, view at $s4 - 0xA0, its yaw to turn to at +0x80) turns itself where the scenery is
# in the way: it casts rays between the eye and the hunter; one hit, it turns 0x400 a frame (5.6 degrees) until it is
# clear; the line of sight lost (+0x89), it swings an eighth of the way behind the hunter a frame. With the mouse
# turning the camera (rt_camera_free) those turns are left out: the camera goes where the mouse puts it.
CAMERA = "the hunt's camera: its own turns away from the scenery, left out while the mouse has the camera"

# The test of a model's sphere against the camera's view (EBOOT 088160F0: a0 the scene, a1 the centre, f12 the radius;
# 1 if it is to be drawn): what is drawn casts the renderer's shadows, so with them a model is drawn when within
# rt_cull_margin (units) of the view - the shadows of what is just out of view do not come and go as the camera turns.
CULL = "the view test of a model's sphere: the sphere larger by rt_cull_margin (the shadows of what is just out of view)"


# The game's main loop (EBOOT 0888FD38), as it wakes for a game frame: whether a 30th falls on it
FRAME = "the main loop, woken for a game frame: rt_frame_start (whether a 30th falls on it, the time scale)"
COUNT = "the main loop's count of its frames: counting 30ths (as what is gated sees the frames go by)"

# The display list's chunks are linked into the frame's list by EBOOT 088596C8 (the list's manager, the chunk, its size
# in words, the layer): those linked by gated code are noted (rt_list_link), for the frames between 30ths to draw again
LINK = "a display list chunk linked: noted, if by gated code (runtime/src/native.c)"

# Drawn by the render pass (every frame) on a word the logic sets each frame and the drawing clears: kept until the
# logic runs again (the frame before a 30th), so that the frames between draw it too
ONCE = "a one-frame flag the render pass clears after drawing: cleared before a 30th only (the frames between draw too)"

# Gates: (module, a function's start or a call) -> (why, when). What has not been converted yet runs as at 30 frames a
# second (rt_gate): on the frames a 30th falls on, with the time scale 1; skipped on the others. "before": on the frames
# before those instead (what makes ready for the 30th's logic). "object": an object's tick (a call, a0 the object):
# every frame if the object is converted, gated otherwise (what it drew kept for it alone). "native": a converted call
# made by gated code: made every frame - on the frames between by the gate it is under, as it was made at the 30th.
GATES = {
    # the main loop's calls (EBOOT 0888FD38): all but the render pass (0884BB58), which draws every game frame
    ("EBOOT", 0x0888FD5C): ("the pad", "on"),
    ("EBOOT", 0x0888FD68): ("?", "on"),
    ("EBOOT", 0x0888FD80): ("the mode's main (0884DBB8)", "on"),
    ("EBOOT", 0x0888FD8C): ("?", "on"),
    ("EBOOT", 0x0888FD98): ("?", "on"),
    ("EBOOT", 0x0888FDA4): ("the game states (their objects)", "on"),
    # the render queues the logic fills (the render pass draws them): emptied before the logic runs again, so that the
    # frames between draw what it queued
    ("EBOOT", 0x0888FDBC): ("the render queues emptied", "before"),
    ("EBOOT", 0x0888FDC8): ("?", "on"),
    ("EBOOT", 0x0888FDD4): ("2D: text and sprites (EBOOT 08891B14)", "on"),
    # the wait for the next frame (EBOOT 088590B0), with a flag of the game's (*0x08A632C0 + 0xF7A57C, bit 0) set: three
    # calls each frame (at a game frame's end: they run if it was a 30th)
    ("EBOOT", 0x088590F8): ("?", "on"),
    ("EBOOT", 0x08859104): ("?", "on"),
    ("EBOOT", 0x08859110): ("?", "on"),
    # the hunt's objects (EBOOT 088DB250, the object manager's run): the loop over them (088666B4, list 2) and the local
    # player's (088DB424) made every frame, each object's tick gated by itself - converted objects (by their vtable)
    # ticking every frame
    ("EBOOT", 0x088DB264): ("the local player's (088DB424), every frame", "native"),
    ("EBOOT", 0x088DB474): ("the local player's (088D61CC)", "object"),
    ("EBOOT", 0x088DB274): ("the objects' ticks (088666B4), every frame", "native"),
    ("EBOOT", 0x088666E4): ("an object's tick (its vtable's +0x14)", "object"),
    # the hunt's tick (game_task 09A627B8), after the objects: the players' world matrices and poses (EBOOT 088DB338 ->
    # 088D2894), more of theirs (088DB39C -> 088D2E04), the cameras (08812BD0) - every frame, each player's by itself
    ("game_task", 0x09A62838): ("the players' matrices and poses (088DB338), every frame", "native"),
    ("EBOOT", 0x088DB370): ("a player's matrices and pose (088D2894)", "object"),
    ("game_task", 0x09A62844): ("the players' (088DB39C), every frame", "native"),
    ("EBOOT", 0x088DB3F8): ("a player's (088D2E04)", "object"),
    ("game_task", 0x09A62850): ("the cameras", "native"),
    # a character's upkeep (EBOOT 08865934's call of its vtable's +0x3C; the hunter's game_task 09A68FC8): stamina,
    # hunger (a 6-minute count), status and skills' timers - as at 30, a converted character's too
    ("EBOOT", 0x088659AC): ("a character's upkeep (its vtable's +0x3C)", "on"),
}


def scaled_load(f, base, off):
    """lwc1 $f<f>, off($<base>), the value times the time scale"""
    return f"c->f[{f}].u = LW((c->r[{base}] + {off:#x}u)); c->f[{f}].f *= rt_time_scale;"


PATCHES = {
    ("EBOOT", 0x088638C0): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x088638D4): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), MOTION),
    ("EBOOT", 0x08863978): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x08863994): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    # a blend between motions: its weight (+0x34) steps by +0x24 a game frame, while its count of frames (+0x30) runs out
    ("EBOOT", 0x08863BE8): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), ROOT),
    ("EBOOT", 0x08863C44): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), ROOT),
    ("EBOOT", 0x08863CFC): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), ROOT),
    ("EBOOT", 0x08865964): ("addiu $v0, $v0, 0x2", "c->r[2] = c->r[2] + (uint32_t)rt_ticks60;", COUNTS),
    ("EBOOT", 0x08865978): ("addiu $v1, $v1, 0x2", "c->r[3] = c->r[3] + (uint32_t)rt_ticks60;", COUNTS),
    ("EBOOT", 0x0886599C): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", COUNTS),
    ("game_task", 0x09A68E3C): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", COMBINE),
    ("EBOOT", 0x088D61F0): ("lw $a1, 0x2D8C($v0)", "c->r[5] = rt_tick30 ? LW((c->r[2] + 0x2D8Cu)) : 0;", EDGES),
    ("EBOOT", 0x088D6200): ("lw $v1, 0x2D90($v1)", "c->r[3] = rt_tick30 ? LW((c->r[3] + 0x2D90u)) : 0;", EDGES),
    ("EBOOT", 0x088D6230): ("lw $a1, 0x2D8C($a1)", "c->r[5] = rt_tick30 ? LW((c->r[5] + 0x2D8Cu)) : 0;", EDGES),
    ("EBOOT", 0x088D6238): ("lw $v1, 0x2D90($v1)", "c->r[3] = rt_tick30 ? LW((c->r[3] + 0x2D90u)) : 0;", EDGES),
    ("EBOOT", 0x088D6C0C): ("lhu $a0, 0x324($s0)", "c->r[9] = (uint32_t)(int32_t)((float)(int32_t)c->r[9] * rt_time_scale + 0.5f); "
                             "c->r[4] = LHU((c->r[16] + 0x324u));", TURN),
    ("EBOOT", 0x088D6C20): ("lw $a3, 0x1F4($s0)", "c->r[8] = (uint32_t)(int32_t)((float)(int32_t)c->r[8] * rt_time_scale + 0.5f); "
                             "c->r[7] = LW((c->r[16] + 0x1F4u));", TURN),
    ("game_task", 0x09A9E4F4): ("lwc1 $f1, 0x10($sp)", scaled_load(1, 29, 0x10), SPRINT),
    ("game_task", 0x09A9E4F8): ("lwc1 $f0, 0x18($sp)", scaled_load(0, 29, 0x18), SPRINT),
    ("game_task", 0x09A9E374): ("addiu $a1, $zero, -0x1", "c->r[5] = rt_tick30 ? 0xFFFFFFFFu : 0;", SPRINT),
    ("EBOOT", 0x08864368): ("lwc1 $f0, 0x14($v1)", scaled_load(0, 3, 0x14), REACHED),
    ("EBOOT", 0x08863894): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", BLEND),
    ("EBOOT", 0x088638A0): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", BLEND),
    ("EBOOT", 0x08887608): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x08887C58): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x0888FD58): ("lui $v0, 0x8A6", "rt_frame_start(); c->r[2] = 0x8A60000u;", FRAME),
    ("EBOOT", 0x088596C8): ("sll $v0, $a3, 2", "rt_list_link(c->r[4], c->r[5], c->r[6], c->r[7]); c->r[2] = c->r[7] << 2;", LINK),
    # the screen's tint (EBOOT 088CACD4, the render pass's at layer 8: the object at *0x09A4F1A0, its +0x3D4)
    ("EBOOT", 0x088CAEB4): ("sb $zero, 0x3D4($s0)", "if (rt_next_frame30()) SB((c->r[16] + 0x3D4u), 0);", ONCE),
    ("EBOOT", 0x0888FDF0): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_frame30;", COUNT),
    ("EBOOT", 0x088160F0): ("lbu $v0, 0xAAA($a0)", "c->r[2] = LBU((c->r[4] + 0xAAAu)); c->f[12].f += rt_cull_margin;", CULL),
}
    ("game_task", 0x09A8CA64): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A90604): ("addiu $a2, $a2, 0x1", "c->r[6] = c->r[6] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A907F0): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A9994C): ("addiu $a0, $a0, 0x1", "c->r[4] = c->r[4] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A99B3C): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A9C54C): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A9D494): ("addiu $a1, $a1, 0x1", "c->r[5] = c->r[5] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09AA0844): ("addiu $a1, $a1, 0x1", "c->r[5] = c->r[5] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09AA161C): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", ACTION),
    ("game_task", 0x09A90704): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A90714): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A90724): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A907AC): ("lwc1 $f2, 0x10($sp)", scaled_load(2, 29, 0x10), AIR),
    ("game_task", 0x09A907B0): ("lwc1 $f1, 0x14($sp)", scaled_load(1, 29, 0x14), AIR),
    ("game_task", 0x09A907B4): ("lwc1 $f0, 0x18($sp)", scaled_load(0, 29, 0x18), AIR),
    ("game_task", 0x09A99A50): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A99A60): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A99A70): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A99AF8): ("lwc1 $f2, 0x10($sp)", scaled_load(2, 29, 0x10), AIR),
    ("game_task", 0x09A99AFC): ("lwc1 $f1, 0x14($sp)", scaled_load(1, 29, 0x14), AIR),
    ("game_task", 0x09A99B00): ("lwc1 $f0, 0x18($sp)", scaled_load(0, 29, 0x18), AIR),
    ("game_task", 0x09A9C670): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A9C680): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A9C690): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", AIR),
    ("game_task", 0x09A9C718): ("lwc1 $f2, 0x10($sp)", scaled_load(2, 29, 0x10), AIR),
    ("game_task", 0x09A9C71C): ("lwc1 $f1, 0x14($sp)", scaled_load(1, 29, 0x14), AIR),
    ("game_task", 0x09A9C720): ("lwc1 $f0, 0x18($sp)", scaled_load(0, 29, 0x18), AIR),
