# mhfu-recomp

A static recompilation of **Monster Hunter Freedom Unite** (PSP, US release `ULUS10391`) into a native PC game:
the game's MIPS (Allegrex) code translated to C and built for the PC, running on a runtime that stands in for the
PSP - with a Vulkan renderer, graphical enhancements, a higher frame rate and native PC controls.

**This repository contains no game code or data.** You need your own copy of the game (an ISO of ULUS10391); every
tool here reads it from your disk, and nothing extracted from it is ever committed.

Status: phase 1 (repository and tools). See [docs/plan.md](docs/plan.md).

## Layout

| Path       | What                                                                                  |
|------------|---------------------------------------------------------------------------------------|
| `docs/`    | the plan, notes on the PSP and on the game as we learn it                            |
| `tools/`   | extraction and analysis scripts (Python, in a local environment: `tools/setup.sh`)   |
| `game/`    | (ignored) your ISO's extracted files - `tools/extract_iso.sh <your.iso>`             |

## Getting started

```sh
tools/setup.sh                      # a Python environment with the tools' modules (tools/.venv)
tools/extract_iso.sh path/to/mhfu.iso   # extracts to game/ and checks it is ULUS10391
tools/dump_modules.sh path/to/mhfu.iso  # decrypted EBOOT and PRXs to game/decrypted/ (via PPSSPP, offscreen)
tools/.venv/bin/python tools/databin.py overlays   # the 298 code overlays from DATA.BIN to game/overlays/
tools/.venv/bin/python tools/databin.py extract    # (optional) all 6657 DATA.BIN files to game/data/
tools/.venv/bin/python tools/funcs.py              # every function, to game/analysis/functions.json
```

## Licence

GPL-2.0-or-later (see `LICENSE`): the runtime reuses code from [PPSSPP](https://github.com/hrydgard/ppsspp),
which is under the same licence.
