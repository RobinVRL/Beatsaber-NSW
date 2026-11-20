"""Inspect the original Quest ARM64 ELF relocations without executing game code.

Usage: py -3 tests/test_so_util_elf_audit.py ../import/quest-engine/APK/lib/arm64-v8a
This intentionally uses only the Python standard library.
"""

from collections import Counter
from pathlib import Path
import struct
import sys

RELA = 4
ANDROID_RELA = 0x60000002
ANDROID_RELR = 0x6FFFFF00
RELR = 19
PT_LOAD = 1
SHT_DYNSYM = 11
SUPPORTED = {257, 1025, 1026, 1027, 1031}  # ABS64, GLOB_DAT, JUMP_SLOT, RELATIVE, TLSDESC


def span(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError(f"invalid ELF range at {offset:#x} + {size:#x}")
    return memoryview(data)[offset : offset + size]


def sleb(data, position):
    value = 0
    shift = 0
    while position < len(data) and shift < 70:
        b = data[position]
        position += 1
        value |= (b & 127) << shift
        shift += 7
        if not (b & 128):
            if b & 64:
                value -= 1 << shift
            if not -(1 << 63) <= value < (1 << 63):
                raise ValueError("out-of-range SLEB128")
            return value, position
    raise ValueError("invalid SLEB128")


def decode_aps2(data):
    if data[:4] != b"APS2":
        raise ValueError("not APS2")
    pos = 4
    count, pos = sleb(data, pos)
    offset, pos = sleb(data, pos)
    if count < 0 or offset < 0:
        raise ValueError("negative relocation count/offset")
    addend = 0
    entries = []
    while len(entries) < count:
        group, pos = sleb(data, pos)
        flags, pos = sleb(data, pos)
        if group <= 0 or group > count - len(entries) or flags < 0 or flags > 15:
            raise ValueError("invalid APS2 group")
        if flags & 4 and not flags & 8:
            raise ValueError("grouped addend without addend")
        if flags & 2:
            delta, pos = sleb(data, pos)
        if flags & 1:
            info, pos = sleb(data, pos)
        if flags & 4:
            increment, pos = sleb(data, pos)
            addend += increment
        elif not flags & 8:
            addend = 0
        for _ in range(group):
            if not flags & 2:
                delta, pos = sleb(data, pos)
            if not flags & 1:
                info, pos = sleb(data, pos)
            if flags & 8 and not flags & 4:
                increment, pos = sleb(data, pos)
                addend += increment
            offset += delta
            entries.append((offset, info, addend))
    if pos != len(data):
        raise ValueError("trailing APS2 data")
    return entries


def decode_relr(data):
    if len(data) % 8:
        raise ValueError("invalid RELR byte count")
    address = 0
    locations = []
    for (word,) in struct.iter_unpack("<Q", data):
        if not word & 1:
            address = word
            locations.append(address)
            address += 8
        else:
            locations.extend(address + (bit - 1) * 8 for bit in range(1, 64)
                             if word & (1 << bit))
            address += 63 * 8
    return locations


def audit(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise ValueError("expected ELF64 little endian")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    shoff = struct.unpack_from("<Q", data, 40)[0]
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 54)
    if phentsize != 56 or shentsize != 64:
        raise ValueError("unexpected header stride")
    span(data, phoff, phnum * phentsize)
    span(data, shoff, shnum * shentsize)
    loads = []
    for i in range(phnum):
        typ, _, fileoff, vaddr, _, filesz, memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize)
        if typ == PT_LOAD:
            span(data, fileoff, filesz)
            if filesz > memsz:
                raise ValueError("PT_LOAD file bytes exceed memory")
            loads.append((vaddr, vaddr + memsz))
    if not loads:
        raise ValueError("no loadable sections")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    dynsyms = [s for s in sections if s[1] == SHT_DYNSYM]
    if len(dynsyms) != 1 or dynsyms[0][5] % 24:
        raise ValueError("invalid dynamic symbol table")
    symbol_count = dynsyms[0][5] // 24
    counts = Counter()
    def check_location(offset):
        if offset % 8 or not any(lo <= offset and offset + 8 <= hi for lo, hi in loads):
            raise ValueError(f"relocation target outside PT_LOAD: {offset:#x}")

    for sec in sections:
        _, typ, _, _, fileoff, size, _, _, _, stride = sec
        if typ in (RELA, ANDROID_RELA) and size:
            blob = span(data, fileoff, size)
            if typ == ANDROID_RELA:
                entries = decode_aps2(blob)
                counts["aps2"] += len(entries)
            else:
                if size % 24 or stride != 24:
                    raise ValueError("invalid RELA table")
                entries = struct.iter_unpack("<QQq", blob)
            for offset, info, _ in entries:
                check_location(offset)
                if info >> 32 >= symbol_count:
                    raise ValueError(f"relocation refers to invalid symbol index: {info >> 32}")
                counts[info & 0xFFFFFFFF] += 1
        if typ in (RELR, ANDROID_RELR) and size:
            for offset in decode_relr(span(data, fileoff, size)):
                check_location(offset)
                counts["relr"] += 1
    return counts


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        __file__).resolve().parents[2] / "import/quest-engine/APK/lib/arm64-v8a"
    paths = sorted(root.glob("*.so"))
    if len(paths) != 15:
        raise ValueError(f"expected 15 staged Quest libraries, got {len(paths)} at {root}")
    total = Counter()
    for path in paths:
        counts = audit(path)
        total.update(counts)
        unfamiliar = sorted(k for k in counts if isinstance(k, int) and k not in SUPPORTED)
        print(f"{path.name:25} relocs={sum(v for k,v in counts.items() if isinstance(k,int)):,} "
              f"APS2={counts['aps2']:,} RELR={counts['relr']:,} unknown={unfamiliar}")
        if unfamiliar:
            raise ValueError(f"unsupported AArch64 relocation type(s) in {path.name}: {unfamiliar}")
    print("TOTAL:", dict(sorted(total.items(), key=lambda kv: str(kv[0]))))


if __name__ == "__main__":
    main()
