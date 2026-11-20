#!/usr/bin/env python3
"""Verify the exact original Quest JNI_OnLoad pinned for limited execution.

No game ELF code executes here: this test checks the source-file identity,
the AArch64 ABI JNI call sequence and the class name before release.
"""
from __future__ import annotations

import pathlib
import struct
import zlib


PROJECT = pathlib.Path(__file__).resolve().parents[1]
ELF = PROJECT / "import" / "quest-engine" / "APK" / "lib" / "arm64-v8a" / "libmain.so"
PIN_SIZE = 6696
PIN_CRC32 = 0x20C44EDE
JNI_ENTRY_VADDR = 0x4A80


def main() -> None:
    image = ELF.read_bytes()
    assert len(image) == PIN_SIZE, "Quest libmain.so size differs from tested source"
    assert zlib.crc32(image) == PIN_CRC32, "Quest libmain.so checksum changed"
    assert image[:4] == b"\x7fELF" and image[4] == 2
    assert b"com/unity3d/player/NativeLoader\x00" in image
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", image, 54)
    assert phentsize == 56 and phnum < 32
    entry = None
    for i in range(phnum):
        p_type, p_flags, p_off, vaddr, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", image, phoff + i * phentsize
        )
        if p_type == 1 and p_flags & 1 and vaddr <= JNI_ENTRY_VADDR < vaddr + filesz:
            entry = p_off + JNI_ENTRY_VADDR - vaddr
            break
    assert entry is not None, "JNI_OnLoad not found in an executable PT_LOAD"
    opcodes = struct.unpack_from("<" + "I" * 6, image, entry)
    # JNI_OnLoad: save LR; read JavaVM invocation table; AttachCurrentThread(slot 4);
    # the later body uses FindClass(slot 6) and RegisterNatives(slot 215).
    assert opcodes == (
        0xA9BF7FFE, 0xF9400008, 0x910023E1,
        0xAA1F03E2, 0xF9401108, 0xD63F0100
    ), "Original JNI ABI call sequence changed"
    print("PASS: pinned original Quest 1.46 libmain JNI_OnLoad ELF/ABI contract")


if __name__ == "__main__":
    main()
