/*
 * Genuine Android NDK MediaDataSource callback-registration lifecycle tests.
 * Original Quest Unity 6 imports the six API 28 AMediaDataSource_* functions.
 * No media extractor, decoder, sample, or guest application code is executed.
 *
 * Run from beat-saber-engine with an installed host C compiler:
 * cc -std=c11 -Wall -Wextra -Werror -O2 -Isource \
 *    tests/test_android_media_datasource.c source/android_media_format.c \
 *    -o /tmp/bs_datasource_test
 *
 * SPDX-License-Identifier: MIT
 */
#include "../source/android_media_format.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct BsCallbackContext {
    int close_count;
    int alternate_close_count;
    int read_count;
    int getsize_count;
    unsigned char payload[9];
} BsCallbackContext;

static ssize_t read_at(void *userdata, int64_t offset, void *dst, size_t count) {
    BsCallbackContext *context = (BsCallbackContext *)userdata;
    assert(context);
    assert(offset >= 0);
    assert(dst || !count);
    ++context->read_count;
    const size_t size = sizeof context->payload;
    if ((uint64_t)offset >= size) return -1;
    size_t available = size - (size_t)offset;
    if (count > available) count = available;
    memcpy(dst, context->payload + offset, count);
    return (ssize_t)count;
}

static ssize_t get_size(void *userdata) {
    BsCallbackContext *context = (BsCallbackContext *)userdata;
    assert(context);
    ++context->getsize_count;
    return (ssize_t)sizeof context->payload;
}

static void close_source(void *userdata) {
    BsCallbackContext *context = (BsCallbackContext *)userdata;
    assert(context);
    ++context->close_count;
}

static void alternate_close(void *userdata) {
    BsCallbackContext *context = (BsCallbackContext *)userdata;
    assert(context);
    ++context->alternate_close_count;
}

static int null_close_count;
static void close_without_userdata(void *userdata) {
    assert(userdata == NULL);
    ++null_close_count;
}

static void test_source_registration(void) {
    BsCallbackContext context = {0};
    memcpy(context.payload, "realbytes", sizeof context.payload);

    /* All six original weak libunity imports must resolve to callable
     * functions. The newer close operation is an additional API 29 export.
     */
    const struct {
        const char *name;
        uintptr_t address;
    } required[] = {
        {"AMediaDataSource_new", (uintptr_t)&AMediaDataSource_new},
        {"AMediaDataSource_delete", (uintptr_t)&AMediaDataSource_delete},
        {"AMediaDataSource_setUserdata", (uintptr_t)&AMediaDataSource_setUserdata},
        {"AMediaDataSource_setReadAt", (uintptr_t)&AMediaDataSource_setReadAt},
        {"AMediaDataSource_setGetSize", (uintptr_t)&AMediaDataSource_setGetSize},
        {"AMediaDataSource_setClose", (uintptr_t)&AMediaDataSource_setClose},
        {"AMediaDataSource_close", (uintptr_t)&AMediaDataSource_close},
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i)
        assert(bs_android_media_format_find_export(required[i].name)
               == required[i].address);

    errno = 0;
    AMediaDataSource *source = AMediaDataSource_new();
    assert(source && errno == 0);
    AMediaDataSource_setUserdata(source, &context);
    AMediaDataSource_setReadAt(source, read_at);
    AMediaDataSource_setGetSize(source, get_size);
    AMediaDataSource_setClose(source, close_source);
    /* Callback registration itself never calls a guest supplied function.
     * The registrations also do not imply that an extractor can consume it.
     */
    assert(context.close_count == 0 && context.read_count == 0 &&
           context.getsize_count == 0);
    BsAndroidMediaSourceCallbacks callbacks = {0};
    assert(bs_android_media_data_source_snapshot(source, &callbacks));
    assert(callbacks.userdata == &context && callbacks.read_at == read_at &&
           callbacks.get_size == get_size && callbacks.close == close_source);
    /* This isolated callback test directly invokes the registered
     * callbacks with the supplied byte positions; there is no fabricated
     * track, decoded frame, or implicit Android service.
     */
    unsigned char buffer[5] = {0};
    assert(callbacks.get_size(callbacks.userdata) ==
           (ssize_t)sizeof context.payload);
    assert(callbacks.read_at(callbacks.userdata, 4, buffer, sizeof buffer) == 5);
    assert(memcmp(buffer, "bytes", sizeof buffer) == 0);
    assert(callbacks.read_at(callbacks.userdata, 9, buffer,
                             sizeof buffer) == -1);
    assert(context.getsize_count == 1 && context.read_count == 2);

    errno = 0;
    assert(AMediaExtractor_new() == NULL && errno == ENOSYS);
    errno = 0;
    assert(AMediaExtractor_setDataSourceCustom(NULL, source) ==
           BS_AMEDIA_ERROR_UNSUPPORTED && errno == ENOSYS);
    assert(context.close_count == 0 && context.read_count == 2 &&
           context.getsize_count == 1);

    /* Updating the close callback replaces the previous function pointer
     * without ever running the old callback.
     */
    AMediaDataSource_setClose(source, alternate_close);
    assert(bs_android_media_data_source_snapshot(source, &callbacks));
    assert(callbacks.close == alternate_close &&
           callbacks.read_at == read_at && callbacks.get_size == get_size);
    AMediaDataSource_close(source);
    assert(context.close_count == 0 && context.alternate_close_count == 1);
    /* NDK permits more than one close callback. No synthetic closed state is
     * invented here, because close is dispatched by the native framework.
     */
    AMediaDataSource_close(source);
    assert(context.alternate_close_count == 2);

    AMediaDataSource_setClose(source, close_source);
    AMediaDataSource_close(source);
    assert(context.close_count == 1 && context.alternate_close_count == 2);

    AMediaDataSource_delete(source);
    /* AOSP's AMediaDataSource_delete does not invoke the close callback and
     * never frees userdata owned by the application.
     */
    assert(context.close_count == 1 && context.alternate_close_count == 2);
    assert(memcmp(context.payload, "realbytes", sizeof context.payload) == 0);
    AMediaDataSource_delete(NULL);
}

static void test_optional_user_data_and_failures(void) {
    /* Close forwards the exact userdata pointer, including a deliberate
     * NULL value. Callback implementations may use no userdata at all.
     */
    AMediaDataSource *no_userdata = AMediaDataSource_new();
    assert(no_userdata);
    AMediaDataSource_setClose(no_userdata, close_without_userdata);
    AMediaDataSource_close(no_userdata);
    assert(null_close_count == 1);
    AMediaDataSource_delete(no_userdata);
    BsAndroidMediaSourceCallbacks callbacks = {0};
    errno = 0;
    assert(!bs_android_media_data_source_snapshot(NULL, &callbacks));
    assert(errno == EINVAL);

    errno = 0;
    AMediaDataSource_close(NULL);
    assert(errno == EINVAL);

    AMediaDataSource *src = AMediaDataSource_new();
    assert(src);
    errno = 0;
    AMediaDataSource_close(src);
    assert(errno == EINVAL);
    AMediaDataSource_delete(src);

    errno = 0;
    AMediaDataSource_setUserdata(NULL, NULL);
    assert(errno == EINVAL);
    errno = 0;
    AMediaDataSource_setReadAt(NULL, NULL);
    assert(errno == EINVAL);
    errno = 0;
    AMediaDataSource_setGetSize(NULL, NULL);
    assert(errno == EINVAL);
    errno = 0;
    AMediaDataSource_setClose(NULL, NULL);
    assert(errno == EINVAL);
}

static void test_many_independent_sources(void) {
    enum { COUNT = 256 };
    AMediaDataSource *sources[COUNT];
    for (size_t i = 0; i < COUNT; ++i) {
        sources[i] = AMediaDataSource_new();
        assert(sources[i]);
    }
    for (size_t i = 0; i < COUNT; ++i)
        AMediaDataSource_delete(sources[i]);
}

int main(void) {
    test_source_registration();
    test_optional_user_data_and_failures();
    test_many_independent_sources();
    puts("MediaDataSource lifecycle and fail-closed extractor tests passed");
    return 0;
}
