"""Inspect real Quest PT_TLS layouts and AArch64 TLSDESC relocations.

Run: py -3 tests/test_guest_tls_elf.py [path/to/arm64-v8a]
This is an independent ELF audit, not a claim of hardware TLS execution.
"""

import struct
import sys
from pathlib import Path

from test_so_util_elf_audit import decode_aps2, decode_relr, span

PT_TLS = 7
PT_LOAD = 1
STT_TLS = 6
R_AARCH64_TLSDESC = 1031
SHT_RELR = 19
ANDROID_RELR = 0x6FFFFF00
TLS_FIRST_OFFSET = 0x100
TP_OFFSET = 0x200
BIONIC_TLS_SIZE = 0x4000
MIN_TCB_GUARD_END = 0x30


def inspect(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError(f"{path}: not an AArch64 ELF64")
    phoff, shoff = struct.unpack_from("<QQ", data, 32)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 54)
    assert phentsize == 56 and shentsize == 64
    span(data, phoff, phnum * phentsize)
    span(data, shoff, shnum * shentsize)
    tls = []
    loads = []
    for i in range(phnum):
        typ, _, fileoff, vaddr, _, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize)
        if typ == PT_LOAD:
            span(data, fileoff, filesz)
            assert filesz <= memsz
            loads.append((vaddr, vaddr + memsz))
        if typ == PT_TLS:
            init = bytes(span(data, fileoff, filesz))
            assert filesz <= memsz and align >= 1 and align & (align - 1) == 0
            tls.append((vaddr, filesz, memsz, align, init))
    assert loads
    for vaddr, filesz, memsz, align, _ in tls:
        assert align <= 16, (path.name, align, "unsupported 16-byte guest TP alignment")
        # The Quest binaries put some zero-only PT_TLS virtual addresses
        # outside PT_LOAD; that is legitimate because no .tdata is copied.
        if filesz:
            assert any(lo <= vaddr and vaddr + filesz <= hi for lo, hi in loads)

    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    symtab = next(s for s in sections if s[1] == 11)
    symdata = span(data, symtab[4], symtab[5])
    strtab = sections[symtab[6]]
    strings = bytes(span(data, strtab[4], strtab[5]))
    assert len(symdata) % 24 == 0
    symbols = []
    for sym in struct.iter_unpack("<IBBHQQ", symdata):
        name_offset, info, _, section, value, size = sym
        end = strings.index(0, name_offset)
        symbols.append((strings[name_offset:end].decode(errors="replace"),
                        info & 15, section, value, size))

    relocs = []
    relr = []
    for section in sections:
        if section[1] in (SHT_RELR, ANDROID_RELR):
            relr.extend(decode_relr(span(data, section[4], section[5])))
            continue
        if section[1] == 4:
            entries = struct.iter_unpack("<QQq", span(data, section[4], section[5]))
        elif section[1] == 0x60000002:
            entries = decode_aps2(span(data, section[4], section[5]))
        else:
            continue
        for target, info, addend in entries:
            if info & 0xFFFFFFFF == R_AARCH64_TLSDESC:
                assert info >> 32 < len(symbols)
                assert target % 8 == 0
                assert any(lo <= target and target + 16 <= hi for lo, hi in loads)
                sym = symbols[info >> 32]
                relocs.append((target, sym, addend))
    return tls, relocs, relr


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        __file__).resolve().parents[1] / "import/quest-engine/APK/lib/arm64-v8a"
    expected = {"libopenxr_loader.so": (0, 0x30, 8, 3),
                "libovrplatformloader.so": (0, 0x10, 8, 1),
                "libOVRPlugin.so": (8, 0x70, 8, 11)}
    # Prime's runtime integration can validate these exact mapped GOT
    # addresses with bs_guest_tls_validate_original_desc(module, vaddr, off).
    runtime_witnesses = {
        "libopenxr_loader.so": (0x75738, 24),
        "libOVRPlugin.so": (0x5098C8, 8),
        "libovrplatformloader.so": (0x5F070, 0),
    }
    found = set()
    tls_by_name = {}
    for path in sorted(root.glob("*.so")):
        tls, relocs, relr = inspect(path)
        if not tls and not relocs:
            continue
        assert len(tls) == 1, (path.name, tls)
        vaddr, filesz, memsz, align, initialized = tls[0]
        assert (filesz, memsz, align, len(relocs)) == expected[path.name]
        assert len(initialized) == filesz
        tls_by_name[path.name] = (memsz, align, relocs)
        assert any(target == runtime_witnesses[path.name][0] and
                   addend == runtime_witnesses[path.name][1]
                   for target, _, addend in relocs), (
                       path.name, runtime_witnesses[path.name])
        if path.name == "libOVRPlugin.so":
            assert vaddr in relr, "relocated .tdata must be captured AFTER RELR"
        found.add(path.name)
        print(f"{path.name}: PT_TLS filesz={filesz}, memsz={memsz}, align={align}; "
              f"{len(relocs)} TLSDESC")
        for target, sym, addend in relocs:
            print(f"  @ {target:#x}: {sym[0] or '<anonymous>'} "
                  f"type={sym[1]} shndx={sym[2]} tls_offset={sym[3]:#x} "
                  f"addend={addend}")
            # The supplied Android APS2 entries use STN_UNDEF (symbol 0)
            # plus a TLS-offset RELA addend for module-local descriptors.
            assert sym[1] in (STT_TLS, 0), (path.name, sym)
            # The shipped APS2 TLSDESCs refer to STN_UNDEF, whose addends
            # are offsets into the caller's own PT_TLS module.
            assert sym[0] == "" and sym[1] == 0 and sym[2] == 0
            assert addend >= 0 and addend < memsz
    assert found == set(expected), found
    # The actual loader stages libopenxr_loader, libOVRPlugin and then
    # libovrplatformloader. Model its statically assigned TP-relative slots,
    # checking that descriptor values cannot overlap Bionic's stack guard.
    next_offset = TLS_FIRST_OFFSET
    total_descriptors = 0
    for name in ("libopenxr_loader.so", "libOVRPlugin.so",
                 "libovrplatformloader.so"):
        memsz, align, relocs = tls_by_name[name]
        next_offset = (next_offset + align - 1) & ~(align - 1)
        assert next_offset >= MIN_TCB_GUARD_END
        assert TP_OFFSET + next_offset + memsz <= BIONIC_TLS_SIZE
        for _, _, addend in relocs:
            argument = next_offset + addend
            assert next_offset <= argument < next_offset + memsz
            total_descriptors += 1
        next_offset += memsz
    assert total_descriptors == 15
    print("PASS: 15 original AArch64 TLS descriptors in 3 PT_TLS modules")


if __name__ == "__main__":
    main()
