/* Guest Bionic mbstate and UTF-8 semantics across every public bridge API.
 * Build with -std=c11 -Wall -Wextra -Werror, without host wchar_t reliance.
 * SPDX-License-Identifier: MIT
 */
#include "../source/bionic_multibyte_bridge.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BAD ((size_t)-1)
#define PARTIAL ((size_t)-2)

static void state_layout(void) {
    assert(sizeof(bs_bionic_mbstate_t) == 8);
    bs_bionic_mbstate_t s = {{0}, {0xaa, 0xbb, 0xcc, 0xdd}};
    assert(bs_bionic_mbsinit(NULL) && bs_bionic_mbsinit(&s));
    uint32_t cp = 0;
    assert(bs_bionic_mbrtowc(&cp, "\xf0", 1, &s) == PARTIAL);
    assert(s.seq[0] == 0xf0 && s.seq[1] == 0 && !bs_bionic_mbsinit(&s));
    assert(bs_bionic_mbrtowc(&cp, "\x9f", 1, &s) == PARTIAL);
    assert(s.seq[1] == 0x9f);
    assert(bs_bionic_mbrtowc(&cp, "\x8c\x8d", 2, &s) == 2);
    assert(cp == 0x1f30du && bs_bionic_mbsinit(&s));
    assert(s.reserved[0] == 0xaa && s.reserved[1] == 0xbb);
    assert(s.reserved[2] == 0xcc && s.reserved[3] == 0xdd);

    assert(bs_bionic_mbrtowc(&cp, "", 0, &s) == PARTIAL);
    assert(bs_bionic_mbrtowc(&cp, "A", 1, &s) == 1 && cp == 'A');
    assert(bs_bionic_mbrtowc(&cp, "\0", 1, &s) == 0 && cp == 0);
    assert(bs_bionic_mbrlen("\xe2\x82\xac", 3, &s) == 3);
    assert(bs_bionic_mbrtoc32(&cp, "\xc2\xa2", 2, &s) == 2 && cp == 0xa2);
    assert(bs_bionic_mbrlen(NULL, 0, &s) == 0);
}

static void decoding_errors(void) {
    const char *invalid[] = {
        "\x80", "\xbf", "\xf8\x88\x80\x80\x80", "\xff",
        "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80", "\xf0\x80\x80\x80",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xf4\x90\x80\x80",
        "\xe2X", "\xc2X"
    };
    bs_bionic_mbstate_t s = {{0}, {0}};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        errno = 0;
        assert(bs_bionic_mbrtowc(NULL, invalid[i], strlen(invalid[i]), &s) == BAD);
        assert(errno == EILSEQ && bs_bionic_mbsinit(&s));
    }
    uint32_t cp = 0;
    s.seq[0] = 0xe2;
    assert(bs_bionic_mbrtowc(&cp, "A", 1, &s) == BAD);
    assert(bs_bionic_mbsinit(&s));
    s.seq[3] = 1;
    errno = 0;
    assert(bs_bionic_mbrtowc(&cp, "A", 1, &s) == BAD);
    assert(errno == EINVAL && bs_bionic_mbsinit(&s));
    s.seq[0] = 0xf0;
    assert(bs_bionic_mbrtowc(&cp, NULL, 100, &s) == BAD);
    assert(bs_bionic_mbsinit(&s));
    assert(bs_bionic_mbrtowc(&cp, "\xf4\x8f\xbf\xbf", 4, &s) == 4);
    assert(cp == 0x10ffffu);
}

static void encoding(void) {
    bs_bionic_mbstate_t s = {{0}, {1, 2, 3, 4}};
    struct case_pair { uint32_t cp; const char *utf8; size_t n; } cases[] = {
        {0, "", 1}, {'A', "A", 1}, {0x7f, "\x7f", 1},
        {0xa2, "\xc2\xa2", 2}, {0x20ac, "\xe2\x82\xac", 3},
        {0x1f30d, "\xf0\x9f\x8c\x8d", 4},
        {0x10ffff, "\xf4\x8f\xbf\xbf", 4}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char out[8] = {0};
        assert(bs_bionic_wcrtomb(out, cases[i].cp, &s) == cases[i].n);
        assert(memcmp(out, cases[i].utf8, cases[i].n) == 0);
        assert(bs_bionic_mbsinit(&s));
    }
    assert(s.reserved[0] == 1 && s.reserved[3] == 4);
    const uint32_t invalid[] = {0xd800, 0xdfff, 0x110000, UINT32_MAX};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        char out[8];
        errno = 0;
        assert(bs_bionic_wcrtomb(out, invalid[i], &s) == BAD);
        assert(errno == EILSEQ && bs_bionic_mbsinit(&s));
    }
    s.seq[0] = 0xe2;
    errno = 0;
    char out[8];
    assert(bs_bionic_wcrtomb(out, 'A', &s) == BAD);
    assert(errno == EILSEQ && bs_bionic_mbsinit(&s));
    s.seq[0] = 0xe2;
    assert(bs_bionic_wcrtomb(NULL, 0x1f30d, &s) == 1 && bs_bionic_mbsinit(&s));
    assert(bs_bionic_c32rtomb(out, 0x20ac, &s) == 3);
    assert(memcmp(out, "\xe2\x82\xac", 3) == 0);
}

static void wide_input(void) {
    const uint32_t text[] = {'A', 0x20ac, 0x1f30d, 0};
    const uint32_t *src = text;
    bs_bionic_mbstate_t s = {{0}, {0}};
    char dst[20] = {0};
    assert(bs_bionic_wcsnrtombs(dst, &src, 4, 1, &s) == 1);
    assert(src == text + 1 && dst[0] == 'A');
    assert(bs_bionic_wcsnrtombs(dst + 1, &src, 4, 2, &s) == 0);
    assert(src == text + 1);
    assert(bs_bionic_wcsrtombs(dst + 1, &src, sizeof dst - 1, &s) == 7);
    assert(src == NULL && !memcmp(dst, "A\xe2\x82\xac\xf0\x9f\x8c\x8d", 8));
    assert(dst[8] == 0);

    src = text;
    assert(bs_bionic_wcsnrtombs(NULL, &src, 3, 0, &s) == 8);
    assert(src == text);
    src = text;
    assert(bs_bionic_wcsrtombs(NULL, &src, 0, &s) == 8);
    assert(src == text);
    assert(bs_bionic_wcstombs(dst, text, sizeof dst) == 8);
    assert(bs_bionic_wcstombs(NULL, text, 0) == 8);

    src = text;
    assert(bs_bionic_wcsrtombs(dst, &src, 0, &s) == 0 && src == text);
    assert(bs_bionic_mbsinit(&s));
    const uint32_t invalid_first[] = {0xd800, 0};
    src = invalid_first;
    assert(bs_bionic_wcsrtombs(dst, &src, 0, &s) == 0 &&
           src == invalid_first);

    const uint32_t bad[] = {'A', 0xd800, 0};
    src = bad;
    errno = 0;
    assert(bs_bionic_wcsrtombs(dst, &src, sizeof dst, &s) == BAD);
    assert(errno == EILSEQ && src == bad + 1);
    src = text;
    s.seq[0] = 0xe2;
    assert(bs_bionic_wcsnrtombs(dst, &src, 4, sizeof dst, &s) == BAD);
    assert(errno == EILSEQ && bs_bionic_mbsinit(&s));
}

static void narrow_input(void) {
    const char mixed[] = "A\xe2\x82\xac\xf0\x9f\x8c\x8d";
    const char *src = mixed;
    bs_bionic_mbstate_t s = {{0}, {0}};
    uint32_t dst[10] = {0};
    assert(bs_bionic_mbsnrtowcs(dst, &src, 1, 10, &s) == 1);
    assert(src == mixed + 1 && dst[0] == 'A');
    assert(bs_bionic_mbsnrtowcs(dst + 1, &src, 1, 10, &s) == 0);
    assert(src == mixed + 2 && s.seq[0] == 0xe2);
    assert(bs_bionic_mbsnrtowcs(dst + 1, &src, 2, 10, &s) == 1);
    assert(src == mixed + 4 && dst[1] == 0x20ac && bs_bionic_mbsinit(&s));
    assert(bs_bionic_mbsrtowcs(dst + 2, &src, 2, &s) == 1);
    assert(src == NULL && dst[2] == 0x1f30d && dst[3] == 0);

    src = mixed;
    assert(bs_bionic_mbsrtowcs(NULL, &src, 0, &s) == 3);
    assert(src == mixed && bs_bionic_mbsinit(&s));
    src = mixed;
    assert(bs_bionic_mbsnrtowcs(NULL, &src, 4, 0, &s) == 2);
    assert(src == mixed);
    src = mixed;
    assert(bs_bionic_mbsrtowcs(dst, &src, 1, &s) == 1 && src == mixed + 1);
    assert(bs_bionic_mbstowcs(dst, mixed, 10) == 3);
    assert(bs_bionic_mbstowcs(NULL, mixed, 0) == 3);

    const char bad[] = "A\xe2X";
    src = bad;
    errno = 0;
    assert(bs_bionic_mbsnrtowcs(dst, &src, 4, 10, &s) == BAD);
    assert(errno == EILSEQ && src == bad + 1 && bs_bionic_mbsinit(&s));
}

static void legacy(void) {
    uint32_t cp = 0;
    assert(bs_bionic_mbtowc_compat(&cp, "\xe2\x82\xac", 3) == 3);
    assert(cp == 0x20ac);
    assert(bs_bionic_mbtowc_compat(&cp, "\xf0", 1) == -1 && errno == EILSEQ);
    assert(bs_bionic_mbtowc_compat(&cp, NULL, 0) == 0);
    assert(bs_bionic_mblen("A", 1) == 1);
    assert(bs_bionic_mblen(NULL, 0) == 0);
    char out[8] = {0};
    assert(bs_bionic_wctomb(out, 0x1f30d) == 4);
    assert(!memcmp(out, "\xf0\x9f\x8c\x8d", 4));
    assert(bs_bionic_wctomb(NULL, 0) == 0);
    assert(bs_bionic_wctomb(out, 0x110000) == -1 && errno == EILSEQ);
}

int main(void) {
    state_layout();
    decoding_errors();
    encoding();
    wide_input();
    narrow_input();
    legacy();
    puts("PASS: Android AArch64 mbstate ABI, split UTF-8, bounded strings, and errors");
    return 0;
}
