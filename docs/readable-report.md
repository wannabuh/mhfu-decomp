# The readable code: report of the first run

Run overnight 2026-11-03 to 04, on `auto/readable-code`. How to use all this: `docs/readable.md`.

## What there is now

- **The view** (`build/read/`, regenerated from your ISO by `tools/readable/all.sh` in about 10 minutes): all 16,966 functions decompiled,
  no bad-instruction markers, about 814,000 lines, 4,999 gotos. Before: register-level C named by address.
- **Names:** 5,878 entries, 272 of them imports. Every one of the 3,763 functions seen running has a name,
  directly or through identical code. 670 `confirmed`, 5,069 `inferred`, 139 `guess`; 47 honestly `unknown_`.
- **Evidence:** `tools/whatran.py` over 22 traced recordings: what ran in which frames, and who called it.
- **Vtables:** 519, read from RAM snapshots: they are filled in by code, so the game's files don't hold them.
- **Types:** `Object` and `Hunter` only, about 40 fields, in a header with offsets checked at compile time. Only a few dozen functions are
  typed, so most of the view still reads `param_1[0x185]`. This is the biggest gap.
- **Names in the generated C**, as comments, and `build/gen/names.h` for hand-written code.

## The game is unchanged

- `.text`, `.rodata` and `.data` of the built game are byte-identical to `d7d7fcd`'s.
- `tools/suite.py`: all 21 scenarios pass (rate, baseline and state checks). Run on the named build before the last fix, which changed only
  comments in the generated C; the byte comparison above was repeated after it.
- Recordings replay the same under the tracing build: RAM hashes match the normal build's.

## How far to trust it

**The code.** Four audits against the generated C. Neutral samples:

| Audit | After | Faithful | Minor | Misleading |
|---|---|---|---|---|
| 2 | Signatures from the decompiler, repeated | 9 | 6 | 5 of 20 |
| 3 | Signatures from machine-code liveness | 16 | 2 | 2 of 20 |
| 4 | Audit 3's fixes | 22 | 2 | 1 of 25 |

A small sample: 1 of 25 has a 95% upper bound of about 20%. "Misleading" in audit 4 doesn't count quirks a reader can recognise from the guide,
so the numbers aren't strictly comparable. Audit 4 ran before the names fix below, which doesn't touch code, and has not been repeated.
Jump tables, VFPU operand order, branches and offsets have held up in every audit.

**Names.** Tested three ways:

- Blind, before any naming: 21 functions our notes document, hidden from the namer. 19 clearly right, 2 partly, none confidently wrong (by my
  scoring against the notes).
- A skeptic per batch, 26-47 entries each, 368 in all: 319 right, 40 overclaimed, 9 wrong (corrections applied, but only to what was sampled). The errors were mostly game words spread from one caller, and details in the prose, rather than misread code.
- Audit 4, neutral: 21 right, 3 overclaimed, 1 wrong of 25, before corrections.

Read that as: mechanics are reliable; a game word in an `inferred` name is a lead. **Unsampled names keep their errors:** about one in eight sampled
names overclaimed or was wrong, and the skeptics picked the strongest claims, so this isn't a neutral rate.

**Carried names** (2,263, mostly stages) are the least checked. The first version wrongly carried names across same-shaped code that called
different things (at least 4%). It was fixed to require the same callees, but the fix has not been audited, and same code reading different tables
can still be wrong. Their evidence says `same code as ...`, so they can be told apart.

**Can it find things?** A tester with only the view and `whatran.py`, kept from our notes, found:

- The sprint (`09A9E288`), its stamina cost (`9A6A480`) and recovery (`09A68FC8`) in about 6 searches: all match functions we fixed by hand.
- The ledge grab (`09A9D0B0`), but that name was seeded from our notes, so that proves less.
- Hits to health, the quest menu and movement: plausible answers I did not verify independently.
- No sun or flare code: see below.

Its main complaint: a call like `9A67878(param_1,0)` can drop the state number, so "what enters state X" can't always be answered.

## What the night found out about the game

- **Our notes had the hunter's health wrong:** `+0x41C` is the red part's limit and `+0x41E` the maximum (food + skill, capped at 150); current health
  looks to be `+0x2E4`. `docs/testing.md` still has the old reading, and I left it (it's on your branch's docs): these are `inferred`.
- `+0x684`, which `tools/ratecheck.py` and the docs call the action, is a copy of the motion id `+0x324`.
- Object list 3 isn't only monsters: it holds about 0x5A entity types.
- **Open: the renderers differ.** Game RAM is the same under the soft and Vulkan renderers at frame 900 of `overnight-shops`, and differs by frame 1200, in about
  18,800 words, including what look like float arrays at `090C5E80` and `08A933A8`. Each renderer repeats itself exactly. `tools/suite.py` runs on soft and you
  play on Vulkan, so the suite may be testing something slightly different from what you play. I don't know the cause or whether it matters.

## What's left

- **Coverage:** 13,203 functions never ran in a recording. Most have no name: other weapons, other monsters (only `em20` was reached among monster overlays with
  traced code), and modes never played. Each new recording makes more of it nameable.
- **Types:** classes beyond `Hunter`, `this` on more methods, and fields. The most valuable next step for readability: the 519 vtables give the classes.
- **Indirect calls** through a global can still lose arguments.
- **Compiling readable code, system by system:** not started, as planned. I'd start with the hunter's sprint and stamina: small, understood, and well tested by the suite.
- **The rate:** naming cost about a million tokens per batch of 9 bundles, some 11 million in all for about 3,300 functions. The rest of the game at this
  rate would be something like 30 million or more, so naming follows need better than covering everything.

## Your other reports

- **Area 9 slowdown:** `overnight-fight2` reaches it near the end, so it can be reproduced headless now. Not investigated.
- **Sun-like textures:** not investigated. The tester found no draw code for a sun or flare by name, so names alone won't find them; they're more likely to be found from the
  draw side (the renderer's texture dump). `overnight-misc` shows them, you said, and a frame number would speed that up.

## Files outside the repository

`~/projects/mhfu-ghidra`, 2.4 GB: Ghidra with the Allegrex module, `work/` (traces, audits, signatures, game bytes), `tracebuild/` and `basebuild/` (git worktrees, `basebuild` can go).
`tests/data/saves/overnight` and `tests/data/rec/overnight-*.rec` hold your snapshot and recordings. `.claude/settings.local.json` is the run's allowlist: delete it to undo.
