/* CPU-buffer MediaNDK backend for the original Quest Unity player.
 * SPDX-License-Identifier: MIT
 */
#ifdef BS_MEDIA_FFMPEG
#include "android_media_format.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef __SWITCH__
#include "asset_fs_bridge.h"
#include <switch.h>
/* newlib on Switch has no pread. Serialize seek/read/restore operations
 * across decoder inputs, including multiple extractors sharing an fd. */
static Mutex g_media_fd_lock;

static ssize_t media_read_at(int fd, void *buffer, size_t count, off_t offset) {
    mutexLock(&g_media_fd_lock);
    off_t saved = lseek(fd, 0, SEEK_CUR);
    if (saved == (off_t)-1 || lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        int saved_errno = errno;
        mutexUnlock(&g_media_fd_lock);
        errno = saved_errno;
        return -1;
    }
    ssize_t result;
    do { result = read(fd, buffer, count); } while (result < 0 && errno == EINTR);
    int saved_errno = errno;
    if (lseek(fd, saved, SEEK_SET) == (off_t)-1 && result >= 0) {
        result = -1;
        saved_errno = errno;
    }
    mutexUnlock(&g_media_fd_lock);
    errno = saved_errno;
    return result;
}
#else
static ssize_t media_read_at(int fd, void *buffer, size_t count, off_t offset) {
    return pread(fd, buffer, count, offset);
}
#endif

bool bs_media_format_copy_codec_parameters(AMediaFormat *, const AVCodecParameters *);
const AVCodecParameters *bs_media_format_codec_parameters(const AMediaFormat *);

typedef struct BsCodecMime {
    enum AVCodecID id;
    const char *mime;
} BsCodecMime;

static const BsCodecMime codec_mimes[] = {
    {AV_CODEC_ID_AAC, "audio/mp4a-latm"},
    {AV_CODEC_ID_VORBIS, "audio/vorbis"},
    {AV_CODEC_ID_OPUS, "audio/opus"},
    {AV_CODEC_ID_MP3, "audio/mpeg"},
    {AV_CODEC_ID_MP2, "audio/mpeg-L2"},
    {AV_CODEC_ID_FLAC, "audio/flac"},
    {AV_CODEC_ID_PCM_MULAW, "audio/g711-mlaw"},
    {AV_CODEC_ID_PCM_ALAW, "audio/g711-alaw"},
    {AV_CODEC_ID_PCM_S16LE, "audio/raw"},
    {AV_CODEC_ID_PCM_S16BE, "audio/raw"},
    {AV_CODEC_ID_PCM_S24LE, "audio/raw"},
    {AV_CODEC_ID_PCM_S32LE, "audio/raw"},
    {AV_CODEC_ID_PCM_U8, "audio/raw"},
    {AV_CODEC_ID_PCM_F32LE, "audio/raw"},
    {AV_CODEC_ID_H264, "video/avc"},
    {AV_CODEC_ID_HEVC, "video/hevc"},
    {AV_CODEC_ID_MPEG4, "video/mp4v-es"},
    {AV_CODEC_ID_H263, "video/3gpp"},
    {AV_CODEC_ID_VP8, "video/x-vnd.on2.vp8"},
    {AV_CODEC_ID_VP9, "video/x-vnd.on2.vp9"},
    {AV_CODEC_ID_AV1, "video/av01"},
    {AV_CODEC_ID_MPEG2VIDEO, "video/mpeg2"}
};

static const char *media_mime(const AVCodecParameters *parameters) {
    for (size_t i = 0; i < sizeof(codec_mimes) / sizeof(codec_mimes[0]); ++i)
        if (codec_mimes[i].id == parameters->codec_id) return codec_mimes[i].mime;
    switch (parameters->codec_type) {
        case AVMEDIA_TYPE_AUDIO: return "audio/x-unknown";
        case AVMEDIA_TYPE_VIDEO: return "video/x-unknown";
        default: return "application/octet-stream";
    }
}

typedef struct BsMediaInput {
    int fd;
    int64_t start;
    int64_t size;
    int64_t position;
    BsAndroidMediaSourceCallbacks callbacks;
} BsMediaInput;

struct AMediaExtractor {
    BsMediaInput *input;
    AVIOContext *io;
    AVFormatContext *container;
    AVPacket *sample;
    bool sample_present;
    bool read_failed;
    bool *selected;
    size_t track_count;
};

static int read_bytes(void *opaque, uint8_t *buffer, int count) {
    BsMediaInput *input = opaque;
    if (input->size >= 0) {
        int64_t remaining = input->size - input->position;
        if (remaining <= 0) return AVERROR_EOF;
        if (count > remaining) count = (int)remaining;
    }
    ssize_t got;
    if (input->callbacks.read_at) {
        got = input->callbacks.read_at(input->callbacks.userdata, input->position,
                                      buffer, (size_t)count);
        /* The MediaDataSource contract uses -1 as end of stream. */
        if (got == -1) return AVERROR_EOF;
    } else {
        do {
            got = media_read_at(input->fd, buffer, (size_t)count,
                        (off_t)(input->start + input->position));
        } while (got < 0 && errno == EINTR);
    }
    if (got < 0 || got > count || got > INT64_MAX - input->position)
        return AVERROR(EIO);
    if (!got) return AVERROR_EOF;
    input->position += got;
    return (int)got;
}

static int64_t seek_input(void *opaque, int64_t offset, int whence) {
    BsMediaInput *input = opaque;
    int64_t base;
    switch (whence & ~AVSEEK_FORCE) {
        case AVSEEK_SIZE: return input->size >= 0 ? input->size : AVERROR(ENOSYS);
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = input->position; break;
        case SEEK_END:
            if (input->size < 0) return AVERROR(ENOSYS);
            base = input->size;
            break;
        default: return AVERROR(EINVAL);
    }
    if (offset < -base || offset > INT64_MAX - base) return AVERROR(EINVAL);
    int64_t next = base + offset;
    if (input->size >= 0 && next > input->size) return AVERROR(EINVAL);
    input->position = next;
    return next;
}

static int deny_secondary_io(AVFormatContext *container, AVIOContext **io,
                             const char *url, int flags, AVDictionary **options) {
    (void)container;
    (void)io;
    (void)url;
    (void)flags;
    (void)options;
    return AVERROR(ENOSYS);
}

static void release_extractor_source(AMediaExtractor *extractor) {
    av_packet_free(&extractor->sample);
    avformat_close_input(&extractor->container);
    if (extractor->io) {
        av_freep(&extractor->io->buffer);
        avio_context_free(&extractor->io);
    }
    if (extractor->input) {
        if (extractor->input->fd >= 0) close(extractor->input->fd);
        if (extractor->input->callbacks.close)
            extractor->input->callbacks.close(extractor->input->callbacks.userdata);
        free(extractor->input);
    }
    free(extractor->selected);
    memset(extractor, 0, sizeof(*extractor));
}

static int32_t bind_input(AMediaExtractor *extractor, BsMediaInput *input) {
    AMediaExtractor candidate = {.input = input};
    int32_t status = BS_AMEDIA_ERROR_UNKNOWN;
    const int block_size = 32768;
    uint8_t *buffer = av_malloc(block_size);
    if (!buffer) goto fail;
    candidate.io = avio_alloc_context(buffer, block_size, 0, input, read_bytes,
                                     NULL, seek_input);
    if (!candidate.io) {
        av_free(buffer);
        goto fail;
    }
    candidate.container = avformat_alloc_context();
    if (!candidate.container) goto fail;
    candidate.container->pb = candidate.io;
    candidate.container->flags |= AVFMT_FLAG_CUSTOM_IO;
    candidate.container->io_open = deny_secondary_io;
    /* Limit ourselves to self-contained media containers and elementary audio. */
    candidate.container->format_whitelist =
        av_strdup("mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,ogg,wav,flac,mp3,aac,avi,mpeg,mpegts");
    if (!candidate.container->format_whitelist) goto fail;
    if (avformat_open_input(&candidate.container, NULL, NULL, NULL) < 0 ||
        avformat_find_stream_info(candidate.container, NULL) < 0 ||
        !candidate.container->nb_streams) {
        status = BS_AMEDIA_ERROR_MALFORMED;
        goto fail;
    }
    candidate.track_count = candidate.container->nb_streams;
    candidate.selected = calloc(candidate.track_count, sizeof(*candidate.selected));
    candidate.sample = av_packet_alloc();
    if (!candidate.selected || !candidate.sample) goto fail;
    release_extractor_source(extractor);
    *extractor = candidate;
    return BS_AMEDIA_OK;
fail:
    /* Ownership of a registered custom source transfers only on success. */
    input->callbacks.close = NULL;
    release_extractor_source(&candidate);
    return status;
}

AMediaExtractor *AMediaExtractor_new(void) {
    return calloc(1, sizeof(AMediaExtractor));
}

int32_t AMediaExtractor_delete(AMediaExtractor *extractor) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    release_extractor_source(extractor);
    free(extractor);
    return BS_AMEDIA_OK;
}

int32_t AMediaExtractor_setDataSourceFd(AMediaExtractor *extractor, int fd,
                                        int64_t offset, int64_t length) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (fd < 0 || offset < 0 || length <= 0 || length > INT64_MAX - offset)
        return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || offset >= st.st_size)
        return BS_AMEDIA_ERROR_IO;
    if (length > st.st_size - offset) length = st.st_size - offset;
    BsMediaInput *input = malloc(sizeof(*input));
    if (!input) return BS_AMEDIA_ERROR_UNKNOWN;
    int owned_fd = dup(fd);
    if (owned_fd < 0) {
        free(input);
        return BS_AMEDIA_ERROR_IO;
    }
    *input = (BsMediaInput){.fd = owned_fd, .start = offset, .size = length};
    return bind_input(extractor, input);
}

int32_t AMediaExtractor_setDataSource(AMediaExtractor *extractor, const char *path) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!path || !*path) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    if (strncmp(path, "file://", 7) == 0) {
        path += 7;
        if (*path != '/') return BS_AMEDIA_ERROR_UNSUPPORTED;
    } else if (strstr(path, "://")) return BS_AMEDIA_ERROR_UNSUPPORTED;
#ifdef __SWITCH__
    int fd = bs_android_open(path, O_RDONLY);
#else
    int fd = open(path, O_RDONLY);
#endif
    if (fd < 0) return BS_AMEDIA_ERROR_IO;
    int32_t status = AMediaExtractor_setDataSourceFd(extractor, fd, 0, INT64_MAX);
    close(fd);
    return status;
}

int32_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *extractor,
                                            AMediaDataSource *source) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    BsMediaInput *input = calloc(1, sizeof(*input));
    if (!input) return BS_AMEDIA_ERROR_UNKNOWN;
    input->fd = -1;
    if (!bs_android_media_data_source_snapshot(source, &input->callbacks) ||
        !input->callbacks.read_at) {
        free(input);
        return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    }
    input->size = input->callbacks.get_size ?
        input->callbacks.get_size(input->callbacks.userdata) : -1;
    return bind_input(extractor, input);
}

size_t AMediaExtractor_getTrackCount(AMediaExtractor *extractor) {
    return extractor ? extractor->track_count : 0;
}

static int android_color_standard(enum AVColorPrimaries primaries) {
    switch (primaries) {
        case AVCOL_PRI_BT709: return 1;
        case AVCOL_PRI_BT470BG: return 2;
        case AVCOL_PRI_SMPTE170M: return 4;
        case AVCOL_PRI_BT2020: return 6;
        default: return 0;
    }
}

static void set_color(AMediaFormat *format, enum AVColorPrimaries primaries,
                      enum AVColorRange range) {
    int standard = android_color_standard(primaries);
    if (standard) AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, standard);
    if (range == AVCOL_RANGE_JPEG || range == AVCOL_RANGE_MPEG)
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE,
                              range == AVCOL_RANGE_JPEG ? 1 : 2);
}

AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *extractor, size_t index) {
    if (!extractor || index >= extractor->track_count) return NULL;
    AVStream *track = extractor->container->streams[index];
    AVCodecParameters *parameters = track->codecpar;
    AMediaFormat *format = AMediaFormat_new();
    if (!format) return NULL;
    if (!bs_media_format_copy_codec_parameters(format, parameters)) {
        AMediaFormat_delete(format);
        return NULL;
    }
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, media_mime(parameters));
    int64_t duration = track->duration != AV_NOPTS_VALUE ?
        av_rescale_q(track->duration, track->time_base, AV_TIME_BASE_Q) :
        extractor->container->duration;
    if (duration != AV_NOPTS_VALUE)
        AMediaFormat_setInt64(format, AMEDIAFORMAT_KEY_DURATION, duration);
    if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, parameters->ch_layout.nb_channels);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, parameters->sample_rate);
        if (parameters->initial_padding > 0)
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ENCODER_DELAY, parameters->initial_padding);
    }
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, parameters->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, parameters->height);
        AVRational rate = av_guess_frame_rate(extractor->container, track, NULL);
        if (rate.num > 0 && rate.den > 0)
            AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, (float)av_q2d(rate));
        set_color(format, parameters->color_primaries, parameters->color_range);
    }
    const AVDictionaryEntry *language = av_dict_get(track->metadata, "language", NULL, 0);
    if (language) AMediaFormat_setString(format, AMEDIAFORMAT_KEY_LANGUAGE, language->value);
    return format;
}

int32_t AMediaExtractor_selectTrack(AMediaExtractor *extractor, size_t index) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (index >= extractor->track_count) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    extractor->selected[index] = true;
    return BS_AMEDIA_OK;
}

static bool current_sample(AMediaExtractor *extractor) {
    if (!extractor || !extractor->container || extractor->read_failed) return false;
    if (extractor->sample_present) return true;
    bool has_selection = false;
    for (size_t i = 0; i < extractor->track_count; ++i) has_selection |= extractor->selected[i];
    if (!has_selection) return false;
    AVPacket *sample = extractor->sample;
    while (av_read_frame(extractor->container, sample) >= 0) {
        if (sample->stream_index >= 0 &&
            (size_t)sample->stream_index < extractor->track_count &&
            extractor->selected[sample->stream_index]) {
            extractor->sample_present = true;
            return true;
        }
        av_packet_unref(sample);
    }
    extractor->read_failed = true;
    return false;
}

ssize_t AMediaExtractor_readSampleData(AMediaExtractor *extractor, uint8_t *buffer,
                                       size_t capacity) {
    if (!current_sample(extractor)) return -1;
    AVPacket *sample = extractor->sample;
    if (sample->size < 0 || (size_t)sample->size > capacity || (!buffer && sample->size))
        return -1;
    if (av_packet_get_side_data(sample, AV_PKT_DATA_ENCRYPTION_INFO, NULL)) return -1;
    if (sample->size) memcpy(buffer, sample->data, (size_t)sample->size);
    return sample->size;
}

int32_t AMediaExtractor_getSampleTrackIndex(AMediaExtractor *extractor) {
    return current_sample(extractor) ? extractor->sample->stream_index : -1;
}

int64_t AMediaExtractor_getSampleTime(AMediaExtractor *extractor) {
    if (!current_sample(extractor)) return -1;
    AVPacket *sample = extractor->sample;
    int64_t timestamp = sample->pts == AV_NOPTS_VALUE ? sample->dts : sample->pts;
    if (timestamp == AV_NOPTS_VALUE) return -1;
    AVStream *track = extractor->container->streams[sample->stream_index];
    return av_rescale_q(timestamp, track->time_base, AV_TIME_BASE_Q);
}

bool AMediaExtractor_advance(AMediaExtractor *extractor) {
    if (!current_sample(extractor)) return false;
    av_packet_unref(extractor->sample);
    extractor->sample_present = false;
    return current_sample(extractor);
}

int32_t AMediaExtractor_seekTo(AMediaExtractor *extractor, int64_t time_us, int mode) {
    if (!extractor) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!extractor->container) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    if (time_us < 0 || mode < 0 || mode > 2) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    int stream = -1;
    for (size_t i = 0; i < extractor->track_count; ++i) {
        if (!extractor->selected[i]) continue;
        if (stream < 0) stream = (int)i;
        if (extractor->container->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream = (int)i;
            break;
        }
    }
    if (stream < 0) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    AVStream *track = extractor->container->streams[stream];
    int64_t timestamp = av_rescale_q(time_us, AV_TIME_BASE_Q, track->time_base);
    if (mode == 2) {
        const AVIndexEntry *previous = avformat_index_get_entry_from_timestamp(track, timestamp, AVSEEK_FLAG_BACKWARD);
        const AVIndexEntry *next = avformat_index_get_entry_from_timestamp(track, timestamp, 0);
        if (next && (!previous ||
            (uint64_t)next->timestamp - timestamp < (uint64_t)timestamp - previous->timestamp))
            mode = 1;
        else
            mode = 0;
    }
    if (av_seek_frame(extractor->container, stream, timestamp,
                      mode == 0 ? AVSEEK_FLAG_BACKWARD : 0) < 0)
        return BS_AMEDIA_ERROR_IO;
    av_packet_unref(extractor->sample);
    extractor->sample_present = false;
    extractor->read_failed = false;
    return BS_AMEDIA_OK;
}

enum {
    BS_MEDIA_TRY_AGAIN = -1,
    BS_MEDIA_FORMAT_CHANGED = -2,
    BS_MEDIA_KEY_FRAME = 1,
    BS_MEDIA_CODEC_CONFIG = 2,
    BS_MEDIA_END_OF_STREAM = 4,
    BS_MEDIA_YUV420_PLANAR = 19,
    BS_MEDIA_YUV420_SEMIPLANAR = 21,
    BS_MEDIA_YUV420_FLEXIBLE = 0x7f420888,
    BS_MEDIA_INPUT_CAPACITY = 4 * 1024 * 1024
};

struct AMediaCodec {
    const AVCodec *decoder;
    AVCodecContext *context;
    AVPacket *pending;
    AVFrame *frame;
    uint8_t *output;
    unsigned output_capacity;
    BsMediaCodecBufferInfo info;
    struct SwsContext *scaler;
    struct SwrContext *resampler;
    int32_t color_format;
    int32_t width;
    int32_t height;
    int32_t channels;
    int32_t sample_rate;
    enum AVColorRange color_range;
    enum AVColorPrimaries color_primaries;
    int64_t eos_timestamp;
    bool started;
    bool input_leased;
    bool input_pending;
    bool eos_queued;
    bool eos_sent;
    bool eos_returned;
    bool output_pending;
    bool output_leased;
    bool format_changed;
};

AMediaCodec *AMediaCodec_createDecoderByType(const char *mime) {
    if (!mime) return NULL;
    for (size_t i = 0; i < sizeof(codec_mimes) / sizeof(codec_mimes[0]); ++i) {
        if (strcmp(mime, codec_mimes[i].mime) != 0) continue;
        const AVCodec *decoder = avcodec_find_decoder(codec_mimes[i].id);
        if (!decoder) continue;
        AMediaCodec *codec = calloc(1, sizeof(*codec));
        if (codec) codec->decoder = decoder;
        return codec;
    }
    return NULL;
}

static void reset_codec_buffers(AMediaCodec *codec) {
    av_packet_unref(codec->pending);
    av_frame_unref(codec->frame);
    swr_free(&codec->resampler);
    codec->input_leased = false;
    codec->input_pending = false;
    codec->eos_queued = false;
    codec->eos_sent = false;
    codec->eos_returned = false;
    codec->output_pending = false;
    codec->output_leased = false;
    codec->format_changed = false;
    codec->width = 0;
    codec->height = 0;
    codec->channels = 0;
    codec->sample_rate = 0;
}

static void release_codec_resources(AMediaCodec *codec) {
    avcodec_free_context(&codec->context);
    av_packet_free(&codec->pending);
    av_frame_free(&codec->frame);
    av_freep(&codec->output);
    codec->output_capacity = 0;
    sws_freeContext(codec->scaler);
    codec->scaler = NULL;
    swr_free(&codec->resampler);
}

int32_t AMediaCodec_delete(AMediaCodec *codec) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    release_codec_resources(codec);
    free(codec);
    return BS_AMEDIA_OK;
}

int32_t AMediaCodec_configure(AMediaCodec *codec, const AMediaFormat *format,
                               ANativeWindow *surface, AMediaCrypto *crypto, uint32_t flags) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!format) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    if (surface || crypto || flags) return BS_AMEDIA_ERROR_UNSUPPORTED;
    if (codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    const AVCodecParameters *parameters = bs_media_format_codec_parameters(format);
    const AVCodec *decoder = codec->decoder;
    if (parameters) {
        const char *requested_mime = NULL;
        if (!AMediaFormat_getString((AMediaFormat *)format, AMEDIAFORMAT_KEY_MIME, &requested_mime) ||
            strcmp(requested_mime, media_mime(parameters)) ||
            parameters->codec_type != decoder->type)
            return BS_AMEDIA_ERROR_INVALID_PARAMETER;
        if (parameters->codec_id != decoder->id &&
            strcmp(requested_mime, "audio/raw") != 0)
            return BS_AMEDIA_ERROR_INVALID_PARAMETER;
        decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) return BS_AMEDIA_ERROR_UNSUPPORTED;
    }
    int32_t color_format = BS_MEDIA_YUV420_PLANAR;
    if (decoder->type == AVMEDIA_TYPE_VIDEO) {
        (void)AMediaFormat_getInt32((AMediaFormat *)format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format);
        if (color_format == BS_MEDIA_YUV420_FLEXIBLE) color_format = BS_MEDIA_YUV420_PLANAR;
        if (color_format != BS_MEDIA_YUV420_PLANAR && color_format != BS_MEDIA_YUV420_SEMIPLANAR)
            return BS_AMEDIA_ERROR_UNSUPPORTED;
    }
    int32_t status = BS_AMEDIA_ERROR_UNKNOWN;
    AVCodecContext *context = avcodec_alloc_context3(decoder);
    AVPacket *pending = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!context || !pending || !frame) goto fail;
    if (parameters && avcodec_parameters_to_context(context, parameters) < 0) goto fail;
    int32_t value;
    if (AMediaFormat_getInt32((AMediaFormat *)format, AMEDIAFORMAT_KEY_WIDTH, &value))
        context->width = value;
    if (AMediaFormat_getInt32((AMediaFormat *)format, AMEDIAFORMAT_KEY_HEIGHT, &value))
        context->height = value;
    if (AMediaFormat_getInt32((AMediaFormat *)format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &value))
        context->sample_rate = value;
    if (AMediaFormat_getInt32((AMediaFormat *)format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &value) &&
        value != context->ch_layout.nb_channels) {
        if (value <= 0 || value > 64) {
            status = BS_AMEDIA_ERROR_INVALID_PARAMETER;
            goto fail;
        }
        av_channel_layout_uninit(&context->ch_layout);
        av_channel_layout_default(&context->ch_layout, value);
    }
    context->pkt_timebase = AV_TIME_BASE_Q;
    context->thread_count = 1;
    context->max_pixels = 4096 * 2160;
    if (avcodec_open2(context, decoder, NULL) < 0) {
        status = BS_AMEDIA_ERROR_UNSUPPORTED;
        goto fail;
    }
    release_codec_resources(codec);
    codec->context = context;
    codec->pending = pending;
    codec->frame = frame;
    codec->color_format = color_format;
    reset_codec_buffers(codec);
    return BS_AMEDIA_OK;
fail:
    avcodec_free_context(&context);
    av_packet_free(&pending);
    av_frame_free(&frame);
    return status;
}

int32_t AMediaCodec_start(AMediaCodec *codec) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->context || codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    codec->started = true;
    return BS_AMEDIA_OK;
}

int32_t AMediaCodec_stop(AMediaCodec *codec) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    codec->started = false;
    release_codec_resources(codec);
    return BS_AMEDIA_OK;
}

int32_t AMediaCodec_flush(AMediaCodec *codec) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    avcodec_flush_buffers(codec->context);
    reset_codec_buffers(codec);
    return BS_AMEDIA_OK;
}

ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *codec, int64_t timeout_us) {
    (void)timeout_us;
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    if (codec->input_leased || codec->input_pending || codec->eos_queued)
        return BS_MEDIA_TRY_AGAIN;
    if (av_new_packet(codec->pending, BS_MEDIA_INPUT_CAPACITY) < 0)
        return BS_AMEDIA_ERROR_UNKNOWN;
    codec->input_leased = true;
    return 0;
}

uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *codec, size_t index, size_t *size) {
    if (size) *size = 0;
    if (!codec || !codec->started || !codec->input_leased || index) return NULL;
    if (size) *size = BS_MEDIA_INPUT_CAPACITY;
    return codec->pending->data;
}

static int submit_packet(AMediaCodec *codec) {
    int result = 0;
    if (codec->input_pending) {
        result = avcodec_send_packet(codec->context, codec->pending);
        if (result != AVERROR(EAGAIN)) {
            av_packet_unref(codec->pending);
            codec->input_pending = false;
        }
        if (result < 0) return result;
    }
    if (codec->eos_queued && !codec->eos_sent) {
        result = avcodec_send_packet(codec->context, NULL);
        if (result >= 0 || result == AVERROR_EOF) codec->eos_sent = true;
    }
    return result;
}

int32_t AMediaCodec_queueInputBuffer(AMediaCodec *codec, size_t index, int64_t offset,
                                      size_t size, uint64_t time_us, uint32_t flags) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->started || !codec->input_leased) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    if (index || offset < 0 || offset > BS_MEDIA_INPUT_CAPACITY ||
        size > BS_MEDIA_INPUT_CAPACITY - (size_t)offset)
        return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    if (flags & ~(BS_MEDIA_KEY_FRAME | BS_MEDIA_CODEC_CONFIG | BS_MEDIA_END_OF_STREAM))
        return BS_AMEDIA_ERROR_UNSUPPORTED;
    /* Initialization data from getTrackFormat is already installed. Unity's
     * normal extractor feeds media samples directly. */
    if (flags & BS_MEDIA_CODEC_CONFIG) return BS_AMEDIA_ERROR_UNSUPPORTED;
    codec->input_leased = false;
    codec->eos_queued = (flags & BS_MEDIA_END_OF_STREAM) != 0;
    if (codec->eos_queued) codec->eos_timestamp = (int64_t)time_us;
    if (size) {
        AVPacket *pending = codec->pending;
        memmove(pending->data, pending->data + offset, size);
        av_shrink_packet(pending, (int)size);
        pending->pts = (int64_t)time_us;
        if (flags & BS_MEDIA_KEY_FRAME) pending->flags |= AV_PKT_FLAG_KEY;
        codec->input_pending = true;
    } else {
        av_packet_unref(codec->pending);
    }
    int result = submit_packet(codec);
    return result >= 0 || result == AVERROR(EAGAIN) || result == AVERROR_EOF ?
        BS_AMEDIA_OK : BS_AMEDIA_ERROR_MALFORMED;
}

static int pack_audio_frame(AMediaCodec *codec, AVFrame *frame) {
    int count = frame->ch_layout.nb_channels;
    if (frame->sample_rate <= 0 || count <= 0 || count > 64) return AVERROR_INVALIDDATA;
    AVChannelLayout channels = {0};
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&channels, count);
    } else if (av_channel_layout_copy(&channels, &frame->ch_layout) < 0) {
        return AVERROR(ENOMEM);
    }
    /* Rate and channel layout stay identical. Only storage representation
     * changes to packed signed 16-bit samples, with no resampling delay. */
    int error = swr_alloc_set_opts2(&codec->resampler, &channels, AV_SAMPLE_FMT_S16,
                                    frame->sample_rate, &channels, frame->format,
                                    frame->sample_rate, 0, NULL);
    av_channel_layout_uninit(&channels);
    if (error < 0 || (error = swr_init(codec->resampler)) < 0) return error;
    int bytes = av_samples_get_buffer_size(NULL, count, frame->nb_samples, AV_SAMPLE_FMT_S16, 1);
    if (bytes < 0) return bytes;
    av_fast_malloc(&codec->output, &codec->output_capacity, (size_t)bytes);
    if (!codec->output) return AVERROR(ENOMEM);
    int samples = swr_convert(codec->resampler, &codec->output, frame->nb_samples,
                             (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (samples < 0) return samples;
    codec->format_changed = codec->channels != count || codec->sample_rate != frame->sample_rate;
    codec->channels = count;
    codec->sample_rate = frame->sample_rate;
    return samples * count * (int)sizeof(int16_t);
}

static int pack_video_frame(AMediaCodec *codec, AVFrame *frame) {
    enum AVPixelFormat format = codec->color_format == BS_MEDIA_YUV420_SEMIPLANAR ?
        AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    int bytes = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    if (bytes < 0) return bytes;
    av_fast_malloc(&codec->output, &codec->output_capacity, (size_t)bytes);
    if (!codec->output) return AVERROR(ENOMEM);
    if (format == frame->format) {
        const uint8_t *source[4] = {frame->data[0], frame->data[1], frame->data[2], frame->data[3]};
        int result = av_image_copy_to_buffer(codec->output, bytes, source, frame->linesize,
                                             format, frame->width, frame->height, 1);
        if (result < 0) return result;
    } else {
        codec->scaler = sws_getCachedContext(codec->scaler, frame->width, frame->height,
                                            frame->format, frame->width, frame->height,
                                            format, SWS_BILINEAR, NULL, NULL, NULL);
        if (!codec->scaler) return AVERROR(ENOSYS);
        uint8_t *planes[4];
        int pitches[4];
        if (av_image_fill_arrays(planes, pitches, codec->output, format,
                                 frame->width, frame->height, 1) < 0)
            return AVERROR_INVALIDDATA;
        const uint8_t *source[4] = {frame->data[0], frame->data[1], frame->data[2], frame->data[3]};
        if (sws_scale(codec->scaler, source, frame->linesize, 0,
                      frame->height, planes, pitches) != frame->height)
            return AVERROR_INVALIDDATA;
    }
    codec->format_changed = codec->width != frame->width || codec->height != frame->height ||
        codec->color_range != frame->color_range || codec->color_primaries != frame->color_primaries;
    codec->width = frame->width;
    codec->height = frame->height;
    codec->color_range = frame->color_range;
    codec->color_primaries = frame->color_primaries;
    return bytes;
}

ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *codec, BsMediaCodecBufferInfo *info,
                                        int64_t timeout_us) {
    (void)timeout_us;
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!info) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    if (!codec->started) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    if (codec->output_leased || codec->eos_returned) return BS_MEDIA_TRY_AGAIN;
    if (!codec->output_pending) {
        int submitted = submit_packet(codec);
        if (submitted < 0 && submitted != AVERROR(EAGAIN) && submitted != AVERROR_EOF)
            return BS_AMEDIA_ERROR_MALFORMED;
        AVFrame *frame = codec->frame;
        int result = avcodec_receive_frame(codec->context, frame);
        if (result == AVERROR(EAGAIN)) return BS_MEDIA_TRY_AGAIN;
        if (result == AVERROR_EOF && codec->eos_sent) {
            codec->info = (BsMediaCodecBufferInfo){
                .presentationTimeUs = codec->eos_timestamp,
                .flags = BS_MEDIA_END_OF_STREAM
            };
            codec->eos_returned = true;
        } else {
            if (result < 0) return BS_AMEDIA_ERROR_MALFORMED;
            int bytes = codec->context->codec_type == AVMEDIA_TYPE_AUDIO ?
                pack_audio_frame(codec, frame) : pack_video_frame(codec, frame);
            int64_t timestamp = frame->best_effort_timestamp;
            uint32_t flags = frame->flags & AV_FRAME_FLAG_KEY ? BS_MEDIA_KEY_FRAME : 0;
            av_frame_unref(frame);
            if (bytes < 0) return BS_AMEDIA_ERROR_MALFORMED;
            codec->info = (BsMediaCodecBufferInfo){
                .size = bytes,
                .presentationTimeUs = timestamp == AV_NOPTS_VALUE ? 0 : timestamp,
                .flags = flags
            };
        }
        codec->output_pending = true;
        if (codec->format_changed) {
            codec->format_changed = false;
            return BS_MEDIA_FORMAT_CHANGED;
        }
    }
    codec->output_pending = false;
    codec->output_leased = true;
    *info = codec->info;
    return 0;
}

uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *codec, size_t index, size_t *size) {
    if (size) *size = 0;
    if (!codec || !codec->started || !codec->output_leased || index) return NULL;
    if (size) *size = (size_t)codec->info.size;
    return codec->output;
}

AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *codec) {
    if (!codec || !codec->started || (!codec->channels && !codec->width)) return NULL;
    AMediaFormat *format = AMediaFormat_new();
    if (!format) return NULL;
    if (codec->channels) {
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/raw");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, codec->channels);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, codec->sample_rate);
        AMediaFormat_setInt32(format, "pcm-encoding", 2);
    } else {
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/raw");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, codec->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, codec->height);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, codec->color_format);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, codec->width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, codec->height);
        set_color(format, codec->color_primaries, codec->color_range);
    }
    return format;
}

int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec *codec, size_t index, bool render) {
    if (!codec) return BS_AMEDIA_ERROR_INVALID_OBJECT;
    if (!codec->started || !codec->output_leased) return BS_AMEDIA_ERROR_INVALID_OPERATION;
    if (index) return BS_AMEDIA_ERROR_INVALID_PARAMETER;
    if (render) return BS_AMEDIA_ERROR_UNSUPPORTED;
    codec->output_leased = false;
    return BS_AMEDIA_OK;
}

int32_t AMediaCodec_setOutputSurface(AMediaCodec *codec, ANativeWindow *surface) {
    (void)surface;
    return codec ? BS_AMEDIA_ERROR_UNSUPPORTED : BS_AMEDIA_ERROR_INVALID_OBJECT;
}

typedef struct BsMediaSelftestSource {
    bool closed;
} BsMediaSelftestSource;

/* Eight G.711 µ-law samples in a RIFF/WAVE container. */
static const uint8_t selftest_wave[] = {
    'R','I','F','F', 46,0,0,0, 'W','A','V','E',
    'f','m','t',' ', 18,0,0,0, 7,0, 1,0, 0x40,0x1f,0,0,
    0x40,0x1f,0,0, 1,0, 8,0, 0,0,
    'd','a','t','a', 8,0,0,0, 0xff,0x7f,0x00,0x80,0x7e,0xfe,0x55,0xd5
};

static ssize_t selftest_read(void *user, int64_t offset, void *buffer, size_t size) {
    BsMediaSelftestSource *state = user;
    if (state->closed || offset < 0 || offset >= (int64_t)sizeof(selftest_wave)) return -1;
    size_t available = sizeof(selftest_wave) - (size_t)offset;
    if (size > available) size = available;
    memcpy(buffer, selftest_wave + offset, size);
    return (ssize_t)size;
}

static ssize_t selftest_size(void *user) {
    (void)user;
    return sizeof(selftest_wave);
}

static void selftest_close(void *user) {
    ((BsMediaSelftestSource *)user)->closed = true;
}

bool bs_android_media_decode_selftest(void) {
    static const int16_t expected[] = {0, 0, -32124, 32124, -8, 8, -716, 716};
    BsMediaSelftestSource state = {0};
    AMediaDataSource *source = AMediaDataSource_new();
    AMediaExtractor *extractor = AMediaExtractor_new();
    AMediaFormat *track = NULL;
    AMediaFormat *output = NULL;
    AMediaCodec *codec = NULL;
    bool okay = false;
    if (!source || !extractor) goto done;
    AMediaDataSource_setUserdata(source, &state);
    AMediaDataSource_setReadAt(source, selftest_read);
    AMediaDataSource_setGetSize(source, selftest_size);
    AMediaDataSource_setClose(source, selftest_close);
    if (AMediaExtractor_setDataSourceCustom(extractor, source) != BS_AMEDIA_OK ||
        AMediaExtractor_getTrackCount(extractor) != 1 ||
        AMediaExtractor_selectTrack(extractor, 0) != BS_AMEDIA_OK)
        goto done;
    track = AMediaExtractor_getTrackFormat(extractor, 0);
    const char *mime;
    if (!track || !AMediaFormat_getString(track, AMEDIAFORMAT_KEY_MIME, &mime))
        goto done;
    codec = AMediaCodec_createDecoderByType(mime);
    if (!codec || AMediaCodec_configure(codec, track, NULL, NULL, 0) != BS_AMEDIA_OK ||
        AMediaCodec_start(codec) != BS_AMEDIA_OK || AMediaCodec_dequeueInputBuffer(codec, 0) != 0)
        goto done;
    size_t capacity;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, 0, &capacity);
    if (!input || AMediaExtractor_readSampleData(extractor, input, capacity) != 8 ||
        AMediaExtractor_getSampleTrackIndex(extractor) != 0 ||
        AMediaExtractor_getSampleTime(extractor) != 0 ||
        AMediaExtractor_advance(extractor) ||
        AMediaCodec_queueInputBuffer(codec, 0, 0, 8, 0, BS_MEDIA_END_OF_STREAM) != BS_AMEDIA_OK)
        goto done;
    BsMediaCodecBufferInfo info = {0};
    if (AMediaCodec_dequeueOutputBuffer(codec, &info, 0) != BS_MEDIA_FORMAT_CHANGED)
        goto done;
    output = AMediaCodec_getOutputFormat(codec);
    int32_t sample_rate;
    int32_t channels;
    if (!output ||
        !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sample_rate) ||
        !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels) ||
        sample_rate != 8000 || channels != 1 ||
        AMediaCodec_dequeueOutputBuffer(codec, &info, 0) != 0 ||
        info.size != sizeof(expected) || info.presentationTimeUs != 0)
        goto done;
    size_t size;
    uint8_t *pcm = AMediaCodec_getOutputBuffer(codec, 0, &size);
    if (!pcm || size != sizeof(expected) || memcmp(pcm, expected, size) ||
        AMediaCodec_releaseOutputBuffer(codec, 0, false) != BS_AMEDIA_OK ||
        AMediaCodec_dequeueOutputBuffer(codec, &info, 0) != 0 ||
        info.size || !(info.flags & BS_MEDIA_END_OF_STREAM))
        goto done;
    okay = true;
done:
    AMediaFormat_delete(output);
    AMediaFormat_delete(track);
    if (codec) AMediaCodec_delete(codec);
    if (extractor) AMediaExtractor_delete(extractor);
    AMediaDataSource_delete(source);
    return okay && state.closed;
}

#endif
