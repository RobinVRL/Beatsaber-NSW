/* Original Android libc++ logging and formatting dependencies.
 *
 * Errors retain their POSIX semantics. Syslog output is sent to the actual
 * NRO's diagnostic log, because Horizon has no Android /dev/log socket.
 */
#include "bionic_misc_bridge.h"
#ifdef __SWITCH__
#include "bionic_posix_bridge.h"
#include "android_socket_bridge.h"
#include "android_process_bridge.h"
#include "android_runtime_compat.h"
#include "android_image_bridge.h"
#include "bionic_event_epoll.h"
#include "bionic_sched_bridge.h"
#include <malloc.h>
#endif
#include "util.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void *bs_bionic_memrchr(const void *buffer, int value, size_t length) {
    if (!buffer) return NULL;
    const unsigned char *bytes = buffer;
    const unsigned char target = (unsigned char)value;
    for (size_t i = length; i > 0; --i) {
        if (bytes[i - 1] == target)
            return (void *)(bytes + i - 1);
    }
    return NULL;
}

char *bs_bionic_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (const unsigned char *outer = (const unsigned char *)haystack;
         *outer; ++outer) {
        const unsigned char *candidate = outer;
        const unsigned char *expected = (const unsigned char *)needle;
        while (*candidate && *expected &&
               tolower(*candidate) == tolower(*expected)) {
            ++candidate;
            ++expected;
        }
        if (!*expected) return (char *)outer;
    }
    return NULL;
}

/* POSIX libgen contract: input is modifiable; pointers returned into input
 * remain valid while the guest keeps the caller-owned buffer alive. */
static char bs_bionic_dot[] = ".";
static char bs_bionic_slash[] = "/";
char *bs_bionic_basename(char *path) {
    if (!path || !*path) return bs_bionic_dot;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') --length;
    path[length] = 0;
    if (length == 1 && *path == '/') return bs_bionic_slash;
    char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}
char *bs_bionic_dirname(char *path) {
    if (!path || !*path) return bs_bionic_dot;
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') --length;
    path[length] = 0;
    char *last = strrchr(path, '/');
    if (!last) return bs_bionic_dot;
    while (last > path && last[-1] == '/') --last;
    if (last == path) return bs_bionic_slash;
    *last = 0;
    return path;
}

int bs_bionic_vasprintf(char **out, const char *format, va_list args) {
    if (!out) { errno = EINVAL; return -1; }
    *out = NULL;
    if (!format) { errno = EINVAL; return -1; }
    va_list sizing;
    va_copy(sizing, args);
    int length = vsnprintf(NULL, 0, format, sizing);
    va_end(sizing);
    if (length < 0 || (size_t)length == SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) { errno = ENOMEM; return -1; }
    va_list writing;
    va_copy(writing, args);
    int written = vsnprintf(buffer, (size_t)length + 1, format, writing);
    va_end(writing);
    if (written != length) {
        free(buffer);
        errno = EIO;
        return -1;
    }
    *out = buffer;
    return written;
}

/* Android libc++ uses syslog to record native runtime failures. No private
 * Android IPC/log daemon exists, so preserve the actual diagnostic messages
 * in loader.log, including facility/priority, without claiming a daemon. */
static char g_syslog_ident[64];
static int g_syslog_facility;

void bs_bionic_openlog(const char *ident, int option, int facility) {
    (void)option;
    g_syslog_facility = facility;
    if (!ident) { g_syslog_ident[0] = 0; return; }
    snprintf(g_syslog_ident, sizeof g_syslog_ident, "%s", ident);
}
void bs_bionic_syslog(int priority, const char *format, ...) {
    char message[768];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof message,
                           format ? format : "", args);
    va_end(args);
    if (length < 0) {
        debugPrintf("[bionic:syslog] invalid format, priority=%d\n", priority);
        return;
    }
    debugPrintf("[bionic:syslog %s fac=%d pri=%d] %s\n",
                g_syslog_ident[0] ? g_syslog_ident : "Quest",
                g_syslog_facility, priority, message);
}
void bs_bionic_closelog(void) {
    g_syslog_ident[0] = 0;
    g_syslog_facility = 0;
}

int bs_bionic_raise(int signal_number) {
    /* Signals are not dispatched as Linux/Bionic signals by Horizon. The
     * original GameActivity's SIGABRT failure path must terminate, but any
     * other signal cannot be reported as delivered successfully. */
    if (signal_number == SIGABRT) {
        debugPrintf("[bionic:signals] raise(SIGABRT) from %p\n",
                    __builtin_return_address(0));
        abort();
    }
    debugPrintf("[bionic:signals] raise(%d) unsupported on Horizon\n",
                signal_number);
    errno = ENOSYS;
    return -1;
}

/* Android native tracing is disabled on Horizon: there is no connected
 * atrace/session state. The Android API defines begin/end as inert while
 * disabled; make that disabled state queryable without claiming a session. */
#ifdef __SWITCH__
static int bs_bionic_atrace_is_enabled(void) {
    return 0;
}
static void bs_bionic_atrace_begin_section(const char *section_name) {
    (void)section_name;
}
static void bs_bionic_atrace_end_section(void) {
}
#endif

/* These entries are absent from the legacy platform table. ELF resolution
 * applies the same DT_VERNEED validation to both tables, so this supplement
 * cannot accidentally satisfy a third-party versioned OpenSSL import. */
uintptr_t bs_bionic_extra_symbol(const char *name) {
#ifdef __SWITCH__
    if (!name) return 0;
    static const struct {
        const char *name;
        uintptr_t address;
    } exports[] = {
        {"pread", (uintptr_t)&bs_bionic_pread},
        {"fdopendir", (uintptr_t)&bs_bionic_fdopendir},
        {"pread64", (uintptr_t)&bs_bionic_pread},
        {"pwrite", (uintptr_t)&bs_bionic_pwrite},
        {"pwrite64", (uintptr_t)&bs_bionic_pwrite},
        {"inet_addr", (uintptr_t)&bs_android_inet_addr},
        {"gai_strerror", (uintptr_t)&bs_android_gai_strerror},
        {"ATrace_isEnabled", (uintptr_t)&bs_bionic_atrace_is_enabled},
        {"ATrace_beginSection", (uintptr_t)&bs_bionic_atrace_begin_section},
        {"ATrace_endSection", (uintptr_t)&bs_bionic_atrace_end_section},
        {"eventfd", (uintptr_t)&bs_bionic_eventfd},
        {"eventfd2", (uintptr_t)&bs_bionic_eventfd2},
        {"epoll_create", (uintptr_t)&bs_bionic_epoll_create},
        {"epoll_create1", (uintptr_t)&bs_bionic_epoll_create1},
        {"epoll_ctl", (uintptr_t)&bs_bionic_epoll_ctl},
        {"epoll_wait", (uintptr_t)&bs_bionic_epoll_wait},
        {"pthread_getcpuclockid", (uintptr_t)&bs_bionic_pthread_getcpuclockid},
        {"pthread_getschedparam", (uintptr_t)&bs_bionic_pthread_getschedparam},
        {"pthread_setschedparam", (uintptr_t)&bs_bionic_pthread_setschedparam},
        {"sched_get_priority_max", (uintptr_t)&bs_bionic_sched_priority_max},
        {"sched_get_priority_min", (uintptr_t)&bs_bionic_sched_priority_min},
        {"statfs", (uintptr_t)&bs_android_runtime_statfs},
        {"fnmatch", (uintptr_t)&bs_android_runtime_fnmatch},
        {"mkstemp", (uintptr_t)&bs_android_runtime_mkstemp},
        {"random", (uintptr_t)&bs_android_runtime_random},
        {"__cmsg_nxthdr", (uintptr_t)&bs_android_runtime_cmsg_nxthdr},
        {"gethostbyname", (uintptr_t)&bs_android_runtime_gethostbyname},
        {"gethostbyaddr", (uintptr_t)&bs_android_runtime_gethostbyaddr},
        {"recvmmsg", (uintptr_t)&bs_android_runtime_recvmmsg},
        {"sendmmsg", (uintptr_t)&bs_android_runtime_sendmmsg},
        {"getrusage", (uintptr_t)&bs_android_runtime_getrusage},
        {"process_vm_readv", (uintptr_t)&bs_android_runtime_process_vm_readv},
        {"malloc_usable_size", (uintptr_t)&malloc_usable_size},
        {"AImage_getWidth", (uintptr_t)&AImage_getWidth},
        {"AImage_getHeight", (uintptr_t)&AImage_getHeight},
        {"AImage_getTimestamp", (uintptr_t)&AImage_getTimestamp},
        {"AImage_getPlaneData", (uintptr_t)&AImage_getPlaneData},
        {"AImage_getPlaneRowStride", (uintptr_t)&AImage_getPlaneRowStride},
        {"AImage_delete", (uintptr_t)&AImage_delete},
        /* No child processes, passwd database, POSIX controlling terminal,
         * or shell exist in this Horizon application. These real error
         * contracts preserve Android callers' negative-return branches. */
        {"fork", (uintptr_t)&bs_android_process_fork},
        {"execl", (uintptr_t)&bs_android_process_execl},
        {"waitpid", (uintptr_t)&bs_android_process_waitpid},
        {"kill", (uintptr_t)&bs_android_process_kill},
        {"ptrace", (uintptr_t)&bs_android_process_ptrace},
        {"getpwuid", (uintptr_t)&bs_android_process_getpwuid},
        {"getpwuid_r", (uintptr_t)&bs_android_process_getpwuid_r},
        {"prctl", (uintptr_t)&bs_android_process_prctl},
        {"getpriority", (uintptr_t)&bs_android_process_getpriority},
        {"setpriority", (uintptr_t)&bs_android_process_setpriority},
        {"tcgetattr", (uintptr_t)&bs_android_process_tcgetattr},
        {"tcsetattr", (uintptr_t)&bs_android_process_tcsetattr},
        {"mlock", (uintptr_t)&bs_android_process_mlock},
        {"if_nametoindex", (uintptr_t)&bs_android_process_if_nametoindex},
        {"popen", (uintptr_t)&bs_android_process_popen},
        {"pclose", (uintptr_t)&bs_android_process_pclose}
    };
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); ++i)
        if (strcmp(name, exports[i].name) == 0)
            return exports[i].address;
#else
    /* The host's isolated misc ABI test does not stage Switch platform
     * adapters; the actual resolver and integration only run on Switch. */
    (void)name;
#endif
    return 0;
}
