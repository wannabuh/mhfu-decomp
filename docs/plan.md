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

## Findings so far

- Disc: ULUS10391, version 1.01. `BOOT.BIN` is zeroed; `EBOOT.BIN` is `~PSP`-encrypted (module name
  `MonsterHunterPortable2ndG`). `tools/dump_modules.sh` gets it decrypted from PPSSPP.
- The decrypted EBOOT is a fixed-address executable (`ET_EXEC`), not a relocatable PRX: one `PT_LOAD` at `0x08804000`
  (`.text` 0x109f58 bytes, `.data` 0xbc198, memsz 0x1561580), entry `0x08906A54`.
- Compiler: **Metrowerks CodeWarrior** (`.comment`: `MW MIPS C Compiler (2.4.1.01)`) - not GCC or SN.
- **Overlays: 298 named ones**, each a `PT_NULL` program header + a `<name>.ovl` section (with a `.rel<name>.ovl`
  relocation section) whose contents are not in the EBOOT - presumably in `DATA.BIN`. Four regions:

  | Base         | Overlays                                                                 |
  |--------------|--------------------------------------------------------------------------|
  | `0x09A5F300` | 12 "tasks" (game modes): game, lobby, demo, edit, movie, install, arcade, show_select, download, gallery, cont, option |
  | `0x09C19100` | `game_sub`, `demo_sub`                                                   |
  | `0x09D1A280` | 17 monsters: `em01` ... `em83`                                           |
  | `0x09D63100` | 267 stages: `stage000` ... `stage266`                                   |

  Plus an empty `debug.elf` section. Every overlay has to be recompiled, and the runtime has to know which one is
  resident in each region (calls into an overlay region dispatch on that).
- `DATA.BIN` (737 MB) is 6657 files, encrypted with a per-read stream cipher keyed by the starting sector - the
  details are in `tools/databin.py`, which lists, decrypts and extracts them (`game/data/`). Content seen so far: PNGs,
  RIFF/WAVE (ATRAC3) audio, and the overlays.
- Each overlay has a 0x40-byte Metrowerks header (`MWo3`: number, load address, text / data / bss sizes, the static
  constructor table), then text and data; the constructors are code at the end of the data (`tools/modules.py`).
- **The overlays are DATA.BIN files**, loaded verbatim at their region's base: tasks 63-74, subs 75-76, stages
  5542-5808 (in order), monsters 6095-6111 (in order). Each contains its own name; `tools/databin.py overlays` matches
  all 298 and writes them to `game/overlays/<name>.bin`.
- The overlay loader is `0x088901F8(ctx, file, region)`: region bases are a table at `0x089CBB90` (0 = the EBOOT,
  1 tasks, 2 subs, 3 monsters, 4 stages, 5 debug); it reads the file to the base, flushes the caches and stores the
  resident file index per region (a u16 array in its context) - the state the runtime's overlay dispatch can mirror.
  Tasks and subs are loaded with constant file indices; monsters and stages with computed ones (not yet traced).
- Other modules: the game loads `sceFont_Library` from a raw sector range on the disc (`sce_lbn0x11981_size0x59c0`) -
  Sony's, and HLE'd by PPSSPP. `SYSDIR/UPDATE/EBOOT.BIN` is the firmware updater (ignored).

- **Functions**: 16959 in the EBOOT and the overlays, every instruction accounted for, checked against PPSSPP's own
  scan; C++ exceptions are compiled in but never thrown. See [functions.md](functions.md).

- **Recompiler v0** (`recomp/recomp.py`): all 16959 functions translate - 1.36 million lines of C in 312 files,
  compiling with no errors (clang `-O2`: 31 s on 12 cores, 23 MB of objects). VFPU instructions other than branches
  go to the runtime (`runtime/src/vfpu.c`, one function per operation).
- **Differential testing against PPSSPP** (`tools/difftest.py`): real calls caught in PPSSPP, the recompiled function
  run from the same state, what a caller can see compared. The first sessions: every function that ran without
  reaching an import passed. See [runtime.md](runtime.md).

## Open questions (answered by phase 2)

- ~~How do the main program and the overlays call each other?~~ Both directly (EBOOT -> `game_task`, monsters ->
  `game_task`, `game_task` -> specific monsters...) and through function pointers; calls into overlay regions will
  dispatch at runtime on the resident overlay. See [overlays.md](overlays.md).
- How much of the game is in Sony's libraries (statically linked, recognisable) versus Capcom's code?
- How many threads, and how do they synchronise (the runtime must schedule them as the PSP kernel does)?
