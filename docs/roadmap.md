# Roadmap

What we want this project to do, in order, and what's next for each goal. Read this at the start of a session and update it at the
end. Results and history go in `docs/readable-report.md` and the commit log; how to prove a rewrite is in `docs/readable.md`.

**Draft, 2026-11-05.** Goals are the user's. The order within a goal, and everything marked *(check)*, is from notes and not yet
confirmed against the current build.

## Goals, in order

1. Smoother gameplay: a fixed 60 ticks a second, shown at the display's rate (144 fps and up) by interpolation
2. Graphics: native options, fixes for current artifacts, further enhancements that stay fast
3. Better PC support: native interface and controls
4. Modding: tools and workflows for textures, meshes, attacks, particles, monsters, and maybe areas

## Decisions

- Convert code by hand only when a goal needs it. Everything else stays at names and types: converting the whole game isn't a goal.
- No Rust rewrite.
- No complete source that stops needing the user's own ISO. Mods patch or add to the user's copy; a shared mod holds new work, not the
  game's files.
- Merge into `native-fps` only at the user's word. "Verified" and "played" are separate states.
- The game ticks at a fixed 60 (user's decision, 2026-11-05): no 120 or 144 ticks, no variable tick. Display rate comes from interpolation.
- Prefer branches; a new worktree only when two things must run at once.
- One set of baselines (user's decision, 2026-11-05): once the shadows fix merges, `tools/suite.py` on Vulkan compares against
  `tests/data/baselines`, the software ones, and `baselines-vulkan` is retired after the merge has settled. Tests still run on Vulkan.
  The first deliberate RAM change re-blesses them from Vulkan, which ends their independence from the code; say so in that commit.

## Status words

- **Not started**, **In progress**
- **Verified:** passes the checks in `docs/readable.md` or `docs/testing.md`
- **Played:** the user has tried it
- **Merged:** in `native-fps`

## 1. Smoother gameplay

**Done when:** the game plays at 60 ticks shown at 144 fps with no system visibly running at a lower rate, and everything lasts the same real time as at 30.

| Work | Why | Status | Next step |
|---|---|---|---|
| Hunter at native rate | Controls and motion feel | Merged; played | Keep checking per weapon |
| Tick 60 + 144 fps combined mode | Current best mode | Merged; played | |
| What still ticks at 30 | Tells us what's left | Done: `docs/ticks.md`, reviewed in part | |
| Rate bugs: hit-stop, poison and other drains, camera ease and shake, weapon effect ops | Hits lose weight, poison ends early, by measurement | In progress on `auto/rate-fixes` | Fix, then play |
| The hunter's spring chains at 60 ticks | Hair and loose armour may move wrongly, if their steps aren't scaled *(check)* | Not started | Compare at 30 and 60 after the rate fixes |
| The 53-78 ms stall about every 30 s in fight 2 | A hitch, if it's the game's | Not started | Find the cause; another process isn't ruled out |
| The pad, read at 30ths | A press waits up to 33 ms; every tick makes it 17 | Not started | Read it every tick in a hunt. Medium risk |
| The hunter's 45-unit jump, 0 / 2 / 6 30ths after a draw attack at 30 / 60 / 144 | Possible real rate bug; blocks comparing fights | Not started | Explain it |
| Inventory of everything that ticks | Conversion goes class by class (`MHFU_NATIVE_VT`): today only the hunter and Popos | Partly done: 107 classes named of 519 vtables; 17 monster overlays, 11 weapon classes | Table: each monster, NPC, weapon class, attack volume, effect and stage object, its class, and how it's tested |
| A way to test each one without playing | Only `em20` has run in a recording | Not started | Force a monster or weapon by quest swap and save edits; compare ticks at 30 and 60 automatically |
| Monsters at 60 ticks | Less lag, hits line up, sharper arcs; smoothing already handles stepping (`docs/ticks.md`) | Not started | Decide after the fixes below are played. High risk; `em*` overlays need recordings first |
| Effects, particles, camera easing at a higher tick | Visible stutter *(check)* | Not started | Follows from the list above |
| Frame-rate dependence in big-monster fights | The rate checks may not cover real fights *(check)* | Not started | `tools/ratecheck.py` over the fight recordings at 60 |
| Frame time at 144 in the worst scenes | Must stay smooth | Area 9 fix merged, not confirmed by play | Measure stage by stage with `MHFU_PROFILE`; log the worst |

Hand conversion helps where a rate assumption is buried in logic: `09BA2060` shows a gate reads plainly once written by hand.

## 2. Graphics

**Done when:** every option can be changed in-game, known artifacts are fixed, and each effect has a stated frame cost.

| Work | Why | Status | Next step |
|---|---|---|---|
| In-game graphics options | Many settings are env vars or `settings.ini` only | Not started | Inventory them; extend the F1 overlay (`runtime/src/overlay.c`) |
| Ground knowledge for grass | Grass goes by triangles as drawn and whether they're green: it can't tell ground from rock or sky, and must see ground before growing on it | Not started | Survey the stage and collision data for surface types and a whole-map ground. Understand it; hand conversion likely not needed *(check)* |
| Artifact: 2D sprite-like textures, as the sun was, that aren't static | Reported 2026-11-05 | Not started | User's save states show where; compare with the flare fix |
| Artifact: Khezu's colour artifacts towards its tail | Reported 2026-11-05 | Not started | Save state; try with `MHFU_GPU_TL=0` and on PPSSPP, to see if it's ours |
| Artifact: Congalala's mesh seams as it moves, such as the tail | Reported 2026-11-05 | Not started | Same tests, plus `MHFU_NATIVE_SMOOTH=0`: smoothing moves matrices apart *(check)* |
| Artifact: snow haze edges | Older report | Not started *(check)* | Ask if still seen |
| Shadows don't affect gameplay | Makes shadows purely visual | Verified on `auto/shadow-random` | Play; check frozen shadows; merge, with the baselines decision above |
| Wounds: slashes that bleed, run down the model, drip and pool | Requested 2026-11-05: big sharp hits leave a mark that starts to bleed. The game's own blood stays | Not started; starts after the artifacts (user's decision) | See below |
| God rays | Requested | Not started | Design in the renderer, using the existing sun direction; no game code needed *(check)* |
| Further enhancements | Requested | Ongoing | Each one needs a frame cost and an off switch |

**Wounds, in stages, each playable:** (1) one slash at the true hit point, staying on the moving model; (2) blood running down from it under gravity; (3) drops falling; (4) pools on the ground. Purely visual: reads the game's memory, writes none of it, draws no game random numbers, has an off switch and a stated frame cost. Proposed: wounds kept in bone space and drawn in the fragment shader, as UVs may be mirrored or shared *(check)*. Size and sharpness come from `readable/monster_hit_damage.c`; first prove the hit's position and the drawn model can be linked. Cut against impact isn't settled yet (see Balance, goal 4).

Most of this lives in `runtime/src/`. Where a feature needs to know what the game's data means, understanding it comes first; rewriting it by hand is a separate question.

## 3. PC support

**Done when:** a player never sees a PSP-era prompt or limitation, and every menu works with mouse and keyboard or a pad.

| Work | Why | Status | Next step |
|---|---|---|---|
| Controls, rebinding, F1 screen | Basics | Merged; F1 live path untested *(check)* | User tries F1 |
| Button hints in menus' text | Still shows PSP buttons | Partly done: 2 of about 15 sprite drawers | Hook the rest, then font text |
| Native interface: menus and HUD | The main item here | Not started | Restyle everything first: art, fonts, layout, scaling, hints, mouse (user's decision). Then replace only the worst screens, chosen by the user |
| Save handling, resolution, fullscreen, settings | Expected on PC | Partly done *(check)* | Inventory what's missing |

Restyling leaves game logic alone. Replacing a screen is where menu code needs naming and perhaps hand conversion, and where saves and inventories need their own checks.

## 4. Modding

**Done when:** someone can add each kind of thing below with documented tools, without reading generated C.

Ordered roughly easiest first. New monsters and areas depend on most of what comes before them.

| Mod type | What it needs | Status | Next step |
|---|---|---|---|
| Debug overlay: hit zones, attack volumes, monster state, drawn in-game | Makes balance and new attacks far easier, and checks our own findings | Not started | **First modding item.** Purely visual; much of the data is already understood |
| Mod folders with live reload | Change a texture or value and see it at once | Not started | Start with textures and tunable values; design with the tools row below |
| Free camera, photo mode | Inspect models and effects, check visual mods | Not started | Cheap; must leave the game's camera state untouched |
| Textures | Packs, dumping | Works, PPSSPP-compatible. Missing: zip, DDS/KTX2, `[indexed]`, pack mipmaps | Docs and an example workflow |
| Meshes | Model format reader and writer, skinning, a replacement hook | Not started; was deferred | Document the model format |
| Balance: damage, hit zones, monster choices | Readable code and data tables | Code hand-written, verified, not merged | Settle what byte 1 and 2 and the five elements are, with a recording |
| New attacks | Weapon code, motion format, hit records, effects hook | Class 0 and part of class 4 hand-written | Document motion and hit record data; identify the classes |
| New particles | Effect system | Names only | Find and document how effects are spawned and described |
| New monsters | Model, motion, script, hit zones, overlay loading | Interpreter in progress | Script language docs and an assembler; then how a monster type is registered |
| New areas *(maybe)* | Stage format, collision, routes, spawn lists | Not started | Survey only; decide later |
| Extended game memory | Lifts the 32 MB limit on what game logic can hold | Not started | See below |
| Larger hunts, more monsters at once | Object slots, overlay loading | Not started | Survey with extended memory |
| Higher-detail models over the game's skeleton and hitboxes | Visuals change, gameplay doesn't | Not started | Follows meshes |
| Cloth simulation | Requested 2026-11-05. The hunter first: armour and capes moving properly. Visual only | Not started; waits for mesh replacement (user's decision) | See below |
| Visual simulation: debris, fur, footprints, foliage reacting | Read-only, as wounds | Not started | Follows wounds |
| Weather and time of day, volumetric fog, water | Renderer | Not started | Each needs a frame cost and off switch; may move to goal 2 |
| Tools and workflow | A mod folder, loading order, a way to check a mod | Not started | Design after textures and meshes show the shape |

**Extended memory, in steps.** Map a new region above the game's RAM (the runtime reserves 1 GB of address space; RAM is 32 MB at `0x08000000`), and keep the original 32 MB and the game's allocator as they are, so the baselines still hold. (1) Measure how full memory gets in the worst hunts. (2) Add the region, with save states covering it, and allocate from it only for mods. (3) Only for a mod that needs it: move or enlarge the game's own pools. Memory alone doesn't raise fixed object slots, or let code that loads at the same address be present together *(check)*. Most visual content lives on the PC side and needs none of this.

**Cloth, in steps,** on the hunter first, then perhaps banners, fabric and NPCs. It waits for mesh replacement, as PSP models have few vertices to bend. (1) Simulate tagged parts of a replacement mesh in the renderer at the display's rate, pinned to bones, colliding with simple body shapes, writing nothing to game memory. (2) Use the same system on the world's fabric. Separately, under goal 1: check whether the game's own `hunter_spring_chains_step` is right at 60 ticks, as its steps run every frame and may not be scaled *(check)*.

## Replacing PSP emulation with native code

The runtime is our own code, so this isn't rewriting the game. Do each only where a goal needs it, keep an off switch while it's new, and check it as any other change.

| Part | Now | Native would give | Worth it? | Next step |
|---|---|---|---|---|
| Files (`io.c`) | Reads the ISO's files | Mods replace any file by a folder over the ISO | **Yes, first:** cheap, and most modding needs it | Design with mod folders |
| Drawing: GE commands (`ge.c`, `gpu.c`) | Translates the PSP's display lists; GE and render were 57% of samples, game code 2% (`docs/ticks.md`) | Direct paths for chosen objects: new meshes, effects, less work | **Yes, by object type:** replacing it all isn't needed | With mesh replacement |
| Screen layout | Game draws for 480x272 | HUD and menus laid out for the display | Yes, for goal 3 | With the native interface decision |
| Input | Pad state, read at 30ths in a hunt | Read every tick; true actions for mouse and keys | Yes, in goal 1 | Already listed |
| Memory | PSP's 32 MB layout | More room | Partly: see extended memory | Measure first |
| Audio (`audio.c`, `atrac.c`) | Plays the game's formats | New sound formats, more voices | Later, for sound mods | None yet |
| Threads (`thread.c`) | One host thread, deterministic | Little: game code is 2% of samples, and tests rely on the fixed schedule | **No** | |
| CPU, VFPU instructions (`vfpu.c`) | Generated C | Little, as above | **No** | |

## Status file

To build: a script that regenerates, per function, its system, lines, whether it's named or hand-written, and whether it ran in a
recording, with totals per system. Never edited by hand. It holds addresses and names only, no game code.

Current figures, 2026-11-05: 16,966 functions and 1,329,592 generated lines; 3,763 functions (402,362 lines) ran in recordings; 91
hand-written and verified (1.2% of lines), 137 once the current batch passes (1.5%); about 8,050 named (47%).

## Open questions for the user

- New areas: wanted enough to survey now, or later?
