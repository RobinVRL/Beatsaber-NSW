#ifndef BS_BIONIC_MMAP_H
#define BS_BIONIC_MMAP_H
#include <stddef.h>
#include <stdint.h>

/* Android anonymous/file mapping adapter for the Switch memory manager.
 * Supports transparent heap-backed mappings and optional virtual reservations
 * backed on demand with svcMapMemory in a reserved stack-region window.
 */
void *bs_mmap(void *hint, size_t length, int prot, int flags,
              int fd, int64_t offset);
int bs_munmap(void *address, size_t length);
int bs_mprotect(void *address, size_t length, int prot);
int bs_madvise(void *address, size_t length, int advice);

#endif
