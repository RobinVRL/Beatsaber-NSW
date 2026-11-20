/* Isolated native/UI provider contracts. The fake registry below models
 * completed external calls; it is not an Android UI hierarchy on Switch. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../source/unity_dex_horizon_surface_view_backend.h"
#include "../source/unity_java_preinit.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fixture {
    int activity, parent, child_handles[2], child_windows[2], java_views[2];
    int jni_surface_tokens[2], jni_surface_issued[2], jni_revokes;
    int wrong_jni_roundtrip;
    int dynamic_dex_identity;
    int attached[2];
    uintptr_t identities[2];
    int ui_thread, activity_attached, parent_live, java_live;
    int reject_create, reject_verify, reject_remove, reject_verify_detached;
    int fake_parent_child, fake_shared_child, fake_java_type;
    int created, removed, callback_calls, format_calls, zorder_calls;
    int32_t last_format;
    int last_zorder;
} Fixture;

static int index_for(Fixture *f, uintptr_t identity) {
    if (identity == 1001) return 0;
    if (identity == 2002) return 1;
    if (f->dynamic_dex_identity && identity)
        return f->identities[0] == 0 || f->identities[0] == identity ? 0 : -1;
    return -1;
}

static int on_ui(void *user) { return ((Fixture *)user)->ui_thread; }
static int valid_activity(void *user, void *context) {
    Fixture *f = user;
    return f->activity_attached && context == &f->activity;
}
static int get_parent(void *user, void *context, void **out) {
    Fixture *f = user;
    if (!out || !valid_activity(user, context)) return 0;
    *out = &f->parent;
    return 1;
}
static int valid_parent(void *user, void *parent) {
    Fixture *f = user;
    return f->parent_live && parent == &f->parent;
}

static int create(void *user, void *context, void *parent,
                  uintptr_t identity, int is_surface,
                  void **child_handle, void **child_window) {
    Fixture *f = user;
    int i = index_for(f, identity);
    if (f->reject_create || i < 0 || f->attached[i] ||
        context != &f->activity || parent != &f->parent ||
        !child_handle || !child_window) return 0;
    f->attached[i] = 1;
    f->identities[i] = identity;
    *child_handle = &f->child_handles[i];
    *child_window = !is_surface ? NULL :
        f->fake_parent_child ? &f->parent :
        f->fake_shared_child ? &f->child_windows[0] :
        &f->child_windows[i];
    ++f->created;
    return 1;
}

static int verified_attached(void *user, void *context, void *parent,
                             uintptr_t identity, void *child,
                             void *window) {
    Fixture *f = user;
    int i = index_for(f, identity);
    if (i < 0 || f->reject_verify || !f->attached[i] ||
        f->identities[i] != identity || child != &f->child_handles[i] ||
        context != &f->activity || parent != &f->parent) return 0;
    return window == NULL || window == &f->child_windows[i] ||
           window == &f->parent || window == &f->child_windows[0];
}

static int remove_child(void *user, void *context, void *parent,
                        uintptr_t identity, void *child, void *window) {
    Fixture *f = user;
    int i = index_for(f, identity);
    (void)window;
    if (i < 0 || !f->attached[i] || f->reject_remove ||
        f->identities[i] != identity || child != &f->child_handles[i] ||
        context != &f->activity || parent != &f->parent) return 0;
    f->attached[i] = 0;
    ++f->removed;
    return 1;
}

static int verified_detached(void *user, void *context, void *parent,
                             uintptr_t identity, void *child,
                             void *window) {
    Fixture *f = user;
    int i = index_for(f, identity);
    (void)window;
    return i >= 0 && !f->reject_verify_detached && !f->attached[i] &&
        f->identities[i] == identity && child == &f->child_handles[i] &&
        context == &f->activity && parent == &f->parent;
}

static void *observed_java(void *user, uintptr_t identity, void *child) {
    Fixture *f = user;
    int i = index_for(f, identity);
    return i >= 0 && f->java_live && f->attached[i] &&
        child == &f->child_handles[i] ? &f->java_views[i] : NULL;
}

static int verified_java(void *user, uintptr_t identity, void *child,
                         void *java) {
    Fixture *f = user;
    int i = index_for(f, identity);
    return i >= 0 && !f->fake_java_type && f->java_live &&
        f->attached[i] && child == &f->child_handles[i] &&
        java == &f->java_views[i];
}

static int callback_type(void *user, BsDexVM *vm, BsDexValue callback) {
    (void)user;
    (void)vm;
    return callback.kind == BS_DEX_REF && callback.bits == 555;
}

static int add_callback(void *user, void *child, uintptr_t identity,
                        BsDexValue callback) {
    Fixture *f = user;
    int i = index_for(f, identity);
    if (i < 0 || !f->attached[i] || child != &f->child_handles[i] ||
        callback.bits != 555) return 0;
    ++f->callback_calls;
    return 1;
}

static int set_format(void *user, void *child, uintptr_t identity,
                      int32_t format) {
    Fixture *f = user;
    int i = index_for(f, identity);
    if (i < 0 || !f->attached[i] || child != &f->child_handles[i]) return 0;
    ++f->format_calls;
    f->last_format = format;
    return 1;
}

static int set_zorder(void *user, void *child, uintptr_t identity, int top) {
    Fixture *f = user;
    int i = index_for(f, identity);
    if (i < 0 || !f->attached[i] || child != &f->child_handles[i]) return 0;
    ++f->zorder_calls;
    f->last_zorder = top;
    return 1;
}

static void configure(Fixture *f, BsDexHorizonSurfaceViewBackend *backend,
                      int java_provider) {
    memset(f, 0, sizeof *f);
    f->ui_thread = f->activity_attached = f->parent_live = f->java_live = 1;
    BsDexHorizonSurfaceViewRootHooks root = {
        .user = f, .on_ui_thread = on_ui,
        .matches_attached_activity = valid_activity,
        .get_parent_window = get_parent,
        .valid_parent_window = valid_parent
    };
    BsDexHorizonSurfaceViewChildOps ops = {
        .user = f, .create = create, .verify_attached = verified_attached,
        .remove = remove_child, .verify_detached = verified_detached,
        .observed_java_view = java_provider ? observed_java : NULL,
        .verify_java_view = java_provider ? verified_java : NULL,
        .is_holder_callback = callback_type,
        .register_holder_callback = add_callback,
        .set_holder_format = set_format,
        .set_z_order_on_top = set_zorder
    };
    assert(bs_dex_horizon_surface_view_backend_init_with_hooks(
        backend, &root, &ops));
}

/* Production root requires the actual, live original Activity, regardless of
 * whether a future child provider is installed. */
int bs_dex_horizon_activity_on_ui_thread(void *user) {
    (void)user;
    return 0;
}
int bs_dex_horizon_activity_context_matches(void *user, void *context) {
    (void)user; (void)context;
    return 0;
}
int bs_dex_horizon_services_on_applet_thread(const BsDexHorizonServices *s) {
    (void)s;
    return 0;
}
int bs_dex_horizon_services_valid_nwindow(const BsDexHorizonServices *s,
                                          void *window) {
    (void)s; (void)window;
    return 0;
}

static void test_no_factory(void) {
    BsDexHorizonActivityHost activity = {0};
    BsDexHorizonServices horizon = {0};
    BsDexHorizonSurfaceViewBackend b;
    assert(bs_dex_horizon_surface_view_backend_init(&b, &activity, &horizon,
                                                    NULL));
    BsDexAndroidSurfaceServices svc = bs_dex_horizon_surface_view_services(&b);
    void *window = (void *)(uintptr_t)42;
    assert(!svc.context_matches_activity(svc.user, &activity.activity));
    assert(!svc.construct_view(svc.user, &activity.activity, 1001, 1, &window));
    assert(window == NULL);
    assert(!b.children[0].active);
    assert(bs_dex_horizon_surface_view_backend_reset(&b));
}

static void test_no_java_still_native(void) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend b;
    configure(&f, &b, 0);
    BsDexAndroidSurfaceServices svc = bs_dex_horizon_surface_view_services(&b);
    void *window = NULL;
    assert(!svc.construct_view(svc.user, &f.activity, 1001, 1, &window));
    assert(f.created == 0);
    assert(bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    assert(window == &f.child_windows[0]);
    assert(bs_dex_horizon_surface_view_validate_native(&b, 1001, window));
    assert(!bs_dex_horizon_surface_view_validate_native(
        &b, 1001, &f.parent));
    assert(!svc.constructed_java_view(svc.user, 1001));
    assert(!svc.set_holder_format(svc.user, 1001, 7));
    assert(bs_dex_horizon_surface_view_remove_native(&b, 1001, window));
    assert(!bs_dex_horizon_surface_view_validate_native(&b, 1001, window));
    assert(f.created == 1 && f.removed == 1);
}

static void test_with_java_provider(void) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend b;
    configure(&f, &b, 1);
    BsDexAndroidSurfaceServices svc = bs_dex_horizon_surface_view_services(&b);
    void *window = NULL;
    assert(svc.construct_view(svc.user, &f.activity, 1001, 1, &window));
    assert(window == &f.child_windows[0] && window != &f.parent);
    assert(svc.constructed_java_view(svc.user, 1001) == &f.java_views[0]);
    BsDexValue callback = {0};
    callback.kind = BS_DEX_REF;
    callback.bits = 555;
    assert(svc.is_holder_callback(svc.user, NULL, callback));
    assert(svc.register_holder_callback(svc.user, 1001, callback));
    assert(svc.set_holder_format(svc.user, 1001, -3));
    assert(svc.set_z_order_on_top(svc.user, 1001, 1));
    assert(f.callback_calls == 1 && f.format_calls == 1 &&
           f.last_format == -3 && f.zorder_calls == 1 && f.last_zorder == 1);
    assert(!svc.construct_view(svc.user, &f.activity, 1001, 1, &window));
    assert(f.created == 1);

    /* Applet event invalidation prevents use even before native detachment. */
    f.ui_thread = 0;
    assert(!svc.constructed_java_view(svc.user, 1001));
    assert(!svc.set_holder_format(svc.user, 1001, 1));
    assert(!svc.remove_view(svc.user, 1001, &f.child_windows[0]));
    f.ui_thread = 1;
    f.activity_attached = 0; /* Native teardown still permitted on UI thread. */
    assert(svc.remove_view(svc.user, 1001, &f.child_windows[0]));
    assert(!svc.constructed_java_view(svc.user, 1001));
    assert(f.removed == 1);
    assert(bs_dex_horizon_surface_view_backend_reset(&b));
}

static void test_reject_parent_alias_and_duplicate(void) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend b;
    configure(&f, &b, 1);
    void *window = NULL;
    f.fake_parent_child = 1;
    assert(!bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    assert(!f.attached[0] && f.removed == 1);
    f.fake_parent_child = 0;
    assert(bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    f.fake_shared_child = 1;
    assert(!bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 2002, 1, &window));
    assert(!f.attached[1] && f.attached[0]);
    assert(bs_dex_horizon_surface_view_backend_reset(&b));
    assert(f.removed == 3);
}

static void test_retry_removal_and_failed_java(void) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend b;
    configure(&f, &b, 1);
    BsDexAndroidSurfaceServices svc = bs_dex_horizon_surface_view_services(&b);
    void *window = NULL;
    f.fake_java_type = 1;
    assert(!svc.construct_view(svc.user, &f.activity, 1001, 1, &window));
    assert(f.created == 1 && f.removed == 1 && !f.attached[0]);
    f.fake_java_type = 0;
    assert(svc.construct_view(svc.user, &f.activity, 1001, 1, &window));

    f.reject_remove = 1;
    assert(!svc.remove_view(svc.user, 1001, window));
    assert(!svc.constructed_java_view(svc.user, 1001));
    assert(f.attached[0]);
    f.reject_remove = 0;
    f.reject_verify_detached = 1;
    assert(!bs_dex_horizon_surface_view_backend_reset(&b));
    assert(!f.attached[0] && f.removed == 2);
    f.reject_verify_detached = 0;
    assert(bs_dex_horizon_surface_view_backend_reset(&b));
    assert(f.removed == 2); /* Idempotent observation, no second removal. */
}

static void test_reject_invalid_root_and_missing_provider(void) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend b;
    configure(&f, &b, 1);
    void *window = NULL;
    f.activity_attached = 0;
    assert(!bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    f.activity_attached = 1;
    f.parent_live = 0;
    assert(!bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    f.parent_live = 1;
    f.reject_verify = 1;
    assert(!bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 1, &window));
    assert(f.removed == 1);
    f.reject_verify = 0;
    assert(bs_dex_horizon_surface_view_register_native(
        &b, &f.activity, 1001, 0, &window));
    assert(window == NULL);
    assert(bs_dex_horizon_surface_view_validate_native(&b, 1001, NULL));
    assert(bs_dex_horizon_surface_view_backend_reset(&b));
}

/* The real SurfaceHost supplies the original DEX method dispatch and checks
 * the exact native-window/JNI-Surface roundtrip. Only physical JNI calls are
 * substituted here, with two separate tokens and verifiable child windows. */
static Fixture *jni_fixture(void *user) {
    BsDexHorizonSurfaceViewBackend *backend = user;
    return backend ? backend->child_ops.user : NULL;
}
static int mock_dimensions(void *user, void *window,
                           uint32_t *width, uint32_t *height) {
    Fixture *f = jni_fixture(user);
    if (!f || !width || !height) return 0;
    for (int i = 0; i < 2; ++i) {
        if (f->attached[i] && window == &f->child_windows[i]) {
            *width = 1280;
            *height = 720;
            return 1;
        }
    }
    return 0;
}
static void *mock_surface(void *user, void *window) {
    Fixture *f = jni_fixture(user);
    if (!f) return NULL;
    for (int i = 0; i < 2; ++i) {
        if (f->attached[i] && window == &f->child_windows[i] &&
            !f->jni_surface_issued[i]) {
            f->jni_surface_issued[i] = 1;
            return &f->jni_surface_tokens[i];
        }
    }
    return NULL;
}
static void *mock_verified_window(void *user, void *surface) {
    Fixture *f = jni_fixture(user);
    if (!f) return NULL;
    for (int i = 0; i < 2; ++i)
        if (f->jni_surface_issued[i] && surface == &f->jni_surface_tokens[i])
            return f->wrong_jni_roundtrip ? &f->parent :
                &f->child_windows[i];
    return NULL;
}
static void mock_revoke(void *user, void *surface) {
    Fixture *f = jni_fixture(user);
    assert(f);
    for (int i = 0; i < 2; ++i) {
        if (surface == &f->jni_surface_tokens[i]) {
            assert(f->jni_surface_issued[i]);
            f->jni_surface_issued[i] = 0;
            ++f->jni_revokes;
            return;
        }
    }
    assert(!"Unknown JNI Surface token");
}

typedef struct VMHarness {
    Fixture *fixture;
    BsDexAndroidSurfaceHost surface;
} VMHarness;

static int vm_new(void *user, BsDexVM *vm, const char *klass,
                  BsDexValue *result, const char **exception) {
    return bs_dex_android_surface_new_platform_object(
        &((VMHarness *)user)->surface, vm, klass, result, exception);
}
static int vm_invoke(void *user, BsDexVM *vm, const char *klass,
                     const char *name, const char *prototype, uint8_t opcode,
                     const BsDexValue *args, size_t count, BsDexValue *result,
                     const char **exception) {
    return bs_dex_android_surface_invoke_platform(
        &((VMHarness *)user)->surface, vm, klass, name, prototype,
        opcode, args, count, result, exception);
}
static int vm_instanceof(void *user, BsDexVM *vm, BsDexValue value,
                         const char *descriptor, int *matches,
                         const char **exception) {
    VMHarness *h = user;
    Fixture *f = h->fixture;
    if (exception) *exception = NULL;
    if (!matches) return 0;
    *matches = !f->fake_java_type &&
        strcmp(descriptor,
            "Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;") == 0 &&
        bs_dex_host_pointer(vm, value) == &f->java_views[0];
    return 1;
}
static int vm_activity(void *user, void *activity) {
    return activity == &((VMHarness *)user)->fixture->activity;
}

static uint8_t *read_dex(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length == BS_UNITY_JAVA_DEX_SIZE);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)length);
    assert(bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    assert(fclose(file) == 0);
    *size = (size_t)length;
    return bytes;
}

static void test_actual_original_dex(const uint8_t *dex, size_t size,
                                     int wrong_roundtrip) {
    Fixture f;
    BsDexHorizonSurfaceViewBackend backend;
    configure(&f, &backend, 1);
    f.dynamic_dex_identity = 1;
    f.wrong_jni_roundtrip = wrong_roundtrip;
    BsDexAndroidSurfaceServices svc =
        bs_dex_horizon_surface_view_services(&backend);
    svc.get_window_dimensions = mock_dimensions;
    svc.surface_from_nwindow = mock_surface;
    svc.verified_nwindow_from_surface = mock_verified_window;
    svc.revoke_surface = mock_revoke;

    VMHarness h = {.fixture = &f};
    bs_dex_android_surface_host_init(&h.surface, &svc);
    BsDexHost guest = {.user = &h, .new_platform_object = vm_new,
        .invoke_platform = vm_invoke, .instance_of = vm_instanceof,
        .validate_activity = vm_activity};
    BsDexVM *vm = bs_dex_vm_create(dex, size, &guest);
    assert(vm);
    assert(bs_dex_vm_start_activity_oncreate(vm, &f.activity, bs_dex_null()));
    BsDexStop stop = bs_dex_vm_run(vm, 10000);
    if (wrong_roundtrip) {
        assert(stop.kind == BS_DEX_STOP_HOST_MISSING);
        assert(stop.code_off == 0x22e9d8 && stop.dex_pc == 2);
        assert(f.created == 1 && f.removed == 1 && f.jni_revokes == 1);
        assert(!f.attached[0] && !f.jni_surface_issued[0]);
        assert(h.surface.observed_surface_constructors == 0);
    } else {
        assert(stop.steps > 9);
        assert(stop.code_off != 0x22e9d8 || stop.dex_pc != 2);
        assert(h.surface.observed_surface_constructors == 1);
        assert(h.surface.views[0].jni_surface == &f.jni_surface_tokens[0]);
        assert(h.surface.views[0].native_window == &f.child_windows[0]);
        assert(bs_dex_host_pointer(vm, h.surface.views[0].receiver) ==
               &f.java_views[0]);
        assert(bs_dex_vm_current_activity(vm).bits == 0);
    }
    bs_dex_vm_destroy(vm);
    assert(bs_dex_android_surface_host_reset(&h.surface));
    assert(bs_dex_horizon_surface_view_backend_reset(&backend));
    assert(!f.attached[0] && !f.jni_surface_issued[0]);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_no_factory();
    test_no_java_still_native();
    test_with_java_provider();
    test_reject_parent_alias_and_duplicate();
    test_retry_removal_and_failed_java();
    test_reject_invalid_root_and_missing_provider();
    size_t dex_size = 0;
    uint8_t *dex = read_dex(argv[1], &dex_size);
    test_actual_original_dex(dex, dex_size, 0);
    test_actual_original_dex(dex, dex_size, 1);
    free(dex);
    puts("PASS: Horizon child registration, UI/Java gating, format/callback, "
         "rollback, original DEX and JNI Surface roundtrip");
    return 0;
}
