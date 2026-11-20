/*
 * Portable host tests for the Bionic fortify ABI adapters.
 * No original Unity libraries, libnx services, or game files are required.
 *
 * Linux: cc -std=c11 -Wall -Wextra -Werror -Isource \
 *   tests/test_bionic_fortify.c source/bionic_fortify.c -o test_bionic_fortify
 */
#include "bionic_fortify.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>

#ifndef _WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int opened, written, streamed, positioned;
static int open_flags, write_fd;
static int64_t write_offset;
static const char *open_path;
static const void *write_buf;
static size_t write_bytes;
static FILE *fwrite_stream;

int debugPrintf(char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int bs_android_open(const char *path, int flags, ...)
{
    opened++;
    open_path = path;
    open_flags = flags;
    return 42;
}

ssize_t bs_pipe_write(int fd, const void *buf, size_t count)
{
    written++;
    write_fd = fd;
    write_buf = buf;
    write_bytes = count;
    return (ssize_t)count;
}

ssize_t bs_bionic_pwrite(int fd, const void *buf, size_t count, int64_t offset)
{
    positioned++;
    write_fd = fd;
    write_buf = buf;
    write_bytes = count;
    write_offset = offset;
    return (ssize_t)count;
}

size_t bs_android_fwrite(const void *p, size_t size, size_t count, FILE *stream)
{
    streamed++;
    write_buf = p;
    write_bytes = size * count;
    fwrite_stream = stream;
    return count;
}

static int print_chk(char *dst, size_t size, size_t cap,
                     const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = bs_fortify_vsnprintf(dst, size, 0, cap, format, ap);
    va_end(ap);
    return n;
}

static void test_success(void)
{
    char buffer[16] = {0};
    assert(bs_fortify_memcpy(buffer, "hello", 6, sizeof buffer) == buffer);
    assert(!strcmp(buffer, "hello"));
    assert(bs_fortify_memmove(buffer + 1, buffer, 5, sizeof buffer - 1)
           == buffer + 1);
    assert(!memcmp(buffer, "hhello", 6));
    assert(bs_fortify_memset(buffer + 1, 'a', 2, 15) == buffer + 1);
    assert(!memcmp(buffer, "haallo", 6));

    assert(bs_fortify_strlen("hello", 6) == 5);
    const char letters[] = "abc";
    assert(bs_fortify_strchr(letters, 'b', 4) == letters + 1);
    assert(bs_fortify_strchr(letters, 'z', 4) == NULL);
    assert(*bs_fortify_strchr(letters, 0, 4) == '\0');
    char word[8] = "a";
    assert(bs_fortify_strncat(word, "bcde", 2, sizeof word) == word);
    assert(!strcmp(word, "abc"));
    assert(bs_fortify_strncat(word, "z", 0, sizeof word) == word);
    assert(!strcmp(word, "abc"));
    assert(bs_fortify_open2("assets/bin/Data/boot.config", 0x80000) == 42);
    assert(opened == 1 && open_flags == 0x80000);
    assert(!strcmp(open_path, "assets/bin/Data/boot.config"));
    assert(bs_fortify_write(0x4001, buffer, 3, sizeof buffer) == 3);
    assert(written == 1 && write_fd == 0x4001 &&
           write_buf == buffer && write_bytes == 3);
    assert(bs_fortify_pwrite(7, buffer, 4, 123, sizeof buffer) == 4);
    assert(positioned == 1 && write_fd == 7 && write_buf == buffer &&
           write_bytes == 4 && write_offset == 123);
    FILE *guest_stream = (FILE *)(uintptr_t)0x12340;
    assert(bs_fortify_fwrite(buffer, 2, 3, guest_stream, sizeof buffer) == 3);
    assert(streamed == 1 && fwrite_stream == guest_stream &&
           write_buf == buffer && write_bytes == 6);
    assert(print_chk(buffer, sizeof buffer, sizeof buffer, "%s %d", "go", 9) == 4);
    assert(!strcmp(buffer, "go 9"));
    assert(print_chk(buffer, 3, sizeof buffer, "abcd") == 4);
    assert(!strcmp(buffer, "ab"));
}

#ifndef _WIN32
static void provoke_abort(int test)
{
    char out[4] = "abc";
    char src[4] = {'a', 'b', 'c', 'd'};
    switch (test) {
    case 0:  (void)bs_fortify_memcpy(out, src, 5, sizeof out); break;
    case 1:  (void)bs_fortify_memmove(out, src, 5, sizeof out); break;
    case 2:  (void)bs_fortify_memset(out, 0, 5, sizeof out); break;
    case 3:  (void)bs_fortify_fwrite(src, SIZE_MAX, 2, stdout, SIZE_MAX); break;
    case 4:  (void)bs_fortify_fwrite(src, 3, 2, stdout, 5); break;
    case 5:  (void)bs_fortify_strlen(src, 4); break;
    case 6:  (void)bs_fortify_strchr(src, 'z', 4); break;
    case 7:  (void)bs_fortify_strncat(out, "z", 1, sizeof out); break;
    case 8:  (void)bs_fortify_open2("a", 0x40); break;
    case 9:  (void)bs_fortify_open2("a", 0x410000); break;
    case 10: (void)print_chk(out, 5, sizeof out, "a"); break;
    case 11: (void)bs_fortify_write(1, src, 3, 2); break;
    case 12: (void)bs_fortify_write(1, src, SIZE_MAX, SIZE_MAX); break;
    case 13: (void)bs_fortify_pwrite(1, src, 5, 0, sizeof src); break;
    }
}

static void test_failures(void)
{
    /* Aborting is part of the ABI contract; assert it from isolated children. */
    struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    fflush(NULL);
    int failures = 0;
    for (int test = 0; test < 14; ++test) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            FILE *quiet = freopen("/dev/null", "w", stderr);
            if (!quiet) _exit(101);
            provoke_abort(test);
            _exit(100);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
            fprintf(stderr, "fortify negative test %d did not abort\n", test);
            failures++;
        }
    }
    if (failures) {
        fprintf(stderr, "%d of 14 fortify abort checks failed\n", failures);
        exit(EXIT_FAILURE);
    }
}
#endif

int main(void)
{
    test_success();
#ifndef _WIN32
    test_failures();
#endif
    puts("Bionic fortify adapter tests passed");
    return 0;
}
