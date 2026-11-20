/* NDK MediaFormat metadata and MediaNDK exports.
 * SPDX-License-Identifier: MIT
 *
 * The original Quest Unity ELF imports the fifteen keys below as undefined
 * OBJECT/WEAK NOTYPE through R_AARCH64_GLOB_DAT. NdkMediaFormat.h declares
 * 'extern const char *KEY', so &KEY points at the actual eight-byte storage
 * that the loaded AArch64 code will dereference after reading its GOT slot.
 *
 * A raw string address in the import table would be WRONG: Unity would
 * interpret the first eight bytes of the string itself as an address.
 */
#include "android_media_format.h"
#include "android_image_bridge.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef BS_MEDIA_FFMPEG
#include <libavcodec/codec_par.h>
#endif
#if defined(__aarch64__)
_Static_assert(sizeof(void *) == 8, "Android arm64 data slots must be 8 bytes");
_Static_assert(sizeof(BsMediaCodecBufferInfo) == 24, "NDK codec buffer-info ABI");
#endif

#define BS_MEDIA_KEY(symbol, text) const char *symbol = text
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_HEIGHT, "height");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_MIME, "mime");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_STRIDE, "stride");
BS_MEDIA_KEY(AMEDIAFORMAT_KEY_WIDTH, "width");
#undef BS_MEDIA_KEY

typedef enum BsFormatType {
    BS_FMT_INT32,
    BS_FMT_INT64,
    BS_FMT_FLOAT,
    BS_FMT_STRING
} BsFormatType;

typedef union BsFormatValue {
    int32_t i32;
    int64_t i64;
    float f32;
    char *string;
} BsFormatValue;

typedef struct BsFormatEntry {
    struct BsFormatEntry *next;
    char *key;
    BsFormatType type;
    BsFormatValue value;
} BsFormatEntry;

struct AMediaFormat {
    BsFormatEntry *first;
#ifdef BS_MEDIA_FFMPEG
    AVCodecParameters *codec_parameters;
#endif
};

#ifdef BS_MEDIA_FFMPEG
/* Internal handoff of container-specific initialization records to a decoder. */
bool bs_media_format_copy_codec_parameters(AMediaFormat *format,
                                          const AVCodecParameters *parameters) {
    format->codec_parameters = avcodec_parameters_alloc();
    return format->codec_parameters &&
           avcodec_parameters_copy(format->codec_parameters, parameters) == 0;
}

const AVCodecParameters *bs_media_format_codec_parameters(const AMediaFormat *format) {
    return format->codec_parameters;
}
#endif

static char *bs_copy_string(const char *source) {
    if (!source) return NULL;
    size_t length = strlen(source);
    if (length == SIZE_MAX) { errno = ENOMEM; return NULL; }
    char *copy = malloc(length + 1);
    if (!copy) { errno = ENOMEM; return NULL; }
    memcpy(copy, source, length + 1);
    return copy;
}

static BsFormatEntry *bs_find_entry(const AMediaFormat *format,
                                    const char *name) {
    if (!format || !name || !*name) return NULL;
    for (BsFormatEntry *entry = format->first; entry; entry = entry->next) {
        if (strcmp(entry->key, name) == 0) return entry;
    }
    return NULL;
}

/* Copy the new value before releasing the previous value: setString may be
 * passed a pointer previously returned by getString on this same format. */
static bool bs_put_entry(AMediaFormat *format, const char *name,
                         BsFormatType type, BsFormatValue value) {
    if (!format || !name || !*name ||
        (type == BS_FMT_STRING && !value.string)) {
        errno = EINVAL;
        return false;
    }
    char *copy = NULL;
    if (type == BS_FMT_STRING) {
        copy = bs_copy_string(value.string);
        if (!copy) return false;
        value.string = copy;
    }

    BsFormatEntry *entry = bs_find_entry(format, name);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) {
            free(copy);
            errno = ENOMEM;
            return false;
        }
        entry->key = bs_copy_string(name);
        if (!entry->key) {
            free(copy);
            free(entry);
            return false;
        }
        entry->next = format->first;
        format->first = entry;
    } else if (entry->type == BS_FMT_STRING) {
        free(entry->value.string);
    }
    entry->type = type;
    entry->value = value;
    return true;
}

AMediaFormat *AMediaFormat_new(void) {
    AMediaFormat *format = calloc(1, sizeof(*format));
    if (!format) errno = ENOMEM;
    return format;
}

int32_t AMediaFormat_delete(AMediaFormat *format) {
    if (!format) return BS_AMEDIA_OK;
    BsFormatEntry *entry = format->first;
    while (entry) {
        BsFormatEntry *next = entry->next;
        free(entry->key);
        if (entry->type == BS_FMT_STRING) free(entry->value.string);
        free(entry);
        entry = next;
    }
#ifdef BS_MEDIA_FFMPEG
    avcodec_parameters_free(&format->codec_parameters);
#endif
    free(format);
    return BS_AMEDIA_OK;
}

bool AMediaFormat_getInt32(AMediaFormat *format, const char *name, int32_t *out) {
    BsFormatEntry *entry = bs_find_entry(format, name);
    if (!out || !entry || entry->type != BS_FMT_INT32) return false;
    *out = entry->value.i32;
    return true;
}

bool AMediaFormat_getInt64(AMediaFormat *format, const char *name, int64_t *out) {
    BsFormatEntry *entry = bs_find_entry(format, name);
    if (!out || !entry || entry->type != BS_FMT_INT64) return false;
    *out = entry->value.i64;
    return true;
}

bool AMediaFormat_getFloat(AMediaFormat *format, const char *name, float *out) {
    BsFormatEntry *entry = bs_find_entry(format, name);
    if (!out || !entry || entry->type != BS_FMT_FLOAT) return false;
    *out = entry->value.f32;
    return true;
}

bool AMediaFormat_getString(AMediaFormat *format, const char *name,
                             const char **out) {
    BsFormatEntry *entry = bs_find_entry(format, name);
    if (!out || !entry || entry->type != BS_FMT_STRING) return false;
    /* The returned pointer belongs to format; caller must not free it. */
    *out = entry->value.string;
    return true;
}

void AMediaFormat_setInt32(AMediaFormat *format, const char *name, int32_t value) {
    (void)bs_put_entry(format, name, BS_FMT_INT32,
                       (BsFormatValue){.i32=value});
}
void AMediaFormat_setInt64(AMediaFormat *format, const char *name, int64_t value) {
    (void)bs_put_entry(format, name, BS_FMT_INT64,
                       (BsFormatValue){.i64=value});
}
void AMediaFormat_setFloat(AMediaFormat *format, const char *name, float value) {
    (void)bs_put_entry(format, name, BS_FMT_FLOAT,
                       (BsFormatValue){.f32=value});
}
void AMediaFormat_setString(AMediaFormat *format, const char *name,
                             const char *value) {
    (void)bs_put_entry(format, name, BS_FMT_STRING,
                       (BsFormatValue){.string=(char *)value});
}

#ifndef BS_MEDIA_FFMPEG
/* The standalone format-only build has no demuxer or codec. Every service entrypoint
 * sets ENOSYS; media_status_t entrypoints return Android's negative
 * UNSUPPORTED value. Extractor/codec constructors return NULL, so there
 * can be no fabricated decoded track or working decoder.
 */
static int32_t bs_service_unsupported(void) {
    errno = ENOSYS;
    return BS_AMEDIA_ERROR_UNSUPPORTED;
}

AMediaExtractor *AMediaExtractor_new(void) {
    errno = ENOSYS;
    return NULL;
}
int32_t AMediaExtractor_delete(AMediaExtractor *ex) {
    (void)ex;
    return bs_service_unsupported();
}
int32_t AMediaExtractor_setDataSource(AMediaExtractor *ex, const char *location) {
    (void)ex; (void)location;
    return bs_service_unsupported();
}
int32_t AMediaExtractor_setDataSourceFd(AMediaExtractor *ex, int fd,
                                        int64_t offset, int64_t length) {
    (void)ex; (void)fd; (void)offset; (void)length;
    return bs_service_unsupported();
}
int32_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *ex,
                                            AMediaDataSource *source) {
    (void)ex; (void)source;
    return bs_service_unsupported();
}
size_t AMediaExtractor_getTrackCount(AMediaExtractor *ex) {
    (void)ex;
    errno = ENOSYS;
    return 0;
}
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *ex, size_t index) {
    (void)ex; (void)index;
    errno = ENOSYS;
    return NULL;
}
int32_t AMediaExtractor_selectTrack(AMediaExtractor *ex, size_t index) {
    (void)ex; (void)index;
    return bs_service_unsupported();
}
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *ex, uint8_t *buffer,
                                       size_t capacity) {
    (void)ex; (void)buffer; (void)capacity;
    errno = ENOSYS;
    return -1;
}
int32_t AMediaExtractor_getSampleTrackIndex(AMediaExtractor *ex) {
    (void)ex;
    errno = ENOSYS;
    return -1;
}
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *ex) {
    (void)ex;
    errno = ENOSYS;
    return -1;
}
bool AMediaExtractor_advance(AMediaExtractor *ex) {
    (void)ex;
    errno = ENOSYS;
    return false;
}
int32_t AMediaExtractor_seekTo(AMediaExtractor *ex, int64_t time_us, int mode) {
    (void)ex; (void)time_us; (void)mode;
    return bs_service_unsupported();
}

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime) {
    (void)mime;
    errno = ENOSYS;
    return NULL;
}
int32_t AMediaCodec_delete(AMediaCodec *codec) {
    (void)codec;
    return bs_service_unsupported();
}
int32_t AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *format,
                               ANativeWindow *surface, AMediaCrypto *crypto,
                               uint32_t flags) {
    (void)codec; (void)format; (void)surface; (void)crypto; (void)flags;
    return bs_service_unsupported();
}
int32_t AMediaCodec_start(AMediaCodec *codec) {
    (void)codec;
    return bs_service_unsupported();
}
int32_t AMediaCodec_stop(AMediaCodec *codec) {
    (void)codec;
    return bs_service_unsupported();
}
int32_t AMediaCodec_flush(AMediaCodec *codec) {
    (void)codec;
    return bs_service_unsupported();
}
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeout_us) {
    (void)codec; (void)timeout_us;
    errno = ENOSYS;
    return BS_AMEDIA_ERROR_UNSUPPORTED;
}
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t index,
                                    size_t *size) {
    (void)codec; (void)index;
    if (size) *size = 0;
    errno = ENOSYS;
    return NULL;
}
int32_t AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t index,
                                      int64_t offset, size_t size,
                                      uint64_t time_us, uint32_t flags) {
    (void)codec; (void)index; (void)offset; (void)size;
    (void)time_us; (void)flags;
    return bs_service_unsupported();
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec,
                                        BsMediaCodecBufferInfo *info,
                                        int64_t timeout_us) {
    (void)codec; (void)info; (void)timeout_us;
    errno = ENOSYS;
    return BS_AMEDIA_ERROR_UNSUPPORTED;
}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t index,
                                     size_t *size) {
    (void)codec; (void)index;
    if (size) *size = 0;
    errno = ENOSYS;
    return NULL;
}
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec) {
    (void)codec;
    errno = ENOSYS;
    return NULL;
}
int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t index,
                                         bool render) {
    (void)codec; (void)index; (void)render;
    return bs_service_unsupported();
}
int32_t AMediaCodec_setOutputSurface(AMediaCodec *codec,
                                     ANativeWindow *surface) {
    (void)codec; (void)surface;
    return bs_service_unsupported();
}

bool bs_android_media_decode_selftest(void) {
    return false;
}
#endif

/*
 * libmediandk's NdkMediaDataSource.cpp (Android frameworks/av) keeps an opaque
 * userdata pointer and three callback pointers. Its API 28 constructor merely
 * allocates/initializes this container; neither construction nor deletion
 * require Binder, a media extractor, or a hardware codec.
 *
 * In particular, AMediaDataSource_delete only deletes the registration
 * object; it does NOT dispatch its close callback. Closing is a separate
 * framework event (AMediaDataSource_close, API 29), which can occur on another
 * thread. Implementing these ownership semantics does not imply an extractor
 * exists or that any sample can be decoded.
 */
struct AMediaDataSource {
    void *userdata;  /* borrowed: NEVER freed by AMediaDataSource_delete */
    BsMediaDataSourceReadAt read_at;
    BsMediaDataSourceGetSize get_size;
    BsMediaDataSourceClose close;
};

AMediaDataSource *AMediaDataSource_new(void) {
    AMediaDataSource *source = calloc(1, sizeof(*source));
    if (!source) errno = ENOMEM;
    return source;
}

void AMediaDataSource_delete(AMediaDataSource *source) {
    /* Also permits NULL, as in Android's original implementation. */
    free(source);
}

void AMediaDataSource_setUserdata(AMediaDataSource *source, void *user) {
    if (!source) { errno = EINVAL; return; }
    source->userdata = user;
}

void AMediaDataSource_setReadAt(AMediaDataSource *source,
                                BsMediaDataSourceReadAt callback) {
    if (!source) { errno = EINVAL; return; }
    source->read_at = callback;
}

void AMediaDataSource_setGetSize(AMediaDataSource *source,
                                 BsMediaDataSourceGetSize callback) {
    if (!source) { errno = EINVAL; return; }
    source->get_size = callback;
}

void AMediaDataSource_setClose(AMediaDataSource *source,
                               BsMediaDataSourceClose callback) {
    if (!source) { errno = EINVAL; return; }
    source->close = callback;
}

void AMediaDataSource_close(AMediaDataSource *source) {
    if (!source || !source->close) {
        errno = EINVAL;
        return;
    }
    /* Android dispatches exactly this registered callback with its borrowed
     * userdata. The callback owns any I/O cleanup; we do not free userdata.
     * Repeated close calls intentionally remain possible (Android contract).
     */
    source->close(source->userdata);
}

bool bs_android_media_data_source_snapshot(
    const AMediaDataSource *source, BsAndroidMediaSourceCallbacks *out) {
    if (!source || !out) {
        errno = EINVAL;
        return false;
    }
    BsAndroidMediaSourceCallbacks copy = {
        .userdata = source->userdata,
        .read_at = source->read_at,
        .get_size = source->get_size,
        .close = source->close
    };
    *out = copy;
    return true;
}

/* Keep this map exact: function addresses are trampoline targets, while
 * OBJECT symbols MUST resolve to pointer-variable addresses. */
#define BS_MEDIA_DATA(name) {#name, (uintptr_t)&name}
#define BS_MEDIA_FUNC(name) {#name, (uintptr_t)&name}
const BsAndroidMediaExport bs_android_media_format_exports[] = {
    BS_MEDIA_FUNC(AImage_getWidth),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_CHANNEL_COUNT),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_FORMAT),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_RANGE),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_STANDARD),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_DURATION),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_ENCODER_DELAY),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_FRAME_RATE),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_HEIGHT),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_LANGUAGE),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_MIME),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_ROTATION),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_SAMPLE_RATE),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_SLICE_HEIGHT),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_STRIDE),
    BS_MEDIA_DATA(AMEDIAFORMAT_KEY_WIDTH),
    BS_MEDIA_FUNC(AMediaFormat_new),
    BS_MEDIA_FUNC(AMediaFormat_delete),
    BS_MEDIA_FUNC(AMediaFormat_getInt32),
    BS_MEDIA_FUNC(AMediaFormat_getInt64),
    BS_MEDIA_FUNC(AMediaFormat_getFloat),
    BS_MEDIA_FUNC(AMediaFormat_getString),
    BS_MEDIA_FUNC(AMediaFormat_setInt32),
    BS_MEDIA_FUNC(AMediaFormat_setInt64),
    BS_MEDIA_FUNC(AMediaFormat_setFloat),
    BS_MEDIA_FUNC(AMediaFormat_setString),
    BS_MEDIA_FUNC(AMediaExtractor_new),
    BS_MEDIA_FUNC(AMediaExtractor_delete),
    BS_MEDIA_FUNC(AMediaExtractor_setDataSource),
    BS_MEDIA_FUNC(AMediaExtractor_setDataSourceFd),
    BS_MEDIA_FUNC(AMediaExtractor_setDataSourceCustom),
    BS_MEDIA_FUNC(AMediaExtractor_getTrackCount),
    BS_MEDIA_FUNC(AMediaExtractor_getTrackFormat),
    BS_MEDIA_FUNC(AMediaExtractor_selectTrack),
    BS_MEDIA_FUNC(AMediaExtractor_readSampleData),
    BS_MEDIA_FUNC(AMediaExtractor_getSampleTrackIndex),
    BS_MEDIA_FUNC(AMediaExtractor_getSampleTime),
    BS_MEDIA_FUNC(AMediaExtractor_advance),
    BS_MEDIA_FUNC(AMediaExtractor_seekTo),
    BS_MEDIA_FUNC(AMediaCodec_createDecoderByType),
    BS_MEDIA_FUNC(AMediaCodec_delete),
    BS_MEDIA_FUNC(AMediaCodec_configure),
    BS_MEDIA_FUNC(AMediaCodec_start),
    BS_MEDIA_FUNC(AMediaCodec_stop),
    BS_MEDIA_FUNC(AMediaCodec_flush),
    BS_MEDIA_FUNC(AMediaCodec_dequeueInputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_getInputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_queueInputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_dequeueOutputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_getOutputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_getOutputFormat),
    BS_MEDIA_FUNC(AMediaCodec_releaseOutputBuffer),
    BS_MEDIA_FUNC(AMediaCodec_setOutputSurface),
    BS_MEDIA_FUNC(AMediaDataSource_new),
    BS_MEDIA_FUNC(AMediaDataSource_delete),
    BS_MEDIA_FUNC(AMediaDataSource_setUserdata),
    BS_MEDIA_FUNC(AMediaDataSource_setReadAt),
    BS_MEDIA_FUNC(AMediaDataSource_setGetSize),
    BS_MEDIA_FUNC(AMediaDataSource_setClose),
    BS_MEDIA_FUNC(AMediaDataSource_close)
};
#undef BS_MEDIA_DATA
#undef BS_MEDIA_FUNC

const size_t bs_android_media_format_exports_count =
    sizeof(bs_android_media_format_exports) /
    sizeof(bs_android_media_format_exports[0]);

uintptr_t bs_android_media_format_find_export(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < bs_android_media_format_exports_count; ++i)
        if (!strcmp(bs_android_media_format_exports[i].name, name))
            return bs_android_media_format_exports[i].address;
    return 0;
}
