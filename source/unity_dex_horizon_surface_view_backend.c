#include "unity_dex_horizon_surface_view_backend.h"

#include <string.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

static int production_ui_thread(void *user) {
    BsDexHorizonSurfaceViewBackend *b = user;
    return b && b->activity && b->horizon &&
           bs_dex_horizon_activity_on_ui_thread(b->activity) == 1 &&
           bs_dex_horizon_services_on_applet_thread(b->horizon) == 1;
}

static int production_matches(void *user, void *context) {
    BsDexHorizonSurfaceViewBackend *b = user;
    return b && b->activity && context == &b->activity->activity &&
           bs_dex_horizon_activity_context_matches(b->activity, context) == 1;
}

static int production_parent(void *user, void *context, void **parent) {
    BsDexHorizonSurfaceViewBackend *b = user;
    if (parent) *parent = NULL;
    if (!b || !parent || !production_matches(b, context) ||
        !b->horizon || !b->activity->activity.window ||
        b->activity->activity.window != b->horizon->observed.nwindow ||
        !bs_dex_horizon_services_valid_nwindow(
            b->horizon, b->activity->activity.window)) return 0;
    *parent = b->activity->activity.window;
    return 1;
}

static int production_valid_parent(void *user, void *window) {
    BsDexHorizonSurfaceViewBackend *b = user;
    return b && b->horizon && window &&
           bs_dex_horizon_services_valid_nwindow(b->horizon, window) == 1;
}

int bs_dex_horizon_surface_view_backend_init(
    BsDexHorizonSurfaceViewBackend *b,
    BsDexHorizonActivityHost *activity, BsDexHorizonServices *horizon,
    const BsDexHorizonSurfaceViewChildOps *ops) {
    if (!b || !activity || !horizon) return 0;
    memset(b, 0, sizeof *b);
    b->activity = activity;
    b->horizon = horizon;
    b->production = 1;
    b->root.user = b;
    b->root.on_ui_thread = production_ui_thread;
    b->root.matches_attached_activity = production_matches;
    b->root.get_parent_window = production_parent;
    b->root.valid_parent_window = production_valid_parent;
    if (ops) b->child_ops = *ops;
    return 1;
}

int bs_dex_horizon_surface_view_backend_init_with_hooks(
    BsDexHorizonSurfaceViewBackend *b,
    const BsDexHorizonSurfaceViewRootHooks *root,
    const BsDexHorizonSurfaceViewChildOps *ops) {
    if (!b || !root || !root->on_ui_thread ||
        !root->matches_attached_activity || !root->get_parent_window ||
        !root->valid_parent_window) return 0;
    memset(b, 0, sizeof *b);
    b->root = *root;
    if (ops) b->child_ops = *ops;
    return 1;
}

static int ui_ready(BsDexHorizonSurfaceViewBackend *b) {
    return b && b->root.on_ui_thread &&
           b->root.on_ui_thread(b->root.user) == 1;
}

static int current_parent(BsDexHorizonSurfaceViewBackend *b,
                          void *context, void **parent) {
    if (parent) *parent = NULL;
    if (!b || !context || !parent || !ui_ready(b) ||
        !b->root.matches_attached_activity ||
        b->root.matches_attached_activity(b->root.user, context) != 1 ||
        !b->root.get_parent_window ||
        b->root.get_parent_window(b->root.user, context, parent) != 1 ||
        !*parent || !b->root.valid_parent_window ||
        b->root.valid_parent_window(b->root.user, *parent) != 1) {
        if (parent) *parent = NULL;
        return 0;
    }
    return 1;
}

static BsDexHorizonSurfaceViewChild *lookup(
    BsDexHorizonSurfaceViewBackend *b, uintptr_t identity) {
    if (!b || !identity) return NULL;
    for (size_t i = 0; i < BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN; ++i)
        if (b->children[i].active && b->children[i].identity == identity)
            return &b->children[i];
    return NULL;
}

static int native_valid(BsDexHorizonSurfaceViewBackend *b,
                        BsDexHorizonSurfaceViewChild *v) {
    void *parent = NULL;
    if (!v || !v->active || v->removal_pending || !v->child_handle ||
        !current_parent(b, v->context, &parent) ||
        parent != v->parent_window ||
        v->child_handle == parent ||
        (v->is_surface_view ? (!v->child_nwindow ||
            v->child_nwindow == parent) : !!v->child_nwindow) ||
        !b->child_ops.verify_attached) return 0;
#ifdef __SWITCH__
    if (v->is_surface_view) {
        u32 width = 0, height = 0;
        NWindow *window = (NWindow *)v->child_nwindow;
        if (!nwindowIsValid(window) ||
            R_FAILED(nwindowGetDimensions(window, &width, &height)) ||
            !width || !height) return 0;
    }
#endif
    for (size_t i = 0; i < BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN; ++i) {
        BsDexHorizonSurfaceViewChild *other = &b->children[i];
        /* A failed/pending detach still retains real native ownership. Never
         * reuse that child's window or handle until detachment is verified. */
        if (other == v || !other->active) continue;
        if (other->child_handle == v->child_handle ||
            (v->child_nwindow && other->child_nwindow == v->child_nwindow))
            return 0;
    }
    return b->child_ops.verify_attached(b->child_ops.user, v->context,
        v->parent_window, v->identity, v->child_handle,
        v->child_nwindow) == 1;
}

int bs_dex_horizon_surface_view_validate_native(
    BsDexHorizonSurfaceViewBackend *b, uintptr_t identity, void *window) {
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    return v && v->child_nwindow == window && native_valid(b, v);
}

/* Even if the UI provider reports success, a failed independent detach
 * observation leaves ownership retained for retry, never silently forgotten.
 * Cleanup is permitted after Activity invalidation, on the original UI thread. */
static int remove_record(BsDexHorizonSurfaceViewBackend *b,
                         BsDexHorizonSurfaceViewChild *v) {
    if (!b || !v || !v->active || !ui_ready(b) ||
        !b->child_ops.remove || !b->child_ops.verify_detached) return 0;
    v->removal_pending = 1;
    if (!v->remove_completed) {
        if (b->child_ops.remove(b->child_ops.user, v->context,
               v->parent_window, v->identity, v->child_handle,
               v->child_nwindow) != 1) return 0;
        v->remove_completed = 1;
    }
    if (b->child_ops.verify_detached(b->child_ops.user, v->context,
            v->parent_window, v->identity, v->child_handle,
            v->child_nwindow) != 1) return 0;
    memset(v, 0, sizeof *v);
    return 1;
}

int bs_dex_horizon_surface_view_remove_native(
    BsDexHorizonSurfaceViewBackend *b, uintptr_t identity, void *window) {
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    if (!v || v->child_nwindow != window) return 0;
    return remove_record(b, v);
}

int bs_dex_horizon_surface_view_backend_reset(
    BsDexHorizonSurfaceViewBackend *b) {
    if (!b) return 0;
    int complete = 1;
    for (size_t i = BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN; i > 0; --i)
        if (b->children[i - 1].active &&
            !remove_record(b, &b->children[i - 1])) complete = 0;
    return complete;
}

int bs_dex_horizon_surface_view_register_native(
    BsDexHorizonSurfaceViewBackend *b, void *context,
    uintptr_t identity, int is_surface, void **observed_window) {
    if (observed_window) *observed_window = NULL;
    if (!b || !observed_window || !identity ||
        (is_surface != 0 && is_surface != 1) || lookup(b, identity)) return 0;
    BsDexHorizonSurfaceViewChildOps *ops = &b->child_ops;
    if (!ops->create || !ops->verify_attached || !ops->remove ||
        !ops->verify_detached) return 0;
    void *parent = NULL;
    if (!current_parent(b, context, &parent)) return 0;

    size_t free_index = BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN;
    for (size_t i = 0; i < BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN; ++i)
        if (!b->children[i].active) { free_index = i; break; }
    if (free_index == BS_HORIZON_SURFACE_VIEW_MAX_CHILDREN) return 0;

    void *child_handle = NULL, *window = NULL;
    if (ops->create(ops->user, context, parent, identity, is_surface,
                    &child_handle, &window) != 1) return 0;
    BsDexHorizonSurfaceViewChild *v = &b->children[free_index];
    *v = (BsDexHorizonSurfaceViewChild){
        .identity = identity, .context = context, .parent_window = parent,
        .child_handle = child_handle, .child_nwindow = window,
        .active = 1, .is_surface_view = (unsigned)is_surface
    };
    if (!native_valid(b, v)) {
        (void)remove_record(b, v);
        return 0;
    }
    *observed_window = window;
    return 1;
}

static int callback_ui(void *user) {
    return ui_ready(user);
}

static int callback_context(void *user, void *context) {
    void *parent = NULL;
    return current_parent(user, context, &parent);
}

static int callback_construct(void *user, void *context, uintptr_t identity,
                              int is_surface, void **observed_window) {
    BsDexHorizonSurfaceViewBackend *b = user;
    if (!observed_window) return 0;
    *observed_window = NULL;
    if (!b || !b->child_ops.observed_java_view ||
        !b->child_ops.verify_java_view) return 0;
    void *window = NULL;
    if (!bs_dex_horizon_surface_view_register_native(b, context, identity,
                                                     is_surface, &window))
        return 0;
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    if (!v) return 0;
    void *java = b->child_ops.observed_java_view(
        b->child_ops.user, identity, v->child_handle);
    if (!java || b->child_ops.verify_java_view(
            b->child_ops.user, identity, v->child_handle, java) != 1) {
        (void)remove_record(b, v);
        return 0;
    }
    v->java_view = java;
    *observed_window = window;
    return 1;
}

static int callback_remove(void *user, uintptr_t identity, void *window) {
    return bs_dex_horizon_surface_view_remove_native(user, identity, window);
}

static void *callback_java_view(void *user, uintptr_t identity) {
    BsDexHorizonSurfaceViewBackend *b = user;
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    if (!v || !v->java_view || !native_valid(b, v) ||
        !b->child_ops.verify_java_view ||
        b->child_ops.verify_java_view(b->child_ops.user, identity,
            v->child_handle, v->java_view) != 1) return NULL;
    return v->java_view;
}

static int callback_is_holder(void *user, BsDexVM *vm, BsDexValue value) {
    BsDexHorizonSurfaceViewBackend *b = user;
    return ui_ready(b) && b->child_ops.is_holder_callback &&
        b->child_ops.is_holder_callback(b->child_ops.user, vm, value) == 1;
}

static int callback_register_holder(void *user, uintptr_t identity,
                                    BsDexValue value) {
    BsDexHorizonSurfaceViewBackend *b = user;
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    return native_valid(b, v) && v->is_surface_view &&
        callback_java_view(b, identity) &&
        b->child_ops.register_holder_callback &&
        b->child_ops.register_holder_callback(b->child_ops.user,
            v->child_handle, identity, value) == 1;
}

static int callback_format(void *user, uintptr_t identity, int32_t format) {
    BsDexHorizonSurfaceViewBackend *b = user;
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    return native_valid(b, v) && v->is_surface_view &&
        callback_java_view(b, identity) && b->child_ops.set_holder_format &&
        b->child_ops.set_holder_format(b->child_ops.user,
            v->child_handle, identity, format) == 1;
}

static int callback_z_order(void *user, uintptr_t identity, int on_top) {
    BsDexHorizonSurfaceViewBackend *b = user;
    BsDexHorizonSurfaceViewChild *v = lookup(b, identity);
    return native_valid(b, v) && v->is_surface_view &&
        callback_java_view(b, identity) && b->child_ops.set_z_order_on_top &&
        b->child_ops.set_z_order_on_top(b->child_ops.user,
            v->child_handle, identity, on_top) == 1;
}

BsDexAndroidSurfaceServices bs_dex_horizon_surface_view_services(
    BsDexHorizonSurfaceViewBackend *b) {
    BsDexAndroidSurfaceServices result = {0};
    if (!b) return result;
    result.user = b;
    result.context_matches_activity = callback_context;
    result.on_ui_thread = callback_ui;
    result.construct_view = callback_construct;
    result.remove_view = callback_remove;
    result.get_window_dimensions = bs_dex_android_horizon_window_dimensions;
    result.surface_from_nwindow = bs_dex_android_jni_surface_from_nwindow;
    result.verified_nwindow_from_surface = bs_dex_android_jni_verified_window;
    result.revoke_surface = bs_dex_android_jni_revoke_surface;
    result.is_holder_callback = callback_is_holder;
    result.register_holder_callback = callback_register_holder;
    result.set_holder_format = callback_format;
    result.set_z_order_on_top = callback_z_order;
    result.constructed_java_view = callback_java_view;
    return result;
}
