/* Offline Oculus Platform SDK for Switch.
 *
 * libovrplatformloader.so is a thin shim: ovr_UnityInitWrapperAsynchronous
 * looks for the Platform SDK driver inside the Horizon / platformsdkruntime
 * APKs and aborts through NativeJava on any JNI failure. Neither APK exists on
 * Switch, so the guest's ovr_* P/Invokes are answered here instead:
 *
 *  - platform initialisation succeeds (PlatformInitializeResult.Success) and
 *    delivers the Platform_InitializeAndroidAsynchronous message;
 *  - ovr_Entitlement_GetIsViewerEntitled answers "entitled" (the owner's own
 *    purchased copy);
 *  - every other ovr_* export resolves to a logging stub returning 0, i.e. a
 *    null handle / zero request id / false. No network service is emulated.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void debugPrintf(const char *text, ...);

/* Oculus.Platform.Message.MessageType values (confirmed as switch constants
 * in libil2cpp's Message.ParseMessageHandle). */
#define OVR_MSG_PLATFORM_INIT_ANDROID_ASYNC 0x1AD307B4u
#define OVR_MSG_ENTITLEMENT_IS_VIEWER_ENTITLED 0x186B58B1u

typedef struct OvrMessage {
    uint32_t type;
    uint64_t request;
    struct OvrMessage *next;
} OvrMessage;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static OvrMessage *g_head, *g_tail;
static uint64_t g_next_request = 1;
static int g_initialized;

static uint64_t ovr_post(uint32_t type) {
    OvrMessage *m = calloc(1, sizeof *m);
    if (!m) return 0;
    pthread_mutex_lock(&g_lock);
    m->type = type;
    m->request = g_next_request++;
    if (g_tail) g_tail->next = m; else g_head = m;
    g_tail = m;
    const uint64_t id = m->request;
    pthread_mutex_unlock(&g_lock);
    return id;
}

static uint64_t ovr_init_async(const char *app_id) {
    debugPrintf("[ovr:offline] platform init (app %s) -> success\n",
                app_id ? app_id : "?");
    g_initialized = 1;
    return ovr_post(OVR_MSG_PLATFORM_INIT_ANDROID_ASYNC);
}
static uint64_t ovr_UnityInitWrapperAsynchronous(const char *app_id) {
    return ovr_init_async(app_id);
}
static uint64_t ovr_PlatformInitializeAndroidAsynchronous(const char *app_id,
                                                          void *activity,
                                                          void *env) {
    (void)activity; (void)env;
    return ovr_init_async(app_id);
}
static int ovr_UnityInitWrapper(const char *app_id) {
    debugPrintf("[ovr:offline] sync platform init (app %s)\n", app_id ? app_id : "?");
    g_initialized = 1;
    return 0; /* ovrPlatformInitialize_Success */
}
static int ovr_IsPlatformInitialized(void) { return g_initialized; }

static uint64_t ovr_Entitlement_GetIsViewerEntitled(void) {
    debugPrintf("[ovr:offline] entitlement check -> entitled\n");
    return ovr_post(OVR_MSG_ENTITLEMENT_IS_VIEWER_ENTITLED);
}

/* A single local offline user. */
#define OVR_MSG_USER_GET_LOGGED_IN_USER 0x436F345Du
#define OVR_OFFLINE_USER_ID UINT64_C(0x5357495443480001)
static const char g_user_name[] = "Switch Player";
static uint64_t ovr_User_GetLoggedInUser(void) {
    return ovr_post(OVR_MSG_USER_GET_LOGGED_IN_USER);
}
static uint64_t ovr_GetLoggedInUserID(void) { return OVR_OFFLINE_USER_ID; }
static const char *ovr_GetLoggedInUserLocale(void) { return "en_US"; }
static const void *ovr_Message_GetUser(const void *m) { return m; }
static uint64_t ovr_User_GetID(const void *u) { (void)u; return OVR_OFFLINE_USER_ID; }
static const char *ovr_User_GetOculusID(const void *u) { (void)u; return g_user_name; }
static const char *ovr_User_GetDisplayName(const void *u) { (void)u; return g_user_name; }
static const char *ovr_User_GetImageUrl(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetSmallImageUrl(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetPresence(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetInviteToken(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetPresenceDeeplinkMessage(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetPresenceDestinationApiName(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetPresenceLobbySessionId(const void *u) { (void)u; return ""; }
static const char *ovr_User_GetPresenceMatchSessionId(const void *u) { (void)u; return ""; }

static OvrMessage *ovr_PopMessage(void) {
    pthread_mutex_lock(&g_lock);
    OvrMessage *m = g_head;
    if (m) {
        g_head = m->next;
        if (!g_head) g_tail = NULL;
        m->next = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    if (m)
        debugPrintf("[ovr:offline] PopMessage type=%#x request=%llu\n", m->type,
                    (unsigned long long)m->request);
    return m;
}
static void ovr_FreeMessage(OvrMessage *m) { free(m); }
static uint32_t ovr_Message_GetType(const OvrMessage *m) { return m ? m->type : 0; }
static uint64_t ovr_Message_GetRequestID(const OvrMessage *m) { return m ? m->request : 0; }
static int ovr_Message_IsError(const OvrMessage *m) { (void)m; return 0; }
static const void *ovr_Message_GetPlatformInitialize(const OvrMessage *m) { return m; }
static int ovr_PlatformInitialize_GetResult(const void *h) { (void)h; return 0; }
static const void *ovr_Message_GetNativeMessage(const OvrMessage *m) { return m; }

/* Requests the boot flow waits on (health warning, remote assets, DLC).
 * Message type constants are the MessageType switch immediates in
 * libil2cpp's Message.ParseMessageHandle. */
#define OVR_MSG_USER_GET_ACCESS_TOKEN 0x06A85ABEu
#define OVR_MSG_USER_AGE_CATEGORY_GET 0x21CBE0C0u
#define OVR_MSG_ASSET_FILE_GET_LIST 0x4AFC6F74u
#define OVR_MSG_IAP_GET_VIEWER_PURCHASES 0x3A0F8419u
static uint64_t ovr_User_GetAccessToken(void) {
    return ovr_post(OVR_MSG_USER_GET_ACCESS_TOKEN);
}
static uint64_t ovr_UserAgeCategory_Get(void) {
    return ovr_post(OVR_MSG_USER_AGE_CATEGORY_GET);
}
static uint64_t ovr_AssetFile_GetList(void) {
    return ovr_post(OVR_MSG_ASSET_FILE_GET_LIST);
}
static uint64_t ovr_IAP_GetViewerPurchases(void) {
    return ovr_post(OVR_MSG_IAP_GET_VIEWER_PURCHASES);
}
/* No Meta account exists offline; an opaque token keeps the client from
 * waiting for one (its network requests then fail fast, offline). */
static const char *ovr_Message_GetString(const OvrMessage *m) {
    (void)m;
    return "OFFLINE|switch";
}
static const void *ovr_Message_GetUserAccountAgeCategory(const OvrMessage *m) { return m; }
static int ovr_UserAccountAgeCategory_GetAgeCategory(const void *h) {
    (void)h;
    return 3; /* AccountAgeCategory.Ad */
}
static const void *ovr_Message_GetAssetDetailsArray(const OvrMessage *m) { return m; }
static size_t ovr_AssetDetailsArray_GetSize(const void *h) { (void)h; return 0; }
static const void *ovr_Message_GetPurchaseArray(const OvrMessage *m) { return m; }
static size_t ovr_PurchaseArray_GetSize(const void *h) { (void)h; return 0; }
static int ovr_PurchaseArray_HasNextPage(const void *h) { (void)h; return 0; }
static const char *ovr_PurchaseArray_GetNextUrl(const void *h) { (void)h; return ""; }

/* Multiplayer (official or BeatTogether via MultiplayerCore) builds its
 * master-server AuthenticationToken from Users.GetUserProof(). A zero request
 * id made that request fail, so the server browser never got a token and spun
 * forever. Answer with a per-launch nonce; custom servers accept it. */
#define OVR_MSG_USER_GET_USER_PROOF 0x22810483u
static char g_user_proof[33];
static uint64_t ovr_User_GetUserProof(void) {
    if (!g_user_proof[0]) {
        uint64_t a = ((uint64_t)rand() << 32) ^ (uint64_t)rand() ^
                     (uint64_t)(uintptr_t)&a;
        uint64_t b = ((uint64_t)rand() << 32) ^ (uint64_t)rand() ^ OVR_OFFLINE_USER_ID;
        snprintf(g_user_proof, sizeof g_user_proof, "%016llx%016llx",
                 (unsigned long long)a, (unsigned long long)b);
    }
    debugPrintf("[ovr:offline] user proof requested\n");
    return ovr_post(OVR_MSG_USER_GET_USER_PROOF);
}
static const void *ovr_Message_GetUserProof(const OvrMessage *m) { return m; }
static const char *ovr_UserProof_GetNonce(const void *h) {
    (void)h;
    return g_user_proof;
}
static const struct { const char *name; void *fn; } g_impl[] = {
#define E(n) {#n, (void *)n}
    E(ovr_UnityInitWrapperAsynchronous),
    E(ovr_PlatformInitializeAndroidAsynchronous),
    E(ovr_UnityInitWrapper),
    E(ovr_IsPlatformInitialized),
    E(ovr_Entitlement_GetIsViewerEntitled),
    E(ovr_PopMessage),
    E(ovr_FreeMessage),
    E(ovr_Message_GetType),
    E(ovr_Message_GetRequestID),
    E(ovr_Message_IsError),
    E(ovr_Message_GetPlatformInitialize),
    E(ovr_PlatformInitialize_GetResult),
    E(ovr_User_GetLoggedInUser),
    E(ovr_GetLoggedInUserID),
    E(ovr_GetLoggedInUserLocale),
    E(ovr_Message_GetUser),
    E(ovr_User_GetID),
    E(ovr_User_GetOculusID),
    E(ovr_User_GetDisplayName),
    E(ovr_User_GetImageUrl),
    E(ovr_User_GetSmallImageUrl),
    E(ovr_User_GetPresence),
    E(ovr_User_GetInviteToken),
    E(ovr_User_GetPresenceDeeplinkMessage),
    E(ovr_User_GetPresenceDestinationApiName),
    E(ovr_User_GetPresenceLobbySessionId),
    E(ovr_User_GetPresenceMatchSessionId),
    E(ovr_Message_GetNativeMessage),
    E(ovr_User_GetAccessToken),
    E(ovr_UserAgeCategory_Get),
    E(ovr_AssetFile_GetList),
    E(ovr_IAP_GetViewerPurchases),
    E(ovr_Message_GetString),
    E(ovr_Message_GetUserAccountAgeCategory),
    E(ovr_UserAccountAgeCategory_GetAgeCategory),
    E(ovr_Message_GetAssetDetailsArray),
    E(ovr_AssetDetailsArray_GetSize),
    E(ovr_Message_GetPurchaseArray),
    E(ovr_PurchaseArray_GetSize),
    E(ovr_PurchaseArray_HasNextPage),
    E(ovr_PurchaseArray_GetNextUrl),
    E(ovr_User_GetUserProof),
    E(ovr_Message_GetUserProof),
    E(ovr_UserProof_GetNonce),
#undef E
};

/* ---- Logging zero stubs, one per distinct unmodelled export ---------- */
#define OVR_STUBS 512
static const char *g_stub_names[OVR_STUBS];
static unsigned char g_stub_logged[OVR_STUBS];
static int g_stub_count;

static short g_suffix_to_slot[900];

static uint64_t ovr_stub_hit(int suffix) {
    const int i = g_suffix_to_slot[suffix];
    if (i < g_stub_count && !g_stub_logged[i]) {
        g_stub_logged[i] = 1;
        debugPrintf("[ovr:offline] stub %s -> 0\n", g_stub_names[i]);
    }
    return 0;
}

#define S(n) static uint64_t ovr_stub_##n(void) { return ovr_stub_hit(n); }
#define S8(n) S(n##0) S(n##1) S(n##2) S(n##3) S(n##4) S(n##5) S(n##6) S(n##7)
#define S64(n) S8(n##0) S8(n##1) S8(n##2) S8(n##3) S8(n##4) S8(n##5) S8(n##6) S8(n##7)
/* Octal-free indices: prefix digits 1..8 so no literal starts with 0. */
S64(1) S64(2) S64(3) S64(4) S64(5) S64(6) S64(7) S64(8)
#undef S
#define S(n) (void *)ovr_stub_##n,
static void *const g_stub_fns[OVR_STUBS] = {
    S64(1) S64(2) S64(3) S64(4) S64(5) S64(6) S64(7) S64(8)
};
#undef S
#undef S8
#undef S64

/* Stub functions pass their numeric suffix (100..877, octal-digit pattern),
 * array slot k holds the k-th generated function; map suffix -> slot. */
static int g_suffix_slot_ready;

static void ovr_build_suffix_table(void) {
    int k = 0;
    for (int a = 1; a <= 8; ++a)
        for (int b = 0; b < 8; ++b)
            for (int c = 0; c < 8; ++c)
                g_suffix_to_slot[a * 100 + b * 10 + c] = (short)k++;
    g_suffix_slot_ready = 1;
}

void *bs_ovr_platform_symbol(const char *name) {
    if (!name || strncmp(name, "ovr_", 4)) return NULL;
    for (size_t i = 0; i < sizeof g_impl / sizeof g_impl[0]; ++i)
        if (!strcmp(g_impl[i].name, name)) return g_impl[i].fn;
    pthread_mutex_lock(&g_lock);
    if (!g_suffix_slot_ready) ovr_build_suffix_table();
    void *fn = NULL;
    for (int i = 0; i < g_stub_count; ++i)
        if (!strcmp(g_stub_names[i], name)) { fn = g_stub_fns[i]; break; }
    if (!fn && g_stub_count < OVR_STUBS) {
        g_stub_names[g_stub_count] = strdup(name);
        fn = g_stub_fns[g_stub_count++];
    }
    pthread_mutex_unlock(&g_lock);
    return fn;
}
