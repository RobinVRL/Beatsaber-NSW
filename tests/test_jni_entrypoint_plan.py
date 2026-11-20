"""Validate planned original JNI_OnLoad entrypoints against Quest 1.46 ELF exports.

Several original Unity symbols have explicit ELF versions (for example,
JNI_OnLoad@@libunity), so matching nm's decorated output by exact name gives
false negatives. Inspect raw dynamic symbol names and bindings instead.
This is a host-only ELF inspection; no guest code is run.
"""
from pathlib import Path
import re
import struct


ENGINE = Path(__file__).resolve().parents[1]
LIBS = ENGINE / "import/quest-engine/APK/lib/arm64-v8a"
LIFECYCLE = ENGINE / "source/unity_lifecycle.c"


def exports_jni_onload(path: Path) -> bool:
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError(f"{path.name}: invalid ELF64 little-endian image")
    section_offset = struct.unpack_from("<Q", data, 40)[0]
    section_stride, count = struct.unpack_from("<HH", data, 58)
    if section_stride != 64 or section_offset + count * 64 > len(data):
        raise ValueError(f"{path.name}: invalid section header table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, section_offset + i * 64)
                for i in range(count)]
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        offset, length, string_index, entry_size = (
            section[4], section[5], section[6], section[9])
        if entry_size != 24 or length % 24 or offset + length > len(data):
            raise ValueError(f"{path.name}: invalid dynamic symbol table")
        if string_index >= count:
            raise ValueError(f"{path.name}: invalid dynamic string table index")
        strtab = sections[string_index]
        start, size = strtab[4], strtab[5]
        if start + size > len(data):
            raise ValueError(f"{path.name}: dynamic strings outside image")
        strings = data[start:start + size]
        for position in range(offset, offset + length, 24):
            name_offset, info, other, section_index, _, _ = struct.unpack_from(
                "<IBBHQQ", data, position)
            if section_index == 0 or name_offset >= len(strings):
                continue
            end = strings.find(b"\0", name_offset)
            if end < 0:
                raise ValueError(f"{path.name}: unterminated dynamic symbol")
            if (strings[name_offset:end] == b"JNI_OnLoad" and
                    (info & 15) == 2 and (info >> 4) in (1, 2) and
                    (other & 3) in (0, 3)):
                return True
    return False


def main():
    text = LIFECYCLE.read_text(encoding="utf-8")
    match = re.search(r"static const char \*const jni_order\[\] = \{(.*?)\};",
                      text, re.DOTALL)
    if not match:
        raise AssertionError("original JNI bootstrap ordering absent")
    libraries = re.findall(r'"([^"\n]+\.so)"', match.group(1))
    assert len(libraries) == len(set(libraries)), "duplicate JNI_OnLoad plan entry"
    assert libraries and "libgame.so" in libraries and "libil2cpp.so" in libraries
    for name in libraries:
        assert exports_jni_onload(LIBS / name), (
            f"{name}: bootstrap requires an original JNI_OnLoad export that is absent")
    assert "libunity.so" in libraries, "versioned Unity JNI_OnLoad omitted"
    print(f"PASS: all {len(libraries)} original JNI_OnLoad plan entries are ELF exports")


if __name__ == "__main__":
    main()
