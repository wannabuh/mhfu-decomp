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

## Converted

| system | patch | at 60 ticks |
|---|---|---|
| motion frames | the four loads of a layer's speed in `088637C4` | motions play at their speed; walking too (root motion: about 500 units a second at either rate) |
| motion blends | the weight's step (`08863894`), the count of frames (`088638A0`, on `rt_tick30`) | blends take their time |

Not yet: everything else - movement that is not root motion, the hunter's and monsters' state timers (attack
windows, invulnerability, AI decisions), effects and particles, sounds started by ticks, the camera's easing, the UI's
animations and fades, menus' waits. Behind the gates these run as at 30 (the motion player too, under them: its
patches see the time scale 1).

## How to carry on

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

The gates go down the call tree as things are converted: a gate is opened, and what is under it that is not converted
yet gated in its place (its calls), so that at every step the game at N a second is the game at 30 but for what is
converted. Down to the hunt's objects:

- `08895538` (the game states) → a state's run (`09A60298` in a hunt) → `09A62740` → `09A627B8` (the hunt's tick:
  the objects, the cameras `08812BD0`, the HUD `0883F270`...) → `088DB068` → the object list → an object's tick,
  `08864634` (its state function, then its vtable's +0x18 and +0x1C): gated there, an object not converted yet ticks
  at 30 while a converted one (by its vtable) ticks every frame.
- The render pass draws every frame. What ticks at 30 changes only every few frames, so its draws need the
  interpolation `MHFU_FPS` does now, across its own 30ths, while what is converted is drawn as it is.
- The hunter first (what the player feels), with the camera; then monsters, effects, the rest; the motion player is
  done for all.
