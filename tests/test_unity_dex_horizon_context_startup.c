/* Read-only native startup observation over the pinned original APK owner.
 * No test path treats Horizon argv or applet IDs as Android Intent evidence. */
#include "unity_dex_horizon_context_objects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct StartupFixture {
    int owner_live;
    int startup_available;
    int bad_boolean;
    uint64_t aruid;
    int has_argv;
    int has_info;
    int is_nso;
    int count;
} StartupFixture;

static int owner_thread(void *user) {
    return ((StartupFixture *)user)->owner_live;
}
static int base_ready(void *user) {
    return ((StartupFixture *)user)->owner_live;
}
static int pixels(void *user, uint32_t *width, uint32_t *height) {
    if (!((StartupFixture *)user)->owner_live || !width || !height)
        return 0;
    *width = 1280;
    *height = 720;
    return 1;
}
static int language(void *user, char tag[9]) {
    if (!((StartupFixture *)user)->owner_live || !tag) return 0;
    memcpy(tag, "en-US", 6);
    return 1;
}
static int startup(void *user, BsDexHorizonStartupObservation *out) {
    StartupFixture *f = user;
    if (!out || !f->startup_available) return 0;
    ++f->count;
    *out = (BsDexHorizonStartupObservation){
        .applet_type = 0,
        .applet_resource_user_id = f->aruid,
        .loader_has_argv = f->bad_boolean ? 2 : f->has_argv,
        .loader_has_info = f->has_info,
        .loader_is_nso = f->is_nso
    };
    return 1;
}
int main(int argc, char **argv) {
    assert(argc == 7);
    BsDexQuestPackageHost package = {0};
    assert(bs_dex_quest_package_open(&package, argv[1]));
    assert(bs_dex_quest_package_observe_native_paths(
        &package, argv[2], argv[3]));

    StartupFixture f = {
        .owner_live = 1, .aruid = UINT64_C(0x1020304050607080),
        .has_argv = 1, .has_info = 1, .is_nso = 0
    };
    BsDexHorizonContextNative callbacks = {
        .user = &f,
        .on_owner_thread = owner_thread,
        .native_activity_base_ready = base_ready,
        .read_window_pixels = pixels,
        .read_horizon_language = language
    };
    BsDexHorizonContextObjects context = {0};
    assert(bs_dex_horizon_context_objects_init(
        &context, &package, &callbacks, argv[4], argv[5], argv[6]));
    BsDexHorizonStartupObservation observed = {
        .applet_type = -1, .applet_resource_user_id = UINT64_MAX
    };
    assert(!bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(observed.applet_type == 0 &&
           observed.applet_resource_user_id == 0);
    assert(!context.native.verify_launch_receipt);
    BsDexHorizonLaunchReceipt intent = {0};
    assert(!bs_dex_horizon_context_read_launch(&context, &intent));
    assert(!intent.action);

    context.native.observe_startup = startup;
    assert(!bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(!observed.applet_resource_user_id);
    f.startup_available = 1;
    assert(bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(observed.applet_type == 0 &&
           observed.applet_resource_user_id == f.aruid &&
           observed.loader_has_argv == 1 &&
           observed.loader_has_info == 1 &&
           observed.loader_is_nso == 0);
    assert(f.count == 1);
    f.has_argv = 0;
    f.has_info = 0;
    f.is_nso = 1;
    assert(bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(observed.loader_has_argv == 0 &&
           observed.loader_has_info == 0 &&
           observed.loader_is_nso == 1);
    assert(f.count == 2);
    f.bad_boolean = 1;
    assert(!bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(!observed.loader_has_argv &&
           !observed.applet_resource_user_id);
    f.bad_boolean = 0;
    f.owner_live = 0;
    assert(!bs_dex_horizon_context_observe_startup(&context, &observed));
    f.owner_live = 1;
    assert(bs_dex_horizon_context_observe_startup(&context, &observed));
    assert(f.count == 4);
    assert(!bs_dex_horizon_context_read_launch(&context, &intent));
    assert(!intent.source_token);
    assert(bs_dex_horizon_context_objects_release(&context));
    assert(!bs_dex_horizon_context_observe_startup(&context, &observed));
    bs_dex_quest_package_close(&package);
    puts("PASS: pinned original Quest native startup owner observes Horizon facts without Android Intent authorization");
    return 0;
}
