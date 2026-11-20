/* Android AArch64 readv/writev built on the shared guest descriptor layer.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_VECTORED_IO_H
#define BS_BIONIC_VECTORED_IO_H
#include <sys/types.h>
#include <stddef.h>
/* devkitA64/newlib does not provide sys/uio.h. This AArch64 guest layout
 * is exactly two eight-byte fields, compatible with Linux struct iovec. */
typedef struct BsBionicIovec {
    void *iov_base;
    size_t iov_len;
} BsBionicIovec;
ssize_t bs_bionic_readv(int fd, const BsBionicIovec *iov, int count);
ssize_t bs_bionic_writev(int fd, const BsBionicIovec *iov, int count);
#endif
