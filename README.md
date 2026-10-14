# mhfu-recomp

A static recompilation of **Monster Hunter Freedom Unite** (PSP, US release `ULUS10391`) into a native PC game:
the game's MIPS (Allegrex) code translated to C and built for the PC, running on a runtime that stands in for the
PSP - with a Vulkan renderer, graphical enhancements, a higher frame rate and native PC controls.

**This repository contains no game code or data.** You need your own copy of the game (an ISO of ULUS10391); every
tool here reads it from your disk, and nothing extracted from it is ever committed.

Status: phase 5 (the recompiled game is playable into Pokke Village: Vulkan renderer, music, PC controls). See [docs/plan.md](docs/plan.md).

## Layout

| Path       | What                                                                                  |
|------------|---------------------------------------------------------------------------------------|
| `docs/`    | the plan, notes on the PSP and on the game as we learn it                            |
| `tools/`   | extraction and analysis scripts (Python, in a local environment: `tools/setup.sh`)   |
| `recomp/`  | the recompiler: the game's functions to C (`build/gen/`, ignored)                    |
| `runtime/` | what the recompiled code runs on (`runtime/include/recomp.h` is the contract)        |
| `game/`    | (ignored) your ISO's extracted files - `tools/extract_iso.sh <your.iso>`             |

## Getting started

```sh
tools/setup.sh                      # a Python environment with the tools' modules (tools/.venv)
tools/extract_iso.sh path/to/mhfu.iso   # extracts to game/ and checks it is ULUS10391
tools/dump_modules.sh path/to/mhfu.iso  # decrypted EBOOT and PRXs to game/decrypted/ (via PPSSPP, offscreen)
tools/.venv/bin/python tools/databin.py overlays   # the 298 code overlays from DATA.BIN to game/overlays/
tools/.venv/bin/python tools/databin.py extract    # (optional) all 6657 DATA.BIN files to game/data/
tools/.venv/bin/python tools/funcs.py              # every function, to game/analysis/functions.json
tools/.venv/bin/python recomp/recomp.py            # the functions as C, to build/gen/
make -j$(nproc)                                    # compile them and the runtime (build/)
build/mhfu path/to/mhfu.iso                        # run it (controls below)
tools/.venv/bin/python tools/difftest.py           # check recompiled functions against PPSSPP
```

## Controls

| Key | Does (PSP button) | Key | Does (PSP button) |
|-----|-------------------|-----|-------------------|
| W A S D | move (analog stick) | Shift | sprint (R) |
| Space | dodge / roll (×) | F | interact: talk, gather, carve (×) |
| Left mouse | primary attack (△) | Right mouse | secondary attack (○) |
| Middle mouse / E | special attack (△ + ○) | Q | use item / sheathe (□) |
| Tab | reset camera (L) | Esc | menu (START) |
| Arrows | D-pad: menus, camera | Enter / Backspace | confirm / back in menus (× / ○) |
| C | SELECT | F10 | release / capture the mouse |

The mouse turns and tilts the camera (it is captured from the start; clicking the window takes it back). In a hunt it
turns the game's own camera; where the game fixes the camera (the village, the camp tent) it orbits the view about the
hunter, the movement keys turning with it. With `MHFU_FPS` the camera moves at the display's rate.

A gamepad works as laid out (south ×, east ○, west □, north △, left stick).

## Settings

Put them in `~/.config/mhfu-recomp/settings.ini` (or `mhfu.ini` where you start the game), one a line, and run just
`build/mhfu`:

```ini
iso = ~/Games/psp/mhfu.iso
fps = 144
mouse_sens = 0.15
```

Each is the environment variable without its `MHFU_` (which still works, and wins over the files): `MHFU_FPS=60` (or
120, 144...: frame rate, the frames between the game's 30 interpolated), `MHFU_SCALE=4` (internal resolution, times the
PSP's; by default as many lines as the screen has: 6 at 1440p), `MHFU_MUTE=1`, `MHFU_NAME=...` (the name the on-screen keyboard gives), `MHFU_SAVEDIR=...`,
`MHFU_RENDERER=soft`, `MHFU_MOUSE_SENS=0.15` (degrees a mouse count turns the camera), `MHFU_CAMERA_ASSIST=1` (the
game turns the camera away from walls itself, as on the PSP: otherwise it stays where the mouse puts it), `MHFU_SHADOWS=0`, `MHFU_PIXEL_LIGHTING=0`, `MHFU_POINT_LIGHTS=0`, `MHFU_POINT_SHADOWS=0` (the renderer's sun
shadows, lighting by the pixel, lights from the game's fires and their shadows: all on otherwise), `MHFU_POINT_GAIN=1`
(the fires' lights' strength), `MHFU_INTRO=1` (play the intros:
otherwise English is chosen and the logos are skipped, to the title in about two seconds).

To get back somewhere quickly - into a quest, say - record playing there once, `MHFU_RECORD=quest.rec`, then start
with `MHFU_REPLAY=quest.rec`: the game replays it in seconds and hands over (saving in the game since can change where
it ends up). Both together, the same file, replay it and carry on recording into it; an existing recording is never
overwritten otherwise (a new one goes to `quest.rec.1`, `.2`...).

## Licence

GPL-2.0-or-later (see `LICENSE`): the runtime reuses code from [PPSSPP](https://github.com/hrydgard/ppsspp),
which is under the same licence.
