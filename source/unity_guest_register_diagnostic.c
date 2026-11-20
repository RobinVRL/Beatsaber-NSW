#include "unity_guest_register_diagnostic.h"

#include <string.h>

static const uint8_t expected_class[BS_UNITY_DIAG_CLASS_BYTES] =
    "com/unity3d/player/UnityPlayerForActivityOrService";

static bool read_bounded(BsUnityGuestRead reader, void *user,
                         uint64_t address, void *out, size_t length) {
    if (!reader || !address || !length ||
        address > UINT64_MAX - (uint64_t)(length - 1u))
        return false;
    return reader(user, address, out, length);
}

bool bs_unity_guest_diagnostic_capture(const BsUnityGuestRegisters *registers,
                                      BsUnityGuestRead read_guest,
                                      void *read_user,
                                      BsUnityGuestDiagnostic *out) {
    if (!registers || !out) return false;
    BsUnityDiagSite site = (BsUnityDiagSite)registers->elf_pc;
    if (site != BS_UNITY_DIAG_ALLOC_RETURN &&
        site != BS_UNITY_DIAG_APPEND_RETURN &&
        site != BS_UNITY_DIAG_PRE_SELECT &&
        site != BS_UNITY_DIAG_FINDCLASS_CALL)
        return false;

    memset(out, 0, sizeof *out);
    out->site = site;
    out->x0 = registers->x[0];
    out->x1 = registers->x[1];
    out->x8 = registers->x[8];
    out->x10 = registers->x[10];
    out->x19 = registers->x[19];
    out->x20 = registers->x[20];
    out->x21 = registers->x[21];
    out->x23 = registers->x[23];
    out->sp = registers->sp;
    out->x1_is_findclass_argument = site == BS_UNITY_DIAG_FINDCLASS_CALL;
    out->string_object_address = site == BS_UNITY_DIAG_APPEND_RETURN ?
                                 registers->sp : registers->x[19];

    if (!out->string_object_address) {
        out->status = BS_UNITY_DIAG_NO_OBJECT;
        return true;
    }
    if (!read_bounded(read_guest, read_user, out->string_object_address,
                      out->object, sizeof out->object)) {
        out->status = BS_UNITY_DIAG_OBJECT_UNREADABLE;
        return true;
    }
    out->object_read = true;
    out->decoded = bs_unity_string_probe(
        out->object, sizeof out->object,
        (uintptr_t)out->string_object_address);
    if (site == BS_UNITY_DIAG_ALLOC_RETURN) {
        /* The growing string has not been published: x0 is the allocation
         * result and string->heap_ptr is still the previous inline content. */
        out->status = BS_UNITY_DIAG_ALLOCATOR_RETURN;
        return true;
    }
    switch (out->decoded.status) {
    case BS_UNITY_STRING_INLINE:
        out->status = BS_UNITY_DIAG_INLINE_STRING;
        return true;
    case BS_UNITY_STRING_LONG_NULL:
        out->status = BS_UNITY_DIAG_LONG_NULL;
        return true;
    case BS_UNITY_STRING_LONG_INCONSISTENT:
        out->status = BS_UNITY_DIAG_BAD_LENGTH;
        return true;
    case BS_UNITY_STRING_LONG:
        break;
    default:
        out->status = BS_UNITY_DIAG_INVALID_STRING;
        return true;
    }
    if (out->decoded.size != BS_UNITY_DIAG_CLASS_CHARS ||
        out->decoded.capacity < BS_UNITY_DIAG_CLASS_CHARS) {
        out->status = BS_UNITY_DIAG_BAD_LENGTH;
        return true;
    }
    if (!read_bounded(read_guest, read_user,
                      (uint64_t)out->decoded.cstr_address,
                      out->class_bytes, sizeof out->class_bytes)) {
        out->status = BS_UNITY_DIAG_TEXT_UNREADABLE;
        return true;
    }
    out->class_bytes_read = true;
    if (memcmp(out->class_bytes, expected_class, sizeof expected_class)) {
        out->status = BS_UNITY_DIAG_TEXT_MISMATCH;
        return true;
    }

    /* At 0x5d6e58 CSEL has not executed; x1 there can be stale. Only the
     * 0x5d6e5c checkpoint permits an actual JNI argument comparison. */
    if (out->x1_is_findclass_argument &&
        out->x1 != (uint64_t)out->decoded.cstr_address) {
        out->status = BS_UNITY_DIAG_POINTER_MISMATCH;
        return true;
    }
    out->status = BS_UNITY_DIAG_VALID_CLASS;
    return true;
}
