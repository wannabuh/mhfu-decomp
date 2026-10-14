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
- **`MHFU_TICK_HZ=N`** (an experiment): the game runs N ticks a second (its vblank handler wakes it every second vblank,
  so the vblanks come 2N a second) and `rt_time_scale` is 30 / N, which patches multiply by. At 30 the scale is 1 and
  the patches change nothing.
- **Finding what counts ticks**: `tools/ramdiff.py` over RAM snapshots (`MHFU_SNAPSHOT=frames`, or F9) finds variables
  that move by a constant a tick (the motion frames: +2.0); `tools/watch.py ADDR --frame N` finds the code that writes
  them; a recording (`MHFU_REPLAY`) gets there the same way every time.

## Converted

| system | patch | at 60 ticks |
|---|---|---|
| motion frames | the four loads of a layer's speed in `088637C4` | motions play at their speed |

Not yet: motion blends (the weight step, and the tick count, an integer), and everything else - movement that is not
root motion, the hunter's and monsters' state timers (attack windows, invulnerability, AI decisions), effects and
particles, sounds started by ticks, the camera's easing, the UI's animations and fades, menus' waits. At 60 ticks a
second those run twice as fast; menus and transitions too, so a recording made at one rate does not replay at another.

## How to carry on

1. Pick what looks wrong at `MHFU_TICK_HZ=60` (twice as fast).
2. Find its variable: snapshots a tick or two apart, `tools/ramdiff.py` for what changes by a constant.
3. `tools/watch.py` for the code that advances it; the add (or count down) is the place.
4. Patch it in `recomp/patches.py` to scale by `rt_time_scale`; for integer counters, the counter needs to count in
   finer steps (scaled by the rate) where it is set, or a fraction kept beside it.
5. Check at 30 that a recording replays to the same RAM (the patches must change nothing there).

The shared helpers come first - one patch fixes every caller: the motion player was one function for every character.
Timers in the objects' state functions are the long tail.
