/* Honest failure contracts for Android process services unavailable on Horizon.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_ANDROID_PROCESS_BRIDGE_H
#define BS_ANDROID_PROCESS_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

struct passwd;
pid_t bs_android_process_fork(void);
int bs_android_process_execl(const char *path, const char *arg0, ...);
pid_t bs_android_process_waitpid(pid_t pid, int *status, int options);
int bs_android_process_kill(pid_t pid, int signal_number);
long bs_android_process_ptrace(long request, long pid, void *address, void *data);
struct passwd *bs_android_process_getpwuid(uint32_t uid);
int bs_android_process_getpwuid_r(uint32_t uid, struct passwd *record,
                                  char *buffer, size_t length,
                                  struct passwd **result);
int bs_android_process_prctl(int option, ...);
int bs_android_process_getpriority(int which, uint32_t who);
int bs_android_process_setpriority(int which, uint32_t who, int value);
int bs_android_process_tcgetattr(int fd, void *termios_out);
int bs_android_process_tcsetattr(int fd, int action, const void *termios_value);
int bs_android_process_mlock(const void *address, size_t length);
unsigned bs_android_process_if_nametoindex(const char *name);
FILE *bs_android_process_popen(const char *command, const char *mode);
int bs_android_process_pclose(FILE *stream);
#endif
