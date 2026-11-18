/* Read-only inspection of the original Quest libunity.so 40-byte string ABI.
 * This helper is intentionally not called from the production loader.
 */
#ifndef BS_UNITY_STRING_PROBE_H
#define BS_UNITY_STRING_PROBE_H

#include <stddef.h>
#include <stdint.h>

typedef enum BsUnityStringProbeStatus {
    BS_UNITY_STRING_INVALID = -1,
    BS_UNITY_STRING_INLINE = 1,
    BS_UNITY_STRING_LONG = 2,
    BS_UNITY_STRING_LONG_NULL = 3,
    BS_UNITY_STRING_LONG_INCONSISTENT = 4,
} BsUnityStringProbeStatus;

typedef struct BsUnityStringSnapshot {
    BsUnityStringProbeStatus status;
    uintptr_t cstr_address; /* An address only; never dereferenced. */
    size_t size;
    size_t capacity;
    uint8_t tag;
} BsUnityStringSnapshot;

/* Snapshot must contain 40 readable bytes from the original Unity string.
 * Unity's JNI_OnLoad uses [object+31] to select inline vs heap; in the heap
 * representation [object+0]=char*, [object+8]=capacity, [object+16]=size.
 * This never reads from the untrusted guest allocation.
 *
 * For the inline case inline_address is the guest address of the 40-byte
 * object; the caller must supply it separately from the copied snapshot.
 */
BsUnityStringSnapshot bs_unity_string_probe(
    const void *snapshot, size_t snapshot_size, uintptr_t inline_address);

#endif
