# Windows

The game builds for Windows as `build/win/mhfu.exe`. **It has been played under Wine on Linux, not yet on real Windows**, so
expect rough edges there, and please report what you find: frame pacing above all.

**Never share a build.** `mhfu.exe` contains the game's code, translated from your copy. Everyone builds their own from
their own ISO (`ULUS10391`).

Not on Windows yet: save states (F2 and the rest say so and do nothing), the stall recorder and `MHFU_PROFILE`.

## Build it from Linux or WSL

This is the tested way: the game is cross-compiled, then runs on Windows. Tested on Arch Linux. On Windows, use it from WSL
(`wsl --install` gives Ubuntu), where these steps should work the same but have not been tried yet.

You need `git`, `make`, `clang`, `python3` with `venv`, `7z`, `glslc` (from the Vulkan SDK, or a package such as `glslc` or
`shaderc`) and PPSSPP's SDL build as `PPSSPPSDL`, with its fonts.

```sh
git clone https://github.com/wannabuh/mhfu-decomp && cd mhfu-decomp
```

First the game's code, as in the README, from your ISO (`path/to/mhfu.iso` below):

```sh
tools/setup.sh
tools/extract_iso.sh path/to/mhfu.iso
tools/dump_modules.sh path/to/mhfu.iso
tools/.venv/bin/python tools/databin.py overlays
tools/.venv/bin/python tools/funcs.py
tools/.venv/bin/python recomp/recomp.py
```

Then the compiler and libraries, into a folder of their own (no root; delete the folder to remove them), and the build:

```sh
python3 tools/win_setup.py ~/mhfu-wintools
export PATH=~/mhfu-wintools/llvm-mingw/bin:$PATH
make -j"$(nproc)" WINDOWS=1 WINROOT=$HOME/mhfu-wintools/sysroot
python3 tools/win_dlls.py build/win/mhfu.exe ~/mhfu-wintools/sysroot/bin ~/mhfu-wintools/llvm-mingw/x86_64-w64-mingw32/bin
```

The game draws its text with the PSP's fonts, which are not on the disc. PPSSPP has compatible ones: copy them to `game/fonts`.
Where they are depends on how PPSSPP was installed (on Windows: `assets\flash0\font` in its folder):

```sh
cp -r /usr/share/ppsspp/assets/flash0/font game/fonts
```

## Run it

Your graphics driver must support Vulkan 1.3, and Windows must be 10 or later.

**On Windows**, copy these from the build into one folder, keeping their layout, then start `build\win\mhfu.exe` (from WSL, a
folder under `/mnt/c/` is on your C: drive):

    build\win\       the program and its DLLs
    game\decrypted\
    game\fonts\

Name the game on the command line, or once in `%APPDATA%\mhfu-recomp\settings.ini`, a file you create, with the line
`iso = C:/path/to/mhfu.iso`.

**Under Wine**, keep Wine from changing your desktop's file associations: when it creates a prefix, its `winemenubuilder`
points them at that prefix. So set `WINEDLLOVERRIDES` as below from the very first run. Use a prefix of its own:

```sh
WINEPREFIX=~/mhfu-wintools/wineprefix WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=' \
    wine build/win/mhfu.exe "Z:/path/to/mhfu.iso"
```

The console window shows the game's messages: copy them into any report.

## Where things are

| What | Where |
|------|-------|
| Settings and controls (`settings.ini`, `controls.ini`) | `%APPDATA%\mhfu-recomp\` |
| Saves | `%APPDATA%\mhfu-recomp\SAVEDATA\` |
| Captures | `%APPDATA%\mhfu-recomp\captures\` |

Texture packs are not looked for in `%APPDATA%`: name the pack's folder with `textures = C:/path/to/pack` in `settings.ini`.

The settings' defaults are for 2560x1440 at 144 Hz. For another screen, set `resolution` (such as `1920x1080`) and `fps` to
your own in `settings.ini` (the README lists the settings).

## Problems

- **`game/decrypted/EBOOT.elf: No such file or directory`:** `game\decrypted` is not two folders above the program.
- **A DLL is missing:** run `tools/win_dlls.py` again, and copy all of `build\win`.
- **`vulkan-1.dll` is missing, or no Vulkan device:** update the graphics driver.
- **No text in menus, and "no PSP fonts" in the console:** `game\fonts` is missing (see above).

## Building on Windows itself

Not tried. MSYS2's CLANG64 environment has what the build needs, as `pacman` packages with the prefix
`mingw-w64-clang-x86_64-`: `clang`, `lld`, `llvm`, `sdl3`, `ffmpeg`, `libpng`, `zlib`, `xxhash`, `vulkan-loader`, `vulkan-headers`,
`shaderc`, `python`; and `make`, `git`, `p7zip`. Then `make WINDOWS=1 WINCC=clang`, with `PYTHONUTF8=1` set for the tools. The
places to expect trouble are `tools/setup.sh` (`rabbitizer` has no wheel for that Python and has to compile) and
`tools/dump_modules.sh`, which assumes Linux's PPSSPP: `dump_modules.sh` shows what it does, by hand in PPSSPP's own settings.

## How it works

`runtime/win/` stands in for what Windows lacks, only in this build: PSP threads as fibers instead of `ucontext`, memory by
`VirtualAlloc`, files opened as bytes, and settings under `%APPDATA%`. `win_compat.h` is included ahead of every file. Under
Wine, the build's RAM hashes match the Linux build's through the intro and title screen; a hunt has been played by hand
but not compared that way. The test suite (`tools/suite.py`) does not run on this build yet.
