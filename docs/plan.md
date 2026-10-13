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
- `DATA.BIN` (737 MB) has no plaintext header: it is encrypted or scrambled from byte 0.
- Other modules: the game loads `sceFont_Library` from a raw sector range on the disc (`sce_lbn0x11981_size0x59c0`) -
  Sony's, and HLE'd by PPSSPP. `SYSDIR/UPDATE/EBOOT.BIN` is the firmware updater (ignored).

## Open questions (answered by phase 2)

- How is `DATA.BIN` encrypted and packed, and where are the overlays in it (and do the relocation sections tell us
  anything the overlays' own data doesn't)?
- How much of the game is in Sony's libraries (statically linked, recognisable) versus Capcom's code?
- How many threads, and how do they synchronise (the runtime must schedule them as the PSP kernel does)?
