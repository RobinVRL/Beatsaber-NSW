/* Minimal, bounded Dalvik interpreter for the ORIGINAL Quest classes.dex.
 * Strictly isolated: neither JNI nor Unity lifetime is modified by this VM.
 * Java platform services must be actually backed by the supplied host.
 */
#ifndef BS_UNITY_DEX_VM_H
#define BS_UNITY_DEX_VM_H

#include <stddef.h>
#include <stdint.h>

typedef struct BsDexVM BsDexVM;

typedef enum BsDexValueKind {
    BS_DEX_INT = 1,
    BS_DEX_REF = 2,
    BS_DEX_WIDE = 3
} BsDexValueKind;

typedef struct BsDexValue {
    uintptr_t bits;
    BsDexValueKind kind;
} BsDexValue;

typedef enum BsDexStopKind {
    BS_DEX_STOP_INVALID = 0,
    BS_DEX_STOP_HOST_MISSING,
    BS_DEX_STOP_OPCODE_MISSING,
    BS_DEX_STOP_JAVA_EXCEPTION,
    BS_DEX_STOP_CORRUPT,
    BS_DEX_STOP_LIMIT,
    BS_DEX_STOP_CHECKPOINT,
    BS_DEX_STOP_RETURNED
} BsDexStopKind;

typedef struct BsDexStop {
    BsDexStopKind kind;
    uint32_t code_off;
    uint32_t dex_pc;
    uint32_t steps;
    uint32_t depth;
    uint32_t method_id;
    uint32_t instruction_count;
    uint8_t opcode;
    const char *method_class;  /* descriptor from original DEX */
    const char *method_name;
    const char *host_class;    /* class requiring a real Android service */
    const char *host_method;
    const char *exception;     /* original exception descriptor, if raised */
    const char *detail;
} BsDexStop;

typedef struct BsDexTraceFrame {
    uint32_t method_id;
    uint32_t code_off;
    uint32_t dex_pc;
    const char *class_descriptor;
    const char *method_name;
} BsDexTraceFrame;

typedef struct BsDexCheckpointSnapshot {
    BsDexValue unity_player;
    BsDexValue player_context;
    BsDexValue player_activity;
    BsDexValue static_current_context;
    BsDexValue static_current_activity;
    BsDexValue handler;
    BsDexValue ui_thread;
    BsDexValue frame_layout;
    BsDexValue surface_view;
    int verified_activity_branch;
    int unity_class_initialized;
    int original_activity_instance_validated;
} BsDexCheckpointSnapshot;

/* Allocation-only state observed from a VM-owned Java fixed thread pool.
 * Java LinkedBlockingQueue() advertises Integer.MAX_VALUE as its logical
 * capacity; vm_queue_limit is an independent bounded memory safety budget.
 * No Java worker/Runnable can be scheduled by these metadata operations. */
typedef struct BsDexExecutorSnapshot {
    uint32_t core_size;
    uint32_t max_size;
    uint32_t live_workers;
    uint32_t queued_tasks;
    uint64_t completed_tasks;
    uint32_t java_queue_capacity;
    uint32_t vm_queue_limit;
    BsDexValue thread_factory;
    BsDexValue queue;
    int shutdown;
    int terminated;
} BsDexExecutorSnapshot;

/* Allocation and construction evidence for a host-backed Android Handler.
 * Both stages require a live independently observed native main Looper and
 * the actual OS calling thread equal to its owning thread. These fields do
 * not confer a runnable queue or canonical Java Looper identity. */
typedef struct BsDexHandlerSnapshot {
    uintptr_t native_main_queue_identity;
    uint64_t native_main_owner_thread_id;
    int allocated;
    int initialized;
} BsDexHandlerSnapshot;

int bs_dex_vm_handler_snapshot(BsDexVM *vm, BsDexValue handler,
                                BsDexHandlerSnapshot *out);
/* Read-only inspection of exactly the method gate used by original DEX
 * invoke. It does not call host callbacks or perform any guest operation:
 * post/removeCallbacks/getLooper must remain false even with a host stub. */
int bs_dex_vm_handler_diagnostic_method_gate(
    BsDexVM *vm, BsDexValue handler, const char *declaring_class,
    const char *method, const char *signature,
    uint8_t invoke_kind, size_t register_count);

/* Host callbacks execute actual Java PLATFORM methods and constructors.
 * DEX-owned executable methods are always interpreted by this VM, never
 * replaced by a callback. Return 1 on witnessed completion, 0 if service
 * unavailable, -1 on thrown exception (exception type must be supplied).
 * No callback is authorized to assert a UnityPlayer static field write.
 */
typedef struct BsDexHost {
    void *user;
    int (*new_platform_object)(void *user, BsDexVM *vm,
                               const char *class_descriptor,
                               BsDexValue *actual_object,
                               const char **exception);
    int (*invoke_platform)(void *user, BsDexVM *vm,
                            const char *declaring_class,
                            const char *method_name,
                            const char *prototype,
                            uint8_t invoke_opcode,
                            const BsDexValue *args, size_t count,
                            BsDexValue *result,
                            const char **exception);
    int (*field_platform)(void *user, BsDexVM *vm,
                           const char *declaring_class,
                           const char *field_name,
                           const char *field_type,
                           BsDexValue receiver, int is_write,
                           BsDexValue *value,
                           const char **exception);
    int (*instance_of)(void *user, BsDexVM *vm,
                        BsDexValue value, const char *class_descriptor,
                        int *result, const char **exception);
    /* A preexisting real UnityPlayerGameActivity must be verified by host.
     * This VM will not construct an Android AppCompatActivity itself. */
    int (*validate_activity)(void *user, void *actual_activity);
    /* Host-managed Java WeakReference for an EXTERNAL referent. The host
     * creates an opaque weak handle without retaining a JNI global/other
     * strong reference. A resolved value is a borrowed, already tracked
     * BsDexValue for the SAME referent, valid only when the host has checked
     * its live lifetime. Return bs_dex_null() after reclamation/clear.
     * All four callbacks are required to construct an external weak ref.
     * create/get/clear: 1 success, 0 unavailable, -1 exception; set a valid
     * descriptor when returning -1. A successful create returns a non-NULL
     * handle. release runs exactly once on VM teardown, even after clear. */
    int (*weak_reference_create)(void *user, BsDexVM *vm,
                                 BsDexValue referent, void **weak_handle,
                                 const char **exception);
    int (*weak_reference_get)(void *user, BsDexVM *vm,
                              void *weak_handle, BsDexValue *live_or_null,
                              const char **exception);
    int (*weak_reference_clear)(void *user, BsDexVM *vm,
                                void *weak_handle, const char **exception);
    void (*weak_reference_release)(void *user, BsDexVM *vm,
                                   void *weak_handle);
    /* Java Random() calls System.nanoTime() after seedUniquifier(). The
     * value MUST come from a real monotonic host clock, measured when this
     * callback is invoked, in nanoseconds. Return 1 for an actual reading,
     * 0 when unavailable. The VM never substitutes a constant/epoch/guess.
     * The host must provide nondecreasing observations in one VM session. */
    int (*monotonic_nanoseconds)(void *user, uint64_t *nanoseconds);
    /* Independently OBSERVE a currently live, main-owned native event queue
     * and the genuine OS thread IDs associated with it and this call.
     * Return 1 only when queue_identity, owner_thread_id and
     * current_thread_id are nonzero, OS-measured, and the main queue is
     * actually registered to that owner. queue_identity must remain stable
     * for one queue lifetime and change when that lifetime changes.
     * Return 0 when a real verified Looper/thread cannot be observed.
     *
     * This authorizes ONLY Java Looper.getMainLooper/getThread and
     * Thread.currentThread identity comparisons, never Java scheduling,
     * Handler readiness, Runnable dispatch, or an Android Thread object. */
    int (*observe_main_looper)(void *user, uintptr_t *queue_identity,
                               uint64_t *owner_thread_id,
                               uint64_t *current_thread_id);
} BsDexHost;

BsDexValue bs_dex_int(int32_t value);
BsDexValue bs_dex_null(void);

/* Import a real Android object into the VM's reference table. The exact
 * class descriptor must be verified by the host. Returned refs are opaque
 * and remain owned by the VM until destroy. */
BsDexValue bs_dex_wrap_platform(BsDexVM *vm, void *actual_object,
                                const char *class_descriptor);
/* Bind a VM-allocated ORIGINAL DEX subclass to an actual Java object of
 * that exact class after the host has created it. The host's instance_of
 * callback MUST verify its real Java identity; a guessed pointer or
 * base-class-only SurfaceView fails. No assignment or constructor is
 * inferred from this operation. Binding is single-assignment. */
int bs_dex_bind_original_object(BsDexVM *vm, BsDexValue original_ref,
                                 void *actual_object);
void *bs_dex_host_pointer(BsDexVM *vm, BsDexValue object);
const char *bs_dex_string(BsDexVM *vm, BsDexValue string_ref);
/* Convert an explicitly observed Java UTF-8 String VALUE from a real host
 * operation (e.g. Intent.getStringExtra) into an owned VM Java String.
 * Length is exact; embedded NUL and malformed UTF-8 are rejected. This does
 * not claim a real JNI String object, and cannot stand in for an unobserved
 * Android service result. NULL return is encoded as bs_dex_null(). */
BsDexValue bs_dex_new_observed_string_utf8(BsDexVM *vm,
                                           const char *observed,
                                           size_t length);
/* Attach independently measured UTF-8 characters to an EXISTING live
 * java.lang.String host reference. Caller must verify the characters came
 * from this exact host object (e.g. JNI GetStringUTFChars). Never guesses
 * missing characters. A successful observation is immutable. */
int bs_dex_attach_observed_string_utf8(BsDexVM *vm,
                                       BsDexValue existing_java_string,
                                       const char *observed, size_t length);
const char *bs_dex_reference_type(BsDexVM *vm, BsDexValue object);
/* Inspect/clear a constructed VM WeakReference through precisely the same
 * implementation as its original Java get()/clear() invocations. These
 * return 0 on absent/unavailable/invalid external host observation. They
 * do not acquire a strong native reference or establish Java liveness. */
int bs_dex_vm_weak_reference_get(BsDexVM *vm, BsDexValue weak,
                                 BsDexValue *live_or_null);
int bs_dex_vm_weak_reference_clear(BsDexVM *vm, BsDexValue weak);
/* Read-only diagnostic lookup in allocation order. Never resolves a
 * referent or asserts its liveness; useful for validating host weak lifetimes
 * after the original constructor allocates a WeakReference. */
BsDexValue bs_dex_vm_weak_reference_at(BsDexVM *vm, size_t ordinal);
/* Inspect already initialized enum constants in allocation order. Values
 * come exclusively from executed original Enum.<init>(String,int) calls. */
int bs_dex_vm_enum_at(BsDexVM *vm, size_t index,
                       BsDexValue *name, int32_t *ordinal);
/* Real original DEX Enum constants, in constructor-observed allocation
 * order. Diagnostic compare exercises Java Enum.compareTo's final ordinal
 * implementation and reports actual Java exceptions without guest progress. */
BsDexValue bs_dex_vm_enum_instance_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_enum_compare_diagnostic_call(BsDexVM *vm, BsDexValue first,
                                            BsDexValue second,
                                            BsDexValue *result,
                                            const char **exception);
/* Diagnose VM-owned ArrayDeque semantics after its ORIGINAL constructor
 * has executed. Calls share the exact Java-library dispatcher used by DEX
 * invoke-virtual, but are explicitly host-test operations: they do NOT
 * execute guest bytecode or advance the VM's checkpoint/readiness state.
 * Only a constructed VM-owned deque and 0-1 arguments are accepted.
 * Returns 1 executed, 0 unavailable, -1 Java exception, -3 unknown equals.
 * This interface must never be used as evidence of original guest progress. */
BsDexValue bs_dex_vm_array_deque_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_array_deque_diagnostic_call(BsDexVM *vm, BsDexValue deque,
                                          const char *method,
                                          const char *signature,
                                          const BsDexValue *arg,
                                          BsDexValue *result,
                                          const char **exception);
/* Read-only enumeration and explicit test-only dispatch of VM-owned
 * AtomicInteger objects initialized by original guest constructors.
 * Diagnostic calls exercise the same java.util.concurrent.atomic.AtomicInteger
 * methods as DEX invoke, but do not execute original DEX instructions or
 * establish Unity/Android readiness. Return 1 executed, 0 unavailable. */
BsDexValue bs_dex_vm_atomic_integer_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_atomic_integer_diagnostic_call(BsDexVM *vm,
                                               BsDexValue integer,
                                               const char *method,
                                               const char *signature,
                                               const BsDexValue *arguments,
                                               size_t argument_count,
                                               BsDexValue *result);
/* Inspect VM-owned java.util.Random created by the ORIGINAL DEX execution.
 * Diagnostic operations run the same Java-library method implementation,
 * without executing a guest DEX instruction or publishing Java/Unity state.
 * A receiver must have run its real interpreted Random constructor.
 * 1 = executed, 0 = unsupported/unavailable, -1 = Java exception. */
BsDexValue bs_dex_vm_random_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_random_diagnostic_call(BsDexVM *vm, BsDexValue random_ref,
                                     const char *method,
                                     const char *signature,
                                     const BsDexValue *args,
                                     size_t argument_register_count,
                                     BsDexValue *result,
                                     const char **exception);
/* Inspect VM-owned, initialized android.os.Bundle Java containers. Only
 * concrete primitive/String/Bundle values are supported; external Bundles,
 * Parcelable, ClassLoader and Parcel operations require a real Android host.
 * Diagnostic calls share the original interpreter method implementation,
 * without executing any additional guest bytecode. */
BsDexValue bs_dex_vm_bundle_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_bundle_diagnostic_call(BsDexVM *vm, BsDexValue bundle,
                                     const char *method,
                                     const char *signature,
                                     const BsDexValue *args,
                                     size_t argument_register_count,
                                     BsDexValue *result);
/* VM-owned ThreadPoolExecutor objects returned by the actual original DEX
 * Executors.newFixedThreadPool(n, suppliedThreadFactory) invocation.
 * Diagnostic calls exercise the real VM dispatcher but never execute guest
 * instructions or pretend Runnable tasks ran. A submission returns -4
 * (host scheduler needed), not success; no queue/task state is mutated. */
BsDexValue bs_dex_vm_executor_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_executor_snapshot(BsDexVM *vm, BsDexValue executor,
                                BsDexExecutorSnapshot *out);
int bs_dex_vm_executor_diagnostic_call(BsDexVM *vm, BsDexValue executor,
                                       const char *method,
                                       const char *signature,
                                       const BsDexValue *argument,
                                       BsDexValue *result,
                                       const char **exception);
/* Explicitly exercise factory validation/limits through the same
 * VM implementation; this is a test-only call and does not execute or
 * advance the original DEX. Return -1 Java exception, -3 VM budget limit,
 * 0 unsupported type, 1 allocation-only success. */
int bs_dex_vm_executor_diagnostic_create(BsDexVM *vm, int32_t threads,
                                          BsDexValue thread_factory,
                                          BsDexValue *executor,
                                          const char **exception);
/* Read-only tokens materialized ONLY after observing a live host queue and
 * actual native owner/current thread IDs. These are limited Java identity
 * tokens, not usable Android Handler or Thread scheduling objects. */
BsDexValue bs_dex_vm_observed_main_looper(BsDexVM *vm);
int bs_dex_vm_observed_thread_id(BsDexVM *vm, BsDexValue java_thread,
                                uint64_t *thread_id);
/* Read canonical Boolean valueOf() results only after the original DEX has
 * executed the Java boxing call. Never creates a Boolean or advances DEX. */
BsDexValue bs_dex_vm_cached_boolean(BsDexVM *vm, int value);
/* Inspect a VM-owned, actually constructed java.util.HashMap and exercise
 * bounded Java-key semantics without running guest instructions. A key
 * requiring unavailable hashCode/equals returns -4; a resource limit -3. */
BsDexValue bs_dex_vm_hash_map_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_hash_map_diagnostic_call(BsDexVM *vm, BsDexValue map,
                                       const char *method,
                                       const char *signature,
                                       const BsDexValue *arguments,
                                       size_t argument_count,
                                       BsDexValue *result);
/* Actual original Collections.synchronizedMap(HashMap) returns a DISTINCT
 * VM-owned wrapper with its own monitor mutex (= wrapper identity) around
 * a concrete original HashMap. The APK has no DEX type-id for internal
 * Collections$SynchronizedMap, so only its declared Map interface and
 * supported map operations are exposed; runtime class reflection and
 * actual cross-thread guest scheduling remain unavailable. */
typedef struct BsDexSynchronizedMapSnapshot {
    BsDexValue delegate;
    BsDexValue mutex;
    uint16_t monitor_depth;
} BsDexSynchronizedMapSnapshot;
BsDexValue bs_dex_vm_synchronized_map_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_synchronized_map_snapshot(BsDexVM *vm, BsDexValue wrapper,
                                         BsDexSynchronizedMapSnapshot *out);
int bs_dex_vm_synchronized_map_diagnostic_call(
    BsDexVM *vm, BsDexValue wrapper, const char *method,
    const char *signature, const BsDexValue *arguments,
    size_t argument_count, BsDexValue *result);
/* VM-owned WeakHashMap values are strong, keys are the special null sentinel,
 * NON-OWNING host weak handles for independently live external Strings, or
 * scalar identities of VM-owned objects in the VM's explicit NO-COLLECTION
 * interval. The latter are not Java strong roots and are not reclaimed:
 * guest GC/ReferenceQueue are not implemented and MUST NOT be simulated.
 * Unknown/externally-owned weak lifetimes return -5 (no insertion/no pin).
 * Diagnostic calls share original guest method dispatch but DO NOT execute
 * guest instructions, schedule GC, or mark Java/Android readiness.
 * Return 1 success, 0 unsupported, -1 actual Java exception,
 * -2 malformed host weak identity, -3 bounded VM limit,
 * -4 unavailable Java hashCode/equals, -5 missing weak-key lifetime. */
BsDexValue bs_dex_vm_weak_hash_map_at(BsDexVM *vm, size_t ordinal);
int bs_dex_vm_weak_hash_map_diagnostic_call(BsDexVM *vm,
                                            BsDexValue map,
                                            const char *method,
                                            const char *signature,
                                            const BsDexValue *arguments,
                                            size_t argument_count,
                                            BsDexValue *result,
                                            const char **exception);
typedef struct BsDexWeakMapDiagnosticCounts {
    uint32_t total_entries;
    uint32_t vm_weak_keys;
    uint32_t external_weak_keys;
    uint32_t null_keys;
} BsDexWeakMapDiagnosticCounts;
/* Reads physical weak-key entry kinds without dereferencing, collecting or
 * pinning any referent. This explicitly avoids implying a GC occurred. */
int bs_dex_vm_weak_hash_map_diagnostic_counts(
    BsDexVM *vm, BsDexValue map, BsDexWeakMapDiagnosticCounts *counts);
/* Canonical java.lang.Class tokens resolved solely from original APK DEX
 * class_def entries. Read-only: never creates classes, initializes classes,
 * loads platform classes or bypasses Android ClassLoader/reflection. */
BsDexValue bs_dex_vm_class_literal_at(BsDexVM *vm, size_t ordinal);
const char *bs_dex_vm_class_literal_target(BsDexVM *vm,
                                           BsDexValue class_literal);

/* Verify exact Quest original DEX identity, then start interpreted
 * UnityPlayerGameActivity.onCreate(Bundle) from DEX PC 0, using the supplied
 * live host activity. The Activity constructors and initializers must have
 * returned, either in a real host or via the backed DEX constructor entry.
 * A VM-created instance must be bound to the same attached host Activity.
 * No native codes are invoked directly. init returns NULL for absent or
 * mismatched DEX. */
BsDexVM *bs_dex_vm_create(const uint8_t *dex, size_t size,
                           const BsDexHost *host);
void bs_dex_vm_destroy(BsDexVM *vm);
/* Start the original no-argument UnityPlayerGameActivity constructor,
 * including its DEX class initialization chain. The platform superclass
 * constructor still requires a host. This entry does not dispatch onCreate. */
int bs_dex_vm_start_activity_constructor(BsDexVM *vm);
/* onCreate is dispatched at most once for this verified Activity; concurrent
 * or repeated attempts fail without disturbing its original DEX frames. */
int bs_dex_vm_start_activity_oncreate(BsDexVM *vm,
                                      void *actual_activity,
                                      BsDexValue saved_state);

/* Execute up to instruction_budget REAL original DEX instructions. Every
 * unsupported/missing service stops before executing that operation. VM
 * state is retained for inspection. A checkpoint only occurs after the
 * original instruction at UnityPlayer.<init> PC 0x9a has been executed.
 * There is no simulated readiness or static-field publishing. */
BsDexStop bs_dex_vm_run(BsDexVM *vm, uint32_t instruction_budget);
BsDexStop bs_dex_vm_stop(const BsDexVM *vm);
/* Read-only exact DEX method stack, starting with index 0 for onCreate.
 * Return 0 for an invalid index; never materializes missing methods. */
int bs_dex_vm_trace_frame(const BsDexVM *vm, size_t index,
                           BsDexTraceFrame *out);
/* Read exact iput/sput values from interpreted UnityPlayer constructor.
 * Returns zero until actual DEX sput-object currentActivity at PC 0x9a
 * executes, and never supplies unobserved Java object fields. */
int bs_dex_vm_checkpoint_snapshot(BsDexVM *vm,
                                   BsDexCheckpointSnapshot *out);

/* These are observed values in this interpreter's private Java field table.
 * Callers must separately validate native/Java host identity and publish
 * to the JNI bridge only if a REAL host has executed the corresponding
 * assignment. The interpreter alone cannot prove a native Activity exists. */
BsDexValue bs_dex_vm_current_context(const BsDexVM *vm);
BsDexValue bs_dex_vm_current_activity(const BsDexVM *vm);

#endif
