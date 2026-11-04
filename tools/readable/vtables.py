#!/usr/bin/env python3
"""The game's vtables, from RAM snapshots: vtables.py WORK SNAPSHOT... A vtable is filled in by code, so the game's files don't
hold them. Scans memory for runs of at least 3 words that are function starts, each checked against the bytes of the module it
is in. Writes WORK/vtables.json: {vtable address: {slot offset: [module, function]}}, and WORK/methods.json: {"module addr":
[[vtable, slot], ...]}. RAM from a recording of the user's game: both files stay out of git."""
import json
import os
import struct
import sys

RAM = 0x08800000


def main():
    work, snaps = sys.argv[1], sys.argv[2:]
    owners = {}
    with open(os.path.join(work, "allfuncs.tsv")) as f:
        for line in f:
            a, m = line.rstrip("\n").split("\t")
            owners.setdefault(int(a, 16), []).append(m)
    bases = {}
    images = {}
    with open(os.path.join(work, "modules.tsv")) as f:
        for line in f:
            n, b, _s = line.split("\t")
            bases[n] = int(b, 16)
    def image(m):
        if m not in images:
            with open(os.path.join(work, m, "image.bin"), "rb") as f:
                images[m] = f.read()
        return images[m]

    tables = {}
    for snap in snaps:
        with open(snap, "rb") as f:
            ram = f.read()
        words = struct.unpack(f"<{len(ram) // 4}I", ram[:len(ram) & ~3])

        def module_at(addr):
            """The owner whose code is what is loaded at addr."""
            if addr not in owners or not RAM <= addr < RAM + len(ram) - 16:
                return None
            here = ram[addr - RAM:addr - RAM + 16]
            for m in owners[addr]:
                img = image(m)
                off = addr - bases[m]
                if img[off:off + 16] == here:
                    return m
            return None

        i = 0
        while i < len(words):
            if words[i] not in owners:
                i += 1
                continue
            j = i
            slots = {}
            while j < len(words) and (words[j] in owners or (words[j] == 0 and j + 1 < len(words) and words[j + 1] in owners)):
                if words[j]:
                    m = module_at(words[j])
                    if m:
                        slots[(j - i) * 4] = [m, words[j]]
                j += 1
            if len(slots) >= 3:
                # slot offsets from the vtable's start: the two words before the first method hold its header
                start = RAM + (i - 2) * 4 if i >= 2 and words[i - 1] == 0 else RAM + i * 4
                delta = (RAM + i * 4) - start
                t = tables.setdefault(start, {})
                for off, v in slots.items():
                    t.setdefault(off + delta, v)
            i = max(j, i + 1)
    methods = {}
    for vt, slots in tables.items():
        for off, (m, fn) in slots.items():
            methods.setdefault(f"{m} {fn:x}", []).append([f"{vt:08X}", off])
    with open(os.path.join(work, "vtables.json"), "w") as f:
        json.dump({f"{k:08X}": {str(o): [v[0], f"{v[1]:08X}"] for o, v in t.items()} for k, t in tables.items()}, f)
    with open(os.path.join(work, "methods.json"), "w") as f:
        json.dump(methods, f)
    print(f"{len(tables)} vtables, {len(methods)} methods")
    for want in (0x089BB3CC, 0x089BC680, 0x089BC77C):
        t = tables.get(want)
        print(f"{want:08X}:", {hex(o): f"{v[0]} {v[1]:08X}" for o, v in sorted((t or {}).items())[:18]})


if __name__ == "__main__":
    main()
