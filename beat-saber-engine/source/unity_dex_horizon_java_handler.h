/* Bounded, executable Android Handler constructor adapter for the original
 * Quest DEX VM. This owns actual Handler-compatible native state associated
 * with the observed Horizon main ALooper; it is NOT a JNI jobject or a
 * complete Java Handler/message dispatcher. Handler.post and Runnable.run
 * are intentionally unavailable until an executable Java scheduler exists.
 */
#ifndef BS_UNITY_DEX_HORIZON_JAVA_HANDLER_H
#define BS_UNITY_DEX_HORIZON_JAVA_HANDLER_H

#include "unity_dex_horizon_main_looper.h"
#include <stddef.h>
#include <stdint.h>

#define BS_HJH_MAX_HANDLERS 16u

typedef int (*BsDexHorizonHandlerObserve)(
    void *user, uintptr_t *queue_identity,
    uint64_t *owner_thread_id, uint64_t *current_thread_id);

typedef struct BsDexHorizonHandlerRecord {
    BsDexVM *vm;                  /* borrowed; do not use after VM destroy */
    BsDexValue reference;        /* exact VM reference for this object */
    uintptr_t queue_identity;    /* observed real native ALooper pointer */
    uint64_t owner_thread_id;    /* observed OS owner thread */
    uint64_t generation;
    int allocated;
    int constructed;
} BsDexHorizonHandlerRecord;

typedef struct BsDexHorizonJavaHandlerHost {
    BsDexHorizonMainLooper *native_main_loop; /* borrowed, must outlive host */
    BsDexHorizonHandlerObserve observe;
    void *observer_user;
    BsDexHorizonHandlerRecord handlers[BS_HJH_MAX_HANDLERS];
    uint64_t generation;
    int initialized;
} BsDexHorizonJavaHandlerHost;

int bs_dex_horizon_java_handler_init(
    BsDexHorizonJavaHandlerHost *host, BsDexHorizonMainLooper *native_loop,
    BsDexHorizonHandlerObserve observe, void *observer_user);
int bs_dex_horizon_java_handler_new_platform_object(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    const char *descriptor, BsDexValue *object, const char **exception);
int bs_dex_horizon_java_handler_invoke_platform(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    const char *klass, const char *name, const char *signature,
    uint8_t opcode, const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);
int bs_dex_horizon_java_handler_instance_of(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm,
    BsDexValue object, const char *descriptor, int *result);
size_t bs_dex_horizon_java_handler_constructed_count(
    const BsDexHorizonJavaHandlerHost *host);
/* Read-only proof that the SAME original Quest DEX Handler instance passed
 * both the native host constructor and VM's committed invoke instruction.
 * This proves a constructor/owner only: it does not prove an Android Java
 * Looper, MessageQueue, Runnable.run or Handler.post implementation. */
int bs_dex_horizon_java_handler_original_constructor_ready(
    BsDexHorizonJavaHandlerHost *host, BsDexVM *vm, BsDexValue handler,
    uintptr_t *native_queue_identity, uint64_t *owner_thread_id);
void bs_dex_horizon_java_handler_shutdown(BsDexHorizonJavaHandlerHost *host);

#endif
