#include "unity_dex_android_context_host.h"
#include <string.h>

#define EQ(a,b) ((a) && strcmp((a),(b)) == 0)
#define BS_ACTIVITY "Landroid/app/Activity;"
#define BS_CONTEXT "Landroid/content/Context;"
#define BS_GAME_ACTIVITY "Lcom/unity3d/player/UnityPlayerGameActivity;"
#define BS_INTENT "Landroid/content/Intent;"
#define BS_APPINFO "Landroid/content/pm/ApplicationInfo;"
#define BS_RESOURCES "Landroid/content/res/Resources;"
#define BS_ASSETS "Landroid/content/res/AssetManager;"
#define BS_PACKAGE_MANAGER "Landroid/content/pm/PackageManager;"
#define BS_BUNDLE "Landroid/os/Bundle;"
#define BS_FILE "Ljava/io/File;"
#define BS_STRING "Ljava/lang/String;"
#define BS_VIEW "Landroid/view/View;"

int bs_dex_android_context_bind_attachment(
    BsDexAndroidContextHost *host,
    const BsDexAndroidContextAttachment *observed) {
    if (!host || !observed || !host->verify ||
        !host->verify_attachment || host->attachment_evidenced ||
        !observed->activity ||
        (host->activity && host->activity != observed->activity) ||
        host->verify(host->user, BS_GAME_ACTIVITY,
                     observed->activity) != 1 ||
        !observed->intent ||
        host->verify(host->user, BS_INTENT, observed->intent) != 1 ||
        !observed->application_info ||
        host->verify(host->user, BS_APPINFO,
                     observed->application_info) != 1 ||
        !observed->resources ||
        host->verify(host->user, BS_RESOURCES,
                     observed->resources) != 1 ||
        !observed->assets ||
        host->verify(host->user, BS_ASSETS, observed->assets) != 1 ||
        (observed->package_manager &&
         host->verify(host->user, BS_PACKAGE_MANAGER,
                      observed->package_manager) != 1) ||
        (observed->metadata_bundle &&
         (!observed->metadata_field_observed ||
          host->verify(host->user, BS_BUNDLE,
                       observed->metadata_bundle) != 1)) ||
        (observed->files_dir &&
         host->verify(host->user, BS_FILE, observed->files_dir) != 1) ||
        (observed->obb_dir &&
         host->verify(host->user, BS_FILE, observed->obb_dir) != 1) ||
        (observed->native_library_dir &&
         host->verify(host->user, BS_STRING,
                      observed->native_library_dir) != 1) ||
        host->verify_attachment(host->user, observed) != 1)
        return 0;
    host->activity = observed->activity;
    host->intent = observed->intent;
    host->application_info = observed->application_info;
    host->resources = observed->resources;
    host->assets = observed->assets;
    host->package_manager = observed->package_manager;
    host->metadata_bundle = observed->metadata_bundle;
    host->metadata_field_observed = !!observed->metadata_field_observed;
    host->files_dir = observed->files_dir;
    host->obb_dir = observed->obb_dir;
    host->native_library_dir = observed->native_library_dir;
    host->attachment_evidenced = 1;
    return 1;
}

void bs_dex_android_context_clear_attachment(BsDexAndroidContextHost *host) {
    if (!host) return;
    host->activity = NULL;
    host->intent = NULL;
    host->application_info = NULL;
    host->resources = NULL;
    host->assets = NULL;
    host->package_manager = NULL;
    host->metadata_bundle = NULL;
    host->metadata_field_observed = 0;
    host->files_dir = NULL;
    host->obb_dir = NULL;
    host->native_library_dir = NULL;
    host->attachment_evidenced = 0;
}

static int live(BsDexAndroidContextHost *host,
                const char *kind, void *handle) {
    return host && host->verify && handle &&
           host->verify(host->user, kind, handle) == 1;
}

static int receiver_is(BsDexAndroidContextHost *host, BsDexVM *vm,
                       const BsDexValue *args, size_t count,
                       const char *kind, void *handle) {
    return count && args && args[0].kind == BS_DEX_REF &&
           bs_dex_host_pointer(vm, args[0]) == handle &&
           live(host, kind, handle);
}

static int return_object(BsDexAndroidContextHost *host, BsDexVM *vm,
                         void *object, const char *kind, BsDexValue *result) {
    if (!live(host, kind, object)) return 0;
    BsDexValue ref = bs_dex_wrap_platform(vm, object, kind);
    if (ref.kind != BS_DEX_REF || !ref.bits) return 0;
    *result = ref;
    return 1;
}

static int context_getter(BsDexAndroidContextHost *host, BsDexVM *vm,
                          const char *name, const char *proto,
                          BsDexValue *result) {
    if (EQ(name, "getApplicationInfo") &&
        EQ(proto, "()Landroid/content/pm/ApplicationInfo;"))
        return return_object(host, vm, host->application_info, BS_APPINFO, result);
    if (EQ(name, "getIntent") &&
        EQ(proto, "()Landroid/content/Intent;"))
        return return_object(host, vm, host->intent, BS_INTENT, result);
    if (EQ(name, "getResources") &&
        EQ(proto, "()Landroid/content/res/Resources;"))
        return return_object(host, vm, host->resources, BS_RESOURCES, result);
    if (EQ(name, "getAssets") &&
        EQ(proto, "()Landroid/content/res/AssetManager;"))
        return return_object(host, vm, host->assets, BS_ASSETS, result);
    if (EQ(name, "getPackageManager") &&
        EQ(proto, "()Landroid/content/pm/PackageManager;"))
        return return_object(host, vm, host->package_manager,
                             BS_PACKAGE_MANAGER, result);
    if (EQ(name, "getFilesDir") &&
        EQ(proto, "()Ljava/io/File;"))
        return return_object(host, vm, host->files_dir, BS_FILE, result);
    if (EQ(name, "getObbDir") &&
        EQ(proto, "()Ljava/io/File;"))
        return return_object(host, vm, host->obb_dir, BS_FILE, result);
    return 0;
}

int bs_dex_android_context_invoke(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    BsDexAndroidContextHost *host = user;
    if (exception) *exception = NULL;
    if (!host || !vm || !method_name || !prototype || !result ||
        !args || !count || !host->verify) return 0;
    (void)invoke_opcode;
    *result = bs_dex_null();

    const int activity_method =
        EQ(declaring_class, BS_ACTIVITY) ||
        EQ(declaring_class, BS_CONTEXT) ||
        EQ(declaring_class, BS_GAME_ACTIVITY);
    if (activity_method) {
        if (!receiver_is(host, vm, args, count, BS_GAME_ACTIVITY,
                         host->activity)) return 0;
        if (count == 1)
            return context_getter(host, vm, method_name, prototype, result);
        if (count == 2 && EQ(method_name, "findViewById") &&
            EQ(prototype, "(I)Landroid/view/View;") &&
            args[1].kind == BS_DEX_INT && host->find_view) {
            void *view = NULL;
            if (host->find_view(host->user, host->activity,
                                (int32_t)args[1].bits, &view) != 1) return 0;
            if (!view) return 1; /* Real Activity lookup returned null. */
            return return_object(host, vm, view, BS_VIEW, result);
        }
        return 0;
    }
    if (EQ(declaring_class, BS_INTENT) &&
        receiver_is(host, vm, args, count, BS_INTENT, host->intent)) {
        if (count == 2 && EQ(method_name, "getStringExtra") &&
            EQ(prototype, "(Ljava/lang/String;)Ljava/lang/String;") &&
            host->get_extra) {
            const char *key = bs_dex_string(vm, args[1]);
            if (!key) return 0;
            void *actual_string = NULL;
            const char *raised = NULL;
            int rc = host->get_extra(host->user, host->intent, key,
                                     &actual_string, &raised);
            if (rc != 1) {
                if (rc < 0 && raised && exception) *exception = raised;
                return rc < 0 ? -1 : 0;
            }
            if (!actual_string) return 1; /* Observed absent key. */
            return return_object(host, vm, actual_string, BS_STRING, result);
        }
        if (count == 3 && EQ(method_name, "putExtra") &&
            EQ(prototype, "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;") &&
            host->put_extra) {
            const char *key = bs_dex_string(vm, args[1]);
            const char *text = args[2].kind == BS_DEX_REF && !args[2].bits ?
                               NULL : bs_dex_string(vm, args[2]);
            if (!key || (args[2].bits && !text)) return 0;
            const char *raised = NULL;
            int rc = host->put_extra(host->user, host->intent, key,
                                     text, &raised);
            if (rc != 1) {
                if (rc < 0 && raised && exception) *exception = raised;
                return rc < 0 ? -1 : 0;
            }
            *result = args[0]; /* Android Intent.putExtra returns receiver. */
            return 1;
        }
    }
    if (EQ(declaring_class, BS_BUNDLE) &&
        receiver_is(host, vm, args, count, BS_BUNDLE, host->metadata_bundle) &&
        count == 2 && EQ(method_name, "getBoolean") &&
        EQ(prototype, "(Ljava/lang/String;)Z") && host->get_boolean) {
        const char *key = bs_dex_string(vm, args[1]);
        if (!key) return 0;
        int value = 0;
        const char *raised = NULL;
        int rc = host->get_boolean(host->user, host->metadata_bundle,
                                   key, &value, &raised);
        if (rc != 1) {
            if (rc < 0 && raised && exception) *exception = raised;
            return rc < 0 ? -1 : 0;
        }
        *result = bs_dex_int(!!value);
        return 1;
    }
    return 0;
}

int bs_dex_android_context_field(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *field_name,
    const char *field_type, BsDexValue receiver, int is_write,
    BsDexValue *value, const char **exception) {
    BsDexAndroidContextHost *host = user;
    if (exception) *exception = NULL;
    if (!vm || !value || !host || is_write || !host->verify) return 0;
    if (receiver_is(host, vm, &receiver, 1, BS_APPINFO,
                    host->application_info)) {
        if (EQ(declaring_class, BS_APPINFO) &&
            EQ(field_name, "nativeLibraryDir") &&
            EQ(field_type, BS_STRING))
            return return_object(host, vm, host->native_library_dir,
                                 BS_STRING, value);
        if (EQ(declaring_class, BS_APPINFO) &&
            EQ(field_name, "metaData") &&
            EQ(field_type, BS_BUNDLE)) {
            if (host->metadata_bundle)
                return return_object(host, vm, host->metadata_bundle,
                                     BS_BUNDLE, value);
            if (!host->metadata_field_observed) return 0;
            *value = bs_dex_null(); /* Real Android field read returned null. */
            return 1;
        }
    }
    return 0;
}
