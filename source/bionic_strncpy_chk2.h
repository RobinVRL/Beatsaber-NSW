/* Android Bionic FORTIFY __strncpy_chk2 bridge (AArch64 five-argument ABI).
 * The loader maps the __strncpy_chk2 import to bs_fortify_strncpy_chk2.
 */
#ifndef BS_BIONIC_STRNCPY_CHK2_H
#define BS_BIONIC_STRNCPY_CHK2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy up to n bytes, including any NUL, and zero-fill the remaining bytes
 * after an early NUL as strncpy does. Both object extents are independently
 * checked; a violation terminates with abort() before the invalid access.
 * The source need not be NUL-terminated if all n source bytes are available.
 */
char *bs_fortify_strncpy_chk2(char *dst, const char *src, size_t n,
                              size_t dst_len, size_t src_len);

#ifdef __cplusplus
}
#endif

#endif
