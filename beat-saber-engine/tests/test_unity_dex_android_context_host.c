/* Isolated service contract test. These addresses model independently
 * verified host Java objects; the real NRO must provide actual objects and
 * callbacks before a single one of these methods may report success. */
#include "unity_apk_dex_loader.h"
#include "unity_dex_android_context_host.h"
#include "unity_java_preinit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int activity, intent, appinfo, metadata, native_dir, string, resources, view;
    int assets, files_dir, obb_dir, package_manager, attachment_observed;
    char stored[64];
    int view_queried;
    int appinfo_live;
} Fixture;

static int validate(void *user, const char *descriptor, void *object) {
    Fixture *f = user;
    if (!strcmp(descriptor,
                "Lcom/unity3d/player/UnityPlayerGameActivity;"))
        return object == &f->activity;
    if (!strcmp(descriptor, "Landroid/content/Intent;"))
        return object == &f->intent;
    if (!strcmp(descriptor, "Landroid/content/pm/ApplicationInfo;"))
        return f->appinfo_live && object == &f->appinfo;
    if (!strcmp(descriptor, "Landroid/os/Bundle;"))
        return object == &f->metadata;
    if (!strcmp(descriptor, "Ljava/lang/String;"))
        return object == &f->string || object == &f->native_dir;
    if (!strcmp(descriptor, "Landroid/content/res/Resources;"))
        return object == &f->resources;
    if (!strcmp(descriptor, "Landroid/content/res/AssetManager;"))
        return object == &f->assets;
    if (!strcmp(descriptor, "Landroid/content/pm/PackageManager;"))
        return object == &f->package_manager;
    if (!strcmp(descriptor, "Ljava/io/File;"))
        return object == &f->files_dir || object == &f->obb_dir;
    if (!strcmp(descriptor, "Landroid/view/View;"))
        return object == &f->view;
    return 0;
}
static int verify_attachment(void *user,
                             const BsDexAndroidContextAttachment *observed) {
    Fixture *f = user;
    return f->attachment_observed && observed->activity == &f->activity &&
           observed->intent == &f->intent &&
           observed->assets == &f->assets;
}

static int find_view(void *user, void *activity, int32_t id, void **view) {
    Fixture *f = user;
    if (activity != &f->activity) return 0;
    ++f->view_queried;
    *view = id == 123 ? &f->view : NULL;
    return 1;
}

static int put_extra(void *user, void *intent, const char *key,
                     const char *value, const char **exception) {
    Fixture *f = user;
    (void)exception;
    if (intent != &f->intent || strcmp(key, "sceneName")) return 0;
    if (value && strlen(value) >= sizeof f->stored) return 0;
    snprintf(f->stored, sizeof f->stored, "%s", value ? value : "");
    return 1;
}
static int get_extra(void *user, void *intent, const char *key,
                     void **out, const char **exception) {
    Fixture *f = user;
    (void)exception;
    if (intent != &f->intent) return 0;
    *out = !strcmp(key, "sceneName") && f->stored[0] ? &f->string : NULL;
    return 1;
}
static int get_boolean(void *user, void *bundle, const char *key,
                       int *out, const char **exception) {
    Fixture *f = user;
    (void)exception;
    if (bundle != &f->metadata) return 0;
    *out = !strcmp(key, "unity.run-without-focus");
    return 1;
}

static int validate_activity(void *user, void *instance) {
    Fixture *f = user;
    return instance == &f->activity;
}

static BsDexValue call(BsDexAndroidContextHost *host, BsDexVM *vm,
                       const char *klass, const char *name, const char *proto,
                       const BsDexValue *args, size_t count, int expected) {
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    int rc = bs_dex_android_context_invoke(
        host, vm, klass, name, proto, 0x6e, args, count, &result, &exception);
    assert(rc == expected && exception == NULL);
    return result;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    BsUnityOriginalDexImage dex = {0};
    assert(bs_unity_apk_load_original_dex(argv[1], &dex));
    Fixture f = {.appinfo_live=1};
    BsDexHost api = {.user=&f, .validate_activity=validate_activity};
    BsDexVM *vm = bs_dex_vm_create(dex.data, dex.size, &api);
    assert(vm);
    BsDexAndroidContextHost host = {
        .user=&f, .activity=&f.activity, .intent=&f.intent,
        .application_info=&f.appinfo, .metadata_bundle=&f.metadata,
        .metadata_field_observed=1,
        .native_library_dir=&f.native_dir, .resources=&f.resources,
        .verify=validate, .find_view=find_view,
        .put_extra=put_extra, .get_extra=get_extra,
        .get_boolean=get_boolean
    };
    BsDexAndroidContextAttachment attachment = {
        .activity=&f.activity, .intent=&f.intent,
        .application_info=&f.appinfo, .resources=&f.resources,
        .assets=&f.assets, .package_manager=&f.package_manager,
        .metadata_bundle=&f.metadata, .metadata_field_observed=1,
        .files_dir=&f.files_dir, .obb_dir=&f.obb_dir,
        .native_library_dir=&f.native_dir
    };
    /* A prepopulated Context pointer must not suffice to attest attachment.
     * The backend must observe the actual lifecycle and all required types. */
    assert(!bs_dex_android_context_bind_attachment(&host, &attachment));
    host.verify_attachment = verify_attachment;
    assert(!bs_dex_android_context_bind_attachment(&host, &attachment));
    f.attachment_observed = 1;
    BsDexAndroidContextAttachment incomplete = attachment;
    incomplete.assets = NULL;
    assert(!bs_dex_android_context_bind_attachment(&host, &incomplete));
    assert(!host.attachment_evidenced);
    assert(bs_dex_android_context_bind_attachment(&host, &attachment));
    assert(host.attachment_evidenced);
    assert(!bs_dex_android_context_bind_attachment(&host, &attachment));
    const BsDexValue activity = bs_dex_wrap_platform(
        vm, &f.activity, "Lcom/unity3d/player/UnityPlayerGameActivity;");
    assert(activity.bits);
    BsDexValue args[] = {activity, bs_dex_int(123), bs_dex_null()};
    BsDexValue intent = call(&host, vm, "Landroid/app/Activity;", "getIntent",
        "()Landroid/content/Intent;", args, 1, 1);
    assert(intent.bits && bs_dex_host_pointer(vm, intent) == &f.intent);
    args[0] = intent;
    args[1] = bs_dex_new_observed_string_utf8(vm, "sceneName", 9);
    args[2] = bs_dex_new_observed_string_utf8(vm, "BeatSaber", 9);
    assert(args[1].bits && args[2].bits);
    BsDexValue ret = call(&host, vm, "Landroid/content/Intent;", "putExtra",
        "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
        args, 3, 1);
    assert(ret.bits == intent.bits && !strcmp(f.stored, "BeatSaber"));
    ret = call(&host, vm, "Landroid/content/Intent;", "getStringExtra",
        "(Ljava/lang/String;)Ljava/lang/String;", args, 2, 1);
    assert(bs_dex_host_pointer(vm, ret) == &f.string);
    BsDexValue bad = activity;
    args[0] = activity;
    ret = call(&host, vm, "Landroid/content/Context;", "getApplicationInfo",
        "()Landroid/content/pm/ApplicationInfo;", args, 1, 1);
    assert(bs_dex_host_pointer(vm, ret) == &f.appinfo);
    BsDexValue field = bs_dex_null();
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "nativeLibraryDir", "Ljava/lang/String;",
        ret, 0, &field, NULL) == 1);
    assert(bs_dex_host_pointer(vm, field) == &f.native_dir);
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "metaData", "Landroid/os/Bundle;", ret, 0, &field, NULL) == 1);
    assert(bs_dex_host_pointer(vm, field) == &f.metadata);
    host.metadata_bundle = NULL;
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "metaData", "Landroid/os/Bundle;", ret, 0, &field, NULL) == 1);
    assert(!field.bits && field.kind == BS_DEX_REF);
    host.metadata_field_observed = 0;
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "metaData", "Landroid/os/Bundle;", ret, 0, &field, NULL) == 0);
    host.metadata_bundle = &f.metadata;
    host.metadata_field_observed = 1;
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "nativeLibraryDir", "Ljava/lang/String;",
        ret, 1, &field, NULL) == 0);
    f.appinfo_live = 0; /* No success after backing is revoked. */
    assert(bs_dex_android_context_field(
        &host, vm, "Landroid/content/pm/ApplicationInfo;",
        "nativeLibraryDir", "Ljava/lang/String;",
        ret, 0, &field, NULL) == 0);
    f.appinfo_live = 1;
    args[0] = activity;
    args[1] = bs_dex_int(123);
    ret = call(&host, vm, "Landroid/app/Activity;", "findViewById",
        "(I)Landroid/view/View;", args, 2, 1);
    assert(bs_dex_host_pointer(vm, ret) == &f.view && f.view_queried == 1);
    args[1] = bs_dex_int(999);
    ret = call(&host, vm, "Landroid/app/Activity;", "findViewById",
        "(I)Landroid/view/View;", args, 2, 1);
    assert(ret.kind == BS_DEX_REF && !ret.bits && f.view_queried == 2);
    args[0] = bs_dex_wrap_platform(vm, &f.metadata, "Landroid/os/Bundle;");
    args[1] = bs_dex_new_observed_string_utf8(vm, "unity.run-without-focus", 23);
    assert(args[0].bits && args[1].bits);
    ret = call(&host, vm, "Landroid/os/Bundle;", "getBoolean",
        "(Ljava/lang/String;)Z", args, 2, 1);
    assert(ret.kind == BS_DEX_INT && ret.bits == 1);
    args[0] = bad;
    assert(call(&host, vm, "Landroid/content/Intent;", "getStringExtra",
        "(Ljava/lang/String;)Ljava/lang/String;", args, 2, 0).bits == 0);
    BsDexAndroidContextHost unavailable = host;
    unavailable.verify = NULL;
    assert(call(&unavailable, vm, "Landroid/app/Activity;", "getIntent",
        "()Landroid/content/Intent;", args, 1, 0).bits == 0);
    bs_dex_vm_destroy(vm);
    bs_dex_android_context_clear_attachment(&host);
    assert(!host.attachment_evidenced && !host.activity && !host.intent);
    bs_unity_apk_free_original_dex(&dex);
    puts("PASS: backed Context/Intent/ApplicationInfo/Bundle JNI-Dex host adapter");
    return 0;
}
