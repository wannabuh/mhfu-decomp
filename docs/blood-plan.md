# Blood on monsters: where it stands and what is next

Handoff written 2026-11-12. Branch `auto/blood-buildup` (not merged; it also carries the audio-lag fix `d910cd0`).
The user plays from `build/mhfu` in this checkout and tests live; they file tickets with F6.

## What the user wants

Their words over the day: monsters "get bloodier over time"; a part hit repeatedly ends up "mostly bloody"; "open to
exaggeration, it doesn't need to be realistic"; never "totally drenched"; they want "all of" Overgrowth's look
(splats at the hit, runs that drip down, a monster that shows a long fight). They rejected blood worked out by a
formula around each wound three times (#81 too little, #82 too much, #83 "looks like flat shading"). Do not go back
to that.

## How it works now

Modelled on Overgrowth's `Source/Graphics/bloodsurface.cpp` (https://github.com/bsmr/WolfireGames---overgrowth).

- **A blood map per monster** (`runtime/src/bloodmap.c`, `bloodmap.h`). The game's own texture coordinates cannot be
  used: on a Tigrex 72-100% of texels are shared by distant skin and a texel is 6-8 units (`MHFU_UV_DUMP`). So each
  kept skin gets a 2048 x 2048 map in which every triangle has its own rectangle (shelf packing, 1.5 texel gutter,
  as fine as 1 unit a texel; a Tigrex lands at 1.17). Four skins at most (`RUN_MESHES`).
- **Storage**: one GPU storage buffer shared with the old trickle pieces (`shared.h`: `RUN_MAPS_AT`, `RUN_TEXELS_AT`).
  Per triangle two rows of four floats map a rest-space position to a texel. A texel is one word: bits 0-7 amount,
  8-15 thickness, 16-23 the second it last got wet (1-255, 0 dry), 24-27 cut, 28-31 bruise.
- **Painting is on the CPU, in 3D**: for each triangle near the hit, each texel's posed world position is evaluated
  against a shape function, so marks continue across triangle edges though the rectangles lie apart. Shapes:
  splash (blot, arms, drops), cut, shot hole, bruise, and a stroke (a run's step).
- **Skins** are the trickle code's (`gpu.c` `trail_models`, `trail_model_keep`, `trail_model_map`): a monster's draws
  found again each recorded frame, each draw told where its triangles' data is (`TlDraw.runs` + `gl_PrimitiveID`).
- **Hits**: queued in `wounds_attach`, painted in `paint_hits` (`gpu.c`). Size by `gain = max(sqrt(damage/60), 0.3)`.
- **Runs**: the trickle walkers (`trails.c`) call `skin->stroke`, which paints into the map (`trail_stroke`).
- **Drawing**: `ge_frag.glsl`, the block beginning "The blood kept on its skin texel by texel": manual bilinear read,
  bruise, stain (skin detail kept), clot, cut on top, gloss by the smooth normal.
- **Uploads**: changed rows only (`bloodmap_sync`), from `runs_sync` in `render_lights.c`, called at draw time too.
- The old per-wound drawing is off while the map is on (`wounds_fill` returns). `MHFU_BLOODMAP=0` brings it back.

Settings (menu and env, all live): `MHFU_BLOOD_AMOUNT`, `MHFU_BLOOD_DARKNESS`, `MHFU_BLOOD_RUN_SPEED`,
`MHFU_BLOOD_DRIP_SPEED`, `MHFU_BLOOD_STREAK`. Debug: `MHFU_BLOODMAP_SHOW=1`, `MHFU_BLOODMAP_LOG=1`,
`MHFU_TRAIL_LOG=1..3`, `MHFU_WOUND_LOG=1`, `MHFU_WOUND_TEST=n,far,high,hard`, `MHFU_TEST_RELAY=n`,
`MHFU_UV_DUMP=file` (`_ALL=1`, `_AT=frame`), `MHFU_BLOOD_OVER=1`. README has each.

## Runs looked "wormy" (#93, fix-ready, awaiting the user's look)

On the Congalala the runs read as worms or veins: one width and darkness all along, up to 380 units long, three to
six per gash plus one or two per repeat hit, a sideways drift, and bending when the monster rolled.

The user's answers (2026-11-12): keep blood visibly running, reshaped; the amount at the hits was **too little**; one
look for fur and hide for now.

Done:

- Shorter and fewer: 20-60 units, about one in four (more from deep wounds) 60-120; one from a cut, two or three
  from a gash; a repeat hit adds one at most, six times in ten, and only if none is waiting (`trail_from_wound`,
  `wounds_frame`, the fed branch in `wounds_attach`).
- Tapered and fading: widest at the wound, a third of that at the end, uneven; less blood further on, dashes in the
  last half, a bead where it stops (`width_at`, `much_at`, `finish_` in `trails.c`; the skin's `stroke` takes `much`).
- Straighter: drift a quarter of what it was; a run stops once its way over the skin (in the vertices' rest space)
  has turned about 70 degrees from the way it set out.
- More at the hit: the splash a third larger and thicker (`paint_hits`).

Seen headless on the Tigrex only (a splash on the cheek with short tapering drips). On the Congalala capture the
monster leaps during the test and the planted hits land under a limb; not judged there. Not done: softer on fur.

## Tickets

Only the user closes tickets; set `fix-ready` with a check line (`python3 tools/tickets.py`).

| # | State | What |
|---|---|---|
| 93 | fix-ready | Runs look wormy. Above. |
| 86 | open | Pale patterned patch in ground blood. Unconfirmed; probably the same cause as #89. |
| 89 | fix-ready | Ground blood drawn twice. Two attempts (`b26410f` layer flag, `c2b8ebb` by the surface's alpha). The first did nothing in the desert. The layer was never identified. If it persists, ask for an F7 click on a ghost blot and an F6 capture. |
| 90 | fix-ready | Blood lost at death, and one-frame flashes. Flash: previous frame's table kept. Death: not reproduced; a model swap now re-keeps the skin and carries blood over by rest-space position. The console prints "kept anew" when it fires. |
| 85 | fix-ready | Blood lost on area change. The clear was removed; untested across areas. |
| 87, 88, 91, 92 | fix-ready | Health-rise drop; snow brightness; whole-map uploads; repeated skin rebuilds. |
| 80-83 | fix-ready | Earlier rounds; superseded by the map but not yet closed by the user. |
| 84 | open | Not blood: the swamp's wet ground is redrawn as dark water. Not started. |
| 21 | open | The umbrella idea ticket. |

## Known gaps

- **Not verified in pictures**: ground blood (it does not draw headless), drops in the air, bruises (no way to plant
  a blunt hit), light and medium hits, shade, the user's texture pack.
- **The test suite has not been run** on this branch since `2c106ae` (`tools/.venv/bin/python tools/suite.py -j 3
  --bin <copy>`). Run it before any merge, when the user is not playing.
- **Frame cost** is unmeasured apart from the game-thread skin step (0.1 ms with two runs). The fragment shader does
  four buffer reads per monster pixel.
- **Memory**: 64 MB of RAM and 64 MB on the card for the four maps. Each triangle wastes about half its rectangle;
  pairing triangles would give finer texels for the same memory.
- **Four skins**: a fifth bloodied monster evicts one (by `touched`, which every skin updates each frame, so in
  effect arbitrary). Should be least recently hit.
- **Model swap carry-over** kept only about 450 texels from 32 bloody triangles in the user's console (#93 capture).
  That was during the #92 misfire, but the yield looks low and is worth checking.
- **Runs advance per game tick**, not per shown frame.
- **Pieces the game culled when a skin was first kept** have no place in the map and never take blood.
- **Furred and layered monsters** (Popo) are untested with the map.
- `trails.c` still holds the old piece storage, unused while the map is on.

## Testing recipe

`docs/blood-tools/capture_run.py` loads a ticket capture's save state headless and writes pictures:

```
SWINGS=20:0 python3 docs/blood-tools/capture_run.py one 25,60 MHFU_WOUND_TEST=5,150,150,120 MHFU_SCALE=4
CAP=~/.local/share/mhfu-recomp/tickets/0087-*/capture-0143 SWINGS=20:0 python3 docs/blood-tools/capture_run.py g 30 MHFU_WOUND_TEST=3,200,120,120
```

Pictures land in `/tmp/blood-runs/run-<name>/build/pictures`. Default capture: ticket 82's (Tigrex, desert), where
`150,150` lands on the left cheek. Ticket 87: a live Giadrome. Ticket 90: a dead Gendrome.

Traps:
- Do not set `MHFU_REPLAY_SPEED=0`: only picture frames render and the blood buffers are never filled.
- Planted hits are cosmetic: no damage, no death. Scripted swings after a state load do not reach the monster.
- zsh does not split `$var`; pass each `VAR=value` as its own argument.
- One run at a time while the user plays; skip long checks.
- `make -j$(nproc)` from the repo root (not `make -C build`).

## Commits on the branch, newest first

`0369773` skin rebuild misfire (#92) · `997c8e5` cuts follow darkness · `1150cb3` row uploads (#91) · `b822abd`
flash fix, model-swap carry-over (#90) · `c2b8ebb`, `b26410f` ground blood on layers (#89) · `ee4a23b` health rise,
area change, snow (#87, #85, #88) · `0a4864a` drops' clock, darkness setting · `5719aa0` streak setting · `e14f679`
streak cap, drop size with distance · `6cae3bc` speed settings · `4e72485` map 2048 · `b4e20ac` cuts and bruises in
the map, darker, faster · `6ce6dc9` runs paint into the map · `a715f5e` the map itself · `dd7b1c0` `MHFU_UV_DUMP` ·
`4f47de3`, `3735bee`, `e811892`, `2c106ae` the formula rounds (superseded) · `d910cd0` audio lag.
