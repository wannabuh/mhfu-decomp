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
  semaphores, event flags, VTimers, the vblank interrupt; object UIDs; the user memory partition; modules.
- **I/O** (`io.c`): the disc read from the ISO image (ISO 9660), raw sectors included.
- **GE** (`ge.c`): display lists walked (jumps, calls, stalls, FINISH / SIGNAL callbacks), each command to the
  renderer. `gpu.c` draws in software into emulated VRAM: sprites (textured or not, all but the DXT formats), the
  texture functions, alpha test, blending, clear mode, block transfers. No triangles or 3D yet.
- **Frames** (`present.c`): written as PNGs (`MHFU_DUMP=N`: every Nth frame, to `build/frames/`); no window yet.
- **Audio** (`audio.c`): output channels timed as the hardware plays them (silent); the SAS mixes silence.
- **System** (`system.c`): display, controller, UMD, utility dialogs (savedata: no saves yet), clock and the rest.

## Booting (where it is)

`build/mhfu <game.iso>` loads the EBOOT, starts its module as the kernel would and runs the threads. The boot follows
PPSSPP's call for call - the same threads, the user heap at `0x09D66000`, DATA.BIN's header read, the font module
loaded from raw disc sectors - and reaches the game's main loop: each frame a VTimer, `user_main` woken, display lists
queued, the controller read, the framebuffer flipped, sleep until the next vblank. The sound engine starts (it prints
its banner). The first screen - the language selection, its background art and menu - is drawn correctly a second
in. Nothing is heard, there is no input yet, and fonts (`sceFont`) and savedata are stubs.

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

`tools/difftest.py` runs the game in PPSSPPHeadless (interpreter, software renderer - the only configuration in which
its debugger's breakpoints are hit and the game keeps getting vblanks), catches real calls to functions not yet tested,
records the state at entry and at return, runs the recompiled function from the entry state (`build/difftest`) and
compares what a caller can see: the return value, the callee-saved registers, and memory. A function that reaches an
import or something else the runtime does not do yet is skipped.
