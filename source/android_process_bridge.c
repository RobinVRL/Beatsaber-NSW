/* Horizon homebrew cannot fork an Android/Linux process, attach ptrace to
 * arbitrary tasks, open POSIX terminals or launch a command interpreter.
 * Keep those imported entrypoints linkable with truthful error semantics.
 * No call in this file reports a successful OS action that did not occur.
 * SPDX-License-Identifier: MIT
 */
#include "android_process_bridge.h"
#include <errno.h>

pid_t bs_android_process_fork(void) {
    errno = ENOSYS;
    return -1;
}
int bs_android_process_execl(const char *path, const char *arg0, ...) {
    (void)path; (void)arg0;
    errno = ENOSYS;
    return -1;
}
pid_t bs_android_process_waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}
int bs_android_process_kill(pid_t pid, int signal_number) {
    (void)pid; (void)signal_number;
    errno = ENOSYS;
    return -1;
}
long bs_android_process_ptrace(long request, long pid, void *address, void *data) {
    (void)request; (void)pid; (void)address; (void)data;
    errno = ENOSYS;
    return -1;
}
struct passwd *bs_android_process_getpwuid(uint32_t uid) {
    (void)uid;
    errno = ENOSYS;
    return NULL;
}
int bs_android_process_getpwuid_r(uint32_t uid, struct passwd *record,
                                  char *buffer, size_t length,
                                  struct passwd **result) {
    (void)uid; (void)record; (void)buffer; (void)length;
    if (!result) return EINVAL;
    *result = NULL;
    return ENOSYS;
}
int bs_android_process_prctl(int option, ...) {
    (void)option;
    errno = ENOSYS;
    return -1;
}
int bs_android_process_getpriority(int which, uint32_t who) {
    (void)which; (void)who;
    errno = ENOSYS;
    return -1;
}
int bs_android_process_setpriority(int which, uint32_t who, int value) {
    (void)which; (void)who; (void)value;
    errno = ENOSYS;
    return -1;
}
int bs_android_process_tcgetattr(int fd, void *termios_out) {
    (void)fd; (void)termios_out;
    errno = ENOTTY;
    return -1;
}
int bs_android_process_tcsetattr(int fd, int action, const void *termios_value) {
    (void)fd; (void)action; (void)termios_value;
    errno = ENOTTY;
    return -1;
}
int bs_android_process_mlock(const void *address, size_t length) {
    (void)address; (void)length;
    errno = ENOSYS;
    return -1;
}
unsigned bs_android_process_if_nametoindex(const char *name) {
    (void)name;
    errno = ENXIO;
    return 0;
}
FILE *bs_android_process_popen(const char *command, const char *mode) {
    (void)command; (void)mode;
    errno = ENOSYS;
    return NULL;
}
int bs_android_process_pclose(FILE *stream) {
    (void)stream;
    errno = ECHILD;
    return -1;
}
