/* Buffer-checked Android libc calls required by libOVRPlugin and libunity.
 * Fail immediately on a violated Bionic _FORTIFY_SOURCE contract.
 */
#include "bionic_fortify.h"
#include "asset_fs_bridge.h"
#include "android_window_bridge.h"
#include "bionic_posix_bridge.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

static void bs_fortify_abort(const char *operation) {
    debugPrintf("[bionic:fortify] %s: detected out-of-bounds operation\n",
                (char *)(operation ? operation : "?"));
    abort();
}

static void bs_fortify_check(size_t bytes, size_t capacity, const char *name) {
    if (bytes > capacity) bs_fortify_abort(name);
}

void *bs_fortify_memcpy(void *dst, const void *src, size_t bytes, size_t dst_bytes) {
    bs_fortify_check(bytes, dst_bytes, "__memcpy_chk");
    return memcpy(dst, src, bytes);
}

void *bs_fortify_memmove(void *dst, const void *src, size_t bytes, size_t dst_bytes) {
    bs_fortify_check(bytes, dst_bytes, "__memmove_chk");
    return memmove(dst, src, bytes);
}

void *bs_fortify_memset(void *dst, int ch, size_t bytes, size_t dst_bytes) {
    bs_fortify_check(bytes, dst_bytes, "__memset_chk");
    return memset(dst, ch, bytes);
}

size_t bs_fortify_strlen(const char *str, size_t max_bytes) {
    if (!str) bs_fortify_abort("__strlen_chk NULL");
    size_t count = strnlen(str, max_bytes);
    if (count == max_bytes) bs_fortify_abort("__strlen_chk");
    return count;
}

char *bs_fortify_strchr(const char *str, int ch, size_t max_bytes) {
    (void)bs_fortify_strlen(str, max_bytes);
    return (char *)strchr(str, ch);
}

char *bs_fortify_strncat(char *dst, const char *src, size_t bytes,
                         size_t dst_bytes) {
    if (!dst || !src) bs_fortify_abort("__strncat_chk NULL");
    size_t len = strnlen(dst, dst_bytes);
    if (len == dst_bytes) bs_fortify_abort("__strncat_chk unterminated");
    size_t copy = strnlen(src, bytes);
    if (copy > dst_bytes - len - 1)
        bs_fortify_abort("__strncat_chk");
    memcpy(dst + len, src, copy);
    dst[len + copy] = '\0';
    return dst;
}

int bs_fortify_vsnprintf(char *dst, size_t bytes, int flags, size_t dst_bytes,
                         const char *format, va_list args) {
    (void)flags;
    bs_fortify_check(bytes, dst_bytes, "__vsnprintf_chk");
    return vsnprintf(dst, bytes, format, args);
}

ssize_t bs_fortify_write(int fd, const void *src, size_t bytes, size_t src_bytes) {
    bs_fortify_check(bytes, src_bytes, "__write_chk");
    /* Android write returns ssize_t; an oversized request must not wrap into
     * a negative byte count when crossing the Bionic ABI boundary. */
    if (bytes > (SIZE_MAX >> 1)) bs_fortify_abort("__write_chk count overflow");
    return bs_pipe_write(fd, src, bytes);
}

ssize_t bs_fortify_pwrite(int fd, const void *src, size_t bytes, off_t offset,
                          size_t src_bytes) {
    bs_fortify_check(bytes, src_bytes, "__pwrite_chk");
    if (bytes > (SIZE_MAX >> 1))
        bs_fortify_abort("__pwrite_chk count overflow");
    return bs_bionic_pwrite(fd, src, bytes, (int64_t)offset);
}

size_t bs_fortify_fwrite(const void *src, size_t size, size_t count, FILE *stream,
                         size_t src_bytes) {
    if (size && count > SIZE_MAX / size) bs_fortify_abort("__fwrite_chk overflow");
    bs_fortify_check(size * count, src_bytes, "__fwrite_chk");
    return bs_android_fwrite(src, size, count, stream);
}

int bs_fortify_open2(const char *path, int flags) {
    /* Open with O_CREAT requires a third mode parameter. Android's two-arg
     * fortified open aborts if the caller attempts to omit that parameter.
     */
    /* Guest flags are Android Linux values, not newlib's host O_* values. */
    if ((flags & 0x40) || ((flags & 0x410000) == 0x410000))
        bs_fortify_abort("__open_2 missing mode");
    return bs_android_open(path, flags);
}
