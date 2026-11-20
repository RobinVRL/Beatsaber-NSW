/* Tests the public Android locale bridge against a real POSIX locale
 * implementation. Use -DBS_BIONIC_LOCALE_TEST_HOST on glibc, otherwise
 * compile the same source with devkitA64 as an ABI/prototype check.
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "../source/bionic_locale_bridge.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* The integrated guest probe logs to the Switch loader; for host tests use
 * the identical signature and direct diagnostic text to stderr. */
int debugPrintf(char *format, ...);
int debugPrintf(char *format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vfprintf(stderr, format, args);
    va_end(args);
    return result;
}

static void check_category_translation(void) {
    assert(bs_bionic_locale_category_to_native(0) == LC_CTYPE);
    assert(bs_bionic_locale_category_to_native(1) == LC_NUMERIC);
    assert(bs_bionic_locale_category_to_native(2) == LC_TIME);
    assert(bs_bionic_locale_category_to_native(3) == LC_COLLATE);
    assert(bs_bionic_locale_category_to_native(4) == LC_MONETARY);
    assert(bs_bionic_locale_category_to_native(5) == LC_MESSAGES);
    assert(bs_bionic_locale_category_to_native(6) == LC_ALL);
    assert(bs_bionic_locale_mask_to_native(0) == 0);
    assert(bs_bionic_locale_mask_to_native(1 << 0) == LC_CTYPE_MASK);
    assert(bs_bionic_locale_mask_to_native(1 << 1) == LC_NUMERIC_MASK);
    assert(bs_bionic_locale_mask_to_native(1 << 2) == LC_TIME_MASK);
    assert(bs_bionic_locale_mask_to_native(1 << 3) == LC_COLLATE_MASK);
    assert(bs_bionic_locale_mask_to_native(1 << 4) == LC_MONETARY_MASK);
    assert(bs_bionic_locale_mask_to_native(1 << 5) == LC_MESSAGES_MASK);
    assert(bs_bionic_locale_mask_to_native(0x3f) == LC_ALL_MASK);
    errno = 0;
    assert(bs_bionic_locale_category_to_native(7) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_locale_category_to_native(-1) == -1 && errno == EINVAL);
    errno = 0;
    assert(bs_bionic_locale_mask_to_native(1 << 7) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_locale_mask_to_native(-1) == -1 && errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_setlocale(7, "C") == NULL && errno == ENOTSUP);
}

static void check_locale_services(void) {
    locale_t c = bs_bionic_newlocale(0x3f, "C", (locale_t)0);
    assert(c != (locale_t)0);
    assert(bs_bionic_localeconv() != NULL);

    assert(bs_bionic_iswalpha_l(L'A', c) != 0);
    assert(bs_bionic_iswalpha_l(L'7', c) == 0);
    assert(bs_bionic_iswblank_l(L' ', c) != 0);
    assert(bs_bionic_iswcntrl_l(L'\n', c) != 0);
    assert(bs_bionic_iswdigit_l(L'7', c) != 0);
    assert(bs_bionic_iswlower_l(L'a', c) != 0);
    assert(bs_bionic_iswprint_l(L'A', c) != 0);
    assert(bs_bionic_iswpunct_l(L'!', c) != 0);
    assert(bs_bionic_iswspace_l(L'\t', c) != 0);
    assert(bs_bionic_iswupper_l(L'A', c) != 0);
    assert(bs_bionic_iswxdigit_l(L'F', c) != 0);
    assert(bs_bionic_towlower_l(L'A', c) == L'a');
    assert(bs_bionic_towupper_l(L'a', c) == L'A');

    assert(bs_bionic_isdigit_l('7', c) != 0);
    assert(bs_bionic_islower_l('a', c) != 0);
    assert(bs_bionic_isupper_l('A', c) != 0);
    assert(bs_bionic_isxdigit_l('F', c) != 0);
    assert(bs_bionic_tolower_l('A', c) == 'a');
    assert(bs_bionic_toupper_l('a', c) == 'A');
    assert(bs_bionic_tolower_l(EOF, c) == EOF);

    char *end = NULL;
    assert(bs_bionic_strtof_l("2.5ms", &end, c) == 2.5f && !strcmp(end, "ms"));
    assert(bs_bionic_strtod_l("2.5ms", &end, c) == 2.5 && !strcmp(end, "ms"));
    assert(bs_bionic_strtold_l("2.5ms", &end, c) == 2.5L && !strcmp(end, "ms"));
    assert(bs_bionic_strtoll_l("-9223372036854775807!", &end, 10, c)
           == -LLONG_MAX && !strcmp(end, "!"));
    assert(bs_bionic_strtoull_l("18446744073709551615!", &end, 10, c)
           == ULLONG_MAX && !strcmp(end, "!"));
    assert(bs_bionic_strcoll_l("test", "test", c) == 0);
    assert(bs_bionic_wcscoll_l(L"test", L"test", c) == 0);
    char collated[16];
    wchar_t wide_collated[16];
    assert(bs_bionic_strxfrm_l(collated, "test", sizeof collated, c) == 4);
    assert(!strcmp(collated, "test"));
    assert(bs_bionic_wcsxfrm_l(wide_collated, L"test", 16, c) == 4);
    assert(!wcscmp(wide_collated, L"test"));

    const struct tm when = {.tm_year = 126, .tm_mon = 10, .tm_mday = 15};
    char date[32];
    assert(bs_bionic_strftime_l(date, sizeof date, "%Y-%m-%d", &when, c) == 10);
    assert(!strcmp(date, "2026-11-15"));

    assert(bs_bionic_btowc('A') == L'A');
    assert(bs_bionic_wctob(L'A') == 'A');
    wchar_t out = 0;
    assert(bs_bionic_mbtowc(&out, "A", 1) == 1 && out == L'A');

    locale_t copy = bs_bionic_duplocale(c);
    assert(copy != (locale_t)0);
    locale_t previous = bs_bionic_uselocale(c);
    assert(previous != (locale_t)0);
    assert(bs_bionic_uselocale((locale_t)0) == c);
    assert(bs_bionic_uselocale(previous) == c);
    bs_bionic_freelocale(copy);
    bs_bionic_freelocale(c);
    assert(bs_bionic_locale_abi_selftest());
}

static void check_fail_closed(void) {
    errno = 0;
    assert(bs_bionic_newlocale(0x3f, NULL, (locale_t)0) == (locale_t)0);
    assert(errno == EINVAL);
    errno = 0;
    assert(bs_bionic_newlocale(0x3f, "C", LC_GLOBAL_LOCALE) == (locale_t)0);
    assert(errno == EINVAL);
    errno = 0;
    assert(bs_bionic_newlocale(0x40, "C", (locale_t)0) == (locale_t)0);
    assert(errno == ENOTSUP);
    errno = 0;
    assert(bs_bionic_iswalpha_l(L'A', (locale_t)0) == 0);
    assert(errno == EINVAL);
    errno = 0;
    assert(bs_bionic_towlower_l(L'A', LC_GLOBAL_LOCALE) == WEOF);
    assert(errno == EINVAL);
    errno = 0;
    assert(bs_bionic_tolower_l(1000, LC_GLOBAL_LOCALE) == EOF);
    assert(errno == EINVAL);
    char *end = NULL;
    errno = 0;
    assert(bs_bionic_strtod_l("3.14", &end, (locale_t)0) == 0.0);
    assert(errno == EINVAL && end && !strcmp(end, "3.14"));
    errno = 0;
    bs_bionic_freelocale((locale_t)0);
    assert(errno == EINVAL);
    char tiny[3];
    assert(bs_bionic_strerror_r(ENOENT, tiny, sizeof tiny) == ERANGE);
    assert(bs_bionic_strerror_r(ENOENT, NULL, 0) == EINVAL);
}

int main(void) {
    check_category_translation();
    check_locale_services();
    check_fail_closed();
    puts("PASS: Bionic locale categories, newlocale lifecycle, _l APIs, and errors");
    return 0;
}
