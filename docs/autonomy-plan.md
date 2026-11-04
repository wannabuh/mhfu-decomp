# Plan: improving and testing the game autonomously

The goal: long sessions in which Claude finds, fixes and verifies problems in the PC port without the user at the
game - reaching any situation, acting in it, and telling right from wrong by itself. Written 2026-11-03, at the end
of the session that built `tools/suite.py`; branch `native-fps` at `d7d7fcd`.

## Ground rules (agreed with the user)

- Claude may commit on its own to work branches named `auto/<topic>` during long sessions; the user merges into
  `native-fps`. Never commit to `native-fps` or `main` without being asked. Commit messages end with the attribution
  lines from the session's system prompt.
- While the user is playing, keep the machine light: one headless run at a time (no `-j 4` suites). When they are
  away, run in parallel freely.
- While the user is playing and reporting bugs live, build the fix and hand it over - they test it themselves.
- Never `pkill -x mhfu` (it kills the user's game and other runs): stop runs by their working directory. Runs go in
  the scratchpad, each in its own folder with a `game` symlink and a copied save (`tests/data/saves/kit`).
- Use `timeout -s KILL` on headless runs: the game can ignore SIGTERM.

## Where things stand

- `tools/suite.py` + `tests/scenarios.toml` (21 scenarios, recordings and saves in `tests/data/`, not in git):
  30-vs-60 comparison of the hunter (`tools/ratecheck.py`), RAM-hash baselines, a save-state round trip.
  `docs/testing.md` explains it. All pass at `d7d7fcd`.
- Replays take `poke` lines (game memory written at a frame; `hunter+<off>` for the hunter) - also recorded when
  the runtime writes memory for an input (`present_poke_hunter`, the item bar's chord).
- Save states (`runtime/src/savestate.c`, F2/F4, `MHFU_STATE_LOAD=frame,slot`, `MHFU_STATE_NOTE=frame,file` dumps
  RAM) load in later builds on `auto/portable-states` (Phase 1). A state saved at 60 and loaded at 30 runs at half speed - compare rates with replays.
- `tools/watch.py ADDR --frame N -- ENV...` (gdb watchpoint: who writes a PSP address, with the game call chain)
  works with a state load in the environment: the fastest way to find the code behind a field.
- Quest swap: `poke 1355-1366 9B19F9C h N` on `quest.rec` loads quest N (monster quests 1, 2, 5, 8, 9);
  `tests/data/rec/quest-q*.rec` end at the camp. Getting to a monster is not automated.
- Known structures: the hunter (vtable `0x089BB3CC`, `rt_local_player`; fields in `docs/testing.md` and memory
  `test-suite`). Objects: the manager `LW(0x08A62D7C)`, list N's head at `mgr + 0x80028 + 4*N`, next at `+0x1C4`
  (list 2 players, list 3 monsters; ticked by `EBOOT 088666B4`). Monsters: the monster manager (`game_task 09AB1498`)
  keeps 20 slots at `+0x1220`; an object is deleted when bit 0 of `+4` clears; `+0x1D4` its life state (0 alive ..
  3), `+0x414` its action count, `+0x3F4` a count in 60ths. Small monsters share a vtable layout (Giaprey
  `0x089BC77C`). Not yet known: the current area, area exits, monster health and position offsets.

## Phase 1 - save states that survive rebuilds

The highest-leverage piece: with it the user records a library of situations once, and every later build uses it.

Built on `auto/portable-states` (the user's OK 2026-11-04; `docs/runtime.md` describes it). The obstacle is the threads' host stacks, whose return addresses
point into the executable, so names alone cannot make a state portable. A waiting thread is instead kept as its `Ctx`,
its wait record and its post-wait step, and resumed by PSP return address: `recomp.py` labels each call's return point
and adds an entry check to each generated function, and on load a loop enters the function holding `pc` and continues
at `c->r[31]`.

Agreed with the readable-code session (branch `auto/readable-code`, also from `d7d7fcd`, not merged):

- `build/gen` stays what ships. `readable/*.c` hand functions are reached through their generated function
  (`if (rt_hand) ...`); symbols and `tables.c` do not change, so every PSP address still resolves.
- Hand functions hold C locals and have no return points. So: `rt_no_resume` counts frames holding C state (hand
  functions, gates, `run_callbacks`) on any thread's stack, and a save waits for a state point where it is 0. No fatal
  error at waits: such a frame above a wait is harmless unless a save is asked for then.
- Top of a generated function: the resume check first, then the `rt_hand` line, so a resume lands in the generated body.
- Hand code keeps no state of its own outside game memory unless it is `SAVED`.
- `savestate.c`, `thread.c` and the resume runtime are this phase's; the shape of hand code is the readable session's;
  changes to what `recomp.py` emits are agreed first.
- Found: game RAM differs by renderer because `rt_cull_margin` (1200 with Vulkan's shadows, else 0) widens the game's
  view test. Models just out of view get drawn and animated, and effects on them draw from the game's random stream 1,
  which gameplay also uses (`overnight-fight1.rec`: it parts at frame 1937). So a recording made with shadows does not
  replay the same random numbers on soft or with shadows off. Decision open: `docs/testing.md`.

Status: done - new format, old one removed. On Vulkan, within replays (frames with input; the game plays on alone after
a replay's end, so those frames prove little): the same as the old build with no state (`overnight-fight1.rec`, all 5931
frames); seven states loaded by another build in a new process, 14,626 frames, none differing; `MHFU_STATE_TEST` round
trips. Speed: 0.2-0.35% more instructions over whole replays, no measurable change in cycles; the program 19% larger.
To do: a cross-build check in `tools/suite.py` (it must load in a new process: a load in the
same process cannot show state only that process holds, which is how the sun was missed).

1. Split what a state holds into the game's side and the runtime's side. Audit every `SAVED` variable
   (`grep -rn SAVED runtime/src`) and the registered spans (`savestate.c`): the game's RAM, the HLE kernel's state
   (threads, semaphores, files, callbacks, the GE's registers and lists, audio channels), the input/replay position,
   versus caches the runtime can rebuild (renderer, interpolation history, HUD tags, native smoothing, texture
   caches).
2. A new state format: the game's side only, stored by name (each saved variable keyed by a stable name, not an
   address; pointers into the runtime's own memory rebuilt or forbidden), with a format version instead of the
   executable's hash. After a load, reset the runtime's caches as a fresh boot would.
3. Verify hard: `MHFU_STATE_TEST` round trips, and a cross-build test - save with one build, change unrelated code,
   rebuild, load, compare RAM and the hunter's trace frame by frame against the original build continuing.
4. The old fast path was removed at the user's choice: one format, so that every F2 and F4 tests it.

## Phase 2 - crash and hang capture

On a fatal error (`rt_bad_jump`, "no function there", unreachable, a signal) or a hang (the game frame not advancing
for N seconds): write a portable state from a few seconds before (keep a ring of in-memory snapshots every ~2 s), the
recent input as a recording, the game call chain, and the log, into `crashes/<date>/`. A crash then reproduces
headless with one command, and becomes a test case.

Also audit the 215 unresolved `jr`s in `game/analysis/functions.json` (`tools/funcs.py`): each one that is really a
jump table is a future crash like `09BABA40`'s (fixed in `2f3d56d`). Classify them (vtable calls vs tables) with
stricter checks; resolve the tables.

## Phase 3 - a script interface

Drive the game from Python, frame by frame, headless and deterministic:

- `MHFU_SCRIPT=<socket or pipe>`: per game frame the runtime answers requests - read/write memory, set buttons and
  stick (and mouse camera), step N frames, save/load a portable state, dump a frame. A small Python library
  (`tools/mhfu_script.py`) wraps it.
- Behaviours on top: face a point, walk to a point, attack (weapon-specific chords), dodge, use an item, wait for a
  state. Inputs a script sends are recorded like a player's, so a scripted run is also a replay.
- Map the structures it needs (with `watch.py` and RAM diffs): monster position/health/state/target, the current
  area and the area-change trigger, area exits per map (the user walks each map's exits once while recording; the
  points are kept in a table).

## Phase 4 - the state library (the user's long session)

With portable states: the user plays through and saves states (F2 into named slots, or a "save with a label" key) -
every monster at the start of a fight, each weapon class, each area by day and night, status effects about to land
(paralysis, poison, sleep, stun), gathering and carving points, the supply box, menus. Kept in `tests/data/states/`
with a manifest (`tests/states.toml`: label, map, area, what is in it). Scenarios start from a state instead of a
recording from boot.

## Phase 5 - checks that need no one

- 30 vs 60 for every object, not only the hunter: trace each object's fields (generalise `MHFU_NATIVE_TRACE` /
  `ratecheck.compare`), flag fields that count game frames. This finds bugs like paralysis wearing off fast, and is
  the tool for converting monsters to tick every frame.
- Picture checks with the Vulkan renderer: frame baselines for chosen scenes, detectors for flicker (a frame unlike
  both neighbours), black or blown-out frames, HUD pieces out of place.
- Performance budgets: `MHFU_FPS_STATS` from fixed states (night areas), tracked per commit.

## Phase 6 - the loop

- A job runner (`tools/run_jobs.py`): queue of scenario/state runs, parallel, faster than real time, results to
  `tests/data/runs/`; a nightly-style full run.
- Session routine: pick a known issue or a failing check, reproduce it from a state, fix, run the affected checks and
  the suite, commit to `auto/<topic>`, write a short report (`tests/data/runs/<date>/session.md`) for the user.
  Scheduled wake-ups keep a long session going while the user is away.

## Later - PPSSPP as the reference

Play the same input in PPSSPP (headless, already used by `tools/difftest.py`) and compare game RAM: catches what is
wrong the same way at 30 and 60 (recompiler, HLE). Needs input injection into PPSSPP and tolerance for its timing.
Worth it once the rest is running.

## Open issues to feed the loop

- Paralysis (and likely other status effects) wears off too fast at 60 - needs a state while paralyzed.
- Area 9 (Old Jungle) slow at night - needs a state there.
- Button prompt near the supply box / interactable things - needs the "can interact" flag (states at and near the box).
- Particle shadows at the jungle camp.
- The flicker at screen changes with no `fps` set (only seen in dumps).
- Corpse time: the Giaprey's ~10 s looked like the game's own 300-30ths count, unconfirmed (a state before the kill).
- Monsters still tick at 30 (smoothed between): converting them is the big remaining 60 fps job (Phase 5's tracing).
