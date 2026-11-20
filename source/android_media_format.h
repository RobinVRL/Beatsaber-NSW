/* Android libmediandk C ABI for original Quest Unity 6000.3.19f1 imports.
 * SPDX-License-Identifier: MIT
 *
 * This bridge implements a typed format map and callback-backed data sources.
 * The optional FFmpeg backend demuxes real tracks and decodes to CPU buffers.
 *
 * The imported AMEDIAFORMAT_KEY_* symbols are DATA. Android declares each as
 * 'extern const char *', and libunity.so uses R_AARCH64_GLOB_DAT (address of
 * an eight-byte pointer variable). The loader must bind &KEY, not KEY.
 */
#ifndef BS_ANDROID_MEDIA_FORMAT_H
#define BS_ANDROID_MEDIA_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaDataSource AMediaDataSource;
typedef struct AMediaCrypto AMediaCrypto;
typedef struct ANativeWindow ANativeWindow;
typedef struct AImage AImage;

/* Values from NdkMediaError.h. Android media_status_t is a 32-bit enum. */
typedef enum BsMediaStatus {
    BS_AMEDIA_OK = 0,
    BS_AMEDIA_ERROR_UNKNOWN = -10000,
    BS_AMEDIA_ERROR_MALFORMED = -10001,
    BS_AMEDIA_ERROR_UNSUPPORTED = -10002,
    BS_AMEDIA_ERROR_INVALID_OBJECT = -10003,
    BS_AMEDIA_ERROR_INVALID_PARAMETER = -10004,
    BS_AMEDIA_ERROR_INVALID_OPERATION = -10005,
    BS_AMEDIA_ERROR_IO = -10007
} BsMediaStatus;

/* Image-reader output is not produced by the CPU-buffer codec. AImage_getWidth
 * must reject unowned image handles without dereferencing guest pointers. */
int32_t AImage_getWidth(const AImage *image, int32_t *width);

typedef struct BsMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} BsMediaCodecBufferInfo;

/* All fifteen undefined DATA symbols of the original Quest libunity.so. */
extern const char *AMEDIAFORMAT_KEY_CHANNEL_COUNT;
extern const char *AMEDIAFORMAT_KEY_COLOR_FORMAT;
extern const char *AMEDIAFORMAT_KEY_COLOR_RANGE;
extern const char *AMEDIAFORMAT_KEY_COLOR_STANDARD;
extern const char *AMEDIAFORMAT_KEY_DURATION;
extern const char *AMEDIAFORMAT_KEY_ENCODER_DELAY;
extern const char *AMEDIAFORMAT_KEY_FRAME_RATE;
extern const char *AMEDIAFORMAT_KEY_HEIGHT;
extern const char *AMEDIAFORMAT_KEY_LANGUAGE;
extern const char *AMEDIAFORMAT_KEY_MIME;
extern const char *AMEDIAFORMAT_KEY_ROTATION;
extern const char *AMEDIAFORMAT_KEY_SAMPLE_RATE;
extern const char *AMEDIAFORMAT_KEY_SLICE_HEIGHT;
extern const char *AMEDIAFORMAT_KEY_STRIDE;
extern const char *AMEDIAFORMAT_KEY_WIDTH;

AMediaFormat *AMediaFormat_new(void);
int32_t AMediaFormat_delete(AMediaFormat *format);
bool AMediaFormat_getInt32(AMediaFormat *format, const char *name, int32_t *out);
bool AMediaFormat_getInt64(AMediaFormat *format, const char *name, int64_t *out);
bool AMediaFormat_getFloat(AMediaFormat *format, const char *name, float *out);
bool AMediaFormat_getString(AMediaFormat *format, const char *name, const char **out);
void AMediaFormat_setInt32(AMediaFormat *format, const char *name, int32_t value);
/* Additional standard typed setters for locally populated formats. */
void AMediaFormat_setInt64(AMediaFormat *format, const char *name, int64_t value);
void AMediaFormat_setFloat(AMediaFormat *format, const char *name, float value);
void AMediaFormat_setString(AMediaFormat *format, const char *name, const char *value);

/* CPU-buffer extraction and decoding. DRM and surface output require separate
 * Android platform services. */
AMediaExtractor *AMediaExtractor_new(void);
int32_t AMediaExtractor_delete(AMediaExtractor *extractor);
int32_t AMediaExtractor_setDataSource(AMediaExtractor *, const char *location);
int32_t AMediaExtractor_setDataSourceFd(AMediaExtractor *, int fd,
                                       int64_t offset, int64_t length);
int32_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *, AMediaDataSource *);
size_t AMediaExtractor_getTrackCount(AMediaExtractor *);
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *, size_t index);
int32_t AMediaExtractor_selectTrack(AMediaExtractor *, size_t index);
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *, uint8_t *buffer,
                                       size_t capacity);
int32_t AMediaExtractor_getSampleTrackIndex(AMediaExtractor *);
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *);
bool AMediaExtractor_advance(AMediaExtractor *);
int32_t AMediaExtractor_seekTo(AMediaExtractor *, int64_t time_us, int mode);

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime);
int32_t AMediaCodec_delete(AMediaCodec *);
int32_t AMediaCodec_configure(AMediaCodec *, const AMediaFormat *,
                               ANativeWindow *, AMediaCrypto *, uint32_t);
int32_t AMediaCodec_start(AMediaCodec *);
int32_t AMediaCodec_stop(AMediaCodec *);
int32_t AMediaCodec_flush(AMediaCodec *);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *, int64_t timeout_us);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *, size_t index, size_t *size);
int32_t AMediaCodec_queueInputBuffer(AMediaCodec *, size_t index,
                                      int64_t offset, size_t size,
                                      uint64_t time_us, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *,
                                        BsMediaCodecBufferInfo *,
                                        int64_t timeout_us);
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *, size_t index, size_t *size);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *);
int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec *, size_t index, bool render);
int32_t AMediaCodec_setOutputSurface(AMediaCodec *, ANativeWindow *);

/* The original libunity.so imports these as OPTIONAL weak symbols.
 * A MediaDataSource is a real, opaque callback-registration object. It does
 * does not invoke readAt/getSize by itself.
 * Callback userdata remains owned by the caller. Delete releases the
 * registration object WITHOUT calling close, matching Android NDK behavior.
 */
typedef ssize_t (*BsMediaDataSourceReadAt)(
    void *userdata, int64_t offset, void *buffer, size_t size);
typedef ssize_t (*BsMediaDataSourceGetSize)(void *userdata);
typedef void (*BsMediaDataSourceClose)(void *userdata);
AMediaDataSource *AMediaDataSource_new(void);
void AMediaDataSource_delete(AMediaDataSource *);
void AMediaDataSource_setUserdata(AMediaDataSource *, void *);
void AMediaDataSource_setReadAt(AMediaDataSource *, BsMediaDataSourceReadAt);
void AMediaDataSource_setGetSize(AMediaDataSource *, BsMediaDataSourceGetSize);
void AMediaDataSource_setClose(AMediaDataSource *, BsMediaDataSourceClose);

/* Android API 29: explicitly signal the caller's close callback. Not
 * imported directly by staged libunity, but useful to future real demuxers.
 * The callback can be invoked more than once, as permitted by the NDK.
 * The callback implementation must be thread safe if used across threads.
 */
void AMediaDataSource_close(AMediaDataSource *);

/* Native demuxer integration point (not an Android import). Copies the
 * registered callbacks and borrowed userdata without invoking guest code.
 * The MediaDataSource must remain alive, and its callback/userdata owners
 * must outlive any use of the snapshot. The extractor drives the callbacks
 * synchronously on the calling thread, which must already have guest TLS.
 */
typedef struct BsAndroidMediaSourceCallbacks {
    void *userdata;
    BsMediaDataSourceReadAt read_at;
    BsMediaDataSourceGetSize get_size;
    BsMediaDataSourceClose close;
} BsAndroidMediaSourceCallbacks;
bool bs_android_media_data_source_snapshot(
    const AMediaDataSource *, BsAndroidMediaSourceCallbacks *out);

/* Independent export table for prime's platform_bridge binding.
 * For data names address == (uintptr_t)&AMEDIAFORMAT_KEY_...;
 * for functions address == (uintptr_t)&AMediaFormat_... etc.
 */
typedef struct BsAndroidMediaExport {
    const char *name;
    uintptr_t address;
} BsAndroidMediaExport;
extern const BsAndroidMediaExport bs_android_media_format_exports[];
extern const size_t bs_android_media_format_exports_count;
uintptr_t bs_android_media_format_find_export(const char *name);
bool bs_android_media_decode_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
