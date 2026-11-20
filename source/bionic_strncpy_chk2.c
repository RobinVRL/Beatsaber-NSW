/* Five-argument Android Bionic __strncpy_chk2 adapter, for guest AArch64.
 * Matches the bounds and NUL-padding behavior in Android 12 Bionic's
 * libc/bionic/fortify.cpp, without replacing newlib's normal strncpy.
 */
#include "bionic_strncpy_chk2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void bs_strncpy_chk2_fail(const char *reason)
{
    /* The output stream is native newlib stderr, never a guest Bionic FILE*. */
    fprintf(stderr, "[bionic:fortify] __strncpy_chk2: %s\n", reason);
    abort();
}

char *bs_fortify_strncpy_chk2(char *dst, const char *src, size_t n,
                              size_t dst_len, size_t src_len)
{
    /* The full requested n bytes must fit in the destination, even if the
     * source has an early NUL. Bionic also enforces this before any copy.
     */
    if (n > dst_len)
        bs_strncpy_chk2_fail("destination buffer overflow");

    /* strncpy(dst, src, 0) does not access either pointer. */
    if (n == 0)
        return dst;

    if (!dst || !src)
        bs_strncpy_chk2_fail("null pointer with nonzero copy length");

    for (size_t i = 0; i < n; ++i) {
        /* Check before dereferencing src[i]. Android 12 Bionic does likewise,
         * avoiding source overreads at the exact known object boundary.
         */
        if (i >= src_len)
            bs_strncpy_chk2_fail("source buffer overread");

        const char ch = src[i];
        dst[i] = ch;
        if (ch == '\0') {
            /* Only write destination bytes; no further source read needed. */
            if (i + 1 < n)
                memset(dst + i + 1, 0, n - i - 1);
            break;
        }
    }
    return dst;
}
