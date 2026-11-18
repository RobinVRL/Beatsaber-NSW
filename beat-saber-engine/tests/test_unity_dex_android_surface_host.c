/* Execute ORIGINAL DEX SurfaceView constructor with an explicitly MOCKED
 * NWindow/JNI layer. These tests validate adapter effects and fail-closed
 * transitions; they do not claim a genuine Switch Java/UI tree exists.
 *
 * Build and run with tests/test_unity_dex_android_surface_host.py
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../source/unity_dex_android_surface_host.h"
#include "../source/unity_java_preinit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeSurfaceService {
    int activity;
    int nwindow;
    int rogue_window;
    int jni_surface;
    int surface_issued;
    int attached;
    int java_view;
    int live_window;
    int ui_thread;
    int reject_context;
    int reject_construct;
    int rogue_backing;
    int reject_java_class;
    int fail_remove;
    int refuse_holder_registration;
    uintptr_t view_identity;
    int attach_calls;
    int detach_calls;
    int revoke_calls;
    int callback_calls;
    int format_calls;
    int zorder_calls;
    int last_zorder;
    int32_t last_format;
    BsDexValue registered_callback;
    BsDexAndroidSurfaceHost surface;
} FakeSurfaceService;

static int match_context(void *user, void *context) {
    FakeSurfaceService *s = user;
    return !s->reject_context && context == &s->activity;
}

static int ui_thread(void *user) {
    return ((FakeSurfaceService *)user)->ui_thread;
}

static int attach_view(void *user, void *context, uintptr_t identity,
                       int is_surface, void **window) {
    FakeSurfaceService *s = user;
    if (s->reject_construct || context != &s->activity || s->attached ||
        !identity || !window) return 0;
    s->view_identity = identity;
    s->attached = 1;
    ++s->attach_calls;
    *window = is_surface ? &s->nwindow : NULL;
    return 1;
}

static int detach_view(void *user, uintptr_t identity, void *window) {
    FakeSurfaceService *s = user;
    if (!s->attached || s->view_identity != identity || s->fail_remove ||
        (window && window != &s->nwindow)) return 0;
    s->attached = 0;
    s->view_identity = 0;
    ++s->detach_calls;
    return 1;
}

static void *constructed_java_view(void *user, uintptr_t identity) {
    FakeSurfaceService *s = user;
    return s->attached && s->view_identity == identity ? &s->java_view : NULL;
}

static int view_instance_of(void *user, BsDexVM *vm, BsDexValue value,
                            const char *descriptor, int *matches,
                            const char **exception) {
    FakeSurfaceService *s = user;
    *exception = NULL;
    *matches = !s->reject_java_class &&
               bs_dex_host_pointer(vm, value) ==
                   constructed_java_view(s, value.bits) &&
               !strcmp(descriptor,
                 "Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;");
    return 1;
}

static int window_dimensions(void *user, void *window,
                             uint32_t *width, uint32_t *height) {
    FakeSurfaceService *s = user;
    if (!s->live_window || !s->attached || window != &s->nwindow ||
        !width || !height) return 0;
    *width = 1280;
    *height = 720;
    return 1;
}

static void *create_surface(void *user, void *window) {
    FakeSurfaceService *s = user;
    if (!s->live_window || !s->attached || window != &s->nwindow) return NULL;
    s->surface_issued = 1;
    return &s->jni_surface;
}

static void *backed_window(void *user, void *surface) {
    FakeSurfaceService *s = user;
    if (!s->surface_issued || !s->live_window ||
        surface != &s->jni_surface) return NULL;
    return s->rogue_backing ? &s->rogue_window : &s->nwindow;
}

static void revoke_surface(void *user, void *surface) {
    FakeSurfaceService *s = user;
    assert(surface == &s->jni_surface);
    s->surface_issued = 0;
    ++s->revoke_calls;
}

static int holder_callback_type(void *user, BsDexVM *vm, BsDexValue callback) {
    (void)user;
    const char *type = bs_dex_reference_type(vm, callback);
    return type && strcmp(type, "Lcom/unity3d/player/UnityPlayer;") == 0;
}

static int register_callback(void *user, uintptr_t identity,
                             BsDexValue callback) {
    FakeSurfaceService *s = user;
    if (!s->attached || s->view_identity != identity ||
        s->refuse_holder_registration || !callback.bits) return 0;
    s->registered_callback = callback;
    ++s->callback_calls;
    return 1;
}

static int set_format(void *user, uintptr_t identity, int32_t format) {
    FakeSurfaceService *s = user;
    if (!s->attached || s->view_identity != identity) return 0;
    ++s->format_calls;
    s->last_format = format;
    return 1;
}

static int set_zorder(void *user, uintptr_t identity, int on_top) {
    FakeSurfaceService *s = user;
    if (!s->attached || s->view_identity != identity) return 0;
    ++s->zorder_calls;
    s->last_zorder = on_top;
    return 1;
}

static void init_fake(FakeSurfaceService *fixture, int supply_surface) {
    memset(fixture, 0, sizeof *fixture);
    fixture->ui_thread = 1;
    fixture->live_window = 1;
    BsDexAndroidSurfaceServices services = {
        .user = fixture,
        .context_matches_activity = match_context,
        .on_ui_thread = ui_thread,
        .construct_view = attach_view,
        .remove_view = detach_view,
        .get_window_dimensions = supply_surface ? window_dimensions : NULL,
        .surface_from_nwindow = supply_surface ? create_surface : NULL,
        .verified_nwindow_from_surface = supply_surface ? backed_window : NULL,
        .revoke_surface = supply_surface ? revoke_surface : NULL,
        .is_holder_callback = holder_callback_type,
        .register_holder_callback = register_callback,
        .set_holder_format = set_format,
        .set_z_order_on_top = set_zorder,
        .constructed_java_view = constructed_java_view
    };
    bs_dex_android_surface_host_init(&fixture->surface, &services);
}

static int validate_activity(void *user, void *activity) {
    FakeSurfaceService *fixture = user;
    return activity == &fixture->activity;
}

static int vm_invoke(void *user, BsDexVM *vm, const char *cls,
                     const char *name, const char *proto, uint8_t op,
                     const BsDexValue *args, size_t count, BsDexValue *result,
                     const char **exception) {
    FakeSurfaceService *fixture = user;
    return bs_dex_android_surface_invoke_platform(
        &fixture->surface, vm, cls, name, proto, op, args, count, result, exception);
}

static int vm_new(void *user, BsDexVM *vm, const char *cls,
                  BsDexValue *result, const char **exception) {
    FakeSurfaceService *fixture = user;
    return bs_dex_android_surface_new_platform_object(
        &fixture->surface, vm, cls, result, exception);
}

static uint8_t *load_dex(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long count = ftell(file);
    assert(count == BS_UNITY_JAVA_DEX_SIZE && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *dex = malloc((size_t)count);
    assert(dex && fread(dex, 1, (size_t)count, file) == (size_t)count);
    assert(fclose(file) == 0);
    *size = (size_t)count;
    return dex;
}

static BsDexVM *new_vm(FakeSurfaceService *fixture,
                        const uint8_t *dex, size_t size) {
    BsDexHost host = {
        .user = fixture,
        .new_platform_object = vm_new,
        .invoke_platform = vm_invoke,
        .instance_of = view_instance_of,
        .validate_activity = validate_activity
    };
    BsDexVM *vm = bs_dex_vm_create(dex, size, &host);
    assert(vm);
    assert(bs_dex_vm_start_activity_oncreate(
        vm, &fixture->activity, bs_dex_null()) == 1);
    return vm;
}

static void assert_constructor_blocked(BsDexStop stop,
                                        FakeSurfaceService *fixture) {
    assert(stop.kind == BS_DEX_STOP_HOST_MISSING);
    assert(stop.code_off == 0x22e9d8);
    assert(stop.dex_pc == 2);
    assert(stop.host_class &&
           strcmp(stop.host_class, "Landroid/view/SurfaceView;") == 0);
    assert(stop.host_method && strcmp(stop.host_method, "<init>") == 0);
    assert(fixture->surface.observed_surface_constructors == 0);
    assert(fixture->callback_calls == 0);
}

static int invoke(BsDexAndroidSurfaceHost *host, BsDexVM *vm,
                  const char *cls, const char *name, const char *proto,
                  uint8_t opcode, BsDexValue *args, size_t count,
                  BsDexValue *result) {
    const char *exception = NULL;
    int rc = bs_dex_android_surface_invoke_platform(
        host, vm, cls, name, proto, opcode, args, count, result, &exception);
    assert(!exception);
    return rc;
}

static void test_missing_backend(const uint8_t *dex, size_t size) {
    FakeSurfaceService f;
    init_fake(&f, 0);
    BsDexVM *vm = new_vm(&f, dex, size);
    assert_constructor_blocked(bs_dex_vm_run(vm, 10000), &f);
    assert(!f.attached && f.attach_calls == 0);
    assert(bs_dex_android_surface_host_reset(&f.surface) == 1);
    bs_dex_vm_destroy(vm);
}

static void test_context_and_thread(const uint8_t *dex, size_t size) {
    for (int mode = 0; mode < 2; ++mode) {
        FakeSurfaceService f;
        init_fake(&f, 1);
        if (mode == 0) f.reject_context = 1;
        else f.ui_thread = 0;
        BsDexVM *vm = new_vm(&f, dex, size);
        assert_constructor_blocked(bs_dex_vm_run(vm, 10000), &f);
        assert(!f.attached && !f.surface_issued);
        bs_dex_vm_destroy(vm);
    }
}

static void test_mismatched_surface_and_rollback(
    const uint8_t *dex, size_t size) {
    FakeSurfaceService f;
    init_fake(&f, 1);
    f.rogue_backing = 1;
    BsDexVM *vm = new_vm(&f, dex, size);
    assert_constructor_blocked(bs_dex_vm_run(vm, 10000), &f);
    assert(f.attach_calls == 1 && f.detach_calls == 1);
    assert(f.revoke_calls == 1 && !f.attached && !f.surface_issued);
    bs_dex_vm_destroy(vm);
}

static void test_live_surface_holder_and_revocation(
    const uint8_t *dex, size_t size) {
    FakeSurfaceService f;
    init_fake(&f, 1);
    BsDexVM *vm = new_vm(&f, dex, size);
    BsDexStop stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.steps > 9);
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING ||
           stopped.kind == BS_DEX_STOP_OPCODE_MISSING);
    assert(stopped.code_off != 0x22e9d8 || stopped.dex_pc != 2);
    assert(f.surface.observed_surface_constructors == 1);
    assert(f.surface.view_count == 1);
    assert(f.attach_calls == 1 && f.attached && f.surface_issued);
    BsDexAndroidSurfaceView *view = &f.surface.views[0];
    assert(view->initialized && view->is_surface_view);
    assert(bs_dex_host_pointer(vm, view->receiver) == &f.java_view);
    assert(view->native_window == &f.nwindow);
    assert(view->jni_surface == &f.jni_surface);
    assert(view->width_px == 1280 && view->height_px == 720);
    assert(view->activity_context == &f.activity);
    assert(bs_dex_vm_current_context(vm).bits == 0);
    assert(bs_dex_vm_current_activity(vm).bits == 0);

    BsDexValue result = bs_dex_null();
    BsDexValue view_args[] = {view->receiver};
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceView;", "getHolder",
                  "()Landroid/view/SurfaceHolder;", 0x6e,
                  view_args, 1, &result) == 1);
    BsDexValue holder = result;
    assert(bs_dex_host_pointer(vm, holder) == &f.surface.holders[0]);
    assert(invoke(&f.surface, vm, "Landroid/view/View;", "getContext",
                  "()Landroid/content/Context;", 0x6e,
                  view_args, 1, &result) == 1);
    assert(result.bits == view->context.bits);
    BsDexValue holder_args[] = {holder, bs_dex_null()};
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceHolder;", "getSurface",
                  "()Landroid/view/Surface;", 0x72, holder_args,
                  1, &result) == 1);
    assert(bs_dex_host_pointer(vm, result) == &f.jni_surface);

    holder_args[1] = bs_dex_int(-3);
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceHolder;", "setFormat",
                  "(I)V", 0x72, holder_args, 2, &result) == 1);
    assert(f.format_calls == 1 && f.last_format == -3);
    BsDexValue zargs[] = {view->receiver, bs_dex_int(1)};
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceView;",
                  "setZOrderOnTop", "(Z)V", 0x6e, zargs, 2, &result) == 1);
    assert(f.zorder_calls == 1 && f.last_zorder == 1);

    BsDexValue wrong_callback = bs_dex_wrap_platform(
        vm, &f.activity, "Lcom/unity3d/player/UnityPlayerGameActivity;");
    holder_args[1] = wrong_callback;
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceHolder;",
                  "addCallback", "(Landroid/view/SurfaceHolder$Callback;)V",
                  0x72, holder_args, 2, &result) == 0);
    assert(f.callback_calls == 0);
    int player_fixture = 0;
    holder_args[1] = bs_dex_wrap_platform(
        vm, &player_fixture, "Lcom/unity3d/player/UnityPlayer;");
    assert(holder_args[1].bits);
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceHolder;",
                  "addCallback", "(Landroid/view/SurfaceHolder$Callback;)V",
                  0x72, holder_args, 2, &result) == 1);
    assert(f.callback_calls == 1 &&
           f.surface.views[0].callback_count == 1);

    /* No accessibility/layout support exists: reject instead of a
     * fabricated Java no-op success. */
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceView;",
                  "setContentDescription", "(Ljava/lang/CharSequence;)V",
                  0x6e, zargs, 2, &result) == 0);

    f.live_window = 0;
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceHolder;", "getSurface",
                  "()Landroid/view/Surface;", 0x72, holder_args,
                  1, &result) == 0);
    assert(invoke(&f.surface, vm, "Landroid/view/SurfaceView;", "getHolder",
                  "()Landroid/view/SurfaceHolder;", 0x6e, view_args,
                  1, &result) == 0);
    assert(bs_dex_android_surface_host_reset(&f.surface) == 1);
    assert(f.revoke_calls == 1 && f.detach_calls == 1);
    assert(f.surface.view_count == 0 && !f.attached && !f.surface_issued);
    bs_dex_vm_destroy(vm);
}

static void test_wrong_java_class(const uint8_t *dex, size_t size) {
    FakeSurfaceService f;
    init_fake(&f, 1);
    f.reject_java_class = 1;
    f.fail_remove = 1;
    BsDexVM *vm = new_vm(&f, dex, size);
    assert_constructor_blocked(bs_dex_vm_run(vm, 10000), &f);
    BsDexAndroidSurfaceView *view = &f.surface.views[0];
    assert(!view->initialized && view->detach_pending && f.attached);
    assert(bs_dex_host_pointer(vm, view->receiver) == NULL);
    assert(f.revoke_calls == 1 && !f.surface_issued);
    assert_constructor_blocked(bs_dex_vm_run(vm, 10000), &f);
    assert(f.attach_calls == 1);
    bs_dex_vm_destroy(vm);
    f.fail_remove = 0;
    assert(bs_dex_android_surface_host_reset(&f.surface));
    assert(f.detach_calls == 1 && !f.attached);
}

static void test_platform_view_constructor(const uint8_t *dex, size_t size) {
    FakeSurfaceService f;
    init_fake(&f, 1);
    BsDexVM *vm = new_vm(&f, dex, size);
    BsDexValue view = bs_dex_null();
    const char *exception = NULL;
    assert(bs_dex_android_surface_new_platform_object(
        &f.surface, vm, "Landroid/view/View;", &view, &exception) == 1);
    assert(view.bits && !exception);
    assert(!f.attached);
    BsDexValue context = bs_dex_wrap_platform(
        vm, &f.activity, "Lcom/unity3d/player/UnityPlayerGameActivity;");
    BsDexValue args[] = {view, context};
    BsDexValue result = bs_dex_null();
    assert(invoke(&f.surface, vm, "Landroid/view/View;", "<init>",
                  "(Landroid/content/Context;)V", 0x70,
                  args, 2, &result) == 1);
    assert(f.surface.observed_base_view_constructors == 1);
    assert(f.surface.views[0].initialized && !f.surface.views[0].jni_surface);
    assert(invoke(&f.surface, vm, "Landroid/view/View;", "getContext",
                  "()Landroid/content/Context;", 0x6e,
                  args, 1, &result) == 1);
    assert(result.bits == context.bits);
    assert(bs_dex_android_surface_host_reset(&f.surface) == 1);
    bs_dex_vm_destroy(vm);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t dex_size = 0;
    uint8_t *dex = load_dex(argv[1], &dex_size);
    test_missing_backend(dex, dex_size);
    test_context_and_thread(dex, dex_size);
    test_mismatched_surface_and_rollback(dex, dex_size);
    test_wrong_java_class(dex, dex_size);
    test_live_surface_holder_and_revocation(dex, dex_size);
    test_platform_view_constructor(dex, dex_size);
    free(dex);
    puts("PASS: original DEX SurfaceView constructor and backed View/SurfaceHolder"
         " behavior; real native services required; fail-closed tests");
    return 0;
}
