"""Pin the original Unity long Java class-name allocation / JNI call graph.

This inspects the staged Quest ELF and performs no emulation or source edits.
The assembly encodings are evidence for the read-only snapshot decoder in
unity_string_probe.[ch], not evidence that Unity's allocator works in Eden.
"""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
LIBUNITY = ROOT.parent / "import/quest-engine/APK/lib/arm64-v8a/libunity.so"
STAGED = ROOT.parent / "private-sd/switch/beat-saber-quest/libunity.so"


def main():
    elf = LIBUNITY.read_bytes()
    assert STAGED.read_bytes() == elf
    assert elf[:6] == b"\x7fELF\x02\x01"
    assert struct.unpack_from("<H", elf, 18)[0] == 183
    phoff = struct.unpack_from("<Q", elf, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", elf, 54)
    assert phentsize == 56 and 0 < phnum <= 16
    assert phoff + phnum * phentsize <= len(elf)
    phdr = [struct.unpack_from("<IIQQQQQQ", elf, phoff + 56 * i)
            for i in range(phnum)]

    def off(vaddr: int, n: int) -> int:
        for typ, flags, offset, va, _, filesz, _, _ in phdr:
            if typ == 1 and va <= vaddr and n <= filesz - (vaddr - va):
                result = offset + vaddr - va
                assert result + n <= len(elf)
                return result
        raise AssertionError(f"unmapped VA 0x{vaddr:x}")

    def word(va: int) -> int:
        return struct.unpack_from("<I", elf, off(va, 4))[0]

    def branch(source: int, target: int):
        opcode = word(source)
        assert (opcode & 0xfc000000) == 0x94000000, hex(source)
        displacement = opcode & 0x03ffffff
        if displacement >= (1 << 25):
            displacement -= 1 << 26
        assert source + displacement * 4 == target, (
            f"BL 0x{source:x} does not target 0x{target:x}"
        )

    first = elf[off(0x166160, 31):off(0x166160, 31) + 31]
    second = elf[off(0x16617f, 51):off(0x16617f, 51) + 51]
    assert first == b"com/unity3d/player/UnityPlayer\x00"
    assert second == b"com/unity3d/player/UnityPlayerForActivityOrService\x00"
    assert len(first) - 1 == 30 and len(second) - 1 == 50

    # JNI_OnLoad second class reads relocation slot 0x1754158.
    assert word(0x9115c0) == 0xf940aed4
    assert word(0x9115cc) == 0xaa1403e0
    assert word(0x9115d0) == 0x39007fe8
    branch(0x9115e4, 0x5c6b8c)
    branch(0x9115ec, 0x5d6e2c)
    branch(0x5c6d50, 0xd5f3ec)
    branch(0x5c6d60, 0x16ef920)
    branch(0xd5f450, 0xd5fbac)
    branch(0xd5fbe8, 0x16a8788)

    # A NULL allocator result should fault at this store before FindClass.
    assert word(0x5c6d64) == 0x38356aff
    assert word(0x5c6d70) == 0xa9005277  # heap pointer,capacity
    assert word(0x5c6d74) == 0xf9000a75  # old length
    assert word(0x5c6d78) == 0x33001509  # long tag
    assert word(0x5c6c3c) == 0x38336adf  # NUL after appended bytes
    assert word(0x5c6c5c) == 0xf9000a93  # final length

    # Unity uses manager at VA 0x184cba0 and allocator vtable offset +16.
    assert word(0xd5f408) == 0xf945d328
    assert word(0xd5f788) == 0xf9401300
    assert word(0xd5f7a4) == 0xf9400903
    assert word(0x5d6e40) == 0x39407e68
    assert word(0x5d6e4c) == 0xf940026a
    assert word(0x5d6e58) == 0x9a8a3261
    assert word(0x5d6e5c) == 0xd63f0100

    source = (ROOT / "source/unity_string_probe.c").read_text()
    assert "bytes[31]" in source
    assert "memcpy(&pointer, bytes, sizeof pointer)" in source
    assert "BS_UNITY_STRING_LONG_NULL" in source
    assert "unity_string_probe.h" not in (ROOT / "source/main.c").read_text()
    print("PASS: original Unity heap allocator, once state, indirect dispatch, "
          "JNI FindClass and isolated probe")


if __name__ == "__main__":
    main()
