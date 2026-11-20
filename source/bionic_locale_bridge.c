/* Android/Bionic locale adapter for the actual original Quest libc++_shared.
 * All _l operations bind to actual newlib locale_t implementations.
 * Locale categories/masks are translated before native locale objects
 * are created. Unsupported Android-only categories fail rather than being
 * silently interpreted as unrelated newlib categories.
 *
 * Android NDK bionic libc/include/locale.h:
 * CTYPE=0, NUMERIC=1, TIME=2, COLLATE=3, MONETARY=4, MESSAGES=5,
 * ALL=6; Android has additional PAPER/NAME/ADDRESS/... 7..12.
 * devkitA64 newlib locale.h:
 * ALL=0, COLLATE=1, CTYPE=2, MONETARY=3, NUMERIC=4, TIME=5, MESSAGES=6.
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* newlib exposes the strto*_l declarations under GNU */
#endif
#include "bionic_locale_bridge.h"
#include "util.h"
#include "bionic_errno.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BS_ANDROID_LC_CTYPE = 0,
    BS_ANDROID_LC_NUMERIC = 1,
    BS_ANDROID_LC_TIME = 2,
    BS_ANDROID_LC_COLLATE = 3,
    BS_ANDROID_LC_MONETARY = 4,
    BS_ANDROID_LC_MESSAGES = 5,
    BS_ANDROID_LC_ALL = 6
};

/* Require the exact native header that the bridge was designed against.
 * A changed devkit toolchain requires a new ABI audit, not silent fallback. */
#if defined(BS_BIONIC_LOCALE_TEST_HOST)
/* Explicitly opt in to the test-only Linux/glibc backend. No host ABI is
 * ever selected by the Switch build. The test backend shares Bionic's
 * category numbering, but its locale handles remain host-private. */
#if LC_CTYPE != 0 || LC_NUMERIC != 1 || LC_TIME != 2 || LC_COLLATE != 3 || LC_MONETARY != 4 || LC_MESSAGES != 5 || LC_ALL != 6
#error "host locale layout differs from test expectations"
#endif
#elif LC_ALL != 0 || LC_COLLATE != 1 || LC_CTYPE != 2 || LC_MONETARY != 3 || LC_NUMERIC != 4 || LC_TIME != 5 || LC_MESSAGES != 6
#error "newlib locale indices have changed; audit Android category mapping"
#endif

int bs_bionic_locale_category_to_native(int android_category) {
    switch (android_category) {
        case BS_ANDROID_LC_CTYPE: return LC_CTYPE;
        case BS_ANDROID_LC_NUMERIC: return LC_NUMERIC;
        case BS_ANDROID_LC_TIME: return LC_TIME;
        case BS_ANDROID_LC_COLLATE: return LC_COLLATE;
        case BS_ANDROID_LC_MONETARY: return LC_MONETARY;
        case BS_ANDROID_LC_MESSAGES: return LC_MESSAGES;
        case BS_ANDROID_LC_ALL: return LC_ALL;
        default: errno = android_category >= 7 && android_category <= 12
                           ? ENOTSUP : EINVAL; return -1;
    }
}

int bs_bionic_locale_mask_to_native(int android_mask) {
    if (android_mask & ~0x3f) {
        /* Bionic additional LC_*_MASK categories 7..12 have no
         * corresponding newlib locale categories. */
        errno = ENOTSUP;
        return -1;
    }
    /* Newlib's LC_ALL_MASK is bit 0, whereas Bionic's LC_ALL_MASK is
     * the OR of CTYPE..MESSAGES bits (0x3f). Translating the bits
     * individually for an all-categories request loses that meaning. */
    if (android_mask == 0x3f) return LC_ALL_MASK;
    int result = 0;
    for (int i = 0; i < BS_ANDROID_LC_ALL; ++i) {
        if (android_mask & (1 << i))
            result |= 1 << bs_bionic_locale_category_to_native(i);
    }
    return result;
}

char *bs_bionic_setlocale(int android_category, const char *locale) {
    const int native_category = bs_bionic_locale_category_to_native(android_category);
    if (native_category < 0) return NULL;
    return setlocale(native_category, locale);
}

locale_t bs_bionic_newlocale(int android_category_mask, const char *locale,
                            locale_t base) {
    const int native_mask = bs_bionic_locale_mask_to_native(android_category_mask);
    if (native_mask < 0) return (locale_t)0;
    if (!locale || base == LC_GLOBAL_LOCALE) {
        errno = EINVAL;
        return (locale_t)0;
    }
    /* Newlib locale objects must have newlib's exact private ABI; only these
     * newly created handles may be passed to _l APIs or to uselocale.
     * Native newlocale will fail with its real error for unsupported names.
     */
    return newlocale(native_mask, locale, base);
}

locale_t bs_bionic_uselocale(locale_t locale) {
    /* Android and newlib both use (locale_t)-1 for LC_GLOBAL_LOCALE.
     * The null argument queries the current thread's locale. */
    return uselocale(locale);
}

locale_t bs_bionic_duplocale(locale_t locale) {
    if (!locale) { errno = EINVAL; return (locale_t)0; }
    /* POSIX permits LC_GLOBAL_LOCALE here; newlib implements that token. */
    return duplocale(locale);
}

void bs_bionic_freelocale(locale_t locale) {
    if (!locale || locale == LC_GLOBAL_LOCALE) {
        errno = EINVAL;
        return;
    }
    freelocale(locale);
}

struct lconv *bs_bionic_localeconv(void) {
    /* Both Android and newlib expose the POSIX struct lconv layout. */
    return localeconv();
}

int bs_bionic_strerror_r(int code, char *buffer, size_t size) {
    /* Bionic exposes POSIX int strerror_r, not GNU's char * strerror_r.
     * Avoid binding the native GNU ABI under an Android POSIX symbol. */
    if (!buffer || !size) return EINVAL;
    /* The guest passes a Linux errno number; see bionic_errno.h. */
    int written = snprintf(buffer, size, "%s",
                           strerror(bs_errno_from_bionic(code)));
    if (written < 0) return EINVAL;
    return (size_t)written < size ? 0 : ERANGE;
}

int bs_bionic_locale_abi_selftest(void) {
    /* Execute real native locale code on the AArch64 thread without altering
     * its global locale. This probes exactly the category/mask translation
     * imported by original libc++_shared/libOVRPlugin/libUnityOpenXR. */
    const int ctype_category = bs_bionic_locale_category_to_native(0);
    const int all_category = bs_bionic_locale_category_to_native(6);
    const int ctype_mask = bs_bionic_locale_mask_to_native(1 << 0);
    const int native_all_mask = bs_bionic_locale_mask_to_native(0x3f);
    const bool masks_ok = ctype_category == LC_CTYPE &&
                          all_category == LC_ALL &&
                          ctype_mask == LC_CTYPE_MASK &&
                          native_all_mask == LC_ALL_MASK;
    errno = 0;
    const bool reject_extra =
        bs_bionic_locale_mask_to_native(1 << 7) == -1 && errno == ENOTSUP;
    locale_t c = bs_bionic_newlocale(1 << 0, "C", (locale_t)0);
    if (!c) {
        debugPrintf("[locale:abi] category/mask=%d extra_rejected=%d "
                    "native newlocale(C) failed errno=%d\n",
                    (int)masks_ok, (int)reject_extra, errno);
        return 0;
    }
    const bool native_ok = strcoll_l("abc", "abc", c) == 0;
    bs_bionic_freelocale(c);
    char text[64];
    const bool strerror_ok =
        bs_bionic_strerror_r(EINVAL, text, sizeof text) == 0 &&
        text[0] != '\0';
    const bool result = masks_ok && reject_extra && native_ok && strerror_ok;
    debugPrintf("[locale:abi] native newlib locale(C)=%d category_masks=%d "
                "unsupported_rejected=%d POSIX_strerror=%d: %s\n",
                (int)native_ok, (int)masks_ok, (int)reject_extra,
                (int)strerror_ok, result ? "PASS" : "FAIL");
    return (int)result;
}

/* These are actual newlib locale operations. The bridge owns category/mask
 * translation and leaves all character classification and numeric parsing
 * to newlib's locale implementations. Do not fabricate UTF-8 behavior.
 */
static int bs_bionic_valid_explicit_locale(locale_t locale) {
    if (locale && locale != LC_GLOBAL_LOCALE) return 1;
    errno = EINVAL;
    return 0;
}

#define BS_BIONIC_WCLASS(name) \
    int bs_bionic_##name(wint_t value, locale_t locale) { \
        if (!bs_bionic_valid_explicit_locale(locale)) return 0; \
        return name(value, locale); \
    }
BS_BIONIC_WCLASS(iswalpha_l)
BS_BIONIC_WCLASS(iswblank_l)
BS_BIONIC_WCLASS(iswcntrl_l)
BS_BIONIC_WCLASS(iswdigit_l)
BS_BIONIC_WCLASS(iswlower_l)
BS_BIONIC_WCLASS(iswprint_l)
BS_BIONIC_WCLASS(iswpunct_l)
BS_BIONIC_WCLASS(iswspace_l)
BS_BIONIC_WCLASS(iswupper_l)
BS_BIONIC_WCLASS(iswxdigit_l)
#undef BS_BIONIC_WCLASS

wint_t bs_bionic_towlower_l(wint_t value, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return WEOF;
    return towlower_l(value, locale);
}
wint_t bs_bionic_towupper_l(wint_t value, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return WEOF;
    return towupper_l(value, locale);
}

/* Avoid newlib's ctype macros accessing out-of-domain table indices. */
static int bs_bionic_valid_byte(int value) {
    if (value == EOF || (unsigned)value <= UCHAR_MAX) return 1;
    errno = EINVAL;
    return 0;
}
#define BS_BIONIC_CLASS(name) \
    int bs_bionic_##name(int value, locale_t locale) { \
        if (!bs_bionic_valid_explicit_locale(locale) || \
            !bs_bionic_valid_byte(value)) return 0; \
        return name(value, locale); \
    }
BS_BIONIC_CLASS(isdigit_l)
BS_BIONIC_CLASS(islower_l)
BS_BIONIC_CLASS(isupper_l)
BS_BIONIC_CLASS(isxdigit_l)
#undef BS_BIONIC_CLASS

int bs_bionic_tolower_l(int value, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale) ||
        !bs_bionic_valid_byte(value)) return EOF;
    return tolower_l(value, locale);
}
int bs_bionic_toupper_l(int value, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale) ||
        !bs_bionic_valid_byte(value)) return EOF;
    return toupper_l(value, locale);
}

/* Preserve strto*_l integer and floating-point AAPCS64 return classes. */
static int bs_bionic_valid_parse(const char *source, char **end,
                                 locale_t locale) {
    if (source && bs_bionic_valid_explicit_locale(locale)) return 1;
    if (!source) errno = EINVAL;
    if (end) *end = (char *)source;
    return 0;
}
double bs_bionic_strtod_l(const char *source, char **end, locale_t locale) {
    if (!bs_bionic_valid_parse(source, end, locale)) return 0.0;
    return strtod_l(source, end, locale);
}
float bs_bionic_strtof_l(const char *source, char **end, locale_t locale) {
    if (!bs_bionic_valid_parse(source, end, locale)) return 0.0f;
    return strtof_l(source, end, locale);
}
long double bs_bionic_strtold_l(const char *source, char **end, locale_t locale) {
    if (!bs_bionic_valid_parse(source, end, locale)) return 0.0L;
    return strtold_l(source, end, locale);
}
long long bs_bionic_strtoll_l(const char *source, char **end, int base,
                              locale_t locale) {
    if (!bs_bionic_valid_parse(source, end, locale)) return 0;
    return strtoll_l(source, end, base, locale);
}
unsigned long long bs_bionic_strtoull_l(const char *source, char **end, int base,
                                        locale_t locale) {
    if (!bs_bionic_valid_parse(source, end, locale)) return 0;
    return strtoull_l(source, end, base, locale);
}

int bs_bionic_strcoll_l(const char *lhs, const char *rhs, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return INT_MIN;
    return strcoll_l(lhs, rhs, locale);
}
size_t bs_bionic_strxfrm_l(char *dst, const char *src, size_t count,
                            locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return SIZE_MAX;
    return strxfrm_l(dst, src, count, locale);
}
int bs_bionic_wcscoll_l(const wchar_t *lhs, const wchar_t *rhs,
                         locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return INT_MIN;
    return wcscoll_l(lhs, rhs, locale);
}
size_t bs_bionic_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t count,
                            locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return SIZE_MAX;
    return wcsxfrm_l(dst, src, count, locale);
}
size_t bs_bionic_strftime_l(char *dst, size_t count, const char *format,
                             const struct tm *time, locale_t locale) {
    if (!bs_bionic_valid_explicit_locale(locale)) return 0;
    return strftime_l(dst, count, format, time, locale);
}

wint_t bs_bionic_btowc(int byte) { return btowc(byte); }
int bs_bionic_wctob(wint_t value) { return wctob(value); }
int bs_bionic_mbtowc(wchar_t *out, const char *bytes, size_t byte_count) {
    return mbtowc(out, bytes, byte_count);
}
