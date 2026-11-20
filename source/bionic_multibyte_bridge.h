/* Android LP64 Bionic UTF-8 multibyte ABI, independent of newlib mbstate_t.
 *
 * Bionic wchar_t and char32_t are 32-bit Unicode scalar values. Its mbstate_t
 * is an 8-byte object containing uint8_t __seq[4] and four reserved bytes;
 * newlib's similarly sized mbstate_t has an incompatible private layout.
 *
 * Source: AOSP bionic libc/include/wchar.h, libc/private/bionic_mbstate.h,
 * libc/bionic/{mbrtoc32,c32rtomb,wchar}.cpp.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_MULTIBYTE_BRIDGE_H
#define BS_BIONIC_MULTIBYTE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t seq[4];
    uint8_t reserved[4];
} bs_bionic_mbstate_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(bs_bionic_mbstate_t) == 8,
               "Bionic AArch64 mbstate_t must be eight bytes");
#endif

/* Explicit guest-wide uint32_t types keep host tests correct even if host
 * wchar_t is 16-bit. They are layout-equivalent to AArch64 wchar_t/char32_t.
 */
int bs_bionic_mbsinit(const bs_bionic_mbstate_t *state);
size_t bs_bionic_mbrtowc(uint32_t *out, const char *bytes, size_t count,
                         bs_bionic_mbstate_t *state);
size_t bs_bionic_mbrtoc32(uint32_t *out, const char *bytes, size_t count,
                          bs_bionic_mbstate_t *state);
size_t bs_bionic_mbrlen(const char *bytes, size_t count,
                        bs_bionic_mbstate_t *state);
size_t bs_bionic_wcrtomb(char *out, uint32_t codepoint,
                         bs_bionic_mbstate_t *state);
size_t bs_bionic_c32rtomb(char *out, uint32_t codepoint,
                           bs_bionic_mbstate_t *state);
size_t bs_bionic_mbsrtowcs(uint32_t *dst, const char **src, size_t dst_len,
                           bs_bionic_mbstate_t *state);
size_t bs_bionic_mbsnrtowcs(uint32_t *dst, const char **src, size_t src_len,
                            size_t dst_len, bs_bionic_mbstate_t *state);
size_t bs_bionic_wcsrtombs(char *dst, const uint32_t **src, size_t dst_len,
                           bs_bionic_mbstate_t *state);
size_t bs_bionic_wcsnrtombs(char *dst, const uint32_t **src, size_t src_len,
                            size_t dst_len, bs_bionic_mbstate_t *state);
size_t bs_bionic_mbstowcs(uint32_t *dst, const char *src, size_t dst_len);
size_t bs_bionic_wcstombs(char *dst, const uint32_t *src, size_t dst_len);
int bs_bionic_mblen(const char *bytes, size_t count);
int bs_bionic_mbtowc_compat(uint32_t *out, const char *bytes, size_t count);
int bs_bionic_wctomb(char *out, uint32_t codepoint);

#endif
