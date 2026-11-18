#include "unity_string_probe.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put_u64(unsigned char *s, size_t at, uint64_t value) {
    memcpy(s + at, &value, sizeof value);
}

int main(void) {
    unsigned char s[40] = {0};
    const char *short_name = "com/unity3d/player/UnityPlayer";
    const char *long_name =
        "com/unity3d/player/UnityPlayerForActivityOrService";
    assert(strlen(short_name) == 30 && strlen(long_name) == 50);
    memcpy(s, short_name, 31);
    s[31] = 1; /* 31-byte small-string capacity minus 30 bytes used. */
    BsUnityStringSnapshot p =
        bs_unity_string_probe(s, sizeof s, (uintptr_t)0x100000);
    assert(p.status == BS_UNITY_STRING_INLINE);
    assert(p.cstr_address == (uintptr_t)0x100000 && p.size == 30);

    memset(s, 0, sizeof s);
    put_u64(s, 0, 0x123456780000ULL);
    put_u64(s, 8, 50);
    put_u64(s, 16, 50);
    s[31] = 0x5f; /* Original grow path 0x5c6d78, length 50. */
    p = bs_unity_string_probe(s, sizeof s, 0x100000);
    assert(p.status == BS_UNITY_STRING_LONG);
    assert(p.cstr_address == (uintptr_t)0x123456780000ULL);
    assert(p.size == 50 && p.capacity == 50);
    put_u64(s, 0, 0);
    p = bs_unity_string_probe(s, sizeof s, 0x100000);
    assert(p.status == BS_UNITY_STRING_LONG_NULL && !p.cstr_address);
    put_u64(s, 0, 0x123456780000ULL);
    put_u64(s, 16, 51);
    p = bs_unity_string_probe(s, sizeof s, 0x100000);
    assert(p.status == BS_UNITY_STRING_LONG_INCONSISTENT);
    s[31] = 0x80;
    p = bs_unity_string_probe(s, sizeof s, 0x100000);
    assert(p.status == BS_UNITY_STRING_INVALID);
    assert(bs_unity_string_probe(NULL, 40, 0).status ==
           BS_UNITY_STRING_INVALID);
    assert(bs_unity_string_probe(s, 39, 0).status ==
           BS_UNITY_STRING_INVALID);
    puts("PASS Unity Quest 40-byte inline/heap string snapshot and NULL discrimination");
    return 0;
}
