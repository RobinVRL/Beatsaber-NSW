/* Minimal OpenSL ES (Android flavour) output for FMOD on Switch.
 * MIT license; see ../LICENSE.
 *
 * Beat Saber 1.40.8's FMOD only has an OpenSL ES output (no AAudio). This
 * implements the engine / output mix / audio player objects with the
 * Android simple buffer queue, play, volume and configuration interfaces.
 * PCM queued by FMOD is pulled by the existing AAudio->audout bridge stream,
 * whose worker thread already carries guest TLS, so the guest buffer queue
 * callback is invoked from there.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <switch.h>

#include "android_audio_bridge.h"
#include "util.h"

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef uint8_t SLboolean;
enum {
    SL_RESULT_SUCCESS = 0,
    SL_RESULT_PRECONDITIONS_VIOLATED = 1,
    SL_RESULT_PARAMETER_INVALID = 2,
    SL_RESULT_MEMORY_FAILURE = 3,
    SL_RESULT_RESOURCE_ERROR = 4,
    SL_RESULT_BUFFER_INSUFFICIENT = 7,
    SL_RESULT_CONTENT_UNSUPPORTED = 9,
    SL_RESULT_FEATURE_UNSUPPORTED = 12,
};
#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATALOCATOR_BUFFERQUEUE 6
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BD
#define SL_DATAFORMAT_PCM 2
#define SL_ANDROID_DATAFORMAT_PCM_EX 4
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT 3

typedef struct { uint32_t a; uint16_t b, c, d; uint8_t e[6]; } SLInterfaceID_;
typedef const SLInterfaceID_ *SLInterfaceID;

/* Interface IDs (values are ours; FMOD obtains them via dlsym). */
#define IID(n, k) static const SLInterfaceID_ iid_##n = {0x51a5d000u + k, 0, 0, 0, {0}}; \
    const SLInterfaceID SL_IID_##n = &iid_##n;
IID(NULL, 0) IID(OBJECT, 1) IID(ENGINE, 2) IID(PLAY, 3) IID(BUFFERQUEUE, 4)
IID(ANDROIDSIMPLEBUFFERQUEUE, 5) IID(VOLUME, 6) IID(ANDROIDCONFIGURATION, 7)
IID(OUTPUTMIX, 8) IID(ENGINECAPABILITIES, 9) IID(ENVIRONMENTALREVERB, 10)
IID(PRESETREVERB, 11) IID(EFFECTSEND, 12) IID(PLAYBACKRATE, 13) IID(SEEK, 14)
IID(PREFETCHSTATUS, 15) IID(METADATAEXTRACTION, 16) IID(RECORD, 17)
IID(ANDROIDEFFECT, 18) IID(ANDROIDEFFECTSEND, 19) IID(MUTESOLO, 20)

static bool iid_eq(SLInterfaceID a, SLInterfaceID b) {
    return a && b && (a == b || !memcmp(a, b, sizeof *a));
}

/* ---- object --------------------------------------------------------- */
typedef struct SLObj SLObj;
typedef SLresult (*GetItfFn)(SLObj *, SLInterfaceID, void *);
typedef struct {
    SLresult (*Realize)(void *, SLboolean);
    SLresult (*Resume)(void *, SLboolean);
    SLresult (*GetState)(void *, SLuint32 *);
    SLresult (*GetInterface)(void *, SLInterfaceID, void *);
    SLresult (*RegisterCallback)(void *, void *, void *);
    void (*AbortAsyncOperation)(void *);
    void (*Destroy)(void *);
    SLresult (*SetPriority)(void *, int32_t, SLboolean);
    SLresult (*GetPriority)(void *, int32_t *, SLboolean *);
    SLresult (*SetLossOfControlInterfaces)(void *, int16_t, SLInterfaceID *, SLboolean);
} ObjVt;

typedef void (*BqCallback)(void *itf, void *ctx);
#define BQ_MAX 16

struct SLObj {
    const ObjVt *obj_itf;          /* SLObjectItf points here */
    const void *main_itf;          /* engine / play */
    const void *bq_itf;
    const void *vol_itf;
    const void *cfg_itf;
    int kind;                      /* 0 engine, 1 mix, 2 player */
    void (*destroy)(SLObj *);
    /* player */
    Mutex lock;
    struct { const uint8_t *data; uint32_t size; } q[BQ_MAX];
    uint32_t q_head, q_count, q_capacity, q_index, q_offset;
    BqCallback cb; void *cb_ctx;
    uint32_t play_state;
    uint32_t channels, rate, bytes_per_frame;
    int32_t format;
    uint64_t frames_played;
    AAudioStream *stream;
    bool started;
};

#define FROM(itf, field) ((SLObj *)((uint8_t *)(itf) - offsetof(SLObj, field)))

static SLresult o_Realize(void *s, SLboolean a) { (void)a; debugPrintf("[opensl] Realize kind=%d\n", ((SLObj *)s)->kind); return SL_RESULT_SUCCESS; }
static SLresult o_Resume(void *s, SLboolean a) { (void)s; (void)a; return SL_RESULT_SUCCESS; }
static SLresult o_GetState(void *s, SLuint32 *st) {
    (void)s; if (!st) return SL_RESULT_PARAMETER_INVALID;
    *st = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS;
}
static SLresult o_RegisterCallback(void *s, void *cb, void *ctx) {
    (void)s; (void)cb; (void)ctx; return SL_RESULT_SUCCESS;
}
static void o_Abort(void *s) { (void)s; }
static SLresult o_SetPriority(void *s, int32_t p, SLboolean b) { (void)s; (void)p; (void)b; return SL_RESULT_SUCCESS; }
static SLresult o_GetPriority(void *s, int32_t *p, SLboolean *b) {
    (void)s; if (p) *p = 0; if (b) *b = 1; return SL_RESULT_SUCCESS;
}
static SLresult o_SetLoss(void *s, int16_t n, SLInterfaceID *i, SLboolean e) {
    (void)s; (void)n; (void)i; (void)e; return SL_RESULT_SUCCESS;
}
static void o_Destroy(void *s) {
    SLObj *o = FROM(s, obj_itf);
    if (o->destroy) o->destroy(o);
    free(o);
}
static SLresult o_GetInterface(void *s, SLInterfaceID iid, void *out);
static const ObjVt g_obj_vt = {
    o_Realize, o_Resume, o_GetState, o_GetInterface, o_RegisterCallback,
    o_Abort, o_Destroy, o_SetPriority, o_GetPriority, o_SetLoss,
};

/* ---- player: data pump ------------------------------------------------ */
static int32_t pump(AAudioStream *stream, void *user, void *audio, int32_t frames) {
    (void)stream;
    SLObj *p = user;
    uint8_t *dst = audio;
    size_t need = (size_t)frames * p->bytes_per_frame;
    int completed = 0;
    mutexLock(&p->lock);
    if (p->play_state == SL_PLAYSTATE_PLAYING) {
        while (need && p->q_count) {
            uint32_t h = p->q_head;
            size_t left = p->q[h].size - p->q_offset;
            size_t n = left < need ? left : need;
            memcpy(dst, p->q[h].data + p->q_offset, n);
            dst += n; need -= n; p->q_offset += (uint32_t)n;
            if (p->q_offset >= p->q[h].size) {
                p->q_head = (h + 1) % BQ_MAX;
                p->q_count--; p->q_index++; p->q_offset = 0;
                completed++;
            }
        }
        p->frames_played += (uint64_t)frames - need / p->bytes_per_frame;
    }
    BqCallback cb = p->cb; void *ctx = p->cb_ctx;
    mutexUnlock(&p->lock);
    if (need) memset(dst, 0, need);
    static unsigned starved;
    if (need && !completed && p->play_state == SL_PLAYSTATE_PLAYING) {
        /* Queue ran dry: give FMOD a chance to refill (it only enqueues
         * from the completion callback). */
        if (starved++ < 4)
            debugPrintf("[opensl] queue empty (count=%u index=%u)\n", p->q_count, p->q_index);
        if (cb && !p->q_count) cb((void *)&p->bq_itf, ctx);
    }
    /* FMOD enqueues the next buffer from this callback. */
    for (int i = 0; i < completed && cb; ++i) cb((void *)&p->bq_itf, ctx);
    return 0; /* AAUDIO_CALLBACK_RESULT_CONTINUE */
}

static void player_destroy(SLObj *p) {
    if (p->stream) {
        AAudioStream_requestStop(p->stream);
        AAudioStream_close(p->stream);
    }
}

/* ---- buffer queue ------------------------------------------------------- */
static SLresult bq_Enqueue(void *s, const void *buf, SLuint32 size) {
    SLObj *p = FROM(s, bq_itf);
    if (!buf || !size) return SL_RESULT_PARAMETER_INVALID;
    mutexLock(&p->lock);
    if (p->q_count >= p->q_capacity) { mutexUnlock(&p->lock); return SL_RESULT_BUFFER_INSUFFICIENT; }
    uint32_t t = (p->q_head + p->q_count) % BQ_MAX;
    p->q[t].data = buf; p->q[t].size = size;
    p->q_count++;
    mutexUnlock(&p->lock);
    static unsigned logged;
    if (logged++ < 4) debugPrintf("[opensl] Enqueue %u bytes\n", size);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(void *s) {
    SLObj *p = FROM(s, bq_itf);
    mutexLock(&p->lock);
    p->q_count = 0; p->q_offset = 0;
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(void *s, SLuint32 *st) {
    SLObj *p = FROM(s, bq_itf);
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    mutexLock(&p->lock);
    st[0] = p->q_count; st[1] = p->q_index;
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(void *s, BqCallback cb, void *ctx) {
    SLObj *p = FROM(s, bq_itf);
    mutexLock(&p->lock);
    p->cb = cb; p->cb_ctx = ctx;
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}
static const struct {
    SLresult (*Enqueue)(void *, const void *, SLuint32);
    SLresult (*Clear)(void *);
    SLresult (*GetState)(void *, SLuint32 *);
    SLresult (*RegisterCallback)(void *, BqCallback, void *);
} g_bq_vt = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

/* ---- play --------------------------------------------------------------- */
static SLresult pl_SetPlayState(void *s, SLuint32 state) {
    SLObj *p = FROM(s, main_itf);
    if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING)
        return SL_RESULT_PARAMETER_INVALID;
    mutexLock(&p->lock);
    p->play_state = state;
    if (state == SL_PLAYSTATE_STOPPED) { p->q_count = 0; p->q_offset = 0; p->frames_played = 0; }
    bool start = state == SL_PLAYSTATE_PLAYING && !p->started;
    if (start) p->started = true;
    mutexUnlock(&p->lock);
    debugPrintf("[opensl] SetPlayState %u\n", state);
    if (start && p->stream) {
        int32_t rc = AAudioStream_requestStart(p->stream);
        debugPrintf("[opensl] output start -> %d\n", rc);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult pl_GetPlayState(void *s, SLuint32 *st) {
    SLObj *p = FROM(s, main_itf);
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    *st = p->play_state; return SL_RESULT_SUCCESS;
}
static SLresult pl_GetDuration(void *s, SLuint32 *ms) { (void)s; if (ms) *ms = 0xFFFFFFFFu; return SL_RESULT_SUCCESS; }
static SLresult pl_GetPosition(void *s, SLuint32 *ms) {
    SLObj *p = FROM(s, main_itf);
    if (!ms) return SL_RESULT_PARAMETER_INVALID;
    *ms = (SLuint32)(p->frames_played * 1000 / (p->rate ? p->rate : 48000));
    return SL_RESULT_SUCCESS;
}
static SLresult pl_ok2(void *s, void *a, void *b) { (void)s; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult pl_ok1(void *s, SLuint32 a) { (void)s; (void)a; return SL_RESULT_SUCCESS; }
static SLresult pl_okp(void *s, SLuint32 *a) { (void)s; if (a) *a = 0; return SL_RESULT_SUCCESS; }
static SLresult pl_ok0(void *s) { (void)s; return SL_RESULT_SUCCESS; }
static const struct {
    void *f[12];
} g_play_vt = {{
    (void *)pl_SetPlayState, (void *)pl_GetPlayState, (void *)pl_GetDuration,
    (void *)pl_GetPosition, (void *)pl_ok2 /* RegisterCallback */,
    (void *)pl_ok1 /* SetCallbackEventsMask */, (void *)pl_okp,
    (void *)pl_ok1 /* SetMarkerPosition */, (void *)pl_ok0 /* ClearMarker */,
    (void *)pl_okp, (void *)pl_ok1 /* SetPositionUpdatePeriod */, (void *)pl_okp,
}};

/* ---- volume / configuration (accepted, no effect) ---------------------- */
static SLresult v_getmax(void *s, int16_t *v) { (void)s; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult v_getbool(void *s, SLboolean *v) { (void)s; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult v_set(void *s, uintptr_t v) { (void)s; (void)v; return SL_RESULT_SUCCESS; }
static const struct { void *f[9]; } g_vol_vt = {{
    (void *)v_set, (void *)v_getmax, (void *)v_getmax, (void *)v_set, (void *)v_getbool,
    (void *)v_set, (void *)v_getbool, (void *)v_set, (void *)v_getmax,
}};
static SLresult c_set(void *s, const char *k, const void *v, SLuint32 n) {
    (void)s; (void)v; (void)n;
    debugPrintf("[opensl] SetConfiguration %s\n", k ? k : "?");
    return SL_RESULT_SUCCESS;
}
static SLresult c_get(void *s, const char *k, SLuint32 *n, void *v) {
    (void)s; (void)k; (void)n; (void)v; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult c_unsup(void *s) { (void)s; return SL_RESULT_FEATURE_UNSUPPORTED; }
static const struct { void *f[4]; } g_cfg_vt = {{
    (void *)c_set, (void *)c_get, (void *)c_unsup, (void *)c_unsup,
}};

/* ---- engine ------------------------------------------------------------- */
static SLObj *new_obj(int kind) {
    SLObj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->obj_itf = &g_obj_vt;
    o->kind = kind;
    mutexInit(&o->lock);
    return o;
}

static SLresult e_unsup(void) { debugPrintf("[opensl] unsupported engine call\n"); return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult e_CreateOutputMix(void *s, void **out, SLuint32 n,
                                  const SLInterfaceID *ids, const SLboolean *req) {
    (void)s; (void)n; (void)ids; (void)req;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    SLObj *o = new_obj(1);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *out = &o->obj_itf;
    debugPrintf("[opensl] CreateOutputMix\n");
    return SL_RESULT_SUCCESS;
}

typedef struct { void *locator, *format; } SLDataSource;
typedef struct {
    SLuint32 type, channels, rate_mhz, bits, container, mask, endian, repr;
} SlPcmFormat;

static SLresult e_CreateAudioPlayer(void *s, void **out, SLDataSource *src,
                                    SLDataSource *sink, SLuint32 n,
                                    const SLInterfaceID *ids, const SLboolean *req) {
    (void)s; (void)sink;
    if (!out || !src || !src->locator || !src->format) return SL_RESULT_PARAMETER_INVALID;
    const SLuint32 *loc = src->locator;
    const SlPcmFormat *f = src->format;
    if ((loc[0] != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
         loc[0] != SL_DATALOCATOR_BUFFERQUEUE) ||
        (f->type != SL_DATAFORMAT_PCM && f->type != SL_ANDROID_DATAFORMAT_PCM_EX)) {
        debugPrintf("[opensl] CreateAudioPlayer: unsupported locator %#x / format %u\n",
                    loc[0], f->type);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    bool is_float = f->type == SL_ANDROID_DATAFORMAT_PCM_EX &&
                    f->repr == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
    if ((f->channels != 1 && f->channels != 2) ||
        (is_float ? f->bits != 32 : f->bits != 16)) {
        debugPrintf("[opensl] CreateAudioPlayer: unsupported pcm ch=%u bits=%u\n",
                    f->channels, f->bits);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    for (SLuint32 i = 0; i < n; ++i) {
        if (!req || !req[i]) continue;
        SLInterfaceID id = ids[i];
        if (!iid_eq(id, SL_IID_PLAY) && !iid_eq(id, SL_IID_BUFFERQUEUE) &&
            !iid_eq(id, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) && !iid_eq(id, SL_IID_VOLUME) &&
            !iid_eq(id, SL_IID_ANDROIDCONFIGURATION) && !iid_eq(id, SL_IID_OBJECT))
            return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    SLObj *p = new_obj(2);
    if (!p) return SL_RESULT_MEMORY_FAILURE;
    p->main_itf = &g_play_vt;
    p->bq_itf = &g_bq_vt;
    p->vol_itf = &g_vol_vt;
    p->cfg_itf = &g_cfg_vt;
    p->destroy = player_destroy;
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->q_capacity = loc[1] ? (loc[1] < BQ_MAX ? loc[1] : BQ_MAX) : 2;
    p->channels = f->channels;
    p->rate = f->rate_mhz / 1000;
    p->format = is_float ? BS_AAUDIO_FORMAT_FLOAT : BS_AAUDIO_FORMAT_I16;
    p->bytes_per_frame = f->channels * (f->bits / 8);

    AAudioStreamBuilder *b = NULL;
    int32_t rc = AAudio_createStreamBuilder(&b);
    if (rc == 0) {
        AAudioStreamBuilder_setDirection(b, BS_AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)p->rate);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)p->channels);
        AAudioStreamBuilder_setFormat(b, p->format);
        AAudioStreamBuilder_setDataCallback(b, pump, p);
        rc = AAudioStreamBuilder_openStream(b, &p->stream);
        AAudioStreamBuilder_delete(b);
    }
    debugPrintf("[opensl] CreateAudioPlayer %u Hz %u ch %s, %u buffers -> stream rc=%d\n",
                p->rate, p->channels, is_float ? "f32" : "s16", p->q_capacity, rc);
    if (rc != 0) {
        free(p);
        return SL_RESULT_RESOURCE_ERROR;
    }
    *out = &p->obj_itf;
    return SL_RESULT_SUCCESS;
}

static void *g_engine_vt[] = {
    (void *)e_unsup, (void *)e_unsup, (void *)e_CreateAudioPlayer, (void *)e_unsup,
    (void *)e_unsup, (void *)e_unsup, (void *)e_unsup, (void *)e_CreateOutputMix,
    (void *)e_unsup, (void *)e_unsup, (void *)e_unsup, (void *)e_unsup,
    (void *)e_unsup, (void *)e_unsup, (void *)e_unsup,
};

static SLresult o_GetInterface(void *s, SLInterfaceID iid, void *out) {
    SLObj *o = FROM(s, obj_itf);
    void **dst = out;
    if (!dst) return SL_RESULT_PARAMETER_INVALID;
    const void *const *itf = NULL;
    if (iid_eq(iid, SL_IID_OBJECT)) itf = (const void *const *)&o->obj_itf;
    else if (o->kind == 0 && iid_eq(iid, SL_IID_ENGINE)) itf = &o->main_itf;
    else if (o->kind == 2) {
        if (iid_eq(iid, SL_IID_PLAY)) itf = &o->main_itf;
        else if (iid_eq(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) ||
                 iid_eq(iid, SL_IID_BUFFERQUEUE)) itf = &o->bq_itf;
        else if (iid_eq(iid, SL_IID_VOLUME)) itf = &o->vol_itf;
        else if (iid_eq(iid, SL_IID_ANDROIDCONFIGURATION)) itf = &o->cfg_itf;
    }
    debugPrintf("[opensl] GetInterface kind=%d iid=%#x -> %s\n", o->kind, iid ? iid->a : 0, itf ? "ok" : "unsupported");
    if (!itf) { *dst = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
    *dst = (void *)itf;
    return SL_RESULT_SUCCESS;
}

SLresult slCreateEngine(void **out, SLuint32 nopt, const void *opts, SLuint32 n,
                        const SLInterfaceID *ids, const SLboolean *req) {
    (void)nopt; (void)opts; (void)n; (void)ids; (void)req;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    SLObj *o = new_obj(0);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->main_itf = g_engine_vt;
    *out = &o->obj_itf;
    debugPrintf("[opensl] slCreateEngine\n");
    return SL_RESULT_SUCCESS;
}

static SLresult bs_slQueryNumSupportedEngineInterfaces(SLuint32 *n) {
    if (!n) return SL_RESULT_PARAMETER_INVALID;
    *n = 1; return SL_RESULT_SUCCESS;
}

void *bs_opensles_find_export(const char *name) {
    static const struct { const char *n; const void *a; } t[] = {
        {"slCreateEngine", (const void *)slCreateEngine},
        {"slQueryNumSupportedEngineInterfaces", (const void *)bs_slQueryNumSupportedEngineInterfaces},
#define E(n) {"SL_IID_" #n, &SL_IID_##n},
        E(NULL) E(OBJECT) E(ENGINE) E(PLAY) E(BUFFERQUEUE) E(ANDROIDSIMPLEBUFFERQUEUE)
        E(VOLUME) E(ANDROIDCONFIGURATION) E(OUTPUTMIX) E(ENGINECAPABILITIES)
        E(ENVIRONMENTALREVERB) E(PRESETREVERB) E(EFFECTSEND) E(PLAYBACKRATE) E(SEEK)
        E(PREFETCHSTATUS) E(METADATAEXTRACTION) E(RECORD) E(ANDROIDEFFECT)
        E(ANDROIDEFFECTSEND) E(MUTESOLO)
#undef E
    };
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
        if (!strcmp(t[i].n, name)) return (void *)t[i].a;
    return NULL;
}
