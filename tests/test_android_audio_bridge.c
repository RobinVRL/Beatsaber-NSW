/* Portable conversion tests for android_audio_bridge.c (no game data). */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "android_audio_bridge.h"

static void test_float_stereo(void) {
    const float source[] = {
        -1.0f, 1.0f,
        -0.5f, 0.5f,
        NAN, INFINITY,
        -INFINITY, 2.0f
    };
    const int16_t expected[] = {
        -32768, 32767,
        -16384, 16384,
        0, 0,
        0, 32767
    };
    int16_t actual[8] = {0};
    size_t n = bs_android_audio_pcm_to_s16_stereo(
        actual, 4, source, 4, BS_AAUDIO_FORMAT_FLOAT, 2);
    assert(n == 4);
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
}

static void test_integer_mono_and_capacity(void) {
    const int16_t source[] = {123, -32768, 32767};
    const int16_t expected[] = {123, 123, -32768, -32768};
    int16_t actual[] = {0, 0, 0, 0, 8080, 8080};
    size_t n = bs_android_audio_pcm_to_s16_stereo(
        actual, 2, source, 3, BS_AAUDIO_FORMAT_I16, 1);
    assert(n == 2);
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    assert(actual[4] == 8080 && actual[5] == 8080);
}

static void test_integer_stereo_and_invalid(void) {
    const int16_t source[] = {-10, 20, 321, -654};
    int16_t actual[] = {0, 0, 0, 0};
    size_t n = bs_android_audio_pcm_to_s16_stereo(
        actual, 2, source, 2, BS_AAUDIO_FORMAT_I16, 2);
    assert(n == 2);
    assert(memcmp(source, actual, sizeof(source)) == 0);
    assert(bs_android_audio_pcm_to_s16_stereo(
        actual, 2, source, 2, BS_AAUDIO_FORMAT_I16, 3) == 0);
    assert(bs_android_audio_pcm_to_s16_stereo(
        actual, 2, source, 2, 99, 2) == 0);
    assert(bs_android_audio_pcm_to_s16_stereo(
        NULL, 2, source, 2, BS_AAUDIO_FORMAT_I16, 2) == 0);
}

int main(void) {
    test_float_stereo();
    test_integer_mono_and_capacity();
    test_integer_stereo_and_invalid();
    puts("AAudio PCM conversion tests passed");
    return 0;
}
