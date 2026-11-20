"""Pinned original Quest 1.46 GameActivity / native_app_glue topology.

Reads actual staged ELF code and assets. Never executes original guest code.
Run: py -3 tests/test_unity_lifecycle_elf.py
"""
from pathlib import Path
import struct
import zlib

from test_original_gameactivity_elf import parse_elf


ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "import/quest-engine/APK"
GAME = APK / "lib/arm64-v8a/libgame.so"

# Original Quest libgame.so function metadata, not recreated Java behavior.
PINS = (
    ("JNI_OnLoad", 0x1D4CC, 120, 0xC013F770),
    ("GameActivity_register", 0x1BD2C, 4296, 0x3E404D70),
    ("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode",
     0x1CDF4, 116, 0x2DACC7EC),
    ("android_main", 0x196D0, 408, 0x0A884BBB),
    ("GameActivity_onCreate", 0x20708, 620, 0x3D137D92),
    ("GameActivityEventsInit", 0x1FC88, 1624, 0xBDCCBD0B),
    ("android_app_read_cmd", 0x202E0, 184, 0xADCA85B7),
    ("android_app_pre_exec_cmd", 0x20398, 236, 0x172BC294),
    ("android_app_post_exec_cmd", 0x20484, 156, 0x0B801613),
    ("android_app_write_cmd", 0x20524, 144, 0xED135930),
    ("android_app_swap_input_buffers", 0x205F0, 136, 0xD39E8A5E),
)


def instruction(data: bytes, offset_of, vaddr: int) -> int:
    return struct.unpack_from("<I", data, offset_of(vaddr, 4))[0]


def branch_target(data, offset_of, callsite, *, link: bool):
    word = instruction(data, offset_of, callsite)
    opcode = 0x94000000 if link else 0x14000000
    assert (word & 0xFC000000) == opcode, (hex(callsite), hex(word))
    displacement = word & 0x3FFFFFF
    if displacement & (1 << 25):
        displacement -= 1 << 26
    return callsite + displacement * 4


def main() -> None:
    data = GAME.read_bytes()
    assert len(data) == 152576
    assert zlib.crc32(data) & 0xFFFFFFFF == 0x41828FC1
    offset_of, symbols, relocations = parse_elf(data)

    for name, vaddr, length, checksum in PINS:
        assert name in symbols, name
        assert symbols[name][:2] == (vaddr, length), (name, symbols[name])
        span = data[offset_of(vaddr, length):offset_of(vaddr, length) + length]
        assert len(span) == length
        assert (zlib.crc32(span) & 0xFFFFFFFF) == checksum, name

    # JNI_OnLoad calls GameActivity_register through its real local PLT.
    assert branch_target(data, offset_of, 0x1D518, link=True) == 0x27210
    assert relocations[(0x2C5B8, 1026)] == ("GameActivity_register", 0)
    assert relocations[(0x2C680, 1026)] == ("__android_log_print", 0)

    # The 116-byte exported Java_...initializeNativeCode is a trampoline:
    # it registers JNI metadata and passes x0..x7 into an internal initializer.
    assert branch_target(data, offset_of, 0x1CE2C, link=True) == 0x27210
    assert branch_target(data, offset_of, 0x1CE64, link=False) == 0x1CE68
    # The internal initializer allocates a 632-byte native object and acquires
    # a thread looper/pipe. This is *not* callable with symbolic Java handles.
    assert instruction(data, offset_of, 0x1CE98) == 0x52804F00  # mov w0,#632
    assert branch_target(data, offset_of, 0x1CF54, link=True) == 0x27110
    assert branch_target(data, offset_of, 0x1CF68, link=True) == 0x27420

    # The real android_main runs MainLoop and reads destroyRequested from the
    # original 64-bit android_app layout. It cannot receive an arbitrary ptr.
    assert branch_target(data, offset_of, 0x19744, link=True) == 0x26F00
    assert instruction(data, offset_of, 0x19764) == 0xB9405E68  # app + 92

    boot_config = APK / "assets/bin/Data/boot.config"
    managers = APK / "assets/bin/Data/globalgamemanagers"
    assert boot_config.is_file() and boot_config.stat().st_size == 768
    assert managers.is_file() and managers.stat().st_size > 0
    print("PASS: all 11 ORIGINAL GameActivity/Unity/native-app-glue symbols, "
          "bytes, JNI PLT, Java initializer, android_main and staged assets")


if __name__ == "__main__":
    main()
