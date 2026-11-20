/* Runtime checks for the genuine AArch64 libmediandk data/metadata ABI.
 * Compile with android_media_format.c. No Quest assets or codec are invoked.
 * SPDX-License-Identifier: MIT
 */
#include "../source/android_media_format.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__aarch64__)
_Static_assert(sizeof(const char *) == 8, "Quest AArch64 key data is 8 bytes");
#endif

typedef struct BsMediaKeyTest {
    const char *name;
    const char *expected_value;
    const char **object_slot;
} BsMediaKeyTest;

#define KEY(sym, value) { #sym, value, &sym }
static const BsMediaKeyTest key_tests[] = {
    KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count"),
    KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format"),
    KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range"),
    KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard"),
    KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs"),
    KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay"),
    KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate"),
    KEY(AMEDIAFORMAT_KEY_HEIGHT, "height"),
    KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language"),
    KEY(AMEDIAFORMAT_KEY_MIME, "mime"),
    KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees"),
    KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate"),
    KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height"),
    KEY(AMEDIAFORMAT_KEY_STRIDE, "stride"),
    KEY(AMEDIAFORMAT_KEY_WIDTH, "width")
};
#undef KEY
_Static_assert(sizeof(key_tests) / sizeof(key_tests[0]) == 15,
               "all actual Quest libunity AMEDIAFORMAT_KEY imports");

int bs_test_android_media_format(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof(key_tests)/sizeof(key_tests[0]); ++i) {
        const BsMediaKeyTest *key = &key_tests[i];
        uintptr_t address = bs_android_media_format_find_export(key->name);
        /* The GOT receives the ADDRESS OF a const-char-pointer variable.
         * Reading the GLOB_DAT target once yields the string pointer. */
        if (address != (uintptr_t)key->object_slot || !address ||
            strcmp(*(const char *const *)address, key->expected_value) ||
            strcmp(*key->object_slot, key->expected_value))
            ++failures;
    }
    if (bs_android_media_format_find_export("AMEDIAFORMAT_KEY_NOT_REAL") ||
        bs_android_media_format_find_export(NULL) ||
        bs_android_media_format_find_export("AMediaCodec_notAnAPI"))
        ++failures;

    AMediaFormat *format = AMediaFormat_new();
    if (!format) return failures + 1;

    int32_t i32 = 777;
    int64_t i64 = 888;
    float f32 = 9.5f;
    const char *string = "untouched";
    /* Missing keys and mismatched types never overwrite output storage. */
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &i32) ||
        i32 != 777 ||
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, NULL))
        ++failures;

    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    if (!AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &i32) ||
        i32 != 1920 ||
        AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_WIDTH, &i64) ||
        i64 != 888)
        ++failures;

    AMediaFormat_setInt64(format, AMEDIAFORMAT_KEY_DURATION, INT64_C(5000000000));
    if (!AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &i64) ||
        i64 != INT64_C(5000000000))
        ++failures;

    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, 29.97f);
    if (!AMediaFormat_getFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, &f32) ||
        f32 < 29.96f || f32 > 29.98f)
        ++failures;

    char original_mime[40] = "video/avc";
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, original_mime);
    strcpy(original_mime, "modified");
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &string) ||
        strcmp(string, "video/avc") != 0)
        ++failures;

    /* Self-overwrite: setString must copy prior getString storage BEFORE
     * destroying it. The format owns the resulting pointer. */
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, string);
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &string) ||
        strcmp(string, "video/avc") != 0)
        ++failures;

    /* Type replacement invalidates only the replaced entry and gets typed. */
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MIME, 4);
    if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &string) ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_MIME, &i32) || i32 != 4)
        ++failures;

    if (AMediaFormat_delete(format) != BS_AMEDIA_OK ||
        AMediaFormat_delete(NULL) != BS_AMEDIA_OK)
        ++failures;

    errno = 0;
    if (AMediaCodec_createDecoderByType("video/avc") != NULL || errno != ENOSYS)
        ++failures;
    errno = 0;
    if (AMediaExtractor_new() != NULL || errno != ENOSYS)
        ++failures;
    errno = 0;
    if (AMediaCodec_start(NULL) != BS_AMEDIA_ERROR_UNSUPPORTED || errno != ENOSYS)
        ++failures;
    errno = 0;
    if (AMediaExtractor_setDataSourceFd(NULL, 4, 0, 1024) !=
            BS_AMEDIA_ERROR_UNSUPPORTED || errno != ENOSYS)
        ++failures;
    size_t output_size = 1234;
    errno = 0;
    if (AMediaCodec_getOutputBuffer(NULL, 0, &output_size) != NULL ||
        output_size != 0 || errno != ENOSYS)
        ++failures;
    errno = 0;
    AMediaDataSource *source = AMediaDataSource_new();
    if (!source || errno != 0)
        ++failures;
    AMediaDataSource_delete(source);
    return failures;
}

#if defined(BS_MEDIA_TEST_MAIN)
int main(void) {
    int failures = bs_test_android_media_format();
    if (failures) fprintf(stderr, "MediaNDK ABI test FAIL: %d\n", failures);
    else puts("MediaNDK ABI test PASS");
    return failures ? 1 : 0;
}
#endif
