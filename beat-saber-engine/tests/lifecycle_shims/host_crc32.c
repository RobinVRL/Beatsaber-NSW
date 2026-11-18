/* Test-only CRC32 for the existing original GameActivity host test.
 * Keeps that existing test unchanged and avoids mixing AArch64 libz objects
 * into an x86_64 Windows host executable.
 */
#include <stdint.h>
#include <zlib.h>

uLong crc32(uLong initial, const Bytef *data, uInt length) {
    if (!data) return initial;
    uint32_t crc = (uint32_t)initial ^ UINT32_C(0xffffffff);
    for (uInt i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^
                  ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    return (uLong)(crc ^ UINT32_C(0xffffffff));
}
