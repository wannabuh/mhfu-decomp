#!/usr/bin/env python3
"""List or extract the files packed in DATA.BIN (decrypting them as the game does).

The cipher (the game's code at 0x0884EA1C, key setup at 0x0884EC3C): a read starting at sector S (2048-byte sectors)
seeds two 16-bit generators, k0 = S & 0xFFFF (0x7F8D if 0) and k1 = S >> 16 (0x2345 if 0). Each 32-bit word of the
read then has its bytes substituted through a 256-byte table (in the EBOOT's data, at 0x0899A138), and is XORed with
the next key: k0 = k0 * 0x7F8D mod 0xFFF1, k1 = k1 * 0x2345 mod 0xFFD9, key = k1 << 16 | k0. The key stream runs on
through a whole read - it is not reset per sector.

Layout: a header at sector 0, read (and decrypted) as one stream of 0x6808 + 0x1968 bytes:
  - u32 start sector of each file, 0x6808 / 4 of them; file i occupies sectors start[i] to start[i+1]
  - (u32 file index, u32 exact size) pairs for files whose size is not a whole number of sectors, ending at index 0xFFFF

Overlays: the EBOOT names 298 of them (a PT_NULL program header with the load address and memory size, and a
`<name>.ovl` section, for each). Their code is in DATA.BIN - files 63-76 (tasks and subs), 5542-5808 (stages) and
6095-6111 (monsters) - and each file contains its own name, which is how `overlays` matches them up.
"""
import argparse
import os
import re
import struct
import sys

import numpy as np
from elftools.elf.elffile import ELFFile

SECTOR = 0x800
STARTS_SIZE = 0x6808
SIZES_SIZE = 0x1968
TABLE_ADDR = 0x0899A138


def load_table(eboot_path):
    """The 256-byte substitution table, from the decrypted EBOOT."""
    with open(eboot_path, "rb") as f:
        elf = ELFFile(f)
        seg = next(s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD")
        off = TABLE_ADDR - seg["p_vaddr"]
        table = seg.data()[off:off + 256]
    if sorted(table) != list(range(256)):
        raise ValueError(f"{eboot_path}: no substitution table at {TABLE_ADDR:#x} (not the ULUS10391 EBOOT?)")
    return np.frombuffer(table, dtype=np.uint8)


def _powers(mul, mod):
    """mul^n mod `mod` for n = 0 .. period-1 (the sequence is periodic, as mul is invertible mod a prime)."""
    p, out = 1, []
    while True:
        out.append(p)
        p = p * mul % mod
        if p == 1:
            return np.array(out, dtype=np.uint64)


POW0 = _powers(0x7F8D, 0xFFF1)
POW1 = _powers(0x2345, 0xFFD9)


def keystream(sector, nwords):
    k0 = sector & 0xFFFF or 0x7F8D
    k1 = sector >> 16 or 0x2345
    n = np.arange(1, nwords + 1, dtype=np.uint64)
    key0 = k0 * POW0[n % len(POW0)] % 0xFFF1
    key1 = k1 * POW1[n % len(POW1)] % 0xFFD9
    return ((key1 << 16) | key0).astype(np.uint32)


def decrypt(data, sector, table):
    """Decrypt a read of `data` that started at `sector`."""
    padded = data + b"\0" * (-len(data) % 4)
    words = table[np.frombuffer(padded, dtype=np.uint8)].view("<u4") ^ keystream(sector, len(padded) // 4)
    return words.tobytes()[:len(data)]


class DataBin:
    def __init__(self, path, table):
        self.f = open(path, "rb")
        self.table = table
        header = decrypt(self.f.read(STARTS_SIZE + SIZES_SIZE), 0, table)
        self.starts = list(struct.unpack_from(f"<{STARTS_SIZE // 4}I", header, 0))
        self.exact = {}
        for index, size in struct.iter_unpack("<II", header[STARTS_SIZE:]):
            if index == 0xFFFF:
                break
            self.exact[index] = size
        self.count = len(self.starts) - 1

    def size(self, i):
        return self.exact.get(i, (self.starts[i + 1] - self.starts[i]) * SECTOR)

    def read(self, i):
        self.f.seek(self.starts[i] * SECTOR)
        return decrypt(self.f.read(self.size(i)), self.starts[i], self.table)


def overlay_regions(eboot_path):
    """{name: (load address, memory size)} for the EBOOT's overlays, in its section order."""
    with open(eboot_path, "rb") as f:
        elf = ELFFile(f)
        segs = [s for s in elf.iter_segments() if s["p_type"] == "PT_NULL"]
        names = [s.name for s in elf.iter_sections() if s.name.endswith(".ovl") and not s.name.startswith(".rel")]
    return {n: (s["p_vaddr"], s["p_memsz"]) for n, s in zip(names, segs)}


def find_overlays(db, regions):
    """{name: file index}: the files that contain exactly one overlay name, which must be unique."""
    found = {}
    for i in range(db.count):
        names = {m.decode() for m in re.findall(rb"([a-z_]+[0-9]*\.ovl)\0", db.read(i))} & regions.keys()
        if len(names) == 1:
            name = names.pop()
            if name in found:
                raise ValueError(f"{name} in both file {found[name]} and file {i}")
            found[name] = i
    missing = regions.keys() - found.keys()
    if missing:
        raise ValueError(f"overlays not found in DATA.BIN: {sorted(missing)}")
    return found


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", default=os.path.join(root, "game/PSP_GAME/USRDIR/DATA.BIN"))
    ap.add_argument("--eboot", default=os.path.join(root, "game/decrypted/EBOOT.elf"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="index, start sector, size and first bytes of each file")
    ex = sub.add_parser("extract", help="write files to a directory as NNNN.bin")
    ex.add_argument("indices", nargs="*", type=int, help="files to extract (default: all)")
    ex.add_argument("-o", "--out", default=os.path.join(root, "game/data"))
    ov = sub.add_parser("overlays", help="write the overlays as <name>.bin and print their files and load addresses")
    ov.add_argument("-o", "--out", default=os.path.join(root, "game/overlays"))
    args = ap.parse_args()

    db = DataBin(args.data, load_table(args.eboot))
    if args.cmd == "overlays":
        regions = overlay_regions(args.eboot)
        os.makedirs(args.out, exist_ok=True)
        for name, i in sorted(find_overlays(db, regions).items(), key=lambda item: item[1]):
            data = db.read(i)
            base, memsz = regions[name]
            if len(data) > memsz:
                raise ValueError(f"{name} (file {i}) is {len(data):#x} bytes, more than its {memsz:#x}")
            with open(os.path.join(args.out, name[:-len(".ovl")] + ".bin"), "wb") as f:
                f.write(data)
            print(f"{name:22} file {i:4}  {base:#010x}  size {len(data):#08x}  memsz {memsz:#08x}")
    elif args.cmd == "list":
        for i in range(db.count):
            db.f.seek(db.starts[i] * SECTOR)
            head = decrypt(db.f.read(16), db.starts[i], db.table)
            print(f"{i:4} sector {db.starts[i]:#08x} size {db.size(i):#09x}  {head.hex(' ')}")
    else:
        os.makedirs(args.out, exist_ok=True)
        indices = args.indices or range(db.count)
        for i in indices:
            with open(os.path.join(args.out, f"{i:04}.bin"), "wb") as f:
                f.write(db.read(i))
        print(f"{len(indices)} files to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
