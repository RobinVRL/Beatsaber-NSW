#include "unity_dex_horizon_activity_host.h"
#include "unity_java_preinit.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Class metadata is read from the original DEX class_def superclass chain;
 * these expected descriptors are assertions, never a runtime type guess. */
static const char *const s_activity_lineage[] = {
    "Lcom/unity3d/player/UnityPlayerGameActivity;",
    "Lcom/google/androidgamesdk/GameActivity;",
    "Landroidx/appcompat/app/AppCompatActivity;",
    "Landroidx/fragment/app/FragmentActivity;",
    "Landroidx/activity/ComponentActivity;",
    "Landroidx/core/app/ComponentActivity;",
    "Landroid/app/Activity;"
};
#define BS_HORIZON_LINEAGE_SIZE \
    (sizeof(s_activity_lineage) / sizeof(s_activity_lineage[0]))
#define BS_HORIZON_WEAK_MAGIC UINT32_C(0x41574542)

typedef struct BsDexHorizonWeakToken {
    uint32_t magic;
    BsDexHorizonActivityHost *owner; /* non-owning native lifetime registry */
    uint64_t generation;
    BsDexValue referent;           /* scalar identity only; no JNI/global pin */
    int cleared;
} BsDexHorizonWeakToken;

static int rd32(const uint8_t *dex, size_t size, size_t off,
                uint32_t *out) {
    if (!dex || !out || off > size || size - off < 4) return 0;
    *out = (uint32_t)dex[off] | ((uint32_t)dex[off+1] << 8) |
           ((uint32_t)dex[off+2] << 16) | ((uint32_t)dex[off+3] << 24);
    return 1;
}

static int string_matches(const uint8_t *dex, size_t size,
                          uint32_t string_id, const char *expected) {
    uint32_t strings_size, strings_off, string_data_off;
    if (!rd32(dex, size, 0x38, &strings_size) ||
        !rd32(dex, size, 0x3c, &strings_off) ||
        string_id >= strings_size ||
        !rd32(dex, size, (size_t)strings_off + (size_t)string_id * 4,
              &string_data_off) || string_data_off >= size) return 0;
    size_t at = string_data_off;
    /* DEX string starts with a ULEB128 count of UTF-16 code points. All
     * descriptors here are ASCII, with encoded count equal to strlen. */
    uint32_t count = 0;
    unsigned shift = 0;
    int completed = 0;
    for (unsigned i = 0; i < 5 && at < size; ++i) {
        uint8_t octet = dex[at++];
        count |= (uint32_t)(octet & 0x7fu) << shift;
        if (!(octet & 0x80u)) {
            completed = 1;
            break;
        }
        shift += 7;
    }
    size_t expected_length = strlen(expected);
    if (!completed || count != expected_length || expected_length >= size ||
        at > size || size - at <= expected_length) return 0;
    return !memcmp(dex + at, expected, expected_length) &&
           dex[at + expected_length] == 0;
}

static int type_index(const uint8_t *dex, size_t size,
                      const char *descriptor, uint32_t *out) {
    uint32_t count, off;
    if (!rd32(dex, size, 0x40, &count) ||
        !rd32(dex, size, 0x44, &off) ||
        count > (size / 4) || off > size ||
        (size_t)count * 4 > size - off) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id;
        if (!rd32(dex, size, (size_t)off + (size_t)i * 4, &id))
            return 0;
        if (string_matches(dex, size, id, descriptor)) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static int original_lineage_matches(const uint8_t *dex, size_t size) {
    uint32_t types[BS_HORIZON_LINEAGE_SIZE];
    uint32_t class_count, class_off;
    if (!bs_unity_java_preinit_verify_dex(dex, size, NULL) ||
        !rd32(dex, size, 0x60, &class_count) ||
        !rd32(dex, size, 0x64, &class_off) ||
        class_count > size / 32 || class_off > size ||
        (size_t)class_count * 32 > size - class_off)
        return 0;
    for (size_t i = 0; i < BS_HORIZON_LINEAGE_SIZE; ++i)
        if (!type_index(dex, size, s_activity_lineage[i], &types[i]))
            return 0;
    for (size_t i = 0; i + 1 < BS_HORIZON_LINEAGE_SIZE; ++i) {
        int found = 0;
        for (uint32_t j = 0; j < class_count; ++j) {
            uint32_t id, super_id;
            size_t at = (size_t)class_off + (size_t)j * 32;
            if (!rd32(dex, size, at, &id) ||
                !rd32(dex, size, at + 8, &super_id)) return 0;
            if (id == types[i]) {
                if (found || super_id != types[i+1]) return 0;
                found = 1;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static int activity_type(const char *descriptor) {
    if (!descriptor) return 0;
    for (size_t i = 0; i < BS_HORIZON_LINEAGE_SIZE; ++i)
        if (!strcmp(descriptor, s_activity_lineage[i])) return 1;
    return 0;
}

static int ui_ready(const BsDexHorizonActivityHost *host) {
    return host && host->original_classes_verified &&
        host->platform.on_ui_thread &&
        host->platform.on_ui_thread(host->platform.user) == 1;
}

static int observed_external(const BsDexHorizonActivityHost *host,
                             const char *type, void *object) {
    if (!host || !host->activity.attached || !object ||
        !host->platform.verify_external ||
        host->platform.verify_external(host->platform.user, type, object) != 1)
        return 0;
    const BsDexAndroidContextAttachment *r = &host->observed;
    if (!strcmp(type, "Landroid/content/Intent;"))
        return object == r->intent;
    if (!strcmp(type, "Landroid/content/pm/ApplicationInfo;"))
        return object == r->application_info;
    if (!strcmp(type, "Landroid/content/res/Resources;"))
        return object == r->resources;
    if (!strcmp(type, "Landroid/content/res/AssetManager;"))
        return object == r->assets;
    if (!strcmp(type, "Landroid/content/pm/PackageManager;"))
        return object == r->package_manager;
    if (!strcmp(type, "Landroid/os/Bundle;"))
        return object == r->metadata_bundle;
    if (!strcmp(type, "Ljava/io/File;"))
        return object == r->files_dir || object == r->obb_dir;
    if (!strcmp(type, "Ljava/lang/String;"))
        return object == r->native_library_dir;
    return 0;
}

static int verify_type(void *user, const char *type, void *object) {
    BsDexHorizonActivityHost *host = user;
    if (!host || !type || !object) return 0;
    if (object == &host->activity)
        return host->activity.constructed && host->activity.alive &&
            activity_type(type);
    return observed_external(host, type, object);
}

static int verify_instance_of(void *user, void *object,
                              const char *descriptor, int *result,
                              const char **exception) {
    BsDexHorizonActivityHost *host = user;
    if (exception) *exception = NULL;
    if (result) *result = 0;
    if (!host || !result || !descriptor || object != &host->activity ||
        !host->activity.constructed || !host->activity.alive) return 0;
    /* Android platform inheritance beyond Activity is not packaged in the
     * Quest APK classes.dex: Activity extends ContextThemeWrapper, which
     * extends ContextWrapper, which extends Context. These are structural
     * Android framework assignability facts for this independently observed
     * original Activity owner; they DO NOT attach a Context or authorize any
     * Java Context getter. Runtime Context/Window/Intent state stays gated. */
    *result = activity_type(descriptor) ||
        !strcmp(descriptor, "Landroid/view/ContextThemeWrapper;") ||
        !strcmp(descriptor, "Landroid/content/ContextWrapper;") ||
        !strcmp(descriptor, "Landroid/content/Context;") ||
        !strcmp(descriptor, "Ljava/lang/Object;");
    return 1;
}

static int construct_base(void *user, const char *subclass,
                          void **activity, const char **exception) {
    BsDexHorizonActivityHost *host = user;
    if (exception) *exception = NULL;
    if (activity) *activity = NULL;
    if (!activity || !host || !subclass ||
        strcmp(subclass, s_activity_lineage[0]) || !ui_ready(host) ||
        !host->platform.construct_base || !host->platform.detach ||
        host->activity.constructed || host->weak_outstanding ||
        host->next_generation == UINT64_MAX) return 0;
    BsDexHorizonActivity created = {0};
    created.generation = ++host->next_generation;
    created.constructed = 1;
    created.alive = 1;
    host->activity = created;
    if (host->platform.construct_base(host->platform.user,
                                      &host->activity) != 1) {
        /* Attempt to undo any partially initialized native UI machinery.
         * Leave an unreleased owner visible if that rollback itself fails. */
        if (host->platform.detach(host->platform.user, &host->activity,
                                  NULL) == 1)
            host->activity = (BsDexHorizonActivity){0};
        return 0;
    }
    *activity = &host->activity;
    return 1;
}

int bs_dex_horizon_activity_on_ui_thread(void *user) {
    return ui_ready(user);
}

int bs_dex_horizon_activity_context_matches(void *user, void *context) {
    BsDexHorizonActivityHost *host = user;
    return host && context == &host->activity &&
        host->activity.constructed && host->activity.alive &&
        host->activity.attached && ui_ready(host) &&
        host->platform.valid_nwindow &&
        host->platform.valid_nwindow(host->platform.user,
                                     host->activity.window) == 1 &&
        host->platform.verify_attachment &&
        host->platform.verify_attachment(host->platform.user,
            &host->activity, host->activity.window, &host->observed) == 1;
}

static int attach_and_observe(void *user, void *activity,
                              BsDexAndroidContextAttachment *out,
                              const char **exception) {
    BsDexHorizonActivityHost *host = user;
    if (exception) *exception = NULL;
    if (!host || !out || activity != &host->activity ||
        !host->activity.constructed || !host->activity.alive ||
        host->activity.attached || !ui_ready(host) ||
        !host->platform.attach || !host->platform.valid_nwindow ||
        !host->platform.verify_external ||
        !host->platform.verify_attachment) return 0;
    *out = (BsDexAndroidContextAttachment){0};
    BsDexAndroidContextAttachment observed = {0};
    void *window = NULL;
    const char *raised = NULL;
    int rc = host->platform.attach(host->platform.user, activity, &window,
                                    &observed, &raised);
    if (rc != 1 || raised) {
        if (exception && rc < 0) *exception = raised;
        return rc < 0 && raised ? -1 : 0;
    }
    /* Require a physically verified Window and the exact observed Java
     * objects required by the Context API, before exposing any of them. */
    const int good =
        window && observed.activity == activity &&
        observed.intent && observed.application_info &&
        observed.resources && observed.assets &&
        host->platform.valid_nwindow(host->platform.user, window) == 1 &&
        host->platform.verify_attachment(host->platform.user, activity,
                                          window, &observed) == 1 &&
        host->platform.verify_external(host->platform.user,
            "Landroid/content/Intent;", observed.intent) == 1 &&
        host->platform.verify_external(host->platform.user,
            "Landroid/content/pm/ApplicationInfo;",
            observed.application_info) == 1 &&
        host->platform.verify_external(host->platform.user,
            "Landroid/content/res/Resources;", observed.resources) == 1 &&
        host->platform.verify_external(host->platform.user,
            "Landroid/content/res/AssetManager;", observed.assets) == 1;
    if (!good) {
        /* The native service may already have attached a Window. Roll it
         * back; if detachment fails retain the native owner for retry. */
        if (host->platform.detach(host->platform.user, activity, window) != 1) {
            host->activity.window = window;
            host->activity.attached = 1;
        } else {
            /* Successful native teardown invalidates the class instance.
             * It cannot be retried with an already-destroyed base owner. */
            host->activity.constructed = 0;
            host->activity.alive = 0;
            host->activity.attached = 0;
            host->activity.window = NULL;
            host->observed = (BsDexAndroidContextAttachment){0};
        }
        return 0;
    }
    host->observed = observed;
    host->activity.window = window;
    host->activity.attached = 1;
    *out = observed;
    return 1;
}

static int verify_attachment(void *user,
                              const BsDexAndroidContextAttachment *observed) {
    BsDexHorizonActivityHost *host = user;
    if (!host || !observed || !host->activity.attached ||
        observed->activity != &host->activity ||
        !bs_dex_horizon_activity_context_matches(host, observed->activity))
        return 0;
    /* Exact snapshot comparison catches replaced resource owners. */
    return observed->intent == host->observed.intent &&
        observed->application_info == host->observed.application_info &&
        observed->resources == host->observed.resources &&
        observed->assets == host->observed.assets &&
        observed->package_manager == host->observed.package_manager &&
        observed->metadata_bundle == host->observed.metadata_bundle &&
        observed->metadata_field_observed ==
            host->observed.metadata_field_observed &&
        observed->files_dir == host->observed.files_dir &&
        observed->obb_dir == host->observed.obb_dir &&
        observed->native_library_dir == host->observed.native_library_dir;
}

int bs_dex_horizon_activity_invalidate(BsDexHorizonActivityHost *host) {
    if (!host || !host->activity.constructed || !ui_ready(host) ||
        !host->platform.detach ||
        host->platform.detach(host->platform.user, &host->activity,
                              host->activity.window) != 1) return 0;
    host->activity.constructed = 0;
    host->activity.attached = 0;
    host->activity.alive = 0;
    host->activity.window = NULL;
    host->observed = (BsDexAndroidContextAttachment){0};
    if (host->context && host->context->activity == &host->activity)
        bs_dex_android_context_clear_attachment(host->context);
    return 1;
}

int bs_dex_horizon_activity_release(BsDexHorizonActivityHost *host) {
    if (!host || !ui_ready(host) || host->weak_outstanding) return 0;
    return !host->activity.constructed ||
           bs_dex_horizon_activity_invalidate(host);
}

static void discard_activity(void *user, void *activity) {
    BsDexHorizonActivityHost *host = user;
    if (host && activity == &host->activity)
        (void)bs_dex_horizon_activity_release(host);
}

static int weak_create(void *user, BsDexVM *vm, BsDexValue referent,
                       void **out, const char **exception) {
    BsDexHorizonActivityHost *host = user;
    if (exception) *exception = NULL;
    if (out) *out = NULL;
    if (!host || !vm || !out || !host->activity.constructed ||
        !host->activity.alive || !ui_ready(host) ||
        !host->platform.verify_external ||
        referent.kind != BS_DEX_REF || !referent.bits ||
        bs_dex_host_pointer(vm, referent) != &host->activity ||
        !bs_dex_reference_type(vm, referent) ||
        strcmp(bs_dex_reference_type(vm, referent),
               s_activity_lineage[0])) return 0;
    BsDexHorizonWeakToken *token = calloc(1, sizeof *token);
    if (!token) return 0;
    token->magic = BS_HORIZON_WEAK_MAGIC;
    token->owner = host;
    token->generation = host->activity.generation;
    token->referent = referent;
    ++host->weak_outstanding;
    *out = token;
    return 1;
}

static int weak_get(void *user, BsDexVM *vm, void *handle,
                    BsDexValue *out, const char **exception) {
    BsDexHorizonActivityHost *host = user;
    BsDexHorizonWeakToken *token = handle;
    if (exception) *exception = NULL;
    if (out) *out = bs_dex_null();
    if (!host || !vm || !out || !token ||
        token->magic != BS_HORIZON_WEAK_MAGIC ||
        token->owner != host) return 0;
    if (token->cleared || !host->activity.alive ||
        !host->activity.constructed ||
        token->generation != host->activity.generation)
        return 1;
    if (bs_dex_host_pointer(vm, token->referent) != &host->activity) return 0;
    *out = token->referent;
    return 1;
}

static int weak_clear(void *user, BsDexVM *vm, void *handle,
                      const char **exception) {
    BsDexHorizonActivityHost *host = user;
    BsDexHorizonWeakToken *token = handle;
    (void)vm;
    if (exception) *exception = NULL;
    if (!host || !token || token->magic != BS_HORIZON_WEAK_MAGIC ||
        token->owner != host) return 0;
    token->cleared = 1;
    return 1;
}

static void weak_release(void *user, BsDexVM *vm, void *handle) {
    BsDexHorizonActivityHost *host = user;
    BsDexHorizonWeakToken *token = handle;
    (void)vm;
    if (!host || !token || token->magic != BS_HORIZON_WEAK_MAGIC ||
        token->owner != host) return;
    token->magic = 0;
    if (host->weak_outstanding) --host->weak_outstanding;
    free(token);
}

int bs_dex_horizon_activity_init(BsDexHorizonActivityHost *host,
                                  const uint8_t *dex, size_t dex_size,
                                  const BsDexHorizonActivityServices *platform) {
    if (!host || !platform || !platform->on_ui_thread ||
        !platform->construct_base || !platform->attach ||
        !platform->valid_nwindow || !platform->verify_attachment ||
        !platform->verify_external || !platform->detach ||
        !original_lineage_matches(dex, dex_size)) return 0;
    *host = (BsDexHorizonActivityHost){0};
    host->platform = *platform;
    host->original_classes_verified = 1;
    return 1;
}

int bs_dex_horizon_activity_bind(BsDexHorizonActivityHost *host,
                                  BsDexAndroidHost *bridge,
                                  BsDexAndroidContextHost *context) {
    if (!host || !bridge || !context ||
        !host->original_classes_verified || host->activity.constructed ||
        host->weak_outstanding) return 0;
    host->context = context;
    context->user = host;
    context->verify = verify_type;
    context->verify_attachment = verify_attachment;
    bridge->user = host;
    bridge->context = context;
    bridge->observed_instance_of = verify_instance_of;
    bridge->construct_activity_base = construct_base;
    bridge->discard_activity = discard_activity;
    bridge->attach_and_observe_activity = attach_and_observe;
    bridge->weak_reference_create = weak_create;
    bridge->weak_reference_get = weak_get;
    bridge->weak_reference_clear = weak_clear;
    bridge->weak_reference_release = weak_release;
    return 1;
}
