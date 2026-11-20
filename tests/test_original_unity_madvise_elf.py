"""Read-only check of the original Quest Unity allocator's madvise calls.

Run with: py -3 tests/test_original_unity_madvise_elf.py

This checks the precise AArch64 mmap/madvise/mprotect call sequence
responsible for the original Unity pool's virtual-memory lifetime.
Behavioral preservation/ownership checks run separately in
test_bionic_madvise_regression.c against the actual bs_madvise.
"""

from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
LIBUNITY = ROOT.parent / "import/quest-engine/APK/lib/arm64-v8a/libunity.so"
STAGED = ROOT.parent / "private-sd/switch/beat-saber-quest/libunity.so"


def main() -> None:
    original = LIBUNITY.read_bytes()
    assert original == STAGED.read_bytes(), "staged original ELF differs"
    assert original[:6] == b"\x7fELF\x02\x01", "expected little-endian ELF64"
    assert struct.unpack_from("<H", original, 18)[0] == 183, "expected ARM64"

    program_header_offset = struct.unpack_from("<Q", original, 32)[0]
    header_size, count = struct.unpack_from("<HH", original, 54)
    assert header_size == 56 and 0 < count <= 16
    assert program_header_offset + header_size * count <= len(original)
    segments = [
        struct.unpack_from("<IIQQQQQQ", original,
                           program_header_offset + i * header_size)
        for i in range(count)
    ]

    def instruction(pc: int) -> int:
        for kind, flags, file_offset, vaddr, _, file_size, _, _ in segments:
            if kind == 1 and (flags & 1) and vaddr <= pc and pc + 4 <= vaddr + file_size:
                return struct.unpack_from("<I", original, file_offset + pc - vaddr)[0]
        raise AssertionError(f"0x{pc:x} absent from executable original PT_LOAD")

    def branch(pc: int, target: int) -> None:
        opcode = instruction(pc)
        assert (opcode & 0xFC000000) == 0x94000000
        signed_imm = opcode & 0x03FFFFFF
        if signed_imm & 0x02000000:
            signed_imm -= 0x04000000
        assert pc + signed_imm * 4 == target, f"unexpected BL at 0x{pc:x}"

    # The original Unity backend enters its VM helper with flags/prot=0.
    # Follow this call to prove that w24, passed as mmap protection, is 0.
    assert instruction(0xD60050) == 0x2A1F03E3  # w3 = 0
    assert instruction(0xD60054) == 0x2A1F03E4  # w4 = PROT_NONE
    branch(0xD60080, 0x12BCF1C)
    assert instruction(0x12BCF3C) == 0x2A0403F8  # w24 = w4

    # Unity's VM reserve call: mmap(NULL, bytes, PROT_NONE, 0x22, -1, 0).
    assert instruction(0x12BD164) == 0xAA1F03E0  # x0 = NULL
    assert instruction(0x12BD168) == 0xAA1903E1  # x1 = length
    assert instruction(0x12BD16C) == 0x2A1803E2  # w2 = protection flags
    assert instruction(0x12BD170) == 0x52800443  # w3 = 0x22
    assert instruction(0x12BD174) == 0x12800004  # w4 = -1
    assert instruction(0x12BD178) == 0xAA1F03E5  # x5 = 0
    branch(0x12BD17C, 0x16F0050)  # mmap@plt

    # Unity passes MADV_NORMAL == 0 explicitly in the commit path.
    assert instruction(0x12BD61C) == 0xAA1503E0  # x0 = memory address
    assert instruction(0x12BD620) == 0xAA1603E1  # x1 = length
    assert instruction(0x12BD624) == 0x2A1F03E2  # w2 = 0 (MADV_NORMAL)
    branch(0x12BD628, 0x16F0ED0)  # madvise@plt
    assert instruction(0x12BD6C4) == 0x2A1403E2  # w2 = protection flags
    branch(0x12BD6C8, 0x16F0EE0)  # mprotect@plt

    # An independent Unity path requests MADV_DONTNEED == 4. The bridge
    # must preserve its separate content-discard semantics.
    assert instruction(0x12BD57C) == 0x52800082  # w2 = 4 (MADV_DONTNEED)
    branch(0x12BD580, 0x16F0ED0)  # madvise@plt

    print("PASS: original Unity MADV_NORMAL(0) and DONTNEED(4) calls, "
          "VM reservation and mprotect graph")


if __name__ == "__main__":
    main()
