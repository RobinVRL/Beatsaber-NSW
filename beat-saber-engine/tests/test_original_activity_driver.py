"""Host-only, source-backed regression for the original Quest Activity driver.

This verifies the native start path in the owner-staged Quest 1.46 libgame.so:
GameActivity initializeNativeCode -> NativeCode setup -> onCreate worker ->
android_main -> Unity MainLoop, plus the native lifecycle callback dispatch.

The script reads ELF bytes and relocation metadata. It does not call ARM64
guest functions, instantiate Android Java objects, or claim Java readiness.
Run from any directory: python tests/test_original_activity_driver.py
"""

from pathlib import Path
import struct
import zlib

from test_original_gameactivity_elf import parse_elf


ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT.parent / "import/quest-engine/APK/lib/arm64-v8a/libgame.so"


def word(data, offset_of, address):
    return struct.unpack_from("<I", data, offset_of(address, 4))[0]


def branch_target(data, offset_of, at, *, linked):
    instruction = word(data, offset_of, at)
    mask = 0xFC000000
    expected = 0x94000000 if linked else 0x14000000
    assert instruction & mask == expected, (
        f"0x{at:x}: expected {'BL' if linked else 'B'}, got 0x{instruction:08x}"
    )
    displacement = instruction & 0x03FFFFFF
    if displacement & 0x02000000:
        displacement -= 0x04000000
    return at + displacement * 4


def expect_call(data, offset_of, at, target):
    assert branch_target(data, offset_of, at, linked=True) == target, (
        f"0x{at:x}: original call target changed"
    )


def original_native_pointer(relocations, address):
    # Each native function in the original 24-entry JNINativeMethod table is
    # materialized by R_AARCH64_RELATIVE, whose addend is the function ELF VA.
    symbol, value = relocations[(address, 1027)]
    assert symbol in (None, "")
    return value


def test_original_driver():
    original = GAME.read_bytes()
    assert len(original) == 152576
    assert zlib.crc32(original) & 0xFFFFFFFF == 0x41828FC1
    offset_of, symbols, relocations = parse_elf(original)

    for name, expected in (
        ("JNI_OnLoad", 0x1D4CC),
        ("GameActivity_register", 0x1BD2C),
        ("Java_com_google_androidgamesdk_GameActivity_initializeNativeCode", 0x1CDF4),
        ("GameActivity_onCreate", 0x20708),
        ("android_main", 0x196D0),
    ):
        assert symbols[name][0] == expected, f"{name} export moved"

    # JNI_OnLoad only registers native methods. The exported Java_* wrapper
    # also registers, then tail-branches to the internal NativeCode creator.
    # The first JNINativeMethod points to the internal function, not wrapper.
    expect_call(original, offset_of, 0x1D518, 0x27210)  # register@plt
    expect_call(original, offset_of, 0x1CE2C, 0x27210)  # register@plt
    assert branch_target(original, offset_of, 0x1CE64,
                         linked=False) == 0x1CE68
    assert original_native_pointer(relocations, 0x2B840 + 16) == 0x1CE68

    # Internal NativeCode creation uses the original C++ allocation, looper,
    # pipe and poll registration. Its existence does not construct Java.
    for site, target in (
        (0x1CEB8, 0x268D0),  # operator new@plt
        (0x1CF54, 0x27110),  # ALooper_forThread@plt
        (0x1CF60, 0x27150),  # ALooper_acquire@plt
        (0x1CF68, 0x27420),  # pipe@plt
        (0x1D0E0, 0x270B0),  # ALooper_addFd@plt
    ):
        expect_call(original, offset_of, site, target)

    # Original onStart/onResume/onPause/onStop native methods are trampolines
    # through NativeCode's callback slots (+80, +88, +104, +112). They cannot
    # be treated as proof that Java dispatched Activity lifecycle events.
    for index, entry, load_slot in (
        (3, 0x1D750, 0xF9402841),
        (4, 0x1D768, 0xF9402C41),
        (6, 0x1D7F4, 0xF9403441),
        (7, 0x1D80C, 0xF9403841),
    ):
        assert original_native_pointer(relocations,
                                       0x2B840 + index * 24 + 16) == entry
        assert word(original, offset_of, entry + 4) == load_slot
        assert word(original, offset_of, entry + 16) == 0xD61F0020  # BR X1

    # GameActivity_onCreate creates a 384-byte android_app and starts a
    # native worker, whose entrypoint is passed to pthread_create in X2.
    assert word(original, offset_of, 0x207F8) == 0x52803000  # MOV W0,#384
    expect_call(original, offset_of, 0x20810, 0x26A70)  # malloc@plt
    expect_call(original, offset_of, 0x20894, 0x27420)  # pipe@plt
    assert word(original, offset_of, 0x208FC) == 0xB0000002  # ADRP X2,+page
    assert word(original, offset_of, 0x20904) == 0x91206042  # ADD X2,X2,#0x818
    assert ((0x208FC & ~0xFFF) + 0x1000 + 0x818) == 0x21818
    expect_call(original, offset_of, 0x20910, 0x27580)  # pthread_create@plt

    # The actual worker owns the ALooper and reaches the original android_main
    # via PLT; only android_main calls the original Unity MainLoop.
    for site, target in (
        (0x21840, 0x27130),  # AConfiguration_new@plt
        (0x21850, 0x27170),  # fromAssetManager@plt
        (0x218D0, 0x27160),  # ALooper_prepare@plt
        (0x218EC, 0x270B0),  # ALooper_addFd@plt
        (0x2191C, 0x26F30),  # android_main@plt
        (0x21A34, 0x274E0),  # android_app_pre_exec_cmd@plt
        (0x21A54, 0x274D0),  # android_app_post_exec_cmd@plt
        (0x19744, 0x26F00),  # Unity MainLoop(android_app*)@plt
        (0x19794, 0x26B70),  # ALooper_pollOnce@plt
    ):
        expect_call(original, offset_of, site, target)

    # Sanity check: changing a call opcode must invalidate the graph check.
    changed = bytearray(original)
    changed[offset_of(0x2191C, 1)] ^= 1
    try:
        expect_call(changed, offset_of, 0x2191C, 0x26F30)
    except AssertionError:
        pass
    else:
        raise AssertionError("driver regression did not detect changed call")


if __name__ == "__main__":
    test_original_driver()
    print("PASS: original native driver startup/callback call graph; guest code not executed")
