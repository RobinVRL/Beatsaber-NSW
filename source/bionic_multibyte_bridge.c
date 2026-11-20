/* Bionic-compatible UTF-8 converter operating on guest mbstate_t bytes.
 * No native wchar.h conversion or host mbstate_t casts are used.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_multibyte_bridge.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

/* Bionic's four first bytes hold 0..3 buffered bytes of an incomplete UTF-8
 * sequence. Its reserved bytes have no conversion meaning and are untouched.
 * Bionic uses one static private state per top-level API for ps == NULL.
 */
static void bs_mb_reset(bs_bionic_mbstate_t *state) {
    memset(state->seq, 0, sizeof state->seq);
}

static size_t bs_mb_error(bs_bionic_mbstate_t *state, int error) {
    bs_mb_reset(state);
    errno = error;
    return (size_t)-1;
}

int bs_bionic_mbsinit(const bs_bionic_mbstate_t *state) {
    if (!state) return 1;
    return (state->seq[0] | state->seq[1] |
            state->seq[2] | state->seq[3]) == 0;
}

static size_t bs_mb_pending(const bs_bionic_mbstate_t *state) {
    return state->seq[2] ? 3u : state->seq[1] ? 2u :
           state->seq[0] ? 1u : 0u;
}

static size_t bs_mb_decode(uint32_t *out, const char *bytes, size_t count,
                           bs_bionic_mbstate_t *state) {
    if (state->seq[3] != 0) return bs_mb_error(state, EINVAL);
    if (!bytes) {
        bytes = "";
        count = 1;
        out = NULL;
    }
    if (count == 0) return (size_t)-2;

    const size_t pending = bs_mb_pending(state);
    if (pending == 0 && (uint8_t)bytes[0] < 0x80u) {
        const uint32_t ascii = (uint8_t)bytes[0];
        if (out) *out = ascii;
        return ascii ? 1u : 0u;
    }
    if ((pending >= 2 && !state->seq[0]) ||
        (pending == 3 && !state->seq[1])) return bs_mb_error(state, EILSEQ);

    const uint8_t first = pending ? state->seq[0] : (uint8_t)bytes[0];
    size_t length;
    uint32_t value;
    uint32_t minimum;
    if ((first & 0xe0u) == 0xc0u) {
        length = 2;
        value = first & 0x1fu;
        minimum = 0x80u;
    } else if ((first & 0xf0u) == 0xe0u) {
        length = 3;
        value = first & 0x0fu;
        minimum = 0x800u;
    } else if ((first & 0xf8u) == 0xf0u) {
        length = 4;
        value = first & 0x07u;
        minimum = 0x10000u;
    } else {
        return bs_mb_error(state, EILSEQ);
    }
    if (pending >= length) return bs_mb_error(state, EILSEQ);

    const size_t wanted = length - pending;
    const size_t available = count < wanted ? count : wanted;
    for (size_t i = 0; i < available; i++) {
        const uint8_t byte = (uint8_t)bytes[i];
        if ((pending + i) != 0 && (byte & 0xc0u) != 0x80u)
            return bs_mb_error(state, EILSEQ);
        state->seq[pending + i] = byte;
    }
    if (available < wanted) return (size_t)-2;

    for (size_t i = 1; i < length; i++)
        value = (value << 6) | (state->seq[i] & 0x3fu);
    if (value < minimum || (value >= 0xd800u && value <= 0xdfffu) ||
        value > 0x10ffffu) return bs_mb_error(state, EILSEQ);
    if (out) *out = value;
    bs_mb_reset(state);
    return wanted; /* Excludes already-buffered bytes, as Bionic does. */
}

size_t bs_bionic_mbrtowc(uint32_t *out, const char *bytes, size_t count,
                         bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_decode(out, bytes, count, state ? state : &private_state);
}

size_t bs_bionic_mbrtoc32(uint32_t *out, const char *bytes, size_t count,
                          bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_decode(out, bytes, count, state ? state : &private_state);
}

size_t bs_bionic_mbrlen(const char *bytes, size_t count,
                        bs_bionic_mbstate_t *state) {
    return bs_bionic_mbrtowc(NULL, bytes, count, state);
}

static size_t bs_mb_encode(char *out, uint32_t codepoint,
                           bs_bionic_mbstate_t *state) {
    if (!out) {
        bs_mb_reset(state);
        return 1;
    }
    if (codepoint == 0) {
        *out = '\0';
        bs_mb_reset(state);
        return 1;
    }
    if (!bs_bionic_mbsinit(state)) return bs_mb_error(state, EILSEQ);
    if (codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu))
        return bs_mb_error(state, EILSEQ);

    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1;
    }
    size_t length;
    uint8_t lead;
    if (codepoint < 0x800u) {
        length = 2;
        lead = 0xc0u;
    } else if (codepoint < 0x10000u) {
        length = 3;
        lead = 0xe0u;
    } else {
        length = 4;
        lead = 0xf0u;
    }
    for (size_t i = length - 1; i != 0; i--) {
        out[i] = (char)(0x80u | (codepoint & 0x3fu));
        codepoint >>= 6;
    }
    out[0] = (char)(lead | codepoint);
    return length;
}

size_t bs_bionic_wcrtomb(char *out, uint32_t codepoint,
                         bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_encode(out, codepoint, state ? state : &private_state);
}

size_t bs_bionic_c32rtomb(char *out, uint32_t codepoint,
                           bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_encode(out, codepoint, state ? state : &private_state);
}

static size_t bs_mb_string_decode(uint32_t *dst, const char **src,
                                  size_t src_len, size_t dst_len,
                                  bs_bionic_mbstate_t *state) {
    if (!src || !*src) {
        errno = EINVAL;
        return (size_t)-1;
    }
    const char *input = *src;
    const int measuring = dst == NULL;
    bs_bionic_mbstate_t measurement_state = *state;
    bs_bionic_mbstate_t *work = measuring ? &measurement_state : state;
    size_t consumed = 0;
    size_t produced = 0;
    if (measuring) dst_len = SIZE_MAX;

    while (consumed < src_len && produced < dst_len) {
        uint32_t codepoint = 0;
        const size_t remaining = src_len - consumed;
        const size_t converted = bs_mb_decode(&codepoint, input + consumed,
                                               remaining, work);
        if (converted == (size_t)-1) {
            if (!measuring) *src = input + consumed;
            return (size_t)-1;
        }
        if (converted == (size_t)-2) {
            /* POSIX bounded conversion keeps incomplete bytes in mbstate_t
             * for a subsequent call; this call consumes all src_len bytes. */
            if (!measuring) *src = input + src_len;
            return produced;
        }
        if (converted == 0) {
            if (!measuring) {
                dst[produced] = 0;
                *src = NULL;
            }
            return produced;
        }
        if (!measuring) dst[produced] = codepoint;
        produced++;
        consumed += converted;
    }
    if (!measuring) *src = input + consumed;
    return produced;
}

size_t bs_bionic_mbsnrtowcs(uint32_t *dst, const char **src, size_t src_len,
                            size_t dst_len, bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_string_decode(dst, src, src_len, dst_len,
                               state ? state : &private_state);
}

size_t bs_bionic_mbsrtowcs(uint32_t *dst, const char **src, size_t dst_len,
                           bs_bionic_mbstate_t *state) {
    /* As in Bionic, this is a bounded converter with no byte limit; it also
     * shares that converter's implicit state when ps == NULL. */
    return bs_bionic_mbsnrtowcs(dst, src, SIZE_MAX, dst_len, state);
}

static size_t bs_mb_string_encode(char *dst, const uint32_t **src,
                                  size_t src_len, size_t dst_len,
                                  bs_bionic_mbstate_t *state) {
    if (!src || !*src) {
        errno = EINVAL;
        return (size_t)-1;
    }
    const uint32_t *input = *src;
    const int measuring = dst == NULL;
    bs_bionic_mbstate_t measurement_state = *state;
    bs_bionic_mbstate_t *work = measuring ? &measurement_state : state;
    if (!bs_bionic_mbsinit(work)) {
        if (!measuring) *src = input;
        return bs_mb_error(work, EILSEQ);
    }
    if (measuring) dst_len = SIZE_MAX;

    size_t produced = 0;
    size_t consumed = 0;
    while (consumed < src_len) {
        if (!measuring && produced == dst_len) break;
        char encoded[4];
        const uint32_t codepoint = input[consumed];
        const size_t converted = bs_mb_encode(encoded, codepoint, work);
        if (converted == (size_t)-1) {
            if (!measuring) *src = input + consumed;
            return (size_t)-1;
        }
        if (converted > dst_len - produced) break;
        if (codepoint == 0) {
            if (!measuring) {
                dst[produced] = '\0';
                *src = NULL;
            }
            return produced;
        }
        if (!measuring) memcpy(dst + produced, encoded, converted);
        produced += converted;
        consumed++;
    }
    if (!measuring) *src = input + consumed;
    return produced;
}

size_t bs_bionic_wcsnrtombs(char *dst, const uint32_t **src, size_t src_len,
                            size_t dst_len, bs_bionic_mbstate_t *state) {
    static bs_bionic_mbstate_t private_state;
    return bs_mb_string_encode(dst, src, src_len, dst_len,
                               state ? state : &private_state);
}

size_t bs_bionic_wcsrtombs(char *dst, const uint32_t **src, size_t dst_len,
                           bs_bionic_mbstate_t *state) {
    return bs_bionic_wcsnrtombs(dst, src, SIZE_MAX, dst_len, state);
}

size_t bs_bionic_mbstowcs(uint32_t *dst, const char *src, size_t dst_len) {
    bs_bionic_mbstate_t state = {{0}, {0}};
    return bs_mb_string_decode(dst, &src, SIZE_MAX, dst_len, &state);
}

size_t bs_bionic_wcstombs(char *dst, const uint32_t *src, size_t dst_len) {
    bs_bionic_mbstate_t state = {{0}, {0}};
    return bs_mb_string_encode(dst, &src, SIZE_MAX, dst_len, &state);
}

int bs_bionic_mbtowc_compat(uint32_t *out, const char *bytes, size_t count) {
    bs_bionic_mbstate_t state = {{0}, {0}};
    if (!bytes) return 0;
    size_t result = bs_mb_decode(out, bytes, count, &state);
    if (result == (size_t)-2) {
        errno = EILSEQ;
        return -1;
    }
    return result == (size_t)-1 ? -1 : (int)result;
}

int bs_bionic_mblen(const char *bytes, size_t count) {
    return bs_bionic_mbtowc_compat(NULL, bytes, count);
}

int bs_bionic_wctomb(char *out, uint32_t codepoint) {
    bs_bionic_mbstate_t state = {{0}, {0}};
    if (!out) return 0;
    const size_t result = bs_mb_encode(out, codepoint, &state);
    return result == (size_t)-1 ? -1 : (int)result;
}
