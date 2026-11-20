#include "unity_dex_horizon_java_attach_runtime.h"
#include <string.h>

#define BS_RESOURCES_CLASS "Landroid/content/res/Resources;"

static int live(BsDexHorizonJavaAttachRuntime *runtime,
                void **java_resources) {
    if (java_resources) *java_resources = NULL;
    if (!runtime || !java_resources || !runtime->attachment ||
        !runtime->resources || !runtime->attachment_generation ||
        !runtime->resources_generation ||
        runtime->attachment_generation != runtime->attachment->generation ||
        runtime->resources_generation != runtime->resources->generation ||
        runtime->resources->attachment != runtime->attachment ||
        !bs_dex_horizon_android_attachment_java_attached(runtime->attachment) ||
        !bs_dex_horizon_java_resources_native_ready(runtime->resources) ||
        !bs_dex_horizon_java_resources_java_get_resources(
            runtime->resources, java_resources) ||
        !*java_resources ||
        *java_resources != runtime->attachment->actual_readback.resources ||
        runtime->attachment->actual_readback.assets !=
            runtime->resources->java_objects.asset_manager ||
        !runtime->attachment->java.verify_type ||
        runtime->attachment->java.verify_type(
            runtime->attachment->java.user, BS_RESOURCES_CLASS,
            *java_resources) != 1) {
        *java_resources = NULL;
        return 0;
    }
    return 1;
}

int bs_dex_horizon_java_attach_runtime_bind(
    BsDexHorizonJavaAttachRuntime *runtime,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonJavaResources *resources) {
    void *observed = NULL;
    if (!runtime || runtime->attachment || runtime->resources ||
        runtime->attachment_generation || runtime->resources_generation ||
        !attachment || !resources ||
        !attachment->generation || !resources->generation ||
        resources->attachment != attachment) return 0;
    BsDexHorizonJavaAttachRuntime candidate = {
        .attachment = attachment,
        .resources = resources,
        .attachment_generation = attachment->generation,
        .resources_generation = resources->generation
    };
    if (!live(&candidate, &observed)) return 0;
    *runtime = candidate;
    return 1;
}

int bs_dex_horizon_java_attach_runtime_invoke(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    BsDexHorizonJavaAttachRuntime *runtime = user;
    if (exception) *exception = NULL;
    if (!vm || !runtime || !result || !declaring_class || !method_name ||
        !prototype || !args || count != 2 ||
        strcmp(declaring_class, BS_RESOURCES_CLASS) ||
        (invoke_opcode != 0x6e && invoke_opcode != 0x74) ||
        args[0].kind != BS_DEX_REF || !args[0].bits ||
        args[1].kind != BS_DEX_INT ||
        !bs_dex_reference_type(vm, args[0]) ||
        strcmp(bs_dex_reference_type(vm, args[0]), BS_RESOURCES_CLASS))
        return 0;

    enum { METHOD_NONE, METHOD_BOOLEAN, METHOD_INTEGER, METHOD_STRING } method =
        METHOD_NONE;
    if (!strcmp(method_name, "getBoolean") &&
        !strcmp(prototype, "(I)Z")) method = METHOD_BOOLEAN;
    else if (!strcmp(method_name, "getInteger") &&
        !strcmp(prototype, "(I)I")) method = METHOD_INTEGER;
    else if (!strcmp(method_name, "getString") &&
        !strcmp(prototype, "(I)Ljava/lang/String;")) method = METHOD_STRING;
    if (method == METHOD_NONE) return 0;

    void *java_resources = NULL;
    if (!live(runtime, &java_resources) ||
        bs_dex_host_pointer(vm, args[0]) != java_resources) return 0;

    const uint32_t resource_id = (uint32_t)args[1].bits;
    BsDexValue observed = bs_dex_null();
    if (method == METHOD_BOOLEAN) {
        int value = 0;
        if (!bs_dex_horizon_java_resources_get_boolean(
                runtime->resources, resource_id, &value)) return 0;
        observed = bs_dex_int(value != 0);
    } else if (method == METHOD_INTEGER) {
        int32_t value = 0;
        if (!bs_dex_horizon_java_resources_get_integer(
                runtime->resources, resource_id, &value)) return 0;
        observed = bs_dex_int(value);
    } else {
        const char *utf8 = NULL;
        if (!bs_dex_horizon_java_resources_get_string(
                runtime->resources, resource_id, &utf8) || !utf8)
            return 0;
        /* Native ARSC resolution is an actual host operation over the
         * authenticated original APK bytes, with observed configuration.
         * VM strings remain guest-owned and are never JNI jobjects. */
        observed = bs_dex_new_observed_string_utf8(vm, utf8, strlen(utf8));
        if (observed.kind != BS_DEX_REF || !observed.bits) return 0;
    }
    *result = observed;
    return 1;
}

void bs_dex_horizon_java_attach_runtime_clear(
    BsDexHorizonJavaAttachRuntime *runtime) {
    if (runtime) *runtime = (BsDexHorizonJavaAttachRuntime){0};
}

int bs_dex_horizon_java_attach_runtime_invoke_platform(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    return bs_dex_horizon_java_attach_runtime_invoke(
        user, vm, declaring_class, method_name, prototype,
        invoke_opcode, args, count, result, exception);
}
