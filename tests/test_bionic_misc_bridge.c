/* AArch64 native bridge unit test; compiles on a host as well.
 * SPDX-License-Identifier: MIT */
#include "../source/bionic_misc_bridge.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_log[1024];
int debugPrintf(char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(last_log, sizeof last_log, fmt, args);
    va_end(args);
    return n;
}

static int test_format(char **output, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int length = bs_bionic_vasprintf(output, fmt, args);
    va_end(args);
    return length;
}

int main(void) {
    char *formatted = (char *)0x1;
    assert(test_format(&formatted, "Quest %s %d", "Unity", 6) == 13);
    assert(formatted && !strcmp(formatted, "Quest Unity 6"));
    free(formatted);
    formatted = (char *)0x1;
    errno = 0;
    assert(test_format(&formatted, NULL) == -1);
    assert(formatted == NULL && errno == EINVAL);
    bs_bionic_openlog("Unity", 0, 0);
    bs_bionic_syslog(3, "startup failure: %s", "unsupported XR");
    assert(strstr(last_log, "Unity") != NULL);
    assert(strstr(last_log, "startup failure: unsupported XR") != NULL);
    bs_bionic_closelog();
    errno = 0;
    assert(bs_bionic_raise(9999) == -1);
    assert(errno == ENOSYS);
    puts("PASS: Bionic vasprintf/syslog/signal error contracts");
    return 0;
}
