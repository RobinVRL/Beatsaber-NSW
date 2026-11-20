/* Host-only original DEX execution test. `validate_test_activity` is a mock
 * used ONLY for disassembly/interpreter tests. It does not prove a real Java
 * Activity exists. The interpreter must stop on the first missing platform
 * service, preserving the exact frame and original bytecode PC. */
#define _CRT_SECURE_NO_WARNINGS 1
#include "../source/unity_dex_vm.h"
#include "../source/unity_java_preinit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_activity;
static int game_library_registered;
static BsDexValue constructed_activity;
static int supply_platform_activity;

static int load_registered_game(void *user, BsDexVM *vm, const char *klass,
                                const char *name, const char *signature,
                                uint8_t opcode, const BsDexValue *args,
                                size_t count, BsDexValue *result,
                                const char **exception) {
    (void)user;
    (void)result;
    (void)exception;
    if (supply_platform_activity && count == 1 && opcode == 0x70 &&
        !strcmp(klass, "Landroid/app/Activity;") && !strcmp(name, "<init>") &&
        !strcmp(signature, "()V")) {
        assert(bs_dex_bind_original_object(vm, args[0], &test_activity));
        constructed_activity = args[0];
        return 1;
    }
    if (!game_library_registered || count != 1 || opcode != 0x71 ||
        strcmp(klass, "Ljava/lang/System;") || strcmp(name, "loadLibrary") ||
        strcmp(signature, "(Ljava/lang/String;)V")) return 0;
    const char *library = bs_dex_string(vm, args[0]);
    return library && !strcmp(library, "game");
}

static int validate_test_activity(void *ignored, void *instance) {
    (void)ignored;
    return instance == &test_activity;
}

static int activity_instanceof(void *user, BsDexVM *vm, BsDexValue value,
                               const char *descriptor, int *result,
                               const char **exception) {
    (void)user;
    (void)exception;
    /* The host fixture has already independently validated the exact live
     * UnityPlayerGameActivity. Android Activity is also a Context through
     * the real Android platform chain (Activity->ContextThemeWrapper->
     * ContextWrapper->Context), which the original APK DEX does not define.
     * The test host must report this concrete external relationship rather
     * than returning a false ClassCastException for its own Activity. */
    *result = bs_dex_host_pointer(vm, value) == &test_activity &&
        (!strcmp(descriptor,
                 "Lcom/unity3d/player/UnityPlayerGameActivity;") ||
         !strcmp(descriptor, "Landroid/app/Activity;") ||
         !strcmp(descriptor, "Landroid/content/Context;"));
    return 1;
}

static int thrown_calls;
static int malformed_exception;

/* Test-only lifecycle authority: the weak token carries no Activity pointer,
 * JNI strong global, or BsDexValue. The host alone decides whether the
 * earlier registered Activity is still live at the instant of each get(). */
#define TEST_WEAK_LIMIT 16
static unsigned char test_weak_token[TEST_WEAK_LIMIT];
static int test_weak_alive, test_weak_created, test_weak_resolved;
static int test_weak_cleared[TEST_WEAK_LIMIT];
static int test_weak_released_per[TEST_WEAK_LIMIT];
static int test_weak_clear_calls, test_weak_released;
static int test_weak_bad_return;
/* Scalar weak identities are not strong host/JNI references. An independent
 * witness controls when each externally backed String referent dies. */
static uintptr_t test_weak_string_identity[TEST_WEAK_LIMIT];
static int test_weak_string_alive[TEST_WEAK_LIMIT];
static int test_map_key_backings[3];
static uint64_t test_monotonic_reading = UINT64_C(1234567890123456);
static int test_monotonic_calls;
static int test_monotonic_available = 1;
/* Test double for a live host-backed main event loop and independently
 * observed native thread IDs. This deliberately creates no Android Looper,
 * Handler or Runnable scheduling services in the host. */
static int test_main_loop_available = 1;
static int test_main_loop_calls;
static uintptr_t test_main_queue = (uintptr_t)UINT64_C(0x123456);
static uint64_t test_main_owner_thread = UINT64_C(0x9abc);
static uint64_t test_calling_thread = UINT64_C(0x9abc);
static int test_handler_native;
static BsDexValue test_handler_ref;
static int test_handler_alloc_calls, test_handler_ctor_calls;
static int test_handler_unexpected_calls, test_handler_refuse_constructor;
static int test_handler_replace_queue_during_constructor;
static int test_handler_flip_thread_after_observations;

static int test_observe_main_looper(void *user, uintptr_t *queue,
                                    uint64_t *owner_thread,
                                    uint64_t *current_thread) {
    (void)user;
    if (!test_main_loop_available || !queue ||
        !owner_thread || !current_thread) return 0;
    *queue = test_main_queue;
    *owner_thread = test_main_owner_thread;
    ++test_main_loop_calls;
    *current_thread = test_calling_thread;
    if (test_handler_flip_thread_after_observations &&
        test_main_loop_calls > test_handler_flip_thread_after_observations)
        *current_thread = test_main_owner_thread + 1u;
    return 1;
}

static void reset_handler_test_fixture(void) {
    test_handler_native = 0;
    test_handler_ref = bs_dex_null();
    test_handler_alloc_calls = 0;
    test_handler_ctor_calls = 0;
    test_handler_unexpected_calls = 0;
    test_handler_refuse_constructor = 0;
    test_handler_replace_queue_during_constructor = 0;
    test_handler_flip_thread_after_observations = 0;
    test_main_loop_calls = 0;
    test_main_loop_available = 1;
    test_main_queue = (uintptr_t)UINT64_C(0x123456);
    test_main_owner_thread = UINT64_C(0x9abc);
    test_calling_thread = UINT64_C(0x9abc);
}

static int test_handler_platform_new(void *user, BsDexVM *vm,
                                     const char *descriptor, BsDexValue *out,
                                     const char **exception) {
    (void)user;
    (void)exception;
    if (strcmp(descriptor, "Landroid/os/Handler;")) return 0;
    ++test_handler_alloc_calls;
    *out = bs_dex_wrap_platform(vm, &test_handler_native, descriptor);
    test_handler_ref = *out;
    return out->bits != 0;
}

static int test_handler_platform_invoke(void *user, BsDexVM *vm,
                                        const char *klass, const char *name,
                                        const char *signature, uint8_t opcode,
                                        const BsDexValue *args, size_t count,
                                        BsDexValue *result,
                                        const char **exception) {
    if (!strcmp(klass, "Landroid/os/Handler;")) {
        if (strcmp(name, "<init>") || strcmp(signature, "()V") ||
            opcode != 0x70 || count != 1) {
            /* Deliberately permissive callback: VM must veto unsupported
             * Handler methods before this test stub could report success. */
            ++test_handler_unexpected_calls;
            *result = bs_dex_int(1);
            return 1;
        }
        assert(bs_dex_host_pointer(vm, args[0]) == &test_handler_native &&
               args[0].bits == test_handler_ref.bits);
        BsDexHandlerSnapshot before = {0};
        assert(bs_dex_vm_handler_snapshot(vm, args[0], &before) &&
               before.allocated && !before.initialized &&
               before.native_main_queue_identity == test_main_queue &&
               before.native_main_owner_thread_id == test_main_owner_thread);
        ++test_handler_ctor_calls;
        if (test_handler_refuse_constructor) return 0;
        test_handler_native = 1;
        if (test_handler_replace_queue_during_constructor)
            ++test_main_queue;
        *result = bs_dex_null();
        return 1;
    }
    return load_registered_game(user, vm, klass, name, signature, opcode,
                                args, count, result, exception);
}

static int test_monotonic_nanoseconds(void *user, uint64_t *nanoseconds) {
    (void)user;
    if (!test_monotonic_available || !nanoseconds) return 0;
    *nanoseconds = test_monotonic_reading;
    ++test_monotonic_calls;
    test_monotonic_reading += UINT64_C(100);
    return 1;
}

static void reset_weak_fixture_after_destroy(void) {
    assert(test_weak_released == test_weak_created);
    memset(test_weak_cleared, 0, sizeof test_weak_cleared);
    memset(test_weak_released_per, 0, sizeof test_weak_released_per);
    memset(test_weak_string_identity, 0, sizeof test_weak_string_identity);
    memset(test_weak_string_alive, 0, sizeof test_weak_string_alive);
    test_weak_created = 0;
    test_weak_released = 0;
    test_weak_clear_calls = 0;
    test_weak_resolved = 0;
}

/* Diagnostic calls exercise the very same VM-owned library dispatcher used
 * by guest Dalvik invoke, but do not count as original guest instructions. */
static BsDexValue expect_deque(BsDexVM *vm, BsDexValue queue,
                               const char *method, const char *signature,
                               const BsDexValue *argument, int expected_rc,
                               const char *expected_exception) {
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    int rc = bs_dex_vm_array_deque_diagnostic_call(vm, queue, method,
                                      signature, argument, &result, &exception);
    assert(rc == expected_rc);
    if (expected_exception) {
        assert(exception && !strcmp(exception, expected_exception));
    } else assert(!exception);
    return result;
}

static BsDexValue expect_atomic(BsDexVM *vm, BsDexValue atomic,
                                const char *method, const char *signature,
                                const BsDexValue *arguments,
                                size_t argument_count, int expected_rc) {
    BsDexValue result = bs_dex_int(0);
    int rc = bs_dex_vm_atomic_integer_diagnostic_call(
        vm, atomic, method, signature, arguments, argument_count, &result);
    assert(rc == expected_rc);
    return result;
}

static BsDexValue expect_random(BsDexVM *vm, BsDexValue random_ref,
                                const char *method, const char *signature,
                                const BsDexValue *arguments, size_t registers,
                                int expected_rc,
                                const char *expected_exception) {
    BsDexValue result = bs_dex_int(0);
    const char *exception = NULL;
    int rc = bs_dex_vm_random_diagnostic_call(vm, random_ref,
                    method, signature, arguments, registers,
                    &result, &exception);
    assert(rc == expected_rc);
    if (expected_exception)
        assert(exception && !strcmp(exception, expected_exception));
    else assert(!exception);
    return result;
}

static BsDexValue expect_bundle(BsDexVM *vm, BsDexValue bundle,
                                const char *method, const char *signature,
                                const BsDexValue *arguments,
                                size_t argument_register_count,
                                int expected_rc) {
    BsDexValue result = bs_dex_null();
    int rc = bs_dex_vm_bundle_diagnostic_call(vm, bundle, method,
                           signature, arguments, argument_register_count,
                           &result);
    assert(rc == expected_rc);
    return result;
}

static BsDexValue expect_executor(BsDexVM *vm, BsDexValue executor,
                                  const char *method, const char *signature,
                                  const BsDexValue *argument,
                                  int expected_rc,
                                  const char *expected_exception) {
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    int rc = bs_dex_vm_executor_diagnostic_call(vm, executor, method,
                                  signature, argument, &result, &exception);
    assert(rc == expected_rc);
    if (expected_exception)
        assert(exception && !strcmp(exception, expected_exception));
    else assert(!exception);
    return result;
}

static BsDexValue expect_hash_map(BsDexVM *vm, BsDexValue map,
                                  const char *method, const char *signature,
                                  const BsDexValue *args, size_t arg_count,
                                  int expected_rc) {
    BsDexValue result = bs_dex_null();
    int rc = bs_dex_vm_hash_map_diagnostic_call(vm, map, method,
                                                signature, args, arg_count,
                                                &result);
    assert(rc == expected_rc);
    return result;
}

static BsDexValue expect_synchronized_map(
    BsDexVM *vm, BsDexValue map, const char *method,
    const char *signature, const BsDexValue *args,
    size_t arg_count, int expected_rc) {
    BsDexValue result = bs_dex_null();
    int rc = bs_dex_vm_synchronized_map_diagnostic_call(
        vm, map, method, signature, args, arg_count, &result);
    assert(rc == expected_rc);
    return result;
}

static BsDexValue expect_weak_hash_map(BsDexVM *vm, BsDexValue map,
                                      const char *method,
                                      const char *signature,
                                      const BsDexValue *args, size_t count,
                                      int expected_rc) {
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    int rc = bs_dex_vm_weak_hash_map_diagnostic_call(
        vm, map, method, signature, args, count, &result, &exception);
    assert(rc == expected_rc);
    assert(!exception);
    return result;
}

static int32_t observed_int(BsDexValue value) {
    assert(value.kind == BS_DEX_INT);
    return (int32_t)(uint32_t)value.bits;
}

static int test_weak_index(void *handle) {
    for (int i = 0; i < test_weak_created; ++i)
        if (handle == &test_weak_token[i]) return i;
    return -1;
}

static int test_create_external_weak(void *user, BsDexVM *vm,
                                     BsDexValue referent, void **handle,
                                     const char **exception) {
    (void)user;
    (void)exception;
    assert(test_weak_created < TEST_WEAK_LIMIT && test_weak_alive);
    assert(referent.kind == BS_DEX_REF && referent.bits);
    int index = test_weak_created++;
    if (referent.bits == constructed_activity.bits) {
        assert(bs_dex_host_pointer(vm, referent) == &test_activity);
    } else {
        void *backing = bs_dex_host_pointer(vm, referent);
        assert(backing == &test_map_key_backings[0] ||
               backing == &test_map_key_backings[1] ||
               backing == &test_map_key_backings[2]);
        assert(!strcmp(bs_dex_reference_type(vm, referent),
                       "Ljava/lang/String;") &&
               bs_dex_string(vm, referent));
        test_weak_string_identity[index] = referent.bits;
        test_weak_string_alive[index] = 1;
    }
    *handle = &test_weak_token[index];
    return 1;
}

static int test_resolve_external_weak(void *user, BsDexVM *vm,
                                      void *handle, BsDexValue *live_or_null,
                                      const char **exception) {
    (void)user;
    (void)vm;
    (void)exception;
    int index = test_weak_index(handle);
    assert(index >= 0 && !test_weak_released_per[index]);
    ++test_weak_resolved;
    if (test_weak_bad_return) {
        *live_or_null = bs_dex_int(123);
    } else if (test_weak_string_identity[index]) {
        *live_or_null = test_weak_string_alive[index] &&
                        !test_weak_cleared[index] ?
            (BsDexValue){test_weak_string_identity[index], BS_DEX_REF} :
            bs_dex_null();
    } else if (test_weak_alive && !test_weak_cleared[index]) {
        /* Return the previously registered VM identity only while the
         * independent host lifecycle witness says the Activity is live. */
        *live_or_null = constructed_activity;
    } else *live_or_null = bs_dex_null();
    return 1;
}

static int test_clear_external_weak(void *user, BsDexVM *vm,
                                    void *handle, const char **exception) {
    (void)user;
    (void)vm;
    (void)exception;
    int index = test_weak_index(handle);
    assert(index >= 0 && !test_weak_released_per[index]);
    ++test_weak_clear_calls;
    test_weak_cleared[index] = 1;
    return 1;
}

static void test_release_external_weak(void *user, BsDexVM *vm,
                                       void *handle) {
    (void)user;
    (void)vm;
    int index = test_weak_index(handle);
    assert(index >= 0 && !test_weak_released_per[index]);
    test_weak_released_per[index] = 1;
    ++test_weak_released;
}

/* A narrow TEST DOUBLE with real C state to verify the DEX interpreter
 * consumes external method return values, field writes and constructors.
 * It deliberately does not stand in for an Android/Quest/Host service. */
typedef struct TestPlatform {
    int view_constructed;
    int editor_constructed;
    int input_connection_constructed;
    int log_messages;
    const char *last_log_tag;
    const char *last_log_message;
    int input_type;
    int action_id;
    int ime_options;
    BsDexValue ime_ref;
    /* Explicitly installed, TEST-ONLY Android Context service. The live
     * Switch production dispatcher does not register this service. */
    int input_method_service_available;
    int input_method_service_reads;
    int view_context_getter_available;
    int view_context_getter_reads;
    BsDexValue view_context;
} TestPlatform;

static TestPlatform platform;
static int dummy_surface, dummy_editor, dummy_input_connection;
static int dummy_input_method_manager;

static int platform_instanceof(void *user, BsDexVM *vm, BsDexValue value,
                               const char *descriptor, int *result,
                               const char **exception) {
    (void)user;
    (void)exception;
    void *backing = bs_dex_host_pointer(vm, value);
    *result = (backing == &dummy_surface &&
               !strcmp(descriptor,
                 "Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;")) ||
              (backing == &dummy_input_connection &&
               !strcmp(descriptor,
                 "Lcom/google/androidgamesdk/gametextinput/InputConnection;")) ||
              (backing == &test_activity &&
               !strcmp(descriptor, "Landroid/app/Activity;"));
    return 1;
}

static int platform_new(void *user, BsDexVM *vm, const char *descriptor,
                        BsDexValue *result, const char **exception) {
    (void)user;
    (void)exception;
    if (strcmp(descriptor, "Landroid/view/inputmethod/EditorInfo;")) return 0;
    platform.input_type = platform.action_id = platform.ime_options = 0;
    platform.editor_constructed = 0;
    *result = bs_dex_wrap_platform(vm, &dummy_editor, descriptor);
    return result->bits ? 1 : 0;
}

static int platform_method(void *user, BsDexVM *vm, const char *klass,
                           const char *name, const char *signature,
                           uint8_t opcode, const BsDexValue *args,
                           size_t count, BsDexValue *result,
                           const char **exception) {
    (void)user;
    (void)opcode;
    (void)exception;
    (void)result;
    /* A View's context is the same Java Context supplied to its successful
     * constructor; this test fixture stores the ORIGINAL VM reference so
     * getContext never creates a duplicate Java identity. */
    if (count == 1 && !strcmp(klass, "Landroid/view/View;") &&
        !strcmp(name, "getContext") &&
        !strcmp(signature, "()Landroid/content/Context;") &&
        opcode == 0x6e && platform.view_constructed &&
        platform.view_context_getter_available &&
        bs_dex_host_pointer(vm, args[0]) == &dummy_surface &&
        bs_dex_host_pointer(vm, platform.view_context) == &test_activity) {
        *result = platform.view_context;
        ++platform.view_context_getter_reads;
        return 1;
    }
    /* Android Context.INPUT_METHOD_SERVICE is the exact String
     * "input_method". In this fixture only, an explicitly installed service
     * returns its stable InputMethodManager object when queried by the
     * original Activity. A missing service must remain a VM host stop. */
    if (count == 2 && !strcmp(klass, "Landroid/content/Context;") &&
        !strcmp(name, "getSystemService") &&
        !strcmp(signature, "(Ljava/lang/String;)Ljava/lang/Object;") &&
        opcode == 0x6e &&
        bs_dex_host_pointer(vm, args[0]) == &test_activity &&
        bs_dex_string(vm, args[1]) &&
        !strcmp(bs_dex_string(vm, args[1]), "input_method")) {
        if (!platform.input_method_service_available) return 0;
        *result = bs_dex_wrap_platform(
            vm, &dummy_input_method_manager,
            "Landroid/view/inputmethod/InputMethodManager;");
        if (!result->bits) return 0;
        ++platform.input_method_service_reads;
        return 1;
    }
    if (count == 2 && !strcmp(klass, "Landroid/view/SurfaceView;") &&
        !strcmp(name, "<init>") &&
        !strcmp(signature, "(Landroid/content/Context;)V") &&
        bs_dex_host_pointer(vm, args[1]) == &test_activity) {
        if (!bs_dex_bind_original_object(vm, args[0], &dummy_surface)) return 0;
        platform.view_constructed = 1;
        platform.view_context = args[1];
        return 1;
    }
    if (count == 1 && !strcmp(klass,
        "Landroid/view/inputmethod/EditorInfo;") && !strcmp(name, "<init>") &&
        !strcmp(signature, "()V") &&
        bs_dex_host_pointer(vm, args[0]) == &dummy_editor) {
        platform.editor_constructed = 1;
        return 1;
    }
    if (count == 3 && !strcmp(klass,
        "Landroid/view/inputmethod/BaseInputConnection;") &&
        !strcmp(name, "<init>") &&
        !strcmp(signature, "(Landroid/view/View;Z)V") &&
        bs_dex_host_pointer(vm, args[1]) == &dummy_surface &&
        platform.view_constructed && args[2].kind == BS_DEX_INT &&
        args[2].bits == 1u &&
        bs_dex_bind_original_object(vm, args[0], &dummy_input_connection)) {
        platform.input_connection_constructed = 1;
        return 1;
    }
    if (count == 2 && !strcmp(klass, "Landroid/util/Log;") &&
        !strcmp(name, "d") &&
        !strcmp(signature,
                "(Ljava/lang/String;Ljava/lang/String;)I")) {
        const char *tag = bs_dex_string(vm, args[0]);
        const char *message = bs_dex_string(vm, args[1]);
        if (!tag || !message || platform.log_messages >= 8) return 0;
        int written = fprintf(stderr, "[test Android Log.d] %s: %s\n",
                              tag, message);
        if (written < 0) return 0;
        ++platform.log_messages;
        platform.last_log_tag = tag;
        platform.last_log_message = message;
        *result = bs_dex_int(written);
        return 1;
    }
    return 0;
}

static int platform_field(void *user, BsDexVM *vm, const char *klass,
                          const char *name, const char *signature,
                          BsDexValue receiver, int write,
                          BsDexValue *value, const char **exception) {
    (void)user;
    (void)exception;
    if (!strcmp(klass, "Lcom/google/androidgamesdk/GameActivity;") &&
        !strcmp(name, "imeEditorInfo") &&
        !strcmp(signature, "Landroid/view/inputmethod/EditorInfo;") &&
        bs_dex_host_pointer(vm, receiver) == &test_activity) {
        if (write) platform.ime_ref = *value;
        else *value = platform.ime_ref;
        return 1;
    }
    if (!strcmp(klass, "Landroid/view/inputmethod/EditorInfo;") &&
        !strcmp(signature, "I") &&
        bs_dex_host_pointer(vm, receiver) == &dummy_editor) {
        int *destination = !strcmp(name, "inputType") ? &platform.input_type :
                           !strcmp(name, "actionId") ? &platform.action_id :
                           !strcmp(name, "imeOptions") ? &platform.ime_options :
                           NULL;
        if (!destination || !platform.editor_constructed) return 0;
        if (write) *destination = (int32_t)value->bits;
        else *value = bs_dex_int(*destination);
        return 1;
    }
    return 0;
}

static int throw_platform_constructor(void *user, BsDexVM *vm,
                                      const char *klass, const char *name,
                                      const char *signature, uint8_t opcode,
                                      const BsDexValue *args, size_t count,
                                      BsDexValue *result,
                                      const char **exception) {
    (void)user;
    (void)result;
    assert(klass && !strcmp(klass, "Landroid/view/SurfaceView;"));
    assert(!strcmp(name, "<init>"));
    assert(!strcmp(signature, "(Landroid/content/Context;)V"));
    assert(opcode == 0x70);
    assert(count == 2 && args[0].bits && args[1].bits);
    assert(!strcmp(bs_dex_reference_type(vm, args[0]),
                   "Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;"));
    assert(bs_dex_host_pointer(vm, args[0]) == NULL);
    assert(bs_dex_host_pointer(vm, args[1]) == &test_activity);
    /* No real View was created. A guessed binding is refused, and the
     * callback supplies a real exception report for the test VM only. */
    assert(bs_dex_bind_original_object(vm, args[0], &test_activity) == 0);
    *exception = malformed_exception ? NULL : "Ljava/lang/IllegalStateException;";
    ++thrown_calls;
    return -1;
}

static uint8_t *load_dex(const char *name, size_t *length) {
    FILE *f = fopen(name, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long count = ftell(f);
    assert(count == BS_UNITY_JAVA_DEX_SIZE);
    assert(!fseek(f, 0, SEEK_SET));
    uint8_t *bytes = malloc((size_t)count);
    assert(bytes && fread(bytes, 1, (size_t)count, f) == (size_t)count);
    assert(!fclose(f));
    *length = (size_t)count;
    return bytes;
}

/* All cases execute the ACTUAL Quest classes.dex from its constructor
 * entry. The fake native observer supplies an explicit queue/owner witness
 * for this host-only test; it cannot run a Java Handler or schedule work.
 * No test result is used as evidence of a real Horizon Handler service. */
static void run_original_handler_fixture(const uint8_t *dex, size_t size,
                                          BsDexHost host, int scenario) {
    reset_handler_test_fixture();
    /* The real original DEX has 18 previous native Looper observations.
     * Flip at the first Handler allocation (scenario 1), or after that
     * allocation but before the constructor (scenario 4). */
    if (scenario == 1) test_handler_flip_thread_after_observations = 18;
    if (scenario == 4) test_handler_flip_thread_after_observations = 19;
    if (scenario == 2) test_handler_refuse_constructor = 1;
    if (scenario == 3)
        test_handler_replace_queue_during_constructor = 1;
    host.observe_main_looper = test_observe_main_looper;
    host.new_platform_object = test_handler_platform_new;
    host.invoke_platform = test_handler_platform_invoke;
    test_weak_alive = 1;
    BsDexVM *vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    /* A premature Activity.onCreate cannot be placed above the actual
     * executing constructor/class-initializer frames. */
    assert(!bs_dex_vm_start_activity_oncreate(
               vm, &test_activity, bs_dex_null()));
    BsDexStop stop = bs_dex_vm_run(vm, 10000);
    printf("Handler host fixture scenario=%d steps=%u code=0x%x pc=0x%x "
           "opcode=0x%x host=%s.%s detail=%s loop_observations=%d\n",
           scenario, stop.steps, stop.code_off, stop.dex_pc, stop.opcode,
           stop.host_class ? stop.host_class : "(none)",
           stop.host_method ? stop.host_method : "(none)",
           stop.detail ? stop.detail : "(none)", test_main_loop_calls);
    fflush(stdout);
    assert(stop.kind == (scenario == 0 ? BS_DEX_STOP_RETURNED :
                                       BS_DEX_STOP_HOST_MISSING));
    assert(test_handler_unexpected_calls == 0);
    assert(!bs_dex_vm_current_activity(vm).bits);
    if (scenario == 0) {
        /* Original Activity constructor has now returned through its
         * unmodified real DEX bytecode, with zero synthetic completion. */
        assert(stop.steps == 2287 && stop.depth == 0);
        /* Continue from that same constructor-complete VM into its actual
         * original onCreate bytecode before any diagnostic map mutations.
         * This must identify the next independently missing Android/Java
         * service instead of assuming a Unity readiness checkpoint. */
        assert(bs_dex_vm_start_activity_oncreate(
                   vm, &test_activity, bs_dex_null()));
        assert(!bs_dex_vm_start_activity_oncreate(
                   vm, &test_activity, bs_dex_null()));
        BsDexStop after_constructor = bs_dex_vm_run(vm, 10000);
        printf("constructor-complete original onCreate: steps=%u "
               "kind=%d code=0x%x pc=0x%x opcode=0x%x host=%s.%s\n",
               after_constructor.steps, after_constructor.kind,
               after_constructor.code_off, after_constructor.dex_pc,
               after_constructor.opcode,
               after_constructor.host_class ?
                   after_constructor.host_class : "(none)",
               after_constructor.host_method ?
                   after_constructor.host_method : "(none)");
        fflush(stdout);
        assert(after_constructor.kind == BS_DEX_STOP_HOST_MISSING &&
               after_constructor.steps == 2296 &&
               after_constructor.code_off == 0x22e9d8 &&
               after_constructor.dex_pc == 0x2 &&
               after_constructor.opcode == 0x70);
        assert(after_constructor.host_class &&
               !strcmp(after_constructor.host_class,
                       "Landroid/view/SurfaceView;"));
        assert(after_constructor.host_method &&
               !strcmp(after_constructor.host_method, "<init>"));
        assert(test_handler_alloc_calls == 1 && test_handler_ctor_calls == 1);
        BsDexHandlerSnapshot snapshot = {0};
        assert(test_handler_ref.bits &&
               bs_dex_vm_handler_snapshot(vm, test_handler_ref, &snapshot));
        assert(snapshot.allocated && snapshot.initialized &&
               snapshot.native_main_queue_identity == test_main_queue &&
               snapshot.native_main_owner_thread_id == test_main_owner_thread);
        /* All non-constructor Handler methods remain unavailable, even
         * though the generic test host would incorrectly accept them. */
        assert(!bs_dex_vm_handler_diagnostic_method_gate(
                   vm, test_handler_ref, "Landroid/os/Handler;", "post",
                   "(Ljava/lang/Runnable;)Z", 0x6e, 2));
        assert(!bs_dex_vm_handler_diagnostic_method_gate(
                   vm, test_handler_ref, "Landroid/os/Handler;",
                   "removeCallbacks", "(Ljava/lang/Runnable;)V", 0x6e, 2));
        assert(!bs_dex_vm_handler_diagnostic_method_gate(
                   vm, test_handler_ref, "Landroid/os/Handler;",
                   "getLooper", "()Landroid/os/Looper;", 0x6e, 1));
        assert(!bs_dex_vm_handler_diagnostic_method_gate(
                   vm, test_handler_ref, "Landroid/os/Handler;",
                   "<init>", "()V", 0x70, 1));
        /* The original FragmentManager constructor has executed three
         * Collections.synchronizedMap(new HashMap()) calls. Each wrapper
         * is a distinct Map object with its own monitor mutex (= self),
         * and a different original VM-owned HashMap delegate. No internal
         * Collections$SynchronizedMap class is fabricated in the DEX. */
        BsDexValue synchronized[3];
        BsDexSynchronizedMapSnapshot sync[3] = {{0}};
        for (size_t i = 0; i < 3; ++i) {
            synchronized[i] = bs_dex_vm_synchronized_map_at(vm, i);
            assert(synchronized[i].bits &&
                   !strcmp(bs_dex_reference_type(vm, synchronized[i]),
                           "Ljava/util/Map;"));
            assert(bs_dex_vm_synchronized_map_snapshot(
                       vm, synchronized[i], &sync[i]));
            assert(sync[i].delegate.bits &&
                   !strcmp(bs_dex_reference_type(vm, sync[i].delegate),
                           "Ljava/util/HashMap;"));
            assert(sync[i].mutex.bits == synchronized[i].bits &&
                   sync[i].monitor_depth == 0);
        }
        assert(!bs_dex_vm_synchronized_map_at(vm, 3).bits);
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = i + 1; j < 3; ++j)
                assert(synchronized[i].bits != synchronized[j].bits &&
                       sync[i].delegate.bits != sync[j].delegate.bits);

        BsDexValue key_fb = bs_dex_new_observed_string_utf8(vm, "FB", 2);
        BsDexValue key_fb_equal =
            bs_dex_new_observed_string_utf8(vm, "FB", 2);
        BsDexValue key_ea = bs_dex_new_observed_string_utf8(vm, "Ea", 2);
        BsDexValue value_one =
            bs_dex_new_observed_string_utf8(vm, "first", 5);
        BsDexValue value_two =
            bs_dex_new_observed_string_utf8(vm, "second", 6);
        assert(key_fb.bits && key_fb_equal.bits && key_ea.bits &&
               value_one.bits && value_two.bits);
        BsDexValue put_fb[2] = {key_fb, value_one};
        BsDexValue put_ea[2] = {key_ea, value_two};
        assert(!expect_synchronized_map(vm, synchronized[0], "put",
               "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               put_fb, 2, 1).bits);
        assert(!expect_synchronized_map(vm, synchronized[0], "put",
               "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               put_ea, 2, 1).bits);
        assert(expect_synchronized_map(vm, synchronized[0], "get",
               "(Ljava/lang/Object;)Ljava/lang/Object;",
               &key_fb_equal, 1, 1).bits == value_one.bits);
        assert(expect_hash_map(vm, sync[0].delegate, "get",
               "(Ljava/lang/Object;)Ljava/lang/Object;",
               &key_ea, 1, 1).bits == value_two.bits);
        assert(observed_int(expect_synchronized_map(vm, synchronized[0],
               "size", "()I", NULL, 0, 1)) == 2);
        assert(observed_int(expect_synchronized_map(vm, synchronized[1],
               "size", "()I", NULL, 0, 1)) == 0);
        assert(observed_int(expect_synchronized_map(vm, synchronized[2],
               "isEmpty", "()Z", NULL, 0, 1)) == 1);
        BsDexValue update_fb[2] = {key_fb_equal, value_two};
        assert(expect_synchronized_map(vm, synchronized[0], "put",
               "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               update_fb, 2, 1).bits == value_one.bits);
        assert(observed_int(expect_hash_map(vm, sync[0].delegate, "size",
               "()I", NULL, 0, 1)) == 2);
        assert(expect_synchronized_map(vm, synchronized[0], "remove",
               "(Ljava/lang/Object;)Ljava/lang/Object;",
               &key_ea, 1, 1).bits == value_two.bits);
        BsDexValue null_key_put[2] = {bs_dex_null(), value_one};
        assert(!expect_synchronized_map(vm, synchronized[0], "put",
               "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
               null_key_put, 2, 1).bits);
        assert(expect_synchronized_map(vm, synchronized[0], "get",
               "(Ljava/lang/Object;)Ljava/lang/Object;",
               null_key_put, 1, 1).bits == value_one.bits);
        expect_synchronized_map(vm, synchronized[0], "get",
               "(Ljava/lang/Object;)Ljava/lang/Object;",
               &constructed_activity, 1, -4);
        expect_synchronized_map(vm, synchronized[0], "keySet",
               "()Ljava/util/Set;", NULL, 0, 0);
        expect_synchronized_map(vm, synchronized[0], "entrySet",
               "()Ljava/util/Set;", NULL, 0, 0);
        expect_synchronized_map(vm, synchronized[0], "clear",
               "()V", NULL, 0, 1);
        assert(observed_int(expect_synchronized_map(vm, synchronized[0],
               "isEmpty", "()Z", NULL, 0, 1)) == 1);
        BsDexSynchronizedMapSnapshot after_sync = {0};
        assert(bs_dex_vm_synchronized_map_snapshot(
                   vm, synchronized[0], &after_sync) &&
               after_sync.monitor_depth == 0 &&
               after_sync.delegate.bits == sync[0].delegate.bits &&
               after_sync.mutex.bits == synchronized[0].bits);
        BsDexStop still_at_guest_boundary = bs_dex_vm_stop(vm);
        assert(still_at_guest_boundary.steps == after_constructor.steps &&
               still_at_guest_boundary.code_off ==
                   after_constructor.code_off &&
               still_at_guest_boundary.dex_pc == after_constructor.dex_pc);
        /* A later native thread migration or a changed Looper generation
         * cannot reauthorize a constructor on this already initialized
         * Handler, or any of its unsupported service methods. */
        test_calling_thread = test_main_owner_thread + 1u;
        assert(!bs_dex_vm_handler_diagnostic_method_gate(
                   vm, test_handler_ref, "Landroid/os/Handler;",
                   "<init>", "()V", 0x70, 1));
    } else if (scenario == 1) {
        assert(stop.steps == 1902 && stop.code_off == 0x204e50 &&
               stop.dex_pc == 0 && stop.opcode == 0x22);
        assert(test_handler_alloc_calls == 0 &&
               test_handler_ctor_calls == 0 &&
               !test_handler_ref.bits);
    } else if (scenario == 2 || scenario == 3) {
        assert(stop.steps == 1903 && stop.code_off == 0x204e50 &&
               stop.dex_pc == 2 && stop.opcode == 0x70);
        assert(test_handler_alloc_calls == 1 &&
               test_handler_ctor_calls == 1);
        BsDexHandlerSnapshot snapshot = {0};
        assert(bs_dex_vm_handler_snapshot(vm, test_handler_ref, &snapshot));
        assert(snapshot.allocated && !snapshot.initialized);
    } else if (scenario == 4) {
        assert(stop.steps == 1903 && stop.code_off == 0x204e50 &&
               stop.dex_pc == 2 && stop.opcode == 0x70);
        assert(test_handler_alloc_calls == 1 &&
               test_handler_ctor_calls == 0);
        BsDexHandlerSnapshot snapshot = {0};
        assert(bs_dex_vm_handler_snapshot(vm, test_handler_ref, &snapshot) &&
               snapshot.allocated && !snapshot.initialized);
    }
    bs_dex_vm_destroy(vm);
    reset_weak_fixture_after_destroy();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t size = 0;
    uint8_t *dex = load_dex(argv[1], &size);
    BsDexHost host = {
        .validate_activity = validate_test_activity,
        .invoke_platform = load_registered_game,
        .instance_of = activity_instanceof,
    };
    BsDexVM *vm = bs_dex_vm_create(dex, size, &host);
    assert(vm);
    assert(bs_dex_vm_start_activity_constructor(vm));
    assert(!bs_dex_vm_start_activity_constructor(vm));
    BsDexStop constructor_stop = bs_dex_vm_run(vm, 10000);
    printf("original Activity construction: steps=%u stop=%d code=0x%x "
           "pc=0x%x host=%s.%s\n", constructor_stop.steps,
           constructor_stop.kind, constructor_stop.code_off,
           constructor_stop.dex_pc,
           constructor_stop.host_class ? constructor_stop.host_class : "(none)",
           constructor_stop.host_method ? constructor_stop.host_method : "(none)");
    assert(constructor_stop.kind == BS_DEX_STOP_HOST_MISSING);
    assert(constructor_stop.steps == 2);
    assert(constructor_stop.code_off == 0x237898 &&
           constructor_stop.dex_pc == 0x2);
    assert(constructor_stop.host_class &&
           !strcmp(constructor_stop.host_class, "Ljava/lang/System;"));
    assert(constructor_stop.host_method &&
           !strcmp(constructor_stop.host_method, "loadLibrary"));
    game_library_registered = 1;
    BsDexStop resumed = bs_dex_vm_run(vm, 10000);
    printf("resumed Activity construction: steps=%u stop=%d code=0x%x "
           "pc=0x%x host=%s.%s\n", resumed.steps, resumed.kind,
           resumed.code_off, resumed.dex_pc,
           resumed.host_class ? resumed.host_class : "(none)",
           resumed.host_method ? resumed.host_method : "(none)");
    assert(resumed.kind == BS_DEX_STOP_HOST_MISSING);
    assert(resumed.steps == 9 && resumed.code_off == 0x1a5550 &&
           resumed.dex_pc == 0x0);
    assert(resumed.host_class &&
           !strcmp(resumed.host_class, "Landroid/app/Activity;"));
    assert(resumed.host_method && !strcmp(resumed.host_method, "<init>"));
    assert(!bs_dex_vm_start_activity_oncreate(vm, &test_activity, bs_dex_null()));
    supply_platform_activity = 1;
    BsDexStop attached = bs_dex_vm_run(vm, 10000);
    printf("Activity base constructor fixture: steps=%u stop=%d code=0x%x "
           "pc=0x%x host=%s.%s\n", attached.steps, attached.kind,
           attached.code_off, attached.dex_pc,
           attached.host_class ? attached.host_class : "(none)",
           attached.host_method ? attached.host_method : "(none)");
    /* ArrayList allocation and constructor, then WeakReference allocation,
     * are interpreted in sequence. WeakReference.<init> is deliberately
     * delegated for the real externally hosted Activity referent because
     * the VM has no authority over Android garbage collection. */
    assert(attached.kind == BS_DEX_STOP_HOST_MISSING &&
           attached.steps == 62 && attached.code_off == 0x2183b0 &&
           attached.dex_pc == 0x1a && attached.opcode == 0x70);
    assert(attached.host_class &&
           !strcmp(attached.host_class, "Ljava/lang/ref/WeakReference;"));
    assert(attached.host_method && !strcmp(attached.host_method, "<init>"));
    assert(constructed_activity.bits);
    assert(bs_dex_host_pointer(vm, constructed_activity) == &test_activity);
    assert(!bs_dex_vm_start_activity_oncreate(vm, &test_activity, bs_dex_null()));
    assert(!bs_dex_vm_current_activity(vm).bits);
    bs_dex_vm_destroy(vm);
    /* Execute the same original constructor with the explicit non-owning
     * weak contract. Its first get must return the exact live Activity,
     * then NULL when host lifetime invalidates despite a tracked VM ref. */
    host.weak_reference_create = test_create_external_weak;
    host.weak_reference_get = test_resolve_external_weak;
    host.weak_reference_clear = test_clear_external_weak;
    host.weak_reference_release = test_release_external_weak;
    /* Missing even one lifetime operation must block construction before
     * the host is allowed to create a partially usable weak handle. */
    host.weak_reference_get = NULL;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop weak_incomplete = bs_dex_vm_run(vm, 10000);
    assert(weak_incomplete.kind == BS_DEX_STOP_HOST_MISSING &&
           weak_incomplete.steps == 62 &&
           weak_incomplete.code_off == 0x2183b0 &&
           weak_incomplete.dex_pc == 0x1a);
    assert(test_weak_created == 0 && test_weak_released == 0);
    bs_dex_vm_destroy(vm);
    host.weak_reference_get = test_resolve_external_weak;
    test_weak_alive = 1;
    /* Without a genuine monotonic clock callback, original new-instance
     * must allocate the Java object but its Random() constructor must STOP
     * without inventing a seed or advancing another guest instruction. */
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop no_clock = bs_dex_vm_run(vm, 10000);
    assert(no_clock.kind == BS_DEX_STOP_HOST_MISSING &&
           no_clock.steps == 254 && no_clock.code_off == 0xed804 &&
           no_clock.dex_pc == 0x5 && no_clock.opcode == 0x70);
    assert(no_clock.host_class &&
           !strcmp(no_clock.host_class, "Ljava/util/Random;"));
    assert(no_clock.host_method &&
           !strcmp(no_clock.host_method, "<init>"));
    assert(!bs_dex_vm_random_at(vm, 0).bits);
    assert(test_monotonic_calls == 0);
    bs_dex_vm_destroy(vm);
    reset_weak_fixture_after_destroy();
    /* A callback that exists but has no reading is equally unavailable.
     * The constructor may not claim success or generate a substitute seed. */
    host.monotonic_nanoseconds = test_monotonic_nanoseconds;
    test_monotonic_available = 0;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop clock_unavailable = bs_dex_vm_run(vm, 10000);
    assert(clock_unavailable.kind == BS_DEX_STOP_HOST_MISSING &&
           clock_unavailable.steps == 254 &&
           clock_unavailable.code_off == 0xed804 &&
           clock_unavailable.dex_pc == 0x5 &&
           clock_unavailable.opcode == 0x70);
    assert(!bs_dex_vm_random_at(vm, 0).bits);
    assert(test_monotonic_calls == 0);
    bs_dex_vm_destroy(vm);
    reset_weak_fixture_after_destroy();
    test_monotonic_available = 1;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop weak_advanced = bs_dex_vm_run(vm, 10000);
    printf("external Activity weak fixture: steps=%u stop=%d code=0x%x "
           "pc=0x%x opcode=0x%x host=%s.%s\n",
           weak_advanced.steps, weak_advanced.kind, weak_advanced.code_off,
           weak_advanced.dex_pc, weak_advanced.opcode,
           weak_advanced.host_class ? weak_advanced.host_class : "(none)",
           weak_advanced.host_method ? weak_advanced.host_method : "(none)");
    fflush(stdout);
    assert(test_weak_created >= 2); /* independent weak handles, same Activity */
    assert(weak_advanced.kind == BS_DEX_STOP_HOST_MISSING &&
           weak_advanced.steps == 370 &&
           weak_advanced.code_off == 0x12c7e0 &&
           weak_advanced.dex_pc == 0x0 &&
           weak_advanced.opcode == 0x71);
    assert(weak_advanced.host_class &&
           !strcmp(weak_advanced.host_class,
                   "Landroid/os/Looper;"));
    assert(weak_advanced.host_method &&
           !strcmp(weak_advanced.host_method, "getMainLooper"));
    BsDexValue executor = bs_dex_vm_executor_at(vm, 0);
    assert(executor.kind == BS_DEX_REF && executor.bits &&
           !strcmp(bs_dex_reference_type(vm, executor),
                   "Ljava/util/concurrent/ThreadPoolExecutor;"));
    assert(!bs_dex_vm_executor_at(vm, 1).bits);
    BsDexExecutorSnapshot pool = {0};
    assert(bs_dex_vm_executor_snapshot(vm, executor, &pool));
    assert(pool.core_size == 4 && pool.max_size == 4);
    assert(!pool.live_workers && !pool.queued_tasks &&
           !pool.completed_tasks && !pool.shutdown && !pool.terminated);
    assert(pool.java_queue_capacity == INT32_MAX &&
           pool.vm_queue_limit == 8192);
    assert(pool.queue.bits && !strcmp(bs_dex_reference_type(vm, pool.queue),
                            "Ljava/util/concurrent/LinkedBlockingQueue;"));
    assert(pool.thread_factory.bits &&
           !strcmp(bs_dex_reference_type(vm, pool.thread_factory),
                   "Landroidx/arch/core/executor/DefaultTaskExecutor$1;"));
    /* Java's original two-argument overload used the exact DEX constructor
     * for a custom ThreadFactory. No default factory is synthesized. */
    assert(expect_executor(vm, executor, "getThreadFactory",
                    "()Ljava/util/concurrent/ThreadFactory;",
                    NULL, 1, NULL).bits == pool.thread_factory.bits);
    assert(expect_executor(vm, executor, "getQueue",
                    "()Ljava/util/concurrent/BlockingQueue;",
                    NULL, 1, NULL).bits == pool.queue.bits);
    assert(observed_int(expect_executor(vm, pool.queue, "size",
                    "()I", NULL, 1, NULL)) == 0);
    assert(observed_int(expect_executor(vm, pool.queue, "isEmpty",
                    "()Z", NULL, 1, NULL)) == 1);
    assert(observed_int(expect_executor(vm, pool.queue,
                    "remainingCapacity", "()I", NULL, 1, NULL)) ==
           INT32_MAX);
    assert(!expect_executor(vm, pool.queue, "poll",
                    "()Ljava/lang/Object;", NULL, 1, NULL).bits);
    expect_executor(vm, pool.queue, "offer",
                    "(Ljava/lang/Object;)Z",
                    &pool.thread_factory, -4, NULL);
    assert(observed_int(expect_executor(vm, executor, "getCorePoolSize",
                    "()I", NULL, 1, NULL)) == 4);
    assert(observed_int(expect_executor(vm, executor, "getMaximumPoolSize",
                    "()I", NULL, 1, NULL)) == 4);
    assert(observed_int(expect_executor(vm, executor, "getPoolSize",
                    "()I", NULL, 1, NULL)) == 0);
    assert(observed_int(expect_executor(vm, executor, "isShutdown",
                    "()Z", NULL, 1, NULL)) == 0);
    assert(observed_int(expect_executor(vm, executor, "isTerminated",
                    "()Z", NULL, 1, NULL)) == 0);
    BsDexValue task_count = expect_executor(vm, executor, "getTaskCount",
                                            "()J", NULL, 1, NULL);
    assert(task_count.kind == BS_DEX_WIDE && task_count.bits == 0);
    /* Actual Runnable scheduling must stop BEFORE worker creation, queue
     * insertion or a synthetic Future/result is produced. */
    expect_executor(vm, executor, "execute",
                    "(Ljava/lang/Runnable;)V",
                    &pool.thread_factory, -4, NULL);
    expect_executor(vm, executor, "submit",
                    "(Ljava/util/concurrent/Callable;)"
                    "Ljava/util/concurrent/Future;",
                    &pool.thread_factory, -4, NULL);
    BsDexValue null_runnable = bs_dex_null();
    expect_executor(vm, executor, "execute",
                    "(Ljava/lang/Runnable;)V", &null_runnable, -1,
                    "Ljava/lang/NullPointerException;");
    BsDexExecutorSnapshot before_shutdown = {0};
    assert(bs_dex_vm_executor_snapshot(vm, executor, &before_shutdown));
    assert(!before_shutdown.live_workers && !before_shutdown.queued_tasks &&
           !before_shutdown.completed_tasks && !before_shutdown.shutdown);
    /* Incorrect pool sizes, null factories and unrelated java.lang.Class
     * references fail at the real constructor's validation boundary. */
    BsDexValue created = bs_dex_null();
    const char *creation_exception = NULL;
    assert(bs_dex_vm_executor_diagnostic_create(vm, 0,
               pool.thread_factory, &created, &creation_exception) == -1);
    assert(creation_exception &&
           !strcmp(creation_exception, "Ljava/lang/IllegalArgumentException;"));
    assert(!created.bits);
    assert(bs_dex_vm_executor_diagnostic_create(vm, 4, bs_dex_null(),
               &created, &creation_exception) == -1);
    assert(creation_exception &&
           !strcmp(creation_exception, "Ljava/lang/NullPointerException;"));
    assert(bs_dex_vm_executor_diagnostic_create(vm, 65,
               pool.thread_factory, &created, &creation_exception) == -3);
    assert(!created.bits && !creation_exception);
    assert(bs_dex_vm_executor_diagnostic_create(vm, 4, executor,
               &created, &creation_exception) == 0);
    assert(!created.bits && !creation_exception);
    assert(!bs_dex_vm_executor_at(vm, 1).bits);
    expect_executor(vm, executor, "shutdown", "()V", NULL, 1, NULL);
    assert(observed_int(expect_executor(vm, executor, "isShutdown",
                    "()Z", NULL, 1, NULL)) == 1);
    assert(observed_int(expect_executor(vm, executor, "isTerminated",
                    "()Z", NULL, 1, NULL)) == 1);
    expect_executor(vm, executor, "execute",
                    "(Ljava/lang/Runnable;)V",
                    &pool.thread_factory, -1,
                    "Ljava/util/concurrent/RejectedExecutionException;");
    BsDexExecutorSnapshot after_shutdown = {0};
    assert(bs_dex_vm_executor_snapshot(vm, executor, &after_shutdown));
    assert(after_shutdown.shutdown && after_shutdown.terminated &&
           !after_shutdown.live_workers && !after_shutdown.queued_tasks &&
           !after_shutdown.completed_tasks);
    /* Original const-class ArchTaskExecutor was followed by a real monitor
     * enter/exit. Class identity comes from the verified DEX class_def.
     * Neither reflection nor a guessed Android ClassLoader is provided. */
    BsDexValue arch_class = bs_dex_vm_class_literal_at(vm, 0);
    assert(arch_class.kind == BS_DEX_REF && arch_class.bits &&
           !strcmp(bs_dex_reference_type(vm, arch_class),
                   "Ljava/lang/Class;"));
    assert(!strcmp(bs_dex_vm_class_literal_target(vm, arch_class),
                   "Landroidx/arch/core/executor/ArchTaskExecutor;"));
    assert(bs_dex_vm_class_literal_at(vm, 0).bits == arch_class.bits);
    assert(!bs_dex_vm_class_literal_target(vm, bs_dex_null()));
    assert(test_monotonic_calls == 1);
    BsDexValue bundle = bs_dex_vm_bundle_at(vm, 0);
    assert(bundle.bits && !strcmp(bs_dex_reference_type(vm, bundle),
                                  "Landroid/os/Bundle;"));
    assert(!bs_dex_vm_bundle_at(vm, 1).bits);
    assert(observed_int(expect_bundle(vm, bundle, "size", "()I",
                                     NULL, 0, 1)) == 0);
    assert(observed_int(expect_bundle(vm, bundle, "isEmpty", "()Z",
                                     NULL, 0, 1)) == 1);
    BsDexValue key_i = bs_dex_new_observed_string_utf8(vm, "count", 5);
    BsDexValue key_i_again = bs_dex_new_observed_string_utf8(vm, "count", 5);
    BsDexValue key_b = bs_dex_new_observed_string_utf8(vm, "enabled", 7);
    BsDexValue key_s = bs_dex_new_observed_string_utf8(vm, "text", 4);
    BsDexValue key_j = bs_dex_new_observed_string_utf8(vm, "long", 4);
    BsDexValue key_other = bs_dex_new_observed_string_utf8(vm, "missing", 7);
    BsDexValue value_s = bs_dex_new_observed_string_utf8(vm, "original", 8);
    assert(key_i.bits && key_i_again.bits && key_b.bits && key_s.bits &&
           key_j.bits && key_other.bits && value_s.bits);
    BsDexValue count_args[2] = {key_i, bs_dex_int(92)};
    expect_bundle(vm, bundle, "putInt", "(Ljava/lang/String;I)V",
                  count_args, 2, 1);
    assert(observed_int(expect_bundle(vm, bundle, "getInt",
                "(Ljava/lang/String;)I", &key_i_again, 1, 1)) == 92);
    assert(observed_int(expect_bundle(vm, bundle, "containsKey",
                "(Ljava/lang/String;)Z", &key_i_again, 1, 1)) == 1);
    BsDexValue boolean_args[2] = {key_b, bs_dex_int(1)};
    expect_bundle(vm, bundle, "putBoolean",
                  "(Ljava/lang/String;Z)V", boolean_args, 2, 1);
    assert(observed_int(expect_bundle(vm, bundle, "getBoolean",
                "(Ljava/lang/String;)Z", &key_b, 1, 1)) == 1);
    BsDexValue string_args[2] = {key_s, value_s};
    expect_bundle(vm, bundle, "putString",
                  "(Ljava/lang/String;Ljava/lang/String;)V",
                  string_args, 2, 1);
    assert(expect_bundle(vm, bundle, "getString",
                "(Ljava/lang/String;)Ljava/lang/String;",
                &key_s, 1, 1).bits == value_s.bits);
    BsDexValue long_args[3] = {
        key_j, {(uintptr_t)UINT64_C(0x1122334455667788), BS_DEX_WIDE},
        bs_dex_int(0)
    };
    expect_bundle(vm, bundle, "putLong", "(Ljava/lang/String;J)V",
                  long_args, 3, 1);
    BsDexValue got_long = expect_bundle(vm, bundle, "getLong",
                "(Ljava/lang/String;)J", &key_j, 1, 1);
    assert(got_long.kind == BS_DEX_WIDE &&
           got_long.bits == long_args[1].bits);
    assert(observed_int(expect_bundle(vm, bundle, "size", "()I",
                                     NULL, 0, 1)) == 4);
    assert(observed_int(expect_bundle(vm, bundle, "getInt",
                "(Ljava/lang/String;)I", &key_other, 1, 1)) == 0);
    BsDexValue missing_default[2] = {key_other, bs_dex_int(573)};
    assert(observed_int(expect_bundle(vm, bundle, "getInt",
                "(Ljava/lang/String;I)I", missing_default, 2, 1)) == 573);
    assert(!expect_bundle(vm, bundle, "getString",
                "(Ljava/lang/String;)Ljava/lang/String;",
                &key_other, 1, 1).bits);
    BsDexValue missing_string_default[2] = {key_other, value_s};
    assert(expect_bundle(vm, bundle, "getString",
                "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                missing_string_default, 2, 1).bits == value_s.bits);
    assert(observed_int(expect_bundle(vm, bundle, "getBoolean",
                "(Ljava/lang/String;)Z", &key_other, 1, 1)) == 0);
    got_long = expect_bundle(vm, bundle, "getLong",
                "(Ljava/lang/String;)J", &key_other, 1, 1);
    assert(got_long.kind == BS_DEX_WIDE && !got_long.bits);
    expect_bundle(vm, bundle, "getInt",
                  "(Ljava/lang/String;)I", &key_s, 1, 0);
    BsDexValue null_key_args[2] = {bs_dex_null(), bs_dex_int(-91)};
    expect_bundle(vm, bundle, "putInt", "(Ljava/lang/String;I)V",
                  null_key_args, 2, 1);
    assert(observed_int(expect_bundle(vm, bundle, "getInt",
                "(Ljava/lang/String;)I", null_key_args, 1, 1)) == -91);
    expect_bundle(vm, bundle, "remove", "(Ljava/lang/String;)V",
                  &key_i_again, 1, 1);
    assert(observed_int(expect_bundle(vm, bundle, "containsKey",
                "(Ljava/lang/String;)Z", &key_i, 1, 1)) == 0);
    expect_bundle(vm, bundle, "clear", "()V", NULL, 0, 1);
    assert(observed_int(expect_bundle(vm, bundle, "size", "()I",
                                     NULL, 0, 1)) == 0);
    BsDexValue random = bs_dex_vm_random_at(vm, 0);
    assert(random.bits && !strcmp(bs_dex_reference_type(vm, random),
                                  "Ljava/util/Random;"));
    /* Java Random() uses first seedUniquifier multiplication XOR the exact
     * 1234567890123456-ns host reading. These are independent OpenJDK LCG
     * golden vectors; a constant/faked default seed fails here. */
    assert(observed_int(expect_random(vm, random, "nextInt", "()I",
                                    NULL, 0, 1, NULL)) == 1184978872);
    BsDexValue seed_zero_args[2] = {
        {(uintptr_t)0, BS_DEX_WIDE}, bs_dex_int(0)
    };
    expect_random(vm, random, "setSeed", "(J)V",
                  seed_zero_args, 2, 1, NULL);
    BsDexValue bound_zero = bs_dex_int(0);
    expect_random(vm, random, "nextInt", "(I)I",
                  &bound_zero, 1, -1,
                  "Ljava/lang/IllegalArgumentException;");
    const int32_t golden_ints[] = {
        -1155484576, -723955400, 1033096058,
        -1690734402, -1557280266, 1327362106
    };
    for (size_t i = 0; i < sizeof golden_ints / sizeof golden_ints[0]; ++i)
        assert(observed_int(expect_random(vm, random, "nextInt", "()I",
                  NULL, 0, 1, NULL)) == golden_ints[i]);
    expect_random(vm, random, "setSeed", "(J)V",
                  seed_zero_args, 2, 1, NULL);
    BsDexValue long_result = expect_random(vm, random, "nextLong", "()J",
                                           NULL, 0, 1, NULL);
    assert(long_result.kind == BS_DEX_WIDE &&
           (uint64_t)long_result.bits ==
               (uint64_t)(-INT64_C(4962768465676381896)));
    expect_random(vm, random, "setSeed", "(J)V",
                  seed_zero_args, 2, 1, NULL);
    const int expected_bools[] = {1, 1, 0, 1, 1, 0, 1, 0};
    for (size_t i = 0; i < sizeof expected_bools / sizeof expected_bools[0]; ++i)
        assert(observed_int(expect_random(vm, random, "nextBoolean", "()Z",
                 NULL, 0, 1, NULL)) == expected_bools[i]);
    expect_random(vm, random, "setSeed", "(J)V",
                  seed_zero_args, 2, 1, NULL);
    BsDexValue bound_ten = bs_dex_int(10);
    const int expected_bounded[] = {0, 8, 9, 7, 5, 3, 1, 1};
    for (size_t i = 0; i < sizeof expected_bounded / sizeof expected_bounded[0]; ++i)
        assert(observed_int(expect_random(vm, random, "nextInt", "(I)I",
                  &bound_ten, 1, 1, NULL)) == expected_bounded[i]);
    expect_random(vm, random, "setSeed", "(J)V",
                  seed_zero_args, 2, 1, NULL);
    BsDexValue bound_sixteen = bs_dex_int(16);
    assert(observed_int(expect_random(vm, random, "nextInt", "(I)I",
                                 &bound_sixteen, 1, 1, NULL)) == 11);
    BsDexValue invalid_random_arg = bs_dex_null();
    expect_random(vm, random, "nextInt", "(I)I",
                  &invalid_random_arg, 1, 0, NULL);
    expect_random(vm, random, "nextGaussian", "()D", NULL, 0, 0, NULL);
    BsDexValue atomic = bs_dex_vm_atomic_integer_at(vm, 0);
    assert(atomic.bits &&
           !strcmp(bs_dex_reference_type(vm, atomic),
                   "Ljava/util/concurrent/atomic/AtomicInteger;"));
    /* Original AndroidX executor initialization has now also constructed
     * another real AtomicInteger; both objects retain independent state. */
    BsDexValue another_atomic = bs_dex_vm_atomic_integer_at(vm, 1);
    assert(another_atomic.bits && another_atomic.bits != atomic.bits);
    /* The real constructor initializes this counter to zero. Stateful
     * diagnostics below use the actual constructed VM object. They are
     * independent of (and do not increment) original DEX instruction count. */
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == 0);
    BsDexValue ai_one[1] = {bs_dex_int(1)};
    assert(observed_int(expect_atomic(vm, atomic, "getAndIncrement", "()I",
                                     NULL, 0, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "incrementAndGet", "()I",
                                     NULL, 0, 1)) == 2);
    assert(observed_int(expect_atomic(vm, atomic, "getAndDecrement", "()I",
                                     NULL, 0, 1)) == 2);
    assert(observed_int(expect_atomic(vm, atomic, "decrementAndGet", "()I",
                                     NULL, 0, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "addAndGet", "(I)I",
                                     ai_one, 1, 1)) == 1);
    assert(observed_int(expect_atomic(vm, atomic, "getAndAdd", "(I)I",
                                     ai_one, 1, 1)) == 1);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == 2);
    BsDexValue ai_cas[2] = {bs_dex_int(5), bs_dex_int(77)};
    assert(observed_int(expect_atomic(vm, atomic, "compareAndSet", "(II)Z",
                                     ai_cas, 2, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == 2);
    ai_cas[0] = bs_dex_int(2);
    assert(observed_int(expect_atomic(vm, atomic, "compareAndSet", "(II)Z",
                                     ai_cas, 2, 1)) == 1);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == 77);
    ai_cas[0] = bs_dex_int(77);
    ai_cas[1] = bs_dex_int(-7);
    assert(observed_int(expect_atomic(vm, atomic, "compareAndExchange", "(II)I",
                                     ai_cas, 2, 1)) == 77);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == -7);
    ai_cas[0] = bs_dex_int(-7);
    ai_cas[1] = bs_dex_int(INT32_MIN);
    assert(observed_int(expect_atomic(vm, atomic, "weakCompareAndSet", "(II)Z",
                                     ai_cas, 2, 1)) == 1);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == INT32_MIN);
    assert(observed_int(expect_atomic(vm, atomic, "decrementAndGet", "()I",
                                     NULL, 0, 1)) == INT32_MAX);
    assert(observed_int(expect_atomic(vm, atomic, "incrementAndGet", "()I",
                                     NULL, 0, 1)) == INT32_MIN);
    BsDexValue ai_max[1] = {bs_dex_int(INT32_MAX)};
    assert(observed_int(expect_atomic(vm, atomic, "getAndAdd", "(I)I",
                                     ai_max, 1, 1)) == INT32_MIN);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == -1);
    assert(observed_int(expect_atomic(vm, atomic, "addAndGet", "(I)I",
                                     ai_one, 1, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "getAndSet", "(I)I",
                                     ai_max, 1, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == INT32_MAX);
    assert(expect_atomic(vm, atomic, "set", "(I)V",
                         ai_one, 1, 1).kind == BS_DEX_INT);
    assert(observed_int(expect_atomic(vm, atomic, "intValue", "()I",
                                     NULL, 0, 1)) == 1);
    assert(observed_int(expect_atomic(vm, atomic, "lazySet", "(I)V",
                                     ai_max, 1, 1)) == 0);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == INT32_MAX);
    BsDexValue invalid[1] = {bs_dex_null()};
    expect_atomic(vm, atomic, "set", "(I)V", invalid, 1, 0);
    expect_atomic(vm, atomic, "getAndAdd", "(J)I", ai_one, 1, 0);
    expect_atomic(vm, atomic, "compareAndSet", "(II)Z", ai_cas, 1, 0);
    assert(observed_int(expect_atomic(vm, atomic, "get", "()I",
                                     NULL, 0, 1)) == INT32_MAX);
    /* Explicit diagnostic calls have not consumed or skipped the next
     * actual guest instruction, nor manufactured a readiness checkpoint. */
    BsDexStop still_waiting = bs_dex_vm_stop(vm);
    assert(still_waiting.kind == BS_DEX_STOP_HOST_MISSING &&
           still_waiting.steps == weak_advanced.steps &&
           still_waiting.code_off == weak_advanced.code_off &&
           still_waiting.dex_pc == weak_advanced.dex_pc);
    assert(!bs_dex_vm_current_activity(vm).bits);
    BsDexValue deque = bs_dex_vm_array_deque_at(vm, 0);
    assert(deque.bits && !strcmp(bs_dex_reference_type(vm, deque),
                                 "Ljava/util/ArrayDeque;"));
    assert(!bs_dex_vm_array_deque_at(vm, 1).bits);
    assert(expect_deque(vm, deque, "size", "()I", NULL, 1, NULL).bits == 0);
    assert(expect_deque(vm, deque, "isEmpty", "()Z", NULL, 1, NULL).bits == 1);
    assert(!expect_deque(vm, deque, "peek", "()Ljava/lang/Object;",
                         NULL, 1, NULL).bits);
    assert(!expect_deque(vm, deque, "poll", "()Ljava/lang/Object;",
                         NULL, 1, NULL).bits);
    expect_deque(vm, deque, "remove", "()Ljava/lang/Object;",
                 NULL, -1, "Ljava/util/NoSuchElementException;");
    BsDexValue nil = bs_dex_null();
    expect_deque(vm, deque, "add", "(Ljava/lang/Object;)Z",
                 &nil, -1, "Ljava/lang/NullPointerException;");
    BsDexValue primitive = bs_dex_int(3);
    expect_deque(vm, deque, "add", "(Ljava/lang/Object;)Z",
                 &primitive, 0, NULL);
    assert(expect_deque(vm, deque, "size", "()I",
                        NULL, 1, NULL).bits == 0);
    BsDexValue alpha = bs_dex_new_observed_string_utf8(vm, "alpha", 5);
    BsDexValue beta = bs_dex_new_observed_string_utf8(vm, "beta", 4);
    BsDexValue other_alpha = bs_dex_new_observed_string_utf8(vm, "alpha", 5);
    assert(alpha.bits && beta.bits && other_alpha.bits);
    for (unsigned i = 0; i < 48; ++i) {
        BsDexValue value = (i % 2) ? beta : alpha;
        BsDexValue added = expect_deque(vm, deque, "add",
                       "(Ljava/lang/Object;)Z", &value, 1, NULL);
        assert(added.kind == BS_DEX_INT && added.bits == 1);
    }
    assert(expect_deque(vm, deque, "size", "()I", NULL, 1, NULL).bits == 48);
    assert(expect_deque(vm, deque, "peekFirst", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == alpha.bits);
    assert(expect_deque(vm, deque, "peekLast", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == beta.bits);
    for (unsigned i = 0; i < 33; ++i) {
        BsDexValue removed = expect_deque(vm, deque, "removeFirst",
                        "()Ljava/lang/Object;", NULL, 1, NULL);
        assert(removed.bits == ((i % 2) ? beta : alpha).bits);
    }
    assert(expect_deque(vm, deque, "size", "()I", NULL, 1, NULL).bits == 15);
    assert(expect_deque(vm, deque, "contains", "(Ljava/lang/Object;)Z",
                        &other_alpha, 1, NULL).bits == 1);
    BsDexValue offered = expect_deque(vm, deque, "offerFirst",
                         "(Ljava/lang/Object;)Z", &other_alpha, 1, NULL);
    assert(offered.bits == 1);
    assert(expect_deque(vm, deque, "pop", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == other_alpha.bits);
    assert(expect_deque(vm, deque, "removeLast", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == beta.bits);
    expect_deque(vm, deque, "clear", "()V", NULL, 1, NULL);
    assert(expect_deque(vm, deque, "size", "()I", NULL, 1, NULL).bits == 0);
    assert(!expect_deque(vm, deque, "peekFirst", "()Ljava/lang/Object;",
                         NULL, 1, NULL).bits);
    expect_deque(vm, deque, "addFirst", "(Ljava/lang/Object;)V",
                 &beta, 1, NULL);
    assert(expect_deque(vm, deque, "pollLast", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == beta.bits);
    assert(!expect_deque(vm, deque, "pollLast", "()Ljava/lang/Object;",
                         NULL, 1, NULL).bits);
    /* An external Android Activity may override Object.equals; the VM
     * refuses to assume identity semantics for contains/remove(Object). */
    expect_deque(vm, deque, "addLast", "(Ljava/lang/Object;)V",
                 &constructed_activity, 1, NULL);
    expect_deque(vm, deque, "contains", "(Ljava/lang/Object;)Z",
                 &constructed_activity, -3, NULL);
    assert(expect_deque(vm, deque, "size", "()I", NULL, 1, NULL).bits == 1);
    expect_deque(vm, deque, "clear", "()V", NULL, 1, NULL);
    /* The bounded interpreter grows through 16/32/64/... to 8192; the
     * first allocation beyond its fixed limit must fail before enqueue. */
    for (unsigned i = 0; i < 8192; ++i)
        assert(expect_deque(vm, deque, "offerLast",
               "(Ljava/lang/Object;)Z", &alpha, 1, NULL).bits == 1);
    assert(expect_deque(vm, deque, "size", "()I",
                        NULL, 1, NULL).bits == 8192);
    expect_deque(vm, deque, "offerLast", "(Ljava/lang/Object;)Z",
                 &beta, 0, NULL);
    assert(expect_deque(vm, deque, "size", "()I",
                        NULL, 1, NULL).bits == 8192);
    assert(expect_deque(vm, deque, "pollFirst", "()Ljava/lang/Object;",
                        NULL, 1, NULL).bits == alpha.bits);
    expect_deque(vm, deque, "clear", "()V", NULL, 1, NULL);
    BsDexValue first_enum_name = bs_dex_null();
    int32_t first_enum_ordinal = -1;
    assert(bs_dex_vm_enum_at(vm, 0, &first_enum_name,
                             &first_enum_ordinal));
    assert(first_enum_name.kind == BS_DEX_REF &&
           bs_dex_string(vm, first_enum_name));
    assert(first_enum_ordinal == 0 &&
           !strcmp(bs_dex_string(vm, first_enum_name), "DESTROYED"));
    printf("original Enum constant[0]: name=%s ordinal=%d\n",
           bs_dex_string(vm, first_enum_name), first_enum_ordinal);
    BsDexValue weak_object = bs_dex_vm_weak_reference_at(vm, 0);
    assert(weak_object.bits &&
           !strcmp(bs_dex_reference_type(vm, weak_object),
                   "Ljava/lang/ref/WeakReference;"));
    BsDexValue live = bs_dex_null();
    assert(bs_dex_vm_weak_reference_get(vm, weak_object, &live));
    assert(live.bits == constructed_activity.bits);
    test_weak_bad_return = 1;
    assert(!bs_dex_vm_weak_reference_get(vm, weak_object, &live));
    test_weak_bad_return = 0;
    test_weak_alive = 0;
    assert(bs_dex_host_pointer(vm, constructed_activity) == &test_activity);
    assert(bs_dex_vm_weak_reference_get(vm, weak_object, &live));
    assert(!live.bits && live.kind == BS_DEX_REF);
    test_weak_alive = 1;
    assert(bs_dex_vm_weak_reference_clear(vm, weak_object));
    assert(bs_dex_vm_weak_reference_get(vm, weak_object, &live));
    assert(!live.bits && test_weak_clear_calls == 1 &&
           test_weak_resolved >= 4);
    bs_dex_vm_destroy(vm);
    assert(test_weak_released == test_weak_created);
    assert(!bs_dex_vm_weak_reference_at(NULL, 0).bits);
    /* Run the ACTUAL constructor again with a witnessed native main loop.
     * The VM may compare canonical thread IDs; any Handler or Runnable
     * dispatch is still unavailable and must stop at its genuine DEX PC. */
    reset_weak_fixture_after_destroy();
    host.observe_main_looper = test_observe_main_looper;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_constructor(vm));
    BsDexStop main_thread_observed = bs_dex_vm_run(vm, 10000);
    printf("observed native Looper identity: steps=%u stop=%d code=0x%x "
           "pc=0x%x opcode=0x%x host=%s.%s exception=%s detail=%s\n",
           main_thread_observed.steps, main_thread_observed.kind,
           main_thread_observed.code_off, main_thread_observed.dex_pc,
           main_thread_observed.opcode,
           main_thread_observed.host_class ?
               main_thread_observed.host_class : "(none)",
           main_thread_observed.host_method ?
               main_thread_observed.host_method : "(none)",
           main_thread_observed.exception ?
               main_thread_observed.exception : "(none)",
           main_thread_observed.detail ?
               main_thread_observed.detail : "(none)");
    fflush(stdout);
    assert(test_main_loop_calls >= 3);
    assert(main_thread_observed.steps == 1902 &&
           main_thread_observed.kind == BS_DEX_STOP_HOST_MISSING &&
           main_thread_observed.code_off == 0x204e50 &&
           main_thread_observed.dex_pc == 0x0 &&
           main_thread_observed.opcode == 0x22);
    assert(main_thread_observed.host_class &&
           !strcmp(main_thread_observed.host_class,
                   "Landroid/os/Handler;"));
    assert(main_thread_observed.host_method &&
           !strcmp(main_thread_observed.host_method, "new-instance"));
    printf("main Looper observations before Handler allocation=%d\n",
           test_main_loop_calls);
    BsDexValue looper = bs_dex_vm_observed_main_looper(vm);
    assert(looper.bits &&
           !strcmp(bs_dex_reference_type(vm, looper),
                   "Landroid/os/Looper;"));
    /* Java Enum.compareTo uses only actual DEX-initialized ordinals and
     * rejects null/other-enum types without fabricating Java readiness. */
    BsDexValue first_state = bs_dex_vm_enum_instance_at(vm, 0);
    BsDexValue second_state = bs_dex_vm_enum_instance_at(vm, 1);
    assert(first_state.bits && second_state.bits &&
           !strcmp(bs_dex_reference_type(vm, first_state),
                   bs_dex_reference_type(vm, second_state)));
    BsDexValue compared = bs_dex_int(0);
    const char *compare_exception = NULL;
    assert(bs_dex_vm_enum_compare_diagnostic_call(
             vm, first_state, second_state, &compared,
             &compare_exception) == 1);
    assert(observed_int(compared) == -1 && !compare_exception);
    assert(bs_dex_vm_enum_compare_diagnostic_call(
             vm, second_state, first_state, &compared,
             &compare_exception) == 1);
    assert(observed_int(compared) == 1 && !compare_exception);
    assert(bs_dex_vm_enum_compare_diagnostic_call(
             vm, first_state, bs_dex_null(), &compared,
             &compare_exception) == -1);
    assert(compare_exception &&
           !strcmp(compare_exception, "Ljava/lang/NullPointerException;"));
    assert(bs_dex_vm_enum_compare_diagnostic_call(
             vm, first_state, constructed_activity, &compared,
             &compare_exception) == -1);
    assert(compare_exception &&
           !strcmp(compare_exception, "Ljava/lang/ClassCastException;"));
    BsDexValue boxed_false = bs_dex_vm_cached_boolean(vm, 0);
    assert(boxed_false.bits &&
           !strcmp(bs_dex_reference_type(vm, boxed_false),
                   "Ljava/lang/Boolean;"));
    assert(!bs_dex_vm_cached_boolean(vm, 1).bits);
    /* The first actual Boolean.valueOf uses const/4 zero in original DEX.
     * HashMap operations below preserve collision resolution and actual
     * Java key equality, including null keys. */
    BsDexValue map = bs_dex_vm_hash_map_at(vm, 0);
    assert(map.bits && !strcmp(bs_dex_reference_type(vm, map),
                               "Ljava/util/HashMap;"));
    expect_hash_map(vm, map, "clear", "()V", NULL, 0, 1);
    BsDexValue fb = bs_dex_new_observed_string_utf8(vm, "FB", 2);
    BsDexValue ea = bs_dex_new_observed_string_utf8(vm, "Ea", 2);
    BsDexValue fb_again = bs_dex_new_observed_string_utf8(vm, "FB", 2);
    BsDexValue test_one = bs_dex_new_observed_string_utf8(vm, "one", 3);
    BsDexValue test_two = bs_dex_new_observed_string_utf8(vm, "two", 3);
    assert(fb.bits && ea.bits && fb_again.bits &&
           test_one.bits && test_two.bits);
    BsDexValue put_fb[2] = {fb, test_one};
    BsDexValue put_ea[2] = {ea, test_two};
    assert(!expect_hash_map(vm, map, "put",
          "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
          put_fb, 2, 1).bits);
    assert(!expect_hash_map(vm, map, "put",
          "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
          put_ea, 2, 1).bits);
    assert(expect_hash_map(vm, map, "get",
          "(Ljava/lang/Object;)Ljava/lang/Object;",
          &fb_again, 1, 1).bits == test_one.bits);
    assert(expect_hash_map(vm, map, "get",
          "(Ljava/lang/Object;)Ljava/lang/Object;",
          &ea, 1, 1).bits == test_two.bits);
    assert(observed_int(expect_hash_map(vm, map, "size",
          "()I", NULL, 0, 1)) == 2);
    BsDexValue put_fb_update[2] = {fb_again, test_two};
    assert(expect_hash_map(vm, map, "put",
          "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
          put_fb_update, 2, 1).bits == test_one.bits);
    assert(observed_int(expect_hash_map(vm, map, "size",
          "()I", NULL, 0, 1)) == 2);
    BsDexValue put_null[2] = {bs_dex_null(), boxed_false};
    assert(!expect_hash_map(vm, map, "put",
          "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
          put_null, 2, 1).bits);
    assert(expect_hash_map(vm, map, "get",
          "(Ljava/lang/Object;)Ljava/lang/Object;",
          put_null, 1, 1).bits == boxed_false.bits);
    assert(observed_int(expect_hash_map(vm, map, "containsKey",
          "(Ljava/lang/Object;)Z", put_null, 1, 1)) == 1);
    assert(expect_hash_map(vm, map, "remove",
          "(Ljava/lang/Object;)Ljava/lang/Object;",
          &ea, 1, 1).bits == test_two.bits);
    assert(observed_int(expect_hash_map(vm, map, "size",
          "()I", NULL, 0, 1)) == 2);
    expect_hash_map(vm, map, "get",
          "(Ljava/lang/Object;)Ljava/lang/Object;",
          &constructed_activity, 1, -4);
    assert(observed_int(expect_hash_map(vm, map, "size",
          "()I", NULL, 0, 1)) == 2);
    expect_hash_map(vm, map, "clear", "()V", NULL, 0, 1);
    assert(observed_int(expect_hash_map(vm, map, "isEmpty",
          "()Z", NULL, 0, 1)) == 1);

    /* The original DEX inserted a VM-owned SafeIterableMap$AscendingIterator
     * into mIterators:WeakHashMap, without a strong key. This bounded VM
     * runs no guest GC and never reclaims an allocation before teardown:
     * the ReferenceQueue has not fired and this key remains in the map.
     * No GC event or Java iterator readiness is claimed. */
    BsDexValue weak_map = bs_dex_null();
    BsDexWeakMapDiagnosticCounts weak_counts = {0};
    for (size_t i = 0; i < 16; ++i) {
        BsDexValue candidate = bs_dex_vm_weak_hash_map_at(vm, i);
        if (!candidate.bits) break;
        BsDexWeakMapDiagnosticCounts sample = {0};
        assert(bs_dex_vm_weak_hash_map_diagnostic_counts(
                   vm, candidate, &sample));
        if (sample.vm_weak_keys) {
            assert(!weak_map.bits);
            weak_map = candidate;
            weak_counts = sample;
        }
    }
    assert(weak_map.bits &&
           !strcmp(bs_dex_reference_type(vm, weak_map),
                   "Ljava/util/WeakHashMap;"));
    assert(weak_counts.vm_weak_keys == 1 &&
           weak_counts.total_entries == 1 &&
           !weak_counts.null_keys && !weak_counts.external_weak_keys);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 1);
    const int created_before_diagnostics = test_weak_created;
    /* Diagnostic edits below do not execute guest bytecode. Starting with
     * clear discards this original iterator entry, without claiming GC. */
    expect_weak_hash_map(vm, weak_map, "clear", "()V", NULL, 0, 1);
    BsDexValue vm_string_key[2] = {fb, test_one};
    assert(!expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           vm_string_key, 2, 1).bits);
    assert(bs_dex_vm_weak_hash_map_diagnostic_counts(vm, weak_map,
                                                      &weak_counts));
    assert(weak_counts.vm_weak_keys == 1 &&
           weak_counts.total_entries == 1 && !weak_counts.external_weak_keys);
    assert(expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &fb_again, 1, 1).bits == test_one.bits);
    assert(test_weak_created == created_before_diagnostics);
    assert(expect_weak_hash_map(vm, weak_map, "remove",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &fb_again, 1, 1).bits == test_one.bits);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 0);
    /* Java WeakHashMap's null-key sentinel is special: it never weakens,
     * and the entry can safely retain a strong value even without GC. */
    BsDexValue null_weak_key[2] = {bs_dex_null(), boxed_false};
    assert(!expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           null_weak_key, 2, 1).bits);
    assert(expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           null_weak_key, 1, 1).bits == boxed_false.bits);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "containsKey", "(Ljava/lang/Object;)Z",
           null_weak_key, 1, 1)) == 1);
    assert(expect_weak_hash_map(vm, weak_map, "remove",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           null_weak_key, 1, 1).bits == boxed_false.bits);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 0);

    /* A separate fake native runtime owns these three String referents.
     * The weak callback retains scalar identity tokens and independent
     * alive flags; WeakHashMap itself retains no key reference. The Java
     * strings FB and Ea have colliding String.hashCode values (2236). */
    BsDexValue external_fb = bs_dex_wrap_platform(
        vm, &test_map_key_backings[0], "Ljava/lang/String;");
    BsDexValue external_ea = bs_dex_wrap_platform(
        vm, &test_map_key_backings[1], "Ljava/lang/String;");
    BsDexValue external_fb_equal = bs_dex_wrap_platform(
        vm, &test_map_key_backings[2], "Ljava/lang/String;");
    assert(external_fb.bits && external_ea.bits &&
           external_fb_equal.bits &&
           bs_dex_attach_observed_string_utf8(vm, external_fb, "FB", 2) &&
           bs_dex_attach_observed_string_utf8(vm, external_ea, "Ea", 2) &&
           bs_dex_attach_observed_string_utf8(vm, external_fb_equal, "FB", 2));
    BsDexValue weak_put_fb[2] = {external_fb, test_one};
    BsDexValue weak_put_ea[2] = {external_ea, test_two};
    const int weak_fb_handle_index = test_weak_created;
    assert(!expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           weak_put_fb, 2, 1).bits);
    const int weak_ea_handle_index = test_weak_created;
    assert(!expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           weak_put_ea, 2, 1).bits);
    assert(test_weak_created == weak_ea_handle_index + 1 &&
           test_weak_string_identity[weak_fb_handle_index] ==
               external_fb.bits &&
           test_weak_string_identity[weak_ea_handle_index] ==
               external_ea.bits);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 2);
    assert(expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &external_fb_equal, 1, 1).bits == test_one.bits);
    assert(expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &external_ea, 1, 1).bits == test_two.bits);
    BsDexValue replace_equal_key[2] = {external_fb_equal, boxed_false};
    assert(expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           replace_equal_key, 2, 1).bits == test_one.bits);
    assert(test_weak_created == weak_ea_handle_index + 1);
    assert(expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &external_fb_equal, 1, 1).bits == boxed_false.bits);
    /* Reject a malformed host weak result without deleting live entries. */
    test_weak_bad_return = 1;
    expect_weak_hash_map(vm, weak_map, "size", "()I", NULL, 0, -2);
    test_weak_bad_return = 0;
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 2);
    /* The original external FB referent has now actually died. Expunge the
     * weak handle, including its strong VALUE, upon the next map access.
     * The other live equal string cannot extend the first key's lifetime. */
    test_weak_string_alive[weak_fb_handle_index] = 0;
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "size", "()I", NULL, 0, 1)) == 1);
    assert(test_weak_released_per[weak_fb_handle_index] == 1);
    assert(!expect_weak_hash_map(vm, weak_map, "get",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &external_fb_equal, 1, 1).bits);
    assert(expect_weak_hash_map(vm, weak_map, "remove",
           "(Ljava/lang/Object;)Ljava/lang/Object;",
           &external_ea, 1, 1).bits == test_two.bits);
    assert(test_weak_released_per[weak_ea_handle_index] == 1);
    assert(observed_int(expect_weak_hash_map(vm, weak_map,
           "isEmpty", "()Z", NULL, 0, 1)) == 1);
    expect_weak_hash_map(vm, weak_map, "put",
           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
           (BsDexValue[2]){constructed_activity, boxed_false}, 2, -4);
    assert(test_weak_created == weak_ea_handle_index + 1);
    BsDexStop unchanged_after_diagnostics = bs_dex_vm_stop(vm);
    assert(unchanged_after_diagnostics.steps == main_thread_observed.steps &&
           unchanged_after_diagnostics.code_off == main_thread_observed.code_off &&
           unchanged_after_diagnostics.dex_pc == main_thread_observed.dex_pc);
    bs_dex_vm_destroy(vm);
    reset_weak_fixture_after_destroy();
    for (int handler_scenario = 0; handler_scenario < 5;
         ++handler_scenario)
        run_original_handler_fixture(dex, size, host, handler_scenario);
    host.observe_main_looper = NULL;
    host.weak_reference_create = NULL;
    host.weak_reference_get = NULL;
    host.weak_reference_clear = NULL;
    host.weak_reference_release = NULL;
    host.invoke_platform = NULL;
    host.instance_of = NULL;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm);
    /* Observed native UTF-8 values become owned String objects, with an
     * exact distinction between absent String/null and a real empty String. */
    BsDexValue text = bs_dex_new_observed_string_utf8(vm, "quest-path", 10);
    assert(text.bits && !strcmp(bs_dex_string(vm, text), "quest-path"));
    BsDexValue empty_text = bs_dex_new_observed_string_utf8(vm, "", 0);
    assert(empty_text.bits && !strcmp(bs_dex_string(vm, empty_text), ""));
    int live_test_string = 1234;
    BsDexValue jstring = bs_dex_wrap_platform(
        vm, &live_test_string, "Ljava/lang/String;");
    assert(jstring.bits && bs_dex_host_pointer(vm, jstring) == &live_test_string);
    assert(bs_dex_string(vm, jstring) == NULL);
    assert(!bs_dex_attach_observed_string_utf8(vm, jstring, "a\0b", 3));
    assert(bs_dex_attach_observed_string_utf8(vm, jstring, "measured", 8));
    assert(!strcmp(bs_dex_string(vm, jstring), "measured"));
    assert(!bs_dex_attach_observed_string_utf8(vm, jstring, "changed", 7));
    assert(bs_dex_host_pointer(vm, jstring) == &live_test_string);
    assert(!bs_dex_new_observed_string_utf8(vm, NULL, 0).bits);
    assert(!bs_dex_new_observed_string_utf8(vm, "a\0b", 3).bits);
    const char invalid_utf8[] = {(char)0xc0, (char)0x80};
    assert(!bs_dex_new_observed_string_utf8(vm, invalid_utf8, 2).bits);
    assert(bs_dex_vm_start_activity_oncreate(vm, NULL, bs_dex_null()) == 0);
    const BsDexValue unknown_bundle = {
        (uintptr_t)&test_activity, BS_DEX_REF
    };
    assert(bs_dex_vm_start_activity_oncreate(vm, &test_activity,
                                             unknown_bundle) == 0);
    assert(bs_dex_vm_start_activity_oncreate(vm, &test_activity,
                                             bs_dex_null()) == 1);
    assert(bs_dex_vm_start_activity_oncreate(vm, &test_activity,
                                             bs_dex_null()) == 0);
    BsDexStop stopped = bs_dex_vm_run(vm, 10000);
    printf("original DEX instructions executed=%u, stop=%d, code=0x%x "
           "pc=0x%x opcode=0x%x depth=%u host=%s.%s detail=%s\n",
           stopped.steps, stopped.kind, stopped.code_off, stopped.dex_pc,
           stopped.opcode, stopped.depth,
           stopped.host_class ? stopped.host_class : "(none)",
           stopped.host_method ? stopped.host_method : "(none)",
           stopped.detail ? stopped.detail : "(none)");
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING ||
           stopped.kind == BS_DEX_STOP_OPCODE_MISSING);
    assert(stopped.steps >= 2 && stopped.depth >= 2);
    /* Every caller is an actual original method frame reached through
     * Dalvik invoke-super/invoke-virtual/invoke-direct dynamic dispatch. */
    const uint32_t exact_stack[] = {
        0x23794c, 0x22edac, 0x237964, 0x22f0b4,
        0x237830, 0x237810, 0x22e9d8
    };
    assert(stopped.depth == sizeof exact_stack / sizeof exact_stack[0]);
    for (size_t i = 0; i < sizeof exact_stack / sizeof exact_stack[0]; ++i) {
        BsDexTraceFrame frame;
        assert(bs_dex_vm_trace_frame(vm, i, &frame) == 1);
        assert(frame.code_off == exact_stack[i]);
        assert(frame.class_descriptor && frame.method_name);
    }
    assert(stopped.host_class && !strcmp(stopped.host_class,
                                         "Landroid/view/SurfaceView;"));
    assert(stopped.host_method && !strcmp(stopped.host_method, "<init>"));
    assert(stopped.code_off == 0x22e9d8 && stopped.dex_pc == 0x02);
    assert(!bs_dex_vm_start_activity_oncreate(vm, &test_activity,
                                               bs_dex_null()));
    assert(!bs_dex_vm_current_activity(vm).bits);
    assert(!bs_dex_vm_current_context(vm).bits);
    assert(bs_dex_vm_stop(vm).kind == stopped.kind);
    assert(bs_dex_vm_run(vm, 5).steps == stopped.steps);
    bs_dex_vm_destroy(vm);

    /* An actual negative host callback is a pending Java exception, never
     * interpreted as successful constructor execution. */
    host.invoke_platform = throw_platform_constructor;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_oncreate(
        vm, &test_activity, bs_dex_null()));
    stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.kind == BS_DEX_STOP_JAVA_EXCEPTION);
    assert(stopped.exception &&
           !strcmp(stopped.exception, "Ljava/lang/IllegalStateException;"));
    assert(stopped.code_off == 0x22e9d8 && stopped.dex_pc == 0x02);
    assert(!bs_dex_vm_current_activity(vm).bits && thrown_calls == 1);
    bs_dex_vm_destroy(vm);

    /* A callback that reports failure without an exception class violates
     * the host ABI and must stop as corrupt, not invent an exception. */
    malformed_exception = 1;
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_oncreate(
        vm, &test_activity, bs_dex_null()));
    stopped = bs_dex_vm_run(vm, 10000);
    assert(stopped.kind == BS_DEX_STOP_CORRUPT && thrown_calls == 2);
    bs_dex_vm_destroy(vm);

    /* Functional test implementation of two actual platform constructors
     * and Java field operations. The same original DEX executes more deeply
     * and must stop at the NEXT missing platform operation. */
    host.new_platform_object = platform_new;
    host.invoke_platform = platform_method;
    host.field_platform = platform_field;
    host.instance_of = platform_instanceof;
    platform.ime_ref = bs_dex_null();
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_oncreate(
        vm, &test_activity, bs_dex_null()));
    stopped = bs_dex_vm_run(vm, 10000);
    printf("bounded modeled platform: DEX instructions=%u stop=%d "
           "code=0x%x pc=0x%x opcode=0x%x host=%s.%s detail=%s\n",
           stopped.steps, stopped.kind, stopped.code_off,
           stopped.dex_pc, stopped.opcode,
           stopped.host_class ? stopped.host_class : "(none)",
           stopped.host_method ? stopped.host_method : "(none)",
           stopped.detail ? stopped.detail : "(none)");
    fflush(stdout);
    /* This modeled host is confined to the fixture. It logs the original
     * string values and constructs the original Java View/InputConnection
     * objects through exact test identity checks; it does not invent the
     * Android input-method service. The next original guest operation must
     * therefore stop at Context.getSystemService("input_method"). */
    assert(stopped.kind == BS_DEX_STOP_HOST_MISSING &&
           stopped.steps == 52 && stopped.code_off == 0x230330 &&
           stopped.dex_pc == 0x1a && stopped.opcode == 0x6e &&
           stopped.host_class &&
           !strcmp(stopped.host_class, "Landroid/content/Context;") &&
           stopped.host_method &&
           !strcmp(stopped.host_method, "getSystemService"));
    assert(platform.view_constructed &&
           platform.input_connection_constructed &&
           platform.editor_constructed && platform.log_messages == 1 &&
           platform.last_log_tag && platform.last_log_message &&
           !strcmp(platform.last_log_tag, "gti.InputConnection") &&
           !strcmp(platform.last_log_message, "InputConnection created"));
    assert(!bs_dex_vm_start_activity_oncreate(vm, &test_activity,
                                              bs_dex_null()));
    assert(!bs_dex_vm_current_activity(vm).bits);
    bs_dex_vm_destroy(vm);
    /* A second independent original-Dex execution makes the Context's
     * input-method service available only inside this named test fixture.
     * It demonstrates subsequent *original* method/opcode dispatch without
     * asserting that a Switch Java framework or Android IME exists. */
    platform = (TestPlatform){ .input_method_service_available = 1,
                                .view_context_getter_available = 1 };
    platform.ime_ref = bs_dex_null();
    vm = bs_dex_vm_create(dex, size, &host);
    assert(vm && bs_dex_vm_start_activity_oncreate(
        vm, &test_activity, bs_dex_null()));
    BsDexStop ime_stop = bs_dex_vm_run(vm, 10000);
    printf("test-only Android input method service: steps=%u stop=%d "
           "code=0x%x pc=0x%x opcode=0x%x host=%s.%s detail=%s\n",
           ime_stop.steps, ime_stop.kind, ime_stop.code_off,
           ime_stop.dex_pc, ime_stop.opcode,
           ime_stop.host_class ? ime_stop.host_class : "(none)",
           ime_stop.host_method ? ime_stop.host_method : "(none)",
           ime_stop.detail ? ime_stop.detail : "(none)");
    fflush(stdout);
    assert(platform.input_method_service_reads == 1 &&
           platform.view_context_getter_reads == 1 &&
           ime_stop.kind == BS_DEX_STOP_HOST_MISSING &&
           ime_stop.steps == 63 && ime_stop.code_off == 0x230330 &&
           ime_stop.dex_pc == 0x31 && ime_stop.opcode == 0x6e &&
           ime_stop.host_class &&
           !strcmp(ime_stop.host_class, "Landroid/app/Activity;") &&
           ime_stop.host_method &&
           !strcmp(ime_stop.host_method, "getWindow") &&
           !bs_dex_vm_current_activity(vm).bits);
    bs_dex_vm_destroy(vm);
    free(dex);
    puts("PASS: real original DEX onCreate invocation and fail-closed Android boundary");
    return 0;
}
