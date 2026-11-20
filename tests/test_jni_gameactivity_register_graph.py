"""Pin the exact ORIGINAL Quest GameActivity registration-only JNI call graph.

Disassembles AArch64 JNI slot LDR instructions directly from the staged ELF,
decodes all 24 R_AARCH64_RELATIVE JNINativeMethod table records, and checks
them against the bridge's independent C declaration inventory. Read-only;
does not run libgame or Java application code.
"""

from pathlib import Path
import re
import struct
import zlib

from test_original_gameactivity_elf import parse_elf


ENGINE = Path(__file__).resolve().parents[1]
GAME = ENGINE / "import/quest-engine/APK/lib/arm64-v8a/libgame.so"
BRIDGE = ENGINE / "source/jni_bridge.c"
GAME_CRC = 0x41828FC1


def read_word(data, offset_of, vaddr):
    return struct.unpack_from("<I", data, offset_of(vaddr, 4))[0]


def branch_target(data, offset_of, vaddr):
    word = read_word(data, offset_of, vaddr)
    assert word & 0xFC000000 == 0x94000000, (
        f"0x{vaddr:x}: expected direct BL, got 0x{word:08x}")
    displacement = word & ((1 << 26) - 1)
    if displacement & (1 << 25):
        displacement -= 1 << 26
    return vaddr + displacement * 4


def jni_slots(data, offset_of, start, length):
    assert length % 4 == 0
    words = struct.unpack_from("<" + "I" * (length // 4),
                               data, offset_of(start, length))
    slots = {}
    # JNI sequence ldr x8, [x8, #slot * sizeof(void*)] / blr x8.
    # Inspect next 10 opcodes to ensure a real indirect dispatch follows.
    for i, word in enumerate(words):
        if word & 0xFFC003E0 == 0xF9400100:
            slot = (word >> 10) & 0xFFF
            if 0xD63F0100 in words[i + 1:i + 11]:
                slots.setdefault(slot, []).append(start + 4 * i)
    return slots


def relocated_pointer(data, offset_of, relocations, vaddr):
    relocation = relocations.get((vaddr, 1027))  # R_AARCH64_RELATIVE
    assert relocation is not None and relocation[0] in (None, ""), (
        f"missing relative relocation for native entry at 0x{vaddr:x}")
    pointer = relocation[1]
    assert pointer > 0, f"invalid relocated pointer for 0x{vaddr:x}"
    return pointer


def c_string(data, offset_of, vaddr):
    start = offset_of(vaddr, 1)
    end = data.find(b"\0", start, start + 1024)
    assert end > start, f"invalid ELF C string at 0x{vaddr:x}"
    return data[start:end].decode("utf-8")


def bridge_inventory():
    text = BRIDGE.read_text(encoding="utf-8")
    members = re.search(
        r"static const BSJNIKnownMember g_gameactivity_members\[\] = \{(.*?)\n\};",
        text, re.S)
    assert members, "missing original GameActivity member metadata"
    count = len(re.findall(r"\bBS_(?:IM|SM|IF)\s*\(", members.group(1)))
    assert count == 77, f"incorrect JNI lookup inventory: {count}"

    native = re.search(
        r"static const BSJNIKnownNative g_gameactivity_natives\[\] = \{(.*?)\n\};",
        text, re.S)
    assert native, "missing original native declarations"
    declarations = re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}',
                              native.group(1))
    assert len(declarations) == 24, len(declarations)
    assert len(set(declarations)) == 24
    return declarations


def main():
    data = GAME.read_bytes()
    assert len(data) == 152576 and zlib.crc32(data) & 0xFFFFFFFF == GAME_CRC
    offset_of, symbols, relocations = parse_elf(data)
    for name, location, length, crc in (
        ("JNI_OnLoad", 0x1D4CC, 120, 0xC013F770),
        ("GameActivity_register", 0x1BD2C, 4296, 0x3E404D70),
        ("GameActivityEventsInit", 0x1FC88, 1624, 0xBDCCBD0B),
    ):
        assert symbols[name][:2] == (location, length)
        body = data[offset_of(location, length):
                    offset_of(location, length) + length]
        assert zlib.crc32(body) & 0xFFFFFFFF == crc, name

    # JNI_OnLoad obtains JNI_1_6 from JavaVM->GetEnv(slot 6) before
    # calling GameActivity_register through its original PLT.
    assert read_word(data, offset_of, 0x1D500) == 0xF9401908
    assert read_word(data, offset_of, 0x1D504) == 0xD63F0100
    assert branch_target(data, offset_of, 0x1D518) == 0x27210
    assert relocations[(0x2C5B8, 1026)] == ("GameActivity_register", 0)

    # GameActivityEventsInit is called during registration, not by a later
    # simulated Activity or by initializeNativeCode.
    assert branch_target(data, offset_of, 0x1C724) == 0x271D0
    assert read_word(data, offset_of, 0x1C754) == 0xF9435D08
    assert read_word(data, offset_of, 0x1C758) == 0x52800303  # w3 = 24
    assert read_word(data, offset_of, 0x1C764) == 0xD63F0100
    assert read_word(data, offset_of, 0x1C778) == 0xF9405D08  # DeleteLocalRef

    registry_slots = jni_slots(data, offset_of, 0x1BD2C, 4296)
    event_slots = jni_slots(data, offset_of, 0x1FC88, 1624)
    required = {6, 23, 33, 94, 113, 215}
    all_slots = set(registry_slots) | set(event_slots)
    assert required <= all_slots, (
        f"GameActivity JNI registration no longer uses expected slots: "
        f"{required - all_slots}")

    # Registration is metadata lookup + native pointer binding; no Java
    # method invocation, instance field evaluation or CreateObject calls.
    # Such a change would invalidate a registration-only boot gate.
    runtime_invocations = set(range(34, 94)) | set(range(95, 113)) | set(range(114, 215))
    unexpected = runtime_invocations & all_slots
    assert not unexpected, (
        f"GameActivity registration now invokes Java operations: {sorted(unexpected)}")

    actual = []
    for i in range(24):
        base = 0x2B840 + i * 24
        name = c_string(data, offset_of,
                        relocated_pointer(data, offset_of, relocations, base))
        sig = c_string(data, offset_of,
                       relocated_pointer(data, offset_of, relocations, base + 8))
        fn = relocated_pointer(data, offset_of, relocations, base + 16)
        assert 0x10000 <= fn < 0x27300, (
            f"native {name} points outside original libgame text: 0x{fn:x}")
        actual.append((name, sig))

    expected = bridge_inventory()
    assert actual == expected, (
        "actual ELF JNINativeMethod[24] differs from JNI bridge declarations")
    print("PASS: original Quest GameActivity register-only JNI_OnLoad, "
          "EventsInit, JNI slot graph, 77 lookups, all 24 relocated natives")


if __name__ == "__main__":
    main()
