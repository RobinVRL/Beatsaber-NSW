#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
/* Real original Quest DEX type table; native main Looper observations are
 * fixtures. No simulated Runnable completion, attachment or JNI activity. */
#include "unity_dex_horizon_java_handler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fixture {
    int alive, same_thread, native_looper, releases;
    uint64_t owner;
} Fixture;

static int owner(void *user, uint64_t *out) {
    Fixture *f = user;
    if (out) *out = 0;
    if (!out || !f->alive) return 0;
    *out = f->owner;
    return 1;
}
static int acquire(void *user, void **ptr) {
    Fixture *f = user;
    if (ptr) *ptr = NULL;
    if (!ptr || !f->alive) return 0;
    *ptr = &f->native_looper;
    return 1;
}
static void release(void *user, void *ptr) {
    Fixture *f = user;
    assert(ptr == &f->native_looper);
    ++f->releases;
}
static int wake(void *user, void *ptr) {
    Fixture *f = user;
    return f->alive && ptr == &f->native_looper;
}
static int monotonic(void *user, uint64_t *ns) {
    Fixture *f = user;
    if (!f->alive || !ns) return 0;
    *ns = 100;
    return 1;
}
static int observed(void *user, uintptr_t *queue,
                    uint64_t *main_thread, uint64_t *current_thread) {
    Fixture *f = user;
    if (!f->alive) return 0;
    *queue = (uintptr_t)&f->native_looper;
    *main_thread = f->owner;
    *current_thread = f->same_thread ? f->owner : f->owner + 1;
    return 1;
}
static int validate_activity(void *user, void *activity) {
    (void)user;
    (void)activity;
    return 0; /* No Activity attachment in this constructor-only test. */
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *fd = fopen(argv[1], "rb");
    assert(fd);
    assert(!fseek(fd, 0, SEEK_END));
    long length = ftell(fd);
    assert(length > 0 && length < 10000000);
    rewind(fd);
    unsigned char *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, fd) == (size_t)length);
    fclose(fd);
    BsDexHost empty = {.validate_activity = validate_activity};
    BsDexVM *vm = bs_dex_vm_create(data, (size_t)length, &empty);
    assert(vm);
    Fixture f = {.alive = 1, .same_thread = 1, .owner = 0x1234};
    BsDexHorizonMainLooperNative callbacks = {
        .user = &f, .observe_owner_thread = owner,
        .acquire_native_looper = acquire,
        .release_native_looper = release, .wake = wake,
        .monotonic_time_ns = monotonic
    };
    BsDexHorizonMainLooper loop = {0};
    BsDexHorizonJavaHandlerHost host = {0};
    assert(bs_dex_horizon_java_handler_init(&host, &loop, observed, &f));
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    assert(!bs_dex_horizon_java_handler_new_platform_object(
        &host, vm, "Landroid/os/Handler;", &result, &exception));
    assert(bs_dex_horizon_main_looper_observe_main_loop(&loop, &callbacks));
    f.same_thread = 0;
    assert(!bs_dex_horizon_java_handler_new_platform_object(
        &host, vm, "Landroid/os/Handler;", &result, &exception));
    f.same_thread = 1;
    assert(bs_dex_horizon_java_handler_new_platform_object(
        &host, vm, "Landroid/os/Handler;", &result, &exception));
    assert(result.kind == BS_DEX_REF && result.bits);
    uintptr_t committed_queue = UINTPTR_MAX;
    uint64_t committed_owner = UINT64_MAX;
    assert(!bs_dex_horizon_java_handler_original_constructor_ready(
        &host, vm, result, &committed_queue, &committed_owner));
    assert(!committed_queue && !committed_owner);
    assert(!bs_dex_horizon_java_handler_constructed_count(&host));
    BsDexValue value = bs_dex_null();
    assert(!bs_dex_horizon_java_handler_invoke_platform(
        &host, vm, "Landroid/os/Handler;", "post",
        "(Ljava/lang/Runnable;)Z", 0x6e, &result, 1,
        &value, &exception));
    f.same_thread = 0;
    assert(!bs_dex_horizon_java_handler_invoke_platform(
        &host, vm, "Landroid/os/Handler;", "<init>",
        "()V", 0x70, &result, 1, &value, &exception));
    f.same_thread = 1;
    assert(bs_dex_horizon_java_handler_invoke_platform(
        &host, vm, "Landroid/os/Handler;", "<init>",
        "()V", 0x70, &result, 1, &value, &exception));
    assert(bs_dex_horizon_java_handler_constructed_count(&host) == 1);
    /* Directly calling a host callback cannot stand in for a committed
     * original DEX invoke: VM Handler snapshot must also be initialized. */
    committed_queue = UINTPTR_MAX;
    committed_owner = UINT64_MAX;
    assert(!bs_dex_horizon_java_handler_original_constructor_ready(
        &host, vm, result, &committed_queue, &committed_owner));
    assert(!committed_queue && !committed_owner);
    assert(!bs_dex_horizon_java_handler_invoke_platform(
        &host, vm, "Landroid/os/Handler;", "<init>",
        "()V", 0x70, &result, 1, &value, &exception));
    int matches = 0;
    assert(bs_dex_horizon_java_handler_instance_of(
        &host, vm, result, "Landroid/os/Handler;", &matches) && matches);
    assert(!bs_dex_horizon_main_looper_java_ready(&loop));
    assert(bs_dex_horizon_main_looper_shutdown(&loop));
    assert(f.releases == 1);
    assert(!bs_dex_horizon_java_handler_instance_of(
        &host, vm, result, "Landroid/os/Handler;", &matches));
    bs_dex_vm_destroy(vm);
    bs_dex_horizon_java_handler_shutdown(&host);
    free(data);
    puts("PASS: original Quest Handler allocation and constructor require real native main queue; no Java post or attachment");
    return 0;
}
