#!/usr/bin/env python3
"""Pinned original Quest 1.46 IL2CPP JNI_OnLoad and its only import.

The C loader invokes this 72-byte AArch64 function after all guest images
have been mapped. This host test proves the code, source image, GOT
relocation and JNI symbol correspond to the actual supplied APK.
"""
from __future__ import annotations

import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATH = ROOT.parent / "import/quest-engine/APK/lib/arm64-v8a/libil2cpp.so"
SIZE = 130481456
CRC = 0x44987195
ENTRY = 0x34A3598
LOGGER_GOT = 0x7821548
CODE = (
    0xA9BF4FFE, 0xAA0003F3, 0x90FEF201, 0x910E9021,
    0xB0FEF162, 0x91247042, 0x52800080, 0x94FB7E97,
    0xF0023FC8, 0xF0023FC9, 0x528000C0, 0xF907F113,
    0xD503201F, 0x100000A8, 0x72A00020, 0xF907E128,
    0xA8C14FFE, 0xD65F03C0,
)


def cstring(table: bytes, index: int) -> str:
    assert 0 <= index < len(table)
    end = table.index(b"\x00", index)
    return table[index:end].decode("ascii")


def main() -> None:
    image = PATH.read_bytes()
    assert len(image) == SIZE and zlib.crc32(image) == CRC
    assert image[:6] == b"\x7fELF\x02\x01"
    assert struct.unpack_from("<H", image, 18)[0] == 183  # EM_AARCH64
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", image, 54)
    shoff = struct.unpack_from("<Q", image, 40)[0]
    shentsize, shnum, shstrindex = struct.unpack_from("<HHH", image, 58)
    assert phentsize == 56 and shentsize == 64 and 0 < shnum < 100

    def segment_for(vaddr: int, length: int):
        for i in range(phnum):
            ph = struct.unpack_from("<IIQQQQQQ", image, phoff + i * phentsize)
            p_type, flags, offset, address, _, file_size, mem_size, _ = ph
            if p_type == 1 and address <= vaddr and vaddr + length <= address + mem_size:
                return flags, offset + vaddr - address, vaddr + length <= address + file_size
        raise AssertionError(f"no PT_LOAD for {vaddr:#x}")

    flags, offset, backed = segment_for(ENTRY, 72)
    assert flags & 1 and backed
    assert struct.unpack_from("<18I", image, offset) == CODE
    flags, _, _ = segment_for(LOGGER_GOT, 8)
    assert flags & 2 and not flags & 1, "logger PLT slot must be writable data"

    sections = {}
    section_headers = []
    shstr_header = struct.unpack_from("<IIQQQQIIQQ",
                                       image, shoff + shstrindex * shentsize)
    shstr = image[shstr_header[4]:shstr_header[4] + shstr_header[5]]
    for i in range(shnum):
        sh = struct.unpack_from("<IIQQQQIIQQ", image, shoff + i * shentsize)
        section_headers.append(sh)
        sections[cstring(shstr, sh[0])] = sh

    dynsym = sections[".dynsym"]
    dynstr = sections[".dynstr"]
    relaplt = sections[".rela.plt"]
    strtab = image[dynstr[4]:dynstr[4] + dynstr[5]]
    syms = image[dynsym[4]:dynsym[4] + dynsym[5]]
    assert dynsym[9] == 24 and relaplt[9] == 24
    assert any(
        cstring(strtab, struct.unpack_from("<I", syms, i)[0]) == "JNI_OnLoad"
        and struct.unpack_from("<Q", syms, i + 8)[0] == ENTRY
        for i in range(0, len(syms), 24)
    )
    found = 0
    for i in range(relaplt[4], relaplt[4] + relaplt[5], 24):
        location, r_info, addend = struct.unpack_from("<QQq", image, i)
        if location != LOGGER_GOT:
            continue
        sym_index, kind = r_info >> 32, r_info & 0xFFFFFFFF
        name_offset = struct.unpack_from("<I", syms, sym_index * 24)[0]
        assert kind == 1026 and addend == 0
        assert cstring(strtab, name_offset) == "__android_log_print"
        found += 1
    assert found == 1
    print("PASS: original IL2CPP JNI_OnLoad checksum, instructions, "
          "symbol, ELF section, and logger relocation")


if __name__ == "__main__":
    main()
