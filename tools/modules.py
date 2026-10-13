"""The game's code modules: the EBOOT and its 298 overlays, with their address ranges and imports.

Overlays come from game/overlays/ (tools/databin.py overlays). Each starts with a 0x40-byte Metrowerks overlay header:
  +0x00 "MWo3"   +0x04 overlay number (its position among the EBOOT's .ovl sections, from 1)   +0x08 load address
  +0x0C text size   +0x10 data size   +0x14 bss size   +0x18, +0x1C the static constructor table   +0x20 name
then the text (from +0x40, starting with zero padding), then the data (to the end of the file). The constructors are
code at the end of the data, just before their table: a second code range, from the first constructor to the table.
"""
import os
import struct

import rabbitizer
from elftools.elf.elffile import ELFFile

import databin

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
EBOOT = os.path.join(ROOT, "game/decrypted/EBOOT.elf")
OVERLAYS = os.path.join(ROOT, "game/overlays")

# Overlay regions: index -> (name, base); region 0 is the EBOOT
REGIONS = {0: "main", 1: "task", 2: "sub", 3: "em", 4: "stage"}

rabbitizer.config.regNames_gprAbiNames = rabbitizer.Abi.O32
CATEGORY = rabbitizer.InstrCategory.R4000ALLEGREX


class Module:
    def __init__(self, name, region, base, data, ranges, data_end):
        self.name = name
        self.region = region
        self.base = base
        self.data = data
        self.ranges = ranges          # code: [(start, end)], the text first
        self.text_start, self.text_end = ranges[0]
        self.data_end = data_end      # end of the initialised data that follows the text
        self.imports = {}             # stub address -> (library, nid); the EBOOT's only
        self.ctors = []               # static constructors (overlays)
        self.number = 0               # the overlay number in its header
        self._ins = {}

    def __repr__(self):
        return f"<{self.name} " + " ".join(f"{s:#x}-{e:#x}" for s, e in self.ranges) + ">"

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.data)

    def in_text(self, addr):
        return any(s <= addr < e for s, e in self.ranges)

    def code(self):
        """Every instruction address, range by range."""
        for s, e in self.ranges:
            yield from range(s, e, 4)

    def data_words(self):
        """Addresses of the aligned words of initialised data (outside the code ranges)."""
        for a in range(self.text_end, self.data_end - 3, 4):
            if not self.in_text(a):
                yield a

    def word(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def ins(self, addr):
        i = self._ins.get(addr)
        if i is None:
            i = self._ins[addr] = rabbitizer.Instruction(self.word(addr), addr, CATEGORY)
        return i


def _imports(elf, seg_base, seg_data):
    """{stub address: (library, nid)} from .lib.stub (SceLibStubEntry: name, version, flags, entry size, var count,
    func count, nid table, stub table)."""
    sec = elf.get_section_by_name(".lib.stub")
    out = {}

    def at(addr, n):
        return seg_data[addr - seg_base:addr - seg_base + n]

    addr, end = sec["sh_addr"], sec["sh_addr"] + sec["sh_size"]
    while addr < end:
        name_ptr, _ver, _flags, entry_size, _nvars, nfuncs, nids, stubs = struct.unpack("<IHHBBHII", at(addr, 20))
        name = at(name_ptr, 64).split(b"\0", 1)[0].decode()
        for k in range(nfuncs):
            nid = struct.unpack("<I", at(nids + 4 * k, 4))[0]
            out[stubs + 8 * k] = (name, nid)
        addr += entry_size * 4
    return out


def load(eboot=EBOOT, overlays=OVERLAYS):
    """The EBOOT and every overlay, as Modules (the EBOOT first, then overlays in their ELF order)."""
    with open(eboot, "rb") as f:
        elf = ELFFile(f)
        seg = next(s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD")
        base, data = seg["p_vaddr"], seg.data()
        text = elf.get_section_by_name(".text")
        main = Module("EBOOT", 0, base, data, [(text["sh_addr"], text["sh_addr"] + text["sh_size"])],
                      base + seg["p_filesz"])
        main.imports = _imports(elf, base, data)
        main.entry = elf["e_entry"]
    mods = [main]
    region_of = {}
    for name, (ovl_base, _memsz) in databin.overlay_regions(eboot).items():
        region_of.setdefault(ovl_base, len(region_of) + 1)
        stem = name[:-len(".ovl")]
        with open(os.path.join(overlays, stem + ".bin"), "rb") as f:
            data = f.read()
        magic, num, load_addr, text_size, data_size, _bss, ctor0, ctor1 = struct.unpack_from("<4s7I", data, 0)
        if magic != b"MWo3" or load_addr != ovl_base or 0x40 + text_size + data_size != len(data):
            raise ValueError(f"{stem}: unexpected overlay header")
        text = (ovl_base + 0x40, ovl_base + 0x40 + text_size)
        ctors = [struct.unpack_from("<I", data, ctor0 - ovl_base + k)[0] for k in range(0, ctor1 - ctor0, 4)]
        ranges = [text] + ([(min(ctors), ctor0)] if ctors else [])
        m = Module(stem, region_of[ovl_base], ovl_base, data, ranges, text[1] + data_size)
        m.ctors = ctors
        m.number = num
        mods.append(m)
    return mods
