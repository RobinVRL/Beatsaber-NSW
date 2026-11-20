/* Host regression that compiles the real unmodified source/bionic_mmap.c.
 *
 * Ubuntu/WSL:
 * gcc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror \
 *   -Itests/madvise_host_stubs -Isource \
 *   tests/test_bionic_madvise_regression.c -o /tmp/bs_madvise_regression
 * /tmp/bs_madvise_regression
 *
 * This test deliberately owns its platform stubs and memory fixtures; it
 * never replaces bs_madvise or modifies production bridge sources.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/bionic_mmap.c"

int debugPrintf(char *format, ...) {
    (void)format;
    return 0;
}

static void expect_pattern(const uint8_t *bytes, size_t count, uint8_t value) {
    for (size_t i = 0; i < count; ++i)
        assert(bytes[i] == value);
}

static void expect_failure(void *address, size_t length, int advice,
                           int expected_errno) {
    errno = 0;
    assert(bs_madvise(address, length, advice) == -1);
    assert(errno == expected_errno);
}

static void fallback_owned_mapping(void) {
    const size_t page = BS_PAGE_SIZE;
    uint8_t *const base =
        bs_mmap(NULL, page * 2, BS_PROT_READ | BS_PROT_WRITE,
                BS_MAP_PRIVATE | BS_MAP_ANONYMOUS, -1, 0);
    assert(base != (void *)-1 && base != NULL);
    assert(((uintptr_t)base & (page - 1)) == 0);

    memset(base, 0xa5, page);
    memset(base + page, 0x3c, page);

    /* Android MADV_NORMAL is an advisory reset: owned memory survives. */
    assert(bs_madvise(base, 2 * page, BS_MADV_NORMAL) == 0);
    assert(bs_madvise(base + page, page, BS_MADV_NORMAL) == 0);
    expect_pattern(base, page, 0xa5);
    expect_pattern(base + page, page, 0x3c);

    /* Ownership is checked BEFORE a successful no-op is reported. */
    expect_failure(base, 3 * page, BS_MADV_NORMAL, ENOMEM);
    expect_failure(base + 1, page, BS_MADV_NORMAL, EINVAL);
    expect_failure(NULL, page, BS_MADV_NORMAL, EINVAL);
    expect_failure(base, page, 123, EINVAL);
    expect_failure(base, page, -1, EINVAL);
    expect_pattern(base, page, 0xa5);
    expect_pattern(base + page, page, 0x3c);

    void *unowned = memalign(page, page);
    assert(unowned != NULL);
    memset(unowned, 0x5e, page);
    expect_failure(unowned, page, BS_MADV_NORMAL, ENOMEM);
    expect_failure(unowned, page, BS_MADV_DONTNEED, ENOMEM);
    expect_pattern(unowned, page, 0x5e);
    free(unowned);

    /* DONTNEED remains a destructive operation on the fallback heap map.
     * A normal-advice call must not accidentally acquire those semantics. */
    assert(bs_madvise(base + page, page, BS_MADV_DONTNEED) == 0);
    expect_pattern(base, page, 0xa5);
    expect_pattern(base + page, page, 0);

    uintptr_t old_address = (uintptr_t)base;
    assert(bs_munmap(base, 2 * page) == 0);
    expect_failure((void *)old_address, page, BS_MADV_NORMAL, ENOMEM);
}

static void virtual_window_owned_mapping(void) {
    const size_t page = BS_PAGE_SIZE;
    const size_t pages = 3;
    uint8_t *base = memalign(page, pages * page);
    uint8_t *used = calloc(pages, 1);
    uint32_t *source = malloc(pages * sizeof *source);
    assert(base && used && source);

    /* Seed genuine bridge ownership metadata. One page is committed, one
     * reserved but not yet backed, and one lies inside the window unowned. */
    memset(base, 0x57, page);
    memset(base + page, 0x29, page);
    memset(base + 2 * page, 0xe1, page);
    used[0] = used[1] = 1;
    source[0] = 0;
    source[1] = source[2] = BS_NO_PAGE;
    g_window.base = base;
    g_window.page_count = pages;
    g_window.used = used;
    g_window.source = source;
    g_window.ready = true;
    g_window.initialized = true;

    assert(bs_madvise(base, 2 * page, BS_MADV_NORMAL) == 0);
    expect_pattern(base, page, 0x57);
    expect_pattern(base + page, page, 0x29);
    expect_failure(base, 3 * page, BS_MADV_NORMAL, ENOMEM);
    expect_failure(base + 2 * page, page, BS_MADV_NORMAL, ENOMEM);
    expect_failure(base, page, 42, EINVAL);
    expect_pattern(base, page, 0x57);
    expect_pattern(base + page, page, 0x29);
    expect_pattern(base + 2 * page, page, 0xe1);

    /* Existing window DONTNEED clears only committed pages. For untouched
     * reservations it must not commit, allocate, or alter the source map. */
    assert(bs_madvise(base, 2 * page, BS_MADV_DONTNEED) == 0);
    expect_pattern(base, page, 0);
    expect_pattern(base + page, page, 0x29);
    assert(source[0] == 0 && source[1] == BS_NO_PAGE);
    expect_pattern(base + 2 * page, page, 0xe1);

    g_window = (BsMmapWindow){0};
    free(source);
    free(used);
    free(base);
}

int main(void) {
    fallback_owned_mapping();
    virtual_window_owned_mapping();

    /* Empty advice follows current POSIX-compatible bridge contract, but
     * unsupported advice remains EINVAL even when length is zero. */
    assert(bs_madvise(NULL, 0, BS_MADV_NORMAL) == 0);
    expect_failure(NULL, 0, 21, EINVAL);
    puts("PASS: owned MADV_NORMAL preserves data; DONTNEED separate; invalid/foreign requests rejected");
    return 0;
}
