/* Minimal native AAudio output adapter for the owner's Unity 6 Android engine.
 * Copyright (c) 2026 Beat Saber NX homebrew contributors.
 * MIT license; see ../LICENSE.
 *
 * This exposes the AAudio C ABI through a symbol table for the loader's
 * Android dlopen/dlsym shim. It forwards original FMOD-generated PCM buffers
 * to libnx audout; it does not decode, synthesize, or embed game audio.
 *
 * Only callback-driven OUTPUT streams are currently supported. Unsupported
 * Android APIs return explicit errors rather than pretending that audio works.
 */
#ifndef BEAT_SABER_ANDROID_AUDIO_BRIDGE_H
#define BEAT_SABER_ANDROID_AUDIO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef struct AAudioStream AAudioStream;
typedef int32_t (*AAudioStream_dataCallback)(
    AAudioStream *stream, void *user_data, void *audio_data, int32_t frames);
typedef void (*AAudioStream_errorCallback)(
    AAudioStream *stream, void *user_data, int32_t error);

/* Android aaudio/AAudio.h ABI values (API 26+). */
enum {
    BS_AAUDIO_FORMAT_I16 = 1,
    BS_AAUDIO_FORMAT_FLOAT = 2,
    BS_AAUDIO_DIRECTION_OUTPUT = 0,
    BS_AAUDIO_DIRECTION_INPUT = 1,
    BS_AAUDIO_STATE_OPEN = 2,
    BS_AAUDIO_STATE_STARTING = 3,
    BS_AAUDIO_STATE_STARTED = 4,
    BS_AAUDIO_STATE_PAUSING = 5,
    BS_AAUDIO_STATE_PAUSED = 6,
    BS_AAUDIO_STATE_FLUSHING = 7,
    BS_AAUDIO_STATE_FLUSHED = 8,
    BS_AAUDIO_STATE_STOPPING = 9,
    BS_AAUDIO_STATE_STOPPED = 10,
    BS_AAUDIO_STATE_CLOSED = 12,
    BS_AAUDIO_STATE_DISCONNECTED = 13,
    BS_AAUDIO_ERROR_DISCONNECTED = -899,
    BS_AAUDIO_ERROR_ILLEGAL_ARGUMENT = -898,
    BS_AAUDIO_ERROR_INTERNAL = -896,
    BS_AAUDIO_ERROR_INVALID_STATE = -895,
    BS_AAUDIO_ERROR_UNIMPLEMENTED = -894,
    BS_AAUDIO_ERROR_UNAVAILABLE = -893,
    BS_AAUDIO_ERROR_NO_MEMORY = -891,
    BS_AAUDIO_ERROR_NULL = -890,
    BS_AAUDIO_ERROR_TIMEOUT = -889,
    BS_AAUDIO_ERROR_INVALID_FORMAT = -887,
    BS_AAUDIO_ERROR_INVALID_RATE = -884,
    BS_AAUDIO_ERROR_INVALID_CHANNEL_COUNT = -883
};

/* Called exactly once on the Switch audio worker before any guest callback.
 * The prime's Bionic runtime must install the actual per-thread Android TLS,
 * including the stack canary at the expected location. Without this hook,
 * AAudioStream_requestStart returns UNAVAILABLE; no unsafe guest callback runs.
 * Return nonzero on success.
 */
typedef int (*BsAudioGuestThreadInit)(void *context);
/* Runs on the SAME audio worker, after its last guest callback and before
 * the native thread exits. Called exactly once only after successful init.
 * The owner can run guest thread destructors and restore native Bionic TLS.
 */
typedef void (*BsAudioGuestThreadExit)(void *context);
void bs_android_audio_set_guest_thread_lifecycle(
    BsAudioGuestThreadInit init, BsAudioGuestThreadExit exit, void *context);
void bs_android_audio_set_guest_thread_init(BsAudioGuestThreadInit init,
                                             void *context);
void bs_android_audio_shutdown(void);

/* Lookup for libaaudio.so dlsym, or merge bs_android_audio_exports into the
 * loader's explicit dynamic import table. The shim should accept libaaudio.so
 * only when this table has been installed. No other Android library is spoofed.
 */
typedef struct BsAndroidAudioExport {
    const char *name;
    uintptr_t address;
} BsAndroidAudioExport;
extern const BsAndroidAudioExport bs_android_audio_exports[];
extern const size_t bs_android_audio_exports_count;
void *bs_android_audio_find_export(const char *name);

/* Host-testable PCM conversion. Input is interleaved signed 16-bit or float32,
 * mono/stereo; output is stereo PCM16 at the SAME frame rate. No resampling is
 * done by this function. Invalid formats/channels return zero frames.
 */
size_t bs_android_audio_pcm_to_s16_stereo(
    int16_t *dst, size_t dst_capacity_frames, const void *src,
    size_t src_frames, int32_t format, int32_t channels);

/* Actual AAudio C exports resolved by the bridge, with Android signatures. */
int32_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
int32_t AAudioStreamBuilder_delete(AAudioStreamBuilder *builder);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder *, int32_t);
void AAudioStreamBuilder_setDataCallback(
    AAudioStreamBuilder *, AAudioStream_dataCallback, void *);
void AAudioStreamBuilder_setErrorCallback(
    AAudioStreamBuilder *, AAudioStream_errorCallback, void *);
int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *, AAudioStream **);

int32_t AAudioStream_requestStart(AAudioStream *);
int32_t AAudioStream_requestPause(AAudioStream *);
int32_t AAudioStream_requestStop(AAudioStream *);
int32_t AAudioStream_requestFlush(AAudioStream *);
int32_t AAudioStream_close(AAudioStream *);
int32_t AAudioStream_waitForStateChange(
    AAudioStream *, int32_t input_state, int32_t *next_state, int64_t timeout_ns);
int32_t AAudioStream_getState(AAudioStream *);
int32_t AAudioStream_getSampleRate(AAudioStream *);
int32_t AAudioStream_getChannelCount(AAudioStream *);
int32_t AAudioStream_getSamplesPerFrame(AAudioStream *);
int32_t AAudioStream_getFormat(AAudioStream *);
int32_t AAudioStream_getDirection(AAudioStream *);
int32_t AAudioStream_getDeviceId(AAudioStream *);
int32_t AAudioStream_getSessionId(AAudioStream *);
int32_t AAudioStream_getSharingMode(AAudioStream *);
int32_t AAudioStream_getPerformanceMode(AAudioStream *);
int32_t AAudioStream_getFramesPerBurst(AAudioStream *);
int32_t AAudioStream_getFramesPerDataCallback(AAudioStream *);
int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream *);
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream *);
int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *, int32_t);
int32_t AAudioStream_getXRunCount(AAudioStream *);
int32_t AAudioStream_isMMapUsed(AAudioStream *);
int64_t AAudioStream_getFramesWritten(AAudioStream *);
int64_t AAudioStream_getFramesRead(AAudioStream *);
int32_t AAudioStream_write(AAudioStream *, const void *, int32_t, int64_t);
int32_t AAudioStream_read(AAudioStream *, void *, int32_t, int64_t);
const char *AAudio_convertResultToText(int32_t);

#ifdef __cplusplus
}
#endif
#endif
