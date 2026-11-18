#include "unity_dex_android_text_input_host.h"
#include <string.h>

#define BS_TEXT_EDITOR "Landroid/view/inputmethod/EditorInfo;"
#define BS_TEXT_CONNECTION "Lcom/google/androidgamesdk/gametextinput/InputConnection;"
#define BS_TEXT_BASE_CONNECTION "Landroid/view/inputmethod/BaseInputConnection;"
#define BS_TEXT_MANAGER "Landroid/view/inputmethod/InputMethodManager;"
#define BS_TEXT_VIEW "Landroid/view/View;"
#define BS_TEXT_CONTEXT "Landroid/content/Context;"
#define BS_TEXT_ACTIVITY "Lcom/unity3d/player/UnityPlayerGameActivity;"
#define BS_TEXT_LOG "Landroid/util/Log;"

static int same(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int bind_vm(BsDexAndroidTextInputHost *host, BsDexVM *vm) {
    if (!host || !vm || (host->vm && host->vm != vm)) return 0;
    host->vm = vm;
    return 1;
}

static int same_ref(BsDexValue a, BsDexValue b) {
    return a.kind == b.kind && a.bits == b.bits;
}

static BsDexTextEditorInfo *find_editor(BsDexAndroidTextInputHost *host,
                                        BsDexVM *vm, BsDexValue reference) {
    if (!host || !vm || !same(bs_dex_reference_type(vm, reference),
                              BS_TEXT_EDITOR)) return NULL;
    for (size_t i = 0; i < host->editor_count; ++i) {
        BsDexTextEditorInfo *editor = &host->editors[i];
        if (editor->allocated && same_ref(reference, editor->reference) &&
            bs_dex_host_pointer(vm, reference) == editor)
            return editor;
    }
    return NULL;
}

static BsDexTextConnection *find_connection(
    BsDexAndroidTextInputHost *host, BsDexVM *vm, BsDexValue reference) {
    if (!host || !vm || !same(bs_dex_reference_type(vm, reference),
                              BS_TEXT_CONNECTION)) return NULL;
    for (size_t i = 0; i < host->connection_count; ++i) {
        BsDexTextConnection *connection = &host->connections[i];
        if (connection->initialized &&
            same_ref(reference, connection->receiver))
            return connection;
    }
    return NULL;
}

void bs_dex_android_text_input_init(BsDexAndroidTextInputHost *host,
                                     BsDexAndroidContextHost *context,
                                     BsDexAndroidSurfaceHost *surface,
                                     const BsDexAndroidTextNative *native) {
    if (!host) return;
    memset(host, 0, sizeof *host);
    host->context = context;
    host->surface = surface;
    if (native) host->native = *native;
}

int bs_dex_android_text_input_reset(BsDexAndroidTextInputHost *host) {
    if (!host) return 0;
    /* The caller destroys the owning VM first; this module cannot release
     * an OS input method obtained from an externally owned native service. */
    BsDexAndroidContextHost *ctx = host->context;
    BsDexAndroidSurfaceHost *view = host->surface;
    BsDexAndroidTextNative native = host->native;
    memset(host, 0, sizeof *host);
    host->context = ctx;
    host->surface = view;
    host->native = native;
    return 1;
}

int bs_dex_android_text_input_new_platform(
    BsDexAndroidTextInputHost *host, BsDexVM *vm, const char *descriptor,
    BsDexValue *object, const char **exception) {
    if (object) *object = bs_dex_null();
    if (exception) *exception = NULL;
    if (!object || !bind_vm(host, vm) || !same(descriptor, BS_TEXT_EDITOR) ||
        host->editor_count >= BS_DEX_TEXT_MAX_EDITORS)
        return 0;
    BsDexTextEditorInfo *editor = &host->editors[host->editor_count];
    *editor = (BsDexTextEditorInfo){.allocated = 1};
    BsDexValue ref = bs_dex_wrap_platform(vm, editor, BS_TEXT_EDITOR);
    if (ref.kind != BS_DEX_REF || !ref.bits) {
        *editor = (BsDexTextEditorInfo){0};
        return 0;
    }
    editor->reference = ref;
    ++host->editor_count;
    *object = ref;
    return 1;
}

/* Activity and UI-thread identity alone do not confer an Android text input
 * manager. The physical applet must have an independently observable native
 * service that a real guest Java InputMethodManager can use. There is no
 * fallback to a touchscreen, keyboard guess, or an arbitrary NWindow. */
static int attached_activity(BsDexAndroidTextInputHost *host, BsDexVM *vm,
                             BsDexValue context) {
    BsDexAndroidContextHost *ctx = host ? host->context : NULL;
    return ctx && vm && ctx->attachment_evidenced && ctx->activity &&
        context.kind == BS_DEX_REF &&
        bs_dex_host_pointer(vm, context) == ctx->activity &&
        ctx->verify && ctx->verify(ctx->user, BS_TEXT_ACTIVITY,
                                   ctx->activity) == 1 &&
        host->native.on_ui_thread &&
        host->native.on_ui_thread(host->native.user) == 1;
}

static int observed_input_method(BsDexAndroidTextInputHost *host, BsDexVM *vm,
                                 BsDexValue context, BsDexValue *result) {
    if (result) *result = bs_dex_null();
    if (!result || !attached_activity(host, vm, context) ||
        !host->native.observe_input_method ||
        !host->native.verify_input_method)
        return 0;
    void *actual = NULL;
    uint64_t generation = 0;
    if (host->native.observe_input_method(host->native.user,
             host->context->activity, &actual, &generation) != 1 ||
        !actual || !generation ||
        host->native.verify_input_method(host->native.user,
             host->context->activity, actual, generation) != 1)
        return 0;

    if (host->input_method_manager.bits) {
        BsDexTextInputMethodManager *owner = &host->java_input_method;
        if (!owner->initialized ||
            bs_dex_host_pointer(vm, host->input_method_manager) != owner ||
            owner->activity != host->context->activity ||
            owner->native_service != actual ||
            owner->generation != generation)
            return 0;
    } else {
        BsDexTextInputMethodManager *owner = &host->java_input_method;
        *owner = (BsDexTextInputMethodManager){
            .activity = host->context->activity, .native_service = actual,
            .generation = generation, .initialized = 1
        };
        BsDexValue manager = bs_dex_wrap_platform(vm, owner, BS_TEXT_MANAGER);
        if (manager.kind != BS_DEX_REF || !manager.bits) {
            *owner = (BsDexTextInputMethodManager){0};
            return 0;
        }
        host->input_method_manager = manager;
        host->native_input_method = actual;
        host->native_generation = generation;
    }
    ++host->observed_service_reads;
    *result = host->input_method_manager;
    return 1;
}

static int construct_base_connection(BsDexAndroidTextInputHost *host,
                                     BsDexVM *vm, const BsDexValue *args,
                                     size_t count) {
    if (!host || !vm || !args || count != 3 ||
        args[0].kind != BS_DEX_REF || !args[0].bits ||
        !same(bs_dex_reference_type(vm, args[0]), BS_TEXT_CONNECTION) ||
        /* The original subclass is allocated and constructed by the DEX.
         * A non-NULL platform backing here would be a different Java
         * object with an unrelated lifetime. */
        bs_dex_host_pointer(vm, args[0]) ||
        args[1].kind != BS_DEX_REF || !args[1].bits ||
        args[2].kind != BS_DEX_INT || args[2].bits > 1 ||
        !host->surface ||
        host->connection_count >= BS_DEX_TEXT_MAX_CONNECTIONS)
        return 0;
    if (find_connection(host, vm, args[0])) return 0;
    /* The framework constructor executes View.getContext() and then
     * Context.getSystemService(INPUT_METHOD_SERVICE). Reuse the actual
     * SurfaceHost's Java View owner and the already verified Context; never
     * look up an input method for a detached or different Activity. */
    BsDexValue context = bs_dex_null();
    const char *exception = NULL;
    if (bs_dex_android_surface_invoke_platform(host->surface, vm,
            BS_TEXT_VIEW, "getContext", "()Landroid/content/Context;",
            0x6e, &args[1], 1, &context, &exception) != 1 ||
        exception || !attached_activity(host, vm, context))
        return 0;
    BsDexValue manager = bs_dex_null();
    if (!observed_input_method(host, vm, context, &manager)) return 0;

    BsDexTextConnection *connection =
        &host->connections[host->connection_count++];
    *connection = (BsDexTextConnection){
        .receiver = args[0],
        .target_view = args[1], .context = context,
        .input_method_manager = manager,
        .native_input_method = host->native_input_method,
        .native_generation = host->native_generation,
        .dummy_mode = !args[2].bits,
        .initialized = 1
    };
    return 1;
}

int bs_dex_android_text_input_invoke(
    BsDexAndroidTextInputHost *host, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    if (result) *result = bs_dex_null();
    if (exception) *exception = NULL;
    if (!result || !bind_vm(host, vm) || !declaring_class ||
        !method_name || !prototype || !args) return 0;
    if (same(declaring_class, BS_TEXT_EDITOR) &&
        same(method_name, "<init>") && same(prototype, "()V") &&
        (invoke_opcode == 0x70 || invoke_opcode == 0x76) && count == 1) {
        BsDexTextEditorInfo *editor = find_editor(host, vm, args[0]);
        if (!editor || editor->initialized) return 0;
        /* Java's noarg EditorInfo constructor has no mandatory native
         * side effect. JVM-default primitive fields remain exactly zero. */
        editor->initialized = 1;
        return 1;
    }
    if (same(declaring_class, BS_TEXT_BASE_CONNECTION) &&
        same(method_name, "<init>") &&
        same(prototype, "(Landroid/view/View;Z)V") &&
        (invoke_opcode == 0x70 || invoke_opcode == 0x76))
        return construct_base_connection(host, vm, args, count);
    if (same(declaring_class, BS_TEXT_CONTEXT) &&
        same(method_name, "getSystemService") &&
        same(prototype, "(Ljava/lang/String;)Ljava/lang/Object;") &&
        (invoke_opcode == 0x6e || invoke_opcode == 0x74) &&
        count == 2 && bs_dex_string(vm, args[1]) &&
        !strcmp(bs_dex_string(vm, args[1]), "input_method"))
        return observed_input_method(host, vm, args[0], result);
    if (same(declaring_class, BS_TEXT_LOG) &&
        same(method_name, "d") &&
        same(prototype, "(Ljava/lang/String;Ljava/lang/String;)I") &&
        (invoke_opcode == 0x71 || invoke_opcode == 0x77) &&
        count == 2 && host->native.write_debug_log) {
        const char *tag = bs_dex_string(vm, args[0]);
        const char *message = bs_dex_string(vm, args[1]);
        if (!tag || !message || strlen(tag) > 256 ||
            strlen(message) > 8192) return 0;
        int32_t count_written = 0;
        if (host->native.write_debug_log(host->native.user, tag,
                                          message, &count_written) != 1)
            return 0;
        *result = bs_dex_int(count_written);
        ++host->observed_log_writes;
        return 1;
    }
    return 0;
}

int bs_dex_android_text_input_field(
    BsDexAndroidTextInputHost *host, BsDexVM *vm,
    const char *declaring_class, const char *field_name,
    const char *field_type, BsDexValue receiver, int is_write,
    BsDexValue *value, const char **exception) {
    if (exception) *exception = NULL;
    if (!value || !bind_vm(host, vm) || !same(declaring_class,
          BS_TEXT_EDITOR) || !same(field_type, "I")) return 0;
    BsDexTextEditorInfo *editor = find_editor(host, vm, receiver);
    if (!editor || !editor->initialized) return 0;
    int32_t *slot = same(field_name, "inputType") ? &editor->input_type :
        same(field_name, "actionId") ? &editor->action_id :
        same(field_name, "imeOptions") ? &editor->ime_options : NULL;
    if (!slot) return 0;
    if (is_write) {
        if (value->kind != BS_DEX_INT) return 0;
        *slot = (int32_t)(uint32_t)value->bits;
    } else {
        *value = bs_dex_int(*slot);
    }
    return 1;
}

int bs_dex_android_text_editor_snapshot(
    const BsDexAndroidTextInputHost *host, BsDexValue editor,
    BsDexTextEditorInfo *out) {
    if (out) *out = (BsDexTextEditorInfo){0};
    if (!host || !out || editor.kind != BS_DEX_REF) return 0;
    for (size_t i = 0; i < host->editor_count; ++i)
        if (host->editors[i].initialized &&
            same_ref(editor, host->editors[i].reference)) {
            *out = host->editors[i];
            return 1;
        }
    return 0;
}

int bs_dex_android_text_connection_snapshot(
    const BsDexAndroidTextInputHost *host, BsDexValue connection,
    BsDexTextConnection *out) {
    if (out) *out = (BsDexTextConnection){0};
    if (!host || !out || connection.kind != BS_DEX_REF) return 0;
    for (size_t i = 0; i < host->connection_count; ++i)
        if (host->connections[i].initialized &&
            same_ref(connection, host->connections[i].receiver)) {
            *out = host->connections[i];
            return 1;
        }
    return 0;
}
