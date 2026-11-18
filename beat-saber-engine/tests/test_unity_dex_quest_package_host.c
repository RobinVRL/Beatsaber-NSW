/* Original Quest APK manifest/resources and native/JNI package truth gates. */
#include "unity_dex_quest_package_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct JavaFixture {
    int intent, component, package_manager, appinfo, app_bundle;
    int activity_info, activity_bundle, native_dir;
    int observed_launch;
    int observed_package_manager;
    int observed_appinfo;
    int observed_activityinfo;
    int observed_bundles;
    int observed_native_dir;
} JavaFixture;

static int verify_class(void *user, void *object, const char *type) {
    JavaFixture *f = user;
    if (!strcmp(type, "Landroid/content/Intent;"))
        return object == &f->intent;
    if (!strcmp(type, "Landroid/content/ComponentName;"))
        return object == &f->component;
    if (!strcmp(type, "Landroid/content/pm/PackageManager;"))
        return object == &f->package_manager;
    if (!strcmp(type, "Landroid/content/pm/ApplicationInfo;"))
        return object == &f->appinfo;
    if (!strcmp(type, "Landroid/content/pm/ActivityInfo;"))
        return object == &f->activity_info;
    if (!strcmp(type, "Landroid/os/Bundle;"))
        return object == &f->app_bundle || object == &f->activity_bundle;
    if (!strcmp(type, "Ljava/lang/String;"))
        return object == &f->native_dir;
    return 0;
}
static int verify_appinfo(void *user, void *info,
                          const BsDexQuestApplicationInfo *model) {
    JavaFixture *f = user;
    return f->observed_appinfo && info == &f->appinfo && model &&
        !strcmp(model->package_name, "com.beatgames.beatsaber") &&
        model->target_sdk_version == 32 &&
        model->min_sdk_version == 32 &&
        model->label_resource == 0x7f0d0021u &&
        model->application_metadata_count == 15 &&
        model->native_library_dir && model->data_dir &&
        model->source_dir && model->public_source_dir;
}
static int verify_activityinfo(void *user, void *info,
                               const BsDexQuestActivityInfo *model) {
    JavaFixture *f = user;
    return f->observed_activityinfo && info == &f->activity_info &&
        model && model->exported && model->enabled &&
        !strcmp(model->name,
                "com.unity3d.player.UnityPlayerGameActivity") &&
        model->theme_resource == 0x7f0e00ffu &&
        model->config_changes == 1073758207u &&
        model->activity_metadata_count == 1;
}
static int verify_launch(void *user, void *intent, void *component,
                         const BsDexQuestLaunchFilter *filter) {
    JavaFixture *f = user;
    return f->observed_launch && intent == &f->intent &&
        component == &f->component && filter &&
        !strcmp(filter->action, "android.intent.action.MAIN") &&
        !strcmp(filter->package_name, "com.beatgames.beatsaber") &&
        !strcmp(filter->activity_class,
                "com.unity3d.player.UnityPlayerGameActivity") &&
        !strcmp(filter->launcher_category, "android.intent.category.LAUNCHER") &&
        !strcmp(filter->vr_category, "com.oculus.intent.category.VR");
}
static int verify_manager(void *user, void *manager,
                          void *activityinfo, void *component) {
    JavaFixture *f = user;
    return f->observed_package_manager &&
        manager == &f->package_manager &&
        activityinfo == &f->activity_info &&
        component == &f->component;
}
static int verify_bundle(void *user, void *bundle,
                         const BsApkManifest *manifest, int activity_scope) {
    JavaFixture *f = user;
    if (!f->observed_bundles || !manifest ||
        (activity_scope ? bundle != &f->activity_bundle :
                          bundle != &f->app_bundle)) return 0;
    const BsApkManifestMetadata *focus = bs_apk_manifest_metadata(
        manifest, "unity.run-without-focus", 0);
    const BsApkManifestMetadata *oculus = bs_apk_manifest_metadata(
        manifest, "com.oculus.vr.focusaware", 1);
    const BsApkManifestMetadata *support = bs_apk_manifest_metadata(
        manifest, "com.oculus.supportedDevices", 0);
    return focus && focus->value.kind == BS_APK_VALUE_BOOLEAN &&
        focus->value.raw == 1 &&
        oculus && oculus->value.kind == BS_APK_VALUE_BOOLEAN &&
        oculus->value.raw == 1 &&
        support && support->value.kind == BS_APK_VALUE_STRING &&
        !strcmp(support->value.text, "quest2|questpro|quest3|quest3s");
}
static int verify_native_dir(void *user, void *string, const char *expected) {
    JavaFixture *f = user;
    return f->observed_native_dir && string == &f->native_dir &&
        expected && expected[0];
}

int main(int argc, char **argv) {
    assert(argc == 4);
    BsDexQuestPackageHost host = {0};
    assert(!bs_dex_quest_package_open(&host, "missing-original-quest.apk"));
    assert(!bs_dex_quest_package_application(&host));
    assert(bs_dex_quest_package_open(&host, argv[1]));
    assert(host.initialized);
    assert(host.resources_crc == BS_QUEST_RESOURCES_ARSC_CRC);
    const BsDexQuestApplicationInfo *application =
        bs_dex_quest_package_application(&host);
    const BsDexQuestActivityInfo *activity =
        bs_dex_quest_package_activity(&host);
    const BsDexQuestLaunchFilter *launch =
        bs_dex_quest_package_launch_filter(&host);
    assert(application && activity && launch);
    assert(!strcmp(application->package_name, "com.beatgames.beatsaber"));
    assert(application->min_sdk_version == 32 &&
           application->target_sdk_version == 32 &&
           application->label_resource == 0x7f0d0021u);
    assert(!strcmp(application->source_dir, argv[1]));
    assert(!strcmp(application->public_source_dir, argv[1]));
    assert(!application->native_library_dir && !application->data_dir);
    assert(!strcmp(host.manifest.version_name, "1.46.0_34060"));
    assert(host.manifest.version_code == 3491);
    assert(application->application_metadata_count > 0);
    assert(activity->activity_metadata_count > 0);
    assert(launch->enabled && launch->exported && launch->vr_category);
    assert(!strcmp(launch->action, "android.intent.action.MAIN"));
    assert(!strcmp(launch->launcher_category,
                   "android.intent.category.LAUNCHER"));
    assert(!strcmp(launch->vr_category, "com.oculus.intent.category.VR"));

    BsApkManifestValue value = {0};
    assert(bs_dex_quest_package_metadata(&host, "unity.run-without-focus",
                                          0, &value));
    assert(value.kind == BS_APK_VALUE_BOOLEAN && value.raw == 1);
    assert(bs_dex_quest_package_metadata(&host,
        "unityplayer.SkipPermissionsDialog", 0, &value));
    assert(value.kind == BS_APK_VALUE_BOOLEAN && value.raw == 0);
    assert(bs_dex_quest_package_metadata(&host,
        "com.oculus.supportedDevices", 0, &value));
    assert(value.kind == BS_APK_VALUE_STRING);
    assert(!strcmp(value.text, "quest2|questpro|quest3|quest3s"));
    assert(bs_dex_quest_package_metadata(&host,
        "com.oculus.vr.focusaware", 1, &value));
    assert(value.kind == BS_APK_VALUE_BOOLEAN && value.raw == 1);
    assert(bs_dex_quest_package_metadata(&host,
        "unity.splash-mode", 0, &value));
    assert(value.kind == BS_APK_VALUE_INTEGER && value.raw == 0);
    assert(!bs_dex_quest_package_metadata(&host,
        "com.oculus.vr.focusaware", 0, &value));
    assert(value.kind == BS_APK_VALUE_ABSENT);
    assert(!bs_dex_quest_package_metadata(&host,
        "android.app.lib_name", 1, &value));
    assert(value.kind == BS_APK_VALUE_ABSENT);
    assert(!bs_dex_quest_package_metadata(&host, "missing", 1, &value));
    const char *string_value = "unobserved";
    assert(bs_dex_quest_package_bundle_get_string(
        &host, "android.app.lib_name", 1, &string_value));
    assert(string_value == NULL);
    assert(bs_dex_quest_package_bundle_get_string(
        &host, "com.oculus.supportedDevices", 0, &string_value));
    assert(string_value && !strcmp(string_value, "quest2|questpro|quest3|quest3s"));
    assert(!bs_dex_quest_package_bundle_get_string(
        &host, "unity.run-without-focus", 0, &string_value));
    assert(!string_value);
    int boolean_value = -1;
    assert(bs_dex_quest_package_bundle_get_boolean(
        &host, "unity.run-without-focus", 0, &boolean_value));
    assert(boolean_value == 1);
    assert(bs_dex_quest_package_bundle_get_boolean(
        &host, "android.app.lib_name", 1, &boolean_value));
    assert(boolean_value == 0);
    assert(!bs_dex_quest_package_bundle_get_boolean(
        &host, "com.oculus.supportedDevices", 0, &boolean_value));
    assert(boolean_value == 0);

    /* Native ELF directories and writable private app paths must be
     * observed separately from the manifest's public PackageItemInfo. */
    assert(!bs_dex_quest_package_observe_native_paths(
        &host, "nonexistent-native-libs", argv[3]));
    assert(!bs_dex_quest_package_observe_native_paths(
        &host, argv[2], "nonexistent-private-data"));
    assert(!host.native_paths_observed);

    JavaFixture f = {0};
    BsDexQuestJavaOwners candidate = {
        .launch_intent = &f.intent,
        .component = &f.component,
        .package_manager = &f.package_manager,
        .application_info = &f.appinfo,
        .application_metadata_bundle = &f.app_bundle,
        .activity_info = &f.activity_info,
        .activity_metadata_bundle = &f.activity_bundle,
        .native_library_dir_string = &f.native_dir
    };
    BsDexQuestJavaWitness witness = {
        .user = &f,
        .verify_class = verify_class,
        .verify_app_info = verify_appinfo,
        .verify_activity_info = verify_activityinfo,
        .verify_launch_intent = verify_launch,
        .verify_package_manager = verify_manager,
        .verify_bundle = verify_bundle,
        .verify_string_value = verify_native_dir
    };
    BsDexQuestJavaOwners accepted = {0};
    assert(!bs_dex_quest_package_verify_java_owners(
        &host, &candidate, &witness, &accepted));
    assert(!accepted.launch_intent && !accepted.application_info);

    /* Python prepares two correctly structured ARM64 ELF fixture headers in
     * the test dir, which prove native file verification without endorsing a
     * Java ApplicationInfo or Unity engine ELF readiness. */
    assert(bs_dex_quest_package_observe_native_paths(
        &host, argv[2], argv[3]));
    assert(host.native_paths_observed);
    assert(application->native_library_dir &&
           !strcmp(application->native_library_dir, argv[2]));
    assert(application->data_dir &&
           !strcmp(application->data_dir, argv[3]));
    assert(!bs_dex_quest_package_observe_native_paths(
        &host, argv[2], argv[3]));

    /* All Java object readiness remains unavailable until exact live
     * independent observations arrive. Missing callbacks must fail closed. */
    BsDexQuestJavaWitness partial = witness;
    partial.verify_launch_intent = NULL;
    assert(!bs_dex_quest_package_verify_java_owners(
        &host, &candidate, &partial, &accepted));
    assert(!accepted.application_info);
    f.observed_launch = 1;
    f.observed_package_manager = 1;
    f.observed_appinfo = 1;
    f.observed_activityinfo = 1;
    f.observed_bundles = 1;
    f.observed_native_dir = 1;
    assert(bs_dex_quest_package_verify_java_owners(
        &host, &candidate, &witness, &accepted));
    assert(accepted.application_info == &f.appinfo);
    assert(accepted.activity_info == &f.activity_info);
    assert(accepted.launch_intent == &f.intent);

    /* The final staging operation RECHECKS the live witness and cannot use
     * an old accepted snapshot after the native launch is invalidated. */
    BsDexAndroidContextAttachment context = {
        .activity = &f.component, .resources = &f.app_bundle,
        .assets = &f.activity_bundle
    };
    f.observed_launch = 0;
    assert(!bs_dex_quest_package_stage_context(
        &host, &accepted, &witness, &context));
    assert(!context.intent && !context.application_info);
    f.observed_launch = 1;
    assert(bs_dex_quest_package_stage_context(
        &host, &accepted, &witness, &context));
    assert(context.intent == &f.intent && context.application_info == &f.appinfo);
    assert(context.package_manager == &f.package_manager &&
           context.metadata_bundle == &f.app_bundle &&
           context.metadata_field_observed == 1 &&
           context.native_library_dir == &f.native_dir);
    assert(context.activity == &f.component &&
           context.resources == &f.app_bundle &&
           context.assets == &f.activity_bundle);
    assert(!bs_dex_quest_package_stage_context(
        &host, &candidate, &witness, &context));

    bs_dex_quest_package_close(&host);
    assert(!bs_dex_quest_package_application(&host));
    assert(!bs_dex_quest_package_launch_filter(&host));
    puts("PASS: original Quest APK package, typed Intent filter/ApplicationInfo/ActivityInfo, resource table, staged ELF paths and Java witness gates");
    return 0;
}
