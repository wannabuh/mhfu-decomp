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
tools/suite.py                                     # play the test scenarios, compare 30 and 60 (docs/testing.md)
```

## Controls

Each action has its own key; the runtime presses the PSP buttons the game wants for it (`runtime/src/controls.c`):

| Action | Keyboard / mouse | Gamepad | What it presses |
|--------|------------------|---------|-----------------|
| Move | W A S D | left stick | the analog stick |
| Camera | mouse | right stick | (turns the camera) |
| Free look (the camera turns, the hunter keeps going the same way) | Left Alt held | | - |
| Attack / attack 2 | left / right mouse | Y / B | △ / ○ |
| Special attack | middle mouse, E | RB | △ + ○ |
| Roll / dodge | Space | A | × |
| Sprint / guard | Shift | RT | R |
| Interact (talk, gather, carve, open) | F | LT | ○ - with the weapon out, it is put away first |
| Use item | R | X | □ - with the weapon out, it is only put away (sword and shield: used drawn) |
| Sheathe | X | D-pad down | □, only with the weapon out |
| Next / previous item | wheel down / up, 3 / 1 | D-pad right / left | L held, then □ / ○ |
| Reset camera | Tab | LB, L3 | L |
| Map | M, C | Back, D-pad up | SELECT |
| Menu | Esc | Start | START |
| D-pad | arrows | | the D-pad |
| Confirm / back in menus | Enter / Backspace | A / B | × / ○ |

F1 opens the controls screen (the game paused under it): choose an action and press the input to bind to it (Enter
or a click; + or a right click adds it to those it has, Delete clears them), Tab between the keyboard and mouse's and
the gamepad's; closing it saves them. They are kept in `controls.ini` (in the working directory, else
`~/.config/mhfu-recomp/`, written with the defaults on the first run): `action = input, input...` under `[keyboard]` and `[gamepad]`, with the camera stick's speed, its
dead zone and an inverted tilt under `[options]`. In a menu the gamepad is laid out as the PSP is (A ×, B ○, X □, Y △,
the shoulders L and R, the D-pad) and the wheel moves the cursor.

The game's button hints (the item bar's, and others drawn with the HUD's button pictures) show the keys and mouse
buttons bound instead of the PSP's, or the gamepad's buttons once the gamepad is used (`MHFU_PSP_HINTS=1`: the PSP's).

| Key | Does |
|-----|------|
| F1 | the controls screen |
| F10 | release / capture the mouse |
| F11 | fullscreen on / off |
| F2 / F4 | save / load a save state |
| F3 | the next save state slot (1-9) |

The mouse turns and tilts the camera (it is captured from the start; clicking the window takes it back). In a hunt it
turns the game's own camera; where the game fixes the camera (the village, the camp tent) it orbits the view about the
hunter, the movement keys turning with it. With `MHFU_FPS` the camera moves at the display's rate. While the game shows
its own camera (talking to someone, cutscenes) the mouse leaves it alone.

In the menus (the start menu and the game's other list windows - the item pouch, shops... -, the quest lists, the
title's, the supply box's slots) the mouse is a pointer: over an item it moves the game's cursor there, a left click picks it and a right click
goes back; it is captured for the camera again when the menu closes. A window opened from another (an item's choices,
a yes or no) takes the pointer while it is open. A click on the arrows beside a page number or a setting (the Options)
turns the page or changes the setting. Menus drawn some other way still go by the keys alone.

Save states keep the whole game as it is - anywhere, in a hunt or a menu - and load it again at once, as PPSSPP's do:
F2 saves into the slot, F4 loads it, F3 moves on to the next slot (the window's title says which, and what was done).
They are kept in `~/.local/share/mhfu-recomp/states` (`MHFU_STATEDIR=...` elsewhere) and load in later runs and later
builds, unless the runtime's own structures have changed (the window's title then says what). The game's own saves are apart from them: loading a
state does not undo a save made since.

## Settings

Put them in `~/.config/mhfu-recomp/settings.ini` (or `mhfu.ini` where you start the game), one a line, and run just
`build/mhfu`:

```ini
iso = ~/Games/psp/mhfu.iso
fps = 144
mouse_sens = 0.15
```

Each is the environment variable without its `MHFU_` (which still works, and wins over the files): `MHFU_FPS=60` (or
120, 144...: frame rate, the frames between the game's 30 interpolated), `MHFU_TICK_HZ=144` (an experiment: the game
itself at that rate - the hunter and the hunt's camera move every frame, the mouse's camera with them, and what is not
converted yet, still at 30, is drawn between its 30ths; `fps` is then not needed. Not all of the hunter's moves are
checked yet, nor the monsters: `docs/native.md`), `MHFU_SCALE=4` (internal resolution, times the
PSP's, 1 to 12, fractions too; by default the window's own, followed as it is resized, so the picture is shown pixel for
pixel), `MHFU_RESOLUTION=2560x1440` (the internal resolution by its size instead: the scale that fits it, 5.29 here;
larger than the window it is filtered down to it), `MHFU_FULLSCREEN=1` (the whole screen from the start; F11 toggles it), `MHFU_MUTE=1`, `MHFU_NAME=...` (the name the on-screen keyboard gives), `MHFU_SAVEDIR=...`,
`MHFU_RENDERER=soft`, `MHFU_RENDER_THREAD=0` (the renderer's work on the game's thread, not its own), `MHFU_MOUSE_SENS=0.15` (degrees a mouse count turns the camera), `MHFU_CAMERA_ASSIST=1` (the
game turns the camera away from walls itself, as on the PSP: otherwise it stays where the mouse puts it), `MHFU_CAMERA_DISTANCE=1` (how far the camera is from the hunter, times the game's: e.g. 1.3 farther, 0.8 closer - farther can go into walls; with `fps` or `tick_hz`), `MHFU_HUD_SCALE=1` (the hunt's HUD smaller, e.g. 0.75: the clock and gauges towards the top left, the map the top right, the item bar the bottom right), `MHFU_SUN_FLARE=1` (the game's sun flare kept: left out otherwise, as it stood in the middle of the picture), `MHFU_SHADOWS=0`, `MHFU_SUN_SHADING=0`, `MHFU_RELIEF=0`, `MHFU_PIXEL_LIGHTING=0`, `MHFU_POINT_LIGHTS=0`,
`MHFU_POINT_SHADOWS=0`, `MHFU_AO=0` (the renderer's sun shadows, the scenery's sides turned from the sun shaded, its
relief in the sun from its textures, lighting by the pixel, lights
from the game's fires and their shadows, ambient occlusion: all on otherwise), `MHFU_POINT_GAIN=1` (the fires' lights'
strength), `MHFU_AO_STRENGTH=2`, `MHFU_AO_RADIUS=120` (the ambient occlusion's strength, and how far it looks, in the
game's units), `MHFU_HAZE=1` (the air's haze: far hills and the sky's foot fading into the world's fog colour, more where it lies low, lit towards the sun - none in rooms; 0 none, up to 4), `MHFU_WATER=0` (lakes and rivers as the game draws them: otherwise drawn as water - what is under them seen through
them, bent by ripples and darker the deeper it is, the banks and sky reflected, the sun glinting), `MHFU_BLOB_SHADOWS=1` (the game's round shadows under the hunter's and monsters' feet kept: left out otherwise, while the sun's shadows are drawn), `MHFU_MIPMAPS=0` (the game's textures without mipmaps - smaller copies for the distance, sampled trilinear and anisotropic, which keep far ground and grass from shimmering; on otherwise), `MHFU_GRASS=0` (no ground grass: blades of our own wherever the ground is grass, swaying in the wind and bending away from feet, as bright as the ground and shaded by the sun's shadows; on by default), `MHFU_GRASS_DENSITY=1` (how dense: blades over the same ground, e.g. 0.5 or 2; up to 4), `MHFU_GRASS_FULL=700` (how far it is at its full density, in the game's units: about centimetres - all of it made first, there), `MHFU_GRASS_DISTANCE=3000` (how far it reaches: between the two thinner the farther, each blade growing in over a stretch rather than popping up, the last quarter shrinking away; grass on ground just seen grows in over half a second), `MHFU_SHADOW_DISTANCE=3000` (how far the sun's shadows reach each way from what the camera looks at), `MHFU_SHADOW_RESOLUTION=4096` (their map's size: with the distance, how sharp they are), `MHFU_TESSELLATION=0` (the hunter, monsters and people drawn
rounder: their triangles cut finer up close and curved by their normals; on otherwise), `MHFU_SWAY=1` (how far grass, bushes and leaves sway in the wind - and bend away from the hunter and
monsters walking through them; 0 none), `MHFU_POST=0` (none of the
picture's post-processing: motion blur, bloom, light shafts, depth of field, sharpening, colour grading), `MHFU_TAA=0` (no temporal anti-aliasing: edges, grass and texture detail steadied over a few frames; on by default), `MHFU_MOTION_BLUR=0.5` (motion blur, of the camera's motion and the hunter's, monsters' and people's own: how long the shutter stays open, a share of the game's 30th of a second; 0 none), `MHFU_BLOOM=3` (the glow round fires and effects, and
the brightest light; 0 none), `MHFU_SHAFTS=0` (light shafts: the sunlight the air scatters past what shades it, e.g. 1;
the game's sun stands high, so they show little), `MHFU_DOF=0` (depth of field: what is beyond the hunter blurred, the more the
farther; e.g. 0.5 or 1), `MHFU_SHARPEN=0.4` (texture detail
crisper; 0 none .. 1), `MHFU_SATURATION=1.08`, `MHFU_CONTRAST=1.05`, `MHFU_WARMTH=0`
(below 0 cooler, above warmer), `MHFU_VIGNETTE=0.2` (the corners darkened), `MHFU_INTRO=1` (play the intros:
otherwise English is chosen and the logos are skipped, to the title in about two seconds).

### Texture packs

PPSSPP's texture packs work as they are: put the pack (its folder, with its `textures.ini`) at
`~/.local/share/mhfu-recomp/TEXTURES/ULUS10391`, or leave it where PPSSPP has it (`~/.config/ppsspp/PSP/TEXTURES/ULUS10391`),
or name it with `textures = /path/to/pack`. Its images are drawn in place of the game's textures, at their own
resolution (with mipmaps); they are read in the background, so a texture can show as the game has it for a moment
first. PNG images, the `quick`, `xxh32` and `xxh64` hashes, `[hashes]`, `[hashranges]`, `[filtering]`,
`[reducehashranges]` and `ignoreAddress` are understood; a zipped pack (`textures.zip`), DDS / KTX2 images and
`[indexed]` are not yet. `texture_dump = 1` writes each texture the game uses to the pack's `new/` under the name a pack
gives it (as PPSSPP's "save new textures"), to make a pack from.

To get back somewhere quickly - into a quest, say - record playing there once, `MHFU_RECORD=quest.rec`, then start
with `MHFU_REPLAY=quest.rec`: the game replays it in seconds and hands over (saving in the game since can change where
it ends up). Both together, the same file, replay it and carry on recording into it; an existing recording is never
overwritten otherwise (a new one goes to `quest.rec.1`, `.2`...).

## Licence

GPL-2.0-or-later (see `LICENSE`): the runtime reuses code from [PPSSPP](https://github.com/hrydgard/ppsspp),
which is under the same licence.
