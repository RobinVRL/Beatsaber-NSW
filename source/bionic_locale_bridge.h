/* Android Bionic locale-category and mask translation for libnx/newlib.
 *
 * Quest ELF is Bionic AArch64, while devkitA64 uses a NEWLIB locale enum:
 * Android LC_CTYPE=0/LC_ALL=6, newlib LC_CTYPE=2/LC_ALL=0.
 * Binding setlocale/newlocale directly would silently affect wrong categories.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_LOCALE_BRIDGE_H
#define BS_BIONIC_LOCALE_BRIDGE_H
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

int bs_bionic_locale_category_to_native(int android_category);
int bs_bionic_locale_mask_to_native(int android_mask);
char *bs_bionic_setlocale(int android_category, const char *locale);
locale_t bs_bionic_newlocale(int android_category_mask, const char *locale,
                            locale_t base);
locale_t bs_bionic_uselocale(locale_t locale);
locale_t bs_bionic_duplocale(locale_t locale);
void bs_bionic_freelocale(locale_t locale);
struct lconv *bs_bionic_localeconv(void);
int bs_bionic_strerror_r(int code, char *buffer, size_t size);
int bs_bionic_locale_abi_selftest(void);

/* These accept locale_t objects created by the bridge (or newlib) only.
 * Android's category constants cannot be passed to the newlib backend.
 * A null/LC_GLOBAL_LOCALE argument to an _l function is rejected.
 */
#define BS_BIONIC_WCLASS_DECLARE(name) int bs_bionic_##name(wint_t value, locale_t locale)
BS_BIONIC_WCLASS_DECLARE(iswalpha_l);
BS_BIONIC_WCLASS_DECLARE(iswblank_l);
BS_BIONIC_WCLASS_DECLARE(iswcntrl_l);
BS_BIONIC_WCLASS_DECLARE(iswdigit_l);
BS_BIONIC_WCLASS_DECLARE(iswlower_l);
BS_BIONIC_WCLASS_DECLARE(iswprint_l);
BS_BIONIC_WCLASS_DECLARE(iswpunct_l);
BS_BIONIC_WCLASS_DECLARE(iswspace_l);
BS_BIONIC_WCLASS_DECLARE(iswupper_l);
BS_BIONIC_WCLASS_DECLARE(iswxdigit_l);
#undef BS_BIONIC_WCLASS_DECLARE
wint_t bs_bionic_towlower_l(wint_t value, locale_t locale);
wint_t bs_bionic_towupper_l(wint_t value, locale_t locale);

#define BS_BIONIC_CLASS_DECLARE(name) int bs_bionic_##name(int value, locale_t locale)
BS_BIONIC_CLASS_DECLARE(isdigit_l);
BS_BIONIC_CLASS_DECLARE(islower_l);
BS_BIONIC_CLASS_DECLARE(isupper_l);
BS_BIONIC_CLASS_DECLARE(isxdigit_l);
BS_BIONIC_CLASS_DECLARE(tolower_l);
BS_BIONIC_CLASS_DECLARE(toupper_l);
#undef BS_BIONIC_CLASS_DECLARE

double bs_bionic_strtod_l(const char *source, char **end, locale_t locale);
float bs_bionic_strtof_l(const char *source, char **end, locale_t locale);
long double bs_bionic_strtold_l(const char *source, char **end, locale_t locale);
long long bs_bionic_strtoll_l(const char *source, char **end, int base, locale_t locale);
unsigned long long bs_bionic_strtoull_l(const char *source, char **end, int base, locale_t locale);
int bs_bionic_strcoll_l(const char *lhs, const char *rhs, locale_t locale);
size_t bs_bionic_strxfrm_l(char *dst, const char *src, size_t count, locale_t locale);
int bs_bionic_wcscoll_l(const wchar_t *lhs, const wchar_t *rhs, locale_t locale);
size_t bs_bionic_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t count, locale_t locale);
size_t bs_bionic_strftime_l(char *dst, size_t count, const char *format,
                            const struct tm *time, locale_t locale);

/* No exposed mbstate_t: newlib's private conversion-state format must not
 * be assumed to match Android's. These stateless interfaces are ABI-safe. */
wint_t bs_bionic_btowc(int byte);
int bs_bionic_wctob(wint_t value);
int bs_bionic_mbtowc(wchar_t *out, const char *bytes, size_t byte_count);

#endif
