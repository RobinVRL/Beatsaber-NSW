/* The original Quest guest uses Bionic readv/writev to read/write both
 * ordinary SD file descriptors and GameActivity's virtual command pipes.
 * Its AArch64 iovec is {void *, size_t}, identical to native AArch64.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_vectored_io.h"
#include "android_window_bridge.h"
#include <errno.h>
#include <stdint.h>
#include <limits.h>

/* newlib does not expose POSIX SSIZE_MAX, while its AArch64 ssize_t and
 * ptrdiff_t are both signed pointer-width integers. */
#ifndef SSIZE_MAX
#define SSIZE_MAX PTRDIFF_MAX
#endif

#define BS_BIONIC_IOV_MAX 1024

static ssize_t bs_iov_operation(int fd, const BsBionicIovec *iov, int count,
                                 int writing) {
    if (count < 0 || count > BS_BIONIC_IOV_MAX || (count && !iov)) {
        errno = EINVAL;
        return -1;
    }
    if (!count) return 0;
    size_t total_size = 0;
    for (int i = 0; i < count; ++i) {
        if (iov[i].iov_len > (SIZE_MAX >> 1) - total_size) {
            errno = EINVAL;
            return -1;
        }
        if (iov[i].iov_len && !iov[i].iov_base) {
            errno = EFAULT;
            return -1;
        }
        total_size += iov[i].iov_len;
    }
    ssize_t completed = 0;
    for (int i = 0; i < count; ++i) {
        if (!iov[i].iov_len) continue;
        ssize_t result = writing ? bs_pipe_write(fd, iov[i].iov_base, iov[i].iov_len)
                                 : bs_pipe_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (result < 0) return completed ? completed : -1;
        completed += result;
        if ((size_t)result != iov[i].iov_len) break;
    }
    return completed;
}

ssize_t bs_bionic_readv(int fd, const BsBionicIovec *iov, int count) {
    return bs_iov_operation(fd, iov, count, 0);
}

ssize_t bs_bionic_writev(int fd, const BsBionicIovec *iov, int count) {
    return bs_iov_operation(fd, iov, count, 1);
}
