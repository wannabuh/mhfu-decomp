# Reading the game's code

The game's own code is generated from your copy of it: register-level C named by address (`build/gen/`), which is what runs.
This is a second, readable output of the same code, and a database of what we have worked out about it.

## The view

    tools/.venv/bin/python tools/readable/prepare.py ~/projects/mhfu-ghidra/work   # once: inputs from game/
    tools/readable/all.sh                                                         # about 10 minutes: build/read/

`build/read/<module>/<ADDRESS>.c` is one function, decompiled by Ghidra with the Allegrex module. Its first lines give the
address, the generated function it matches (`f_<module>_<ADDRESS>`, which is what patches and the build use) and, once named,
the confidence and purpose. It comes from your game: it stays in `build/`, and is not compiled.

Ghidra lives outside the repository (`GHIDRA`, default `~/projects/mhfu-ghidra/ghidra_12.2_PUBLIC`, with `ghidra-allegrex` in its
`Ghidra/Extensions`). So does the work directory (`WORK`), which also holds game bytes.

## Finding what does what

    tools/whatran.py during fight1 --from 3000 --to 3100 --minus shops    # what ran in these frames and not in shops
    tools/whatran.py during fight1 --from 3000 --to 3100 --tree           # the same as a call tree
    tools/whatran.py where 09A9E690                                       # where a function ran, and its callers
    tools/watch.py ADDR --frame N -- ENV...                               # what writes an address

Traces come from a build that logs every function (`RECOMP_TRACE_ALL=1`, `make TRACE_HOOK=tools/readable/trace_hook.c`, kept in a
worktree of its own) replaying recordings (`tools/readable/trace.py`). A new recording of something not yet covered adds evidence
for it. Traces show what ran, so "in traces" is never "only". A caller's module can be mislabelled where overlays share addresses
(`whatran.py` picks one running in that span): confirm a surprising caller by searching `build/read`.

## Names and types

`tools/readable/names/*.toml`: function, address, name, purpose, confidence, evidence. Addresses and notes only.

- `confirmed`: settled by a trace, a watchpoint, a patch, a string, or a short whole-body read. For a name about mechanics, that
  is strong; a game word in it is only as good as what it rests on.
- `inferred`: follows from the code and callers. Treat it as a lead: check it before relying on it.
- `guess`: a plausible reading.

`tools/readable/types.toml` holds structures and `fields/` their fields; `gen_types.py` makes `build/read/mhfu_types.h` and, for the build,
`build/gen/mhfu_types.h`, offsets checked at compile time. To add either, edit the file and run `tools/readable/all.sh`.

## Classes and fields

`tools/readable/classes.py WORK TRACES SNAPSHOT...` works out the game's classes: what each function ran on (trace build, `*.args`), vtable
slots, callers that all pass the same object, inheritance from constructors, fields from every load and store, pointer fields and globals
from RAM snapshots. A vtable's entries outlive the overlay that filled them, so a slot in another overlay's region isn't counted. `[[global]]`
in `types.toml` declares a structure with no vtable reached through a global (`g_game`, `g_camera`...).

Fields are named in `tools/readable/fields/<Struct>.toml`. `decimal = true` shows a quantity in decimal in the view (`self->stamina < 76`, by
`decimals.py`); ids and flags stay hex, to match notes and function names. `tools/readable/fieldlist.py` lists what the code does with each
field, for naming them; its lists miss accesses through untyped pointers, so "no reader there" proves nothing.

## Rewriting a function by hand

`readable/*.c` holds game code rewritten readably. A function listed in `recomp/patches.py` HAND runs that instead, with the generated code
still behind it: `MHFU_HAND=0` runs the generated code in the same build, so it can be compared and is one setting away.

Write from the generated C (`build/gen/`), with the view as a reading aid: it has hidden arguments before. Keep the order of calls and writes,
the signedness of comparisons, and call the same addresses, thunks included. Nothing has to be kept in a dead register or on the game's stack.
No `static` state unless `SAVED`: a save state wouldn't hold it. Patches in the original (`rt_tick30`, `rt_time_scale`) become plain code.
No thread may block beneath a hand-written function if save states are to survive rebuilds (the save-states work's contract).

To trust one, all of these, as a first-try pass means little by itself:

1. `build/handtest`, in a worktree generated with `RECOMP_TRACE_ALL=1` (not one you play), `stubs WORK/signatures.tsv`: every callee is a stub
   that logs its arguments and returns the next random result, the same for both versions, and may change fields; RAM and the call sequence are
   compared. Add a row to `cases` and the function's inputs to `randomise()`. Also without `stubs`, callees then running for real.
2. Coverage (`HAND_FLAGS="-fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation"`, `HAND_LDFLAGS=-fprofile-instr-generate`,
   `LLVM_PROFILE_FILE=dir/%p-%c.profraw`, then `llvm-cov show ... --show-branches=count`): every branch both ways, or a reason it can't be.
3. Bugs put in on purpose, one at a time. Each must be caught or shown to change nothing. Random inputs without stubs let real bugs past.
4. `tools/suite.py`, and `tools/readable/trace.py` over the recordings with `TRACE_BIN=build/mhfu`: RAM hashes against the earlier runs.
   Compare only the hashes both runs have: when a replay stops can add one at the end.
5. Someone playing it: replays can't tell how it feels.

## What the view gets wrong

It is a reading aid: when it matters, check the generated C. Known, with how to recognise them:

- **Float bits shown as conversions:** `param_1[0x81] = (int)fVar6` and `(float)param_1[0xb3]` on an `int *` are usually copies of
  a float's bits, not conversions. Real conversions are `rt_trunc_w` or `(float)...i` in the generated C.
- **Unsigned range checks:** `*(byte *)x - 2 < 2` means "2 or 3". Read as C it would also be true for 0 and 1.
- **Indirect calls can lose arguments:** `(**(code **)(*DAT_... + n))()` with assignments just before it, especially through a global
  object, or anywhere the caller's class isn't known.
- **Leftover registers:** an `in_a3` that is only passed along is usually an artifact; one used as a pointer in the body is real.
- **Skipped arguments:** `arg_a1` means earlier argument registers are unread; `param_N` the Nth argument register; `stack_N` a
  stack argument.
- **Wrong callee name:** `switchD_09d1a334::default` in `em58` is `f_EBOOT_08865E40`.
- **Thunks:** a call may show a `j` thunk's target; search both addresses.
- **VFPU:** matrix transposition is not shown; vector operations appear as calls such as `vdot_t`.
- **Patches and gates:** the view shows the game's original code, not `recomp/patches.py`'s changes.
- **Calls through a vtable can drop `this`:** `(**(code **)(*(int *)self + 0x20))()` with nothing set beforehand usually means a0 still holds `self`.
- **Local floats that are really words:** `float local_c = DAT_...; self->field_4C = local_c` copied through a float register, often the three words of a member function pointer. The field isn't a float: check the generated C.
- **Structs of a class only as good as the evidence:** a function's `self` comes from what it ran on, its vtable or its callers, so one that ran only on hunters shows `Hunter *` even if it is more general.
- **Rarely, a lost store:** one write to a stack struct was missing in `demo_task 9A60A28`. When a local array matters, check the generated C.
