"""Static original-ELF regression for one in-memory OpenSSL timer instruction.

Parses the owner-staged ARM64 libcrypto.so, pins the exact source SHA256 and
original constructor branch, and models the strict patch preconditions in a
disposable PT_LOAD image. Never modifies either source ELF or executes code.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT.parent / "import/quest-engine/APK/lib/arm64-v8a/libcrypto.so"
STAGED = ROOT.parent / "private-sd/switch/beat-saber-quest/libcrypto.so"
SOURCE = ROOT / "source/openssl_timer_compat.c"
HEADER = ROOT / "source/openssl_timer_compat.h"

EXPECTED_SHA256 = bytes.fromhex(
    "0c68642b32b4d149695b09cdee88427d"
    "136e684de779ab7758dde6742fca9a88"
)
CONTEXT_VA = 0xF8B38
TARGET_VA = 0xF8B48
PROBE_VA = 0xF8DB8
CALL_VA = 0xF8DCC
VIRTUAL_OPCODE = struct.pack("<I", 0xD53BE040)
PHYSICAL_OPCODE = struct.pack("<I", 0xD53BE020)


def array_in_source(name: str) -> bytes:
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        rf"static const uint8_t {name}\[[^]]*\] = \{{(.*?)\}};",
        source,
        re.S,
    )
    assert match, f"missing byte array {name}"
    region = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.S)
    literals = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", region)]
    assert all(0 <= x < 256 for x in literals)
    return bytes(literals)


def parse_program_headers(data: bytes):
    assert data[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64"
    assert struct.unpack_from("<H", data, 18)[0] == 183, "expected AArch64"
    assert struct.unpack_from("<H", data, 16)[0] == 3, "expected ET_DYN"
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    assert phentsize == 56
    assert 0 < phnum <= 16 and phoff + phnum * phentsize <= len(data)
    programs = [
        struct.unpack_from("<IIQQQQQQ", data, phoff + 56 * i)
        for i in range(phnum)
    ]
    loads = [p for p in programs if p[0] == 1]
    assert loads
    for _, _, offset, vaddr, _, filesz, memsz, _ in loads:
        assert filesz <= memsz and offset + filesz <= len(data)
        assert vaddr + memsz < 0x10000000
    return loads


def file_offset(loads, va: int, length: int) -> int:
    for _, flags, offset, vaddr, _, filesz, _, _ in loads:
        if flags & 1 and vaddr <= va and length <= filesz - (va - vaddr):
            return offset + (va - vaddr)
    raise AssertionError(f"VA 0x{va:x} absent from executable PT_LOAD")


def loaded_image(data: bytes, loads) -> bytearray:
    size = (max(p[3] + p[6] for p in loads) + 0xFFF) & ~0xFFF
    image = bytearray(size)
    for _, _, offset, vaddr, _, filesz, _, _ in loads:
        image[vaddr:vaddr + filesz] = data[offset:offset + filesz]
    return image


def apply_contract(data: bytes, name: str, image: bytearray,
                   context: bytes, probe: bytes) -> bool:
    """Model the C function's fail-closed checks without writing original ELF."""
    if name not in ("libcrypto.so", "sdmc:/switch/beat-saber-quest/libcrypto.so"):
        return False
    if len(data) != 2368784 or hashlib.sha256(data).digest() != EXPECTED_SHA256:
        return False
    if image[CONTEXT_VA:CONTEXT_VA + len(context)] != context:
        return False
    if image[PROBE_VA:PROBE_VA + len(probe)] != probe:
        return False
    image[TARGET_VA:TARGET_VA + 4] = PHYSICAL_OPCODE
    return True


def main():
    data = ORIGINAL.read_bytes()
    assert STAGED.read_bytes() == data
    assert len(data) == 2368784
    assert hashlib.sha256(data).digest() == EXPECTED_SHA256
    assert array_in_source("original_sha256") == EXPECTED_SHA256
    assert array_in_source("physical_counter_opcode") == PHYSICAL_OPCODE

    loads = parse_program_headers(data)
    context = array_in_source("original_context")
    probe = array_in_source("original_probe")
    assert len(context) == 32 and len(probe) == 32
    assert data[file_offset(loads, CONTEXT_VA, 32):
                file_offset(loads, CONTEXT_VA, 32) + 32] == context
    assert data[file_offset(loads, PROBE_VA, 32):
                file_offset(loads, PROBE_VA, 32) + 32] == probe
    assert context[TARGET_VA - CONTEXT_VA:
                   TARGET_VA - CONTEXT_VA + 4] == VIRTUAL_OPCODE
    assert context[TARGET_VA - CONTEXT_VA + 4:
                   TARGET_VA - CONTEXT_VA + 8] == struct.pack("<I", 0xD65F03C0)

    # The real constructor calls this probe ONLY after sigsetjmp.
    call = struct.unpack_from("<I", data, file_offset(loads, CALL_VA, 4))[0]
    assert call & 0xFC000000 == 0x94000000, "expected original direct BL"
    immediate = call & 0x3FFFFFF
    if immediate & (1 << 25):
        immediate -= 1 << 26
    assert CALL_VA + immediate * 4 == TARGET_VA
    assert struct.unpack_from("<I", data, file_offset(loads, 0xF8DC4, 4))[0] == 0x94047DBF

    before = loaded_image(data, loads)
    output = before.copy()
    assert apply_contract(data, "libcrypto.so", output, context, probe)
    assert output[TARGET_VA:TARGET_VA + 4] == PHYSICAL_OPCODE
    changes = [i for i, (a, b) in enumerate(zip(before, output)) if a != b]
    assert changes == [TARGET_VA], "only opcode bit #5 should change"
    assert ORIGINAL.read_bytes() == data and STAGED.read_bytes() == data
    assert not apply_contract(data, "libcrypto.so", output, context, probe), (
        "second patch application must reject already patched instruction"
    )

    bad_file = bytearray(data)
    bad_file[0x400] ^= 1  # Original source identity pin must fail.
    altered = before.copy()
    assert not apply_contract(bad_file, "libcrypto.so", altered, context, probe)
    assert altered == before
    assert not apply_contract(data, "libssl.so", altered, context, probe)
    assert altered == before
    altered[CONTEXT_VA] ^= 1  # Context mismatch must fail without a write.
    snapshot = altered.copy()
    assert not apply_contract(data, "libcrypto.so", altered, context, probe)
    assert altered == snapshot
    altered = before.copy()
    altered[PROBE_VA + 20] ^= 1  # Original branch to probe absent.
    snapshot = altered.copy()
    assert not apply_contract(data, "libcrypto.so", altered, context, probe)
    assert altered == snapshot

    c_text = SOURCE.read_text(encoding="utf-8")
    h_text = HEADER.read_text(encoding="utf-8")
    for marker in (
        "sha256CalculateHash(actual_hash, module->so_base, module->so_size)",
        "memcmp(staged + BS_CRYPTO_CONTEXT_VA",
        "memcmp(staged + BS_CRYPTO_PROBE_VA",
        "memcpy(staged + BS_CRYPTO_OPCODE_VA, physical_counter_opcode,",
        "original_executable_range(module, BS_CRYPTO_CONTEXT_VA",
    ):
        assert marker in c_text, f"missing C safety check: {marker}"
    assert "so_patch_code(" not in c_text
    assert "bs_openssl_timer_compat_patch(so_module *module)" in h_text
    print("PASS: original libcrypto SHA256/ELF, ctor sigsetjmp-to-MRS BL, "
          "single-byte opcode delta, fail-closed mismatch checks, no file edits")


if __name__ == "__main__":
    main()
