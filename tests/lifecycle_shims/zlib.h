/* Host-only zlib CRC API fixture for the lifecycle ELF read-only test.
 * Its exact RFC 1952-compatible CRC32 implementation lives in the C test.
 * The Switch build uses libz and the actual devkitPro zlib.h instead.
 */
#ifndef BS_LIFECYCLE_TEST_ZLIB_H
#define BS_LIFECYCLE_TEST_ZLIB_H
#include <stddef.h>
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
#define Z_NULL ((void *)0)
uLong crc32(uLong seed, const Bytef *data, uInt length);
#endif
