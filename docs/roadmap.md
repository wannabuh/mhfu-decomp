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
| What still ticks at 30 | Tells us what's left | In progress | List every remaining gate and the systems behind it: this list sets the rest of this table |
| Inventory of everything that ticks | Conversion goes class by class (`MHFU_NATIVE_VT`): today only the hunter and Popos | Partly done: 107 classes named of 519 vtables; 17 monster overlays, 11 weapon classes | Table: each monster, NPC, weapon class, attack volume, effect and stage object, its class, and how it's tested |
| A way to test each one without playing | Only `em20` has run in a recording | Not started | Force a monster or weapon by quest swap and save edits; compare ticks at 30 and 60 automatically |
| Monsters at a higher tick | Biggest visible part left *(check)* | Not started | Read how much the script and tick code assume 30ths; hand-written interpreter helps here |
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
| Known artifacts | User-visible | Sun flare merged, not confirmed by play | Build a list from the user; snow haze edges are still open *(check)* |
| Shadows don't affect gameplay | Makes shadows purely visual | Verified on `auto/shadow-random` | Play; check frozen shadows; merge, with the baselines decision above |
| God rays | Requested | Not started | Design in the renderer, using the existing sun direction; no game code needed *(check)* |
| Further enhancements | Requested | Ongoing | Each one needs a frame cost and an off switch |

Most of this lives in `runtime/src/`. Where a feature needs to know what the game's data means, understanding it comes first; rewriting it by hand is a separate question.

## 3. PC support

**Done when:** a player never sees a PSP-era prompt or limitation, and every menu works with mouse and keyboard or a pad.

| Work | Why | Status | Next step |
|---|---|---|---|
| Controls, rebinding, F1 screen | Basics | Merged; F1 live path untested *(check)* | User tries F1 |
| Button hints in menus' text | Still shows PSP buttons | Partly done: 2 of about 15 sprite drawers | Hook the rest, then font text |
| Native interface: menus and HUD | The main item here | Not started | Decide scope with the user: restyle, resize, or replace. Then convert the menu code it needs |
| Save handling, resolution, fullscreen, settings | Expected on PC | Partly done *(check)* | Inventory what's missing |

Menu and HUD code would be the next hand conversion after the framerate work. It's mostly unnamed so far.

## 4. Modding

**Done when:** someone can add each kind of thing below with documented tools, without reading generated C.

Ordered roughly easiest first. New monsters and areas depend on most of what comes before them.

| Mod type | What it needs | Status | Next step |
|---|---|---|---|
| Textures | Packs, dumping | Works, PPSSPP-compatible. Missing: zip, DDS/KTX2, `[indexed]`, pack mipmaps | Docs and an example workflow |
| Meshes | Model format reader and writer, skinning, a replacement hook | Not started; was deferred | Document the model format |
| Balance: damage, hit zones, monster choices | Readable code and data tables | Code hand-written, verified, not merged | Settle what byte 1 and 2 and the five elements are, with a recording |
| New attacks | Weapon code, motion format, hit records, effects hook | Class 0 and part of class 4 hand-written | Document motion and hit record data; identify the classes |
| New particles | Effect system | Names only | Find and document how effects are spawned and described |
| New monsters | Model, motion, script, hit zones, overlay loading | Interpreter in progress | Script language docs and an assembler; then how a monster type is registered |
| New areas *(maybe)* | Stage format, collision, routes, spawn lists | Not started | Survey only; decide later |
| Tools and workflow | A mod folder, loading order, a way to check a mod | Not started | Design after textures and meshes show the shape |

## Status file

To build: a script that regenerates, per function, its system, lines, whether it's named or hand-written, and whether it ran in a
recording, with totals per system. Never edited by hand. It holds addresses and names only, no game code.

Current figures, 2026-11-05: 16,966 functions and 1,329,592 generated lines; 3,763 functions (402,362 lines) ran in recordings; 91
hand-written and verified (1.2% of lines), 137 once the current batch passes (1.5%); about 8,050 named (47%).

## Open questions for the user

- Native interface: restyle the existing menus, or replace them?
- New areas: wanted enough to survey now, or later?
- Which artifacts bother you most right now?
