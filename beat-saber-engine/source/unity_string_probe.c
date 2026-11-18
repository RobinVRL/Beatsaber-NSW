#include "unity_string_probe.h"

#include <string.h>

BsUnityStringSnapshot bs_unity_string_probe(
    const void *snapshot, size_t snapshot_size, uintptr_t inline_address) {
    BsUnityStringSnapshot result = {0};
    result.status = BS_UNITY_STRING_INVALID;
    if (!snapshot || snapshot_size != 40) return result;

    const uint8_t *bytes = snapshot;
    result.tag = bytes[31];
    if ((result.tag & 0xc0u) == 0) {
        result.status = BS_UNITY_STRING_INLINE;
        result.cstr_address = inline_address;
        result.capacity = 31;
        result.size = 31u - result.tag;
        return result;
    }
    if ((result.tag & 0xc0u) != 0x40u) return result;

    /* Do not cast a guest byte buffer to pointer/size_t lvalues: tests run on
     * host too, and memcpy guarantees no misaligned or aliasing accesses. */
    uint64_t pointer = 0, capacity = 0, length = 0;
    memcpy(&pointer, bytes, sizeof pointer);
    memcpy(&capacity, bytes + 8, sizeof capacity);
    memcpy(&length, bytes + 16, sizeof length);
    result.cstr_address = (uintptr_t)pointer;
    result.size = (size_t)length;
    result.capacity = (size_t)capacity;
    if (!pointer) {
        result.status = BS_UNITY_STRING_LONG_NULL;
    } else if (length > capacity) {
        result.status = BS_UNITY_STRING_LONG_INCONSISTENT;
    } else {
        result.status = BS_UNITY_STRING_LONG;
    }
    return result;
}
