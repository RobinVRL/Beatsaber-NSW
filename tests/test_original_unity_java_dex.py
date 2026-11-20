"""Read-only audit of the ORIGINAL Quest APK's Java Unity bootstrap classes.

The guest's JNI_OnLoad uses these class contracts. A symbolic FindClass
handle is insufficient to claim an executable Android Java runtime. Parsing
the exact DEX prevents implementations based on guessed class names/methods.
No external DEX packages or modifications to the original APK are required.
"""

from __future__ import annotations

import pathlib
import struct
import zipfile


APK = pathlib.Path(__file__).resolve().parents[1] / "SwitchPort" / "beat-saber.apk"
TARGET = "Lcom/unity3d/player/UnityPlayerForActivityOrService;"
UNITY_PLAYER = "Lcom/unity3d/player/UnityPlayer;"


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in range(0, 35, 7):
        if offset >= len(data):
            raise ValueError("truncated DEX ULEB128")
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError("invalid DEX ULEB128")


class Dex:
    def __init__(self, data: bytes):
        if len(data) < 112 or not data.startswith(b"dex\n"):
            raise ValueError("not a valid DEX header")
        if u32(data, 32) != len(data):
            raise ValueError("DEX file_size does not match the APK member")
        self.data = data
        self.strings = []
        size, offset = u32(data, 0x38), u32(data, 0x3C)
        for i in range(size):
            at = u32(data, offset + 4 * i)
            _, start = uleb(data, at)
            end = data.index(0, start)
            self.strings.append(data[start:end].decode("utf-8", errors="replace"))

        size, offset = u32(data, 0x40), u32(data, 0x44)
        self.types = [self.strings[u32(data, offset + 4 * i)] for i in range(size)]

        size, offset = u32(data, 0x50), u32(data, 0x54)
        self.fields = []
        for i in range(size):
            cls, typ, name = struct.unpack_from("<HHI", data, offset + 8 * i)
            self.fields.append((self.types[cls], self.types[typ], self.strings[name]))

        size, offset = u32(data, 0x58), u32(data, 0x5C)
        self.methods = []
        for i in range(size):
            cls, proto, name = struct.unpack_from("<HHI", data, offset + 8 * i)
            self.methods.append((self.types[cls], proto, self.strings[name]))

        size, offset = u32(data, 0x60), u32(data, 0x64)
        self.classes = {}
        for i in range(size):
            desc, flags, superclass, _, _, _, data_off, _ = struct.unpack_from(
                "<IIIIIIII", data, offset + 32 * i
            )
            self.classes[self.types[desc]] = (
                self.types[superclass] if superclass != 0xFFFFFFFF else None,
                flags,
                data_off,
            )

    def members(self, descriptor: str):
        """Return declared fields and methods (not inherited Java members)."""
        _, _, offset = self.classes[descriptor]
        if not offset:
            return [], []
        sizes = []
        for _ in range(4):
            n, offset = uleb(self.data, offset)
            sizes.append(n)

        fields = []
        for size in sizes[:2]:
            index = 0
            for _ in range(size):
                delta, offset = uleb(self.data, offset)
                index += delta
                flags, offset = uleb(self.data, offset)
                fields.append((*self.fields[index], flags))

        methods = []
        for size in sizes[2:]:
            index = 0
            for _ in range(size):
                delta, offset = uleb(self.data, offset)
                index += delta
                flags, offset = uleb(self.data, offset)
                code, offset = uleb(self.data, offset)
                methods.append((*self.methods[index], flags, code))
        return fields, methods


def main():
    if not APK.is_file():
        raise SystemExit(f"Original Quest APK unavailable for DEX audit: {APK}")
    with zipfile.ZipFile(APK) as archive:
        dex_files = [name for name in archive.namelist()
                     if name.startswith("classes") and name.endswith(".dex")]
        if not dex_files:
            raise AssertionError("APK is missing classes.dex")
        candidates = [Dex(archive.read(name)) for name in dex_files]

    for descriptor in (TARGET, UNITY_PLAYER):
        matching = [dex for dex in candidates if descriptor in dex.classes]
        assert len(matching) == 1, f"missing/duplicate original DEX class: {descriptor}"
        dex = matching[0]
        superclass, flags, _ = dex.classes[descriptor]
        fields, methods = dex.members(descriptor)
        print(f"DEX {descriptor}: extends {superclass}; access=0x{flags:x}, "
              f"declares {len(fields)} fields and {len(methods)} methods")
        if descriptor == TARGET:
            print("  Declared field names:", ", ".join(field[2] for field in fields))
            print("  Native methods:", ", ".join(method[2] for method in methods
                                                if method[3] & 0x100))
        if descriptor == UNITY_PLAYER:
            assert any(method[2] == "<init>" for method in methods)
    print("PASS: original Unity Java descriptor and declared-method DEX inventory")


if __name__ == "__main__":
    main()
