#include "unity_dex_horizon_java_handler.h"
#include <string.h>

#define BS_JAVA_HANDLER "Landroid/os/Handler;"
#define BS_JAVA_OBJECT "Ljava/lang/Object;"

/* Require a fresh native owner/queue/OS-thread observation on EVERY call.
 * Constructor execution never equates native main Looper presence with
 * available Java Handler scheduling, Runnable.run or Context attachment. */
static int observed_owner(BsDexHorizonJavaHandlerHost *host,
                          uintptr_t *queue, uint64_t *thread) {
    if (queue) *queue = 0;
    if (thread) *thread = 0;
    if (!host || !host->initialized || !host->native_main_loop ||
        !host->observe || !queue || !thread ||
        !bs_dex_horizon_main_looper_native_ready(host->native_main_loop))
        return 0;
    uintptr_t observed_queue = 0;
    uint64_t owner = 0, current = 0, native_owner = 0;
    if (host->observe(host->observer_user, &observed_queue,
                      &owner, &current) != 1 ||
        !observed_queue || !owner || current != owner ||
        !bs_dex_horizon_main_looper_owner_thread_id(
            host->native_main_loop, &native_owner) ||
        native_owner != owner ||
        observed_queue != (uintptr_t)
            host->native_main_loop->native_looper_handle)
        return 0;
    *queue = observed_queue;
    *thread = owner;
    return 1;
}

int bs_dex_horizon_java_handler_init(
    BsDexHorizonJavaHandlerHost *host, BsDexHorizonMainLooper *native_loop,
    BsDexHorizonHandlerObserve observe, void *observer_user) {
    if (!host || host->initialized || !native_loop || !observe)
        return 0;
    host->native_main_loop = native_loop;
    host->observe = observe;
    host->observer_user = observer_user;
    host->initialized = 1;
    return 1;
}

static BsDexHorizonHandlerRecord *find_record(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm, BsDexValue ref) {
    if (!host || !host->initialized || !vm ||
        ref.kind != BS_DEX_REF || !ref.bits) return NULL;
    void *actual = bs_dex_host_pointer(vm, ref);
    if (!actual) return NULL;
    for (size_t i = 0; i < BS_HJH_MAX_HANDLERS; ++i) {
        BsDexHorizonHandlerRecord *r = &host->handlers[i];
        if (r == actual && r->allocated && r->vm == vm &&
            r->generation == host->generation &&
            r->reference.kind == ref.kind &&
            r->reference.bits == ref.bits &&
            bs_dex_reference_type(vm, ref) &&
            !strcmp(bs_dex_reference_type(vm, ref), BS_JAVA_HANDLER))
            return r;
    }
    return NULL;
}

int bs_dex_horizon_java_handler_new_platform_object(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    const char *descriptor, BsDexValue *object, const char **exception) {
    if (object) *object = bs_dex_null();
    if (exception) *exception = NULL;
    if (!host || !vm || !descriptor || !object ||
        strcmp(descriptor, BS_JAVA_HANDLER)) return 0;
    uintptr_t queue = 0;
    uint64_t thread = 0;
    if (!observed_owner(host, &queue, &thread)) return 0;
    for (size_t i = 0; i < BS_HJH_MAX_HANDLERS; ++i) {
        BsDexHorizonHandlerRecord *r = &host->handlers[i];
        if (r->allocated) continue;
        *r = (BsDexHorizonHandlerRecord){0};
        r->allocated = 1;
        r->vm = vm;
        r->queue_identity = queue;
        r->owner_thread_id = thread;
        r->generation = host->generation;
        BsDexValue ref = bs_dex_wrap_platform(vm, r, BS_JAVA_HANDLER);
        if (ref.kind != BS_DEX_REF || !ref.bits) {
            *r = (BsDexHorizonHandlerRecord){0};
            return 0;
        }
        r->reference = ref;
        *object = ref;
        return 1;
    }
    return 0; /* Bounded allocation: no invented Java OOME or success. */
}

int bs_dex_horizon_java_handler_invoke_platform(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    const char *klass, const char *name, const char *signature,
    uint8_t opcode, const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception) {
    if (exception) *exception = NULL;
    if (!host || !vm || !klass || !name || !signature || !result ||
        strcmp(klass, BS_JAVA_HANDLER)) return 0;
    /* Handler() binds this precise Java-compatible instance to the current
     * thread's observed Looper. The original constructor performs no post. */
    if (strcmp(name, "<init>") || strcmp(signature, "()V") ||
        (opcode != 0x70 && opcode != 0x76) || !args || count != 1)
        return 0;
    BsDexHorizonHandlerRecord *r = find_record(host, vm, args[0]);
    uintptr_t queue = 0;
    uint64_t thread = 0;
    if (!r || r->constructed || !observed_owner(host, &queue, &thread) ||
        r->queue_identity != queue || r->owner_thread_id != thread)
        return 0;
    r->constructed = 1;
    *result = bs_dex_null();
    return 1;
}

int bs_dex_horizon_java_handler_instance_of(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    BsDexValue object, const char *descriptor, int *result) {
    if (result) *result = 0;
    if (!descriptor || !result || !find_record(host, vm, object)) return 0;
    uintptr_t queue = 0;
    uint64_t thread = 0;
    if (!observed_owner(host, &queue, &thread)) return 0;
    const BsDexHorizonHandlerRecord *r = find_record(host, vm, object);
    if (!r || r->queue_identity != queue || r->owner_thread_id != thread)
        return 0;
    if (strcmp(descriptor, BS_JAVA_HANDLER) &&
        strcmp(descriptor, BS_JAVA_OBJECT)) return 0;
    *result = 1;
    return 1;
}

size_t bs_dex_horizon_java_handler_constructed_count(
    const BsDexHorizonJavaHandlerHost *host) {
    if (!host || !host->initialized) return 0;
    size_t count = 0;
    for (size_t i = 0; i < BS_HJH_MAX_HANDLERS; ++i)
        if (host->handlers[i].allocated && host->handlers[i].constructed)
            ++count;
    return count;
}

int bs_dex_horizon_java_handler_original_constructor_ready(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm, BsDexValue handler,
    uintptr_t *native_queue_identity, uint64_t *owner_thread_id) {
    if (native_queue_identity) *native_queue_identity = 0;
    if (owner_thread_id) *owner_thread_id = 0;
    if (!native_queue_identity || !owner_thread_id) return 0;
    BsDexHorizonHandlerRecord *record = find_record(host, vm, handler);
    if (!record || !record->constructed) return 0;
    uintptr_t observed_queue = 0;
    uint64_t observed_thread = 0;
    if (!observed_owner(host, &observed_queue, &observed_thread) ||
        record->queue_identity != observed_queue ||
        record->owner_thread_id != observed_thread) return 0;
    /* The host callback runs BEFORE the VM commits the original invoke.
     * Requiring the VM snapshot catches partial or failed constructions. */
    BsDexHandlerSnapshot vm_state = {0};
    if (!bs_dex_vm_handler_snapshot(vm, handler, &vm_state) ||
        !vm_state.allocated || !vm_state.initialized ||
        vm_state.native_main_queue_identity != observed_queue ||
        vm_state.native_main_owner_thread_id != observed_thread) return 0;
    *native_queue_identity = observed_queue;
    *owner_thread_id = observed_thread;
    return 1;
}

void bs_dex_horizon_java_handler_shutdown(BsDexHorizonJavaHandlerHost *host) {
    if (!host) return;
    uint64_t generation = host->generation + 1;
    *host = (BsDexHorizonJavaHandlerHost){0};
    host->generation = generation;
}
