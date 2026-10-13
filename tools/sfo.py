#!/usr/bin/env python3
"""Print a PSP PARAM.SFO's entries (the disc's ID, title, version...), or one of them (--get KEY).

PARAM.SFO: a header (magic "\\0PSF", version, key table offset, data table offset, entry count), then 16-byte index
entries (key offset u16, data format u16, data length u32, data capacity u32, data offset u32); format 0x0204 is a
UTF-8 string (NUL-terminated), 0x0404 a u32, 0x0004 raw bytes.
"""
import argparse
import struct
import sys


def read_sfo(path):
    data = open(path, "rb").read()
    magic, version, key_table, data_table, count = struct.unpack_from("<4sIIII", data, 0)
    if magic != b"\0PSF":
        raise ValueError(f"{path}: not a PARAM.SFO (magic {magic!r})")
    entries = {}
    for i in range(count):
        key_off, fmt, length, _capacity, data_off = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key_start = key_table + key_off
        key = data[key_start:data.index(b"\0", key_start)].decode("ascii")
        raw = data[data_table + data_off:data_table + data_off + length]
        if fmt == 0x0204:
            value = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        elif fmt == 0x0404:
            value = struct.unpack("<I", raw[:4])[0]
        else:
            value = raw.hex()
        entries[key] = value
    return entries


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sfo")
    ap.add_argument("--get", metavar="KEY", help="print only this entry's value")
    args = ap.parse_args()
    entries = read_sfo(args.sfo)
    if args.get:
        if args.get not in entries:
            sys.exit(f"{args.get}: not in {args.sfo}")
        print(entries[args.get])
        return
    for key, value in entries.items():
        print(f"{key:16} {value}")


if __name__ == "__main__":
    main()
