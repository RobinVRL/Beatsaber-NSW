/* Additional Android ARM64 libc contracts backed by Horizon filesystem,
 * secure randomness and explicit offline network policy. MIT license. */
#ifndef BS_ANDROID_RUNTIME_COMPAT_H
#define BS_ANDROID_RUNTIME_COMPAT_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct BsAndroidStatfs {
    int64_t f_type, f_bsize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    uint32_t f_fsid[2];
    int64_t f_namelen, f_frsize, f_flags, f_spare[4];
} BsAndroidStatfs;

int bs_android_runtime_statfs(const char *path, BsAndroidStatfs *output);
int bs_android_runtime_fnmatch(const char *pattern, const char *text, int flags);
int bs_android_runtime_mkstemp(char *pattern);
long bs_android_runtime_random(void);

typedef struct BsAndroidMsghdr {
    void *msg_name;
    uint32_t msg_namelen;
    uint32_t padding;
    void *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int32_t msg_flags;
    int32_t padding2;
} BsAndroidMsghdr;

void *bs_android_runtime_cmsg_nxthdr(const BsAndroidMsghdr *message,
                                      const void *control);
struct hostent;
struct hostent *bs_android_runtime_gethostbyname(const char *name);
struct hostent *bs_android_runtime_gethostbyaddr(const void *address,
                                                  unsigned length, int type);
int bs_android_runtime_recvmmsg(int fd, void *messages, unsigned count,
                                 int flags, void *timeout);
int bs_android_runtime_sendmmsg(int fd, void *messages, unsigned count,
                                 int flags);
int bs_android_runtime_getrusage(int who, void *usage);
long bs_android_runtime_process_vm_readv(int pid, const void *local,
                                        unsigned long local_count,
                                        const void *remote,
                                        unsigned long remote_count,
                                        unsigned long flags);
#endif
