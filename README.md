# mhfu-recomp

A static recompilation of **Monster Hunter Freedom Unite** (PSP, US release `ULUS10391`) into a native PC game:
the game's MIPS (Allegrex) code translated to C and built for the PC, running on a runtime that stands in for the
PSP - with a Vulkan renderer, graphical enhancements, a higher frame rate and native PC controls.

**This repository contains no game files, assets or generated translation of the game's code.** You need your own copy
of the game (an ISO of ULUS10391); every tool here reads it from your disk, and nothing extracted from it is committed.
It does contain work based on studying the game's code: functions rewritten by hand (`readable/`), patches to the
translation (`recomp/patches.py`), and names and notes on functions and data (`tools/readable/`).

This is an unofficial fan project, not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of Capcom.

It runs on Linux, and on Windows so far under Wine: see [docs/windows.md](docs/windows.md).

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
| Special attack | Left Ctrl | RB | △ + ○ |
| Roll / dodge | Space | A | × |
| Sprint / guard | Shift | RT | R |
| PSP R | middle mouse | | R |
| Interact (talk, gather, carve, open) | F | LT | ○ - with the weapon out, it is put away first |
| Use item | R | X | □ - with the weapon out, it is only put away (sword and shield: used drawn) |
| Sheathe | X | D-pad down | □, only with the weapon out |
| Next / previous item | wheel up / down | D-pad right / left | L held, then □ / ○ |
| Reset camera | Tab | LB, L3 | L |
| Map | M, C | Back, D-pad up | SELECT |
| Menu | Esc | Start | START |
| D-pad | arrows | | the D-pad |
| Confirm / back in menus | Enter / Backspace | A / B | × / ○ |

F1 opens the settings screen (the game paused under it), as does **PC Settings**, the last entry of the menu START
opens (its second page). It has three pages - Tab, or a click on one: the graphics settings, the keyboard and mouse's
controls and the gamepad's. On the controls' pages choose an action and press the input to bind to it (Enter
or a click; + or a right click adds it to those it has, Delete clears them); closing the screen saves them. They are kept in `controls.ini` (in the working directory, else
`~/.config/mhfu-recomp/`, written with the defaults on the first run): `action = input, input...` under `[keyboard]` and `[gamepad]`, with the camera stick's speed across and up and down
(`pad_camera_speed_x`, `pad_camera_speed_y`, degrees a second; also on the F1 screen), its dead zone, and the tilt inverted for the mouse and the stick apart (`mouse_invert_y`, `pad_invert_y`) under `[options]`. In a menu the gamepad is laid out as the PSP is (A ×, B ○, X □, Y △,
the shoulders L and R, the D-pad) and the wheel moves the cursor.

The game's button hints (the item bar's, and others drawn with the HUD's button pictures) show the keys and mouse
buttons bound instead of the PSP's, or the gamepad's buttons once the gamepad is used (`MHFU_PSP_HINTS=1`: the PSP's).

| Key | Does |
|-----|------|
| F1 | the settings screen (graphics, controls) |
| F10 | release / capture the mouse |
| F11 | fullscreen on / off |
| F2 / F4 | save / load a save state |
| F3 | the next save state slot (1-9) |
| F5 | the performance panel on / off |
| F6 | bug capture: writes a numbered folder (`~/.local/share/mhfu-recomp/captures/NNNN`, `MHFU_CAPTUREDIR=...` elsewhere), says its number on the screen; tell it to whoever is looking at the bug |
| F7 | the click inspector on / off: the mouse is a pointer; click something for what it is, again for the next nearest; what is under the pointer is listed with its textures |
| F8 | with the click inspector open: hold the game / let it run |

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

The graphics settings can be changed in the game: the settings screen's first page (F1, or PC Settings in the game's
menu) lists them - left and right, or a click, change the one chosen - and most take effect at once, the frame under
the screen drawn again with them. A few are read only as the game starts (the frame rates, mipmaps, tessellation, the
GPU's vertex stage, the shadow map's size, the grass's density and reach, and turning the grass on where it was off):
those say NEXT START once changed. Closing the screen writes what was changed into `settings.ini`, leaving its other
lines as they are. (A setting given in the environment is as given there each time the game starts, whatever the
file says.)

By hand: put them in `~/.config/mhfu-recomp/settings.ini` (or `mhfu.ini` where you start the game), one a line, and run just
`build/mhfu`:

```ini
iso = ~/Games/psp/mhfu.iso
fps = 144
mouse_sens = 0.15
```

Whatever is not set there has a default in `runtime/src/main.c` (`setting_defaults`). Without `fps` the game runs at
the screen's refresh rate, and without `resolution` at the window's size. Headless runs and replays leave those defaults
out; `MHFU_DEFAULTS=0` or `1` says otherwise.

Each is the environment variable without its `MHFU_` (which still works, and wins over the files): `MHFU_FPS=60` (or
120, 144...: frame rate, the frames between the game's 30 interpolated), `MHFU_TICK_HZ=144` (an experiment: the game
itself at that rate - the hunter and the hunt's camera move every frame, the mouse's camera with them, and what is not
converted yet, still at 30, is drawn between its 30ths; `fps` is then not needed. Not all of the hunter's moves are
checked yet, nor the monsters: `docs/native.md`), `MHFU_SCALE=4` (internal resolution, times the
PSP's, 1 to 12, fractions too; by default the window's own, followed as it is resized, so the picture is shown pixel for
pixel), `MHFU_RESOLUTION=2560x1440` (the internal resolution by its size instead: the scale that fits it, 5.29 here;
larger than the window it is filtered down to it), `MHFU_FILL=0` (a 16:9 screen is 0.7% wider than the PSP's: the picture is drawn that much wider to fill it, all 2560 of 1440p's columns; 0 keeps the PSP's shape exactly, 2541 across with a thin bar each side), `MHFU_FULLSCREEN=1` (the whole screen from the start; F11 toggles it), `MHFU_MUTE=1`, `MHFU_AUDIO_SYNC=0` (the sound is kept in time with the game: what piles up behind a hitch is hurried through or dropped; 0 leaves it, as it was), `MHFU_AUDIO_LOG=1` (every five seconds, how long each channel's sound waits to be played), `MHFU_NAME=...` (the name the on-screen keyboard gives), `MHFU_SAVEDIR=...`,
`MHFU_RENDERER=soft`, `MHFU_RENDER_THREAD=0` (the renderer's work on the game's thread, not its own), `MHFU_SCENERY_LOD=1` (the stage's repeated pieces - the desert's ground tiles - with the coarser model the game draws beyond 1500 units, not the full one at any distance), `MHFU_WEAPON_BLOOD=1` (how many drops of blood a weapon throws off as it hits and for the rest of that swing, times the usual: 0 none, up to 4; `MHFU_WEAPON_BLOOD_SIZE=1` how big each is, 0.25 to 4; both in the settings menu too), `MHFU_BLOOD_TRICKLES=1` (unfinished, off by default: blood runs down a monster from its wounds - trickles that go down the skin from triangle to triangle, by how the animal stands as they go, stay where they ran for as long as the animal is there, and fall as drops from under it - in place of each wound's own short runs; on a monster whose hide is in layers, a Popo, most of a trickle is still hidden under the outer layer; `MHFU_TRICKLES=5` that many from every wound and `MHFU_TRAIL_LOG=1` what they do, to look at them), `MHFU_BLOOD_AMOUNT=1` (how bloody a monster gets: every hit leaves blood about where it landed, by how hard it was - a blunt one half as much - and one landing in blood already there adds to it, so that a part hit again and again is covered; times as much, 0 none; "how bloody monsters get" in the settings), `MHFU_BLOOD_RUN_SPEED=1` and `MHFU_BLOOD_DRIP_SPEED=1` (how fast blood runs down a monster, and how fast the drops dripping from it fall, times the usual, 0.25 to 4; both in the settings menu), `MHFU_BLOOD_DARKNESS=1` (how dark blood is - on a monster, in the air, on the ground: 0 twice as light as made, 2 half; in the settings menu), `MHFU_BLOOD_STREAK=1` (how long a streak a drop of blood in the air is drawn as, at most: times twice its width - 0 a round drop, up to 4; in the settings menu), `MHFU_BLOOD_OVER=1` (the ground's blood drawn on the layers over the world too - mist, dust, a sheen - as it was), `MHFU_BLOODMAP=0` (the blood on a monster worked out about each wound by the pixel, as it was, and not kept texel by texel in a map of the animal's own - splashes where the hits landed, runs down from the cuts; `MHFU_BLOODMAP_SHOW=1` that map shown in squares on the animal, `MHFU_UV_DUMP=file` every lit model's triangles written out), `MHFU_WOUND_TEST=n,far,high,hard` (to look at it without a fight: n blows on the first monster, that far from the hunter towards it and that high, as hard as that damage; `MHFU_WOUND_TEST_EVENT=1` as if the monster flinched at each, `2` as if a part broke), `MHFU_BLOOD_OPACITY=1` (how solid the blood that has run down a monster is: 0 thin, the skin showing through, 1 its own colour over the skin; `MHFU_BLOOD_SHINE=0.6` how wet blood on a monster shines, times, 0 none, up to 2; all three in the settings menu), `MHFU_WOUNDS=0` (no slash left on a monster where a hit lands; `MHFU_WOUND_SIZE=2` twice as large, `MHFU_WOUND_SECONDS=180` to have them fade - a middling one in that long, a scratch two thirds of it, a gash five thirds; not set, a wound and its blood stay as long as the animal is there; `MHFU_WOUND_DEPTH=0..1` every wound that deep and `MHFU_WOUND_AGE=40` every wound drawn that many seconds older, to look at one; a deep wound is drawn from a picture, `wound.png` in `~/.config/mhfu-recomp/` if there is one - `docs/wound-template.png` is the one made otherwise, to paint over: the cut runs along it, its alpha is how much of the skin is covered), `MHFU_PAINTED_SHADOWS=1` (the shadows the game paints by its buildings kept beside the renderer's own), `MHFU_BACKDROP_SHADOWS=1` (the far scenery casts shadows too) and `MHFU_SHADOW_CLAMP=1` (what is beyond the shadow map's range towards the sun still casts), `MHFU_CAMERA_CATCH=1` (up against the scenery the game holds its camera, as it did), `MHFU_GAME_NAME_TAGS=1` (the names over people where the game's own camera puts them, not the one shown), `MHFU_TALK_CAMERA=1` (talking to someone the game's camera shown at game frames only, not smoothed between them), `MHFU_MOUSE_SENS=0.15` (degrees a mouse count turns the camera), `MHFU_MOUSE_SMOOTHING=40` and `MHFU_PAD_SMOOTHING=40` (the camera eased into and out of the mouse's, and the right stick's, turns: about how many milliseconds it takes to follow two thirds of a motion, e.g. 30 light, 80 heavy; it still comes to rest where you aimed it; 0, the default, none; `MHFU_CAMERA_SMOOTHING` sets both), `MHFU_CAMERA_ASSIST=1` (the
game turns the camera away from walls itself, as on the PSP: otherwise it stays where the mouse puts it), `MHFU_CAMERA_DISTANCE=1` (how far the camera is from the hunter, times the game's: e.g. 1.3 farther, 0.8 closer - farther can go into walls; with `fps` or `tick_hz`), `MHFU_HUD_SCALE=1` (the hunt's HUD smaller, e.g. 0.75: the clock and gauges towards the top left, the map the top right, the item bar the bottom right), `MHFU_SUN_FLARE=1` (the game's sun flare kept: left out otherwise, as it stood in the middle of the picture), `MHFU_SHADOWS=0`, `MHFU_SUN_SHADING=0`, `MHFU_RELIEF=0`, `MHFU_PIXEL_LIGHTING=0`, `MHFU_POINT_LIGHTS=0`,
`MHFU_POINT_SHADOWS=0`, `MHFU_AO=0` (the renderer's sun shadows, the scenery's sides turned from the sun shaded, its
relief in the sun from its textures, lighting by the pixel, lights
from the game's fires and their shadows, ambient occlusion: all on otherwise), `MHFU_POINT_GAIN=1` (the fires' lights'
strength), `MHFU_AO_STRENGTH=2`, `MHFU_AO_RADIUS=120` (the ambient occlusion's strength, and how far it looks, in the
game's units), `MHFU_HAZE=1` (the air's haze: far hills and the sky's foot fading into the world's fog colour, more where it lies low, lit towards the sun - none in rooms; up to 4; by default 0, none: at 1 it hides the village's distant mountains and clouds), `MHFU_WATER=0` (lakes and rivers as the game draws them: otherwise drawn as water - what is under them seen through
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
first. PNG and KTX2 images (KTX2 needs the `basis-universal` package when building; `make` uses it when it is
installed), the `quick`, `xxh32` and `xxh64` hashes, `[hashes]`, `[hashranges]`, `[filtering]`,
`[reducehashranges]` and `ignoreAddress` are understood; a zipped pack (`textures.zip`), DDS images and
`[indexed]` are not yet. Install a pack as its readme says for PPSSPP (for a Freedom Unite pack, its US
`textures.ini`); PPSSPP's "Replace textures" setting has no counterpart here, a pack is used whenever it is there. `texture_dump = 1` writes each texture the game uses to the pack's `new/` under the name a pack
gives it (as PPSSPP's "save new textures"), to make a pack from.

To get back somewhere quickly - into a quest, say - record playing there once, `MHFU_RECORD=quest.rec`, then start
with `MHFU_REPLAY=quest.rec`: the game replays it in seconds and hands over (saving in the game since can change where
it ends up). Both together, the same file, replay it and carry on recording into it; an existing recording is never
overwritten otherwise (a new one goes to `quest.rec.1`, `.2`...).

## Licence

GPL-2.0-or-later (see `LICENSE`): the runtime reuses code from [PPSSPP](https://github.com/hrydgard/ppsspp),
which is under the same licence. The licence applies to this project's own work and grants no rights in the game itself.
