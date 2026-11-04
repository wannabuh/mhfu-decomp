#!/usr/bin/env python3
"""Quantities in decimal: decimals.py VIEW. A field marked `decimal = true` (a count, a timer, stamina) is compared and assigned in
decimal in the view: `self->stamina < 76`, not `< 0x4c`. Everything else stays as the decompiler has it, so ids and flags still
match the notes and function names."""
import glob
import os
import re
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = r"(==|!=|<=|>=|<|>|=|\+|-)"


def decimal_fields():
    out = set()
    tables = []
    with open(os.path.join(HERE, "types.toml"), "rb") as f:
        for s in tomllib.load(f).get("struct", []):
            tables += s.get("fields", [])
    for path in glob.glob(os.path.join(HERE, "fields", "*.toml")):
        with open(path, "rb") as f:
            tables += tomllib.load(f).get("field", [])
    for fd in tables:
        if fd.get("decimal"):
            out.add(fd["name"])
    return out


def to_decimal(m):
    return str(int(m, 16))


def main():
    names = decimal_fields()
    if not names:
        return
    alt = "|".join(sorted(map(re.escape, names), key=len, reverse=True))
    after = re.compile(rf"(->(?:{alt})\s*{OPS}\s*)(0x[0-9a-f]+)\b")
    before = re.compile(rf"\b(0x[0-9a-f]+)(\s*(?:==|!=|<=|>=|<|>)\s*[\w>\-\[\]]*->(?:{alt})\b)")
    changed = 0
    for path in glob.glob(os.path.join(sys.argv[1], "*", "*.c")):
        with open(path) as f:
            text = f.read()
        new = after.sub(lambda m: m.group(1) + to_decimal(m.group(3)), text)
        new = before.sub(lambda m: to_decimal(m.group(1)) + m.group(2), new)
        if new != text:
            with open(path, "w") as f:
                f.write(new)
            changed += 1
    print(f"decimals: {changed} files")


if __name__ == "__main__":
    main()
