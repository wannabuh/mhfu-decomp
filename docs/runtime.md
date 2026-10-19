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
  leaves the picture), Fresnel, the sun's glint. Interpolated frames move scrolled textures' offsets on too.
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
98-100% of primitives match and 144 fps holds. What the game writes the vertices of each frame - the 2D (the HUD, menus,
text, fades, the loading screen) in the frame's display list, and its own grass in a ring of three buffers, a different
place every frame - is matched by what it is instead (vertex format, primitive, count, texture) in the order drawn: the
next of its kind if it shows the same picture nearby, else the nearest showing it, else the next of its kind nearby (64
pixels in 2D, 150 units in the world). Matched, its vertices are blended too - positions and colours; what they show is
the latest frame's. So the 2D moves with the rest, in step (it used to be drawn a frame ahead, at 30).
Running the game itself faster - the native frame rate - is in `docs/native.md` (`MHFU_TICK_HZ`, an experiment).
The display's frames are drawn while the game waits (one host thread); when the game's clock falls behind the wall
clock by more than a frame shown, frames are left out rather than the game slowed. Each primitive keeps only the span
of vertices it indexes, consecutive primitives on one buffer sharing a copy (a hunt's strips index big shared buffers:
copied whole, that was 70 MB a frame, and the game ran at 20-24 at 144).
`MHFU_FPS_STATS=1` reports, by the wall clock, the frames shown and left out, the game's rate, its longest frame, the
time idle, and the matching (with `MHFU_PRIM_STATS=1`, also the primitives a frame drawn by class - 2D, scenery,
lit, skinned... -, their vertices, the vertex stage's time on them and how often their matrices change);
`MHFU_REPLAY_SPEED=N` plays a replay at the game's speed from frame N (to measure somewhere a recording gets to);
`MHFU_DUMP_AT=N` writes game frame N at five steps between frames.

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
