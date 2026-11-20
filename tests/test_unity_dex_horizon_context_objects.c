#include "unity_dex_horizon_context_objects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    int owner_thread, base_ready, display_valid, language_valid;
    int original_launch_observed;
    int token;
    uint32_t width, height;
    char language[9];
} Fixture;

static int on_owner_thread(void *user) {
    return ((Fixture *)user)->owner_thread;
}
static int base_ready(void *user) {
    return ((Fixture *)user)->base_ready;
}
static int window_pixels(void *user, uint32_t *width, uint32_t *height) {
    Fixture *f = user;
    if (!f->display_valid || !width || !height) return 0;
    *width = f->width;
    *height = f->height;
    return 1;
}
static int horizon_language(void *user, char tag[9]) {
    Fixture *f = user;
    if (!f->language_valid || !tag) return 0;
    memcpy(tag, f->language, 9);
    return 1;
}
static int observed_launch(void *user,
                           const BsDexHorizonLaunchReceipt *receipt) {
    Fixture *f = user;
    return f->original_launch_observed && receipt &&
        receipt->source_token == &f->token;
}
static BsDexHorizonContextNative fixture_services(Fixture *fixture) {
    return (BsDexHorizonContextNative){
        .user = fixture,
        .on_owner_thread = on_owner_thread,
        .native_activity_base_ready = base_ready,
        .read_window_pixels = window_pixels,
        .read_horizon_language = horizon_language
        /* No launcher callback until genuinely observed. */
    };
}

int main(int argc, char **argv) {
    assert(argc == 7);
    BsDexQuestPackageHost package = {0};
    assert(bs_dex_quest_package_open(&package, argv[1]));
    assert(bs_dex_quest_package_observe_native_paths(
        &package, argv[2], argv[3]));
    Fixture f = {.owner_thread = 1, .base_ready = 1,
                 .display_valid = 1, .language_valid = 1,
                 .width = 1280, .height = 720,
                 .language = "en-US"};
    BsDexHorizonContextNative native = fixture_services(&f);
    BsDexHorizonContextObjects context = {0};
    assert(!bs_dex_horizon_context_objects_init(
        &context, &package, &native, "relative-path", argv[5], argv[6]));
    assert(!context.initialized);
    f.owner_thread = 0;
    assert(!bs_dex_horizon_context_objects_init(
        &context, &package, &native, argv[4], argv[5], argv[6]));
    f.owner_thread = 1;
    f.language[0] = '@';
    assert(!bs_dex_horizon_context_objects_init(
        &context, &package, &native, argv[4], argv[5], argv[6]));
    memcpy(f.language, "en-US", 6);
    assert(bs_dex_horizon_context_objects_init(
        &context, &package, &native, argv[4], argv[5], argv[6]));
    assert(context.initialized && context.generation == 1);
    assert(context.configuration.window_width_px == 1280 &&
           context.configuration.window_height_px == 720);
    assert(!strcmp(context.configuration.horizon_language_tag, "en-US"));
    assert(context.configuration.observation_number == 1);
    const BsDexHorizonContextFile *files = bs_dex_horizon_context_file(
        &context, BS_HCTX_FILES_DIR);
    const BsDexHorizonContextFile *obb = bs_dex_horizon_context_file(
        &context, BS_HCTX_OBB_DIR);
    const BsDexHorizonContextFile *external = bs_dex_horizon_context_file(
        &context, BS_HCTX_EXTERNAL_FILES_DIR);
    assert(files && obb && external && files != obb && obb != external);
    const char *path = NULL;
    assert(bs_dex_horizon_context_get_absolute_path(files, &path));
    assert(path && !strcmp(path, argv[4]));
    assert(bs_dex_horizon_context_get_absolute_path(obb, &path));
    assert(path && !strcmp(path, argv[5]));
    assert(bs_dex_horizon_context_get_absolute_path(external, &path));
    assert(path && !strcmp(path, argv[6]));
    assert(!bs_dex_horizon_context_file(&context, 77));
    assert(bs_dex_horizon_context_native_library_dir(&context, &path));
    assert(path && !strcmp(path, argv[2]));
    assert(!bs_dex_horizon_context_java_attachment_available(&context));
    /* Directory liveness is checked on EVERY read, not a cached pointer
     * equality to a once-observed native filesystem object. */
    char renamed[850];
    int written = snprintf(renamed, sizeof renamed, "%s.relocated", argv[4]);
    assert(written > 0 && (size_t)written < sizeof renamed);
    assert(rename(argv[4], renamed) == 0);
    assert(!bs_dex_horizon_context_get_absolute_path(files, &path));
    assert(path == NULL);
    assert(rename(renamed, argv[4]) == 0);
    assert(bs_dex_horizon_context_get_absolute_path(files, &path));

    BsDexHorizonContextConfig observed = {0};
    f.width = 1920;
    f.height = 1080;
    memcpy(f.language, "ja-JP", 6);
    assert(bs_dex_horizon_context_read_configuration(&context, &observed));
    assert(observed.window_width_px == 1920 &&
           observed.window_height_px == 1080 &&
           !strcmp(observed.horizon_language_tag, "ja-JP") &&
           observed.observation_number == 2);
    f.base_ready = 0;
    assert(!bs_dex_horizon_context_get_absolute_path(files, &path));
    assert(!path);
    assert(!bs_dex_horizon_context_read_configuration(&context, &observed));
    assert(!observed.observation_number);
    f.base_ready = 1;

    BsDexHorizonLaunchReceipt expected = {
        .action = "android.intent.action.MAIN",
        .package_name = "com.beatgames.beatsaber",
        .activity_class = "com.unity3d.player.UnityPlayerGameActivity",
        .launcher_category = 1, .vr_category = 1, .source_token = &f.token
    };
    BsDexHorizonLaunchReceipt actual = {0};
    assert(!bs_dex_horizon_context_bind_launch(&context, &expected));
    assert(!bs_dex_horizon_context_read_launch(&context, &actual));
    assert(!actual.action && !actual.source_token);
    native.verify_launch_receipt = observed_launch;
    context.native.verify_launch_receipt = observed_launch;
    f.original_launch_observed = 1;
    BsDexHorizonLaunchReceipt incorrect = expected;
    incorrect.activity_class = "com.example.Impostor";
    assert(!bs_dex_horizon_context_bind_launch(&context, &incorrect));
    incorrect = expected;
    incorrect.vr_category = 0;
    assert(!bs_dex_horizon_context_bind_launch(&context, &incorrect));
    assert(bs_dex_horizon_context_bind_launch(&context, &expected));
    assert(bs_dex_horizon_context_read_launch(&context, &actual));
    assert(!strcmp(actual.activity_class, expected.activity_class));
    assert(actual.source_token == &f.token);
    f.original_launch_observed = 0;
    assert(!bs_dex_horizon_context_read_launch(&context, &actual));
    assert(!actual.action && !actual.source_token);
    assert(!bs_dex_horizon_context_bind_launch(&context, &expected));

    assert(bs_dex_horizon_context_objects_release(&context));
    assert(!context.initialized && !context.launch_observed);
    assert(!bs_dex_horizon_context_get_absolute_path(files, &path));
    assert(!bs_dex_horizon_context_read_configuration(&context, &observed));
    assert(!bs_dex_horizon_context_objects_release(&context));
    BsDexHorizonContextProduction none = {0};
    BsDexHorizonContextNative unavailable =
        bs_dex_horizon_context_production_callbacks(&none);
    assert(!unavailable.on_owner_thread && !unavailable.read_window_pixels);
    assert(bs_dex_horizon_context_objects_init(
        &context, &package, &native, argv[4], argv[5], argv[6]));
    assert(context.generation == 2);
    assert(bs_dex_horizon_context_objects_release(&context));
    bs_dex_quest_package_close(&package);
    puts("PASS: native Quest Context filesystem/Configuration and observed-launch typed owners without Java readiness");
    return 0;
}
