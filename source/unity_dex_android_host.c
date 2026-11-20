#include "unity_dex_android_host.h"
#include "unity_dex_android_text_input_host.h"
#include <string.h>
#ifdef __SWITCH__
#include "util.h"
#endif

#define LIVE_ACTIVITY "Lcom/unity3d/player/UnityPlayerGameActivity;"

static int invoke_activity_constructor(BsDexAndroidHost *bridge, BsDexVM *vm,
                                       BsDexValue receiver,
                                       const char **exception) {
    BsDexAndroidContextHost *context = bridge->context;
    const char *type = bs_dex_reference_type(vm, receiver);
#ifdef __SWITCH__
    debugPrintf("[java:activity-base] invoke type=%s receiver=%p context=%p "
                "verify=%d construct=%d assignable=%d ui=%d\n",
                type ? type : "(null)", bs_dex_host_pointer(vm, receiver),
                context ? context->activity : NULL,
                context && context->verify != NULL,
                bridge->construct_activity_base != NULL,
                bridge->observed_instance_of != NULL,
                bridge->surface && bridge->surface->services.on_ui_thread ?
                    bridge->surface->services.on_ui_thread(
                        bridge->surface->services.user) : -1);
#endif
    if (!type || strcmp(type, LIVE_ACTIVITY) ||
        bs_dex_host_pointer(vm, receiver) ||
        !context || context->activity || !context->verify ||
        !bridge->construct_activity_base || !bridge->discard_activity ||
        !bridge->observed_instance_of || !bridge->surface ||
        !bridge->surface->services.on_ui_thread ||
        bridge->surface->services.on_ui_thread(
            bridge->surface->services.user) != 1) return 0;
    void *activity = NULL;
    const char *failure = NULL;
    int rc = bridge->construct_activity_base(
        bridge->user, type, &activity, &failure);
#ifdef __SWITCH__
    debugPrintf("[java:activity-base] native ctor result=%d activity=%p "
                "exception=%s\n", rc, activity,
                failure ? failure : "(none)");
#endif
    int actual_subclass = 0;
    int actual_activity = 0;
    if (rc == 1 && !failure && activity &&
        context->verify(context->user, type, activity) == 1 &&
        bridge->observed_instance_of(bridge->user, activity, type,
                                     &actual_subclass, &failure) == 1 &&
        actual_subclass && !failure &&
        bridge->observed_instance_of(bridge->user, activity,
                                     "Landroid/app/Activity;",
                                     &actual_activity, &failure) == 1 &&
        actual_activity && !failure &&
        bs_dex_bind_original_object(vm, receiver, activity)) {
        context->activity = activity;
        return 1;
    }
    if (activity) bridge->discard_activity(bridge->user, activity);
#ifdef __SWITCH__
    debugPrintf("[java:activity-base] native Activity identity rejected "
                "rc=%d observed_subclass=%d observed_base=%d\n",
                rc, actual_subclass, actual_activity);
#endif
    if (exception) *exception = failure;
    return failure || rc < 0 ? -1 : 0;
}

int bs_dex_android_host_complete_attachment(
    BsDexAndroidHost *bridge, BsDexVM *vm, const char **exception) {
    if (exception) *exception = NULL;
    if (!bridge || !vm || !bridge->context || !bridge->surface ||
        !bridge->construct_activity_base ||
        !bridge->attach_and_observe_activity ||
        !bridge->context->activity || bridge->context->attachment_evidenced ||
        !bridge->context->verify_attachment ||
        !bridge->surface->services.on_ui_thread ||
        !bridge->surface->services.context_matches_activity ||
        bridge->surface->services.on_ui_thread(
            bridge->surface->services.user) != 1 ||
        bs_dex_vm_stop(vm).kind != BS_DEX_STOP_RETURNED) return 0;
    void *activity = bridge->context->activity;
    BsDexAndroidContextAttachment observed = {0};
    const char *raised = NULL;
    int rc = bridge->attach_and_observe_activity(
        bridge->user, activity, &observed, &raised);
    if (rc != 1 || raised) {
        if (exception) *exception = raised;
        return rc < 0 && raised ? -1 : 0;
    }
    if (observed.activity != activity ||
        bridge->surface->services.context_matches_activity(
            bridge->surface->services.user, activity) != 1 ||
        !bs_dex_android_context_bind_attachment(bridge->context, &observed))
        return 0;
    return 1;
}

static int observed_activity(void *user, void *actual_activity) {
    BsDexAndroidHost *bridge = user;
    if (!bridge || !bridge->context || !bridge->surface ||
        !actual_activity || actual_activity != bridge->context->activity ||
        (bridge->construct_activity_base &&
         !bridge->context->attachment_evidenced) ||
        !bridge->context->verify || !bridge->surface->services.on_ui_thread ||
        !bridge->surface->services.context_matches_activity)
        return 0;
    return bridge->context->verify(bridge->context->user, LIVE_ACTIVITY,
                                  actual_activity) == 1 &&
           bridge->surface->services.context_matches_activity(
               bridge->surface->services.user, actual_activity) == 1 &&
           bridge->surface->services.on_ui_thread(
               bridge->surface->services.user) == 1;
}

static int new_platform(void *user, BsDexVM *vm, const char *descriptor,
                        BsDexValue *object, const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (bridge && bridge->text_input && descriptor &&
        !strcmp(descriptor, "Landroid/view/inputmethod/EditorInfo;"))
        return bs_dex_android_text_input_new_platform(
            bridge->text_input, vm, descriptor, object, exception);
    if (bridge && bridge->handler && bridge->handler_new && descriptor &&
        !strcmp(descriptor, "Landroid/os/Handler;"))
        return bridge->handler_new(
            bridge->handler, vm, descriptor, object, exception);
    return bridge && bridge->surface ?
        bs_dex_android_surface_new_platform_object(
            bridge->surface, vm, descriptor, object, exception) : 0;
}

static int invoke_platform(void *user, BsDexVM *vm, const char *klass,
                           const char *name, const char *signature,
                           uint8_t opcode, const BsDexValue *args,
                           size_t count, BsDexValue *result,
                           const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (!bridge || !result || !klass || !name || !signature) return 0;
    if (bridge->text_input) {
        int rc = bs_dex_android_text_input_invoke(
            bridge->text_input, vm, klass, name, signature, opcode,
            args, count, result, exception);
        if (rc) return rc;
        if (exception && *exception) return -1;
    }
    if (bridge->handler && bridge->handler_invoke &&
        !strcmp(klass, "Landroid/os/Handler;"))
        return bridge->handler_invoke(
            bridge->handler, vm, klass, name, signature,
            opcode, args, count, result, exception);
    if (bridge->resource_invoke && bridge->resource_user &&
        !strcmp(klass, "Landroid/content/res/Resources;"))
        return bridge->resource_invoke(
            bridge->resource_user, vm, klass, name, signature,
            opcode, args, count, result, exception);
    if (!strcmp(klass, "Ljava/lang/System;") &&
        !strcmp(name, "loadLibrary") &&
        !strcmp(signature, "(Ljava/lang/String;)V") &&
        (opcode == 0x71 || opcode == 0x77) && args && count == 1) {
        const char *library = bs_dex_string(vm, args[0]);
        return library && !strcmp(library, "game") &&
               bridge->load_game_library &&
               bridge->load_game_library(bridge->user) == 1;
    }
    if (!strcmp(klass, "Landroid/app/Activity;") &&
        !strcmp(name, "<init>") && !strcmp(signature, "()V") &&
        (opcode == 0x70 || opcode == 0x76) && args && count == 1)
        return invoke_activity_constructor(bridge, vm, args[0], exception);
    if (bridge->surface) {
        int rc = bs_dex_android_surface_invoke_platform(
            bridge->surface, vm, klass, name, signature, opcode,
            args, count, result, exception);
        if (rc != 0) return rc;
        if (exception && *exception) return -1;
    }
    if (bridge->context)
        return bs_dex_android_context_invoke(
            bridge->context, vm, klass, name, signature, opcode,
            args, count, result, exception);
    return 0; /* Other Android platform calls are intentionally unsupported. */
}

static int field_platform(void *user, BsDexVM *vm, const char *klass,
                          const char *field, const char *descriptor,
                          BsDexValue receiver, int is_write,
                          BsDexValue *value, const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (bridge && bridge->text_input && klass &&
        !strcmp(klass, "Landroid/view/inputmethod/EditorInfo;"))
        return bs_dex_android_text_input_field(
            bridge->text_input, vm, klass, field, descriptor,
            receiver, is_write, value, exception);
    return bridge && bridge->context ?
        bs_dex_android_context_field(
            bridge->context, vm, klass, field, descriptor,
            receiver, is_write, value, exception) : 0;
}

static int instance_of(void *user, BsDexVM *vm, BsDexValue object,
                       const char *descriptor, int *result,
                       const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (result) *result = 0;
    if (bridge && bridge->handler && bridge->handler_instance_of &&
        result && descriptor && bridge->handler_instance_of(
            bridge->handler, vm, object, descriptor, result)) return 1;
    if (!bridge || !result || !descriptor ||
        !bridge->observed_instance_of) return 0;
    void *live = bs_dex_host_pointer(vm, object);
    if (!live) return 0;
    return bridge->observed_instance_of(
        bridge->user, live, descriptor, result, exception);
}

static int weak_services_ready(const BsDexAndroidHost *bridge) {
    return bridge && bridge->weak_reference_create &&
        bridge->weak_reference_get && bridge->weak_reference_clear &&
        bridge->weak_reference_release;
}

static int weak_create(void *user, BsDexVM *vm, BsDexValue referent,
                       void **handle, const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (handle) *handle = NULL;
    if (!weak_services_ready(bridge) || !vm || !handle ||
        !bridge->context || !bridge->context->verify ||
        referent.kind != BS_DEX_REF || !referent.bits) return 0;
    const char *type = bs_dex_reference_type(vm, referent);
    void *live = bs_dex_host_pointer(vm, referent);
    if (!type || !live ||
        bridge->context->verify(bridge->context->user, type, live) != 1)
        return 0;

    const char *raised = NULL;
    void *token = NULL;
    int rc = bridge->weak_reference_create(bridge->user, vm, referent,
                                           &token, &raised);
    if (rc != 1 || raised || !token) {
        /* A callback may have allocated a weak token before failing. It must
         * not survive a rejected construction or leak into a later VM. */
        if (token) bridge->weak_reference_release(bridge->user, vm, token);
        if (rc < 0 && raised && exception) *exception = raised;
        return rc < 0 && raised ? -1 : 0;
    }
    *handle = token;
    return 1;
}

static int weak_get(void *user, BsDexVM *vm, void *handle,
                    BsDexValue *result, const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (result) *result = bs_dex_null();
    if (!weak_services_ready(bridge) || !vm || !handle || !result ||
        !bridge->context || !bridge->context->verify) return 0;

    BsDexValue live = bs_dex_null();
    const char *raised = NULL;
    int rc = bridge->weak_reference_get(bridge->user, vm, handle,
                                       &live, &raised);
    if (rc != 1 || raised) {
        if (rc < 0 && raised && exception) *exception = raised;
        return rc < 0 && raised ? -1 : 0;
    }
    if (!live.bits) {
        if (live.kind != BS_DEX_REF) return 0;
        *result = live; /* Native backend witnessed reclamation or clear. */
        return 1;
    }
    if (live.kind != BS_DEX_REF) return 0;
    const char *type = bs_dex_reference_type(vm, live);
    void *object = bs_dex_host_pointer(vm, live);
    if (!type || !object ||
        bridge->context->verify(bridge->context->user, type, object) != 1)
        return 0;
    *result = live;
    return 1;
}

static int weak_clear(void *user, BsDexVM *vm, void *handle,
                      const char **exception) {
    BsDexAndroidHost *bridge = user;
    if (exception) *exception = NULL;
    if (!weak_services_ready(bridge) || !vm || !handle) return 0;
    const char *raised = NULL;
    int rc = bridge->weak_reference_clear(bridge->user, vm, handle,
                                          &raised);
    if (rc < 0 && raised && exception) *exception = raised;
    return rc == 1 && !raised ? 1 : rc < 0 && raised ? -1 : 0;
}

static void weak_release(void *user, BsDexVM *vm, void *handle) {
    BsDexAndroidHost *bridge = user;
    if (weak_services_ready(bridge) && vm && handle)
        bridge->weak_reference_release(bridge->user, vm, handle);
}

static int monotonic_nanoseconds(void *user, uint64_t *out) {
    BsDexAndroidHost *bridge = user;
    return bridge && out && bridge->monotonic_nanoseconds &&
        bridge->monotonic_nanoseconds(bridge->user, out) == 1;
}

static int observe_main_looper(void *user, uintptr_t *queue_identity,
                               uint64_t *owner_thread_id,
                               uint64_t *current_thread_id) {
    BsDexAndroidHost *bridge = user;
    if (queue_identity) *queue_identity = 0;
    if (owner_thread_id) *owner_thread_id = 0;
    if (current_thread_id) *current_thread_id = 0;
    if (!bridge || !bridge->observe_main_looper || !queue_identity ||
        !owner_thread_id || !current_thread_id) return 0;
    return bridge->observe_main_looper(bridge->user, queue_identity,
                                       owner_thread_id,
                                       current_thread_id) == 1 &&
        *queue_identity != 0 && *owner_thread_id != 0 &&
        *current_thread_id != 0;
}

int bs_dex_android_host_callbacks(BsDexAndroidHost *bridge, BsDexHost *out) {
    if (!bridge || !out || !bridge->context || !bridge->surface ||
        bridge->surface->attached_vm)
        return 0;
    const int weak_count = !!bridge->weak_reference_create +
        !!bridge->weak_reference_get + !!bridge->weak_reference_clear +
        !!bridge->weak_reference_release;
    if (weak_count != 0 && weak_count != 4) return 0;
    *out = (BsDexHost){
        .user = bridge,
        .new_platform_object = new_platform,
        .invoke_platform = invoke_platform,
        .field_platform = field_platform,
        .instance_of = instance_of,
        .validate_activity = observed_activity,
        .weak_reference_create = weak_count ? weak_create : NULL,
        .weak_reference_get = weak_count ? weak_get : NULL,
        .weak_reference_clear = weak_count ? weak_clear : NULL,
        .weak_reference_release = weak_count ? weak_release : NULL,
        .monotonic_nanoseconds = bridge->monotonic_nanoseconds ?
                                 monotonic_nanoseconds : NULL,
        .observe_main_looper = bridge->observe_main_looper ?
                               observe_main_looper : NULL
    };
    return 1;
}
