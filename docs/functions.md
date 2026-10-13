# Functions

`tools/funcs.py` finds every function in the EBOOT and the 298 overlays (the recompiler's input), writing
`game/analysis/functions.json`. Its docstring describes the passes; this is what they found, and the compiler's habits
they had to allow for.

## Results

16959 functions in 299 modules; every non-nop instruction is in one; no function's walk runs over another's start.

| Found from                                                        | Functions |
|-------------------------------------------------------------------|-----------|
| calls in the module's own code (and the EBOOT's entry)            | 9647      |
| static constructors (overlays)                                    | 284       |
| tail jumps (`j`), and tail calls through tables of functions      | 197       |
| pointers in the module's own data or code (vtables, callbacks)    | 1078      |
| code no call or pointer reaches - mostly virtual methods whose vtables are filled in by code, not data | 5750 |
| pointers from other modules                                        | 3         |

- 1073 jump tables resolved; 4 tables of functions (`return handlers[state]()`).
- 217 `jr <reg>` unresolved: all but one are virtual calls as tail calls (`lw $t9, N(vtable); jr $t9`) - dispatched by
  address at runtime like any other indirect call. The other is the C++ exception runtime's context restore (below).
- Calls between modules: all 26534 into the EBOOT land on a function start; all 17993 into overlay regions land on a
  function start in at least one overlay of the region (in exactly one for 17828).
- Checked against PPSSPP's own function scan of the EBOOT (its debugger's `hle.func.list`): of its 3562 functions, 3552
  start where ours do, 3525 also end where ours do. The rest differ in explainable ways, ours at least as right: code
  after a call that never returns is a separate function (`abs`, after an abort) where PPSSPP merges them; a library
  routine with two entries sharing a body is three functions with tail calls here; PPSSPP splits at jump-table case
  labels that end in a `j`.

## CodeWarrior's habits

- **`j` within a function**: out-of-line blocks are placed after the epilogue and jump back with `j` (not `b`). A `j`
  to an address within the function's extent is followed; any other `j` is a tail call.
- **Tables of functions** dispatched with `jr` (state machines): told from jump tables by their targets being functions.
- **Jump table bases in callee-saved registers**, hoisted out of loops.
- **Unbounded jump tables** (an index loaded as a byte, no range check): taken to run to the next table.
- **Overlay layout**: a 0x40-byte header, text, data; the static constructors are code at the end of the data, before
  their table (see `tools/modules.py`).

## C++ exceptions: compiled in, never thrown

The EBOOT links CodeWarrior's exception runtime (`std::exception`, `bad_exception`; unwinding at `0x0890C684`, a
context restore at `0x0890DA7C` that reloads the callee-saved registers and `$sp` and jumps to a landing pad). But the
only throw (`0x0890DAE0`, which captures the registers and starts unwinding) is in the runtime's own unexpected-exception
handler, reached only from landing pads - such as `operator delete`'s (`0x0890C0C4`, declared `throw()`). Nothing in
the game throws, so the recompiler can make the landing pads and the context restore traps instead of supporting
non-local jumps.
