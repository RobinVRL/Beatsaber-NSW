#include "unity_apk_stored_entry.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 4);
    BsApkStoredEntry entry = {0};
    assert(bs_apk_read_stored_entry(argv[1], "AndroidManifest.xml", 100000,
                                    &entry));
    assert(entry.bytes && entry.size == 8956 && entry.crc32);
    assert(entry.bytes[0] == 0x03 && entry.bytes[1] == 0x00);
    assert(!bs_apk_read_stored_entry(argv[1], "resources.arsc", 1024,
                                     &entry));
    assert(entry.size == 8956);
    bs_apk_free_stored_entry(&entry);
    assert(!entry.bytes && !entry.size && !entry.crc32);

    assert(bs_apk_read_stored_entry(argv[1], "resources.arsc", 1024 * 1024,
                                    &entry));
    assert(entry.bytes && entry.size == 258492 && entry.crc32);
    bs_apk_free_stored_entry(&entry);
    assert(!bs_apk_read_stored_entry(argv[1], "not-real-file.txt", 2048,
                                     &entry));
    assert(!entry.bytes && !entry.size && !entry.crc32);

    assert(!bs_apk_read_stored_entry(argv[2], "AndroidManifest.xml", 1024,
                                     &entry));
    assert(!bs_apk_read_stored_entry(argv[3], "AndroidManifest.xml", 1024,
                                     &entry));
    printf("PASS: original Quest binary manifest/resources stored ZIP CRC,"
           " size and duplicate/corruption refusal\n");
    return 0;
}
