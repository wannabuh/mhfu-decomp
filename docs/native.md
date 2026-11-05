# Towards the native frame rate

The game runs 30 frames a second ("ticks") and counts time in them: nothing reads a clock, there is no time step. To
run it at the display's rate, what advances by so much a tick has to advance by that much times 30 / the rate. Until
then `MHFU_FPS` shows the frames in between interpolated (`docs/runtime.md`, Frame rate), with the camera turned by the
mouse at the display's rate (`runtime/src/camera.c`).

## How a tick runs

`user_main` (thread entry `EBOOT 08906B68` → `08897E1C`) loops over `EBOOT 0888FD38`:

| call | what |
|---|---|
| `088590B0` | wait: the vblank handler (`08859188`) wakes it every second vblank |
| `08868F50` | the pad |
| `08890BBC` | ? |
| (indirect) | the mode's main |
| `088DDA44`, `0884C22C` | ? |
| `08895538` | the game states: four slots of objects with vtables (`*0x08A8B308`), each run by its +0xC |
| `0884BB58` | **the render pass**: every model is drawn here (`0884CDE4` puts its world matrix in the GE list), after the update |
| `0884BB00`, `08880708`, `08891B14` | ? (the end of the frame, sound...) |

Drawing is a pass of its own after the logic, so the game can draw a frame without running one.

In a hunt the state's run is `game_task 09A60298` → `09A62740` → `09A627B8`, which updates the objects
(`EBOOT 088DB068` → `088DB250` → `088666B4` over the object list), then the cameras (`08812BD0`), and more.

**Objects** are C++: a vtable at +0, and a member function pointer at +0x3FC (`{delta, index, function}`, called by
`0890C490`) - the state it is in. `08864634` runs an object a tick: its state's function, then its vtable's +0x18 and
+0x1C. The hunter's state function is `game_task 09A665F0` (`09AA94C8` in the village's `lobby_task`). Every timer,
speed and wait in those functions is a count of ticks: there are thousands.

**The hunter** (in a hunt; found from its vtable `089BB3CC`, whose +0x38 is its state function `game_task 09A66198`):
+0x10 its rotation (3x3, rows of 4), +0x40 its position, +0x90 / +0xD0 a motion layer's frame and +0x9C / +0xDC its
end, +0x1F4 its facing (16 bits), +0x200 / +0x210 its position now and before, +0x3F4 ticks in its state (counting up:
a timer to convert). Starting to walk it turns at once: there is no turning rate.

**Motions** (animations) are made at 60 frames a second. The motion player, `EBOOT 088637C4`, called from
`08865648` ← `088D5C7C` (an object's motion update), advances each layer's frame (+0x10) by its speed (+0x14) a tick -
2.0 normally; 0 holds a frame (hit-stop); a blend steps a weight (+0x34 by +0x24) and counts down ticks (+0x30, an
integer). Root motion (+0x40..+0x48) is the motion's own displacement, so it follows the frame.

**The camera**: the scene (`*0x089CBD04`) has six views (+0xB0, 0x1A0 each); a view's controller is a member function
pointer at +0x180 (`08886E50` the hunt's, `08888B98` the village's fixed one), called by `08886B2C` from `08812BD0`.

## What there is to do it with

- **Patches** (`recomp/patches.py`): (module, address) → the instruction expected there (checked, so a patch never
  lands on something else after the code moves), the C to run instead, why. The generated code marks them
  `/* patched */`; the build fails if one is not applied.
- **`MHFU_TICK_HZ=N[@frame]`** (an experiment): the game runs N ticks a second (from that game frame: a recording made
  at 30 gets somewhere, the game carries on at N - the way to try a conversion in a hunt) (its vblank handler wakes it every second vblank,
  so the vblanks come 2N a second) and `rt_time_scale` is 30 / N, which patches multiply by. At 30 the scale is 1 and
  the patches change nothing.
- **Finding what counts ticks**: `tools/ramdiff.py` over RAM snapshots (`MHFU_SNAPSHOT=frames`, or F9) finds variables
  that move by a constant a tick (the motion frames: +2.0); `tools/watch.py ADDR --frame N` finds the code that writes
  them; a recording (`MHFU_REPLAY`) gets there the same way every time.

## Gates: what is not converted runs as at 30

Everything the game's main loop (`0888FD38`) runs but the render pass (`0884BB58`) is behind a **gate**
(`recomp/patches.py`'s `GATES`, `runtime/src/native.c`): at any rate it runs on the frames a 30th falls on
(`rt_frame30`), with the time scale 1 - as at 30 a second - and is skipped on the frames between. The render pass draws
every frame. So at `MHFU_TICK_HZ=N` the game is the game at 30, each of its frames shown N / 30 times - exactly: at 60 and
144 every frame shown is the same picture, to the pixel, as one at 30 (reference settings: the hunt, the camp, the
grass field, the guild hall, the hunt's menu). Converting is opening gates.

- A gate is on a function (its start: every caller) or a call (`jal` / `jalr`: that call). The generated code marks
  them `/* gated */`; the build fails if one is neither. `"before"` gates run on the frame before a 30th instead: the
  render queues (`0884BB00` empties what the logic queued) are emptied just before the logic fills them again, so the
  frames between draw what was queued.
- Much is drawn by the logic itself - the HUD (`0883F270`, the hunt's tick's last call), the text (`08891B14`), the
  screen's state for the render pass (`0884C22C`) - into display list chunks it links into the frame's list by layer
  (`088596C8`, patched to tell `rt_list_link`). The chunks linked from gated code at a 30th are noted; the GE keeps the
  commands it runs from them (`gpu.c`: state, and primitives with copies of their vertices); on the frames between, the
  skipped gate links a three-word chunk in each one's place (`NOP 0xA5nnnn`) where the GE runs them again. What a chunk
  drew stays on screen, in its place among the layers, until its code runs again.
- A word the logic sets each frame for the render pass, which clears it once drawn, is cleared only before a 30th
  (`"once"` patches: the screen's tint, `088CACD4`).
- The display lists are double-buffered (`08859224` flips them, the GE runs the one made the frame before): the
  primitives kept are drawn into this frame's buffers.
- The renderer's clocks (wind, grass, the moment shown) go by the game's time (`rt_game_30ths`), not its frame count.
- `MHFU_UNGATE=addr,...` leaves gates open (what they gate runs every frame), to try one; `MHFU_DUMP_FROM=N` with
  `MHFU_DUMP=1` dumps every frame from N, to compare a run at N a second with one at 30.

### Down to the objects: converted calls and object gates

Opening the gates above the objects would not do: the hunt's state run (`game_task 09A60298`) counts its own timers
inline (`*0x089CC558 + 0x488`, counted down as it goes), and so would every caller on the way. Instead what is
converted is made again, under the gate it is in:

- A **"native"** gate is on a call made by gated code. At a 30th the call is made (at the frame's own time scale, not
  the gate's 1) and noted, with its arguments, in order among the chunks the gate's code links; on the frames between,
  the skipped gate makes the noted calls again, in that order, where it would have linked those chunks. So only those
  calls run every frame, not the code around them.
- An **"object"** gate is on an object's call (a0 the object): every frame if the object's vtable is converted
  (the hunter's, `089BB3CC`, and the Popos', `089BC680`; `MHFU_NATIVE_VT=hex,...` instead, 0 for none), else gated as any other - what it drew noted and drawn again for it alone (the
  chunks are keyed by gate and object).

In a hunt the native calls are the object manager's loop over its objects (`088DB250` → `088666B4`, list 2: the
hunter, and a few more; each object's tick, its vtable's +0x14, an object gate), the local player's input
(`088DB424` → `088D61CC`, an object gate), and in the hunt's tick (`09A627B8`) the players' world matrices and poses
(`088DB338` → `088D2894`: without it the hunter is drawn a step behind between 30ths), more of theirs (`088DB39C` →
`088D2E04`) and the cameras (`08812BD0`). The monsters tick elsewhere (still gated whole). With nothing converted
every frame at 60 and 144 is still a frame at 30, to the pixel (`MHFU_NATIVE_SMOOTH=0`, below).

`rt_ticks60` is the 60ths of a second a game frame takes up (2 at 30, 1 at 60, 0 or 1 at 144; 2 under a gate): what
counts in 60ths steps by it. `MHFU_NATIVE_TRACE=file` writes, each game frame, the hunter's place, state, ticks in it
and motion frame and the camera's eye and target (`MHFU_TRACE_VT` for another vtable; `MHFU_TRACE_VIEW=1` adds the
view's words) - compared at the same game times between a run at 30 and one at N. `MHFU_NATIVE_LOG=1` names the
objects the object gates see.

### What still ticks at 30, smoothed (gpu.c)

With the hunter and the camera every frame, the rest - stepping at its 30ths - would judder against them. The render
pass places everything anew each frame, the frame's camera multiplied into each world matrix (the GE keeps 24 bits
of a float: at 20000 units a matrix is good to half a unit), so what ticks at 30 is told by what stays between 30ths:
its bones (no camera in them) and its place with the camera taken out. Each frame's draws in the world are noted as
the GE draws them - key, place, bones - and matched to the last 30th's (in the order copies of a model come, else the
nearest); one that ticks at 30 and moved between the last two 30ths is drawn between those places by how far the frame
is into the 30th (one 30th behind, as `MHFU_FPS` shows the game), with the frame's camera; at a 30th, as the frame
before told. What gated code drew at the 30th and the GE draws again has that 30th's camera in it, taken out as such.
The rest is drawn as the game drew it. (A frame's display list is that game frame's own, made after its cameras: the
camera is the game's as it is drawn, and the 30th that frame's.) `MHFU_NATIVE_SMOOTH=0`: frames as the game draws
them. `MHFU_FPS_STATS` gives the share of draws smoothed.

The HUD and the menus (2D, drawn by gated code) are smoothed the same way (`gpu.c`, the HUD smoothed): each 2D draw
matched to the 30th before's - the same kind and picture (texture coordinates), its sides moved 24 pixels at most, and
over 4 only as the next in the order drawn (so that text that changes is not matched letter to the same letter
elsewhere) - is drawn between its places by how far the frame is into the 30th, its colours too: the stamina and
health gauges, the map's arrow, windows sliding in. What is not matched (other text) is drawn as it is.
`MHFU_HUD_SMOOTH=0`: not smoothed.

Where the game's camera moves only at 30ths (nothing converted: the village), it is shown between its places at the
last two 30ths as what ticks at 30 is (gpu.c `native_camera_between`). When the game is well behind the wall clock (four frames, as it shows one) the next frame is not drawn - never two
running, nothing kept from one frame to the next (a cadence that held on to drawing fewer locked at 48 or 72 frames a
second) - so that it keeps time, and its sound, which goes by the game's clock, does not break up. A frame not drawn
still has its draws noted for the smoothing. `MHFU_FPS_STATS` counts them; `MHFU_NATIVE_SKIP=0`: every frame drawn, the game slowed if need be. `RECOMP_TRACE=hex,...` (at `recomp.py`'s run) and `MHFU_CALL_LOG=1`
log those functions' calls with the game's time - how the sounds' doubling was found; `MHFU_SAS_LOG=1` the sounds
started.

The mouse's camera: in the hunt the game's camera is turned every frame (`camera.c`, as at 30 but each frame: at 144 a
steady 1/144th of the turn a frame); the mouse's raise of the eye, and its orbit where the game keeps its own angles
(the village, the camp tent), are shown as `MHFU_FPS` shows them - each frame drawn with the game's camera turned
about its target (`camera_native_shown`), the world, the shadows and the light shafts with it. `MHFU_FPS` is left off
when the game runs as fast itself; the intros' skip goes by the game's time.

**Both at once** (`tick_hz = 60` with `fps = 144`): the game at 60, each of its frames recorded and the frames shown
interpolated between the last two (`MHFU_FPS`), each drawn for when it is shown - so the display's timing (its refresh,
VRR, a late wake-up) never shows as judder, as it does with the game's frames shown as they come. What ticks at 30 is
recorded smoothed (the same smoothing, as the draws are recorded); where the game's camera moves only at 30ths,
`camera.c` gives the interpolation the camera between its 30ths (`camera_between_30ths`, one 30th behind), and keeps the
game's for taking it out of the draws. What is shown trails the game by a frame of its: a 60th, against MHFU_FPS's 30th.
`MHFU_MOTION_LOG=file` logs where each skinned model lands on the screen in each frame shown, whatever draws it
(how evenly things move: 0.002 px from a smooth path at 30 and at 60 with the smoothing, 0.03 without); `MHFU_FPS_STATS`
gives the game's thread's time a frame shown. The frames shown cost the game's thread 6-7 ms each (the vertex stage
again for each), near all of a 144th.

## Converted

| system | patch | at 60 ticks |
|---|---|---|
| motion frames | the four loads of a layer's speed in `088637C4` | motions play at their speed; walking too (root motion: about 500 units a second at either rate) |
| motion blends | the weight's step (`08863894`), the count of frames (`088638A0`, on `rt_tick30`) | blends take their time |
| root motion | the three loads of a layer's speed in `08863B70` (the step from a layer's frame to the next, before the player) | the displacement is the frame's, not a tick's |
| a character's counts | `08865934`: ticks in its state (+0x3F4) and +0x45C by `rt_ticks60`, the byte +0x276 down on `rt_tick30` | its state's timing |
| the hunter (`MHFU_NATIVE_VT=089bb3cc`) | object gates; its input, matrices, pose (above) | walks as at 30: the same distance and state timing (within a 60th) |
| the hunt's cameras | a native call (`08812BD0`) | follow every frame |
| the local player's buttons | `088D61CC`'s copies of the pad's pressed and released: at 30ths only (the pad is read at 30ths) | a press is seen once, not again between 30ths as a second press (combinations) |
| how long L is held | `088D61CC`: the local player's `+0x61C`, +1 a game frame while L is held (the item bar's choice opens at 10) - a 30th | the item bar's choice opens as late as at 30 |
| the attack buttons' combination | the wait for a second button (`09A68DB0`, +0x1128) counts 30ths | special attacks as at 30 |
| a character's turn | `088D6B00`: the facing's step to where it goes (+0x1F4) and its lean (+0x41A) times the time scale | turns as fast as at 30 |
| the sprint | `09A9E288`: its push forward (5 units a game frame) times the time scale | runs as far |
| stamina so much a frame | `09A6A370` and `09A6A480` (used: the sprint, the flee from a large monster `09A9E690`, `09A99228`, the weapons' states) at 30ths only; got back so much a frame (out of breath `09A9EAE8` +4, `09A8F990` +2) at 30ths | tires and gets breath back as fast |
| the hunter in the air | falling (`09A9C530`) and the other leaps and flights (`09A905F0`, `09A99928`): the velocity's step (+0x240 by +0x250) and the place's (+0x200 by the velocity) times the time scale; the actions' counts (+0x414, 9 places) in 30ths | falls as fast and as far as at 30 (a cliff: 22 30ths, 404 units at 30; 22.5, 411 at 144) - 4.8 times as fast at 144 before |
| an action's counts | every `addiu $x, $x, ±1` stored straight back to +0x414 (an action's count) or +0x672 (a charge's time) in `game_task`: in 30ths (`recomp.py` `count_step`, `patches.py` `COUNTED`; 426 places) | a ledge's hang, a greatsword's charge (it ran out in half the time: 90 30ths at 30), and the long tail of actions timed so |
| an action's countdown, R held | five "while above 0, less 1" of the hunter's byte +0x1D7 (starting to run counts 9 down...), and +0x11B0 (R held, up to 10: `game_task 09A70464`) in 30ths - not +0x1D7's other steps, events (a charge's levels) | found by `tools/ratecheck.py`: twice as fast at 60 before |
| the air's step | `EBOOT 08865E94` (a character's velocity +0x240 += acceleration, place +0x200 += velocity, VFPU): both adds times the time scale | the jump up to a ledge as at 30 (13 30ths; 6.5 at 60 before) |
| a motion's frame reached | `08864348` (3120 callers: sounds, effects, hits at a motion's frame N): N within the player's next step - its speed times the time scale | each fires once; the attacks' sounds were started up to 5 times at 144 |
| a character's upkeep | gated whole (the call of its vtable's +0x3C, `088659AC`): stamina, hunger (a 6-minute count, +0x632), status and skills' timers | as at 30 |
| hits taken | the hit manager (`game_sub 09C41E78`, at 30ths: each player's and monster's hit records cleared, then the new hits found) clears every frame, each object's records by itself (`09C41ED8`, `09C41F2C` object gates): a converted object's after its one tick with the hit, the others' at 30ths | a Popo took each hit twice at 60 (its tick, `game_task 09AC7A08`, takes the damage while the flag +0x285 is up); the hunter would have too |
| hit-stop | `088D5C7C`: `+0x558`, 1 less a 30th (the slowed speed, 0.2, goes through the motion player's patches) | 5 30ths at 30 and 60; 2.5 at 60 before |
| the hunter's drains | `09A6D418`: waits `+0x452` (poison's second), `+0x454` (its loss), `+0x11B6`, `+0x696` (heat) in 30ths; "the hunt's count % n" tests (`09A6D7AC`, `09A6D888`, `09A6DE34`, `game_sub 09CB2510`) at 30ths only: the count holds still between 30ths, so they'd pass on each of its frames | poison, heat and harmful ground as at 30; twice as fast at 60 before |
| the camera's height and shake | `08886E50`: its target's and eye's eases by `rt_ease` (a fraction f a game frame becomes `1 - (1 - f)^scale`, 4 places); its lift over a drop (`0888D1BC`) times the time scale; the shake's countdown (`08814E18`) in 30ths | a half and an eighth of what's left a 30th at 30, 60 and 144; shake as long, stepping at 30ths |
| a weapon's effect ops | drawn every game frame (`09A95EA8`, `09A96954`): the state's step, flag `0x10`, at 30ths only, and op `0x13`'s count (`088D07DC`) | the ops' steps a 30th as at 30, shown with rows poked in: no real weapon with ops run yet |
| a place's steps | every "place += step" stored straight back to +0x200 or +0x208 in `game_task` and the large monsters' overlays: the step (a displacement worked out for the frame, a velocity) times the time scale (`recomp.py` `place_step`, `patches.py` `PLACED`; 113 places, not those scaled by a patch already) | a Popo knocked back 10.9 units a game frame: 250 units at 60, 109 at 30; 126 now |

Checked at 60 and 144 (`MHFU_NATIVE_TRACE` with `MHFU_TRACE_WORDS=684,1`, the hunter's action at +0x684, its kind
at +0x685): walking, a roll, drawing with an attack, a second attack, circle's, sheathing - each starts at the same
game time as at 30 (but for where the input lines fall) and takes as long, within a 30th; the hunter is where it is at
30 within a few units. Its integer counts compared field by field (+0 to +0x2000) at the same game times agree, but a
blend's count (+0xB0), which starts one later between 30ths (its blend takes up to half a 30th longer).

The herd animals (Popos, `089BC680` - `089BC584` is theirs as they are made), ticking every frame by default since
the hits were fixed: checked at 60 against 30 by `tools/ratecheck.py --vt 089bc680 --window 30 --switch N --whole` - they walk as far (88
units a second), wander as much, count as at 30, but for a colour's fade (`08860CB0`, a byte: its last steps of 1
come a game frame apart, at 60 twice as fast - a few 255ths). Hit (a recording of the hunter walking up to one,
attacking): each hit's damage once, the knockback as far within a 30th.

Not yet: the hunter's other weapons' moves and items (checked: the one in the test, its basic moves), the monsters' state
timers (attack windows, invulnerability, AI decisions), effects and particles, sounds started by ticks, the camera's
other easing and timers (`docs/ticks.md` 2.1), the UI's animations and fades, menus' waits. Behind the gates these run as at 30 (the motion player too, under them: its
patches see the time scale 1).

## How to carry on

`tools/ratecheck.py REC --rate 30|60 [--saves DIR]` finds what still runs at the game's frame rate rather than in game
time: the recording played at both rates (converted: frames halved or doubled), the hunter traced, compared action by
action - an action longer or shorter at 60, a field that counts game frames (steps by 1 on most frames at 30, twice as
often at 60), ground covered - until the runs part (the mouse's camera, converted, steers them apart in time). Its
findings are offsets in the hunter: `tools/watch.py` finds what writes them.

Another object: `--vt` (the first ticked with that vtable; `MHFU_TRACE_OBJ=hex` in the game for one at an address,
`MHFU_NATIVE_LOG=2` lists every object ticked, with its place), `--convert` (the vtables ticking every frame at 60),
`--window 30` (compared by windows of game time: no action field). `--switch N` plays both at 30 up to frame N and
the one at 60 from there (`MHFU_TICK_HZ=60@N`): the mouse's moves converted no longer steer the runs apart on the way;
`--whole` compares to the end though the object's places part (a monster's own choices part them anyway).

1. Open a gate (`MHFU_UNGATE`, then in `GATES`) and see what goes wrong at `MHFU_TICK_HZ=60@N` (twice as fast) -
   with a recording to get there and a few lines added to it for the input to try
   (`<frame> <buttons> <stick x> <stick y> 0 0`).
2. Find its variable: snapshots a tick or two apart, `tools/ramdiff.py` for what changes by a constant.
3. `tools/watch.py` for the code that advances it; the add (or count down) is the place.
4. Patch it in `recomp/patches.py`: what adds a float, to add it times `rt_time_scale`; an integer count of frames, to
   count only when `rt_tick30` is 1 (the frames where a frame at 30 a second falls: the same time, in steps of a
   30th).
5. Check at 30 that a recording replays to the same RAM (the patches must change nothing there), and that at 60 what
   is still gated is the same as at 30 (frames dumped at both rates from the switch, compared).

The shared helpers come first - one patch fixes every caller: the motion player was one function for every character.
Timers in the objects' state functions are the long tail.

## Where it is going

- The hunter's actions one at a time (a recording, lines added for the input), each timer in its state functions
  converted; hits between a hunter ticking every frame and a monster at 30 checked (no double or missed hits).
- The camera's easing left, its yaw, field of view and blends, at the frame's rate (its height is done); the mouse's
  camera (`camera.c`) with it.
- The CPU: at 144 the GE's vertex stage (on the CPU) fills the frame already, as with `MHFU_FPS=144`; the smoothing
  costs a tenth more. The GE's work is to be spread over threads.
- Then the monsters, effects, the rest; the motion player is done for all.
