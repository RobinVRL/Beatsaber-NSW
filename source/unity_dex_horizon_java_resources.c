#include "unity_dex_horizon_java_resources.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#if defined(__SWITCH__)
#include "asset_fs_bridge.h"
#endif

static int native_api_complete(const BsDexHorizonJavaResourcesNative *api) {
    return api && api->observed_asset_manager && api->open_asset &&
        api->read_asset && api->seek_asset && api->length_asset &&
        api->close_asset;
}

static int package_identity(const BsDexHorizonJavaResources *s) {
    if (!s || !s->initialized || !s->attachment || !s->context ||
        !s->package || !s->package->initialized ||
        !s->package->native_paths_observed ||
        s->attachment->package != s->package ||
        s->attachment->context != s->context ||
        s->context->package != s->package) return 0;
    BsDexHorizonAttachmentRef ref = {0};
    const BsDexQuestApplicationInfo *application = NULL;
    return bs_dex_horizon_android_attachment_ref(
               s->attachment, BS_HA_APPLICATION_INFO, &ref) == 1 &&
        bs_dex_horizon_android_attachment_application(
            ref, &application) == 1 &&
        application && application->package_name &&
        !strcmp(application->package_name,
                s->package->manifest.package_name);
}

static int native_ready(const BsDexHorizonJavaResources *s) {
    if (!package_identity(s) || !native_api_complete(&s->native)) return 0;
    void *observed = NULL;
    return s->native.observed_asset_manager(s->native.user, &observed) == 1 &&
        observed && observed == s->native_manager;
}

int bs_dex_horizon_java_resources_native_ready(
    const BsDexHorizonJavaResources *service) {
    return native_ready(service);
}

int bs_dex_horizon_java_resources_init(
    BsDexHorizonJavaResources *s,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonContextObjects *context,
    const BsDexQuestPackageHost *package,
    const BsDexHorizonJavaResourcesNative *native) {
    if (!s || s->initialized || s->java_verified ||
        !attachment || !context || !package ||
        !native_api_complete(native) || s->generation == UINT64_MAX)
        return 0;
    BsDexHorizonAttachmentRef ref = {0};
    void *bound_manager = NULL, *manager = NULL;
    if (!package->initialized || !package->native_paths_observed ||
        attachment->package != package || attachment->context != context ||
        context->package != package ||
        !bs_dex_horizon_android_attachment_ref(
            attachment, BS_HA_NATIVE_ASSET_MANAGER, &ref) ||
        !bs_dex_horizon_android_attachment_native_handle(
            ref, &bound_manager) ||
        !bound_manager ||
        native->observed_asset_manager(native->user, &manager) != 1 ||
        manager != bound_manager) return 0;
    uint64_t generation = s->generation + 1;
    *s = (BsDexHorizonJavaResources){0};
    s->attachment = attachment;
    s->context = context;
    s->package = package;
    s->native = *native;
    s->native_manager = manager;
    s->generation = generation;
    s->initialized = 1;
    if (!native_ready(s)) {
        *s = (BsDexHorizonJavaResources){0};
        s->generation = generation;
        return 0;
    }
    return 1;
}

int bs_dex_horizon_java_resources_configuration(
    BsDexHorizonJavaResources *s,
    BsDexHorizonObservedConfiguration *observed) {
    if (observed) *observed = (BsDexHorizonObservedConfiguration){0};
    if (!observed || !native_ready(s)) return 0;
    BsDexHorizonAttachmentRef ref = {0};
    BsDexHorizonContextConfig config = {0};
    if (!bs_dex_horizon_android_attachment_ref(
            s->attachment, BS_HA_CONFIGURATION, &ref) ||
        !bs_dex_horizon_android_attachment_configuration(ref, &config) ||
        !config.window_width_px || !config.window_height_px ||
        !config.horizon_language_tag[0])
        return 0;
    observed->width_px = config.window_width_px;
    observed->height_px = config.window_height_px;
    memcpy(observed->language_tag, config.horizon_language_tag,
           sizeof observed->language_tag);
    observed->observation_number = config.observation_number;
    return observed->width_px && observed->height_px &&
        observed->language_tag[0];
}

int bs_dex_horizon_java_resources_asset_manager(
    const BsDexHorizonJavaResources *s, void **manager) {
    if (manager) *manager = NULL;
    if (!manager || !native_ready(s)) return 0;
    *manager = s->native_manager;
    return 1;
}

static int valid_asset_path(const char *name) {
    if (!name || !*name) return 0;
    size_t total = 0;
    const char *start = name;
    for (const char *p = name;; ++p) {
        char c = *p;
        if (++total > 512) return 0;
        if (c == '/' || !c) {
            size_t n = (size_t)(p - start);
            if (!n || (n == 1 && start[0] == '.') ||
                (n == 2 && start[0] == '.' && start[1] == '.'))
                return 0;
            if (!c) break;
            start = p + 1;
        } else if (c == '\\' || c == ':' ||
                   (unsigned char)c < 32u || c == 127)
            return 0;
    }
    return 1;
}

int bs_dex_horizon_java_resources_open(
    BsDexHorizonJavaResources *s, const char *name,
    BsDexHorizonAssetStream *out, uint64_t *length) {
    if (out) *out = (BsDexHorizonAssetStream){0};
    if (length) *length = 0;
    if (!out || !length || !native_ready(s) ||
        !valid_asset_path(name) || s->next_asset_serial == UINT64_MAX)
        return 0;
    size_t slot = BS_HJR_MAX_OPEN_ASSETS;
    for (size_t i = 0; i < BS_HJR_MAX_OPEN_ASSETS; ++i) {
        if (!s->assets[i].active) { slot = i; break; }
    }
    if (slot == BS_HJR_MAX_OPEN_ASSETS) return 0;
    void *stream = s->native.open_asset(
        s->native.user, s->native_manager, name);
    if (!stream) return 0;
    int64_t stream_size = s->native.length_asset(s->native.user, stream);
    if (stream_size < 0 ||
        (uint64_t)stream_size > BS_HJR_MAX_ASSET_LENGTH) {
        s->native.close_asset(s->native.user, stream);
        return 0;
    }
    uint64_t serial = ++s->next_asset_serial;
    s->assets[slot] = (BsDexHorizonAssetSlot){
        .native_asset = stream,
        .length = (uint64_t)stream_size,
        .serial = serial, .active = 1
    };
    *out = (BsDexHorizonAssetStream){
        .owner = s, .generation = s->generation,
        .serial = serial, .slot = (uint32_t)slot
    };
    *length = (uint64_t)stream_size;
    return 1;
}

static BsDexHorizonAssetSlot *stream_slot(BsDexHorizonAssetStream handle) {
    BsDexHorizonJavaResources *s = handle.owner;
    if (!s || !s->initialized || !handle.generation ||
        handle.generation != s->generation ||
        handle.slot >= BS_HJR_MAX_OPEN_ASSETS || !handle.serial)
        return NULL;
    BsDexHorizonAssetSlot *slot = &s->assets[handle.slot];
    return slot->active && slot->native_asset &&
        slot->serial == handle.serial ? slot : NULL;
}

int bs_dex_horizon_java_resources_read(
    BsDexHorizonAssetStream handle, void *buffer, size_t capacity,
    size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    BsDexHorizonAssetSlot *slot = stream_slot(handle);
    if (!bytes_read || (capacity && !buffer) ||
        capacity > INT32_MAX || !slot || !native_ready(handle.owner))
        return 0;
    int read_count = handle.owner->native.read_asset(
        handle.owner->native.user, slot->native_asset, buffer, capacity);
    if (read_count < 0 || (size_t)read_count > capacity) return 0;
    *bytes_read = (size_t)read_count;
    return 1;
}

int bs_dex_horizon_java_resources_seek(
    BsDexHorizonAssetStream handle, int64_t offset, int whence,
    uint64_t *position) {
    if (position) *position = 0;
    BsDexHorizonAssetSlot *slot = stream_slot(handle);
    if (!position || !slot || !native_ready(handle.owner) ||
        (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END))
        return 0;
    int64_t next = handle.owner->native.seek_asset(
        handle.owner->native.user, slot->native_asset, offset, whence);
    if (next < 0 || (uint64_t)next > slot->length) return 0;
    *position = (uint64_t)next;
    return 1;
}

int bs_dex_horizon_java_resources_close(BsDexHorizonAssetStream handle) {
    BsDexHorizonAssetSlot *slot = stream_slot(handle);
    if (!slot) return 0;
    /* Close even if native readiness was revoked after opening; the bridge
     * created this FILE/asset and its owning callback must free it. */
    handle.owner->native.close_asset(
        handle.owner->native.user, slot->native_asset);
    *slot = (BsDexHorizonAssetSlot){0};
    return 1;
}

static int registry_complete(const BsDexHorizonJavaResourceRegistry *api) {
    return api && api->build && api->verify_class &&
        api->verify_link && api->destroy;
}

static int verify_java(
    BsDexHorizonJavaResources *s,
    const BsDexHorizonJavaResourceRegistry *registry,
    const BsDexHorizonJavaResourceObjects *objects,
    const BsDexHorizonObservedConfiguration *observed) {
    return s && registry_complete(registry) && objects &&
        objects->strong_owner && objects->resources &&
        objects->asset_manager && objects->configuration &&
        registry->verify_class(registry->user, objects->resources,
            "Landroid/content/res/Resources;") == 1 &&
        registry->verify_class(registry->user, objects->asset_manager,
            "Landroid/content/res/AssetManager;") == 1 &&
        registry->verify_class(registry->user, objects->configuration,
            "Landroid/content/res/Configuration;") == 1 &&
        registry->verify_link(registry->user, objects,
                               s->native_manager, observed) == 1;
}

int bs_dex_horizon_java_resources_bind_java(
    BsDexHorizonJavaResources *s,
    const BsDexHorizonJavaResourceRegistry *registry) {
    if (!native_ready(s) || s->java_verified ||
        !registry_complete(registry)) return 0;
    BsDexHorizonObservedConfiguration observed = {0};
    if (!bs_dex_horizon_java_resources_configuration(s, &observed))
        return 0;
    BsDexHorizonJavaResourceObjects objects = {0};
    int created = registry->build(registry->user, s->package, &observed,
                                  s->native_manager, &objects);
    if (created != 1 ||
        !verify_java(s, registry, &objects, &observed) ||
        !native_ready(s)) {
        if (objects.strong_owner || objects.resources ||
            objects.asset_manager || objects.configuration)
            registry->destroy(registry->user, &objects);
        return 0;
    }
    s->java = *registry;
    s->java_objects = objects;
    s->java_verified = 1;
    return 1;
}

static int java_ready(BsDexHorizonJavaResources *s) {
    BsDexHorizonObservedConfiguration observed = {0};
    return native_ready(s) && s->java_verified &&
        registry_complete(&s->java) &&
        bs_dex_horizon_java_resources_configuration(s, &observed) == 1 &&
        verify_java(s, &s->java, &s->java_objects, &observed);
}

int bs_dex_horizon_java_resources_java_get_assets(
    const BsDexHorizonJavaResources *s, void **object) {
    if (object) *object = NULL;
    if (!object || !java_ready((BsDexHorizonJavaResources *)s)) return 0;
    *object = s->java_objects.asset_manager;
    return 1;
}

int bs_dex_horizon_java_resources_java_get_configuration(
    const BsDexHorizonJavaResources *s, void **object) {
    if (object) *object = NULL;
    if (!object || !java_ready((BsDexHorizonJavaResources *)s)) return 0;
    *object = s->java_objects.configuration;
    return 1;
}

int bs_dex_horizon_java_resources_java_get_resources(
    const BsDexHorizonJavaResources *s, void **object) {
    if (object) *object = NULL;
    if (!object || !java_ready((BsDexHorizonJavaResources *)s)) return 0;
    *object = s->java_objects.resources;
    return 1;
}

int bs_dex_horizon_java_resources_bind_native_arsc(
    BsDexHorizonJavaResources *s,
    const BsQuestArscResolver *table) {
    if (!native_ready(s) || !table || !table->bytes ||
        table->size != BS_QUEST_RESOURCES_ARSC_SIZE ||
        table->crc32 != BS_QUEST_RESOURCES_ARSC_CRC ||
        table->package_id != 0x7fu || s->native_arsc) return 0;
    s->native_arsc = table;
    return 1;
}

static int observed_arsc_configuration(
    const BsDexHorizonJavaResources *s, BsQuestArscConfiguration *out) {
    if (out) *out = (BsQuestArscConfiguration){0};
    if (!out || !native_ready(s) || !s->native_arsc ||
        !s->native_arsc->bytes) return 0;
    BsDexHorizonObservedConfiguration native = {0};
    if (!bs_dex_horizon_java_resources_configuration(
            (BsDexHorizonJavaResources *)s, &native)) return 0;
    /* NWindow pixels are deliberately NOT treated as density, dp or
     * orientation. Horizon Settings language is the only observed qualifier. */
    return bs_quest_arsc_set_observed_language(out, native.language_tag);
}

int bs_dex_horizon_java_resources_get_boolean(
    const BsDexHorizonJavaResources *s, uint32_t resource_id, int *value) {
    if (value) *value = 0;
    BsQuestArscConfiguration observed = {0};
    return value && observed_arsc_configuration(s, &observed) &&
        bs_quest_arsc_get_boolean(s->native_arsc, resource_id,
                                   &observed, value);
}

int bs_dex_horizon_java_resources_get_string(
    BsDexHorizonJavaResources *s, uint32_t resource_id,
    const char **value) {
    if (value) *value = NULL;
    BsQuestArscConfiguration observed = {0};
    if (!s || !value) return 0;
    s->last_native_resource_string[0] = 0;
    if (!observed_arsc_configuration(s, &observed) ||
        !bs_quest_arsc_get_string(s->native_arsc, resource_id,
             &observed, s->last_native_resource_string,
             sizeof s->last_native_resource_string)) return 0;
    /* Borrowed until next lookup or service shutdown, not a Java String. */
    *value = s->last_native_resource_string;
    return 1;
}

int bs_dex_horizon_java_resources_get_integer(
    const BsDexHorizonJavaResources *s, uint32_t resource_id,
    int32_t *value) {
    if (value) *value = 0;
    BsQuestArscConfiguration observed = {0};
    return value && observed_arsc_configuration(s, &observed) &&
        bs_quest_arsc_get_integer(s->native_arsc, resource_id,
                                   &observed, value);
}

int bs_dex_horizon_java_resources_shutdown(
    BsDexHorizonJavaResources *s) {
    if (!s || !s->initialized) return 0;
    for (size_t i = 0; i < BS_HJR_MAX_OPEN_ASSETS; ++i)
        if (s->assets[i].active) {
            s->native.close_asset(s->native.user,
                                  s->assets[i].native_asset);
            s->assets[i] = (BsDexHorizonAssetSlot){0};
        }
    if (s->java_verified) {
        s->java.destroy(s->java.user, &s->java_objects);
        s->java_verified = 0;
    }
    uint64_t generation = s->generation;
    *s = (BsDexHorizonJavaResources){0};
    s->generation = generation;
    return 1;
}

#if defined(__SWITCH__)
static int native_manager(void *user, void **manager) {
    if (manager) *manager = NULL;
    BsDexHorizonAndroidAttachment *a = user;
    BsDexHorizonAttachmentRef ref = {0};
    return manager && a &&
        bs_dex_horizon_android_attachment_ref(
            a, BS_HA_NATIVE_ASSET_MANAGER, &ref) == 1 &&
        bs_dex_horizon_android_attachment_native_handle(ref, manager) == 1 &&
        *manager;
}
static void *native_open(void *user, void *manager, const char *name) {
    void *current = NULL;
    if (!native_manager(user, &current) || current != manager) return NULL;
    return bs_android_assetmanager_open(manager, name, 0);
}
static int native_read(void *user, void *asset,
                       void *buffer, size_t n) {
    (void)user;
    return bs_android_asset_read(asset, buffer, n);
}
static int64_t native_seek(void *user, void *asset,
                           int64_t offset, int whence) {
    (void)user;
    return bs_android_asset_seek64(asset, offset, whence);
}
static int64_t native_length(void *user, void *asset) {
    (void)user;
    return bs_android_asset_length64(asset);
}
static void native_close(void *user, void *asset) {
    (void)user;
    bs_android_asset_close(asset);
}
#endif

BsDexHorizonJavaResourcesNative
bs_dex_horizon_java_resources_production_native(
    BsDexHorizonAndroidAttachment *attachment) {
    BsDexHorizonJavaResourcesNative result = {0};
#if defined(__SWITCH__)
    if (attachment) {
        result.user = attachment;
        result.observed_asset_manager = native_manager;
        result.open_asset = native_open;
        result.read_asset = native_read;
        result.seek_asset = native_seek;
        result.length_asset = native_length;
        result.close_asset = native_close;
    }
#else
    (void)attachment;
#endif
    return result;
}
