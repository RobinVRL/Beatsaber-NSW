/* Host-only isolated tests for Android's __strncpy_chk2 five-argument ABI.
 * Build: gcc -std=c11 -Wall -Wextra -Werror -Isource
 *       tests/test_bionic_strncpy_chk2.c source/bionic_strncpy_chk2.c -o test
 * Negative SIGABRT tests require POSIX fork/waitpid (WSL Ubuntu works).
 */
#include "bionic_strncpy_chk2.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void success_cases(void)
{
    /* NUL and remaining padding all count against the destination extent. */
    char padded[8] = {'?', '?', '?', '?', '?', '?', '?', '?'};
    const char short_src[] = "Hi";
    assert(bs_fortify_strncpy_chk2(padded, short_src, 6, 8, 3) == padded);
    assert(memcmp(padded, "Hi\0\0\0\0??", sizeof padded) == 0);

    /* No NUL is required if n source bytes are available. */
    const char no_nul[4] = {'A', 'B', 'C', 'D'};
    char exact[5] = {'?', '?', '?', '?', '#'};
    assert(bs_fortify_strncpy_chk2(exact, no_nul, 4, 4, 4) == exact);
    assert(memcmp(exact, "ABCD#", sizeof exact) == 0);

    /* Reading the final source byte (which is NUL) must succeed. */
    const char terminal_nul[] = {'x', '\0'};
    char terminal_out[5] = {'?', '?', '?', '?', '?'};
    assert(bs_fortify_strncpy_chk2(terminal_out, terminal_nul, 5, 5, 2)
           == terminal_out);
    assert(memcmp(terminal_out, "x\0\0\0\0", 5) == 0);

    /* Source can be shorter than n if its NUL comes before src_len. */
    char single[4] = {'?', '?', '?', '!'};
    const char nul[] = {'\0'};
    assert(bs_fortify_strncpy_chk2(single, nul, 3, 4, 1) == single);
    assert(memcmp(single, "\0\0\0!", 4) == 0);

    /* A zero-length copy must not touch either pointer or destination. */
    char untouched[] = "ok";
    assert(bs_fortify_strncpy_chk2(untouched, NULL, 0, 0, 0) == untouched);
    assert(bs_fortify_strncpy_chk2(NULL, NULL, 0, 0, 0) == NULL);
    assert(strcmp(untouched, "ok") == 0);

    /* A single source byte is valid without a terminator. */
    char one[2] = {'?', '!'};
    assert(bs_fortify_strncpy_chk2(one, no_nul, 1, 1, 1) == one);
    assert(one[0] == 'A' && one[1] == '!');
}

#ifndef _WIN32
static void fail_case(int which)
{
    char dst[4] = {'?', '?', '?', '?'};
    const char no_nul[4] = {'a', 'b', 'c', 'd'};
    switch (which) {
    case 0: (void)bs_fortify_strncpy_chk2(dst, no_nul, 5, 4, 4); break;
    case 1: (void)bs_fortify_strncpy_chk2(dst, no_nul, 4, 4, 3); break;
    case 2: (void)bs_fortify_strncpy_chk2(dst, no_nul, 1, 4, 0); break;
    case 3: (void)bs_fortify_strncpy_chk2(NULL, no_nul, 1, 4, 4); break;
    case 4: (void)bs_fortify_strncpy_chk2(dst, NULL, 1, 4, 4); break;
    case 5: (void)bs_fortify_strncpy_chk2(dst, no_nul, SIZE_MAX, 4, 4); break;
    }
}

static void abort_cases(void)
{
    const struct rlimit limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    for (int i = 0; i < 6; ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            FILE *quiet = freopen("/dev/null", "w", stderr);
            if (!quiet) _exit(101);
            fail_case(i);
            _exit(100);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
            fprintf(stderr, "FAIL expected SIGABRT from case %d (status %d)\n",
                    i, status);
            exit(EXIT_FAILURE);
        }
    }
}
#endif

int main(void)
{
    success_cases();
#ifndef _WIN32
    abort_cases();
#endif
    puts("Bionic __strncpy_chk2: success and bounds-abort tests passed");
    return 0;
}
