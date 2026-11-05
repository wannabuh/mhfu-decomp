# Runtime

What the recompiled code runs on. The contract with the generated code is `runtime/include/recomp.h`; this is the plan
for the rest, in the order it is needed to boot.

## Done

- **Memory** (`memory.c`): the PSP address space as one 1 GB reservation, indexed by address with the mirror bits
  masked; scratchpad, VRAM and the 32 MB of RAM mapped, everything else left inaccessible so stray accesses fault.
- **Dispatch** (`dispatch.c`): calls by address go to the EBOOT's functions or to the resident overlay's. An overlay is
  loaded verbatim at its region's base, header included, so the resident one can be read from RAM (`MWo3`, number) -
  no need to hook the game's loader.
- **CPU helpers** (`cpu.c`): unaligned loads and stores, FPU control.
- **VFPU** (`vfpu.c`): the 45 operations the game uses, one function per instruction, prefixes included.
- **Kernel** (`thread.c`, `kernel.c`): threads as coroutines with the kernel's scheduling, sleep / wakeup, delays,
  semaphores, event flags, VTimers (handlers get the times by pointer; a schedule of 0 fires at once - the sound
  engine's tick), the vblank interrupt; object UIDs; the user memory partition; modules.
- **I/O** (`io.c`): the disc read from the ISO image (ISO 9660), raw sectors included.
- **GE** (`ge.c`): display lists walked (jumps, calls, stalls, FINISH / SIGNAL callbacks), each command to the
  renderer. `gpu.c` draws in software into emulated VRAM: sprites (textured or not, all but the DXT formats), the
  texture functions, alpha test, blending, clear mode, block transfers. No triangles or 3D yet.
- **Vulkan renderer** (`render.c` and its parts `render_*.c` - see `render_internal.h` - `vk.c`, `runtime/shaders/`
  with `shared.h`): the default. The software GE still decodes, transforms, lights, clips and culls; the GPU
  rasterizes, textures, blends and depth-tests, at `MHFU_SCALE` times the PSP's resolution (default 2). One pipeline with dynamic state, bindless textures cached by content, render targets
  per framebuffer address (render to texture included). `MHFU_RENDERER=soft` draws in software instead;
  `MHFU_VK_VALIDATE=1` turns on the validation layer.
- **Water** (`render_water.c`, `ge.frag`): the game's lakes and rivers - blended by their alpha, in the world, their
  texture scrolling, flat, facing up and below the eye - drawn as water: at the first one drawn into a picture its
  colour and depth are copied, and the water reads them - the picture under it bent by ripples (swells and drifting
  noise) and tinted by depth, the picture above reflected (marched through the depth, screen space; the sky where it
  leaves the picture), Fresnel, the sun's glint. Interpolated frames move scrolled textures' offsets on too. Not
  the desert's sand sea, scrolled and blended the same way: a texture the colour of sand (red over green over blue)
  is not water (`render_water.c`).
- **The HUD smaller** (`gpu.c` `hud_scaled`, `MHFU_HUD_SCALE`): in the hunt, the HUD's parts - the gauges (clock, health,
  stamina, sharpness, the name), the map, the item bar - each scaled whole towards its corner (top left, top right,
  bottom right). A part is told by the code drawing it (`patches.py` `HUD`: `0883F9F8`, `08842EE8`, `08845A34` called
  from the HUD, `0883F270`): the display list chunks linked while it draws (its 2D draws, by their commands' addresses;
  kept with recorded draws for the frames between 30ths and interpolated ones), the texts and numbers it queues (drawn
  later by the text's code: by their entries; the name, given once, by its place). The menus' windows, the supply box
  and the windows drawn among them (the quest's details, an item obtained) are no part. The button hints (`glyphs.c`)
  with their part.
- **The sun flare** (`gpu.c` `flare_texture`): models drawn towards the sun in several maps, glares and rainbow rings, left out. It stood in the
  middle of the picture wherever the sun was behind the trees or turned from, and moved about with the camera. Told by what its textures are, not
  which, as the maps have several: added onto the picture, and a quarter of a circle round one corner, drawn four times mirrored, so that what
  lies at the same distance from that corner is alike and the far side black. `MHFU_SUN_FLARE=1` keeps it. `MHFU_TEX_LOG=<dir>` lists and saves the
  textures the game adds onto the picture, and `MHFU_TEX_HIDE=hash,...` leaves some out, to look over others like them.
- **The Snowy Mountains' haze**: a dome of mist round the map (`game_sub 09CC0CE0`, an object's draw method, once a
  frame in the snowfields and the ice cave) and the snowstorm's sheets of fog (`game_sub 09C57E08`) - big flat
  triangles whose straight edges show as the camera turns. `MHFU_SNOW_HAZE=0` leaves both out (`patches.py` `SNOW`,
  `gpu.c` `rt_snow_haze`); the falling snow stays.
- **Ground grass's ground** (`grass.c` `grass_ground`): the scenery's triangles facing up laid into cells of 10 units,
  each row's span of a triangle only. One within the grass's reach is laid once (its corners noted); one reaching
  beyond is laid over the tiles about the camera and noted with them, laid again only over the tiles the camera brings
  in - a cave's walls and roof reach far: laid again every 30th, they took the render thread 35 ms a frame there.
- **Texture packs** (`texpack.c`): PPSSPP's - each texture's key worked out as PPSSPP does (its address, size and
  palette - the palette hashed as PPSSPP keeps it, bytes left from an earlier, bigger load included - and the `quick` /
  `xxh32` / `xxh64` hash of its data), checked against PPSSPP 1.20's own dumps of the same textures (names and pixels
  the same); the pack's PNGs read by background threads and uploaded in the texture's place, the effects still learning
  from the game's texture (its colour, its holes).
- **Frames** (`present.c`): written as PNGs (`MHFU_DUMP=N`: every Nth frame, to `build/frames/`); no window yet.
  Input can be scripted (`MHFU_INPUT="first-last:buttons,..."`, frames and PSP button bits in hex).
- **Audio** (`audio.c`, `atrac.c`): output channels timed as the hardware plays them, heard through SDL (one stream
  per channel); ATRAC3 / ATRAC3+ decoded with FFmpeg - the game's music, which matches FFmpeg's own decoding of the
  same files. Sound effects through the SAS synthesiser: VAG ADPCM voices with pitch, SPU-style ADSR envelopes and
  volumes (no reverb yet). `MHFU_MUTE=1` silences;
  `MHFU_AUDIO_DUMP=1` writes each channel's output to `build/audio-ch<N>.raw`.
- **System** (`system.c`): display, controller, UMD, utility dialogs (savedata: no saves yet), clock and the rest.

## Booting (where it is)

`build/mhfu <game.iso>` loads the EBOOT, starts its module as the kernel would and runs the threads. The boot follows
PPSSPP's call for call - the same threads, the user heap at `0x09D66000`, DATA.BIN's header read, the font module
loaded from raw disc sectors - and reaches the game's main loop: each frame a VTimer, `user_main` woken, display lists
queued, the controller read, the framebuffer flipped, sleep until the next vblank. The sound engine starts (it prints
its banner). With scripted input (`MHFU_INPUT`) it goes through the language selection, the Memory Stick check, the
ESRB and Dolby screens, the title screen, character creation (name, appearance, saving to a slot) and into Pokke
Village - the opening fly-over, then the hunter's house, in 3D. Movies end at once (no MPEG decoding yet), nothing is
heard, there is no lighting, and the system's own dialogs (save messages) are answered without being shown.
- **Fonts** (`font.c`): sceFont over PGF fonts (PPSSPP's compatible ones, found in its assets, or `MHFU_FONTS`).
- **Savedata** (`savedata.c`): plain files in `~/.local/share/mhfu-recomp/SAVEDATA` (or `MHFU_SAVEDIR`).
- **Callbacks**: run on their thread at its ...CB waits; the Memory Stick's insert callback hears "inserted".
- **Debugging**: `MHFU_WATCH=addr,...` logs memory words as they change, each frame; `MHFU_THREADS=1`
  lists the threads and what they wait on at exit; `MHFU_VBLANK_HZ=120` doubles the vblank rate (an experiment:
  the game then ticks at 60 a second, and runs twice as fast - see "Frame rate" below).

## Frame rate

The game ticks at 30 a second: its vblank handler (`0x08859188`) wakes `user_main` every second vblank
(`if (++count < 2) return`), and it counts time in ticks - nothing reads a clock. Doubling the tick rate
(`MHFU_VBLANK_HZ=120`) runs everything twice as fast: the splash screens last the same number of ticks. 30.0 and 60.0
appear ~1500 times in its code and data, 1/30 ten times: no central time step. So higher frame rates come from
interpolation - the game's frames recorded, frames in between drawn with their matrices interpolated - and a native 60
would mean finding and rescaling every system that counts ticks.

`MHFU_FPS=N` (Vulkan) shows N frames a second: each primitive of the game's frame is recorded (registers, matrices,
texture, a copy of its vertex data) instead of drawn; in the scheduler's idle time the display replays the latest
frame with each primitive's world, bone, view and projection matrices interpolated towards it from the previous
frame's matching primitive (same vertex and index addresses, format, type and count; among several, the nearest), the
moment shown
trailing the game by a frame (about 33 ms). A camera cut - the camera moving over 600 units or turning
over 60 degrees in a frame - turns the interpolation off for that frame. In the village
98-100% of primitives match and 144 fps holds. What the game writes the vertices of each frame in a different place -
its own grass in a ring of three buffers - is matched by what it is instead (vertex format, primitive, count, texture)
in the order drawn: the next of its kind if it shows the same picture nearby, else the nearest showing it, else the
next of its kind nearby (150 units). Matched, its vertices are blended too - positions and colours. The 2D (the HUD,
menus, text, fades, the loading screen) is not interpolated: it is drawn as in the latest frame (matched so, a menu's
text jittered as it changed - a letter blended into another nearby; `MHFU_BLEND_2D=1` as before). At the native frame
rate what moves in it is smoothed between 30ths instead (`docs/native.md`).
Running the game itself faster - the native frame rate - is in `docs/native.md` (`MHFU_TICK_HZ`, an experiment).
The display's frames are drawn while the game waits (one host thread); when the game's clock falls behind the wall
clock by more than a frame shown, frames are left out rather than the game slowed. Each primitive keeps only the span
of vertices it indexes, consecutive primitives on one buffer sharing a copy (a hunt's strips index big shared buffers:
copied whole, that was 70 MB a frame, and the game ran at 20-24 at 144).
`MHFU_FRAME_LOG=<file>` writes each game frame's wall time and stage (with `MHFU_PRIM_STATS=1`, the primitives by class every 240 frames), and
`MHFU_PROFILE=<file>,<from>,<to>` samples where the time goes between two frames, named by `tools/profile.py BINARY FILE`. Measure with `MHFU_FPS=0`: a recording
doesn't stay in step at a display's rate.

**Frames and the display** (`pacing.c`, on a window, where the driver has `VK_EXT_present_timing`). The driver's reports give the time each frame's first pixel really went out, and so the refresh grid to some tens of us. Each frame shown is made for one refresh: it begins a lead before it, and shows the game as it will be at that refresh less the lead, so the game's time moves by the same step at every refresh, however late a tick lets the frame begin. The lead is what frames have been taking from their due time to the end of their queue operations (all but about 1 in 1000), 0.7 ms to spare, and a refresh for the compositor. A frame that misses its refresh anyway is replaced by the next (mailbox), which is on time: no queue builds up. `MHFU_FPS` above the refresh gets one frame a refresh; below it, every `round(refresh / MHFU_FPS)` refreshes (60 on a 144 Hz screen: 72). Without reports (no such driver, a hidden window's nothing, the driver calling the refresh variable, the first frames), a timer at `MHFU_FPS`, and no `MHFU_FPS=0` or headless run is touched: this has nothing to do with the game's clock, its ticks or its memory. `MHFU_PRESENT_MODE=old` (`present_mode = old` in `settings.ini`) goes back to before: a timer at `1000000 / MHFU_FPS` whole us, mailbox. `MHFU_PRESENT_SWAP=fifo|latest|immediate` picks the swapchain's mode, to compare; `MHFU_PRESENT_TIMING=0` asks for no reports.

**Ticks between frames** (`thread.c`). The game's thread draws the display's frames too, so a frame due during a tick waits for it. The game's clock is its own, so a vblank whose work would run into the next frame is let go that much early on the wall clock (at most 8 ms), at the same time on the game's clock: the tick is done when the frame is due, and the game's schedule and what the frames interpolate between don't change. What a vblank's work takes is reckoned near the most of late, odd and even vblanks apart (the game wakes on every second one). Only with a display frame rate of its own; `MHFU_TICK_PHASE=0`: each at its own time. `MHFU_FPS_STATS` says how many, how early and what a tick's reckoned at, and with `MHFU_TEST_PERF=1` a tick's GE and its wait for the render thread. `tools/profile.py --threads` separates the game's work, the display's frames and other threads.

`MHFU_PRESENT_LOG=<file>` logs each frame shown (when it began and was due, the refresh it was for, when its present returned, the driver's time for its queue operations' end and its first pixel or that it was never shown, the game time it shows, whether a tick came before it) and `tools/pacing.py <file>` sums it up: intervals, repeats, frames replaced, the game's time from each refresh to the next (what the eye sees), periods. About 0.7 MB a minute. The file's head says what its columns are (`pacing.c`).

`MHFU_FPS_STATS=1` reports, by the wall clock, the frames shown and left out, the game's rate, its longest frame, the
time idle, and the matching (with `MHFU_PRIM_STATS=1`, also the primitives a frame drawn by class - 2D, scenery,
lit, skinned... -, their vertices, the vertex stage's time on them and how often their matrices change);
`MHFU_REPLAY_SPEED=N` plays a replay at the game's speed from frame N (to measure somewhere a recording gets to);
`MHFU_DUMP_AT=N` writes game frame N at five steps between frames.

## Tools

Debugging tools, in `docs/roadmap.md`'s "Tools first". They only read: no write to the game's memory, none of its random numbers, no change to what is scheduled, and no `SAVED` variable added. Checked by `MHFU_RAM_HASH=1` frame by frame against a build without them, with the panel, the inspector open and held, and captures, at `MHFU_HAND=0`: `popo.rec` with recorded frames, and `overnight-fight1.rec` (with a Giadrome) without.

- **Performance panel** (`perf.c`, F5; `MHFU_TEST_PERF=1` turns it on, and `MHFU_DUMP_AT=N MHFU_FPS=60 MHFU_DUMP_PRESENT=960x544` shows it in a dumped frame): in the top right corner, FPS and frame time over the last second, a graph of the last 208 frames' times (the grey line where the usual frame is; yellow over 1.5 times it, red over 2 and marked above the graph), the worst in the last 10 s, the tick rate, frames skipped and the time asleep. The time split's labels are at the top of `perf.c`: `GE`, `DRAW`, `RT WAIT`, `GPU WAIT`, `ACQ+PRES` and `RENDER THR` are timed, `LOGIC` is the game's thread's wall clock less all those. Draws and triangles are as the game issues them a tick. `DISPLAY` and the line under it are the driver's reports over a second (`pacing.c`), where it gives them: the refresh, frames that reached the screen, `LOCK` the lead or `TIMER`; `REPEAT` refreshes showing a frame again, `REPLACED` frames that never reached it, `LATE` those after the refresh they were for, `STEP` the least and most game time from one refresh to the next, in ms. The frame times above are between frames handed over, and can look even while the screen isn't. `RAM` is the kernel's view, the image and the partition's blocks out of the 24 MB from `0x08800000`: the game's own allocators work inside those blocks, so it can't say how full they are. With a swapchain missing (headless) `FLIP`, `GPU WAIT` and `ACQ+PRES` read near nothing. Off, the panel's timers are each one branch, and two small rings of frame times stay filled for captures.

- **Bug capture** (`capture.c`, F6; `MHFU_TEST_CAPTURE=frame,...` at those game frames): one press makes the next numbered folder under `~/.local/share/mhfu-recomp/captures/` (`MHFU_CAPTUREDIR` elsewhere), the number shown on the screen, with: `state1.bin` (a save state in the slots' format, taken at the next point every thread waits, the 9 slots untouched), `screenshot.png` (as shown, at the shown resolution, panels included), `info.txt` (time, commit, frame, rates, stage and quest, the hunter, every object with vtable, class and confidence, type, place, state, motion, health; the settings in effect; which frame the state has), `inputs.txt` (recording lines), `frame-times.txt`, `events.txt` (state and motion changes of hunters and monsters, objects coming and going, the stage changing) for the last 10 s, and `env.txt`. On the game's thread: a copy of the state, as F2 takes, and the frame read back; the files are written by threads of their own. The rings behind the last 10 s are always kept, a few words a game frame, and aren't in what a state saves. `objects.c` reads the objects (the manager at `*0x08A62D7C`, 15 lists); `build/gen/class_table.h` is made from `classes.toml` and `types.toml`, names only, and species are read from the player's copy at run time (the game's name list by the type byte, inferred). To load one:

      tools/capture.py 12 --frames 30           # by number or folder: headless, its env.txt's settings, frames in <folder>/frames
      MHFU_STATEDIR=<folder> MHFU_STATE_LOAD=30,1 build/mhfu    # with a window; MHFU_SAVEDIR at a copy if the game may save

- **Click inspector** (`inspect.c`, picking in `gpu.c`; F7, F8 holds the game while it is open, as F1 does; `MHFU_TEST_INSPECT="frame,x,y;..."` clicks at PSP pixels, `MHFU_TEST_PAUSE="frame,n"` holds for n passes): the mouse is let go of and isn't the game's while it is open; keys and the pad still are. A click picks the object nearest it on the screen: each object's place (+0x200) and a point 90 units up (times its scale) projected with the camera shown, which isn't always the game's (`camera.c`; the turned or farther camera is included, checked by the marker on the hunter's feet at `MHFU_FPS=144`, `MHFU_TICK_HZ=60` and `MHFU_FPS=0`). Another click at the same spot takes the next nearest. A panel shows it live (class and confidence, vtable, kind, type, species by the type byte, inferred, place, state, motion, ticks, health), a yellow marker its feet. Fields read past the base class can mean something else on another class: health is read only for lists 2 and 3, kinds 0 and 1, and even there `health_max` has been seen below health on a small creature.
  - **Under the pointer** the panel lists the draws covering that pixel in the frame shown, nearest first after the 2D (up to 6; `inspect.txt` the same), each with its texture's hash, as `MHFU_TEX_HIDE` and texture packs name it (checked: hiding a hash listed for the hunter's legs takes the legs out and the list then gives the ground), size and address. How exact: each draw's triangles as placed, including skinning, near clipped, culled, scissored and depth tested in draw order for that one pixel, into the framebuffer shown. Not the texels: what an alpha test leaves a hole in, and colour masks, are not seen, so a leaf's quad can head the list over what shows through it; alpha tested draws aren't taken to hide what comes after. Most of the game's draws are alpha tested or blended, so several are listed. With recorded frames (`MHFU_FPS`) the frame is gone over at 5 Hz, and on each click; between those the list can be up to 200 ms old unless the game is held. Without them each game frame's draws are noted as drawn, while the inspector is open. A texture written over since it was drawn would hash as it is now.
  - The selection goes to `inspect.txt` in the captures' folder, and into a capture's `info.txt`. Its cost is only while it is open: the triangles kept (64 bytes each) and, for recorded frames, a pass over the draws 5 times a second. The pass writes only the renderer's scratch below `0x08800000`, as drawing a recorded frame does.

## Threads

The game runs ten threads (`user_main`, `fakeRofsLoader` - the DATA.BIN reader -, `sha1Thread`, `transferThread`, the
sound driver's four...) synchronised with sleep / wakeup, semaphores, event flags and delays. `user_main` runs a frame
and sleeps; the vblank interrupt handler (`0x08859188`) wakes it.

- Each PSP thread is a **coroutine** on one host thread, with its own host stack: recompiled functions are ordinary C
  functions, so a thread blocked in a kernel call is a C call stack that has to be kept, and switching coroutines is the
  cheapest way to keep it. One host thread means no locking and a deterministic schedule.
- The **scheduler** is the PSP kernel's: the highest-priority ready thread runs; a thread only loses the CPU when it
  blocks, yields, or a higher-priority thread becomes ready - which only happens inside kernel calls or interrupts.
- **Interrupts** (vblank at 59.94 Hz, GE list completion, timers) run at kernel calls rather than at any instruction.
  The game waits for vblank through the kernel every frame, so that should be enough; if a thread is found busy-waiting
  on memory an interrupt handler sets, the recompiler can add a check at backward branches.
- **Time** is virtual, so that a run is the same every time for the same input: the clock stands still while the game
  runs and moves only when every thread waits - to what they wait for (a vblank, a timer, a timeout), once the host
  clock gets there (or at once, fast-forwarding). A slow host slows the game instead of making it skip; reading the
  clock moves it a microsecond, so that a wait by reading it ends. The date and time are the run's start (recorded)
  plus the clock. Input reaches the game only at its frames. Two runs of the village script leave all 24 MB of RAM the
  same at frames 1500 and 2500.
- **Recording and replay**: `MHFU_RECORD=file` writes the input of each frame that changes it (buttons, stick, mouse)
  and the start time; `MHFU_REPLAY=file` plays it back fast-forwarded (nothing shown or heard), then hands over (with
  `MHFU_RECORD` too, the recording carries on). A replay ends where the recording did, byte for byte - as long as the
  saves it loads and the way the runtime runs the game are unchanged.
- **Save states** (`savestate.c`): the game's whole state copied where every thread waits - the scheduler's idle loop
  (`state_point`) - and copied back there. On disk they load in later builds, so a state holds nothing of the program
  itself: no host stacks (their return addresses are the build's), no function pointers.
  - **Threads.** A waiting thread is in a kernel call, with every game register in its `Ctx`, and generated code keeps
    nothing else across a call. So it is kept as its `Ctx`, its wait and what the call has left to do (`Thread.after`:
    a wait's result, a delay's, the rest of `sceAudioOutputPannedBlocking`; `after_callbacks`). After a load each gets
    a new host stack (`thread_resume_main`): it finishes the call, then `rt_resume_run` enters the function holding the
    return address, and each function it returns from at `r31`, until the thread's entry returns. For this `recomp.py`
    labels every call's return point (`R_<addr>`), gives each function with calls a check at its top (`rt_resume_pc`),
    and lists the return points in `RecompModule.returns`.
  - **Frames with C state.** A gate running code as at 30 (`run_as_30`, `run_native`), a callback (`run_callbacks`) and
    a hand-written function (`readable/`: generated code brackets the call) hold state in C locals, so a thread blocked
    beneath one cannot be kept. `rt_no_resume`
    counts them, and a save waits for a state point with none (logged; after 300 frames the window says so). A new
    runtime frame that calls game code and does anything after it must count itself too.
  - **The file** (`states/stateN.bin`, about 11 MB): a header (`MHFUPST1`, the format, a hash of the ISO's size and
    first megabyte, the arena's layout, the time, the frame), then compressed named chunks: `mem:scratchpad`,
    `mem:vram`, `mem:ram`, `arena`, and `var:<file>:<name>` for each `SAVED` variable (a global: `var:<name>`; a static
    in a function: `<function>.<name>`). The names come from the program's own symbol table, so it must not be stripped.
  - **What a later build does with it.** A variable the state lacks takes its value at boot, one the build lacks is
    left out (each logged). A variable of another size refuses the load, and is named. The arena is copied raw, so what
    is allocated from it (threads, semaphores, event flags, callbacks, vtimers, memory blocks) must keep its layout:
    their sizes are checked, and a field changed for another of the same size needs `ST_LAYOUT` bumped. Renaming a
    variable or its function counts as one lost and one added. A load only says the state fits: if the build changed
    what the game does (`patches.py`, the camera), the run goes on differently from there, as it should.
  - **What is the host's** is made again after a load: the ISO's file, the sound streams (emptied), the ATRAC decoders
    (brought to their place by decoding the two frames before), the renderer (textures follow the memory by their
    hashes; the frames shown between are taken as a cut). A slot is kept in memory and written by a thread of its own.
  - `MHFU_STATE_TEST=A,B` (save at A, load at B, compare each frame), `MHFU_STATE_SAVE=frame,slot`,
    `MHFU_STATE_LOAD=frame,slot`, `MHFU_STATE_NOTE=frame,file` (RAM to a file) script it; `docs/testing.md` has the test
    across builds. A state loaded stops an input recording (`MHFU_RECORD`). A new variable of the runtime that the
    game's run depends on must be `SAVED` (or made again after a load) and hold no address in the program, including
    what the renderer decides for the game (`rt_cull_margin`, the sun it goes by). `MHFU_STATE_TEST` finds what is missed
    only if a load in the same process would lose it; a load in a new process finds the rest.
- **Finding code**: `tools/watch.py ADDR --frame N [--read|--access] -- MHFU_REPLAY=...` runs the game under gdb and,
  from game frame N, prints each write (or read) of the PSP address: the value and the game functions on the stack with
  the instruction (`EBOOT_08886E50@08887814`: the camera reading its preset). `tools/ramdiff.py` finds variables from
  RAM snapshots (F9, or `MHFU_SNAPSHOT`).
- **Intros** are skipped (`MHFU_INTRO=1` plays them): cross is pressed at the language menu (frame 40) and the game
  fast-forwarded to the title (frame 470) - in 30ths of the game's time (at `MHFU_TICK_HZ`, more of its frames).
- **The renderer's thread** (`render_queue.c`): the GE runs on the game's thread as the game queues its lists, up to
  the vertex stage (gpu.c); what follows - the clipping, the renderer's batches, textures, Vulkan - is queued and done
  on a thread of its own at the same time. The renderer reads PSP memory (textures, palettes), so the game's thread
  waits for the queue to be done whenever that could change - a list's run ends, its callback runs game code, a block
  transfer - and before anything else calls the renderer. Frames are the same to the pixel either way;
  `MHFU_RENDER_THREAD=0` keeps it all on the game's thread. In the Popo field at 144 (scale 4) the two threads take
  about as long each: from about 121 frames a second on one to 144.
- **The vertex stage on the GPU** (`MHFU_GPU_TL`, `2` by default; `0`: the CPU's, the
  reference for pixel-exact checks): a primitive's vertices go to the
  renderer as the game gave them (decoded) with its draw - matrices, viewport, fog, texture scaling, material, lights
  (`shared.h` `TlDraw`) - and `ge_tl.vert` does what `vertex()` and `light()` would (the near plane a clip distance
  where gpu.c clips). `MHFU_GPU_TL=1`: the hunter, monsters and people (lit, skinned); `2`: every 3D triangle - but
  what adds to the picture (fire, glows: the point lights and glow pass read it) and the game's round shadows, left to
  the CPU. Its draws cast shadows - the sun's and the point lights' (`shadow_tl.vert`, `pshadow_tl.vert`, from the
  same placement: `tl_common.glsl`), whole triangles whatever the camera's near plane cuts, and the motion blur's pass
  draws their motion again; what is lit by the pixel is tessellated as the CPU's batches are, its shape normals and
  straight edges made as a frame is recorded (each group's corners welded by their place as given; the edges on each
  triangle's third corner, the one it ends with). The feet of what is lit (for the wind and the grass) come from each lit group's
  lowest vertices - chosen once a frame recorded, placed in the world on the CPU each frame shown. The scenery's
  effects are made by the renderer as a batch is flushed (`render.c` `tl_scenery`, from its draws' vertices kept on
  the CPU; once a recorded frame for a slot's): each vertex's sun shading normal (the faces summed where corners meet,
  as `make_normals`) or its height above its feet for the wind, into a buffer the shaders read by the vertex
  (`BIND_TL_EXTRA`, after each frame's wind and feet); `tl_common.glsl` `tl_sway` moves it (shadows too), and
  `ge_tl.frag` - `ge.frag` reading its triangle's corners (`VK_KHR_fragment_shader_barycentric`) - takes a triangle's
  own face where the sum bends more than 60 degrees from it, per corner as the CPU's batches do (without the extension:
  per pixel, `ge.frag`). The grass's ground is laid from the same triangles. So `1` and `2` have all the effects
  (Popo field at 60 shown at 144, every effect, scale 1: a frame shown 6.6-7.4 ms on the CPU, 4.9-5.3 with `1`,
  3.7-4.1 with `2`). Frames match the CPU's within a few pixels (tessellated: half a percent, the welding not quite the same; every
  recording, at 30, 60 and 144 and shown interpolated, under 0.2% of pixels off by more than 24, validation clean). (Between frames, point lights differ from run to run: they flicker by the
  wall clock and ease in by frames shown - compare with `MHFU_POINT_LIGHTS=0`.) `MHFU_TL_CHECK=1` places each such vertex in C as the shader would
  and compares it with `vertex()`. With `MHFU_FPS`, a frame's vertices are decoded once, as it is recorded, and sent
  once as it becomes the latest (into one of four slots); draws in a row with the same state, matrices and texture are
  a group, and a group whose draws all match into one group of the frame before is replayed as one draw - one
  interpolation, one draw call - so a frame shown costs a few hundred groups, not thousands of primitives (Popo field
  at 60 shown at 144: 1.1 ms a frame shown from 4.8; the guild hall 0.7 from 3.5). `MHFU_TL_GROUPS=0` replays each
  draw on its own (for comparing).

## HLE

`hle_table.c` dispatches the 272 imports (34 libraries; `nids.inc` names them) by NID to `hle_<name>`; one not
implemented logs its first call and returns 0. Still to do: fonts (`sceFont`, PGF), savedata, the SAS's voices and
ATRAC3, MPEG (cutscenes), and the GE's drawing. PPSSPP's HLE is the reference for their behaviour - and
`tools/difftest.py`, which runs functions in PPSSPP and the recompiled code from the same state, checks the results.

## Checking against PPSSPP

`tools/vfputest.py` checks the VFPU on its own: the game's VFPU instructions (each with its prefixes), run as small
programs - load all 128 registers from random values, run, store them back - in PPSSPP and in the runtime
(`build/vfputest`), compared register by register. All agree but `vasin`, within 0.1% (PPSSPP reproduces the
hardware's approximation).

`tools/difftest.py` runs the game in PPSSPPHeadless (interpreter, software renderer - the only configuration in which
its debugger's breakpoints are hit and the game keeps getting vblanks), catches real calls to functions not yet tested,
records the state at entry and at return, runs the recompiled function from the entry state (`build/difftest`) and
compares what a caller can see: the return value, the callee-saved registers, and memory. A function that reaches an
import or something else the runtime does not do yet is skipped.
