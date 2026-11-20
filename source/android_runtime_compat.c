/* Android libc services not supplied by devkitA64/newlib. MIT license. */
#include "android_runtime_compat.h"
#include "asset_fs_bridge.h"
#include "bionic_posix_bridge.h"
#include "bionic_random_bridge.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <netdb.h>

int bs_android_runtime_statfs(const char *path, BsAndroidStatfs *output) {
    if (!output) { errno = EFAULT; return -1; }
    BsBionicStatvfs native = {0};
    if (bs_bionic_statvfs(path, &native) < 0) return -1;
    BsAndroidStatfs result = {0};
    /* The native filesystem does not expose a Linux f_type magic number;
     * zero identifies unknown type, with all space/inode counts observed. */
    result.f_bsize = (int64_t)native.f_bsize;
    result.f_blocks = native.f_blocks;
    result.f_bfree = native.f_bfree;
    result.f_bavail = native.f_bavail;
    result.f_files = native.f_files;
    result.f_ffree = native.f_ffree;
    result.f_fsid[0] = (uint32_t)native.f_fsid;
    result.f_fsid[1] = (uint32_t)(native.f_fsid >> 32);
    result.f_namelen = (int64_t)native.f_namemax;
    result.f_frsize = (int64_t)native.f_frsize;
    result.f_flags = (int64_t)native.f_flag;
    *output = result;
    return 0;
}

enum { BS_FNM_PATHNAME=1, BS_FNM_NOESCAPE=2, BS_FNM_PERIOD=4,
       BS_FNM_LEADING_DIR=8, BS_FNM_CASEFOLD=16 };

static int match_character(unsigned char a, unsigned char b, int flags) {
    if (flags & BS_FNM_CASEFOLD) {
        a = (unsigned char)tolower(a);
        b = (unsigned char)tolower(b);
    }
    return a == b;
}

static int match_class(const char *pattern, const char **end,
                       unsigned char character, int flags) {
    const char *cursor = pattern + 1;
    int invert = *cursor == '!' || *cursor == '^';
    if (invert) ++cursor;
    int matched = 0, valid = 0;
    if (*cursor == ']') {
        matched = character == ']';
        ++cursor;
    }
    for (; *cursor && *cursor != ']'; ++cursor) {
        unsigned char first = (unsigned char)*cursor;
        if (first == '\\' && !(flags & BS_FNM_NOESCAPE) && cursor[1])
            first = (unsigned char)*++cursor;
        if (cursor[1] == '-' && cursor[2] && cursor[2] != ']') {
            cursor += 2;
            unsigned char last = (unsigned char)*cursor;
            if (last == '\\' && !(flags & BS_FNM_NOESCAPE) && cursor[1])
                last = (unsigned char)*++cursor;
            unsigned char value = character;
            if (flags & BS_FNM_CASEFOLD) {
                first = (unsigned char)tolower(first);
                last = (unsigned char)tolower(last);
                value = (unsigned char)tolower(value);
            }
            if (value >= first && value <= last) matched = 1;
        } else if (match_character(first, character, flags)) matched = 1;
    }
    if (*cursor != ']') return -1;
    valid = 1;
    *end = cursor + 1;
    return valid && (invert ? !matched : matched);
}

static int match_pattern(const char *pattern, const char *text,
                         int flags, unsigned depth, int segment_start) {
    if (depth > 128) return 0;
    for (;;) {
        unsigned char token = (unsigned char)*pattern;
        unsigned char character = (unsigned char)*text;
        if (!token)
            return !character || ((flags & BS_FNM_LEADING_DIR) && character == '/');
        if (token == '*') {
            while (*pattern == '*') ++pattern;
            if (match_pattern(pattern, text, flags, depth + 1, segment_start)) return 1;
            while (*text && (!(flags & BS_FNM_PATHNAME) || *text != '/')) {
                if ((flags & BS_FNM_PERIOD) && segment_start && *text == '.') break;
                ++text;
                segment_start = 0;
                if (match_pattern(pattern, text, flags, depth + 1, segment_start)) return 1;
            }
            return 0;
        }
        if (!character) return 0;
        if ((flags & BS_FNM_PERIOD) && segment_start && character == '.' &&
            (token == '?' || token == '[')) return 0;
        if (token == '?' || token == '[') {
            if ((flags & BS_FNM_PATHNAME) && character == '/') return 0;
            if (token == '[') {
                const char *next = NULL;
                int result = match_class(pattern, &next, character, flags);
                if (result >= 0) {
                    if (!result) return 0;
                    pattern = next;
                    ++text;
                    segment_start = 0;
                    continue;
                }
            } else {
                ++pattern;
                ++text;
                segment_start = 0;
                continue;
            }
        }
        if (token == '\\' && !(flags & BS_FNM_NOESCAPE) && pattern[1])
            token = (unsigned char)*++pattern;
        if (!match_character(token, character, flags)) return 0;
        segment_start = character == '/' && (flags & BS_FNM_PATHNAME);
        ++pattern;
        ++text;
    }
}

int bs_android_runtime_fnmatch(const char *pattern, const char *text, int flags) {
    if (!pattern || !text || (flags & ~31)) return 1;
    return match_pattern(pattern, text, flags, 0, 1) ? 0 : 1;
}

int bs_android_runtime_mkstemp(char *pattern) {
    if (!pattern) { errno = EFAULT; return -1; }
    size_t length = strlen(pattern);
    if (length < 6 || memcmp(pattern + length - 6, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (unsigned trial = 0; trial < 100; ++trial) {
        uint8_t choices[6];
        if (bs_bionic_getentropy(choices, sizeof choices) < 0) return -1;
        for (int i = 0; i < 6; ++i)
            pattern[length - 6 + (size_t)i] = alphabet[choices[i] % 62];
        int descriptor = bs_android_open(pattern, 02 | 0100 | 0200, 0600);
        if (descriptor >= 0) return descriptor;
        if (errno != EEXIST) break;
    }
    memcpy(pattern + length - 6, "XXXXXX", 6);
    return -1;
}

long bs_android_runtime_random(void) {
    uint32_t value = 0;
    if (bs_bionic_getrandom(&value, sizeof value, 0) < 0) return 0;
    return (long)(value & 0x7fffffff);
}

void *bs_android_runtime_cmsg_nxthdr(const BsAndroidMsghdr *message,
                                      const void *control) {
    if (!message || !control || !message->msg_control) return NULL;
    uintptr_t start = (uintptr_t)message->msg_control;
    if (message->msg_controllen > UINTPTR_MAX - start) return NULL;
    uintptr_t end = start + message->msg_controllen;
    uintptr_t current = (uintptr_t)control;
    if (current < start || current > end || end - current < 16) return NULL;
    size_t length = 0;
    memcpy(&length, control, sizeof length);
    if (length < 16 || length > end - current ||
        length > SIZE_MAX - 7) return NULL;
    uintptr_t next = current + ((length + 7) & ~(size_t)7);
    return next <= end && end - next >= 16 ? (void *)next : NULL;
}

/* bionic and newlib struct hostent share the LP64 layout (name, aliases,
 * int addrtype, int length, addr_list); AF_INET is 2 on both. */
struct hostent *bs_android_runtime_gethostbyname(const char *name) {
    extern bool bs_android_network_ready(void);
    extern void debugPrintf(const char *, ...);
    struct hostent *h = name && bs_android_network_ready() ?
        gethostbyname(name) : NULL;
    static int logged;
    if (logged++ < 20)
        debugPrintf("[net] gethostbyname(%s) -> %p\n", name ? name : "(null)",
                    (void *)h);
    if (!h) errno = ENOENT;
    return h;
}
struct hostent *bs_android_runtime_gethostbyaddr(const void *address,
                                                  unsigned length, int type) {
    (void)address; (void)length; (void)type;
    errno = ENETDOWN;
    return NULL;
}
int bs_android_runtime_recvmmsg(int fd, void *messages, unsigned count,
                                 int flags, void *timeout) {
    (void)fd; (void)messages; (void)count; (void)flags; (void)timeout;
    errno = ENETDOWN;
    return -1;
}
int bs_android_runtime_sendmmsg(int fd, void *messages, unsigned count,
                                 int flags) {
    (void)fd; (void)messages; (void)count; (void)flags;
    errno = ENETDOWN;
    return -1;
}
int bs_android_runtime_getrusage(int who, void *usage) {
    (void)who; (void)usage;
    errno = ENOSYS;
    return -1;
}
long bs_android_runtime_process_vm_readv(int pid, const void *local,
                                        unsigned long local_count,
                                        const void *remote,
                                        unsigned long remote_count,
                                        unsigned long flags) {
    (void)pid; (void)local; (void)local_count; (void)remote;
    (void)remote_count; (void)flags;
    errno = ENOSYS;
    return -1;
}
