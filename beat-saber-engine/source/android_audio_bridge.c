/* Native AAudio output adapter for Unity 6000.3.19f1 / FMOD on Switch.
 * Copyright (c) 2026 Beat Saber NX homebrew contributors.
 * MIT license; see ../LICENSE.
 *
 * AAudio's callback produces the guest engine's real PCM into our scratch
 * buffer. The worker converts/resamples it to audout's 48 kHz stereo PCM16.
 * Only ONE output stream owns audout, and no input/recording path is emulated.
 *
 * The OpenSL reference in quest-loader/source/opensles.c uses SDL2 and carries
 * Unity/PvZ-specific AudioTrack hooks. This standalone adapter uses libnx
 * directly and exports the AAudio C names found in the actual Quest libunity.so.
 */
#include "android_audio_bridge.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdatomic.h>

#define BS_OUTPUT_RATE 48000
#define BS_OUTPUT_FRAMES 1024
#define BS_OUTPUT_BUFFERS 3
#define BS_CALLBACK_FRAMES 256
#define BS_CALLBACK_FRAMES_MAX 4096

static int16_t bs_clip_s16(float value) {
    if (!isfinite(value)) return 0;
    if (value >= 32767.0f) return 32767;
    if (value <= -32768.0f) return -32768;
    return (int16_t)lrintf(value);
}

size_t bs_android_audio_pcm_to_s16_stereo(
    int16_t *dst, size_t capacity, const void *src, size_t frames,
    int32_t format, int32_t channels)
{
    if (!dst || !src || (channels != 1 && channels != 2) ||
        (format != BS_AAUDIO_FORMAT_I16 && format != BS_AAUDIO_FORMAT_FLOAT))
        return 0;
    if (frames > capacity) frames = capacity;
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            size_t index = i * (size_t)channels + (size_t)(channels == 1 ? 0 : c);
            if (format == BS_AAUDIO_FORMAT_I16)
                dst[i * 2 + (size_t)c] = ((const int16_t *)src)[index];
            else {
                float f = ((const float *)src)[index];
                if (!isfinite(f)) f = 0.0f;
                dst[i * 2 + (size_t)c] = bs_clip_s16(
                    f <= -1.0f ? -32768.0f : f * 32767.0f);
            }
        }
    }
    return frames;
}

#ifdef __SWITCH__

#include <malloc.h>
#include <switch.h>
#include <time.h>

/* Stream states are atomic because guest JNI/FMOD queries can run on other
 * threads. The guest callback is never invoked under an audout mutex. */
struct AAudioStreamBuilder {
    int32_t direction;
    int32_t rate;
    int32_t channels;
    int32_t format;
    int32_t device_id;
    int32_t session_id;
    int32_t sharing_mode;
    int32_t performance_mode;
    int32_t callback_frames;
    int32_t buffer_capacity;
    AAudioStream_dataCallback callback;
    AAudioStream_errorCallback error_callback;
    void *userdata;
    void *error_userdata;
};

struct AAudioStream {
    int32_t rate;
    int32_t channels;
    int32_t format;
    int32_t callback_frames;
    AAudioStream_dataCallback callback;
    AAudioStream_errorCallback error_callback;
    void *userdata;
    void *error_userdata;
    BsAudioGuestThreadInit guest_init;
    BsAudioGuestThreadExit guest_exit;
    void *guest_init_context;

    Thread worker;
    bool worker_created;
    _Atomic int state;
    _Atomic bool quit;
    _Atomic uint64_t frames_written;
    _Atomic uint64_t frames_played_device;
    _Atomic uint32_t underruns;

    AudioOutBuffer out[BS_OUTPUT_BUFFERS];
    bool queued[BS_OUTPUT_BUFFERS];
    /* AAudio supplies either I16 or FLOAT32. u32 alignment works for both. */
    uint32_t input[BS_CALLBACK_FRAMES_MAX * 2];
    int input_frames;
    int input_pos;
    bool callback_done;
    bool source_done;
    bool have_current;
    bool have_next;
    float current[2];
    float next[2];
    double source_fraction;
};

static Mutex g_audio_lock;
static AAudioStream *g_active_stream;
static BsAudioGuestThreadInit g_guest_init;
static BsAudioGuestThreadExit g_guest_exit;
static void *g_guest_init_context;

void bs_android_audio_set_guest_thread_lifecycle(
    BsAudioGuestThreadInit init, BsAudioGuestThreadExit exit, void *context)
{
    mutexLock(&g_audio_lock);
    g_guest_init = init;
    g_guest_exit = exit;
    g_guest_init_context = context;
    mutexUnlock(&g_audio_lock);
}
void bs_android_audio_set_guest_thread_init(
    BsAudioGuestThreadInit init, void *context)
{
    bs_android_audio_set_guest_thread_lifecycle(init, NULL, context);
}

static bool bs_pcm_input_frame(AAudioStream *stream, float out[2])
{
    if (stream->input_pos >= stream->input_frames) {
        if (stream->callback_done) return false;
        /* The engine owns this PCM. Clearing only guards against a callback
         * incorrectly leaving part of its requested buffer unwritten. */
        const size_t bytes = (size_t)stream->callback_frames *
                             (size_t)stream->channels *
                             (size_t)(stream->format == BS_AAUDIO_FORMAT_FLOAT ? 4 : 2);
        memset(stream->input, 0, bytes);
        int32_t rc = stream->callback(stream, stream->userdata,
                                      stream->input, stream->callback_frames);
        if (rc != 0) stream->callback_done = true;
        stream->input_frames = stream->callback_frames;
        stream->input_pos = 0;
        atomic_fetch_add(&stream->frames_written, (uint64_t)stream->callback_frames);
    }
    int idx = stream->input_pos++;
    for (int c = 0; c < 2; ++c) {
        const int source_channel = stream->channels == 1 ? 0 : c;
        const int sample = idx * stream->channels + source_channel;
        if (stream->format == BS_AAUDIO_FORMAT_FLOAT) {
            float f = ((float *)stream->input)[sample];
            if (!isfinite(f)) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            out[c] = f <= -1.0f ? -32768.0f : f * 32767.0f;
        } else {
            out[c] = (float)((int16_t *)stream->input)[sample];
        }
    }
    return true;
}

/* Streaming linear resampler. The fractional source-frame position survives
 * across audout blocks, including when the guest returns 44.1 kHz or 96 kHz.
 * No fabricated waveforms or substitute songs are generated. */
static int bs_fill_output(AAudioStream *s, int16_t *dst)
{
    int frames = 0;
    const double step = (double)s->rate / (double)BS_OUTPUT_RATE;
    while (frames < BS_OUTPUT_FRAMES) {
        if (!s->have_current) {
            if (!bs_pcm_input_frame(s, s->current)) break;
            s->have_current = true;
        }
        if (!s->have_next)
            s->have_next = bs_pcm_input_frame(s, s->next);
        for (int c = 0; c < 2; ++c) {
            float value = s->current[c];
            if (s->have_next) {
                value += (s->next[c] - value) * (float)s->source_fraction;
            }
            dst[(size_t)frames * 2 + (size_t)c] = bs_clip_s16(value);
        }
        ++frames;
        s->source_fraction += step;
        while (s->source_fraction >= 1.0 && s->have_current) {
            s->source_fraction -= 1.0;
            if (s->have_next) {
                s->current[0] = s->next[0];
                s->current[1] = s->next[1];
                s->have_next = false;
            } else {
                s->have_current = false;
            }
            if (s->source_fraction >= 1.0 && s->have_current) {
                s->have_next = bs_pcm_input_frame(s, s->next);
            }
        }
    }
    if (frames < BS_OUTPUT_FRAMES)
        memset(dst + (size_t)frames * 2, 0,
               (size_t)(BS_OUTPUT_FRAMES - frames) * 2 * sizeof(int16_t));
    return frames;
}

static void bs_worker_failure(AAudioStream *s, int32_t error)
{
    atomic_store(&s->state, BS_AAUDIO_STATE_DISCONNECTED);
    if (s->error_callback)
        s->error_callback(s, s->error_userdata, error);
}

static void bs_audio_worker(void *userdata)
{
    AAudioStream *s = (AAudioStream *)userdata;
    if (!s->guest_init(s->guest_init_context)) {
        /* The guest error callback would need exactly the same Bionic TLS.
         * A failed initialization therefore cannot call guest code at all. */
        atomic_store(&s->state, BS_AAUDIO_STATE_DISCONNECTED);
        return;
    }

    bool output_running = false;
    unsigned queued_count = 0;
    while (!atomic_load(&s->quit)) {
        int32_t state = atomic_load(&s->state);
        if (state == BS_AAUDIO_STATE_STARTING ||
            state == BS_AAUDIO_STATE_STARTED) {
            /* Keep at least two periods ahead of the hardware. Preparing only
             * after the last buffer finishes produces an audible underrun at
             * every boundary, even if the callback itself is perfectly fast. */
            for (unsigned i = 0; i < BS_OUTPUT_BUFFERS && !s->source_done; ++i) {
                if (atomic_load(&s->state) != BS_AAUDIO_STATE_STARTING &&
                    atomic_load(&s->state) != BS_AAUDIO_STATE_STARTED)
                    break;
                if (s->queued[i]) continue;
                AudioOutBuffer *b = &s->out[i];
                int frames = bs_fill_output(s, (int16_t *)b->buffer);
                if (!frames) { s->source_done = true; break; }
                b->data_offset = 0;
                b->data_size = (uint64_t)BS_OUTPUT_FRAMES * 2 * sizeof(int16_t);
                armDCacheFlush(b->buffer, (size_t)b->data_size);
                if (R_FAILED(audoutAppendAudioOutBuffer(b))) {
                    bs_worker_failure(s, BS_AAUDIO_ERROR_INTERNAL);
                    break;
                }
                s->queued[i] = true;
                ++queued_count;
                if (frames < BS_OUTPUT_FRAMES) s->source_done = true;
            }
            if (atomic_load(&s->state) == BS_AAUDIO_STATE_DISCONNECTED) break;
            if (!queued_count && s->source_done) {
                atomic_store(&s->state, BS_AAUDIO_STATE_STOPPING);
                continue;
            }
            if (!queued_count) { svcSleepThread(1000000ULL); continue; }
            if (!output_running) {
                if (R_FAILED(audoutStartAudioOut())) {
                    bs_worker_failure(s, BS_AAUDIO_ERROR_INTERNAL);
                    break;
                }
                output_running = true;
            }

            int expected = BS_AAUDIO_STATE_STARTING;
            atomic_compare_exchange_strong(&s->state, &expected,
                                           BS_AAUDIO_STATE_STARTED);

            /* audoutWaitPlayFinish returns the first released descriptor and
             * how many were released. For batched completion, query ownership
             * of the other descriptors before reusing any output memory. */
            unsigned wait_errors = 0;
            while (!atomic_load(&s->quit)) {
                AudioOutBuffer *released = NULL;
                u32 released_count = 0;
                Result rc = audoutWaitPlayFinish(&released, &released_count,
                                                  50000000ULL);
                if (R_SUCCEEDED(rc) && released && released_count) {
                    unsigned found = 0;
                    for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i) {
                        if (!s->queued[i]) continue;
                        bool complete = &s->out[i] == released;
                        if (!complete && released_count > 1) {
                            bool contains = true;
                            if (R_SUCCEEDED(audoutContainsAudioOutBuffer(
                                    &s->out[i], &contains)) && !contains)
                                complete = true;
                        }
                        if (complete) {
                            s->queued[i] = false;
                            --queued_count;
                            ++found;
                            atomic_fetch_add(&s->frames_played_device,
                                             BS_OUTPUT_FRAMES);
                        }
                    }
                    if (found) break;
                }
                if (++wait_errors > 40) {
                    bs_worker_failure(s, BS_AAUDIO_ERROR_INTERNAL);
                    break;
                }
            }
            if (atomic_load(&s->state) == BS_AAUDIO_STATE_DISCONNECTED) break;
        } else {
            if (output_running) {
                audoutStopAudioOut();
                output_running = false;
            }
            /* AAudio pause/stop discards native device queue ownership.
             * Flushing before reusing descriptors avoids a still-referenced
             * buffer being written after the driver has stopped. */
            bool flushed = false;
            if (queued_count && R_FAILED(audoutFlushAudioOutBuffers(&flushed))) {
                bs_worker_failure(s, BS_AAUDIO_ERROR_INTERNAL);
                break;
            }
            for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i)
                s->queued[i] = false;
            queued_count = 0;
            if (state == BS_AAUDIO_STATE_PAUSING)
                atomic_store(&s->state, BS_AAUDIO_STATE_PAUSED);
            if (state == BS_AAUDIO_STATE_STOPPING)
                atomic_store(&s->state, BS_AAUDIO_STATE_STOPPED);
            svcSleepThread(1000000ULL);
        }
    }
    if (output_running) audoutStopAudioOut();
    /* Guest code has finished using this worker's Android TLS. Keep the
     * native thread alive for its owner to run destructors and detach it. */
    if (s->guest_exit) s->guest_exit(s->guest_init_context);
}

int32_t AAudio_createStreamBuilder(AAudioStreamBuilder **out)
{
    if (!out) return BS_AAUDIO_ERROR_NULL;
    *out = NULL;
    AAudioStreamBuilder *b = calloc(1, sizeof(*b));
    if (!b) return BS_AAUDIO_ERROR_NO_MEMORY;
    b->rate = BS_OUTPUT_RATE;
    b->channels = 2;
    b->format = BS_AAUDIO_FORMAT_I16;
    b->session_id = -1;
    b->sharing_mode = 1; /* SHARED */
    b->performance_mode = 10; /* NONE */
    b->callback_frames = BS_CALLBACK_FRAMES;
    *out = b;
    return 0;
}

int32_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b)
{ if (!b) return BS_AAUDIO_ERROR_NULL; free(b); return 0; }

#define BS_BUILDER_SET(name, member) \
    void AAudioStreamBuilder_set##name(AAudioStreamBuilder *b, int32_t value) \
    { if (b) b->member = value; }
BS_BUILDER_SET(Direction, direction)
BS_BUILDER_SET(SampleRate, rate)
BS_BUILDER_SET(ChannelCount, channels)
BS_BUILDER_SET(Format, format)
BS_BUILDER_SET(SharingMode, sharing_mode)
BS_BUILDER_SET(PerformanceMode, performance_mode)
BS_BUILDER_SET(DeviceId, device_id)
BS_BUILDER_SET(SessionId, session_id)
BS_BUILDER_SET(BufferCapacityInFrames, buffer_capacity)
BS_BUILDER_SET(FramesPerDataCallback, callback_frames)
#undef BS_BUILDER_SET

void AAudioStreamBuilder_setDataCallback(
    AAudioStreamBuilder *b, AAudioStream_dataCallback cb, void *data)
{ if (b) { b->callback = cb; b->userdata = data; } }

void AAudioStreamBuilder_setErrorCallback(
    AAudioStreamBuilder *b, AAudioStream_errorCallback cb, void *data)
{ if (b) { b->error_callback = cb; b->error_userdata = data; } }

int32_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **out)
{
    if (!b || !out) return BS_AAUDIO_ERROR_NULL;
    *out = NULL;
    if (b->direction != BS_AAUDIO_DIRECTION_OUTPUT || !b->callback)
        return BS_AAUDIO_ERROR_UNIMPLEMENTED;
    if ((b->rate != 0 && b->rate < 8000) || b->rate > 192000)
        return BS_AAUDIO_ERROR_INVALID_RATE;
    if (b->channels != 0 && b->channels != 1 && b->channels != 2)
        return BS_AAUDIO_ERROR_INVALID_CHANNEL_COUNT;
    if (b->format != 0 && b->format != BS_AAUDIO_FORMAT_I16 &&
        b->format != BS_AAUDIO_FORMAT_FLOAT)
        return BS_AAUDIO_ERROR_INVALID_FORMAT;
    if (b->callback_frames < 0 || b->callback_frames > BS_CALLBACK_FRAMES_MAX)
        return BS_AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (b->device_id) return BS_AAUDIO_ERROR_UNAVAILABLE;

    mutexLock(&g_audio_lock);
    if (g_active_stream) {
        mutexUnlock(&g_audio_lock);
        return BS_AAUDIO_ERROR_UNAVAILABLE;
    }
    AAudioStream *s = calloc(1, sizeof(*s));
    if (!s) {
        mutexUnlock(&g_audio_lock);
        return BS_AAUDIO_ERROR_NO_MEMORY;
    }
    s->rate = b->rate ? b->rate : BS_OUTPUT_RATE;
    s->channels = b->channels ? b->channels : 2;
    s->format = b->format ? b->format : BS_AAUDIO_FORMAT_I16;
    s->callback_frames = b->callback_frames ? b->callback_frames : BS_CALLBACK_FRAMES;
    s->callback = b->callback;
    s->error_callback = b->error_callback;
    s->userdata = b->userdata;
    s->error_userdata = b->error_userdata;
    s->guest_init = g_guest_init;
    s->guest_exit = g_guest_exit;
    s->guest_init_context = g_guest_init_context;
    atomic_init(&s->state, BS_AAUDIO_STATE_OPEN);
    atomic_init(&s->quit, false);
    atomic_init(&s->frames_written, 0);
    atomic_init(&s->frames_played_device, 0);
    atomic_init(&s->underruns, 0);

    for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i) {
        s->out[i].buffer = memalign(0x1000, BS_OUTPUT_FRAMES * 4);
        if (!s->out[i].buffer) {
            for (unsigned j = 0; j < i; ++j) free(s->out[j].buffer);
            free(s);
            mutexUnlock(&g_audio_lock);
            return BS_AAUDIO_ERROR_NO_MEMORY;
        }
        s->out[i].buffer_size = BS_OUTPUT_FRAMES * 4;
    }
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i) free(s->out[i].buffer);
        free(s);
        mutexUnlock(&g_audio_lock);
        return BS_AAUDIO_ERROR_UNAVAILABLE;
    }
    if (audoutGetSampleRate() != BS_OUTPUT_RATE ||
        audoutGetChannelCount() != 2 ||
        audoutGetPcmFormat() != PcmFormat_Int16) {
        audoutExit();
        for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i) free(s->out[i].buffer);
        free(s);
        mutexUnlock(&g_audio_lock);
        return BS_AAUDIO_ERROR_UNAVAILABLE;
    }
    g_active_stream = s;
    *out = s;
    mutexUnlock(&g_audio_lock);
    return 0;
}

int32_t AAudioStream_requestStart(AAudioStream *s)
{
    if (!s) return BS_AAUDIO_ERROR_NULL;
    int state = atomic_load(&s->state);
    if (state == BS_AAUDIO_STATE_STARTED ||
        state == BS_AAUDIO_STATE_STARTING) return 0;
    if (state != BS_AAUDIO_STATE_OPEN &&
        state != BS_AAUDIO_STATE_FLUSHED &&
        state != BS_AAUDIO_STATE_PAUSED &&
        state != BS_AAUDIO_STATE_STOPPED)
        return BS_AAUDIO_ERROR_INVALID_STATE;
    /* An Android IL2CPP/Unity callback reads Bionic per-thread TLS; entering it
     * on a raw libnx worker without the caller's TLS hook would crash. */
    if (!s->guest_init) {
        mutexLock(&g_audio_lock);
        s->guest_init = g_guest_init;
        s->guest_exit = g_guest_exit;
        s->guest_init_context = g_guest_init_context;
        mutexUnlock(&g_audio_lock);
    }
    if (!s->guest_init) return BS_AAUDIO_ERROR_UNAVAILABLE;
    if (state == BS_AAUDIO_STATE_STOPPED) {
        s->input_frames = s->input_pos = 0;
        s->callback_done = s->have_current = s->have_next = false;
        s->source_done = false;
        s->source_fraction = 0.0;
    }
    atomic_store(&s->state, BS_AAUDIO_STATE_STARTING);
    if (!s->worker_created) {
        Result rc = threadCreate(&s->worker, bs_audio_worker, s, NULL,
                                 0x10000, 0x2c, -2);
        if (R_FAILED(rc)) {
            atomic_store(&s->state, state);
            return BS_AAUDIO_ERROR_UNAVAILABLE;
        }
        s->worker_created = true;
        rc = threadStart(&s->worker);
        if (R_FAILED(rc)) {
            threadClose(&s->worker);
            s->worker_created = false;
            atomic_store(&s->state, state);
            return BS_AAUDIO_ERROR_UNAVAILABLE;
        }
    }
    return 0;
}

int32_t AAudioStream_requestPause(AAudioStream *s)
{
    if (!s) return BS_AAUDIO_ERROR_NULL;
    int state = atomic_load(&s->state);
    if (state != BS_AAUDIO_STATE_STARTED &&
        state != BS_AAUDIO_STATE_STARTING)
        return BS_AAUDIO_ERROR_INVALID_STATE;
    atomic_store(&s->state, BS_AAUDIO_STATE_PAUSING);
    return 0;
}

int32_t AAudioStream_requestStop(AAudioStream *s)
{
    if (!s) return BS_AAUDIO_ERROR_NULL;
    int state = atomic_load(&s->state);
    if (state != BS_AAUDIO_STATE_STARTED &&
        state != BS_AAUDIO_STATE_STARTING &&
        state != BS_AAUDIO_STATE_PAUSED)
        return BS_AAUDIO_ERROR_INVALID_STATE;
    atomic_store(&s->state, BS_AAUDIO_STATE_STOPPING);
    return 0;
}

int32_t AAudioStream_requestFlush(AAudioStream *s)
{
    if (!s) return BS_AAUDIO_ERROR_NULL;
    int state = atomic_load(&s->state);
    if (state != BS_AAUDIO_STATE_OPEN && state != BS_AAUDIO_STATE_STOPPED &&
        state != BS_AAUDIO_STATE_PAUSED && state != BS_AAUDIO_STATE_FLUSHED)
        return BS_AAUDIO_ERROR_INVALID_STATE;
    s->input_frames = s->input_pos = 0;
    s->have_current = s->have_next = false;
    s->callback_done = false;
    s->source_done = false;
    s->source_fraction = 0.0;
    atomic_store(&s->state, BS_AAUDIO_STATE_FLUSHED);
    return 0;
}

int32_t AAudioStream_close(AAudioStream *s)
{
    if (!s) return BS_AAUDIO_ERROR_NULL;
    mutexLock(&g_audio_lock);
    if (g_active_stream != s) {
        mutexUnlock(&g_audio_lock);
        return BS_AAUDIO_ERROR_INVALID_STATE;
    }
    atomic_store(&s->quit, true);
    if (s->worker_created) {
        threadWaitForExit(&s->worker);
        threadClose(&s->worker);
    }
    audoutStopAudioOut();
    bool flushed = false;
    audoutFlushAudioOutBuffers(&flushed);
    audoutExit();
    for (unsigned i = 0; i < BS_OUTPUT_BUFFERS; ++i) free(s->out[i].buffer);
    g_active_stream = NULL;
    mutexUnlock(&g_audio_lock);
    free(s);
    return 0;
}

void bs_android_audio_shutdown(void)
{
    mutexLock(&g_audio_lock);
    AAudioStream *s = g_active_stream;
    mutexUnlock(&g_audio_lock);
    if (s) AAudioStream_close(s);
}

int32_t AAudioStream_waitForStateChange(
    AAudioStream *s, int32_t previous, int32_t *next, int64_t timeout_ns)
{
    if (!s || !next) return BS_AAUDIO_ERROR_NULL;
    if (timeout_ns < 0) timeout_ns = INT64_MAX;
    uint64_t now = armGetSystemTick();
    uint64_t deadline = timeout_ns == INT64_MAX ? UINT64_MAX :
         now + (uint64_t)((long double)timeout_ns *
                (long double)armGetSystemTickFreq() / 1000000000.0L);
    while ((*next = atomic_load(&s->state)) == previous) {
        if (timeout_ns == 0 || armGetSystemTick() >= deadline)
            return BS_AAUDIO_ERROR_TIMEOUT;
        svcSleepThread(1000000ULL);
    }
    return 0;
}

#define BS_STREAM_GET(name, type, expression) \
    type AAudioStream_get##name(AAudioStream *s) \
    { return s ? (expression) : (type)BS_AAUDIO_ERROR_NULL; }
BS_STREAM_GET(State, int32_t, atomic_load(&s->state))
BS_STREAM_GET(SampleRate, int32_t, s->rate)
BS_STREAM_GET(ChannelCount, int32_t, s->channels)
BS_STREAM_GET(SamplesPerFrame, int32_t, s->channels)
BS_STREAM_GET(Format, int32_t, s->format)
BS_STREAM_GET(Direction, int32_t, BS_AAUDIO_DIRECTION_OUTPUT)
BS_STREAM_GET(DeviceId, int32_t, 0)
BS_STREAM_GET(SessionId, int32_t, -1)
BS_STREAM_GET(SharingMode, int32_t, 1)
BS_STREAM_GET(PerformanceMode, int32_t, 10)
BS_STREAM_GET(FramesPerBurst, int32_t, BS_OUTPUT_FRAMES)
BS_STREAM_GET(FramesPerDataCallback, int32_t, s->callback_frames)
BS_STREAM_GET(BufferCapacityInFrames, int32_t,
    BS_OUTPUT_FRAMES * BS_OUTPUT_BUFFERS)
BS_STREAM_GET(BufferSizeInFrames, int32_t,
    BS_OUTPUT_FRAMES * BS_OUTPUT_BUFFERS)
BS_STREAM_GET(XRunCount, int32_t, (int32_t)atomic_load(&s->underruns))
BS_STREAM_GET(FramesWritten, int64_t, (int64_t)atomic_load(&s->frames_written))
BS_STREAM_GET(FramesRead, int64_t,
    (int64_t)((long double)atomic_load(&s->frames_played_device) *
              (long double)s->rate / BS_OUTPUT_RATE))
#undef BS_STREAM_GET

int32_t AAudioStream_isMMapUsed(AAudioStream *s)
{ return s ? 0 : BS_AAUDIO_ERROR_NULL; }

int32_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t frames)
{ return !s ? BS_AAUDIO_ERROR_NULL :
         frames <= 0 ? BS_AAUDIO_ERROR_ILLEGAL_ARGUMENT :
         BS_OUTPUT_FRAMES * BS_OUTPUT_BUFFERS; }

/* This first milestone implements FMOD's realtime data-callback mode.
 * Blocking AAudioStream_write/read have no queue implementation yet. */
int32_t AAudioStream_write(AAudioStream *s, const void *data,
                           int32_t frames, int64_t timeout_ns)
{ (void)s; (void)data; (void)frames; (void)timeout_ns;
  return BS_AAUDIO_ERROR_UNIMPLEMENTED; }
int32_t AAudioStream_read(AAudioStream *s, void *data,
                          int32_t frames, int64_t timeout_ns)
{ (void)s; (void)data; (void)frames; (void)timeout_ns;
  return BS_AAUDIO_ERROR_UNIMPLEMENTED; }

const char *AAudio_convertResultToText(int32_t error)
{
    switch (error) {
    case 0: return "AAUDIO_OK";
    case BS_AAUDIO_ERROR_NULL: return "AAUDIO_ERROR_NULL";
    case BS_AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
    case BS_AAUDIO_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case BS_AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case BS_AAUDIO_ERROR_INVALID_FORMAT: return "AAUDIO_ERROR_INVALID_FORMAT";
    case BS_AAUDIO_ERROR_INVALID_RATE: return "AAUDIO_ERROR_INVALID_RATE";
    case BS_AAUDIO_ERROR_INVALID_CHANNEL_COUNT: return "AAUDIO_ERROR_INVALID_CHANNEL_COUNT";
    case BS_AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    default: return "AAUDIO_ERROR_UNKNOWN";
    }
}

#define BS_AAUDIO_EXPORT(symbol) { #symbol, (uintptr_t)&symbol }
const BsAndroidAudioExport bs_android_audio_exports[] = {
    BS_AAUDIO_EXPORT(AAudio_createStreamBuilder),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_delete),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setDirection),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setSampleRate),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setChannelCount),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setFormat),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setSharingMode),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setPerformanceMode),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setDeviceId),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setSessionId),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setBufferCapacityInFrames),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setFramesPerDataCallback),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setDataCallback),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_setErrorCallback),
    BS_AAUDIO_EXPORT(AAudioStreamBuilder_openStream),
    BS_AAUDIO_EXPORT(AAudioStream_requestStart),
    BS_AAUDIO_EXPORT(AAudioStream_requestStop),
    BS_AAUDIO_EXPORT(AAudioStream_requestPause),
    BS_AAUDIO_EXPORT(AAudioStream_requestFlush),
    BS_AAUDIO_EXPORT(AAudioStream_close),
    BS_AAUDIO_EXPORT(AAudioStream_waitForStateChange),
    BS_AAUDIO_EXPORT(AAudioStream_getState),
    BS_AAUDIO_EXPORT(AAudioStream_getSampleRate),
    BS_AAUDIO_EXPORT(AAudioStream_getChannelCount),
    BS_AAUDIO_EXPORT(AAudioStream_getSamplesPerFrame),
    BS_AAUDIO_EXPORT(AAudioStream_getFormat),
    BS_AAUDIO_EXPORT(AAudioStream_getDirection),
    BS_AAUDIO_EXPORT(AAudioStream_getDeviceId),
    BS_AAUDIO_EXPORT(AAudioStream_getSessionId),
    BS_AAUDIO_EXPORT(AAudioStream_getSharingMode),
    BS_AAUDIO_EXPORT(AAudioStream_getPerformanceMode),
    BS_AAUDIO_EXPORT(AAudioStream_getFramesPerBurst),
    BS_AAUDIO_EXPORT(AAudioStream_getFramesPerDataCallback),
    BS_AAUDIO_EXPORT(AAudioStream_getBufferCapacityInFrames),
    BS_AAUDIO_EXPORT(AAudioStream_getBufferSizeInFrames),
    BS_AAUDIO_EXPORT(AAudioStream_setBufferSizeInFrames),
    BS_AAUDIO_EXPORT(AAudioStream_getXRunCount),
    BS_AAUDIO_EXPORT(AAudioStream_isMMapUsed),
    BS_AAUDIO_EXPORT(AAudioStream_getFramesWritten),
    BS_AAUDIO_EXPORT(AAudioStream_getFramesRead),
    BS_AAUDIO_EXPORT(AAudioStream_write),
    BS_AAUDIO_EXPORT(AAudioStream_read),
    BS_AAUDIO_EXPORT(AAudio_convertResultToText)
};
#undef BS_AAUDIO_EXPORT
const size_t bs_android_audio_exports_count =
    sizeof(bs_android_audio_exports) / sizeof(bs_android_audio_exports[0]);

void *bs_android_audio_find_export(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < bs_android_audio_exports_count; ++i)
        if (!strcmp(bs_android_audio_exports[i].name, name))
            return (void *)bs_android_audio_exports[i].address;
    return NULL;
}

#else
/* Host PCM unit tests build this file without devkitPro. Native AAudio export
 * definitions are Switch-only and are compile-checked with devkitA64. */
#endif /* __SWITCH__ */
