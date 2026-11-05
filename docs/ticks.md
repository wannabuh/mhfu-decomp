# What still ticks at 30

The list behind goal 1 of `docs/roadmap.md`: what runs at the display's rate, what doesn't, what that looks like at 144, and the order to
convert it in. Surveyed 2026-11-05 on `a6ca90e`; no game behaviour changed. `docs/native.md` explains gates and smoothing.

Each claim is marked **[M]** measured, **[R]** read from code, or **[I]** inferred. Runs are headless, on Vulkan, with `MHFU_FPS=0` unless
stated. The fights are `tests/data/rec/overnight-fight1.rec` and `-fight2.rec` with the `overnight` saves.

## Short version

- Half the code a fight runs still runs at 30ths: 1,047 functions and 101,000 generated lines, against 891 and 119,000 every frame **[M]**.
- At 144 almost nothing *looks* like 30. Monsters are smoothed well, and the worst things left aren't stepping. They are:
  - **rate bugs in code already converted:** hit-stop lasts a fifth as long at 144, poison runs out 4.8 times as fast, camera easing and shake run fast;
  - **the pad, read only at 30ths:** the hunter ticks every 7 ms but hears a button up to 33 ms late;
  - **a stall of 50-80 ms** about every 30 seconds in fight 2, cause unknown.
- So fix those first, and convert monsters after them. Monster conversion buys less lag and sharper fast arcs, not less stepping.

## 1. What runs at what rate

`MHFU_NATIVE_LOG=2`, both fights **[M]**. Only objects going through an object gate are logged:

| vtable | what | where | at 60 and 144 |
|---|---|---|---|
| `089BB3CC` | hunter in a hunt | both | every frame |
| `089BC680` | Popo, Aptonoth, Apceros (types `0x46`, `0x0C`) | both | every frame |
| `089BB05C`, `089BAF48` | hunter in the village, hunter variant | before the quest | 30, smoothed |
| `089BB154` | village NPCs, 15 of them | before the quest | 30, smoothed |
| `089BCA70` | Giadrome (type `0x4D`, fight 1) | | 30, smoothed |
| `089BCD64` | Congalala and Conga (`0x34`, `0x3E`, fight 2) | | 30, smoothed |
| `089BC77C` | Giaprey, Velociprey | | 30, smoothed |
| `089BC974`, `089BC194`, `089BC878`, `089BC584` | Vespoid, Kelbi, type `0x12`, type `0x0A` | | 30, smoothed |

Species come from the type byte `+0x1E8` in RAM snapshots, named by `classes.toml`, whose species names are inferred.

**Every game frame [M]** (`tools/readable/ticks_hook.c`: each function's calls on frames between 30ths, under a gate, or neither):

- the render pass (`0884BB58`) and what it reaches, 121 functions and 28,000 lines
- the hunter's tick, input, matrices and spring chains
- the hunt's cameras (`08812BD0`)
- the Popos' ticks
- the motion player, and the hit records' clear

**At 30ths only [M]:** everything else, in section 2's table. The gates are `recomp/patches.py`'s `GATES`:

| gate | what | |
|---|---|---|
| `0888FD5C` | the pad | section 5, item 2 |
| `0888FD68`, `0888FDD4` | text | |
| `0888FD80` | the mode's main | |
| `0888FD8C` | asset slots | |
| `0888FD98` | screen states | |
| `0888FDA4` | game states, which hold the hunt and the village | |
| `0888FDBC` | render queues, run on the frame before a 30th | |
| `0888FDC8` | sound manager | |
| `088590F8`, `08859104`, `08859110` | the frame wait's three calls | still unnamed |
| `088659AC` | a character's upkeep | |

Native calls reopen only the hunter's paths, the cameras, the object loops and the hit clear. In the village nothing is reopened: there it is all 30, shown smoothed **[R]**.

## 2. The table

Size counts **own** functions and generated lines: what only that system reached in the two fights at 60. Another 162 functions and 12,053 lines are shared between systems. *ints* and *floats* are places a rate could hide: integer +1/-1 and float adds stored straight back, from `tools/ticks.py`. They are candidates, not findings.

| System | Rate now | At 144 | Conversion needs | Size: funcs / lines / ints / floats | Risk |
|---|---|---|---|---|---|
| Hunter | every frame, upkeep at 30 | fine **[M]**: action lengths match within a 30th or so, and the field scan finds only hit-stop as a counter | fix the rate bugs in section 2.1; other weapons unchecked | every frame already | low |
| Hunter's weapon and attacks | moves every frame; attack volumes and hit tests at 30 | swing smooth; hit-stop wrong **[M]**; hits land at 30ths | hit-stop patch; keep hits at 30 | 1 line for the patch | low |
| Large monsters, `game_task` | 30 | smoothed well **[M]**, shown a 30th late **[R]** | base timers, then each class | base 413 / 36,752 / 350 / 171; Congalala family 211 / 29,333 / 399 / 51; Drome 95 / 7,194 / 134 / 18 | high: AI, hits, state timing |
| Large monsters, `em*` | 30 | unknown, **no recording reaches one** | the same, plus recordings | 17 overlays, 299,214 lines in all; 6,253 to 51,099 each | highest: nothing can check them today |
| Small monsters | 30 | smoothed **[M]** | base timers, then each class | Prey 106 / 7,101 / 149 / 10; Vespoid 83 / 5,889 / 188 / 75; Kelbi 65 / 5,376 / 231 / 24 | medium |
| Popos | every frame | fine in looks; base timers still count frames **[M from the hook, R]** | the same base timers | 82 / 8,393 / 189 / 83, done bar those | low |
| NPCs | 30, village only | smoothed **[R]** | no reason to convert yet | not measured | low value |
| Projectiles, hit volumes | 30 | draws matched and smoothed **[M]**; not told apart from other draws | leave at 30 with hits | 48 / 7,114 / 57 / 152 | medium |
| Particles, effects | tick at 30, drawn every frame | places smoothed; 0 to 29 draws a frame unmatched **[M]**; colours and texture scroll step at 30ths **[R]** | don't convert: smooth colour and UV in `gpu.c` | 72 / 19,407 / 89 / 859 | high to convert, low to smooth |
| Camera | every frame | steady, but eases and shakes too fast **[M, R]** | scale its steps | about 15 functions run every frame with steps | low to medium |
| HUD | 30, smoothed | gauges fine: 48-84 draws a frame matched, 0-3 not **[M]**; changing text steps | none | 130 / 15,221 / 146 / 29 | no need |
| Animation players | every frame for converted objects, under the gate for others | fine; patches see scale 1 under gates **[R]** | done, one function | | low |
| Sound cues | fire from the object's tick | once each, by `08864348` **[R]**; the sound manager is at 30 | none | 84 / 3,703 / 31 / 0 | no need |
| Physics, collision | hunter's every frame; push and hits at 30 | no visible fault found | leave | 4 / 711 / 0 / 1, plus hits 26 / 5,184 / 22 / 35 | medium |
| Quest, timers | 30 | fine: timers go by game time | none | 11 / 1,605 / 40 / 2 | no need |
| Menus | 30, smoothed | windows slide smoothly **[R, per `docs/native.md`]**; waits in 30ths | none for rate; roadmap goal 3 may replace them | not measured | no need |

Species sizes are what a class's vtable reaches by direct calls and no other class does **[R]**. State tables held in data aren't followed, so they are floors.

### 2.1 Rate bugs in converted code

Found by asking which fields step by 1 on three or more game frames in a row (`tools/ticks.py streak`). A count in 30ths can't do that above 60.

| What | Evidence | 30 / 60 / 144 | What you'd see |
|---|---|---|---|
| **Hit-stop**, `motion_slow_timer` `+0x558`, in `088D5C7C` (`hunter_run.c:525`) | **[M]** | 5.0 / 2.5 / 1.04 30ths | Hits lose their weight: a sword barely catches at 144. Moves shorten a little too |
| **Poison and other drains**, `drain_status_seconds` `+0x388`, in `09A6D418` | **[M]**: 20 seconds poked in | a "second" lasts 30 / 15 / 6.25 30ths | Poison ends in about 4 s at 144. Its counters step per tick too, so health loss may speed up: **not measured** |
| Camera height ease | **[M]** on a slope: target-to-hunter swing | 161-183 / 164-178 / about 170-173 | The camera settles too fast, so it feels stiffer on hills. Harmless, but it isn't the game's feel |
| Camera shake countdown, `08814E18` | **[R]**, and **[M]** that it runs every frame | | Shake is over sooner and flickers faster |
| Weapon effect ops, `088CF2C8` and `088D08D4` with flag `0x30`, from `hunter_draw_weapon_model` in the render pass | **[R]**, and **[M]** that it runs on every frame | twice / 4.8 times the steps | Weapons whose ops run pulse too fast. The test's weapon has none running (its state stays 0), so **not seen** |
| Hunter phase counters `+0x426`, `+0x427`, `+0x43E`, in `09A6FB68` | **[R]** | | Not seen in these fights |
| A monster base's timers on Popos: `09AC5C30`, `09AC9910`, `09AC9C08`, `09AD2698`, `09AD7850`, `09AD8378` | **[M]** every frame, **[R]** unscaled | | Popos forget or calm down too quickly; barely visible |

## 3. Do the fights stay correct above 30?

**Short answer: one fault proven (hit-stop), one open lead, and the replays part within seconds, so they can't show more.**

- Both fights replay to the end at 60 and 144 with no crash **[M]**. A second run at 60 gave identical RAM hashes, so runs are deterministic **[M]**.
- `ratecheck` only runs at 30 or 60, on the soft renderer. I gave it Vulkan traces with `--reuse`. Fight 1: 14 actions paired, parting at 30th 3141. Fight 2: 12 paired, parting at 2842. With the mouse off after the switch it found `+0x558` (the hit-stop) and `+0x2AC` (the tint already allowed). Its "covers" lines come from the jump described below **[M, I]**.
- **Why they part.** With the recorded mouse, the converted mouse steers the hunter apart within seconds, as `docs/testing.md` says. So I removed the mouse after the switch, at every rate. Then:
  - Congalala stays within 0.01 units of its place at 30 until 30th 2667 at 60 and 2670 at 144, so a gated monster really is the same until something reaches it **[M]**;
  - at 30 it changes motion (`0x3EB` to `0x3EE`) at 2667, at 60 at 2668.5, at 144 about 2670 **[M]**. From then on they are different fights: its next random number, its choices and the hunter's all differ;
  - in the same frame **the hunter's place jumps about 45 units sideways**, at every rate. That jump comes 0 30ths after the draw attack (`0x597`) starts at 30, 2 at 60, 6 at 144 **[M]**. I didn't find why, or whether it causes the monster's change or the reverse. **This is the one open lead on a real rate bug in a fight**, worth a look before anything is built on these recordings;
  - before that the hunter is only a few units off: 4 by 30th 2665 at 60. That is the known difference in `docs/testing.md`, "a run's start" **[M]**.
- After the fights part, the hunter's and monster's use of shared random stream 0 (`088900C4`) reorders too, so no later moment can be compared **[I]**. Sound's most frequent draws use stream 1 **[R]**.
- **Comparing by kind instead of by moment** (action lengths by action id over five runs at each rate): the moves agree within a 30th or so where the input does. `0582` 13 / 12.5 / 12.1, `0589` 19 / 19 / 18.8, a full charge `0579` 84.5 / 84 / 84.2 **[M]**. The others that differ (`0003`, `0026`, `00D1`, `00D2`, `0584`, `0594`) are mostly standing, running and being hit, whose length follows the fight **[I]**. Congalala's gated motions match at 60 and mostly at 144, where `03EF` reads 57.5 against 38, probably repeats of the same motion run together **[I]**.
- **Hits:** not checked hit by hit. Hunter and monster health fall by plausible amounts at every rate, with no sign of doubling. Congalala lost 26 at 30, 94 at 60, 75 at 144 in the no-mouse runs: that follows the fight, not the rate **[I]**.

**The rate checks still don't cover real fights** past their first few seconds. To get further, compare by kind, as `tools/ticks.py streak` and action lengths do. Exact comparison would need the jump above explained and the random stream's order made independent of the rate. `auto/shadow-random` touches the same question for shadows **[R]**.

## 4. Frame cost at 144

Your settings (`settings.ini`, 2560x1440, shadows and point lights on, `gpu_tl` 2), headless, machine idle, real time from the Congalala fight:

| Mode | Rate held | Idle | Frames left out |
|---|---|---|---|
| Native 144, fight 2, worst stretch | 143.9 | 20-23% | 0 |
| Native 144, fight 2, later | 143.9 | 48-59% | 0 |
| Native 144, fight 1 | 143.9 | 55% | 0 |
| Combined, tick 60 + `fps` 144, fight 2 | 144.0 | 29-34%, a frame shown 2.3-2.6 ms | 0 |

- **The same run with others loading the machine (load 18+, GPU 93%)** gave 87-147 a second and about 300 frames left out in each 5 s. That is the difference load makes, not the game's cost **[M]**.
- **Game code is 2% of the samples** at the worst stretch (`MHFU_PROFILE`, 7000-11000): GE and render 57%, libraries and driver 27%, other runtime 14% **[M]**. Converting more objects won't hurt frame time noticeably **[I]**.
- **A stall of 53-78 ms** recurs about every 30 s in fight 2 in native and combined modes, against 8-10 ms normally **[M]**. Not in fight 1 after its start. Cause unknown; another process on the machine isn't ruled out. This is probably the most visible stutter left, if it shows up in play.
- **Not measured:** a real window and its present path, the ice cave and Pokke Village under these settings, and `overnight-fight2`'s area 9 on its own.

## 5. Recommended order

1. **Fix the rate bugs in section 2.1.** A few lines each, on code already converted. *You'd see:* hits that catch as they do at 30, poison that lasts as long, and at 60 the same. Add `tools/ticks.py streak` to the checks. Risk low.
2. **Find the 50-80 ms stall.** Cheap to look into. *You'd see:* no hitch every half minute, if it is the game's.
3. **Read the pad every frame in a hunt.** Hand each frame's new presses and releases to the hunter, keeping what gated code sees at 30ths the same. It needs care with `EDGES`, `COMBINE` and `L_HELD`, and with menus staying as they are. *You'd feel:* attacks and rolls starting up to 33 ms sooner, the largest change in response left. Risk medium.
4. **Scale camera easing and shake.** About 15 functions, on one path. *You'd see:* the camera moving over slopes as at 30, and shakes as long. Risk low to medium.
5. **Explain the hunter's jump at the draw attack (section 3).** It may be a real rate bug. Then decide how to compare fights: by kind, or with the random stream's order fixed. Nothing else visible, but monster work can't be proven without it.
6. **Monsters' base timers, then small classes.** Start with those every class shares (`+0x4BE`, `+0x514`, `+0x550` were seen, and the rest of section 2.1's last row), then Drome and Prey, then Congalala. *You'd see:* monsters no longer a 30th behind the hunter, so a hit and its reaction line up, and fast lunges and turns follow their true curve. They won't stop stepping, as they already don't. Risk high; keep hits at 30ths.
7. **`em*` monsters need recordings first.** Rathian and the rest have 299,000 lines and no way to check them. Ask for a recording of each kind, or a poke to reach them.
8. **Effects' colours and texture scroll, in `gpu.c`, not by conversion.** *You'd see:* fades and scrolls without steps. Whether they are noticeable at all is unknown: check by eye first.
9. **Leave alone:** HUD, text, quest, sound, menus, hits, push. The roadmap's "done when" ("no system visibly running at a lower rate") is met for these by smoothing, and converting them adds risk with nothing to see. Its other half, "no behaviour depends on the frame rate", is what items 1 and 4 are for.

**The roadmap's guess needs changing:** monsters are listed as "biggest visible part left". Measured, they aren't: smoothing already handles their stepping.

## How monsters look now

`MHFU_MOTION_LOG`, Congalala's fight **[M]**. *Stalled* is frames where a model's root barely moves on screen; *rough* is its change of step against its step. Only models drawn less than every frame are counted here, taken to be the monsters and the like **[I]**:

| | stalled | rough |
|---|---|---|
| 144, smoothed | 2-4% | 0.07-0.10 |
| 144, `MHFU_NATIVE_SMOOTH=0` | 27-29% | 0.15-0.17 |
| 60, smoothed | 2-7% | 0.26-0.36 |
| 30 | 5-11% | 0.46-0.47 |

**Draws at 144 between 30ths** (a survey build of `gpu.c`, fight 2, medians a frame) **[M]**:

| | draws | primitives |
|---|---|---|
| static | 6,078 | 27,649 |
| smoothed | 1,030 (up to 2,290) | 5,460 |
| moving by themselves: the hunter, Popos | 952 | 5,063 |
| matched, no earlier place to smooth from | 58 | 296 |
| unmatched, drawn as is | 0 (up to 29) | 0 |

Colour isn't part of this. A frame dump near a hit showed gauges moving and nothing wrong, but the camera was behind a leaf, so dumps say nothing about particles here.

## The monster experiment

`MHFU_NATIVE_VT` with every monster class in the fights, at 60, in two runs **[M]**: both played to the end without a crash, health and motions continuing. Most motions kept their lengths within a 30th. Some differ and I didn't judge them: `03FC` 19.5 against 28, `03EB` 185 against 16 (3 instances; input or a stuck state unknown). `streak` lists `+0x550`, `+0x514`, `+0x4BE`, `+0x698`, `+0x699`, `+0x554` and `+0x7F4`. Some of those may be other things than timers. **This shows conversion isn't hopeless. It doesn't show it's correct.** Not tried at 144.

## What the hand-written code changes

- **It found the hit-stop's cause at a glance.** `self->motion_slow_timer--` in `hunter_run.c` reads as what it is, beside `freeze_ticks`, which is already converted. The patch is `-= rt_tick30`. Its readable twin needs the same change as the generated code's.
- **The monster script interpreter is mostly rate-free.** A grep of `monster_script*.c`, not a full read: it tests timers (`area_table_ticks <= 0`) and picks by `script_random_counter`, and doesn't count them, except `request_bit_8_ticks` and `script_delay_ticks`, which it sets. Counting happens in the tick functions (`09AC5C30` and kin), which aren't hand-written. So the interpreter needn't change, and those are what to write next.
- **Field names help.** `fields/Monster.toml` names 62 fields as ticks, timers or counts, a ready checklist for step 6.
- **Hit code in `readable/` runs at 30ths** and is unaffected.

## What I couldn't determine

- How any of this looks in a real window: everything is from traces, logs and dumps, and you haven't played these.
- Other weapons: both fights use weapon class 0. Attacks' timers elsewhere are unchecked.
- Whether poison's health loss speeds up too, as its counters suggest.
- Rate behaviour of anything in `em*`; other hunters' objects (none here); carts and the quest's end at 144.
- Whether effects' stepping in colour is visible.
- The stall's cause.
- Whether `+0x698`, `+0x699`, `+0x554` and `+0x7F4` on monsters are counters at all.

`docs/native.md` says `089BC584` is what a Popo has as it's made. In these fights it's a type `0x0A` creature, 200 health, not moving: **that note looks stale**, though only `089BC680` is converted, so nothing depends on it.

## To measure it again

    tools/ticks.py convert SRC DST 144 SWITCH --zero-mouse      # a recording at any rate, each input on the frame its 30th falls on, no mouse after the switch
    MHFU_TICK_HZ=144@SWITCH MHFU_NATIVE_TRACE=trace MHFU_TRACE_WORDS=0,800 MHFU_TRACE_VT=089bb3cc ...
    tools/ticks.py streak trace@SWITCH                          # fields counting game frames
    tools/ticks.py static                                       # sizes per overlay and class
    tools/ticks.py systems FILE...                              # a hook's files, by system
    tools/ticks.py ran FILE                                     # a hook's file: every frame against 30ths

`tools/readable/ticks_hook.c` is linked into a `RECOMP_TRACE_ALL=1` build, with `MHFU_TICKS_OUT`, `MHFU_TICKS_FROM` and `MHFU_TICKS_TO`. The measuring here was done with a variant for the older hook signature, linked against the `tracebuild` worktree at `9a0ca2d`, whose gates match today's; the version in the repository has the current signature and hasn't been run. The runs and logs are in `~/projects/mhfu-ghidra/work/scratch-ticks/`.
