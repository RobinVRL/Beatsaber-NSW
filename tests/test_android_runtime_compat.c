/* Focused behavior tests for guest fnmatch and Linux cmsghdr navigation.
 * Link with -ffunction-sections -Wl,--gc-sections for independent host test.
 */
#include "../source/android_runtime_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int errors;
static void check(int success, const char *description) {
    ++checks;
    if (!success) {
        ++errors;
        fprintf(stderr, "FAIL: %s\n", description);
    }
}

int main(void) {
    check(bs_android_runtime_fnmatch("file*.txt", "file123.txt", 0) == 0,
          "wildcard and suffix");
    check(bs_android_runtime_fnmatch("file?.txt", "file123.txt", 0) != 0,
          "question mark consumes one character");
    check(bs_android_runtime_fnmatch("[a-c]at", "bat", 0) == 0,
          "bracket range");
    check(bs_android_runtime_fnmatch("[!a-c]at", "dat", 0) == 0,
          "negated bracket range");
    check(bs_android_runtime_fnmatch("[!a-c]at", "bat", 0) != 0,
          "negated bracket rejects range");
    check(bs_android_runtime_fnmatch("foo/*", "foo/bar/baz", 1) != 0,
          "FNM_PATHNAME blocks slash in wildcard");
    check(bs_android_runtime_fnmatch("foo/*", "foo/bar", 1) == 0,
          "FNM_PATHNAME permits single segment");
    check(bs_android_runtime_fnmatch("*.cfg", ".secret.cfg", 4) != 0,
          "FNM_PERIOD blocks leading dot");
    check(bs_android_runtime_fnmatch("A*.CFG", "abc.cfg", 16) == 0,
          "FNM_CASEFOLD matches case-insensitively");
    check(bs_android_runtime_fnmatch("foo", "foo/bar", 8) == 0,
          "FNM_LEADING_DIR permits trailing directory");

    uint64_t entries[6] = {0};
    entries[0] = 16;
    entries[2] = 16;
    BsAndroidMsghdr message = {0};
    message.msg_control = entries;
    message.msg_controllen = 32;
    check(bs_android_runtime_cmsg_nxthdr(&message, &entries[0]) == &entries[2],
          "walk Linux 8-byte aligned control messages");
    check(bs_android_runtime_cmsg_nxthdr(&message, &entries[2]) == NULL,
          "stop at end of control buffer");
    entries[0] = 0;
    check(bs_android_runtime_cmsg_nxthdr(&message, &entries[0]) == NULL,
          "reject malformed zero-length control message");
    message.msg_controllen = 8;
    check(bs_android_runtime_cmsg_nxthdr(&message, &entries[0]) == NULL,
          "reject truncated header");

    printf("Android runtime compatibility: %d checks, %d failed\n", checks, errors);
    return errors ? 1 : 0;
}
