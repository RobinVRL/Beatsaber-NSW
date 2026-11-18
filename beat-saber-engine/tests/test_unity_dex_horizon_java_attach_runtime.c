#include "unity_dex_horizon_java_attach_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Only the Java attachment/VM handles are fixture tokens. All three
 * resolved resource values below come from the authenticated real Quest APK. */
static BsQuestArscResolver original_table;
static BsQuestArscConfiguration observed;
static int allow_attachment, allow_resources, allow_type;
static int resource_marker, asset_marker;
static void *guest_receiver;
static char guest_string[BS_QUEST_ARSC_MAX_STRING_BYTES];

int bs_dex_horizon_android_attachment_java_attached(
    const BsDexHorizonAndroidAttachment *attachment) {
    return allow_attachment && attachment->java_attached &&
           attachment->java_verified;
}

int bs_dex_horizon_java_resources_native_ready(
    const BsDexHorizonJavaResources *resources) {
    return resources && resources->initialized && resources->native_arsc &&
           resources->native_arsc->crc32 == BS_QUEST_RESOURCES_ARSC_CRC;
}

int bs_dex_horizon_java_resources_java_get_resources(
    const BsDexHorizonJavaResources *resources, void **object) {
    if (object) *object = NULL;
    if (!object || !resources || !allow_resources || !resources->java_verified)
        return 0;
    *object = resources->java_objects.resources;
    return *object != NULL;
}

int bs_dex_horizon_java_resources_get_boolean(
    const BsDexHorizonJavaResources *resources, uint32_t id, int *value) {
    return bs_quest_arsc_get_boolean(resources->native_arsc, id,
                                     &observed, value);
}
int bs_dex_horizon_java_resources_get_integer(
    const BsDexHorizonJavaResources *resources, uint32_t id, int32_t *value) {
    return bs_quest_arsc_get_integer(resources->native_arsc, id,
                                     &observed, value);
}
int bs_dex_horizon_java_resources_get_string(
    BsDexHorizonJavaResources *resources, uint32_t id, const char **value) {
    if (value) *value = NULL;
    return value && bs_quest_arsc_get_string(
        resources->native_arsc, id, &observed,
        resources->last_native_resource_string,
        sizeof resources->last_native_resource_string) &&
        ((*value = resources->last_native_resource_string) != NULL);
}

static int actual_type(void *user, const char *type, void *object) {
    (void)user;
    return allow_type && object == &resource_marker &&
        !strcmp(type, "Landroid/content/res/Resources;");
}

BsDexValue bs_dex_null(void) {
    return (BsDexValue){0, BS_DEX_REF};
}
BsDexValue bs_dex_int(int32_t value) {
    return (BsDexValue){(uintptr_t)(uint32_t)value, BS_DEX_INT};
}
void *bs_dex_host_pointer(BsDexVM *vm, BsDexValue value) {
    (void)vm;
    return value.kind == BS_DEX_REF && value.bits == 0x1234u ?
        guest_receiver : NULL;
}
const char *bs_dex_reference_type(BsDexVM *vm, BsDexValue value) {
    (void)vm;
    return value.kind == BS_DEX_REF && value.bits == 0x1234u ?
        "Landroid/content/res/Resources;" : NULL;
}
BsDexValue bs_dex_new_observed_string_utf8(BsDexVM *vm,
                                            const char *utf8, size_t n) {
    (void)vm;
    if (!utf8 || n >= sizeof guest_string) return bs_dex_null();
    memcpy(guest_string, utf8, n);
    guest_string[n] = 0;
    return (BsDexValue){0xBEEFu, BS_DEX_REF};
}

static int invoke(BsDexHorizonJavaAttachRuntime *runtime,
                  const char *method, const char *proto, uint32_t id,
                  uint8_t opcode, BsDexValue *out) {
    BsDexValue args[] = {
        {0x1234u, BS_DEX_REF}, {id, BS_DEX_INT}
    };
    const char *exception = "should clear";
    int rc = bs_dex_horizon_java_attach_runtime_invoke_platform(
        runtime, (BsDexVM *)(uintptr_t)0x1,
        "Landroid/content/res/Resources;", method, proto,
        opcode, args, 2, out, &exception);
    assert(exception == NULL);
    return rc;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(bs_quest_arsc_open_apk(argv[1], &original_table));
    assert(original_table.crc32 == BS_QUEST_RESOURCES_ARSC_CRC);
    assert(bs_quest_arsc_set_observed_language(&observed, "en-US"));

    BsDexHorizonAndroidAttachment a = {0};
    a.generation = 11;
    a.java.verify_type = actual_type;
    a.actual_readback.resources = &resource_marker;
    a.actual_readback.assets = &asset_marker;
    a.java_attached = a.java_verified = 1;
    BsDexHorizonJavaResources r = {0};
    r.attachment = &a;
    r.generation = 5;
    r.initialized = r.java_verified = 1;
    r.native_arsc = &original_table;
    r.java_objects.resources = &resource_marker;
    r.java_objects.asset_manager = &asset_marker;
    BsDexHorizonJavaAttachRuntime runtime = {0};
    BsDexValue value = {0xFEEDu, BS_DEX_INT};

    /* A live native ARSC with missing Activity.attach is never enough. */
    allow_resources = allow_type = 1;
    assert(!bs_dex_horizon_java_attach_runtime_bind(&runtime, &a, &r));
    assert(!runtime.attachment);
    allow_attachment = 1;
    assert(bs_dex_horizon_java_attach_runtime_bind(&runtime, &a, &r));

    /* Independently observed receiver identity is mandatory on EVERY call. */
    assert(!invoke(&runtime, "getBoolean", "(I)Z", 0x7f040001u,
                   0x6e, &value));
    assert(value.bits == 0xFEEDu);
    guest_receiver = &resource_marker;
    assert(invoke(&runtime, "getBoolean", "(I)Z", 0x7f040001u,
                  0x6e, &value) && value.kind == BS_DEX_INT && value.bits == 1);
    assert(invoke(&runtime, "getInteger", "(I)I", 0x7f090000u,
                  0x74, &value) && value.bits == 220);
    assert(invoke(&runtime, "getString", "(I)Ljava/lang/String;",
                  0x7f0d0021u, 0x6e, &value));
    assert(value.kind == BS_DEX_REF && value.bits == 0xBEEFu);
    assert(!strcmp(guest_string, "Beat Saber"));

    /* Original APK types/qualifiers cannot be guessed or coerced. */
    assert(!invoke(&runtime, "getBoolean", "(I)Z", 0x7f090000u,
                   0x6e, &value));
    assert(!invoke(&runtime, "getBoolean", "(I)Z", 0x7f040000u,
                   0x6e, &value)); /* unknown device orientation */
    assert(!invoke(&runtime, "getString", "(I)Ljava/lang/String;",
                   0x7f0c0000u, 0x6e, &value)); /* density unsupported */
    assert(!invoke(&runtime, "getString", "(I)Ljava/lang/String;",
                   0x7f0d0021u, 0x70, &value)); /* wrong opcode */
    assert(!invoke(&runtime, "getString", "(I)I", 0x7f0d0021u,
                   0x6e, &value)); /* wrong prototype */

    allow_type = 0;
    assert(!invoke(&runtime, "getInteger", "(I)I", 0x7f090000u,
                   0x6e, &value));
    allow_type = 1;
    allow_resources = 0;
    assert(!invoke(&runtime, "getInteger", "(I)I", 0x7f090000u,
                   0x6e, &value));
    allow_resources = 1;
    ++r.generation;
    assert(!invoke(&runtime, "getInteger", "(I)I", 0x7f090000u,
                   0x6e, &value));
    --r.generation;
    allow_attachment = 0;
    assert(!invoke(&runtime, "getInteger", "(I)I", 0x7f090000u,
                   0x6e, &value));
    bs_dex_horizon_java_attach_runtime_clear(&runtime);
    assert(!runtime.attachment);
    bs_quest_arsc_release(&original_table);
    puts("original APK Resources VM adapter: strict dispatch and attachment gate passed");
    return 0;
}
