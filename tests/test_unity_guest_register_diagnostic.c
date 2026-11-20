#include "unity_guest_register_diagnostic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OBJECT_BASE = 0x1000, TEXT_BASE = 0x2000 };
typedef struct GuestMemory {
    unsigned char object[BS_UNITY_DIAG_OBJECT_BYTES];
    unsigned char text[BS_UNITY_DIAG_CLASS_BYTES];
    unsigned calls;
    size_t max_read;
    bool deny_text;
} GuestMemory;

static bool read_guest(void *user, uint64_t address, void *out, size_t len) {
    GuestMemory *m = user;
    assert(len <= BS_UNITY_DIAG_CLASS_BYTES);
    ++m->calls;
    if (len > m->max_read) m->max_read = len;
    if (address == OBJECT_BASE && len == sizeof m->object) {
        memcpy(out, m->object, len);
        return true;
    }
    if (address == TEXT_BASE && len == sizeof m->text && !m->deny_text) {
        memcpy(out, m->text, len);
        return true;
    }
    return false;
}

static void u64_at(unsigned char *buffer, size_t offset, uint64_t value) {
    memcpy(buffer + offset, &value, sizeof value);
}

int main(void) {
    GuestMemory memory = {0};
    const char *klass =
        "com/unity3d/player/UnityPlayerForActivityOrService";
    assert(strlen(klass) == BS_UNITY_DIAG_CLASS_CHARS);
    memcpy(memory.text, klass, sizeof memory.text);
    u64_at(memory.object, 0, TEXT_BASE);
    u64_at(memory.object, 8, 50);
    u64_at(memory.object, 16, 50);
    memory.object[31] = 0x5f;

    BsUnityGuestRegisters registers = {0};
    BsUnityGuestDiagnostic diag = {0};

    registers.elf_pc = BS_UNITY_DIAG_ALLOC_RETURN;
    registers.x[0] = TEXT_BASE;
    registers.x[19] = OBJECT_BASE;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_ALLOCATOR_RETURN);
    assert(diag.x0 == TEXT_BASE && diag.x19 == OBJECT_BASE);
    assert(memory.calls == 1 && !diag.class_bytes_read);

    registers.elf_pc = BS_UNITY_DIAG_APPEND_RETURN;
    registers.sp = OBJECT_BASE;
    memory.calls = 0;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_VALID_CLASS);
    assert(diag.string_object_address == OBJECT_BASE);
    assert(diag.decoded.cstr_address == TEXT_BASE);
    assert(diag.decoded.size == 50 && diag.class_bytes_read);
    assert(memory.calls == 2 && memory.max_read == 51);

    registers.elf_pc = BS_UNITY_DIAG_PRE_SELECT;
    registers.x[10] = TEXT_BASE;
    registers.x[1] = 0; /* This is NOT the final argument yet. */
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_VALID_CLASS);
    assert(!diag.x1_is_findclass_argument && diag.x1 == 0);

    registers.elf_pc = BS_UNITY_DIAG_FINDCLASS_CALL;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_POINTER_MISMATCH);
    assert(diag.x1_is_findclass_argument && diag.x1 == 0);

    registers.x[1] = TEXT_BASE;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_VALID_CLASS);
    assert(diag.x1_is_findclass_argument);

    u64_at(memory.object, 0, 0);
    memory.calls = 0;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_LONG_NULL);
    assert(memory.calls == 1);

    u64_at(memory.object, 0, TEXT_BASE);
    u64_at(memory.object, 16, UINT64_MAX);
    memory.calls = 0;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_BAD_LENGTH);
    assert(memory.calls == 1); /* No unbounded length/heap reads. */

    u64_at(memory.object, 16, 50);
    memory.deny_text = true;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_TEXT_UNREADABLE);
    memory.deny_text = false;
    memory.text[0] = 'X';
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_TEXT_MISMATCH);
    memory.text[0] = 'c';

    registers.x[19] = UINT64_MAX - 4;
    memory.calls = 0;
    assert(bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(diag.status == BS_UNITY_DIAG_OBJECT_UNREADABLE);
    assert(memory.calls == 0); /* Reject arithmetic overflow. */

    registers.elf_pc = 0x123456;
    assert(!bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, &diag));
    assert(!bs_unity_guest_diagnostic_capture(
        NULL, read_guest, &memory, &diag));
    assert(!bs_unity_guest_diagnostic_capture(
        &registers, read_guest, &memory, NULL));

    puts("PASS: allocator/append/CSEL/JNI snapshots, bounded reads, invalid pointers");
    return 0;
}
