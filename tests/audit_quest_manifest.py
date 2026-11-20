"""Inspect the original Quest APK binary AndroidManifest.xml without modifying it.

This is a read-only aid for implementing the real Activity/Context adapter:
Java package and launch metadata must originate in the staged APK, rather
than guessed values or a synthetic UnityPlayerGameActivity object.
"""

import argparse
import struct
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"
NO_INDEX = 0xFFFFFFFF


def u16(data: bytes, pos: int) -> int:
    return struct.unpack_from("<H", data, pos)[0]


def u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


def leb(data: bytes, pos: int, utf8: bool) -> tuple[int, int]:
    if utf8:
        b = data[pos]
        if b & 0x80:
            return ((b & 0x7F) << 8) | data[pos + 1], pos + 2
        return b, pos + 1
    v = u16(data, pos)
    if v & 0x8000:
        return ((v & 0x7FFF) << 16) | u16(data, pos + 2), pos + 4
    return v, pos + 2


def strings_from_chunk(data: bytes, start: int, size: int) -> list[str]:
    header_size = u16(data, start + 2)
    count = u32(data, start + 8)
    flags = u32(data, start + 16)
    strings_start = u32(data, start + 20)
    assert header_size >= 28 and count <= 100000 and strings_start < size
    utf8 = bool(flags & 0x100)
    result = []
    for i in range(count):
        off = u32(data, start + header_size + i * 4)
        pos = start + strings_start + off
        _, pos = leb(data, pos, utf8)
        if utf8:
            byte_count, pos = leb(data, pos, True)
            value = data[pos : pos + byte_count].decode("utf-8")
            assert data[pos + byte_count] == 0
        else:
            char_count = u16(data, pos - 2)
            value = data[pos : pos + char_count * 2].decode("utf-16le")
        result.append(value)
    return result


def manifest_entries(data: bytes):
    assert len(data) >= 8 and u16(data, 0) == 3
    assert u32(data, 4) == len(data)
    strings = []
    i = u16(data, 2)
    while i < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, i)
        assert header >= 8 and size >= header and i + size <= len(data)
        if kind == 1:
            strings = strings_from_chunk(data, i, size)
        elif kind == 0x102:
            assert header >= 16 and size >= 36 and strings
            name_idx = u32(data, i + 20)
            name = strings[name_idx]
            attr_start = u16(data, i + 24)
            attr_size = u16(data, i + 26)
            attr_count = u16(data, i + 28)
            attrs = {}
            assert attr_size >= 20 and 16 + attr_start + attr_count * attr_size <= size
            for n in range(attr_count):
                p = i + 16 + attr_start + n * attr_size
                attr_name = strings[u32(data, p + 4)]
                raw = u32(data, p + 8)
                typ = data[p + 15]
                value = u32(data, p + 16)
                if raw != NO_INDEX:
                    decoded = strings[raw]
                elif typ == 3:
                    decoded = strings[value]
                elif typ == 0x12:
                    decoded = bool(value)
                elif typ in (0x10, 0x11):
                    decoded = value
                else:
                    decoded = f"@{value:08x}" if typ == 1 else f"type{typ}:{value:08x}"
                attrs[attr_name] = decoded
            yield name, attrs
        i += size
    assert i == len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", nargs="?", default=APK, type=Path)
    args = parser.parse_args()
    with zipfile.ZipFile(args.apk, "r") as archive:
        entry = archive.getinfo("AndroidManifest.xml")
        assert entry.file_size == 8956, "Unexpected original Quest manifest size"
        contents = archive.read(entry)
        print(f"APK AndroidManifest.xml: bytes={entry.file_size} CRC32={entry.CRC:08x}")
    for name, attrs in manifest_entries(contents):
        if name in {"manifest", "uses-sdk", "application", "activity", "meta-data", "intent-filter", "action", "category"}:
            print(f"{name}: {attrs}")


if __name__ == "__main__":
    main()
