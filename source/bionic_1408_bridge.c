/* Bionic imports needed by the Quest 1.40.8 libraries (Unity 2022.3,
 * OVRPlugin, OculusXRPlugin, ovrplatformloader, IL2CPP) that the 1.46
 * table never had to provide. Errors set newlib errno, which the guest
 * __errno() bridge translates. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include "so_util.h"
#include "bionic_event_epoll.h"
#include "bionic_misc_bridge.h"
#include "bionic_posix_bridge.h"
#include "asset_fs_bridge.h"
#include "android_window_bridge.h"
#include "util.h"

extern DynLibFunction bs_platform_symbols[];
extern const int bs_platform_symbols_count;
extern const char _ctype_[];

/* Same lookup order as so_resolve_symbol for unversioned imports. */
static uintptr_t resolve_like_guest(const char *name) {
    uintptr_t a = bs_bionic_event_dispatch_symbol(name);
    if (a) return a;
    for (int i = 0; i < bs_platform_symbols_count; ++i)
        if (!strcmp(bs_platform_symbols[i].symbol, name))
            return bs_platform_symbols[i].func;
    return bs_bionic_extra_symbol(name);
}

static void fortify_abort(const char *what) {
    debugPrintf("[android:FATAL] FORTIFY: %s: buffer overflow\n", what);
    abort();
}

static ssize_t bs_read_chk(int fd, void *buf, size_t count, size_t buf_size) {
    if (count > buf_size) fortify_abort("read");
    static ssize_t (*real)(int, void *, size_t);
    if (!real) real = (ssize_t (*)(int, void *, size_t))resolve_like_guest("read");
    return real(fd, buf, count);
}

static ssize_t bs_pread_chk(int fd, void *buf, size_t count, int64_t offset,
                            size_t buf_size) {
    if (count > buf_size) fortify_abort("pread");
    return bs_bionic_pread(fd, buf, count, offset);
}

static int bs_poll_chk(void *fds, uint32_t count, int timeout, size_t fds_size) {
    if (fds_size / 8 < count) fortify_abort("poll");
    static int (*real)(void *, uint32_t, int);
    if (!real) real = (int (*)(void *, uint32_t, int))resolve_like_guest("poll");
    return real(fds, count, timeout);
}

static void *bs_memchr_chk(const void *s, int c, size_t n, size_t buf_size) {
    if (n > buf_size) fortify_abort("memchr");
    return memchr(s, c, n);
}

static size_t bs_fread_chk(void *buf, size_t buf_size, size_t size,
                           size_t count, FILE *stream) {
    if (size && count > buf_size / size) fortify_abort("fread");
    return bs_android_fread(buf, size, count, stream);
}

static int bs_setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0; /* buffering policy is owned by the host stdio bridge */
}

static float bs_nanf(const char *tag) { (void)tag; return __builtin_nanf(""); }
static int bs_isnan(double v) { return __builtin_isnan(v); }
static int bs_tolower(int c) { return tolower(c); }
static lldiv_t bs_lldiv(long long n, long long d) { return lldiv(n, d); }

static int bs_sched_affinity(int pid, size_t size, void *mask) {
    (void)pid; (void)size; (void)mask;
    errno = ENOSYS;
    return -1;
}

static int bs_pthread_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void)) {
    (void)prepare; (void)parent; (void)child;
    return 0; /* Horizon applications never fork */
}

static int bs_looper_poll_all(int timeout_ms, int *fd, int *events, void **data) {
    return bs_looper_poll_once(timeout_ms, fd, events, data);
}

static int bs_inet_aton(const char *cp, uint32_t *out) {
    unsigned a, b, c, d;
    char tail;
    if (!cp || sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) return 0;
    if (out) *out = (uint32_t)(a | b << 8 | c << 16 | d << 24); /* network order */
    return 1;
}

static char *bs_inet_ntoa(uint32_t addr) {
    static __thread char text[16];
    snprintf(text, sizeof text, "%u.%u.%u.%u", addr & 255, (addr >> 8) & 255,
             (addr >> 16) & 255, addr >> 24);
    return text;
}

uintptr_t bs_1408_extra_symbol(const char *name) {
    static const struct { const char *name; uintptr_t address; } exports[] = {
        {"_ctype_", (uintptr_t)_ctype_},
        {"inet_aton", (uintptr_t)&bs_inet_aton},
        {"inet_ntoa", (uintptr_t)&bs_inet_ntoa},
        {"setvbuf", (uintptr_t)&bs_setvbuf},
        {"nanf", (uintptr_t)&bs_nanf},
        {"sched_setaffinity", (uintptr_t)&bs_sched_affinity},
        {"sched_getaffinity", (uintptr_t)&bs_sched_affinity},
        {"ALooper_pollAll", (uintptr_t)&bs_looper_poll_all},
        {"lldiv", (uintptr_t)&bs_lldiv},
        {"pthread_atfork", (uintptr_t)&bs_pthread_atfork},
        {"isnan", (uintptr_t)&bs_isnan},
        {"tolower", (uintptr_t)&bs_tolower},
        {"__pread_chk", (uintptr_t)&bs_pread_chk},
        {"__memchr_chk", (uintptr_t)&bs_memchr_chk},
        {"__read_chk", (uintptr_t)&bs_read_chk},
        {"__poll_chk", (uintptr_t)&bs_poll_chk},
        {"__fread_chk", (uintptr_t)&bs_fread_chk},
    };
    if (!name) return 0;
    for (size_t i = 0; i < sizeof exports / sizeof exports[0]; ++i)
        if (!strcmp(exports[i].name, name)) return exports[i].address;
    return 0;
}
