/* Bionic AArch64 POSIX filesystem and process ABI for original Quest ELF.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_POSIX_BRIDGE_H
#define BS_BIONIC_POSIX_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "android_window_bridge.h"

/* Android arm64 uses 64-bit time_t, suseconds_t, and long. These structs
 * are deliberately independent of the host/newlib definitions. */
typedef struct { int64_t tv_sec, tv_usec; } BsBionicTimeval;
typedef struct { int64_t tv_sec, tv_nsec; } BsBionicTimespec;
typedef struct { int64_t actime, modtime; } BsBionicUtimbuf;

#ifdef __SWITCH__
int bs_bionic_poll(BsBionicPollfd *fds, uint32_t count, int timeout_ms);
int bs_bionic_select(int nfds, uint64_t *read_fds, uint64_t *write_fds,
                     uint64_t *except_fds, BsBionicTimeval *timeout);
#endif

typedef struct {
    uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
    uint64_t f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
    uint32_t reserved[6];
} BsBionicStatvfs;

/* Android/Linux utsname field length is 65 bytes, including the NUL. */
typedef struct {
    char sysname[65], nodename[65], release[65];
    char version[65], machine[65], domainname[65];
} BsBionicUtsname;

int bs_bionic_ftruncate(int fd, int64_t length);
ssize_t bs_bionic_pread(int fd, void *buffer, size_t count, int64_t offset);
ssize_t bs_bionic_pwrite(int fd, const void *buffer, size_t count, int64_t offset);
void *bs_bionic_fdopendir(int fd);
/* Called by the Android open/openat wrappers after native open succeeds.
 * Only real directory descriptors are retained; paths are canonicalized.
 * A successful fdopendir consumes the descriptor, while failure leaves it
 * open. These records never imply that arbitrary Linux fds are directories. */
void bs_bionic_note_open_fd(int fd, const char *native_path);
void bs_bionic_note_dup2_fd(int source_fd, int destination_fd);
int bs_bionic_truncate(const char *path, int64_t length);
int bs_bionic_fsync(int fd);
int bs_bionic_fdatasync(int fd);
int bs_bionic_flock(int fd, int operation);
int bs_bionic_statvfs(const char *path, BsBionicStatvfs *output);
long bs_bionic_pathconf(const char *path, int name);
int bs_bionic_utime(const char *path, const BsBionicUtimbuf *times);
int bs_bionic_utimes(const char *path, const BsBionicTimeval times[2]);
int bs_bionic_futimens(int fd, const BsBionicTimespec times[2]);
int bs_bionic_utimensat(int dirfd, const char *path,
                       const BsBionicTimespec times[2], int flags);

uint32_t bs_bionic_getuid(void);
uint32_t bs_bionic_geteuid(void);
uint32_t bs_bionic_getgid(void);
uint32_t bs_bionic_getegid(void);
int bs_bionic_gethostname(char *name, size_t capacity);
int bs_bionic_uname(BsBionicUtsname *output);

#endif
