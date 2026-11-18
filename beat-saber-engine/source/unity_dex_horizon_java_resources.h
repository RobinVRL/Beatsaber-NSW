/* Original Quest Resource/AssetManager/Configuration native service.
 *
 * This module owns actual native AAsset open/read/seek/close operations and
 * observes Horizon locale and NWindow pixel size. A resource table's CRC is
 * only file identity; no Android Resources#getString/getDrawable/etc is
 * implied without a real resources.arsc parser and runtime configuration.
 *
 * Java objects are produced ONLY by an independently executable host class
 * registry which constructs and strongly retains all three Java instances,
 * verifies their runtime identities, their actual getter links, and the
 * AssetManager's native backing. This module never manufactures jobjects,
 * calls Activity.attach or publishes UnityPlayer.currentActivity.
 */
#ifndef BS_UNITY_DEX_HORIZON_JAVA_RESOURCES_H
#define BS_UNITY_DEX_HORIZON_JAVA_RESOURCES_H

#include "unity_dex_horizon_android_attachment.h"
#include "unity_dex_quest_arsc_resolver.h"
#include <stddef.h>
#include <stdint.h>

#define BS_HJR_MAX_OPEN_ASSETS 16u
#define BS_HJR_MAX_ASSET_LENGTH UINT64_C(134217728)

typedef struct BsDexHorizonJavaResources BsDexHorizonJavaResources;

typedef struct BsDexHorizonJavaResourcesNative {
    void *user;
    /* Must return a fresh independently validated NDK AAssetManager owner. */
    int (*observed_asset_manager)(void *user, void **manager);
    /* AAsset semantics. Open must return an owned stream; failure must
     * return NULL. Read returns -1 for I/O error, 0 on clean EOF. */
    void *(*open_asset)(void *user, void *manager, const char *name);
    int (*read_asset)(void *user, void *asset, void *buffer, size_t length);
    int64_t (*seek_asset)(void *user, void *asset, int64_t off, int whence);
    int64_t (*length_asset)(void *user, void *asset);
    void (*close_asset)(void *user, void *asset);
} BsDexHorizonJavaResourcesNative;

typedef struct BsDexHorizonObservedConfiguration {
    uint32_t width_px;            /* actual NWindow framebuffer pixels */
    uint32_t height_px;
    char language_tag[9];        /* observed Horizon Settings language */
    uint64_t observation_number; /* monotonically increasing Context read */
} BsDexHorizonObservedConfiguration;

typedef struct BsDexHorizonAssetStream {
    BsDexHorizonJavaResources *owner;
    uint64_t generation;
    uint64_t serial;
    uint32_t slot;
} BsDexHorizonAssetStream;

typedef struct BsDexHorizonJavaResourceObjects {
    void *resources;      /* independently backed android.content.res.Resources */
    void *asset_manager;  /* independently backed android.content.res.AssetManager */
    void *configuration;  /* independently backed android.content.res.Configuration */
    void *strong_owner;   /* holds all three Java objects until destroy */
} BsDexHorizonJavaResourceObjects;

/* A registry is executable Java framework code, not a DEX type-label table.
 * build must perform the real constructors/registry initialization with the
 * original APK assets and an actual configuration. It owns its token even
 * on a partial unsuccessful construction, allowing rollback.
 *
 * verify_link must independently read Resources.getAssets(),
 * Resources.getConfiguration(), validate NDK AAssetManager_fromJava and
 * compare every supplied Java Configuration field with observed sources.
 * Missing Android density, dp, orientation, font scale and input fields must
 * remain UNDEFINED; never derive them from NWindow framebuffer dimensions.
 * Destroy must release every retained Java class/object, even when a later
 * external native asset/context observation has gone stale.
 */
typedef struct BsDexHorizonJavaResourceRegistry {
    void *user;
    int (*build)(void *user, const BsDexQuestPackageHost *package,
                 const BsDexHorizonObservedConfiguration *observed,
                 void *ndk_asset_manager,
                 BsDexHorizonJavaResourceObjects *objects);
    int (*verify_class)(void *user, void *object,
                        const char *dex_descriptor);
    int (*verify_link)(void *user,
                       const BsDexHorizonJavaResourceObjects *objects,
                       void *native_asset_manager,
                       const BsDexHorizonObservedConfiguration *observed);
    void (*destroy)(void *user, BsDexHorizonJavaResourceObjects *objects);
} BsDexHorizonJavaResourceRegistry;

typedef struct BsDexHorizonAssetSlot {
    void *native_asset;
    uint64_t length;
    uint64_t serial;
    int active;
} BsDexHorizonAssetSlot;

struct BsDexHorizonJavaResources {
    BsDexHorizonAndroidAttachment *attachment; /* borrowed native graph */
    BsDexHorizonContextObjects *context;       /* borrowed native owner */
    const BsDexQuestPackageHost *package;      /* borrowed original APK */
    BsDexHorizonJavaResourcesNative native;
    BsDexHorizonJavaResourceRegistry java;
    BsDexHorizonJavaResourceObjects java_objects;
    const BsQuestArscResolver *native_arsc; /* borrowed authenticated table */
    char last_native_resource_string[BS_QUEST_ARSC_MAX_STRING_BYTES];
    BsDexHorizonAssetSlot assets[BS_HJR_MAX_OPEN_ASSETS];
    uint64_t generation;
    uint64_t next_asset_serial;
    void *native_manager;
    int initialized;
    int java_verified;
};

/* This only initializes a native data/service owner after the original
 * Quest package, native Context and NWindow/AssetManager have been verified.
 * All providers and borrowed owners must outlive the resource service. */
int bs_dex_horizon_java_resources_init(
    BsDexHorizonJavaResources *service,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonContextObjects *context,
    const BsDexQuestPackageHost *package,
    const BsDexHorizonJavaResourcesNative *native);

int bs_dex_horizon_java_resources_native_ready(
    const BsDexHorizonJavaResources *service);
int bs_dex_horizon_java_resources_configuration(
    BsDexHorizonJavaResources *service,
    BsDexHorizonObservedConfiguration *observed);
int bs_dex_horizon_java_resources_asset_manager(
    const BsDexHorizonJavaResources *service, void **native_manager);

/* Actual native stream on original staged asset. Names are relative to
 * APK assets/; rejects path traversal, absolute paths and \\ separators.
 * The stream token contains a slot + serial, preventing stale reuse. */
int bs_dex_horizon_java_resources_open(
    BsDexHorizonJavaResources *service, const char *relative_asset,
    BsDexHorizonAssetStream *stream, uint64_t *asset_length);
int bs_dex_horizon_java_resources_read(
    BsDexHorizonAssetStream stream, void *buffer, size_t capacity,
    size_t *bytes_read);
int bs_dex_horizon_java_resources_seek(
    BsDexHorizonAssetStream stream, int64_t offset, int whence,
    uint64_t *position);
int bs_dex_horizon_java_resources_close(BsDexHorizonAssetStream stream);

/* Only when independent registry executable services are present. A failed
 * validation calls destroy and cannot expose partially built Java handles.
 * Native file stream operations remain available even when Java is absent. */
int bs_dex_horizon_java_resources_bind_java(
    BsDexHorizonJavaResources *service,
    const BsDexHorizonJavaResourceRegistry *registry);
int bs_dex_horizon_java_resources_java_get_assets(
    const BsDexHorizonJavaResources *service, void **jobject);
int bs_dex_horizon_java_resources_java_get_configuration(
    const BsDexHorizonJavaResources *service, void **jobject);
int bs_dex_horizon_java_resources_java_get_resources(
    const BsDexHorizonJavaResources *service, void **jobject);
/* Bind an authenticated, live original APK table. This enables NATIVE typed
 * resource lookup only, never Java Resources/getString method dispatch.
 * Caller must release this service BEFORE releasing the borrowed ARSC. */
int bs_dex_horizon_java_resources_bind_native_arsc(
    BsDexHorizonJavaResources *service,
    const BsQuestArscResolver *authenticated_table);
/* Resolve only unambiguous original typed values against genuinely observed
 * language; unknown density/orientation/sdk qualifiers remain unavailable. */
int bs_dex_horizon_java_resources_get_boolean(
    const BsDexHorizonJavaResources *service, uint32_t resource_id,
    int *value);
int bs_dex_horizon_java_resources_get_string(
    BsDexHorizonJavaResources *service, uint32_t resource_id,
    const char **value);
int bs_dex_horizon_java_resources_get_integer(
    const BsDexHorizonJavaResources *service, uint32_t resource_id,
    int32_t *value);

/* Close native streams and release Java registry refs while owners live;
 * close is best effort even after the native owners become unavailable. */
int bs_dex_horizon_java_resources_shutdown(
    BsDexHorizonJavaResources *service);

/* Production NDK callbacks backed by existing asset_fs_bridge operations.
 * Original files remain owned by the staged Quest asset manager. */
BsDexHorizonJavaResourcesNative
bs_dex_horizon_java_resources_production_native(
    BsDexHorizonAndroidAttachment *attachment);

#endif
