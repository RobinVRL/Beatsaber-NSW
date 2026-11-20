#include "unity_dex_android_surface_host.h"

#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#include "jni_bridge.h"
#endif

/* These descriptors and signatures are present in the pinned Quest 1.46
 * classes.dex. The concrete receiver at code_item 0x22E9D8 PC2 is a
 * DEX-owned GameActivity$InputEnabledSurfaceView, not a platform JNI object. */
#define BS_SURFACE_VIEW "Landroid/view/SurfaceView;"
#define BS_SURFACE_HOLDER "Landroid/view/SurfaceHolder;"
#define BS_SURFACE "Landroid/view/Surface;"
#define BS_VIEW "Landroid/view/View;"
#define BS_INPUT_SURFACE_VIEW \
    "Lcom/google/androidgamesdk/GameActivity$InputEnabledSurfaceView;"
/* Original GameActivity.createSurfaceView instantiates this concrete class,
 * whose constructor invokes InputEnabledSurfaceView.<init> then the Android
 * SurfaceView base constructor. Keep the two original DEX classes distinct. */
#define BS_GAME_ACTIVITY_SURFACE_VIEW \
    "Lcom/google/androidgamesdk/GameActivity$GameActivitySurfaceView;"
/* The original Quest UnityPlayerGameActivity override instantiates this
 * concrete subclass, observed in the actual DEX interpreter frame. */
#define BS_UNITY_GAME_ACTIVITY_SURFACE_VIEW \
    "Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;"

static int same(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

void bs_dex_android_surface_host_init(BsDexAndroidSurfaceHost *host,
                                      const BsDexAndroidSurfaceServices *services) {
    if (!host) return;
    memset(host, 0, sizeof *host);
    if (services) host->services = *services;
    for (size_t i = 0; i < BS_DEX_ANDROID_MAX_VIEWS; ++i) {
        host->holders[i].owner = host;
        host->holders[i].view = &host->views[i];
    }
}

/* The caller destroys the DEX VM before resetting this host so that its
 * external references are no longer live. Removing a view is a real side
 * effect, and failure must not claim the native view was detached. */
int bs_dex_android_surface_host_reset(BsDexAndroidSurfaceHost *host) {
    if (!host) return 0;
    int complete = 1;
    for (size_t i = host->view_count; i > 0; --i) {
        BsDexAndroidSurfaceView *view = &host->views[i - 1];
        if (!view->initialized && !view->detach_pending) continue;
        if (!host->services.remove_view ||
            host->services.remove_view(host->services.user,
                    view->receiver.bits, view->native_window) != 1) {
            complete = 0;
            continue;
        }
        if (view->jni_surface && host->services.revoke_surface)
            host->services.revoke_surface(host->services.user, view->jni_surface);
        memset(view, 0, sizeof *view);
    }
    if (complete) {
        BsDexAndroidSurfaceServices retained = host->services;
        bs_dex_android_surface_host_init(host, &retained);
    }
    return complete;
}

static int bind_vm(BsDexAndroidSurfaceHost *host, BsDexVM *vm) {
    if (!host || !vm) return 0;
    if (host->attached_vm && host->attached_vm != vm) return 0;
    host->attached_vm = vm;
    return 1;
}

static BsDexAndroidSurfaceView *view_for(
    BsDexAndroidSurfaceHost *host, BsDexValue receiver) {
    if (!host || receiver.kind != BS_DEX_REF || !receiver.bits) return NULL;
    for (size_t i = 0; i < host->view_count; ++i)
        if (host->views[i].receiver.bits == receiver.bits)
            return &host->views[i];
    return NULL;
}

static BsDexAndroidSurfaceView *allocate_view(
    BsDexAndroidSurfaceHost *host, BsDexValue receiver, int is_surface_view) {
    if (!host || host->view_count >= BS_DEX_ANDROID_MAX_VIEWS) return NULL;
    BsDexAndroidSurfaceView *view = &host->views[host->view_count++];
    memset(view, 0, sizeof *view);
    view->receiver = receiver;
    view->reserved = 1;
    view->is_surface_view = !!is_surface_view;
    return view;
}

int bs_dex_android_surface_new_platform_object(
    void *user, BsDexVM *vm, const char *class_descriptor,
    BsDexValue *actual_object, const char **exception) {
    BsDexAndroidSurfaceHost *host = user;
    if (exception) *exception = NULL;
    if (!actual_object || !bind_vm(host, vm)) return 0;
    if (!same(class_descriptor, BS_SURFACE_VIEW) &&
        !same(class_descriptor, BS_VIEW)) return 0;
    /* new-instance is only allocation. The real constructor does the
     * observable registration and surface creation later. */
    if (host->view_count >= BS_DEX_ANDROID_MAX_VIEWS) return 0;
    size_t index = host->view_count;
    BsDexAndroidSurfaceView *view = &host->views[index];
    memset(view, 0, sizeof *view);
    view->reserved = 1;
    view->is_surface_view = same(class_descriptor, BS_SURFACE_VIEW);
    BsDexValue ref = bs_dex_wrap_platform(vm, view, class_descriptor);
    if (ref.kind != BS_DEX_REF || !ref.bits) {
        memset(view, 0, sizeof *view);
        return 0;
    }
    view->receiver = ref;
    ++host->view_count;
    *actual_object = ref;
    return 1;
}

static int verify_view_surface(BsDexAndroidSurfaceHost *host,
                               BsDexAndroidSurfaceView *view) {
    if (!view || !view->initialized || !view->is_surface_view ||
        !view->jni_surface || !view->native_window ||
        !host->services.verified_nwindow_from_surface) return 0;
    return host->services.verified_nwindow_from_surface(
               host->services.user, view->jni_surface) == view->native_window;
}

/* A base View can exist without a Surface. SurfaceView initialization is
 * stronger: native child view attachment, a real NWindow, measured dimensions,
 * a distinct JNI Java Surface token and exact token->NWindow round trip.
 * No Android Surface may be fabricated from a raw pointer or non-window. */
static int construct_view(BsDexAndroidSurfaceHost *host, BsDexVM *vm,
                          const char *declaring_class, const BsDexValue *args,
                          size_t count) {
    if (count != 2 || !host || !vm || !args ||
        args[0].kind != BS_DEX_REF || !args[0].bits ||
        args[1].kind != BS_DEX_REF || !args[1].bits) return 0;
    const char *runtime_type = bs_dex_reference_type(vm, args[0]);
    const int requested_surface = same(declaring_class, BS_SURFACE_VIEW);
    const int requires_binding = !bs_dex_host_pointer(vm, args[0]);
    if (!same(runtime_type, BS_INPUT_SURFACE_VIEW) &&
        !same(runtime_type, BS_GAME_ACTIVITY_SURFACE_VIEW) &&
        !same(runtime_type, BS_UNITY_GAME_ACTIVITY_SURFACE_VIEW) &&
        !same(runtime_type, BS_SURFACE_VIEW) &&
        !same(runtime_type, BS_VIEW)) return 0;
    if (requested_surface && same(runtime_type, BS_VIEW)) return 0;
    if (!requested_surface && !same(runtime_type, BS_VIEW)) {
        /* Calling View.<init> directly on a DEX-owned SurfaceView does not
         * establish its separate child SurfaceView's native state. */
        return 0;
    }
    void *context = bs_dex_host_pointer(vm, args[1]);
    BsDexAndroidSurfaceServices *svc = &host->services;
    if (!context || !svc->context_matches_activity ||
        !svc->on_ui_thread || !svc->construct_view ||
        !svc->remove_view || svc->context_matches_activity(svc->user, context) != 1 ||
        svc->on_ui_thread(svc->user) != 1) return 0;
    if (requires_binding && !svc->constructed_java_view) return 0;
    if (requested_surface &&
        (!svc->get_window_dimensions || !svc->surface_from_nwindow ||
         !svc->verified_nwindow_from_surface || !svc->revoke_surface))
        return 0;

    BsDexAndroidSurfaceView *view = view_for(host, args[0]);
    int allocated = 0;
    if (!view) {
        view = allocate_view(host, args[0], requested_surface);
        allocated = 1;
    }
    if (!view || view->initialized || view->detach_pending || !view->reserved ||
        !!view->is_surface_view != requested_surface) return 0;

    void *window = NULL;
    void *surface = NULL;
    uint32_t width = 0, height = 0;
    if (svc->construct_view(svc->user, context, args[0].bits,
                            requested_surface, &window) != 1) goto discard;
    if (requested_surface) {
        if (window && svc->get_window_dimensions(
                    svc->user, window, &width, &height) == 1 &&
            width && height) {
            surface = svc->surface_from_nwindow(svc->user, window);
        }
        if (!surface || svc->verified_nwindow_from_surface(
                            svc->user, surface) != window) goto rollback;
    } else if (window) {
        goto rollback;
    }
    if (requires_binding &&
        !bs_dex_bind_original_object(vm, args[0],
            svc->constructed_java_view(svc->user, args[0].bits))) {
        goto rollback;
    }
    view->context = args[1];
    view->activity_context = context;
    view->native_window = window;
    view->jni_surface = surface;
    view->width_px = width;
    view->height_px = height;
    view->initialized = 1;
    if (requested_surface) ++host->observed_surface_constructors;
    else ++host->observed_base_view_constructors;
    return 1;

rollback:
    if (surface) svc->revoke_surface(svc->user, surface);
    if (svc->remove_view(svc->user, args[0].bits, window) != 1) {
        view->native_window = window;
        view->detach_pending = 1;
        return 0;
    }
discard:
    if (allocated) {
        --host->view_count;
        memset(view, 0, sizeof *view);
    }
    return 0;
}

static BsDexAndroidSurfaceView *verified_receiver(
    BsDexAndroidSurfaceHost *host, BsDexVM *vm, const BsDexValue *args,
    size_t count, int need_surface) {
    if (!args || count == 0 || args[0].kind != BS_DEX_REF || !args[0].bits)
        return NULL;
    const char *type = bs_dex_reference_type(vm, args[0]);
    if (!same(type, BS_INPUT_SURFACE_VIEW) &&
        !same(type, BS_GAME_ACTIVITY_SURFACE_VIEW) &&
        !same(type, BS_UNITY_GAME_ACTIVITY_SURFACE_VIEW) &&
        !same(type, BS_SURFACE_VIEW) &&
        !same(type, BS_VIEW)) return NULL;
    BsDexAndroidSurfaceView *view = view_for(host, args[0]);
    if (!view || !view->initialized || (!!need_surface && !view->is_surface_view))
        return NULL;
    if (view->is_surface_view && !verify_view_surface(host, view))
        return NULL;
    return view;
}

static BsDexAndroidSurfaceView *verified_holder_view(
    BsDexAndroidSurfaceHost *host, BsDexVM *vm, BsDexValue receiver) {
    if (!same(bs_dex_reference_type(vm, receiver), BS_SURFACE_HOLDER)) return NULL;
    void *actual_holder = bs_dex_host_pointer(vm, receiver);
    for (size_t i = 0; i < host->view_count; ++i) {
        if (actual_holder != &host->holders[i]) continue;
        BsDexAndroidSurfaceView *view = &host->views[i];
        return host->holders[i].owner == host &&
               host->holders[i].view == view &&
               verify_view_surface(host, view) ? view : NULL;
    }
    return NULL;
}

int bs_dex_android_surface_invoke_platform(
    void *user, BsDexVM *vm, const char *declaring_class,
    const char *method_name, const char *prototype,
    uint8_t invoke_opcode, const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    BsDexAndroidSurfaceHost *host = user;
    if (exception) *exception = NULL;
    if (!result || !bind_vm(host, vm)) return 0;
    *result = bs_dex_null();
    if (!declaring_class || !method_name || !prototype || !args) return 0;

    if ((same(declaring_class, BS_SURFACE_VIEW) ||
         same(declaring_class, BS_VIEW)) &&
        same(method_name, "<init>") &&
        same(prototype, "(Landroid/content/Context;)V")) {
        if (invoke_opcode != 0x70 && invoke_opcode != 0x76) return 0;
        return construct_view(host, vm, declaring_class, args, count);
    }

    if (same(declaring_class, BS_SURFACE_VIEW)) {
        BsDexAndroidSurfaceView *view =
            verified_receiver(host, vm, args, count, 1);
        if (!view) return 0;
        if (same(method_name, "getHolder") &&
            same(prototype, "()Landroid/view/SurfaceHolder;") && count == 1) {
            size_t index = (size_t)(view - host->views);
            BsDexValue holder = bs_dex_wrap_platform(
                vm, &host->holders[index], BS_SURFACE_HOLDER);
            if (!holder.bits || holder.kind != BS_DEX_REF) return 0;
            *result = holder;
            return 1;
        }
        if (same(method_name, "setZOrderOnTop") &&
            same(prototype, "(Z)V") && count == 2 &&
            args[1].kind == BS_DEX_INT && args[1].bits <= 1 &&
            host->services.on_ui_thread &&
            host->services.on_ui_thread(host->services.user) == 1 &&
            host->services.set_z_order_on_top &&
            host->services.set_z_order_on_top(
                    host->services.user, view->receiver.bits,
                    (int)args[1].bits) == 1) {
            view->z_order_set = 1;
            view->z_order_on_top = (unsigned)args[1].bits;
            return 1;
        }
        return 0; /* Layout, accessibility and ViewTreeObserver unavailable. */
    }

    if (same(declaring_class, BS_SURFACE_HOLDER)) {
        if (!count) return 0;
        BsDexAndroidSurfaceView *view =
            verified_holder_view(host, vm, args[0]);
        if (!view) return 0;
        if (same(method_name, "getSurface") &&
            same(prototype, "()Landroid/view/Surface;") && count == 1) {
            BsDexValue surface = bs_dex_wrap_platform(
                vm, view->jni_surface, BS_SURFACE);
            if (!surface.bits || surface.kind != BS_DEX_REF) return 0;
            *result = surface;
            return 1;
        }
        if (same(method_name, "addCallback") &&
            same(prototype, "(Landroid/view/SurfaceHolder$Callback;)V") &&
            count == 2 && args[1].kind == BS_DEX_REF && args[1].bits &&
            bs_dex_reference_type(vm, args[1]) &&
            view->callback_count < BS_DEX_ANDROID_MAX_HOLDER_CALLBACKS &&
            host->services.is_holder_callback &&
            host->services.is_holder_callback(
                    host->services.user, vm, args[1]) == 1 &&
            host->services.on_ui_thread &&
            host->services.on_ui_thread(host->services.user) == 1 &&
            host->services.register_holder_callback &&
            host->services.register_holder_callback(
                    host->services.user, view->receiver.bits, args[1]) == 1) {
            view->holder_callbacks[view->callback_count++] = args[1];
            ++host->observed_holder_callbacks;
            return 1;
        }
        if (same(method_name, "setFormat") &&
            same(prototype, "(I)V") && count == 2 &&
            args[1].kind == BS_DEX_INT &&
            host->services.on_ui_thread &&
            host->services.on_ui_thread(host->services.user) == 1 &&
            host->services.set_holder_format &&
            host->services.set_holder_format(
                    host->services.user, view->receiver.bits,
                    (int32_t)args[1].bits) == 1) {
            view->last_format = (int32_t)args[1].bits;
            view->format_set = 1;
            return 1;
        }
        return 0;
    }
    if (same(declaring_class, BS_VIEW) &&
        same(method_name, "getContext") &&
        same(prototype, "()Landroid/content/Context;") && count == 1) {
        BsDexAndroidSurfaceView *view =
            verified_receiver(host, vm, args, count, 0);
        if (!view) return 0;
        *result = view->context;
        return 1;
    }
    /* All Android methods not explicitly backed by this service fail closed.
     * There is no Activity/currentActivity/graphics or placeholder readiness. */
    return 0;
}

int bs_dex_android_horizon_window_dimensions(
    void *user, void *window, uint32_t *width, uint32_t *height) {
    (void)user;
    if (!width || !height) return 0;
    *width = *height = 0;
#ifdef __SWITCH__
    if (!window || !nwindowIsValid((NWindow *)window)) return 0;
    u32 measured_width = 0, measured_height = 0;
    if (R_FAILED(nwindowGetDimensions((NWindow *)window, &measured_width,
                                      &measured_height)) ||
        !measured_width || !measured_height) return 0;
    *width = measured_width;
    *height = measured_height;
    return 1;
#else
    (void)window;
    return 0;
#endif
}

void *bs_dex_android_jni_surface_from_nwindow(void *user, void *window) {
    (void)user;
#ifdef __SWITCH__
    return bs_jni_surface_from_nwindow(window);
#else
    (void)window;
    return NULL;
#endif
}

void *bs_dex_android_jni_verified_window(void *user, void *surface) {
    (void)user;
#ifdef __SWITCH__
    return bs_jni_backed_surface_nwindow(bs_jni_env(), surface);
#else
    (void)surface;
    return NULL;
#endif
}

void bs_dex_android_jni_revoke_surface(void *user, void *surface) {
    (void)user;
#ifdef __SWITCH__
    bs_jni_revoke_surface(surface);
#else
    (void)surface;
#endif
}
