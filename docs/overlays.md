# Overlays

298 overlays (see [plan.md](plan.md#findings-so-far) for where they are and how they load). This is how code moves
between them and the main program, and what that means for the recompiler and the runtime.

## Regions and what is resident together

| Region | Base         | Overlays                         |
|--------|--------------|----------------------------------|
| 0      | `0x08804000` | the EBOOT (always resident)      |
| 1      | `0x09A5F300` | 12 tasks (game modes)            |
| 2      | `0x09C19100` | `game_sub`, `demo_sub`           |
| 3      | `0x09D1A280` | 17 monsters (`em*`)              |
| 4      | `0x09D63100` | 267 stages                       |

The loader `0x088901F8(ctx, file, region)` is called only from the EBOOT, with constant file indices for tasks (63-74,
region 1) and subs (75-76, region 2); monsters and stages are loaded with computed indices.

## Calls across regions (direct `jal`s, counted statically)

A call into a region other than the caller's own is only valid while a particular overlay is resident there. Nearly
all of them resolve to exactly one overlay that has a function at the target, which shows what is loaded together:

| Caller        | Calls into                                                           |
|---------------|----------------------------------------------------------------------|
| EBOOT         | `game_task`, `lobby_task` (a few: `download_task`, `arcade_task`); `game_sub` (a few: `demo_sub`); one each to `em83`, `stage210` |
| `game_task`   | `game_sub`; **specific monsters** - see below                         |
| `lobby_task`  | `game_sub` (so `game_sub` is resident in the lobby as well as in a hunt) |
| `game_sub`    | `game_task`, `lobby_task`; specific monsters                         |
| monsters      | `game_task`, `game_sub` (13353 and 1580 calls)                       |
| stages        | the EBOOT and themselves only                                        |

**Per-monster code in `game_task`** (around `0x09B633F0`-`0x09B66800`): small groups of calls, each into one particular
monster's functions (em07's, em75's, em02's...) - glue that only runs when that monster is loaded. One target,
`0x09D1A300` (+0x80 into the region), is a function in 14 of the 17 monsters: a common entry point, dispatched by
whichever monster is resident. Some of these targets match no monster's function start by the simple tests used here
(stack-frame prologue, follows a `jr $ra`, called from within its own overlay), so static resolution cannot be relied
on.

Indirect calls (`jalr`) are common - 1497 in `game_task`, 2319 across the monsters, 1163 across the stages - and the
EBOOT holds function pointers into the stage region (e.g. the table at `0x089CBAE0`).

## Design

- **Every call or jump into an overlay region dispatches at runtime**: region -> resident overlay -> that overlay's
  function at the address. One rule covers direct calls into other regions, the per-monster calls, the shared monster
  entry and every function pointer. The runtime tracks the resident overlay per region by replacing the loader
  (`0x088901F8`) - the same state the game keeps itself. A call to an address with no function in the resident
  overlay is a hard error naming both, which feeds back into function discovery.
- Calls within an overlay's own region go directly to its own functions (they cannot refer to anything else).
- Statically resolved calls (one candidate overlay) can be direct calls later, as an optimisation, guarded by a check of
  the resident overlay in debug builds.
- **Function discovery** for an overlay includes the targets of calls into it from every other module (and pointers to
  it in their data), not just its own calls.
- Overlays are only loaded from EBOOT code, but the stack may hold frames of an overlay being replaced (if, say, a task
  asks for the next task from its own code). The runtime should check that a region being reloaded has no live frames
  on the call stack, rather than assume it.
