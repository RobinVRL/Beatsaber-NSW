"""Verify original Quest Unity 6 MediaNDK imports and our AArch64 DATA ABI.

Run: py -3 tests/test_android_media_format.py [path/to/libunity.so]
Uses the staged legally obtained original ELF, the standard library, and the
existing devkitA64 compiler. Does not execute game code or initialize a codec.
"""

from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_so_util_elf_audit import decode_aps2, span


ROOT = Path(__file__).resolve().parents[1]
UNITY = ROOT / "import/quest-engine/APK/lib/arm64-v8a/libunity.so"
KEY_GOT = {
    "AMEDIAFORMAT_KEY_DURATION": 0x17499A8,
    "AMEDIAFORMAT_KEY_WIDTH": 0x17499B0,
    "AMEDIAFORMAT_KEY_HEIGHT": 0x17499B8,
    "AMEDIAFORMAT_KEY_MIME": 0x17499C0,
    "AMEDIAFORMAT_KEY_CHANNEL_COUNT": 0x17499D0,
    "AMEDIAFORMAT_KEY_SAMPLE_RATE": 0x17499D8,
    "AMEDIAFORMAT_KEY_COLOR_FORMAT": 0x17499E0,
    "AMEDIAFORMAT_KEY_COLOR_STANDARD": 0x17499E8,
    "AMEDIAFORMAT_KEY_COLOR_RANGE": 0x17499F0,
    "AMEDIAFORMAT_KEY_FRAME_RATE": 0x17499F8,
    "AMEDIAFORMAT_KEY_ROTATION": 0x1749A00,
    "AMEDIAFORMAT_KEY_LANGUAGE": 0x1749A08,
    "AMEDIAFORMAT_KEY_ENCODER_DELAY": 0x1749A10,
    "AMEDIAFORMAT_KEY_STRIDE": 0x1749A18,
    "AMEDIAFORMAT_KEY_SLICE_HEIGHT": 0x1749A20,
}

FUNCTIONS = {
    "AMediaFormat_delete",
    "AMediaFormat_getFloat",
    "AMediaFormat_getInt32",
    "AMediaFormat_getInt64",
    "AMediaFormat_getString",
    "AMediaFormat_setInt32",
}


def parse_elf(path: Path):
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError(f"{path}: expected little-endian ELF64")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != 183:
        raise ValueError(f"{path}: expected EM_AARCH64, found {machine}")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 58)
    assert shentsize == 64 and shnum
    span(data, shoff, shnum * shentsize)
    sections = [
        struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    symbols = {}
    symbol_tables = {}
    for section_index, section in enumerate(sections):
        if section[1] not in (2, 11):
            continue
        file_offset, size, strings_index, entsize = (
            section[4], section[5], section[6], section[9]
        )
        assert entsize == 24 and 0 <= strings_index < len(sections)
        sym_data = span(data, file_offset, size)
        strings_sec = sections[strings_index]
        strings = bytes(span(data, strings_sec[4], strings_sec[5]))
        assert len(sym_data) % entsize == 0
        values = []
        for name_index, info, other, shndx, value, length in struct.iter_unpack(
            "<IBBHQQ", sym_data
        ):
            assert name_index < len(strings)
            end = strings.index(0, name_index)
            name = strings[name_index:end].decode("ascii", errors="replace")
            values.append((name, info >> 4, info & 15, other, shndx, value, length))
            # Prefer defined definitions over undefined imports.
            if name and (name not in symbols or shndx):
                symbols[name] = values[-1]
        symbol_tables[section_index] = values
    assert symbol_tables

    relocations = []
    for section in sections:
        if section[1] == 4:
            blob = span(data, section[4], section[5])
            assert len(blob) % 24 == 0
            entries = struct.iter_unpack("<QQq", blob)
        elif section[1] == 0x60000002:
            entries = decode_aps2(span(data, section[4], section[5]))
        else:
            continue
        table = symbol_tables[section[6]]
        for target, info, addend in entries:
            symbol_index = info >> 32
            assert symbol_index < len(table)
            relocations.append((target, info & 0xFFFFFFFF,
                                table[symbol_index][0], addend))
    return symbols, relocations


def verify_imports(source: Path) -> set[str]:
    symbols, relocs = parse_elf(source)
    imported = {
        name for name, sym in symbols.items()
        if sym[4] == 0 and name.startswith("AMEDIAFORMAT_KEY_")
    }
    assert imported == set(KEY_GOT), sorted(imported ^ set(KEY_GOT))

    for name, expected_target in KEY_GOT.items():
        symbol = symbols[name]
        # A few OPTIONAL Unity API29+ keys are WEAK NOTYPE (binding 2,
        # symbol type 0); normal keys are STT_OBJECT (type 1). Both are
        # undefined DATA, NOT STT_FUNC.
        assert symbol[2] == 1 or (symbol[1] == 2 and symbol[2] == 0), (
            name, symbol
        )
        matches = [(target, kind, addend) for target, kind, sym, addend in relocs
                   if sym == name]
        assert matches == [(expected_target, 1025, 0)], (name, matches)
        assert expected_target % 8 == 0

    found_funcs = {
        name for name, sym in symbols.items()
        if sym[4] == 0 and name.startswith("AMediaFormat_")
    }
    assert found_funcs == FUNCTIONS, sorted(found_funcs ^ FUNCTIONS)
    for name in FUNCTIONS:
        assert symbols[name][2] == 2, (name, symbols[name])  # STT_FUNC

    codec = [name for name, sym in symbols.items()
             if name.startswith(("AMediaCodec_", "AMediaExtractor_"))
             and sym[4] == 0]
    assert "AMediaCodec_createDecoderByType" in codec
    assert "AMediaExtractor_new" in codec
    optional_data_source = {
        name for name, sym in symbols.items()
        if name.startswith("AMediaDataSource_") and sym[4] == 0
    }
    assert len(optional_data_source) == 6, sorted(optional_data_source)
    print(f"Original Unity: {len(imported)} data keys with exact GLOB_DAT GOT "
          f"addresses, {len(FUNCTIONS)} format calls, {len(codec)} media "
          f"service calls, {len(optional_data_source)} weak data-source calls")
    return set(codec) | optional_data_source


def verify_our_arm64_object(required_service_functions: set[str]) -> None:
    compiler = (
        Path(os.environ.get("DEVKITPRO", "C:/devkitPro")) /
        "devkitA64/bin/aarch64-none-elf-gcc.exe"
    )
    if not compiler.exists():
        raise RuntimeError(f"devkitA64 compiler unavailable: {compiler}")
    with tempfile.TemporaryDirectory(prefix="bs_mediandk_abi_") as scratch:
        object_file = Path(scratch) / "android_media_format.o"
        subprocess.run(
            [str(compiler), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-fPIC", "-c", str(ROOT / "source/android_media_format.c"),
             "-o", str(object_file)],
            check=True, capture_output=True, text=True
        )
        symbols, _ = parse_elf(object_file)
        for name in KEY_GOT:
            sym = symbols[name]
            # 'extern const char *KEY' is 8-byte STT_OBJECT on AArch64.
            # A const char KEY[] array would have a string-length st_size.
            assert sym[2] == 1 and sym[4] != 0 and sym[6] == 8, (name, sym)
        for name in FUNCTIONS | required_service_functions:
            sym = symbols[name]
            assert sym[2] == 2 and sym[4] != 0, (name, sym)
    print("devkitA64 object: all 15 exported KEY slots are defined 8-byte "
          "STT_OBJECT pointer variables; format exports are functions")


def main() -> None:
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else UNITY
    media_functions = verify_imports(source)
    verify_our_arm64_object(media_functions)
    print("PASS: Quest Unity 6 MediaNDK imported symbol + AArch64 storage ABI")


if __name__ == "__main__":
    main()
