#!/usr/bin/env python3
"""Inputs for the readable view: for each module, its bytes and its functions as Ghidra's script reads them.

Writes <work>/<module>/image.bin (the module's bytes at its base) and funcs.tsv: one line per function,
    start end how jr:target,target;jr:target,...     (hex; the last field is its jump tables, may be empty)
and <work>/modules.tsv: name, base, size. The function boundaries and jump tables are tools/funcs.py's, so the view
covers exactly what the recompiler translates. Everything written here comes from the game: keep <work> out of git.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import modules  # noqa: E402

FUNCTIONS = os.path.join(HERE, "..", "..", "game", "analysis", "functions.json")


def tables(func):
    """Jump tables only: tail calls through tables of functions (fptables) stay calls, as in the recompiler."""
    return ";".join(f"{jr:x}:" + ",".join(f"{t:x}" for t in sorted(set(targets)))
                    for jr, _table, targets in func.get("tables", []))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("work")
    ap.add_argument("--only", help="comma-separated module names (default: all)")
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    with open(FUNCTIONS) as f:
        functions = json.load(f)
    mods = modules.load()
    os.makedirs(args.work, exist_ok=True)
    with open(os.path.join(args.work, "modules.tsv"), "w") as index:
        for m in mods:
            index.write(f"{m.name}\t{m.base:x}\t{len(m.data):x}\n")
            if only and m.name not in only and m.name != "EBOOT":
                continue
            out = os.path.join(args.work, m.name)
            os.makedirs(out, exist_ok=True)
            with open(os.path.join(out, "image.bin"), "wb") as f:
                f.write(m.data)
            with open(os.path.join(out, "funcs.tsv"), "w") as f:
                for func in functions[m.name]["functions"]:
                    f.write(f"{func['start']:x}\t{func['end']:x}\t{func['how']}\t{tables(func)}\n")


if __name__ == "__main__":
    main()
