/* Read-only guest-register and bounded guest-memory evidence for the original
 * Quest UnityPlayerForActivityOrService allocation/FindClass failure.
 *
 * This is an offline decoder, not an instruction hook. An external debugger
 * or host callback must stop a guest thread and supply its registers and a
 * checked memory reader. No guest code or JNI state is changed.
 */
#ifndef BS_UNITY_GUEST_REGISTER_DIAGNOSTIC_H
#define BS_UNITY_GUEST_REGISTER_DIAGNOSTIC_H

#include "unity_string_probe.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BS_UNITY_DIAG_CLASS_CHARS 50u
#define BS_UNITY_DIAG_CLASS_BYTES (BS_UNITY_DIAG_CLASS_CHARS + 1u)
#define BS_UNITY_DIAG_OBJECT_BYTES 40u

/* Original libunity.so ELF-relative PC, not an absolute host code pointer.
 * At PRE_SELECT, x1 has NOT yet been selected. The first instruction at
 * 0x5d6e58 is CSEL x1,x19,x10,cc; x1 is meaningful at 0x5d6e5c. */
typedef enum BsUnityDiagSite {
    BS_UNITY_DIAG_ALLOC_RETURN = 0x5c6d54,
    BS_UNITY_DIAG_APPEND_RETURN = 0x9115e8,
    BS_UNITY_DIAG_PRE_SELECT = 0x5d6e58,
    BS_UNITY_DIAG_FINDCLASS_CALL = 0x5d6e5c
} BsUnityDiagSite;

typedef struct BsUnityGuestRegisters {
    uint64_t x[31];
    uint64_t sp;
    uint64_t elf_pc;
} BsUnityGuestRegisters;

/* Return true only after copying exactly count accessible guest bytes.
 * The producer must check mapping/permissions before reading; never blindly
 * reinterpret an untrusted guest pointer as a host pointer. The decoder caps
 * all calls to 40 or 51 bytes and makes at most two reads per checkpoint. */
typedef bool (*BsUnityGuestRead)(void *user, uint64_t guest_address,
                                 void *out, size_t count);

typedef enum BsUnityDiagStatus {
    BS_UNITY_DIAG_INVALID_SITE = 0,
    BS_UNITY_DIAG_NO_OBJECT,
    BS_UNITY_DIAG_OBJECT_UNREADABLE,
    BS_UNITY_DIAG_INVALID_STRING,
    BS_UNITY_DIAG_LONG_NULL,
    BS_UNITY_DIAG_BAD_LENGTH,
    BS_UNITY_DIAG_TEXT_UNREADABLE,
    BS_UNITY_DIAG_TEXT_MISMATCH,
    BS_UNITY_DIAG_POINTER_MISMATCH,
    BS_UNITY_DIAG_VALID_CLASS,
    BS_UNITY_DIAG_INLINE_STRING,
    BS_UNITY_DIAG_ALLOCATOR_RETURN
} BsUnityDiagStatus;

typedef struct BsUnityGuestDiagnostic {
    BsUnityDiagSite site;
    BsUnityDiagStatus status;
    uint64_t x0;
    uint64_t x1;
    uint64_t x8;
    uint64_t x10;
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x23;
    uint64_t sp;
    uint64_t string_object_address;
    bool x1_is_findclass_argument;
    bool object_read;
    bool class_bytes_read;
    uint8_t object[BS_UNITY_DIAG_OBJECT_BYTES];
    uint8_t class_bytes[BS_UNITY_DIAG_CLASS_BYTES];
    BsUnityStringSnapshot decoded;
} BsUnityGuestDiagnostic;

/* Returns false only for unsupported PC or invalid arguments. Otherwise
 * status explains evidence that is unavailable, inconsistent, or valid.
 * There are NO allocations, guest writes, unbounded strlen, or log calls. */
bool bs_unity_guest_diagnostic_capture(const BsUnityGuestRegisters *registers,
                                      BsUnityGuestRead read_guest,
                                      void *read_user,
                                      BsUnityGuestDiagnostic *out);

#endif
