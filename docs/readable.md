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
for it. Traces show what ran, so "in traces" is never "only".

## Names and types

`tools/readable/names/*.toml`: function, address, name, purpose, confidence, evidence. Addresses and notes only.

- `confirmed`: settled by a trace, a watchpoint, a patch, a string, or a short whole-body read. For a name about mechanics, that
  is strong; a game word in it is only as good as what it rests on.
- `inferred`: follows from the code and callers. Treat it as a lead: check it before relying on it.
- `guess`: a plausible reading.

`tools/readable/types.toml` holds structures; `gen_types.py` makes `build/read/mhfu_types.h`, its offsets checked at compile time.
To add either, edit the file and run `tools/readable/all.sh`.

## What the view gets wrong

It is a reading aid: when it matters, check the generated C. Known, with how to recognise them:

- **Float bits shown as conversions:** `param_1[0x81] = (int)fVar6` and `(float)param_1[0xb3]` on an `int *` are usually copies of
  a float's bits, not conversions. Real conversions are `rt_trunc_w` or `(float)...i` in the generated C.
- **Unsigned range checks:** `*(byte *)x - 2 < 2` means "2 or 3". Read as C it would also be true for 0 and 1.
- **Indirect calls can lose arguments:** `(**(code **)(*DAT_... + n))()` with assignments just before it, especially through a global
  object. Calls through `param_1` have been right where checked.
- **Leftover registers:** an `in_a3` that is only passed along is usually an artifact; one used as a pointer in the body is real.
- **Skipped arguments:** `arg_a1` means earlier argument registers are unread; `param_N` the Nth argument register; `stack_N` a
  stack argument.
- **Wrong callee name:** `switchD_09d1a334::default` in `em58` is `f_EBOOT_08865E40`.
- **Thunks:** a call may show a `j` thunk's target; search both addresses.
- **VFPU:** matrix transposition is not shown; vector operations appear as calls such as `vdot_t`.
- **Patches and gates:** the view shows the game's original code, not `recomp/patches.py`'s changes.
