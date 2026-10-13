# Plan

Goal: Monster Hunter Freedom Unite (PSP, US `ULUS10391`) as a native PC game - its code statically recompiled to C,
on a runtime replacing the PSP's hardware and system software - with a Vulkan renderer, graphical enhancements, a
higher frame rate and native PC controls (mouse / right-stick camera, no "claw").

## How it fits together

```
   game ISO (yours) ──► EBOOT.BIN (encrypted) ──decrypt──► ELF / PRX (MIPS Allegrex)
                                                               │
                                                     recompiler (rabbitizer decoding)
                                                               ▼
                                         C: one function per game function, registers as locals,
                                         memory through the runtime; built with clang
                                                               │
                runtime ◄──────────────────────────────────────┘
                ├── memory: the PSP's 32 MB of user RAM (+ scratchpad, VRAM) as host arrays, at the same addresses
                ├── HLE: the kernel and libraries the game imports (by NID): threads, semaphores, events, timers,
                │        files (the ISO), savedata, controller, audio, display
                ├── VFPU: the vector unit's instructions in C (PPSSPP's as the reference)
                ├── GE: the graphics engine's display lists ──► our Vulkan renderer (enhancements live here, and in
                │        hooks on the game's own render functions)
                └── audio: SAS (software synth) and ATRAC3 decoding
```

PPSSPP (installed: 1.20.4) runs the same game and is our oracle: its debugger and symbol maps for learning the code,
and traces (calls, memory, display lists) to compare the recompiled game against when they disagree.

## Phases

1. **Repository and tools** - this skeleton; rabbitizer (Allegrex + VFPU decoding); Ghidra with an Allegrex
   processor module (to install into the local Ghidra when we start analysing).
2. **Analysis** (needs the ISO) - extract; decrypt `EBOOT.BIN` (PPSSPP can dump the decrypted module); identify it
   (ELF or relocatable PRX, load address, sections); its imports by library and NID; modules it loads from the disc;
   overlays (code in `DATA.BIN`?); the compiler and SDK version from its strings; its threads.
3. **Recompiler v0** - function discovery (symbols, call targets, Ghidra); MIPS -> C for every function, delay slots,
   jump tables, indirect calls through a lookup table, VFPU through runtime helpers; the whole game building.
4. **Runtime v0** - boot to the first displayed frame (the logos), checked against PPSSPP.
5. **Playable** - the GE -> Vulkan renderer, audio, controller input, savedata.
6. **The PC game** - native controls (camera driven by mouse / right stick), 60 fps (the game's 30 fps timing fixed at
   its source), graphical enhancements (as ao-vk: lighting, shadows, materials, effects).

## Open questions (answered by phase 2)

- Is code also loaded from `DATA.BIN` (overlays) or other modules on the disc - and how is `DATA.BIN` packed?
- Which compiler built it (GCC 3.4 from Sony's SDK, or SN Systems')? Matters for reading its code, not for the
  recompilation (we don't need matching C).
- How much of the game is in Sony's libraries (statically linked, recognisable) versus Capcom's code?
- How many threads, and how do they synchronise (the runtime must schedule them as the PSP kernel does)?
