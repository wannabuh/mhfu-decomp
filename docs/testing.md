# The test suite

`tools/suite.py` plays the scenarios of `tests/scenarios.toml` headless and checks them. A scenario is a recording
from the game's start (`MHFU_REPLAY`), cut and extended as it says: input lines, and pokes - the game's memory written
at a frame. The recordings, the saves they were made with, the baselines and the runs are in `tests/data/` (not in
the repository: the game's and the player's).

    tools/suite.py              # every scenario, 4 at once (each runs two or three games)
    tools/suite.py --only sprint,climb    # by name or tag
    tools/suite.py --bless      # this run's results kept as the baselines
    tools/suite.py --keep       # the traces kept (tests/data/runs/<date>/<scenario>/r30/trace ...)
    tools/suite.py --list

It exits 1 if anything failed or changed, and writes `tests/data/runs/<date>/report.md` (and `report.json`). A full
run of the 21 scenarios takes about 40 minutes at -j 4.

## Checks

- **rate** - the scenario played at 30 and at 60 (converted: `tools/ratecheck.py`'s `convert`; a scenario made at 30
  plays both runs at 30 up to its `switch` frame) and compared action by action (`ratecheck.compare`): an action
  lasting longer at 60, a field of the hunter counting game frames (stepping twice as often a 30th), ground covered.
  Compared up to where the runs part (the hunters apart for 2 seconds): from there the input does other things in each
  - the mouse's moves converted between rates steer them apart in the end (see "Parting" below). `allow` in a
  scenario lists findings known and accepted (text they contain).
- **baseline** - the game's RAM hashed every 10 seconds of game time (`MHFU_RAM_HASH=N`: from `0x08800000`, the
  game's memory) and the hunter's states in order with their lengths, at 30 and at 60, against those kept in
  `tests/data/baselines/`: where a run first changed - a hash, then the hunter's first state that differs. A change
  meant (a fix that changes what the game does at 60, say) is kept with `--bless`. Runs are the same every time: the
  game's clock is virtual and the replay its only input.
- **state** - a save state saved and loaded at 60 (`MHFU_STATE_TEST=A,B`, default the end's last 400 to 100 frames):
  every frame after the load as before (savestate.c; the test prints the first words that differ).

What is after a replay's end (the game playing on alone until the run is stopped) is left out of all of them.

## Coverage

The report lists the hunter's states seen (`+0x299`, the state `game_task 09A9A9E0` switches on: below 0x80 the
common ones, 123 handlers - those never seen are listed with their handler) and actions (`+0x684`: the weapons' moves
have their own numbers), by weapon class (`+0x1E8`). The recordings so far are of one weapon (class 4).

## Writing a scenario

```toml
[[scenario]]
name = "out-of-breath"
rec = "popo.rec"            # tests/data/rec
cut = 3574                  # its input after this frame (and its end) dropped
switch = 3574               # the run at 60: at 30 up to here
tags = ["field", "stamina"]
lines = """
3576 0 128 128 0 0
poke 3590 hunter+55A h 40
3600 200 128 0 0 0
3700 0 128 128 0 0
end 4100
"""
```

Input lines: `<frame> <buttons hex> <stick x> <stick y> <mouse x> <mouse y>`, each held until the next (0x200 R,
0x100 L, 0x1000 triangle, 0x2000 circle, 0x4000 cross, 0x8000 square, 0x8 start, 0x1 select; the stick 128 centred, y
0 forward). After the end the game plays on with nothing pressed (and at real speed: keep the `end` past what is
looked at).

Pokes (`runtime/src/present.c`): `poke <frame>[-<last>] <where> <b|h|w|f> <value>` - an address (hex) or
`hunter+<offset>` (the hunt's hunter, `native.c`'s `rt_local_player`, when there is one), written as each frame from
the first to the last starts. Never recorded. Hunter fields known: `+0x55A` stamina (h, 300 full), `+0x62E` its most,
`+0x41C` health (h, 100; `+0x41E` the part that comes back, `+0x60C` 150 - not yet checked against a hit), `+0x684`
action, `+0x299` state, `+0x1E8` weapon class, `+0x200` place (3 floats), `+0x1F4` facing.

## Another quest: the elder's board

The village elder's quest table (`quest.rec` at about frame 1360, the kit's saves) is loaded with its quests' summaries
(each: time limit, quest number, stars, map, the goal: monster or item and how many): numbers 1-10 and 10001-10005 -
among them big monsters. The elder offers the first of the list for the level chosen (the object at `0x09B19F1C`:
`+0x58` the quest chosen, `+0x80` the level's list of numbers, 0xA0 bytes a level; `lobby_task 09A87D50` picks it
and copies the quest to `0x09B04040`). A poke of the list's first number before the pick makes the elder offer that
quest, and the game loads it whole - its map, its monsters:

    poke 1355-1366 9B19F9C h 8       # quest.rec: "The Land Shark" (Cephadrome, desert) instead of Mountain Herb Picking

| quest | | map | camp, then the stick |
|---|---|---|---|
| 1 | A True Foe - The Giadrome! (Giadrome) | snowy mountains | |
| 2 | Reckless Bulldrome Hunter (Bulldrome) | 0x15: a grassy camp, night | |
| 3 | A Pack of Blangos (15 Blangos) | snowy mountains | |
| 4 | Mountain Herb Picking (deliver 20) | snowy mountains | |
| 5 | Rarest of the Rare Beasts (Congalala) | 0x14: jungle, the beach camp | left: area 5 |
| 6 | Hunt the Rare Forest Congas! (15) | 0x06 | |
| 7 | A Mushroom Goldrush! | 0x06 | |
| 8 | The Land Shark (Cephadrome) | 0x11: desert, night | left (`0 128` from 2300 to ~2600): area 3 |
| 9 | The Giant Enemy Crab (Daimyo Hermitaur) | 0x03: desert | left: area 3 |
| 10 | The Lady Gourmet (deliver) | 0x03 | |

`tests/data/rec/quest-q<N>.rec` are `quest.rec` with that poke, ending in the quest's camp (frame 2300). To record a
scenario from one - play on from its end, the walk to the monster, the fight - replay it while recording (the pokes
are carried over into the new recording):

    MHFU_TICK_HZ=30 MHFU_REPLAY=tests/data/rec/quest-q5.rec MHFU_RECORD=tests/data/rec/congalala.rec \
        MHFU_SAVEDIR=<a copy of tests/data/saves/kit> build/mhfu

(at 30: the recording it extends was made at 30, and settings.ini's tick_hz would play it otherwise), and add it to
`tests/scenarios.toml` with a `switch` after 2300 (the run at 60 is then converted from there).

The monsters are not near the camp: getting to one is a walk through the map's areas (not yet in a scenario). Where a
quest keeps its large monsters' areas was not found: they are made only when the hunter comes into their area.

## Findings so far (2026-11-02)

- **stamina** (fixed): the flee from a large monster (`09A9E690`) and others used stamina every game frame -
  `patches.py` STAMINA; scenarios `sprint`, `out-of-breath`.
- **the hunter's tint** (`shadow`, `+0x2AC`/`+0x2B0`): eased toward the place's light a step a game frame - twice as
  fast at 60. Only seen; allowed.
- **Parting**: recordings made at 60 converted to 30 (and back) differ a little in the mouse's turns: after a long
  sprint the hunter is some 30 units off, enough to catch a ledge in one run and not the other (`snow` at 30th 2925).
  Compared up to there only.
- **holding L** (fixed): the count of how long L is held (`+0x61C`, the item bar's choice opens at 10) went by game
  frames - the choice opened in half the time at 60. `patches.py` L_HELD; scenario `items`.
- **a run's start**: short runs from standing (12-39 30ths) cover 5-8% more ground at 60 (long ones the same) - the
  start of a run a little quicker. Not yet looked into. Enough to move where a sword meets a Popo: `popo-hits` parts
  there (at 30 the first hit leaves the hunter 1.5 s in action `0x57C`, the next presses taken by nothing - the same
  in a build of 2026-10-25: the game's own; at 60 the combination goes on).
- **camp**: a stop after a sprint ends half a 30th apart, the next stick press on either side of it - `until`.
- **state** (fixed in the test): after a load the renderer's scratch below the game's memory (`gpu.c`
  SCRATCH_VERTICES, `0x08000000`) differed for a few frames - not the game's state; left out.
