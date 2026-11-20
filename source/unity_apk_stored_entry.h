/* Bounded, unencrypted ZIP32 stored-entry reader for original Quest APK
 * resources. This does not load executable code or publish Android objects.
 * Returned bytes belong to the caller, and must be freed using free(). */
#ifndef BS_UNITY_APK_STORED_ENTRY_H
#define BS_UNITY_APK_STORED_ENTRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct BsApkStoredEntry {
    uint8_t *bytes;
    size_t size;
    uint32_t crc32;
} BsApkStoredEntry;

/* Requires a unique exact ZIP name, consistent local/central headers and a
 * verified CRC. Returns 0 for encrypted/compressed/oversize/ambiguous data. */
int bs_apk_read_stored_entry(const char *apk_path, const char *entry_name,
                             size_t max_bytes, BsApkStoredEntry *out);
void bs_apk_free_stored_entry(BsApkStoredEntry *entry);

#endif
