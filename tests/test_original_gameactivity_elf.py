"""Read-only verification of the supplied Quest 1.46 GameActivity call graph.

Parses the actual ARM64 ELF, symbol table and JUMP_SLOT relocations. This is
independent of the mocked runtime preflight test and never executes guest code.
"""

from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parents[1]
ELF = ROOT / "import/quest-engine/APK/lib/arm64-v8a/libgame.so"


def parse_elf(data):
    assert data[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64"
    assert struct.unpack_from("<H", data, 18)[0] == 183, "expected ARM64"
    phoff, shoff = struct.unpack_from("<QQ", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    shentsize, shnum = struct.unpack_from("<HH", data, 58)
    assert phentsize == 56 and shentsize == 64
    assert phoff + phnum * phentsize <= len(data)
    assert shoff + shnum * shentsize <= len(data)
    programs = [struct.unpack_from("<IIQQQQQQ", data, phoff + 56 * i)
                for i in range(phnum)]
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + 64 * i)
                for i in range(shnum)]

    def offset_of(vaddr, length):
        for typ, _, offset, start, _, filesz, _, _ in programs:
            if typ == 1 and start <= vaddr and length <= filesz - (vaddr - start):
                location = offset + (vaddr - start)
                assert location + length <= len(data)
                return location
        raise AssertionError(f"0x{vaddr:x}: range absent from PT_LOAD file image")

    symbols = {}
    by_index = {}
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        offset, size, link, entsize = section[4], section[5], section[6], section[9]
        assert entsize == 24 and size % 24 == 0 and link < len(sections)
        strsec = sections[link]
        assert strsec[4] + strsec[5] <= len(data)
        strtab = data[strsec[4]:strsec[4] + strsec[5]]
        for j in range(size // 24):
            nameoff, info, _, shndx, value, symsize = struct.unpack_from(
                "<IBBHQQ", data, offset + j * 24)
            assert nameoff < len(strtab)
            end = strtab.find(b"\0", nameoff)
            assert end >= 0
            name = strtab[nameoff:end].decode("utf-8")
            by_index[j] = name
            if name and shndx:
                symbols[name] = (value, symsize, info)

    relocations = {}
    for section in sections:
        if section[1] != 4:  # SHT_RELA
            continue
        offset, size, entsize = section[4], section[5], section[9]
        assert entsize == 24 and size % 24 == 0 and offset + size <= len(data)
        for j in range(size // 24):
            where, info, addend = struct.unpack_from("<QQq", data,
                                                      offset + j * 24)
            relocations[(where, info & 0xffffffff)] = (
                by_index.get(info >> 32), addend)
    return offset_of, symbols, relocations


def main():
    data = ELF.read_bytes()
    assert len(data) == 152576
    assert zlib.crc32(data) & 0xffffffff == 0x41828fc1
    offset_of, symbols, relocations = parse_elf(data)

    for name, vaddr, length, checksum in (
        ("JNI_OnLoad", 0x1D4CC, 120, 0xC013F770),
        ("GameActivity_register", 0x1BD2C, 4296, 0x3E404D70),
    ):
        assert name in symbols, f"missing original {name} export"
        assert symbols[name][:2] == (vaddr, length)
        assert zlib.crc32(data[offset_of(vaddr, length):
                               offset_of(vaddr, length) + length]) & 0xffffffff == checksum

    # JNI_OnLoad calls the original GameActivity_register via a local PLT.
    callsite = 0x1D518
    word = struct.unpack_from("<I", data, offset_of(callsite, 4))[0]
    assert word & 0xFC000000 == 0x94000000, "expected ARM64 BL"
    displacement = word & 0x3FFFFFF
    if displacement & (1 << 25):
        displacement -= 1 << 26
    assert callsite + displacement * 4 == 0x27210

    assert relocations[(0x2C5B8, 1026)] == ("GameActivity_register", 0)
    assert relocations[(0x2C680, 1026)] == ("__android_log_print", 0)

    # Recomputed from exact mapped instructions; no guest symbols are called.
    start = offset_of(0x1BD2C, 4296)
    instructions = struct.unpack_from("<1074I", data, start)
    # A64 LDR Xt,[x8,#imm12*8] includes the JNI method/field lookups.
    table_reads = {}
    for opcode in instructions:
        if opcode & 0xFFC003E0 == 0xF9400100:
            slot = (opcode >> 10) & 0xFFF
            table_reads[slot] = table_reads.get(slot, 0) + 1
    assert table_reads.get(94, 0) >= 1, "original GetFieldID dispatch absent"
    assert table_reads.get(113, 0) >= 1, "original GetStaticMethodID absent"

    changed = bytearray(data)
    changed[offset_of(0x1BD2C, 1)] ^= 1
    assert zlib.crc32(changed) & 0xffffffff != 0x41828FC1
    print("PASS: original libgame ABI, direct BL, both GOT relocations, JNI field lookups and source pin")


if __name__ == "__main__":
    main()
