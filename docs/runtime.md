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
- **Vulkan renderer** (`render.c`, `vk.c`, `runtime/shaders/`): the default. The software GE still decodes,
  transforms, lights, clips and culls; the GPU rasterizes, textures, blends and depth-tests, at `MHFU_SCALE` times the
  PSP's resolution (default 2). One pipeline with dynamic state, bindless textures cached by content, render targets
  per framebuffer address (render to texture included). `MHFU_RENDERER=soft` draws in software instead;
  `MHFU_VK_VALIDATE=1` turns on the validation layer.
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
frame's matching primitive (same vertex address, format, type and count - the k-th with the k-th), the moment shown
trailing the game by a frame (about 33 ms). A camera cut - the camera moving over 600 units or turning
over 60 degrees in a frame - turns the interpolation off for that frame. In the village
98-100% of primitives match and 144 fps holds. What the game animates by rewriting vertices, and the 2D, move at 30.
`MHFU_FPS_STATS=1` reports the rate and matching; `MHFU_DUMP_AT=N` writes game frame N at five steps between frames.

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
- **Time**: the emulated clock follows the host clock, so the game runs in real time; `sceKernelDelayThread` and the
  vblank are scheduled on it.

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
