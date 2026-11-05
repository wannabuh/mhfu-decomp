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
# How long L has been held (EBOOT 088D61CC, the local player's +0x61C: +1 a game frame while held; at 10 the item
# bar's choice opens, game_task 09A6EE48 / 09A6A94C): counted in 30ths
L_HELD = "how long L is held (the item bar's choice opens at 10): counted in 30ths"
# A character's turn to where it is going (EBOOT 088D6B00): its facing (+0x1F4) steps by so much a game frame, its lean
# (+0x41A) by 0x80 - times the time scale
TURN = "a character's turn and lean: their steps a game frame times the time scale"
# The hunter's sprint (game_task 09A9E288): a push of 5 units forward a game frame on top of the motion's
SPRINT = "the sprint's push (units a game frame) times the time scale"
# Stamina (+0x55A) used or got back a game frame (game_task 09A6A370 and 09A6A480: so much a frame, less with the
# skills - they test the hunt's count of 30ths, *0x089CC558 + 0x1C; their callers, all a point or two a frame: the
# sprints 09A9E288 and 09A9E690, 09A99228, the weapons' 09BA67D0 09BA72F8 09BA78D8 09BB6D10) and the states that get it
# back so much a frame (09A9EAE8 out of breath +4, 09A8F990 +2): at 30ths
STAMINA = "stamina used or got back so much a game frame: at 30ths"
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
# The air's step (EBOOT 08865E94, a character's: velocity +0x240 += acceleration +0x250, place +0x200 += velocity, in
# VFPU columns C010, C020, C000) - the hunter's jump up to a ledge, among others
AIRSTEP = "a character's step in the air (velocity and place, a game frame's) times the time scale"

# The Snowy Mountains' haze, left out with MHFU_SNOW_HAZE=0 (gpu.c rt_snow_haze): the dome of mist round the map (an
# object's draw method, game_sub 09CC0CE0, once a frame in the render pass: in the snowfields and the ice cave, not
# elsewhere) - big flat triangles whose edges show as the camera turns - and the snowstorm's sheets of fog (the snow
# weather's draw method, game_sub 09C57E08, called by 09C56EC0)
SNOW = "the Snowy Mountains' haze drawn, unless MHFU_SNOW_HAZE=0"

# The hunter's countdowns in its byte +0x1D7 (an action's own: starting to run counts 9 game frames down) - each "while
# above 0, less 1" a game frame, counted in 30ths; not its other steps, which are events (a greatsword's charge levels;
# a count taken off as another timer runs out). And how long R (the sprint) has been held, up to 10 (+0x11B0, game_task
# 09A703F0): found by tools/ratecheck.py, twice as fast at 60
COUNTDOWN = "a hunter's countdown (+0x1D7) or R held (+0x11B0): its game frames counted in 30ths"
# The herd animals' countdown +0x54E (game_task 09AC5C30, from their tick 09AC4950: a minute or so, a game frame at a
# time): found by tools/ratecheck.py with their vtable (089BC680) converted
COUNTDOWN54E = "a herd animal's countdown +0x54E: 1 less a 30th, not a game frame"

# A model's colour easing to where it goes (EBOOT 08860CB0, drawing - a creature as it comes, flashes): a fifth of the
# way a game frame (its three channels), in game time (native.c rt_ease) - found by tools/ratecheck.py
EASE = "a colour's ease: a fifth of the way a game frame, in game time"

# A character's countdown +0x33C (EBOOT 08865A2C, from its tick: a pending amount, +0x396, added to it, else 1 less a
# game frame - through a greatsword's attacks): found by tools/ratecheck.py
COUNTDOWN33C = "a character's countdown +0x33C: 1 less a 30th, not a game frame"

# Counts of game frames in a module's objects (recomp.py count_step): every "addiu $x, $x, 1" (or -1) stored back to one
# of these fields counts a 30th (rt_tick30), not a game frame. In the hunt's code (game_task): an action's count
# (+0x414, some 580 places: a ledge's hang, a greatsword's charge running out...) and a charge's time (+0x672).
COUNTED = {
    "game_task": {0x414, 0x672},
    # the large monsters (their overlays, em*): their action's count at +0x414 too - as at 30 while they are (gated),
    # in game time once their vtables are converted
    **{f"em{n:02d}": {0x414} for n in (1, 2, 7, 14, 15, 17, 20, 21, 33, 40, 54, 55, 58, 59, 75, 82, 83)},
}

# Functions rewritten by hand, readably (readable/*.c): (module, address) -> the C function to run instead. The generated
# code stays behind it: MHFU_HAND=0 runs that, so one build can be checked against itself (docs/readable.md).
HAND = {
    ("game_task", 0x09A6A2D0): "hand_hunter_stamina_add",
    ("game_task", 0x09A6A370): "hand_hunter_stamina_use",
    ("game_task", 0x09A6A480): "hand_hunter_stamina_use_alternate_ticks",
    ("game_task", 0x09A9E288): "hand_hunter_state_sprint",
    ("game_task", 0x09A9E690): "hand_hunter_state_sprint_linked",
    ("game_task", 0x09A9EAE8): "hand_hunter_state_out_of_breath",
    ("game_task", 0x09A68FC8): "hand_hunter_tick_upkeep",
    ("game_task", 0x09A6A808): "hand_hunter_stamina_loss_blocked",
    ("EBOOT", 0x088D6A1C): "hand_hunter_has_active_skill",
    ("EBOOT", 0x088D6A54): "hand_hunter_has_hunt_bonus",
    ("game_task", 0x09A67878): "hand_hunter_set_state_noted",
    ("game_task", 0x09A8B578): "hand_hunter_face_stick_direction",
    ("EBOOT", 0x08865A64): "hand_object_set_state_flags",
    ("EBOOT", 0x08865CB8): "hand_object_state_is",
    ("game_task", 0x09A6F5C0): "hand_object_set_movement_mode_and_flags",
    ("game_task", 0x09AA5FB0): "hand_hunter_classify_move_input",
    ("EBOOT", 0x088D6488): "hand_hunter_tick_countdowns",
    ("game_task", 0x09A8B3F0): "hand_hunter_clear_overlays",
    ("game_task", 0x09A67F30): "hand_hunter_start_airborne_motion",
    ("game_task", 0x09A68700): "hand_hunter_clear_transient_action_fields",
    ("game_task", 0x09A67630): "hand_hunter_set_state",
    ("game_task", 0x09A678F8): "hand_hunter_reset_to_base_state_and_motion",
    ("game_task", 0x09A67568): "hand_hunter_run_state",
    ("game_task", 0x09A9A9E0): "hand_hunter_group0_state",
    ("game_task", 0x09A66198): "hand_hunter_state_function",
    ("EBOOT", 0x088D5C34): "hand_hunter_set_motion",
    ("EBOOT", 0x088D1DD0): "hand_hunter_tick_motion_copy",
    ("EBOOT", 0x088D61CC): "hand_hunter_copy_pad",
    ("game_task", 0x09AA5270): "hand_hunter_common_input",
    ("game_task", 0x09A6A598): "hand_hunter_stamina_add_scaled",
    ("EBOOT", 0x088D5C7C): "hand_hunter_motion_update",
    ("game_task", 0x09A665F0): "hand_hunter_state_run",
    ("EBOOT", 0x088D6594): "hand_hunter_add_health",
    ("game_task", 0x09A6C968): "hand_hunter_can_guard_received_hit",
    ("EBOOT", 0x088D92F8): "hand_hunter_recalc_defense_value",
    ("game_task", 0x09A6B130): "hand_hunter_process_received_hit",
    ("game_task", 0x09AC8450): "hand_entity_sum_pending_damage_and_add_buildups",
    ("game_task", 0x09AC7A08): "hand_entity_apply_pending_damage_and_choose_result_code",
    ("EBOOT", 0x088D96BC): "hand_hunter_recalc_resist_values",
    ("EBOOT", 0x088D9880): "hand_hunter_resist_values_add_skill_bonuses",
    ("EBOOT", 0x088D9EB8): "hand_hunter_add_resist_value_by_id",
    ("EBOOT", 0x088D9F3C): "hand_hunter_resist_values_add_game_bytes_6993d",
    ("game_task", 0x09AC9318): "hand_entity_pending_damage_scale_from_quest_table",
    ("game_task", 0x09A6FB30): "hand_hunter_clear_received_hit_fields",
    ("game_task", 0x09BA0AE0): "hand_hunter_weapon0_state_dispatch",
    ("game_task", 0x09BA0CD8): "hand_hunter_weapon0_state_motion_057e",
    ("game_task", 0x09BA1108): "hand_hunter_weapon0_state_motion_057f",
    ("game_task", 0x09BA15D8): "hand_hunter_weapon0_state_motion_0594",
    ("game_task", 0x09BA1E38): "hand_hunter_weapon0_state_motion_057c",
    ("game_task", 0x09BA1FB8): "hand_hunter_weapon0_state_motion_0583",
    ("game_task", 0x09BA2E28): "hand_hunter_weapon0_chain_kind",
    ("game_task", 0x09BA2E68): "hand_hunter_weapon0_chain_to_state",
    ("game_task", 0x09BA32E8): "hand_hunter_weapon0_connected_reaction",
    ("game_task", 0x09BA2060): "hand_hunter_weapon0_state_charge",
    ("game_task", 0x09BA2608): "hand_hunter_weapon0_state_motion_0594_charged",
    ("game_task", 0x09BA2D10): "hand_hunter_weapon0_pick_state_from_field_1120",
    ("game_task", 0x09BA31B0): "hand_hunter_weapon0_enter_state_14",
    ("game_task", 0x09BA3200): "hand_hunter_weapon0_apply_tier",
    ("EBOOT", 0x088D1BA8): "hand_hunter_field_672_tier",
    ("game_sub", 0x09C43558): "hand_entity_apply_hit_hitzone_damage_and_attacker_sharpness",
    ("game_sub", 0x09C45008): "hand_object_apply_hit_mask_28_amounts",
    ("game_sub", 0x09C43000): "hand_hunter_register_hit_add_defense_scaled_damage",
    ("game_sub", 0x09C426E0): "hand_hit_node_test_target_if_in_current_map_and_state_2",
    ("game_sub", 0x09C41F58): "hand_hit_manager_test_nodes_against_targets",
    ("game_sub", 0x09C42750): "hand_hit_node_test_target_and_apply",
}

# A character's place stepped in its state functions (recomp.py place_step): "place += step" stored straight back to
# +0x200 or +0x208 (x, z) - the step a displacement worked out for the frame (a speed along the facing: a Popo knocked
# back, game_task 09B01A28, 10.9 units a game frame) or a velocity (+0x240). Each times the time scale: as at 30 for
# what is gated (the time scale 1 there), in game time for a converted object. Some 60 places in the hunter's, the
# herd animals' and the other creatures' code (game_task) and the large monsters' (em*); not where the step is scaled
# already by a patch just before (falls, leaps, the sprint)
PLACED = {
    "game_task": {0x200, 0x208},
    "em": {0x200, 0x208},
}
# Not those: a place moved once, by a distance of its own (along the facing), not a step for the frame - scaled, it went
# only part of the way. The hunter's grab of a ledge (game_task 09A9D0B0: 55 units forward onto it; at 60 only half,
# so the ground below was not the ledge's - he fell off at once and grabbed it again, round and round) and his drop
# to hang from one (09A9D478: 30 back, 195 down)
NOT_PLACED = {("game_task", a) for a in (0x09A9D328, 0x09A9D340, 0x09A9D578, 0x09A9D584)}

# The hunt's camera (EBOOT 08886E50, view at $s4 - 0xA0, its yaw to turn to at +0x80) turns itself where the scenery is
# in the way: it casts rays between the eye and the hunter; one hit, it turns 0x400 a frame (5.6 degrees) until it is
# clear; the line of sight lost (+0x89), it swings an eighth of the way behind the hunter a frame. With the mouse
# turning the camera (rt_camera_free) those turns are left out: the camera goes where the mouse puts it.
CAMERA = "the hunt's camera: its own turns away from the scenery, left out while the mouse has the camera"

# The test of a model's sphere against the camera's view (EBOOT 088160F0: a0 the scene, a1 the centre, f12 the radius;
# 1 if it is to be drawn): what is drawn casts the renderer's shadows, so with them a model is drawn when within
# rt_cull_margin (units) of the view - the shadows of what is just out of view do not come and go as the camera turns.
CULL = "the view test of a model's sphere: the sphere larger by rt_cull_margin (the shadows of what is just out of view)"

# The game's list windows (the menus, shops, quest lists...) are drawn by EBOOT 0882F580 (a0 the drawer, a1 the
# window's layout, a2 its title, a3 the row the cursor's bar is on, below 0 none): noted for the mouse (menu.c)
MENU = "a list window drawn: its layout and the cursor's row noted, for the mouse to point at its rows (menu.c)"
# The cursor's bar (pulsing), drawn by EBOOT 0882F4A4 and 08831F3C (a0 the drawer, a1 x, a2 y, a3 width, t0 height):
# for the lists drawn otherwise, noted too
BAR = "a menu cursor's bar drawn: its place noted, for the mouse to move it (menu.c)"
# The quest lists' plates (lobby_task 09A8C068, a plate a row: EBOOT 0882AF70 with a1 x, a2 y, a3 width, t0 height;
# s3 the row, the cursor's at +0x62 of the list, s5): each noted as it is laid out, before the call (and the cursor's
# sound made there, for a cursor the mouse has put on another plate)
ITEM = "a quest list's plate drawn: its place noted, and whether the cursor is on it, for the mouse (menu.c)"
# The arrows either side of a page number or a setting (EBOOT 0882E6F0: a1 x, a2 y, a3 from the left one to the right):
# noted, for a click on one to press left or right
ARROWS = "a left and right arrow drawn: their place noted, for a click on one to press left or right (menu.c)"
# The menus' cursors (EBOOT 0881C2E4: a1 the cursor's byte, a2 the buttons, up or down a row, a3 how many rows, round
# and round): where the mouse points another row, the cursor put next to it and the step to it pressed, at once
CURSOR = "a menu's cursor moved by the buttons: to the row the mouse points at, in one step (menu.c)"
# The Options (EBOOT 08839824, its window drawn with no cursor's bar: the setting's arrows on the cursor's row, s0+0x57F)
OPTIONS = "the Options' window drawn: its layout (sp+0x38) and the cursor's row noted, for the mouse (menu.c)"
# The supply box (EBOOT 08847CB0, drawn: 24 slots in rows of 8, the first at 230,54, 28 and 24 pixels apart, 26 square;
# its cursor the hunter's byte +0x55D, $s7 the hunter; EBOOT 088476F4 moves it, with the sound 0x16): noted as its
# cursor's frame is about to be drawn, and the sound made there for a cursor the mouse has put on another slot
BOX = "the supply box's slots drawn: their places and the cursor noted, for the mouse (menu.c)"
# An item taken from the supply box (EBOOT 08847B74): the box's input ignored for 30 frames after (the hunter's byte
# +0x55E, counted down by 088476F4 at 30ths) - a second between items; 3 instead, enough not to take twice on one press
BOX_WAIT = "the supply box's wait after an item is taken: a tenth of a second, not a second"
# The hunt's HUD (EBOOT 0883F270, the hunt's tick's last call): its gauges (0883F9F8, called at 0883F84C), and in
# 0883FAD4 the map (08842EE8, at 0883FD18) and the item bar (08845A34, at 0883FD20) - each told apart while it is drawn
# (rt_hud_part: the chunks it links, the text it queues - EBOOT 08891474 and its kin add a text's entry, 0889377C
# draws one; 08895220 adds a number, 08892A88 draws them in turn), to be made smaller towards its corner (gpu.c,
# MHFU_HUD_SCALE); not the menus' windows nor the supply box drawn there too, nor other windows
HUD = "the hunt's HUD's parts (gauges, map, item bar) told apart as they are drawn, to be scaled (gpu.c)"
# The HUD's sprites (EBOOT 0882AF70: a1 x, a2 y, a3 width, t0 height, t1 the sprite; 0882A9DC: a1 x, a2 y, a3 the
# sprite, at its own size - the item bar's buttons): the PSP's buttons among them left out where the renderer shows
# the keys or gamepad buttons bound instead (glyphs.c)
SPRITE = "a HUD sprite drawn: a PSP button's left out, the input bound shown in its place (glyphs.c)"


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
    # the monsters' (game_task 09AB1498, the monster manager's run, in the hunt's tick at 09A627FC): the loop over their
    # objects (088666B4, list 3) made every frame - each one's tick gated by itself (088666E4, above), so that a
    # monster's ticks every frame once its vtable is converted (MHFU_NATIVE_VT) and as at 30 till then
    ("game_task", 0x09AB14B8): ("the monsters' ticks (088666B4, list 3), every frame", "native"),
    # the hits (game_sub 09C41E78, in the hunt's at 09A62774, after the objects' ticks): each player's and monster's hit
    # records cleared (09C41EA0: their vtables' +0x2C - EBOOT 08865EC0, a hit's flag +0x285 and its damage), then the
    # new hits found (09C41F58, as at 30). An object takes a hit in its tick (a Popo: game_task 09AC7A08) while the
    # flag is up - till the next clear: a converted one, ticking twice between 30ths, took each hit twice. The clear
    # made every frame, each object's by itself: a converted object's every frame (after its one tick with the hit),
    # the others' at 30ths, after theirs
    ("game_sub", 0x09C41E80): ("the hit records cleared (09C41EA0), every frame", "native"),
    ("game_sub", 0x09C41ED8): ("a player's hit records cleared (its vtable's +0x2C)", "object"),
    ("game_sub", 0x09C41F2C): ("a monster's hit records cleared (its vtable's +0x2C)", "object"),
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
    ("EBOOT", 0x088D6348): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", L_HELD),
    ("EBOOT", 0x088D6C0C): ("lhu $a0, 0x324($s0)", "c->r[9] = (uint32_t)(int32_t)((float)(int32_t)c->r[9] * rt_time_scale + 0.5f); "
                             "c->r[4] = LHU((c->r[16] + 0x324u));", TURN),
    ("EBOOT", 0x088D6C20): ("lw $a3, 0x1F4($s0)", "c->r[8] = (uint32_t)(int32_t)((float)(int32_t)c->r[8] * rt_time_scale + 0.5f); "
                             "c->r[7] = LW((c->r[16] + 0x1F4u));", TURN),
    ("game_task", 0x09A9E4F4): ("lwc1 $f1, 0x10($sp)", scaled_load(1, 29, 0x10), SPRINT),
    ("game_task", 0x09A9E4F8): ("lwc1 $f0, 0x18($sp)", scaled_load(0, 29, 0x18), SPRINT),
    ("game_task", 0x09A6A370): ("addiu $sp, $sp, -0x10", "if (!rt_tick30) return; c->r[29] = c->r[29] + 0xFFFFFFF0u;", STAMINA),
    ("game_task", 0x09A6A480): ("addiu $sp, $sp, -0x10", "if (!rt_tick30) return; c->r[29] = c->r[29] + 0xFFFFFFF0u;", STAMINA),
    ("game_task", 0x09A9EAFC): ("addiu $a1, $zero, 0x4", "c->r[5] = rt_tick30 ? 4u : 0;", STAMINA),
    ("game_task", 0x09A8FBB0): ("addiu $a1, $zero, 0x2", "c->r[5] = rt_tick30 ? 2u : 0;", STAMINA),
    ("EBOOT", 0x08864368): ("lwc1 $f0, 0x14($v1)", scaled_load(0, 3, 0x14), REACHED),
    ("EBOOT", 0x08863894): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", BLEND),
    ("EBOOT", 0x088638A0): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", BLEND),
    ("game_sub", 0x09CC0CE0): ("addiu $sp, $sp, -0x110", "{ extern int rt_snow_haze(void); if (!rt_snow_haze()) return; } "
                               "c->r[29] = c->r[29] + 0xFFFFFEF0u;", SNOW),
    ("game_sub", 0x09C57E08): ("addiu $sp, $sp, -0x140", "{ extern int rt_snow_haze(void); if (!rt_snow_haze()) return; } "
                               "c->r[29] = c->r[29] + 0xFFFFFEC0u;", SNOW),
    ("EBOOT", 0x08865EAC): ("vadd.t C010, C010, C020", "for (int i = 0; i < 3; i++) c->v[1 + 32 * i] += c->v[2 + 32 * i] * rt_time_scale;",
                            AIRSTEP),
    ("EBOOT", 0x08865EB0): ("vadd.t C000, C000, C010", "for (int i = 0; i < 3; i++) c->v[32 * i] += c->v[1 + 32 * i] * rt_time_scale;", AIRSTEP),
    ("game_task", 0x09AC65F4): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", COUNTDOWN54E),
    ("EBOOT", 0x08860DB4): ("div.s $f2, $f2, $f3", "{ extern float rt_ease(float x, float n); c->f[2].f = rt_ease(c->f[2].f, c->f[3].f); }", EASE),
    ("EBOOT", 0x08860DC0): ("div.s $f1, $f1, $f3", "{ extern float rt_ease(float x, float n); c->f[1].f = rt_ease(c->f[1].f, c->f[3].f); }", EASE),
    ("EBOOT", 0x08860DC4): ("div.s $f0, $f0, $f3", "{ extern float rt_ease(float x, float n); c->f[0].f = rt_ease(c->f[0].f, c->f[3].f); }", EASE),
    ("EBOOT", 0x08865A54): ("addiu $v1, $v1, -0x1", "c->r[3] = c->r[3] - (uint32_t)rt_tick30;", COUNTDOWN33C),
    ("game_task", 0x09A8CAF0): ("addiu $v1, $v1, -0x1", "c->r[3] = c->r[3] - (uint32_t)rt_tick30;", COUNTDOWN),
    ("game_task", 0x09A8FAC0): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", COUNTDOWN),
    ("game_task", 0x09A8FDE4): ("addiu $v1, $v1, -0x1", "c->r[3] = c->r[3] - (uint32_t)rt_tick30;", COUNTDOWN),
    ("game_task", 0x09A9BCA4): ("addiu $v1, $v1, -0x1", "c->r[3] = c->r[3] - (uint32_t)rt_tick30;", COUNTDOWN),
    ("game_task", 0x09BAE714): ("addiu $v1, $v1, -0x1", "c->r[3] = c->r[3] - (uint32_t)rt_tick30;", COUNTDOWN),
    ("game_task", 0x09A70464): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_tick30;", COUNTDOWN),
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
    ("EBOOT", 0x08887608): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x08887C58): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x0888FD58): ("lui $v0, 0x8A6", "rt_frame_start(); c->r[2] = 0x8A60000u;", FRAME),
    ("EBOOT", 0x088596C8): ("sll $v0, $a3, 2", "rt_list_link(c->r[4], c->r[5], c->r[6], c->r[7]); c->r[2] = c->r[7] << 2;", LINK),
    # the screen's tint (EBOOT 088CACD4, the render pass's at layer 8: the object at *0x09A4F1A0, its +0x3D4)
    ("EBOOT", 0x088CAEB4): ("sb $zero, 0x3D4($s0)", "if (rt_next_frame30()) SB((c->r[16] + 0x3D4u), 0);", ONCE),
    ("EBOOT", 0x0888FDF0): ("addiu $v1, $v1, 0x1", "c->r[3] = c->r[3] + (uint32_t)rt_frame30;", COUNT),
    ("EBOOT", 0x0882F4A4): ("addiu $sp, $sp, -0x30", "rt_menu_bar(c->r[5], c->r[6], c->r[7], c->r[8]); c->r[29] = c->r[29] + 0xFFFFFFD0u;", BAR),
    ("EBOOT", 0x08831F3C): ("addiu $sp, $sp, -0x30", "rt_menu_bar(c->r[5], c->r[6], c->r[7], c->r[8]); c->r[29] = c->r[29] + 0xFFFFFFD0u;", BAR),
    ("lobby_task", 0x09A8C210): ("andi $a2, $v0, 0xFFFF", "c->r[6] = c->r[2] & 0xFFFFu; "
                                 "rt_menu_item(c->r[5], c->r[6], c->r[7], c->r[8], c->r[19] == (uint32_t)LH(c->r[21] + 0x62u), c->r[21] + 0x62u); "
                                 "rt_menu_sound(c);", ITEM),
    ("EBOOT", 0x0882F580): ("addiu $sp, $sp, -0x40", "rt_menu_list(c->r[5], c->r[7]); c->r[29] = c->r[29] + 0xFFFFFFC0u;", MENU),
    ("EBOOT", 0x0881C2E4): ("addiu $sp, $sp, -0x20", "rt_menu_cursor(c->r[5], c->r[7], &c->r[6]); c->r[29] = c->r[29] + 0xFFFFFFE0u;", CURSOR),
    ("EBOOT", 0x0882E6F0): ("addiu $sp, $sp, -0x20", "rt_menu_arrows(c->r[5], c->r[6], c->r[7]); c->r[29] = c->r[29] + 0xFFFFFFE0u;", ARROWS),
    ("EBOOT", 0x08839AE8): ("lhu $v1, 0x48($sp)", "rt_menu_list(c->r[29] + 0x38u, c->r[6]); c->r[3] = LHU((c->r[29] + 0x48u));", OPTIONS),
    ("EBOOT", 0x08848094): ("addiu $a3, $zero, 0x1A", "rt_menu_grid(0xE6, 0x36, 28, 24, 0x1A, 0x1A, 8, 24, c->r[23] + 0x55Du, 0x16); "
                            "rt_menu_sound(c); c->r[7] = 0x1Au;", BOX),
    ("EBOOT", 0x08847B98): ("addiu $v0, $zero, 0x1E", "c->r[2] = 3u;", BOX_WAIT),
    ("EBOOT", 0x0883F9F8): ("addiu $sp, $sp, -0x10", "rt_hud_part(c->r[31] == 0x0883F854u ? 1 : 0); c->r[29] = c->r[29] + 0xFFFFFFF0u;", HUD),
    ("EBOOT", 0x0883FAD4): ("addiu $sp, $sp, -0x10", "rt_hud_part(0); c->r[29] = c->r[29] + 0xFFFFFFF0u;", HUD),
    ("EBOOT", 0x08842EE8): ("addiu $sp, $sp, -0x10", "rt_hud_part(c->r[31] == 0x0883FD20u ? 2 : 0); c->r[29] = c->r[29] + 0xFFFFFFF0u;", HUD),
    ("EBOOT", 0x08845A34): ("addiu $sp, $sp, -0x20", "rt_hud_part(c->r[31] == 0x0883FD28u ? 3 : 0); c->r[29] = c->r[29] + 0xFFFFFFE0u;", HUD),
    ("EBOOT", 0x0883FD28): ("addu $a0, $s0, $zero", "rt_hud_part(0); c->r[4] = c->r[16] + 0;", HUD),
    ("EBOOT", 0x088914F8): ("sb $v0, 0x134($v1)", "SB((c->r[3] + 0x134u), c->r[2]); rt_hud_text_queued(c->r[7]);", HUD),
    ("EBOOT", 0x08891644): ("sb $v0, 0x134($v1)", "SB((c->r[3] + 0x134u), c->r[2]); rt_hud_text_queued(c->r[7]);", HUD),
    ("EBOOT", 0x08891784): ("sb $v0, 0x134($v1)", "SB((c->r[3] + 0x134u), c->r[2]); rt_hud_text_queued(c->r[7]);", HUD),
    ("EBOOT", 0x088918D0): ("sb $v0, 0x134($v1)", "SB((c->r[3] + 0x134u), c->r[2]); rt_hud_text_queued(c->r[7]);", HUD),
    ("EBOOT", 0x0889377C): ("addiu $sp, $sp, -0x20", "rt_hud_text_drawn(c->r[5]); c->r[29] = c->r[29] + 0xFFFFFFE0u;", HUD),
    ("EBOOT", 0x08893888): ("lw $s0, 0x8($sp)", "rt_hud_part(0); c->r[16] = LW((c->r[29] + 0x8u));", HUD),
    ("EBOOT", 0x08895274): ("sh $a1, 0x0($v1)", "SH((c->r[3] + 0x0u), c->r[5]); rt_hud_text_queued(c->r[3]);", HUD),
    ("EBOOT", 0x08892CAC): ("lbu $a0, 0x6($s4)", "rt_hud_text_drawn(c->r[20]); c->r[4] = LBU((c->r[20] + 0x6u));", HUD),
    ("EBOOT", 0x08892E50): ("lw $fp, 0x28($sp)", "rt_hud_part(0); c->r[30] = LW((c->r[29] + 0x28u));", HUD),
    ("EBOOT", 0x0882AF70): ("addiu $sp, $sp, -0x70", "if (rt_sprite(c, c->r[9], c->r[5], c->r[6], c->r[7], c->r[8])) return; "
                            "c->r[29] = c->r[29] + 0xFFFFFF90u;", SPRITE),
    ("EBOOT", 0x0882A9DC): ("addiu $sp, $sp, -0x70", "if (rt_sprite(c, c->r[7], c->r[5], c->r[6], -1, -1)) return; "
                            "c->r[29] = c->r[29] + 0xFFFFFF90u;", SPRITE),
    ("EBOOT", 0x088160F0): ("lbu $v0, 0xAAA($a0)", "c->r[2] = LBU((c->r[4] + 0xAAAu)); c->f[12].f += rt_cull_margin;", CULL),
}
