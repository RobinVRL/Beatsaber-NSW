#include "unity_dex_vm.h"
#include "unity_java_preinit.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* This is an interpreter, not a method-call transcript: it reads and
 * executes the actual 16-bit Dalvik instructions from the verified APK DEX.
 * The original bytecode decides control flow and writes. An Android host
 * resolves only methods with no executable original DEX body. An unresolved
 * operation terminates the run with its exact original file/DEX PC.
 *
 * The VM is deliberately single-threaded. It cannot implement Android UI,
 * Java thread scheduling or native library loading on its own. */

#define BS_DEX_MAX_FRAMES 40u
#define BS_DEX_MAX_REGISTERS 256u
#define BS_DEX_MAX_OBJECTS 768u
#define BS_DEX_MAX_STEPS 200000u
#define BS_DEX_MAGIC UINT32_C(0x44455852)
#define BS_DEX_ATOMIC_REF "Ljava/util/concurrent/atomic/AtomicReference;"
#define BS_DEX_ATOMIC_INT "Ljava/util/concurrent/atomic/AtomicInteger;"
#define BS_DEX_WEAK_MAP "Ljava/util/WeakHashMap;"
#define BS_DEX_HASH_MAP "Ljava/util/HashMap;"
#define BS_DEX_COLLECTIONS "Ljava/util/Collections;"
#define BS_DEX_MAP "Ljava/util/Map;"
#define BS_DEX_ARRAY_LIST "Ljava/util/ArrayList;"
#define BS_DEX_WEAK_REFERENCE "Ljava/lang/ref/WeakReference;"
#define BS_DEX_REFERENCE "Ljava/lang/ref/Reference;"
#define BS_DEX_ENUM "Ljava/lang/Enum;"
#define BS_DEX_COW_SET "Ljava/util/concurrent/CopyOnWriteArraySet;"
#define BS_DEX_COW_LIST "Ljava/util/concurrent/CopyOnWriteArrayList;"
#define BS_DEX_ARRAY_DEQUE "Ljava/util/ArrayDeque;"
#define BS_DEX_RANDOM "Ljava/util/Random;"
#define BS_DEX_BUNDLE "Landroid/os/Bundle;"
#define BS_DEX_OBJECT "Ljava/lang/Object;"
#define BS_DEX_EXECUTORS "Ljava/util/concurrent/Executors;"
#define BS_DEX_EXECUTOR "Ljava/util/concurrent/Executor;"
#define BS_DEX_EXECUTOR_SERVICE "Ljava/util/concurrent/ExecutorService;"
#define BS_DEX_ABSTRACT_EXECUTOR_SERVICE "Ljava/util/concurrent/AbstractExecutorService;"
#define BS_DEX_THREAD_POOL_EXECUTOR "Ljava/util/concurrent/ThreadPoolExecutor;"
#define BS_DEX_LINKED_BLOCKING_QUEUE "Ljava/util/concurrent/LinkedBlockingQueue;"
#define BS_DEX_THREAD_FACTORY "Ljava/util/concurrent/ThreadFactory;"
#define BS_DEX_LOOPER "Landroid/os/Looper;"
#define BS_DEX_HANDLER "Landroid/os/Handler;"
#define BS_DEX_THREAD "Ljava/lang/Thread;"
#define BS_DEX_BOOLEAN "Ljava/lang/Boolean;"
#define BS_DEX_SPANNABLE_BUILDER "Landroid/text/SpannableStringBuilder;"
#define BS_DEX_MAX_DEQUE_CAPACITY 8192u
#define BS_DEX_MAX_BUNDLE_ENTRIES 128u
#define BS_DEX_MAX_EXECUTOR_WORKERS 64u
#define BS_DEX_MAX_EXECUTOR_TASKS 8192u
#define BS_DEX_MAX_HASH_MAP_ENTRIES 512u
#define BS_DEX_MAX_WEAK_MAP_ENTRIES 512u
#define BS_DEX_RANDOM_MASK UINT64_C(0x0000ffffffffffff)
#define BS_DEX_RANDOM_MULTIPLIER UINT64_C(0x5deece66d)
#define BS_DEX_RANDOM_ADDEND UINT64_C(0xb)
#define BS_DEX_RANDOM_UNIQUIFIER_START UINT64_C(8682522807148012)
#define BS_DEX_RANDOM_UNIQUIFIER_MULTIPLIER UINT64_C(181783497276652981)

typedef struct BsDexFieldValue {
    uint32_t field_index;
    BsDexValue value;
} BsDexFieldValue;

typedef struct BsDexBundleEntry {
    BsDexValue key;
    BsDexValue value;
    char type; /* I=int, Z=boolean, J=long, S=String, B=Bundle */
} BsDexBundleEntry;

typedef struct BsDexHashMapEntry {
    uint32_t java_hash;
    BsDexValue key;
    BsDexValue value;
} BsDexHashMapEntry;

/* A WeakHashMap key MUST NOT be rooted by an entry: retain an opaque host
 * weak token and an integer identity used solely to validate host results.
 * Null keys follow Java's special permanent null sentinel (no weak handle).
 * Values are deliberately strong, as they are in java.util.WeakHashMap. */
typedef struct BsDexWeakMapEntry {
    uint32_t java_hash;
    void *weak_handle; /* optional NON-OWNING host weak handle */
    uintptr_t key_identity; /* NOT a BsDexValue or a Java/JNI strong global */
    BsDexValue value;
    int null_key;
    int vm_weak_key; /* VM allocation ID; no guest GC runs in this VM */
} BsDexWeakMapEntry;

typedef struct BsDexRef {
    uint32_t magic;
    uint32_t type_index;
    void *backing;         /* actual object from the host, if present */
    const char *utf8;      /* original literal for DEX const-string */
    char *owned_utf8;      /* owned text converted from observed host string */
    BsDexFieldValue *fields;
    size_t fields_count;
    size_t fields_capacity;
    BsDexValue *array;
    BsDexValue atomic_value;
    int32_t atomic_integer_value;
    int atomic_integer_initialized;
    uint64_t random_seed;
    int random_initialized;
    BsDexBundleEntry *bundle_entries;
    uint32_t bundle_count;
    uint32_t bundle_capacity;
    int bundle_initialized;
    uint32_t class_literal_target;
    int is_class_literal;
    /* java.util.concurrent.Executors.newFixedThreadPool allocates its
     * ThreadPoolExecutor and LinkedBlockingQueue eagerly, but workers lazily.
     * No Runnable has been dispatched or completed in this VM. */
    BsDexValue executor_factory;
    BsDexValue executor_queue;
    uint32_t executor_core_size;
    uint32_t executor_max_size;
    uint32_t executor_live_workers;
    uint32_t executor_queued_tasks;
    uint64_t executor_completed_tasks;
    int executor_initialized;
    int executor_shutdown;
    int executor_terminated;
    uint32_t executor_queue_vm_limit;
    uint32_t executor_queue_java_capacity;
    int executor_queue_initialized;
    uintptr_t observed_looper_identity;
    uint64_t observed_thread_id;
    int is_observed_main_looper;
    int is_observed_thread;
    /* Only a REAL host-backed Handler allocation may set these fields.
     * Default Handler() binds Looper.myLooper() to the actual calling OS
     * thread, so a live main Looper and its owner must be independently
     * observed at allocation AND again at constructor dispatch. */
    uintptr_t handler_main_queue_identity;
    uint64_t handler_owner_thread_id;
    int handler_allocated;
    int handler_initialized;
    int boolean_initialized;
    int boolean_value;
    /* The default Android SpannableStringBuilder constructor produces a
     * mutable empty UTF-16 sequence with no spans. Operations which would
     * create text/spans require separate exact implementations. */
    int empty_spannable_initialized;
    BsDexValue weak_value;
    BsDexValue enum_name;
    int32_t enum_ordinal;
    int enum_initialized;
    void *weak_external_handle; /* opaque NON-OWNING host weak reference */
    uintptr_t weak_referent_identity; /* VM reference identity; no native pin */
    int weak_initialized;
    int weak_cleared;
    /* Initial table capacity, and a bounded weak-key table. VM-only keys
     * use non-owning integer identities in an explicit no-collection VM. */
    struct BsDexRef **weak_map_buckets;
    BsDexWeakMapEntry *weak_map_entries;
    uint32_t weak_map_count;
    uint32_t weak_map_capacity;
    size_t map_initial_capacity;
    BsDexHashMapEntry *map_entries;
    uint32_t map_entry_count;
    uint32_t map_entry_capacity;
    /* Java Collections.synchronizedMap(new HashMap()) allocates a distinct
     * mutex-owning wrapper. The APK DEX has a Map interface type but has NO
     * Collections$SynchronizedMap class type ID. Retain this exact runtime
     * behavior as VM metadata while keeping unsupported class reflection
     * and arbitrary Map implementations unavailable. */
    BsDexValue synchronized_map_delegate;
    BsDexValue synchronized_map_mutex;
    int synchronized_map_initialized;
    uint32_t array_count;
    uint32_t list_capacity;
    uint32_t list_size;
    int list_initialized;
    BsDexValue *set_elements;
    uint32_t set_count;
    uint32_t set_capacity;
    int set_initialized;
    BsDexValue *deque_elements;
    uint32_t deque_head;
    uint32_t deque_size;
    uint32_t deque_capacity;
    int deque_initialized;
    uint16_t monitor_depth;
    int owns_instance;     /* new-instance interpreted by this VM */
} BsDexRef;

typedef struct BsDexFrame {
    uint32_t method_index;
    uint32_t code_off;
    uint32_t dex_pc;
    uint32_t instruction_count;
    uint16_t reg_count;
    BsDexValue reg[BS_DEX_MAX_REGISTERS];
    BsDexValue pending_result;
    int pending_result_valid;
    uint32_t initializing_type; /* type+1; 0 means a normal method */
} BsDexFrame;

struct BsDexVM {
    const uint8_t *dex;
    size_t size;
    BsDexHost host;
    uint32_t type_count, field_count, method_count, string_count;
    uint32_t type_off, field_off, method_off, proto_off, string_off;
    uint32_t class_count, class_off;
    const char **strings;
    BsDexRef **literal_refs;
    BsDexRef **class_literal_refs;
    uint32_t *class_definition;       /* type_id -> class_def offset or 0 */
    uint32_t *method_code;            /* method_id -> code_item offset */
    uint32_t *method_access;          /* method_id -> declared flags */
    BsDexValue *static_fields;
    uint8_t *static_known;            /* set only by executed sput */
    uint8_t *class_state;             /* 0 uninitialized, 1 pending, 2 done */
    BsDexRef **references;
    size_t reference_count;
    BsDexFrame *frames;
    size_t depth;
    BsDexValue activity;
    BsDexValue activity_under_construction;
    BsDexValue return_value;
    BsDexValue current_context, current_activity;
    BsDexStop stop;
    uint32_t instruction_limit;
    uint32_t initializing_entry_type; /* type+1 while constructor waits */
    int started;
    int oncreate_started; /* one original onCreate dispatch per Activity */
    int activity_instanceof_observed;
    uint64_t random_seed_uniquifier;
    uint64_t last_monotonic_ns;
    int has_monotonic_ns;
    uintptr_t observed_main_queue_identity;
    uint64_t observed_main_owner_thread_id;
    BsDexValue observed_main_looper;
    BsDexValue cached_boolean_false;
    BsDexValue cached_boolean_true;
};

static BsDexValue dex_zero(BsDexValueKind kind) {
    BsDexValue value = {0, kind};
    return value;
}

/* Dalvik's const/4 0 may represent either integer zero or a null object.
 * Reference interpretation follows the consuming opcode/field descriptor. */
static int is_dex_null(BsDexValue value) {
    return !value.bits && (value.kind == BS_DEX_INT ||
                           value.kind == BS_DEX_REF);
}

BsDexValue bs_dex_null(void) { return dex_zero(BS_DEX_REF); }

BsDexValue bs_dex_int(int32_t value) {
    BsDexValue v = {(uintptr_t)(uint32_t)value, BS_DEX_INT};
    return v;
}

static int bounds(const BsDexVM *vm, size_t offset, size_t amount) {
    return vm && offset <= vm->size && amount <= vm->size - offset;
}

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_uleb(const BsDexVM *vm, size_t *off, uint32_t *value) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (!bounds(vm, *off, 1)) return 0;
        uint8_t b = vm->dex[(*off)++];
        result |= (uint64_t)(b & 0x7f) << (i * 7);
        if (!(b & 0x80)) {
            if (result > UINT32_MAX) return 0;
            *value = (uint32_t)result;
            return 1;
        }
    }
    return 0;
}

static const char *string_id(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->string_count ? vm->strings[index] : NULL;
}

static const char *type_id(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->type_count ?
        string_id(vm, u32(vm->dex + vm->type_off + 4u * index)) : NULL;
}

static const char *method_class(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->method_count ?
        type_id(vm, u16(vm->dex + vm->method_off + 8u * index)) : NULL;
}

static const char *method_name(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->method_count ?
        string_id(vm, u32(vm->dex + vm->method_off + 8u * index + 4)) : NULL;
}

static uint32_t method_proto(const BsDexVM *vm, uint32_t index) {
    return u16(vm->dex + vm->method_off + 8u * index + 2);
}

static const char *field_class(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->field_count ?
        type_id(vm, u16(vm->dex + vm->field_off + 8u * index)) : NULL;
}

static const char *field_type(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->field_count ?
        type_id(vm, u16(vm->dex + vm->field_off + 8u * index + 2)) : NULL;
}

static const char *field_name(const BsDexVM *vm, uint32_t index) {
    return vm && index < vm->field_count ?
        string_id(vm, u32(vm->dex + vm->field_off + 8u * index + 4)) : NULL;
}

/* DEX shorty is NOT used for identity; the complete method descriptor is
 * constructed on demand from proto_id's return and parameter type indexes. */
static int method_signature(const BsDexVM *vm, uint32_t index,
                            char *buffer, size_t capacity) {
    if (!vm || index >= vm->method_count || !buffer || capacity < 4) return 0;
    const uint32_t proto_index = method_proto(vm, index);
    if (proto_index >= u32(vm->dex + 0x48)) return 0;
    const size_t off = (size_t)u32(vm->dex + 0x4c) + 12u * proto_index;
    if (!bounds(vm, off, 12)) return 0;
    const char *result = type_id(vm, u32(vm->dex + off + 4));
    const uint32_t param_off = u32(vm->dex + off + 8);
    if (!result) return 0;
    size_t pos = 0;
    buffer[pos++] = '(';
    if (param_off) {
        if (!bounds(vm, param_off, 4)) return 0;
        uint32_t count = u32(vm->dex + param_off);
        if (count > 256 || !bounds(vm, param_off + 4u, count * 2u)) return 0;
        for (uint32_t i = 0; i < count; ++i) {
            const char *p = type_id(vm, u16(vm->dex + param_off + 4u + 2u * i));
            if (!p || strlen(p) >= capacity - pos) return 0;
            size_t length = strlen(p);
            memcpy(buffer + pos, p, length);
            pos += length;
        }
    }
    if (capacity - pos < 2) return 0;
    buffer[pos++] = ')';
    if (strlen(result) >= capacity - pos) return 0;
    memcpy(buffer + pos, result, strlen(result) + 1);
    return 1;
}

static void error_stop(BsDexVM *vm, BsDexStopKind kind,
                       const char *detail, uint8_t opcode) {
    if (!vm) return;
    BsDexStop *stop = &vm->stop;
    stop->kind = kind;
    stop->detail = detail;
    stop->opcode = opcode;
    stop->depth = (uint32_t)vm->depth;
    if (!vm->depth) return;
    BsDexFrame *f = &vm->frames[vm->depth - 1];
    stop->code_off = f->code_off;
    stop->dex_pc = f->dex_pc;
    stop->method_id = f->method_index;
    stop->method_class = method_class(vm, f->method_index);
    stop->method_name = method_name(vm, f->method_index);
}

static void host_stop(BsDexVM *vm, const char *class_name,
                      const char *member_name, const char *exception,
                      uint8_t opcode, const char *detail) {
    error_stop(vm, exception ? BS_DEX_STOP_JAVA_EXCEPTION :
               BS_DEX_STOP_HOST_MISSING, detail, opcode);
    vm->stop.host_class = class_name;
    vm->stop.host_method = member_name;
    vm->stop.exception = exception;
}

static void host_failure(BsDexVM *vm, int result,
                         const char *class_name, const char *member,
                         const char *exception, uint8_t opcode,
                         const char *detail) {
    /* Platform callbacks must report the actual thrown Java exception. A
     * negative return with an absent/malformed exception cannot be treated
     * as a missing service or made into a synthesized Java Throwable. */
    if (result < 0 && (!exception || exception[0] != 'L' ||
                       strlen(exception) < 3 ||
                       exception[strlen(exception) - 1] != ';')) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "host reported Java exception without valid class", opcode);
        vm->stop.host_class = class_name;
        vm->stop.host_method = member;
        return;
    }
    host_stop(vm, class_name, member, result < 0 ? exception : NULL,
              opcode, detail);
}

static BsDexRef *known_ref(const BsDexVM *vm, BsDexValue value) {
    if (!vm || value.kind != BS_DEX_REF || !value.bits) return NULL;
    for (size_t i = 0; i < vm->reference_count; ++i)
        if ((uintptr_t)vm->references[i] == value.bits &&
            vm->references[i]->magic == BS_DEX_MAGIC)
            return vm->references[i];
    return NULL;
}

static BsDexValue ref_value(BsDexRef *obj) {
    BsDexValue result = {(uintptr_t)obj, BS_DEX_REF};
    return result;
}

static BsDexRef *allocate_ref(BsDexVM *vm, uint32_t type_index,
                              void *backing, int owns_instance,
                              const char *literal) {
    if (!vm || vm->reference_count >= BS_DEX_MAX_OBJECTS) return NULL;
    BsDexRef *obj = calloc(1, sizeof *obj);
    if (!obj) return NULL;
    obj->magic = BS_DEX_MAGIC;
    obj->type_index = type_index;
    obj->backing = backing;
    obj->owns_instance = owns_instance;
    obj->utf8 = literal;
    vm->references[vm->reference_count++] = obj;
    return obj;
}

static int descriptor_type(const BsDexVM *vm, const char *descriptor,
                           uint32_t *result) {
    if (!vm || !descriptor) return 0;
    for (uint32_t i = 0; i < vm->type_count; ++i)
        if (!strcmp(descriptor, type_id(vm, i))) {
            *result = i;
            return 1;
        }
    return 0;
}

/* DEX const-class resolves one canonical Class object per declared original
 * type. It does not execute <clinit> or claim an Android ClassLoader.
 * Unknown framework, primitive, or external types must be handled by the
 * real host and therefore cannot be invented by this interpreter. */
static BsDexValue resolve_dex_class_literal(BsDexVM *vm, uint32_t type) {
    if (!vm || type >= vm->type_count ||
        !vm->class_definition[type] || !vm->class_literal_refs)
        return bs_dex_null();
    if (vm->class_literal_refs[type])
        return ref_value(vm->class_literal_refs[type]);
    uint32_t class_type;
    if (!descriptor_type(vm, "Ljava/lang/Class;", &class_type))
        return bs_dex_null();
    BsDexRef *r = allocate_ref(vm, class_type, NULL, 0, NULL);
    if (!r) return bs_dex_null();
    r->is_class_literal = 1;
    r->class_literal_target = type;
    vm->class_literal_refs[type] = r;
    return ref_value(r);
}

/* Object.getClass() is final and returns the precise runtime class. For
 * VM-allocated ORIGINAL DEX instances without external backing, the VM
 * knows the exact allocation type and canonical Class literal. Android
 * objects (including a DEX wrapper bound to a native Activity) may have a
 * more derived class: their exact class must come from real host reflection. */
static int exact_vm_object_class(BsDexVM *vm, BsDexValue object,
                                 BsDexValue *class_literal) {
    BsDexRef *receiver = known_ref(vm, object);
    if (!receiver || !receiver->owns_instance || receiver->backing ||
        !vm->class_definition[receiver->type_index] ||
        !class_literal) return 0;
    *class_literal = resolve_dex_class_literal(vm, receiver->type_index);
    return class_literal->bits ? 1 : -3;
}

BsDexValue bs_dex_vm_class_literal_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    /* Report resolution order, not type-id order. */
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->is_class_literal) continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

const char *bs_dex_vm_class_literal_target(BsDexVM *vm,
                                           BsDexValue class_literal) {
    BsDexRef *r = known_ref(vm, class_literal);
    if (!r || !r->is_class_literal ||
        r->class_literal_target >= vm->type_count) return NULL;
    return type_id(vm, r->class_literal_target);
}

static int vm_enum_subclass(BsDexVM *vm, const BsDexRef *receiver) {
    if (!vm || !receiver || !receiver->owns_instance || receiver->backing)
        return 0;
    uint32_t type = receiver->type_index;
    for (unsigned level = 0; level < 48 && type < vm->type_count; ++level) {
        if (!strcmp(type_id(vm, type), BS_DEX_ENUM)) return 1;
        uint32_t def = vm->class_definition[type];
        if (!def) return 0;
        uint32_t parent = u32(vm->dex + def + 8u);
        if (parent == UINT32_MAX || parent == type) return 0;
        type = parent;
    }
    return 0;
}

BsDexValue bs_dex_wrap_platform(BsDexVM *vm, void *actual_object,
                                const char *class_descriptor) {
    if (!vm || !actual_object || !class_descriptor) return bs_dex_null();
    uint32_t type;
    if (!descriptor_type(vm, class_descriptor, &type)) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (r->backing == actual_object) {
            if (r->type_index == type) return ref_value(r);
            /* The Java reference's existing concrete class is canonical.
             * A requested view under another class must be verified by the
             * genuine host's class hierarchy, or explicitly rejected. */
            if (vm->host.instance_of) {
                int matches = 0;
                const char *exception = NULL;
                if (vm->host.instance_of(vm->host.user, vm, ref_value(r),
                                         class_descriptor, &matches,
                                         &exception) == 1 && matches)
                    return ref_value(r);
            }
            return bs_dex_null();
        }
    }
    return ref_value(allocate_ref(vm, type, actual_object, 0, NULL));
}

int bs_dex_bind_original_object(BsDexVM *vm, BsDexValue original_ref,
                                 void *actual_object) {
    BsDexRef *r = known_ref(vm, original_ref);
    if (!vm || !r || !r->owns_instance || !vm->class_definition[r->type_index] ||
        r->backing || !actual_object ||
        !vm->host.instance_of) return 0;
    for (size_t i = 0; i < vm->reference_count; ++i)
        if (vm->references[i]->backing == actual_object) return 0;
    /* Validation inspects an actual platform object through its assigned
     * pointer. Revert the assignment on every failure. */
    r->backing = actual_object;
    int matches = 0;
    const char *exception = NULL;
    int rc = vm->host.instance_of(vm->host.user, vm, original_ref,
                                 type_id(vm, r->type_index), &matches,
                                 &exception);
    if (rc != 1 || !matches || exception) {
        r->backing = NULL;
        return 0;
    }
    return 1;
}

void *bs_dex_host_pointer(BsDexVM *vm, BsDexValue object) {
    BsDexRef *r = known_ref(vm, object);
    return r ? r->backing : NULL;
}

const char *bs_dex_string(BsDexVM *vm, BsDexValue string_ref) {
    BsDexRef *r = known_ref(vm, string_ref);
    return r ? r->utf8 : NULL;
}

static int valid_observed_utf8(const unsigned char *input, size_t length) {
    for (size_t i = 0; i < length;) {
        unsigned char c = input[i++];
        if (c == 0) return 0; /* Java modified UTF8 requires explicit decode */
        if (c < 0x80) continue;
        uint32_t point;
        unsigned trailing;
        if (c >= 0xc2 && c <= 0xdf) {
            trailing = 1;
            point = c & 0x1fu;
        } else if (c >= 0xe0 && c <= 0xef) {
            trailing = 2;
            point = c & 0x0fu;
        } else if (c >= 0xf0 && c <= 0xf4) {
            trailing = 3;
            point = c & 0x07u;
        } else return 0;
        if (trailing > length - i) return 0;
        while (trailing--) {
            unsigned char next = input[i++];
            if ((next & 0xc0u) != 0x80u) return 0;
            point = (point << 6) | (next & 0x3fu);
        }
        if (point < 0x80u || (point < 0x800u && c >= 0xe0u) ||
            (point < 0x10000u && c >= 0xf0u) ||
            point > 0x10ffffu || (point >= 0xd800u && point <= 0xdfffu))
            return 0;
    }
    return 1;
}

int bs_dex_attach_observed_string_utf8(BsDexVM *vm,
                                       BsDexValue existing_java_string,
                                       const char *observed, size_t length) {
    BsDexRef *r = known_ref(vm, existing_java_string);
    if (!vm || !r || !r->backing || r->utf8 || r->owned_utf8 ||
        !observed || length > 65535 ||
        strcmp(type_id(vm, r->type_index), "Ljava/lang/String;") ||
        !valid_observed_utf8((const unsigned char *)observed, length))
        return 0;
    char *copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, observed, length);
    copy[length] = '\0';
    r->owned_utf8 = copy;
    r->utf8 = copy;
    return 1;
}

BsDexValue bs_dex_new_observed_string_utf8(BsDexVM *vm,
                                           const char *observed,
                                           size_t length) {
    if (!vm || !observed || length > 65535 ||
        !valid_observed_utf8((const unsigned char *)observed, length))
        return bs_dex_null();
    uint32_t type;
    if (!descriptor_type(vm, "Ljava/lang/String;", &type)) return bs_dex_null();
    char *copy = malloc(length + 1);
    if (!copy) return bs_dex_null();
    memcpy(copy, observed, length);
    copy[length] = '\0';
    BsDexRef *r = allocate_ref(vm, type, NULL, 1, copy);
    if (!r) {
        free(copy);
        return bs_dex_null();
    }
    r->owned_utf8 = copy;
    return ref_value(r);
}

const char *bs_dex_reference_type(BsDexVM *vm, BsDexValue object) {
    BsDexRef *r = known_ref(vm, object);
    return r ? type_id(vm, r->type_index) : NULL;
}

static int parse_classes(BsDexVM *vm) {
    /* Class definitions contain compressed field and method delta indexes.
     * Build the actual original method->code index once; never rely on
     * guessed hardcoded mapping between DEX method IDs and offsets. */
    for (uint32_t c = 0; c < vm->class_count; ++c) {
        size_t offset = vm->class_off + (size_t)c * 32u;
        uint32_t type = u32(vm->dex + offset);
        uint32_t data_off = u32(vm->dex + offset + 24u);
        if (type >= vm->type_count) return 0;
        vm->class_definition[type] = (uint32_t)offset;
        if (!data_off) continue;
        size_t at = data_off;
        uint32_t sizes[4];
        for (size_t i = 0; i < 4; ++i)
            if (!read_uleb(vm, &at, &sizes[i]) || sizes[i] > 65536u)
                return 0;
        for (unsigned group = 0; group < 4; ++group) {
            uint32_t index = 0;
            for (uint32_t i = 0; i < sizes[group]; ++i) {
                uint32_t delta, access, code = 0;
                if (!read_uleb(vm, &at, &delta) ||
                    delta > UINT32_MAX - index ||
                    !read_uleb(vm, &at, &access)) return 0;
                index += delta;
                if (group < 2) {
                    if (index >= vm->field_count) return 0;
                } else {
                    if (index >= vm->method_count ||
                        !read_uleb(vm, &at, &code) ||
                        (code && !bounds(vm, code, 16))) return 0;
                    vm->method_code[index] = code;
                    vm->method_access[index] = access;
                }
            }
        }
    }
    return 1;
}

BsDexVM *bs_dex_vm_create(const uint8_t *dex, size_t size,
                           const BsDexHost *host) {
    if (!dex || !host || !host->validate_activity ||
        !bs_unity_java_preinit_verify_dex(dex, size, NULL)) return NULL;
    BsDexVM *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    vm->dex = dex;
    vm->size = size;
    vm->host = *host;
    vm->random_seed_uniquifier = BS_DEX_RANDOM_UNIQUIFIER_START;
    vm->string_count = u32(dex + 0x38);
    vm->string_off = u32(dex + 0x3c);
    vm->type_count = u32(dex + 0x40);
    vm->type_off = u32(dex + 0x44);
    vm->field_count = u32(dex + 0x50);
    vm->field_off = u32(dex + 0x54);
    vm->method_count = u32(dex + 0x58);
    vm->method_off = u32(dex + 0x5c);
    vm->class_count = u32(dex + 0x60);
    vm->class_off = u32(dex + 0x64);
    vm->proto_off = u32(dex + 0x4c);
    if (vm->string_count > 200000 || vm->type_count > 65536 ||
        vm->field_count > 65536 || vm->method_count > 65536 ||
        vm->class_count > 65536 ||
        !bounds(vm, vm->string_off, (size_t)vm->string_count * 4) ||
        !bounds(vm, vm->type_off, (size_t)vm->type_count * 4) ||
        !bounds(vm, vm->field_off, (size_t)vm->field_count * 8) ||
        !bounds(vm, vm->method_off, (size_t)vm->method_count * 8) ||
        !bounds(vm, vm->class_off, (size_t)vm->class_count * 32)) goto fail;
    vm->strings = calloc(vm->string_count, sizeof *vm->strings);
    vm->literal_refs = calloc(vm->string_count, sizeof *vm->literal_refs);
    vm->class_literal_refs = calloc(vm->type_count,
                                    sizeof *vm->class_literal_refs);
    vm->class_definition = calloc(vm->type_count, sizeof *vm->class_definition);
    vm->method_code = calloc(vm->method_count, sizeof *vm->method_code);
    vm->method_access = calloc(vm->method_count, sizeof *vm->method_access);
    vm->static_fields = calloc(vm->field_count, sizeof *vm->static_fields);
    vm->static_known = calloc(vm->field_count, sizeof *vm->static_known);
    vm->class_state = calloc(vm->type_count, sizeof *vm->class_state);
    vm->references = calloc(BS_DEX_MAX_OBJECTS, sizeof *vm->references);
    vm->frames = calloc(BS_DEX_MAX_FRAMES, sizeof *vm->frames);
    if (!vm->strings || !vm->literal_refs || !vm->class_literal_refs ||
        !vm->class_definition || !vm->method_code ||
        !vm->method_access || !vm->static_fields || !vm->static_known ||
        !vm->references || !vm->frames || !vm->class_state) goto fail;
    for (uint32_t s = 0; s < vm->string_count; ++s) {
        uint32_t data_off = u32(dex + vm->string_off + 4u * s);
        size_t at = data_off;
        uint32_t count;
        if (!read_uleb(vm, &at, &count) || !bounds(vm, at, 1)) goto fail;
        const uint8_t *nul = memchr(dex + at, 0, size - at);
        if (!nul) goto fail;
        vm->strings[s] = (const char *)(dex + at);
        (void)count;
    }
    if (!parse_classes(vm)) goto fail;
    return vm;
fail:
    bs_dex_vm_destroy(vm);
    return NULL;
}

void bs_dex_vm_destroy(BsDexVM *vm) {
    if (!vm) return;
    for (size_t i = 0; i < vm->reference_count; ++i) {
        if (vm->references[i]) {
            if (vm->references[i]->weak_external_handle &&
                vm->host.weak_reference_release)
                vm->host.weak_reference_release(vm->host.user, vm,
                    vm->references[i]->weak_external_handle);
            if (vm->references[i]->weak_map_entries) {
                for (uint32_t j = 0;
                     j < vm->references[i]->weak_map_count; ++j) {
                    void *handle =
                        vm->references[i]->weak_map_entries[j].weak_handle;
                    if (handle && vm->host.weak_reference_release)
                        vm->host.weak_reference_release(vm->host.user,
                                                        vm, handle);
                }
            }
            free(vm->references[i]->fields);
            free(vm->references[i]->array);
            free(vm->references[i]->set_elements);
            free(vm->references[i]->deque_elements);
            free(vm->references[i]->bundle_entries);
            free(vm->references[i]->map_entries);
            free(vm->references[i]->weak_map_buckets);
            free(vm->references[i]->weak_map_entries);
            free(vm->references[i]->owned_utf8);
            vm->references[i]->magic = 0;
            free(vm->references[i]);
        }
    }
    free(vm->references);
    free(vm->strings);
    free(vm->literal_refs);
    free(vm->class_literal_refs);
    free(vm->class_definition);
    free(vm->method_code);
    free(vm->method_access);
    free(vm->static_fields);
    free(vm->static_known);
    free(vm->class_state);
    free(vm->frames);
    free(vm);
}

static uint32_t find_method(const BsDexVM *vm, uint32_t class_type,
                            uint32_t method_index, int virtual_lookup) {
    const uint32_t target_name = u32(vm->dex + vm->method_off + 8u * method_index + 4);
    const uint16_t target_proto = (uint16_t)method_proto(vm, method_index);
    for (size_t level = 0; level < 48; ++level) {
        for (uint32_t m = 0; m < vm->method_count; ++m)
            if (u16(vm->dex + vm->method_off + 8u * m) == class_type &&
                u16(vm->dex + vm->method_off + 8u * m + 2) == target_proto &&
                u32(vm->dex + vm->method_off + 8u * m + 4) == target_name &&
                vm->method_code[m]) return m;
        if (!virtual_lookup || class_type >= vm->type_count ||
            !vm->class_definition[class_type]) break;
        const uint32_t parent = u32(vm->dex + vm->class_definition[class_type] + 8);
        if (parent == UINT32_MAX || parent == class_type ||
            parent >= vm->type_count) break;
        class_type = parent;
    }
    return UINT32_MAX;
}

static int push_frame(BsDexVM *vm, uint32_t method_index,
                      const BsDexValue *args, size_t count) {
    if (!vm || method_index >= vm->method_count ||
        vm->depth >= BS_DEX_MAX_FRAMES) return 0;
    uint32_t off = vm->method_code[method_index];
    if (!bounds(vm, off, 16)) return 0;
    uint16_t reg_count = u16(vm->dex + off);
    uint16_t input_count = u16(vm->dex + off + 2);
    uint32_t insns = u32(vm->dex + off + 12);
    if (reg_count > BS_DEX_MAX_REGISTERS ||
        input_count > reg_count || count != input_count ||
        !bounds(vm, (size_t)off + 16, (size_t)insns * 2)) return 0;
    BsDexFrame *frame = &vm->frames[vm->depth++];
    memset(frame, 0, sizeof *frame);
    frame->method_index = method_index;
    frame->code_off = off;
    frame->instruction_count = insns;
    frame->reg_count = reg_count;
    for (size_t i = 0; i < count; ++i)
        frame->reg[reg_count - input_count + i] = args[i];
    return 1;
}

static int read_instruction(BsDexVM *vm, const BsDexFrame *frame,
                            uint32_t relative_pc, uint16_t *value) {
    if (relative_pc >= frame->instruction_count ||
        !bounds(vm, (size_t)frame->code_off + 16u +
                (size_t)relative_pc * 2u, 2)) return 0;
    *value = u16(vm->dex + frame->code_off + 16u + (size_t)relative_pc * 2u);
    return 1;
}

static int read_words(BsDexVM *vm, const BsDexFrame *frame,
                      uint16_t *words, uint32_t count) {
    if (count > 5 || frame->dex_pc >= frame->instruction_count ||
        count > frame->instruction_count - frame->dex_pc) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!read_instruction(vm, frame, frame->dex_pc + i, &words[i]))
            return 0;
    return 1;
}

static int check_register(const BsDexFrame *frame, uint32_t r) {
    return r < frame->reg_count;
}

static int declare_class_initialized(BsDexVM *vm, uint32_t type) {
    if (!vm || type >= vm->type_count) return 0;
    vm->class_state[type] = 2;
    return 1;
}

/* Returns 1 if ready, 0 if class initializer was pushed and the current
 * opcode must be retried, -1 for bad DEX/frame. No fabricated Java state. */
static int ensure_class_initialized(BsDexVM *vm, uint32_t type) {
    if (type >= vm->type_count) return -1;
    if (vm->class_state[type] == 2 || vm->class_state[type] == 1) return 1;
    uint32_t def = vm->class_definition[type];
    if (!def) return 1; /* external Android class: host owns initialization */
    uint32_t superclass = u32(vm->dex + def + 8);
    if (superclass != UINT32_MAX && superclass < vm->type_count &&
        vm->class_definition[superclass] && vm->class_state[superclass] != 2) {
        int parent = ensure_class_initialized(vm, superclass);
        if (parent != 1) return parent;
    }
    const char *clinit = "<clinit>";
    for (uint32_t method = 0; method < vm->method_count; ++method) {
        if (u16(vm->dex + vm->method_off + method * 8u) != type ||
            strcmp(method_name(vm, method), clinit)) continue;
        if (!vm->method_code[method]) return -1;
        if (!push_frame(vm, method, NULL, 0)) return -1;
        vm->class_state[type] = 1;
        vm->frames[vm->depth - 1].initializing_type = type + 1;
        return 0;
    }
    return declare_class_initialized(vm, type);
}

static BsDexValue default_field_value(const BsDexVM *vm, uint32_t field) {
    const char *descriptor = field_type(vm, field);
    if (!descriptor) return bs_dex_null();
    return descriptor[0] == 'L' || descriptor[0] == '[' ?
        bs_dex_null() : descriptor[0] == 'J' || descriptor[0] == 'D' ?
        dex_zero(BS_DEX_WIDE) : bs_dex_int(0);
}

static int find_field_value(const BsDexRef *receiver, uint32_t field,
                            BsDexValue *value) {
    for (size_t i = 0; i < receiver->fields_count; ++i) {
        if (receiver->fields[i].field_index == field) {
            *value = receiver->fields[i].value;
            return 1;
        }
    }
    return 0;
}

static int remember_field(BsDexRef *receiver, uint32_t field,
                          BsDexValue value) {
    for (size_t i = 0; i < receiver->fields_count; ++i)
        if (receiver->fields[i].field_index == field) {
            receiver->fields[i].value = value;
            return 1;
        }
    if (receiver->fields_count == receiver->fields_capacity) {
        size_t capacity = receiver->fields_capacity ?
                          receiver->fields_capacity * 2 : 8;
        if (capacity > 1024) return 0;
        BsDexFieldValue *fields = realloc(receiver->fields,
                                          capacity * sizeof *fields);
        if (!fields) return 0;
        receiver->fields = fields;
        receiver->fields_capacity = capacity;
    }
    receiver->fields[receiver->fields_count++] =
        (BsDexFieldValue){field, value};
    return 1;
}

/* Traverse every original DEX supertype and declared interface, including
 * interfaces extending other interfaces. Return 1 match, 0 proven mismatch,
 * -1 incomplete graph (external parent/interface, cyclic/corrupt graph).
 * A negative result is safe only when EVERY branch ends at the known Java
 * Object root or a fully DEX-defined terminal interface. */
static int dex_type_matches(BsDexVM *vm, uint32_t current,
                            uint32_t desired, uint32_t path[64],
                            unsigned depth) {
    if (current == desired) return 1;
    if (current >= vm->type_count || depth == 64) return -1;
    if (!strcmp(type_id(vm, current), BS_DEX_OBJECT)) return 0;
    uint32_t def = vm->class_definition[current];
    if (!def || !bounds(vm, def, 32)) return -1;
    for (unsigned i = 0; i < depth; ++i)
        if (path[i] == current) return -1;
    path[depth] = current;
    int missing = 0;
    uint32_t parent = u32(vm->dex + def + 8u);
    if (parent != UINT32_MAX) {
        int rc = dex_type_matches(vm, parent, desired, path, depth + 1);
        if (rc == 1) return 1;
        if (rc < 0) missing = 1;
    }
    uint32_t interfaces = u32(vm->dex + def + 12u);
    if (interfaces) {
        if (!bounds(vm, interfaces, 4)) return -1;
        uint32_t count = u32(vm->dex + interfaces);
        if (count > vm->type_count ||
            !bounds(vm, (size_t)interfaces + 4u,
                    (size_t)count * sizeof(uint16_t))) return -1;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t iface = u16(vm->dex + interfaces + 4u + i * 2u);
            int rc = dex_type_matches(vm, iface, desired, path, depth + 1);
            if (rc == 1) return 1;
            if (rc < 0) missing = 1;
        }
    }
    return missing ? -1 : 0;
}

/* Java source types are checked against the parsed original DEX supertype
 * AND interface graph. For VM-owned exact DEX classes, only fully traversed
 * graphs may establish a negative instanceof result. Externally backed
 * classes and incomplete Java platform ancestry require a real host. */
static int object_instance_of(BsDexVM *vm, BsDexValue object,
                              uint32_t desired_type, int *result,
                              const char **exception) {
    *result = 0;
    if (is_dex_null(object)) return 1;
    if (object.kind != BS_DEX_REF) return -1;
    BsDexRef *ref = known_ref(vm, object);
    if (!ref) return -1;
    if (ref->empty_spannable_initialized) {
        /* Java's documented class/interface graph for a VM-owned empty
         * SpannableStringBuilder. An unknown target still requires a Java
         * class host; never report an unsupported negative type result. */
        const char *requested = type_id(vm, desired_type);
        if (requested &&
            (!strcmp(requested, BS_DEX_SPANNABLE_BUILDER) ||
             !strcmp(requested, "Landroid/text/Editable;") ||
             !strcmp(requested, "Landroid/text/Spannable;") ||
             !strcmp(requested, "Landroid/text/Spanned;") ||
             !strcmp(requested, "Landroid/text/GetChars;") ||
             !strcmp(requested, "Ljava/lang/CharSequence;") ||
             !strcmp(requested, BS_DEX_OBJECT))) {
            *result = 1;
            return 1;
        }
        return 0;
    }
    if (ref->synchronized_map_initialized) {
        /* The wrapper is a real VM-owned Java Map whose hidden concrete
         * Collections$SynchronizedMap class is not present in this APK's
         * DEX type IDs. Java Map and Object assignability is provable; all
         * other class/interface checks require the missing Java runtime.
         * Never report an invented negative or concrete class identity. */
        const char *requested = type_id(vm, desired_type);
        if (requested &&
            (!strcmp(requested, BS_DEX_MAP) ||
             !strcmp(requested, BS_DEX_OBJECT))) {
            *result = 1;
            return 1;
        }
        return 0;
    }
    if (ref->backing && vm->class_definition[ref->type_index]) {
        /* A host-backed ORIGINAL DEX class may be more derived at runtime,
         * so the DEX graph proves only positive assignability. It can
         * nevertheless prove UnityPlayerGameActivity implements
         * LifecycleOwner through ComponentActivity.interfaces_off, even
         * when the host Java reflection adapter is still unavailable. */
        uint32_t path[64];
        if (dex_type_matches(vm, ref->type_index, desired_type,
                             path, 0) == 1) {
            *result = 1;
            return 1;
        }
    }
    if (ref->owns_instance && !ref->backing &&
        vm->class_definition[ref->type_index]) {
        uint32_t path[64];
        int match = dex_type_matches(vm, ref->type_index,
                                     desired_type, path, 0);
        if (match >= 0) {
            *result = match;
            return 1;
        }
        /* No native JVM owns this VM-only instance. A host instanceof
         * callback cannot reliably inspect its missing platform ancestry. */
        return 0;
    }
    uint32_t current = ref->type_index;
    for (unsigned level = 0; level < 48; ++level) {
        if (current == desired_type) {
            *result = 1;
            return 1;
        }
        if (current >= vm->type_count ||
            !vm->class_definition[current]) break;
        uint32_t next = u32(vm->dex + vm->class_definition[current] + 8);
        if (next == UINT32_MAX || next == current) break;
        current = next;
    }
    if (!vm->host.instance_of) return 0;
    return vm->host.instance_of(vm->host.user, vm, object,
                               type_id(vm, desired_type), result, exception);
}

static int field_operation(BsDexVM *vm, BsDexValue receiver,
                           uint32_t field, int is_static, int is_write,
                           BsDexValue *value, uint8_t opcode) {
    if (field >= vm->field_count) {
        error_stop(vm, BS_DEX_STOP_CORRUPT, "DEX field ID out of range", opcode);
        return 0;
    }
    BsDexRef *ref = NULL;
    if (!is_static) {
        ref = known_ref(vm, receiver);
        if (!ref) {
            if (is_dex_null(receiver))
                error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                           "NullPointerException on instance field", opcode);
            else error_stop(vm, BS_DEX_STOP_CORRUPT,
                            "instance field requires a tracked Java reference", opcode);
            vm->stop.exception = "Ljava/lang/NullPointerException;";
            return 0;
        }
        /* java.lang.Enum's native base constructor initialized these final
         * fields. The original APK does not contain the platform class's
         * field-writing bytecode, so read the exact values captured there. */
        if (!strcmp(field_class(vm, field), BS_DEX_ENUM) &&
            ((!strcmp(field_name(vm, field), "name") &&
              !strcmp(field_type(vm, field), "Ljava/lang/String;")) ||
             (!strcmp(field_name(vm, field), "ordinal") &&
              !strcmp(field_type(vm, field), "I"))) &&
            vm_enum_subclass(vm, ref)) {
            if (!ref->enum_initialized || is_write) {
                error_stop(vm, BS_DEX_STOP_CORRUPT,
                           "immutable enum field accessed outside initialized state",
                           opcode);
                return 0;
            }
            *value = !strcmp(field_name(vm, field), "name") ?
                     ref->enum_name : bs_dex_int(ref->enum_ordinal);
            return 1;
        }
    }
    BsDexValue old = bs_dex_null();
    int known = is_static ? vm->static_known[field] :
                find_field_value(ref, field, &old);
    if (is_static && known) old = vm->static_fields[field];
    /* Existing external Java objects may carry fields this interpreter has
     * not observed. Reading them requires a real host snapshot. Modifying
     * them requires the host to perform the actual field write first. */
    const int from_host = (ref && !ref->owns_instance) ||
        (is_static && !vm->class_definition[
             u16(vm->dex + vm->field_off + field * 8u)]);
    if (from_host && (is_write || !known)) {
        if (!vm->host.field_platform) {
            host_stop(vm, field_class(vm, field), field_name(vm, field),
                      NULL, opcode, "live external Java field unavailable");
            return 0;
        }
        const char *exception = NULL;
        BsDexValue host_value = is_write ? *value : bs_dex_null();
        int rc = vm->host.field_platform(vm->host.user, vm,
                        field_class(vm, field), field_name(vm, field),
                        field_type(vm, field), receiver, is_write,
                        &host_value, &exception);
        if (rc != 1) {
            host_failure(vm, rc, field_class(vm, field), field_name(vm, field),
                         exception, opcode,
                         "live external Java field read/write failed");
            return 0;
        }
        if (!is_write) *value = host_value;
    } else if (!is_write) {
        *value = known ? old : default_field_value(vm, field);
    }
    const char *expected_type = field_type(vm, field);
    if (!expected_type) {
        error_stop(vm, BS_DEX_STOP_CORRUPT, "missing DEX field type", opcode);
        return 0;
    }
    if (expected_type[0] == 'L' || expected_type[0] == '[') {
        if (is_dex_null(*value)) *value = bs_dex_null();
        if (value->kind != BS_DEX_REF) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "Java object field received non-reference value", opcode);
            return 0;
        }
    } else if (value->kind == BS_DEX_REF) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "Java primitive field received object reference", opcode);
        return 0;
    }
    if (value->kind == BS_DEX_REF && value->bits &&
        !known_ref(vm, *value)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "host returned a reference outside the VM", opcode);
        return 0;
    }
    if (is_static) {
        if (is_write) {
            vm->static_fields[field] = *value;
            vm->static_known[field] = 1;
        }
    } else if (is_write && !remember_field(ref, field, *value)) {
        error_stop(vm, BS_DEX_STOP_LIMIT, "Java object fields exceeded budget", opcode);
        return 0;
    }
    return 1;
}

static int return_frame(BsDexVM *vm, BsDexValue value) {
    if (!vm->depth) return 0;
    BsDexFrame *f = &vm->frames[vm->depth - 1];
    const int implicit_class_init = f->initializing_type != 0;
    if (f->initializing_type) {
        uint32_t type = f->initializing_type - 1;
        if (type < vm->type_count) vm->class_state[type] = 2;
    }
    --vm->depth;
    if (vm->depth && !implicit_class_init) {
        BsDexFrame *parent = &vm->frames[vm->depth - 1];
        parent->pending_result = value;
        parent->pending_result_valid = 1;
    } else if (!vm->depth) {
        vm->return_value = value;
        error_stop(vm, BS_DEX_STOP_RETURNED, "Java entry method returned", 0);
    }
    return 1;
}

static BsDexValue literal_string(BsDexVM *vm, uint32_t index) {
    if (index >= vm->string_count) return bs_dex_null();
    BsDexRef *r = vm->literal_refs[index];
    if (!r) {
        uint32_t type;
        if (!descriptor_type(vm, "Ljava/lang/String;", &type))
            return bs_dex_null();
        r = allocate_ref(vm, type, NULL, 0, string_id(vm, index));
        if (r) vm->literal_refs[index] = r;
    }
    return ref_value(r);
}

/* The VM has one interpreter thread. A VM-owned reference's value is
 * accessed only by that thread, with no platform callback between a read
 * and write in a Java operation. Externally backed objects use the host. */
static int construct_map(BsDexVM *vm, const char *klass, const char *signature,
                         uint8_t kind, const BsDexValue *args, size_t count) {
    if (kind != 0x70 || !count) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver || !receiver->owns_instance || receiver->backing ||
        receiver->map_initial_capacity ||
        strcmp(type_id(vm, receiver->type_index), klass)) return 0;
    size_t capacity = 16;
    if (count == 2 && !strcmp(signature, "(I)V") &&
        args[1].kind == BS_DEX_INT) {
        int32_t requested = (int32_t)args[1].bits;
        if (requested < 0 || requested > (int32_t)BS_DEX_MAX_OBJECTS) return 0;
        capacity = 1;
        while (capacity < (size_t)requested) capacity *= 2;
    } else if (count != 1 || strcmp(signature, "()V")) {
        return 0;
    }
    if (!strcmp(klass, BS_DEX_WEAK_MAP)) {
        receiver->weak_map_buckets =
            calloc(capacity, sizeof *receiver->weak_map_buckets);
        if (!receiver->weak_map_buckets) return 0;
    }
    receiver->map_initial_capacity = capacity;
    return 1;
}

/* Java String.hashCode is a polynomial over UTF-16 code units, including
 * surrogate pairs for supplementary characters. Only UTF-8 values already
 * observed and validated by this VM are hashed locally; an unknown external
 * String's characters require an actual host value observation. */
static int known_java_string_hash(const char *string, uint32_t *result) {
    if (!string || !result ||
        !valid_observed_utf8((const unsigned char *)string, strlen(string)))
        return 0;
    const unsigned char *p = (const unsigned char *)string;
    uint32_t hash = 0;
    while (*p) {
        uint32_t point;
        if (*p < 0x80u) point = *p++;
        else if ((*p & 0xe0u) == 0xc0u) {
            point = ((uint32_t)(p[0] & 0x1fu) << 6) |
                    (uint32_t)(p[1] & 0x3fu);
            p += 2;
        } else if ((*p & 0xf0u) == 0xe0u) {
            point = ((uint32_t)(p[0] & 0x0fu) << 12) |
                    ((uint32_t)(p[1] & 0x3fu) << 6) |
                    (uint32_t)(p[2] & 0x3fu);
            p += 3;
        } else {
            point = ((uint32_t)(p[0] & 0x07u) << 18) |
                    ((uint32_t)(p[1] & 0x3fu) << 12) |
                    ((uint32_t)(p[2] & 0x3fu) << 6) |
                    (uint32_t)(p[3] & 0x3fu);
            p += 4;
        }
        if (point > 0xffffu) {
            point -= 0x10000u;
            hash = hash * 31u + 0xd800u + (point >> 10);
            hash = hash * 31u + 0xdc00u + (point & 0x3ffu);
        } else hash = hash * 31u + point;
    }
    *result = hash;
    return 1;
}

/* Prove that the exact VM-owned DEX class inherits Object.equals/hashCode.
 * Any real override or external ancestor could run arbitrary Java methods,
 * so a map operation on such a key must stop before invoking it. */
static int vm_identity_hash_equals(BsDexVM *vm, BsDexRef *key) {
    if (!vm || !key || !key->owns_instance || key->backing) return 0;
    if (!strcmp(type_id(vm, key->type_index), BS_DEX_OBJECT)) return 1;
    uint32_t type = key->type_index;
    for (unsigned depth = 0; depth < 48 && type < vm->type_count; ++depth) {
        if (!strcmp(type_id(vm, type), BS_DEX_OBJECT)) return 1;
        uint32_t def = vm->class_definition[type];
        if (!def) return 0;
        for (uint32_t m = 0; m < vm->method_count; ++m) {
            if (u16(vm->dex + vm->method_off + 8u * m) != type)
                continue;
            const char *name = method_name(vm, m);
            if (name && (!strcmp(name, "equals") ||
                         !strcmp(name, "hashCode")))
                return 0;
        }
        uint32_t next = u32(vm->dex + def + 8u);
        if (next == type || next == UINT32_MAX) return 0;
        type = next;
    }
    return 0;
}

/* HashMap uses hashCode for bucket selection, and key.equals when hashes
 * match. The numeric identity-hash value is unspecified by Java; this VM
 * chooses a stable per-object identity only for classes proven to inherit
 * Object.hashCode. Hash collisions are resolved with actual key.equals.
 * No HashMap iteration or externally overridden hashCode is simulated. */
static int known_map_hash(BsDexVM *vm, BsDexValue key, uint32_t *hash) {
    if (!hash) return 0;
    if (is_dex_null(key)) { *hash = 0; return 1; }
    BsDexRef *ref = known_ref(vm, key);
    if (!ref) return 0;
    if (!strcmp(type_id(vm, ref->type_index), "Ljava/lang/String;"))
        return known_java_string_hash(bs_dex_string(vm, key), hash);
    if (!ref->is_class_literal && !ref->enum_initialized &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_ATOMIC_REF) &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_ATOMIC_INT) &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_WEAK_REFERENCE) &&
        !vm_identity_hash_equals(vm, ref)) return 0;
    /* The value of Java identityHashCode is not observable through these
     * map operations. Mix the VM object's stable identity to distribute
     * keys; equality remains the independent Java correctness criterion. */
    uintptr_t identity = key.bits;
    *hash = (uint32_t)(identity >> 4) ^
            (uint32_t)(identity >> (sizeof(uintptr_t) * 4u));
    return 1;
}

static int known_map_key_equals(BsDexVM *vm, BsDexValue key,
                                BsDexValue stored, int *equal) {
    if (!equal) return 0;
    if (key.kind == BS_DEX_REF && stored.kind == BS_DEX_REF &&
        key.bits == stored.bits) { *equal = 1; return 1; }
    if (is_dex_null(key)) {
        *equal = is_dex_null(stored);
        return 1;
    }
    BsDexRef *ref = known_ref(vm, key);
    if (!ref) return 0;
    if (!strcmp(type_id(vm, ref->type_index), "Ljava/lang/String;")) {
        const char *a = bs_dex_string(vm, key);
        if (!a) return 0;
        if (is_dex_null(stored)) { *equal = 0; return 1; }
        BsDexRef *other = known_ref(vm, stored);
        if (!other) return 0;
        if (strcmp(type_id(vm, other->type_index), "Ljava/lang/String;")) {
            *equal = 0;
            return 1;
        }
        const char *b = bs_dex_string(vm, stored);
        if (!b) return 0;
        *equal = !strcmp(a, b);
        return 1;
    }
    if (!ref->is_class_literal && !ref->enum_initialized &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_ATOMIC_REF) &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_ATOMIC_INT) &&
        strcmp(type_id(vm, ref->type_index), BS_DEX_WEAK_REFERENCE) &&
        !vm_identity_hash_equals(vm, ref)) return 0;
    *equal = 0; /* known Java identity equals; references differ */
    return 1;
}

static int invoke_hash_map(BsDexVM *vm, const char *name,
                           const char *signature, uint8_t kind,
                           const BsDexValue *args, size_t count,
                           BsDexValue *result) {
    if (!vm || !name || !signature || !count || count > 3) return 0;
    BsDexRef *map = known_ref(vm, args[0]);
    if (!map || !map->owns_instance || map->backing ||
        strcmp(type_id(vm, map->type_index), BS_DEX_HASH_MAP) ||
        !map->map_initial_capacity || (kind != 0x6e && kind != 0x72))
        return 0;
    if (count == 1) {
        if (!strcmp(name, "size") && !strcmp(signature, "()I")) {
            *result = bs_dex_int((int32_t)map->map_entry_count);
            return 1;
        }
        if (!strcmp(name, "isEmpty") && !strcmp(signature, "()Z")) {
            *result = bs_dex_int(map->map_entry_count == 0);
            return 1;
        }
        if (!strcmp(name, "clear") && !strcmp(signature, "()V")) {
            if (map->map_entry_count)
                memset(map->map_entries, 0, (size_t)map->map_entry_count *
                       sizeof *map->map_entries);
            map->map_entry_count = 0;
            return 1;
        }
        return 0;
    }
    const int get = !strcmp(name, "get") &&
                    !strcmp(signature, "(Ljava/lang/Object;)Ljava/lang/Object;") &&
                    count == 2;
    const int contains = !strcmp(name, "containsKey") &&
                         !strcmp(signature, "(Ljava/lang/Object;)Z") &&
                         count == 2;
    const int remove = !strcmp(name, "remove") &&
                       !strcmp(signature, "(Ljava/lang/Object;)Ljava/lang/Object;") &&
                       count == 2;
    const int put = !strcmp(name, "put") &&
                    !strcmp(signature, "(Ljava/lang/Object;Ljava/lang/Object;)"
                                       "Ljava/lang/Object;") && count == 3;
    if (!get && !contains && !remove && !put) return 0;
    if (!is_dex_null(args[1]) && !known_ref(vm, args[1])) return 0;
    if (put && !is_dex_null(args[2]) && !known_ref(vm, args[2])) return 0;
    BsDexValue key = is_dex_null(args[1]) ? bs_dex_null() : args[1];
    uint32_t hash;
    if (!known_map_hash(vm, key, &hash)) return -4;
    int32_t found = -1;
    for (uint32_t i = 0; i < map->map_entry_count; ++i) {
        BsDexHashMapEntry *entry = &map->map_entries[i];
        if (entry->java_hash != hash) continue;
        int equal = 0;
        if (!known_map_key_equals(vm, key, entry->key, &equal)) return -4;
        if (equal) { found = (int32_t)i; break; }
    }
    if (contains) { *result = bs_dex_int(found >= 0); return 1; }
    if (get || remove) {
        *result = found >= 0 ? map->map_entries[found].value : bs_dex_null();
        if (remove && found >= 0) {
            size_t trailing = map->map_entry_count - (uint32_t)found - 1;
            memmove(map->map_entries + found, map->map_entries + found + 1,
                    trailing * sizeof *map->map_entries);
            --map->map_entry_count;
        }
        return 1;
    }
    BsDexValue value = is_dex_null(args[2]) ? bs_dex_null() : args[2];
    if (found >= 0) {
        *result = map->map_entries[found].value;
        map->map_entries[found].value = value;
        return 1;
    }
    if (map->map_entry_count >= BS_DEX_MAX_HASH_MAP_ENTRIES) return -3;
    if (map->map_entry_count == map->map_entry_capacity) {
        uint32_t capacity = map->map_entry_capacity ?
                            map->map_entry_capacity * 2u : 8u;
        if (capacity > BS_DEX_MAX_HASH_MAP_ENTRIES)
            capacity = BS_DEX_MAX_HASH_MAP_ENTRIES;
        BsDexHashMapEntry *entries = realloc(map->map_entries,
                                         (size_t)capacity * sizeof *entries);
        if (!entries) return -3;
        map->map_entries = entries;
        map->map_entry_capacity = capacity;
    }
    map->map_entries[map->map_entry_count++] =
        (BsDexHashMapEntry){hash, key, value};
    *result = bs_dex_null();
    return 1;
}

BsDexValue bs_dex_vm_hash_map_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->owns_instance || r->backing || !r->map_initial_capacity ||
            strcmp(type_id(vm, r->type_index), BS_DEX_HASH_MAP))
            continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_hash_map_diagnostic_call(BsDexVM *vm, BsDexValue map,
                                       const char *method,
                                       const char *signature,
                                       const BsDexValue *arguments,
                                       size_t argument_count,
                                       BsDexValue *result) {
    if (!vm || !method || !signature || !result ||
        argument_count > 2 || (argument_count && !arguments)) return 0;
    BsDexValue args[3] = {map, bs_dex_null(), bs_dex_null()};
    for (size_t i = 0; i < argument_count; ++i)
        args[i + 1] = arguments[i];
    *result = bs_dex_null();
    return invoke_hash_map(vm, method, signature, 0x6e,
                           args, argument_count + 1, result);
}

/* Original FragmentManager.<init> calls Collections.synchronizedMap three
 * times with newly allocated, fully initialized VM-owned HashMaps.
 *
 * Java's Collections$SynchronizedMap is absent from this APK's DEX type-id
 * table. A distinct VM object carries the available Map interface type and
 * explicit private wrapper state: original backing Map, identity mutex
 * (the wrapper itself), and monitor depth. The exact hidden runtime class
 * cannot be reflected without a Java class loader, and is NEVER invented.
 *
 * Every map call executes synchronously on the VM's single interpreter
 * thread while its wrapper monitor is held. No other guest threads are
 * scheduled; any unsupported delegate or re-entrant monitor overflow
 * stops rather than pretending a second-thread monitor behaved correctly. */
static int invoke_synchronized_map(BsDexVM *vm, const char *klass,
                                   const char *name, const char *signature,
                                   uint8_t kind, const BsDexValue *args,
                                   size_t count, BsDexValue *result,
                                   const char **exception) {
    if (!vm || !klass || !name || !signature || !result || !exception)
        return 0;
    const int factory = kind == 0x71 && count == 1 &&
                        !strcmp(klass, BS_DEX_COLLECTIONS) &&
                        !strcmp(name, "synchronizedMap") &&
                        !strcmp(signature,
                                "(Ljava/util/Map;)Ljava/util/Map;");
    if (factory) {
        if (is_dex_null(args[0])) {
            *exception = "Ljava/lang/NullPointerException;";
            return -1;
        }
        BsDexRef *delegate = known_ref(vm, args[0]);
        /* Only support the original concrete VM-owned HashMap, whose
         * Java hashCode/equals/map methods are already interpreted. */
        if (!delegate || !delegate->owns_instance || delegate->backing ||
            strcmp(type_id(vm, delegate->type_index), BS_DEX_HASH_MAP) ||
            !delegate->map_initial_capacity)
            return 0;
        uint32_t map_type;
        if (!descriptor_type(vm, BS_DEX_MAP, &map_type)) return -3;
        BsDexRef *wrapper = allocate_ref(vm, map_type, NULL, 1, NULL);
        if (!wrapper) return -3;
        wrapper->synchronized_map_delegate = args[0];
        wrapper->synchronized_map_mutex = ref_value(wrapper);
        wrapper->synchronized_map_initialized = 1;
        *result = ref_value(wrapper);
        return 1;
    }
    if ((kind != 0x6e && kind != 0x72) ||
        !count || count > 3) return 0;
    BsDexRef *wrapper = known_ref(vm, args[0]);
    if (!wrapper || !wrapper->owns_instance || wrapper->backing ||
        !wrapper->synchronized_map_initialized ||
        strcmp(type_id(vm, wrapper->type_index), BS_DEX_MAP) ||
        wrapper->synchronized_map_mutex.bits != args[0].bits)
        return 0;
    BsDexRef *delegate = known_ref(vm, wrapper->synchronized_map_delegate);
    if (!delegate || !delegate->owns_instance || delegate->backing ||
        strcmp(type_id(vm, delegate->type_index), BS_DEX_HASH_MAP) ||
        !delegate->map_initial_capacity) return 0;
    if (wrapper->monitor_depth == UINT16_MAX) return -3;
    BsDexValue delegated[3] = {wrapper->synchronized_map_delegate,
                               bs_dex_null(), bs_dex_null()};
    for (size_t i = 1; i < count; ++i) delegated[i] = args[i];
    ++wrapper->monitor_depth;
    const int rc = invoke_hash_map(vm, name, signature, kind, delegated,
                                   count, result);
    --wrapper->monitor_depth;
    return rc;
}

BsDexValue bs_dex_vm_synchronized_map_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *wrapper = vm->references[i];
        if (!wrapper->synchronized_map_initialized) continue;
        if (!ordinal--) return ref_value(wrapper);
    }
    return bs_dex_null();
}

int bs_dex_vm_synchronized_map_snapshot(BsDexVM *vm,
                                         BsDexValue wrapper_value,
                                         BsDexSynchronizedMapSnapshot *out) {
    BsDexRef *wrapper = known_ref(vm, wrapper_value);
    if (!wrapper || !out || !wrapper->owns_instance ||
        wrapper->backing || !wrapper->synchronized_map_initialized ||
        strcmp(type_id(vm, wrapper->type_index), BS_DEX_MAP))
        return 0;
    *out = (BsDexSynchronizedMapSnapshot){
        wrapper->synchronized_map_delegate,
        wrapper->synchronized_map_mutex,
        wrapper->monitor_depth
    };
    return 1;
}

int bs_dex_vm_synchronized_map_diagnostic_call(
    BsDexVM *vm, BsDexValue wrapper, const char *method,
    const char *signature, const BsDexValue *arguments, size_t count,
    BsDexValue *result) {
    if (!vm || !method || !signature || !result ||
        count > 2 || (count && !arguments)) return 0;
    BsDexValue args[3] = {wrapper, bs_dex_null(), bs_dex_null()};
    for (size_t i = 0; i < count; ++i) args[i + 1] = arguments[i];
    *result = bs_dex_null();
    const char *exception = NULL;
    return invoke_synchronized_map(vm, BS_DEX_MAP, method, signature,
                                    0x6e, args, count + 1,
                                    result, &exception);
}

/* WeakHashMap may keep a strong VALUE, but it must not store a strong KEY.
 * There are two safe non-null key representations:
 *  (1) an independently live external object with a real host weak handle
 *      and locally provable hashCode/equals (observed final String);
 *  (2) an integer identity for a VM-allocated object with locally provable
 *      hashCode/equals. The current bounded VM has NO garbage collector and
 *      never reclaims a VM object between allocation and VM destruction;
 *      its references[] array is an allocation registry, NOT a Java GC root.
 *      Java permits collection to be deferred indefinitely; the map can
 *      accurately keep such a non-owning key alive for this no-GC interval.
 *      Future collection MUST clear these weak identities and process their
 *      ReferenceQueue before freeing/reusing an allocation identity.
 *
 * Every WeakHashMap method observes host weak handles before touching the
 * map, matching the JDK's opportunistic expungeStaleEntries behavior after
 * the collector clears/enqueues weak references. No collector event, queue
 * notification or reachability transition is fabricated by this VM. */
static void weak_map_release_entry(BsDexVM *vm, BsDexWeakMapEntry *entry) {
    if (!vm || !entry || !entry->weak_handle) return;
    if (vm->host.weak_reference_release)
        vm->host.weak_reference_release(vm->host.user, vm,
                                        entry->weak_handle);
    entry->weak_handle = NULL;
}

static void weak_map_delete(BsDexVM *vm, BsDexRef *map, uint32_t index) {
    weak_map_release_entry(vm, &map->weak_map_entries[index]);
    uint32_t trailing = map->weak_map_count - index - 1u;
    if (trailing)
        memmove(map->weak_map_entries + index,
                map->weak_map_entries + index + 1u,
                (size_t)trailing * sizeof *map->weak_map_entries);
    --map->weak_map_count;
    memset(map->weak_map_entries + map->weak_map_count, 0,
           sizeof *map->weak_map_entries);
}

/* rc=1: resolved live key or observed cleared referent; rc=0: unavailable;
 * rc=-2: impossible/stale/foreign identity returned by the host. */
static int weak_map_live_key(BsDexVM *vm, BsDexWeakMapEntry *entry,
                             BsDexValue *live, const char **exception) {
    if (entry->null_key) {
        *live = bs_dex_null();
        return 1;
    }
    if (entry->vm_weak_key) {
        /* The bounded interpreter NEVER invokes guest GC. An allocation
         * identity remains live for this whole VM epoch; the stored integer
         * is not a strong Java edge. No ReferenceQueue event is invented. */
        BsDexValue identity = {entry->key_identity, BS_DEX_REF};
        BsDexRef *ref = known_ref(vm, identity);
        if (!ref || !ref->owns_instance || ref->backing)
            return -2;
        *live = identity;
        return 1;
    }
    if (!entry->weak_handle || !vm->host.weak_reference_get) return 0;
    BsDexValue observed = bs_dex_null();
    int rc = vm->host.weak_reference_get(vm->host.user, vm,
                                        entry->weak_handle, &observed,
                                        exception);
    if (rc != 1) return rc;
    if (is_dex_null(observed)) {
        *live = bs_dex_null();
        return 1;
    }
    BsDexRef *ref = known_ref(vm, observed);
    if (!ref || !ref->backing ||
        observed.bits != entry->key_identity) return -2;
    *live = observed;
    return 1;
}

static int weak_map_expunge(BsDexVM *vm, BsDexRef *map,
                            const char **exception) {
    for (uint32_t i = 0; i < map->weak_map_count;) {
        BsDexWeakMapEntry *entry = &map->weak_map_entries[i];
        BsDexValue key = bs_dex_null();
        int rc = weak_map_live_key(vm, entry, &key, exception);
        if (rc != 1) return rc;
        if (!entry->null_key && is_dex_null(key)) {
            /* A real host observed that this weak referent was cleared.
             * Release the handle and its strongly held VALUE now. */
            weak_map_delete(vm, map, i);
            continue;
        }
        ++i;
    }
    return 1;
}

static int invoke_weak_hash_map(BsDexVM *vm, const char *name,
                                const char *signature, uint8_t kind,
                                const BsDexValue *args, size_t count,
                                BsDexValue *result,
                                const char **exception) {
    if (!vm || !name || !signature || !result || !exception ||
        count < 1 || count > 3) return 0;
    BsDexRef *map = known_ref(vm, args[0]);
    if (!map || !map->owns_instance || map->backing ||
        strcmp(type_id(vm, map->type_index), BS_DEX_WEAK_MAP) ||
        !map->map_initial_capacity ||
        (kind != 0x6e && kind != 0x72)) return 0;

    const int size = count == 1 && !strcmp(name, "size") &&
                     !strcmp(signature, "()I");
    const int empty = count == 1 && !strcmp(name, "isEmpty") &&
                      !strcmp(signature, "()Z");
    const int clear = count == 1 && !strcmp(name, "clear") &&
                      !strcmp(signature, "()V");
    const int get = count == 2 && !strcmp(name, "get") &&
                    !strcmp(signature,
                            "(Ljava/lang/Object;)Ljava/lang/Object;");
    const int contains = count == 2 && !strcmp(name, "containsKey") &&
                         !strcmp(signature, "(Ljava/lang/Object;)Z");
    const int remove = count == 2 && !strcmp(name, "remove") &&
                       !strcmp(signature,
                               "(Ljava/lang/Object;)Ljava/lang/Object;");
    const int put = count == 3 && !strcmp(name, "put") &&
                    !strcmp(signature, "(Ljava/lang/Object;Ljava/lang/Object;)"
                                       "Ljava/lang/Object;");
    if (!size && !empty && !clear && !get && !contains && !remove && !put)
        return 0;
    if (count >= 2 && !is_dex_null(args[1]) &&
        !known_ref(vm, args[1])) return 0;
    if (put && !is_dex_null(args[2]) && !known_ref(vm, args[2]))
        return 0;

    BsDexValue key = bs_dex_null();
    uint32_t java_hash = 0;
    if (count >= 2) {
        key = is_dex_null(args[1]) ? bs_dex_null() : args[1];
        if (!is_dex_null(key)) {
            BsDexRef *referent = known_ref(vm, key);
            if (!referent) return -5;
            if (!referent->backing) {
                /* A literal-only/unknown reference lacks a verified
                 * lifetime. VM-owned DEX instances belong to this explicit
                 * no-collection interpreter epoch. */
                if (!referent->owns_instance) return -5;
                if (!known_map_hash(vm, key, &java_hash)) return -4;
            } else {
                if (!vm->host.weak_reference_create ||
                    !vm->host.weak_reference_get ||
                    !vm->host.weak_reference_clear ||
                    !vm->host.weak_reference_release)
                    return -5;
                if (!known_map_hash(vm, key, &java_hash)) return -4;
                /* Identity hash/equals for arbitrary external types is
                 * unknown. A witnessed final String is the supported type. */
                if (strcmp(type_id(vm, referent->type_index),
                           "Ljava/lang/String;") ||
                    !bs_dex_string(vm, key))
                    return -4;
            }
        }
    }

    int rc = weak_map_expunge(vm, map, exception);
    if (rc != 1) return rc;
    if (size) {
        *result = bs_dex_int((int32_t)map->weak_map_count);
        return 1;
    }
    if (empty) {
        *result = bs_dex_int(map->weak_map_count == 0);
        return 1;
    }
    if (clear) {
        while (map->weak_map_count)
            weak_map_delete(vm, map, map->weak_map_count - 1u);
        return 1;
    }
    int32_t index = -1;
    for (uint32_t i = 0; i < map->weak_map_count; ++i) {
        BsDexWeakMapEntry *entry = &map->weak_map_entries[i];
        if (entry->java_hash != java_hash) continue;
        if (entry->null_key) {
            if (is_dex_null(key)) { index = (int32_t)i; break; }
            continue;
        }
        BsDexValue live = bs_dex_null();
        rc = weak_map_live_key(vm, entry, &live, exception);
        if (rc != 1) return rc;
        if (is_dex_null(live)) return -2; /* host changed inside one map op */
        int equal = 0;
        if (!known_map_key_equals(vm, key, live, &equal)) return -4;
        if (equal) { index = (int32_t)i; break; }
    }
    if (contains) {
        *result = bs_dex_int(index >= 0);
        return 1;
    }
    if (get || remove) {
        *result = index >= 0 ? map->weak_map_entries[index].value :
                               bs_dex_null();
        if (remove && index >= 0)
            weak_map_delete(vm, map, (uint32_t)index);
        return 1;
    }
    BsDexValue value = is_dex_null(args[2]) ? bs_dex_null() : args[2];
    if (index >= 0) {
        /* Java WeakHashMap.put keeps the ORIGINAL weak key on an equal-key
         * replacement, so its lifetime is not extended to a new key. */
        *result = map->weak_map_entries[index].value;
        map->weak_map_entries[index].value = value;
        return 1;
    }
    if (map->weak_map_count >= BS_DEX_MAX_WEAK_MAP_ENTRIES) return -3;
    if (map->weak_map_count == map->weak_map_capacity) {
        uint32_t capacity = map->weak_map_capacity ?
                            map->weak_map_capacity * 2u : 8u;
        if (capacity > BS_DEX_MAX_WEAK_MAP_ENTRIES)
            capacity = BS_DEX_MAX_WEAK_MAP_ENTRIES;
        BsDexWeakMapEntry *grown = realloc(map->weak_map_entries,
                                            (size_t)capacity * sizeof *grown);
        if (!grown) return -3;
        map->weak_map_entries = grown;
        map->weak_map_capacity = capacity;
    }
    void *handle = NULL;
    if (!is_dex_null(key)) {
        BsDexRef *referent = known_ref(vm, key);
        if (referent->backing) {
            rc = vm->host.weak_reference_create(vm->host.user, vm, key,
                                                &handle, exception);
            if (rc != 1) {
                if (handle) vm->host.weak_reference_release(vm->host.user, vm,
                                                            handle);
                return rc;
            }
            if (!handle) return -2;
        }
    }
    map->weak_map_entries[map->weak_map_count++] =
        (BsDexWeakMapEntry){java_hash, handle, key.bits, value,
                            is_dex_null(key),
                            !is_dex_null(key) &&
                            !known_ref(vm, key)->backing};
    *result = bs_dex_null();
    return 1;
}

BsDexValue bs_dex_vm_weak_hash_map_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *map = vm->references[i];
        if (!map->owns_instance || map->backing ||
            !map->map_initial_capacity ||
            strcmp(type_id(vm, map->type_index), BS_DEX_WEAK_MAP))
            continue;
        if (!ordinal--) return ref_value(map);
    }
    return bs_dex_null();
}

int bs_dex_vm_weak_hash_map_diagnostic_counts(
    BsDexVM *vm, BsDexValue map_value, BsDexWeakMapDiagnosticCounts *counts) {
    BsDexRef *map = known_ref(vm, map_value);
    if (!map || !counts || !map->owns_instance || map->backing ||
        !map->map_initial_capacity ||
        strcmp(type_id(vm, map->type_index), BS_DEX_WEAK_MAP)) return 0;
    memset(counts, 0, sizeof *counts);
    counts->total_entries = map->weak_map_count;
    for (uint32_t i = 0; i < map->weak_map_count; ++i) {
        BsDexWeakMapEntry *entry = &map->weak_map_entries[i];
        if (entry->null_key) ++counts->null_keys;
        else if (entry->vm_weak_key) ++counts->vm_weak_keys;
        else if (entry->weak_handle) ++counts->external_weak_keys;
        else return 0;
    }
    return 1;
}

int bs_dex_vm_weak_hash_map_diagnostic_call(BsDexVM *vm,
                                            BsDexValue map,
                                            const char *method,
                                            const char *signature,
                                            const BsDexValue *arguments,
                                            size_t argument_count,
                                            BsDexValue *result,
                                            const char **exception) {
    if (!vm || !method || !signature || !result || !exception ||
        argument_count > 2 || (argument_count && !arguments)) return 0;
    BsDexValue args[3] = {map, bs_dex_null(), bs_dex_null()};
    for (size_t i = 0; i < argument_count; ++i)
        args[i + 1] = arguments[i];
    *result = bs_dex_null();
    *exception = NULL;
    return invoke_weak_hash_map(vm, method, signature, 0x6e,
                                args, argument_count + 1, result, exception);
}

static int invoke_atomic_reference(BsDexVM *vm, const char *name,
                                   const char *signature, uint8_t kind,
                                   const BsDexValue *args, size_t count,
                                   BsDexValue *result) {
    if (!count || count > 2) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver || !receiver->owns_instance || receiver->backing ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_ATOMIC_REF)) return 0;
    BsDexValue value = bs_dex_null();
    if (count == 2 && !is_dex_null(args[1])) {
        if (!known_ref(vm, args[1])) return 0;
        value = args[1];
    }
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        receiver->atomic_value.kind != BS_DEX_REF &&
        !strcmp(signature, count == 1 ? "()V" : "(Ljava/lang/Object;)V")) {
        receiver->atomic_value = value;
        return 1;
    }
    if (kind != 0x6e || receiver->atomic_value.kind != BS_DEX_REF) return 0;
    if (count == 1 && !strcmp(name, "get") &&
        !strcmp(signature, "()Ljava/lang/Object;")) {
        *result = receiver->atomic_value;
        return 1;
    }
    if (count == 2 && !strcmp(name, "set") &&
        !strcmp(signature, "(Ljava/lang/Object;)V")) {
        receiver->atomic_value = value;
        return 1;
    }
    if (count == 2 && !strcmp(name, "getAndSet") &&
        !strcmp(signature, "(Ljava/lang/Object;)Ljava/lang/Object;")) {
        *result = receiver->atomic_value;
        receiver->atomic_value = value;
        return 1;
    }
    return 0;
}

/* java.util.concurrent.atomic.AtomicInteger stores a signed Java int. The
 * original DEX interpreter executes on one thread: each operation below is
 * an indivisible VM instruction. Java's signed overflow is defined to wrap
 * modulo 2^32, so additions work on uint32_t to avoid signed C overflow.
 * There is no host-backed Java object and no guessed inter-thread state. */
static int invoke_atomic_integer(BsDexVM *vm, const char *name,
                                 const char *signature, uint8_t kind,
                                 const BsDexValue *args, size_t count,
                                 BsDexValue *result) {
    if (!vm || !args || !count || count > 3) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver || !receiver->owns_instance || receiver->backing ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_ATOMIC_INT))
        return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !receiver->atomic_integer_initialized) {
        int32_t initial;
        if (count == 1 && !strcmp(signature, "()V")) initial = 0;
        else if (count == 2 && !strcmp(signature, "(I)V") &&
                 args[1].kind == BS_DEX_INT)
            initial = (int32_t)(uint32_t)args[1].bits;
        else return 0;
        receiver->atomic_integer_value = initial;
        receiver->atomic_integer_initialized = 1;
        return 1;
    }
    if (kind != 0x6e || !receiver->atomic_integer_initialized) return 0;
    const int32_t old = receiver->atomic_integer_value;
    if (count == 1 && !strcmp(signature, "()I")) {
        if (!strcmp(name, "get") || !strcmp(name, "intValue")) {
            *result = bs_dex_int(old);
            return 1;
        }
        if (!strcmp(name, "getAndIncrement") ||
            !strcmp(name, "incrementAndGet") ||
            !strcmp(name, "getAndDecrement") ||
            !strcmp(name, "decrementAndGet")) {
            uint32_t delta = (!strcmp(name, "getAndDecrement") ||
                              !strcmp(name, "decrementAndGet")) ?
                             UINT32_MAX : 1u;
            int32_t next = (int32_t)((uint32_t)old + delta);
            receiver->atomic_integer_value = next;
            *result = bs_dex_int((!strcmp(name, "getAndIncrement") ||
                                  !strcmp(name, "getAndDecrement")) ?
                                 old : next);
            return 1;
        }
    }
    if (count == 2 && args[1].kind == BS_DEX_INT) {
        const int32_t value = (int32_t)(uint32_t)args[1].bits;
        if (!strcmp(signature, "(I)V") &&
            (!strcmp(name, "set") || !strcmp(name, "lazySet"))) {
            receiver->atomic_integer_value = value;
            return 1;
        }
        if (!strcmp(signature, "(I)I")) {
            if (!strcmp(name, "getAndSet")) {
                receiver->atomic_integer_value = value;
                *result = bs_dex_int(old);
                return 1;
            }
            if (!strcmp(name, "addAndGet") ||
                !strcmp(name, "getAndAdd")) {
                int32_t next = (int32_t)((uint32_t)old + (uint32_t)value);
                receiver->atomic_integer_value = next;
                *result = bs_dex_int(!strcmp(name, "getAndAdd") ? old : next);
                return 1;
            }
        }
    }
    if (count == 3 && args[1].kind == BS_DEX_INT &&
        args[2].kind == BS_DEX_INT) {
        const int32_t expected = (int32_t)(uint32_t)args[1].bits;
        const int32_t update = (int32_t)(uint32_t)args[2].bits;
        if (!strcmp(signature, "(II)Z") &&
            (!strcmp(name, "compareAndSet") ||
             !strcmp(name, "weakCompareAndSet"))) {
            const int equal = old == expected;
            if (equal) receiver->atomic_integer_value = update;
            *result = bs_dex_int(equal);
            return 1;
        }
        if (!strcmp(signature, "(II)I") &&
            !strcmp(name, "compareAndExchange")) {
            if (old == expected) receiver->atomic_integer_value = update;
            *result = bs_dex_int(old);
            return 1;
        }
    }
    return 0;
}

/* java.util.Random uses a precisely specified 48-bit linear congruential
 * generator. next(bits) is the ONLY state advance; the public methods
 * below reuse it, including nextInt(bound)'s rejection loop.
 * This is a single-threaded Java interpreter; VM-owned RNG state is never
 * shared with an Android object or seeded from a fabricated timestamp. */
static uint32_t random_next_bits(BsDexRef *random, unsigned bits) {
    random->random_seed = (random->random_seed *
                          BS_DEX_RANDOM_MULTIPLIER +
                          BS_DEX_RANDOM_ADDEND) & BS_DEX_RANDOM_MASK;
    return (uint32_t)(random->random_seed >> (48u - bits));
}

static void random_set_seed(BsDexRef *random, uint64_t java_seed) {
    random->random_seed = (java_seed ^ BS_DEX_RANDOM_MULTIPLIER) &
                          BS_DEX_RANDOM_MASK;
    random->random_initialized = 1;
}

static int invoke_random(BsDexVM *vm, const char *name,
                         const char *signature, uint8_t kind,
                         const BsDexValue *args, size_t count,
                         BsDexValue *result, const char **exception) {
    if (!vm || !name || !signature || !count || count > 3) return 0;
    BsDexRef *random = known_ref(vm, args[0]);
    if (!random || !random->owns_instance || random->backing ||
        strcmp(type_id(vm, random->type_index), BS_DEX_RANDOM)) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !random->random_initialized) {
        if (count == 1 && !strcmp(signature, "()V")) {
            /* OpenJDK java.util.Random(): this(seedUniquifier() ^
             * System.nanoTime()). The uniquifier is shared by all Random
             * instances within this one Java VM. As in Java, multiplicative
             * overflow wraps modulo 2^64. No clock => no constructor. */
            if (!vm->host.monotonic_nanoseconds) return 0;
            uint64_t nanoseconds = 0;
            if (vm->host.monotonic_nanoseconds(vm->host.user,
                                               &nanoseconds) != 1)
                return 0;
            if (vm->has_monotonic_ns &&
                nanoseconds < vm->last_monotonic_ns) return 0;
            uint64_t unique = vm->random_seed_uniquifier *
                              BS_DEX_RANDOM_UNIQUIFIER_MULTIPLIER;
            random_set_seed(random, unique ^ nanoseconds);
            vm->random_seed_uniquifier = unique;
            vm->last_monotonic_ns = nanoseconds;
            vm->has_monotonic_ns = 1;
            return 1;
        }
        if (count == 3 && !strcmp(signature, "(J)V") &&
            args[1].kind == BS_DEX_WIDE) {
            random_set_seed(random, (uint64_t)args[1].bits);
            return 1;
        }
        return 0;
    }
    if (kind != 0x6e || !random->random_initialized) return 0;
    if (count == 3 && !strcmp(name, "setSeed") &&
        !strcmp(signature, "(J)V") && args[1].kind == BS_DEX_WIDE) {
        random_set_seed(random, (uint64_t)args[1].bits);
        return 1;
    }
    if (count == 1 && !strcmp(name, "nextInt") &&
        !strcmp(signature, "()I")) {
        *result = bs_dex_int((int32_t)random_next_bits(random, 32));
        return 1;
    }
    if (count == 2 && !strcmp(name, "nextInt") &&
        !strcmp(signature, "(I)I") && args[1].kind == BS_DEX_INT) {
        int32_t bound = (int32_t)(uint32_t)args[1].bits;
        if (bound <= 0) {
            *exception = "Ljava/lang/IllegalArgumentException;";
            return -1;
        }
        uint32_t r = random_next_bits(random, 31);
        uint32_t mask = (uint32_t)bound - 1u;
        if (((uint32_t)bound & mask) == 0) {
            r = (uint32_t)(((uint64_t)(uint32_t)bound * r) >> 31);
        } else {
            uint32_t u = r;
            r = u % (uint32_t)bound;
            /* Java expression u - (r = u % bound) + m uses signed int
             * overflow. Bitwise 32-bit evaluation retains exact behavior. */
            while ((int32_t)(u - r + mask) < 0) {
                u = random_next_bits(random, 31);
                r = u % (uint32_t)bound;
            }
        }
        *result = bs_dex_int((int32_t)r);
        return 1;
    }
    if (count == 1 && !strcmp(name, "nextBoolean") &&
        !strcmp(signature, "()Z")) {
        *result = bs_dex_int(random_next_bits(random, 1) != 0);
        return 1;
    }
    if (count == 1 && !strcmp(name, "nextLong") &&
        !strcmp(signature, "()J")) {
#if UINTPTR_MAX < UINT64_MAX
        return 0; /* cannot store Java 64-bit value in this VM representation */
#else
        uint32_t high = random_next_bits(random, 32);
        uint32_t low = random_next_bits(random, 32);
        /* Java: ((long)next(32) << 32) + next(32). The second int is
         * sign-extended BEFORE addition (unlike unsigned concatenation). */
        uint64_t bits = ((uint64_t)high << 32) +
                        (uint64_t)(int64_t)(int32_t)low;
        *result = (BsDexValue){(uintptr_t)bits, BS_DEX_WIDE};
        return 1;
#endif
    }
    if (count == 2 && !strcmp(name, "next") &&
        !strcmp(signature, "(I)I") && args[1].kind == BS_DEX_INT) {
        int32_t bits = (int32_t)(uint32_t)args[1].bits;
        if (bits < 1 || bits > 32) return 0;
        *result = bs_dex_int((int32_t)random_next_bits(random, (unsigned)bits));
        return 1;
    }
    return 0;
}

BsDexValue bs_dex_vm_random_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->owns_instance || !r->random_initialized ||
            strcmp(type_id(vm, r->type_index), BS_DEX_RANDOM))
            continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_random_diagnostic_call(BsDexVM *vm, BsDexValue random_ref,
                                     const char *method,
                                     const char *signature,
                                     const BsDexValue *arguments,
                                     size_t argument_register_count,
                                     BsDexValue *result,
                                     const char **exception) {
    if (!vm || !method || !signature || !result || !exception ||
        argument_register_count > 2 ||
        (argument_register_count && !arguments)) return 0;
    BsDexValue regs[3] = {random_ref, bs_dex_int(0), bs_dex_int(0)};
    for (size_t i = 0; i < argument_register_count; ++i)
        regs[i + 1] = arguments[i];
    *result = bs_dex_int(0);
    *exception = NULL;
    return invoke_random(vm, method, signature, 0x6e, regs,
                         argument_register_count + 1, result, exception);
}

/* An empty android.os.Bundle is a key/value container, NOT a platform
 * Context, Activity or Parcel. Only these concrete primitive/String/nested
 * Bundle values can be represented without Android class loaders,
 * unparceling, or JNI. A key must be an observed immutable Java String
 * (or Java null), so equality is actual String content rather than a
 * guessed Java-object pointer identity. The VM never reads external Bundle
 * contents or assumes ApplicationInfo.metaData is an empty instance. */
static int bundle_key_text(BsDexVM *vm, BsDexValue key,
                           const char **characters) {
    if (is_dex_null(key)) {
        *characters = NULL;
        return 1;
    }
    if (!known_ref(vm, key) ||
        strcmp(bs_dex_reference_type(vm, key), "Ljava/lang/String;"))
        return 0;
    *characters = bs_dex_string(vm, key);
    return *characters != NULL;
}

static int bundle_find(BsDexVM *vm, BsDexRef *bundle,
                       const char *key_text) {
    for (uint32_t i = 0; i < bundle->bundle_count; ++i) {
        const char *present = NULL;
        if (!bundle_key_text(vm, bundle->bundle_entries[i].key, &present))
            return -2; /* cannot establish real Java String equality */
        if ((!key_text && !present) ||
            (key_text && present && !strcmp(key_text, present)))
            return (int)i;
    }
    return -1;
}

static int bundle_put(BsDexRef *bundle, int index,
                      BsDexValue key, BsDexValue value, char type) {
    if (index >= 0) {
        bundle->bundle_entries[index].value = value;
        bundle->bundle_entries[index].type = type;
        return 1;
    }
    if (bundle->bundle_count == BS_DEX_MAX_BUNDLE_ENTRIES) return 0;
    if (bundle->bundle_count == bundle->bundle_capacity) {
        uint32_t capacity = bundle->bundle_capacity ?
                            bundle->bundle_capacity * 2 : 8u;
        if (capacity > BS_DEX_MAX_BUNDLE_ENTRIES)
            capacity = BS_DEX_MAX_BUNDLE_ENTRIES;
        BsDexBundleEntry *entries = realloc(bundle->bundle_entries,
                                            capacity * sizeof *entries);
        if (!entries) return 0;
        bundle->bundle_entries = entries;
        bundle->bundle_capacity = capacity;
    }
    bundle->bundle_entries[bundle->bundle_count++] =
        (BsDexBundleEntry){key, value, type};
    return 1;
}

static int invoke_bundle(BsDexVM *vm, const char *name,
                         const char *signature, uint8_t kind,
                         const BsDexValue *args, size_t count,
                         BsDexValue *result) {
    if (!vm || !name || !signature || !count || count > 4) return 0;
    BsDexRef *bundle = known_ref(vm, args[0]);
    if (!bundle || !bundle->owns_instance || bundle->backing ||
        strcmp(type_id(vm, bundle->type_index), BS_DEX_BUNDLE)) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !bundle->bundle_initialized &&
        count == 1 && !strcmp(signature, "()V")) {
        bundle->bundle_initialized = 1;
        return 1;
    }
    if ((kind != 0x6e && kind != 0x72) ||
        !bundle->bundle_initialized) return 0;
    if (count == 1) {
        if (!strcmp(name, "size") && !strcmp(signature, "()I")) {
            *result = bs_dex_int((int32_t)bundle->bundle_count);
            return 1;
        }
        if (!strcmp(name, "isEmpty") && !strcmp(signature, "()Z")) {
            *result = bs_dex_int(bundle->bundle_count == 0);
            return 1;
        }
        if (!strcmp(name, "clear") && !strcmp(signature, "()V")) {
            bundle->bundle_count = 0;
            return 1;
        }
        return 0;
    }
    const char *key_text = NULL;
    if (!bundle_key_text(vm, args[1], &key_text)) return 0;
    int index = bundle_find(vm, bundle, key_text);
    if (index == -2) return 0;
    if (count == 2) {
        if (!strcmp(name, "containsKey") &&
            !strcmp(signature, "(Ljava/lang/String;)Z")) {
            *result = bs_dex_int(index >= 0);
            return 1;
        }
        if (!strcmp(name, "remove") &&
            !strcmp(signature, "(Ljava/lang/String;)V")) {
            if (index >= 0) {
                size_t trailing = bundle->bundle_count - (uint32_t)index - 1;
                memmove(bundle->bundle_entries + index,
                        bundle->bundle_entries + index + 1,
                        trailing * sizeof *bundle->bundle_entries);
                --bundle->bundle_count;
            }
            return 1;
        }
    }
    if (count == 3 || count == 4) {
        char put_type = 0;
        if (count == 3 && !strcmp(name, "putInt") &&
            !strcmp(signature, "(Ljava/lang/String;I)V") &&
            args[2].kind == BS_DEX_INT) put_type = 'I';
        else if (count == 3 && !strcmp(name, "putBoolean") &&
                 !strcmp(signature, "(Ljava/lang/String;Z)V") &&
                 args[2].kind == BS_DEX_INT) put_type = 'Z';
        else if (count == 4 && !strcmp(name, "putLong") &&
                 !strcmp(signature, "(Ljava/lang/String;J)V") &&
                 args[2].kind == BS_DEX_WIDE) put_type = 'J';
        else if (count == 3 && !strcmp(name, "putString") &&
                 !strcmp(signature,
                         "(Ljava/lang/String;Ljava/lang/String;)V") &&
                 (is_dex_null(args[2]) ||
                  (known_ref(vm, args[2]) &&
                   !strcmp(bs_dex_reference_type(vm, args[2]),
                           "Ljava/lang/String;")))) put_type = 'S';
        else if (count == 3 && !strcmp(name, "putBundle") &&
                 !strcmp(signature,
                         "(Ljava/lang/String;Landroid/os/Bundle;)V") &&
                 (is_dex_null(args[2]) ||
                  (known_ref(vm, args[2]) &&
                   !strcmp(bs_dex_reference_type(vm, args[2]),
                           BS_DEX_BUNDLE) &&
                   known_ref(vm, args[2])->bundle_initialized)))
            put_type = 'B';
        if (put_type) {
            BsDexValue value = args[2];
            if (put_type == 'Z') value = bs_dex_int(args[2].bits != 0);
            if ((put_type == 'S' || put_type == 'B') && is_dex_null(value))
                value = bs_dex_null();
            return bundle_put(bundle, index, args[1], value, put_type);
        }
    }
    if (count == 2 || count == 3 || count == 4) {
        char get_type = 0;
        if (!strcmp(name, "getInt") &&
            ((!strcmp(signature, "(Ljava/lang/String;)I") && count == 2) ||
             (!strcmp(signature, "(Ljava/lang/String;I)I") && count == 3 &&
              args[2].kind == BS_DEX_INT))) get_type = 'I';
        else if (!strcmp(name, "getBoolean") &&
            ((!strcmp(signature, "(Ljava/lang/String;)Z") && count == 2) ||
             (!strcmp(signature, "(Ljava/lang/String;Z)Z") && count == 3 &&
              args[2].kind == BS_DEX_INT))) get_type = 'Z';
        else if (!strcmp(name, "getLong") &&
            ((!strcmp(signature, "(Ljava/lang/String;)J") && count == 2) ||
             (!strcmp(signature, "(Ljava/lang/String;J)J") && count == 4 &&
              args[2].kind == BS_DEX_WIDE))) get_type = 'J';
        else if (!strcmp(name, "getString") &&
            ((!strcmp(signature, "(Ljava/lang/String;)Ljava/lang/String;") &&
              count == 2) ||
             (!strcmp(signature,
                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") &&
              count == 3 &&
              (is_dex_null(args[2]) ||
               (known_ref(vm, args[2]) &&
                !strcmp(bs_dex_reference_type(vm, args[2]),
                        "Ljava/lang/String;")))))) get_type = 'S';
        else if (!strcmp(name, "getBundle") &&
                 !strcmp(signature,
                         "(Ljava/lang/String;)Landroid/os/Bundle;") &&
                 count == 2) get_type = 'B';
        if (get_type) {
            if (index >= 0) {
                BsDexBundleEntry *entry = &bundle->bundle_entries[index];
                /* Type mismatch requires Android BaseBundle cast behavior,
                 * logging and potential ClassLoader; never guess here. */
                if (entry->type != get_type) return 0;
                *result = entry->value;
                return 1;
            }
            if (get_type == 'S' || get_type == 'B')
                *result = count == 3 ? args[2] : bs_dex_null();
            else if (get_type == 'J')
                *result = count == 4 ? args[2] :
                          (BsDexValue){0, BS_DEX_WIDE};
            else *result = count == 3 ? args[2] : bs_dex_int(0);
            return 1;
        }
    }
    return 0;
}

BsDexValue bs_dex_vm_bundle_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *bundle = vm->references[i];
        if (!bundle->owns_instance || !bundle->bundle_initialized ||
            strcmp(type_id(vm, bundle->type_index), BS_DEX_BUNDLE))
            continue;
        if (!ordinal--) return ref_value(bundle);
    }
    return bs_dex_null();
}

int bs_dex_vm_bundle_diagnostic_call(BsDexVM *vm, BsDexValue bundle,
                                     const char *method,
                                     const char *signature,
                                     const BsDexValue *arguments,
                                     size_t argument_register_count,
                                     BsDexValue *result) {
    if (!vm || !method || !signature || !result ||
        argument_register_count > 3 ||
        (argument_register_count && !arguments)) return 0;
    BsDexValue regs[4] = {bundle, bs_dex_null(),
                          bs_dex_int(0), bs_dex_int(0)};
    for (size_t i = 0; i < argument_register_count; ++i)
        regs[i + 1] = arguments[i];
    *result = bs_dex_null();
    return invoke_bundle(vm, method, signature, 0x6e,
                         regs, argument_register_count + 1, result);
}

/* The original APK declares DefaultTaskExecutor$1 implements ThreadFactory.
 * Prove such a relationship from class_def.interfaces_off. A class name or
 * Java reference by itself is not proof of a working ThreadFactory. */
static int dex_class_implements(BsDexVM *vm, uint32_t type,
                                const char *interface_name) {
    if (!vm || !interface_name) return 0;
    uint32_t desired;
    if (!descriptor_type(vm, interface_name, &desired)) return 0;
    for (unsigned depth = 0; depth < 48 && type < vm->type_count; ++depth) {
        uint32_t def = vm->class_definition[type];
        if (!def) return 0;
        uint32_t interfaces = u32(vm->dex + def + 12u);
        if (interfaces) {
            if (!bounds(vm, interfaces, 4)) return 0;
            uint32_t count = u32(vm->dex + interfaces);
            if (count > vm->type_count ||
                !bounds(vm, (size_t)interfaces + 4u, (size_t)count * 2u))
                return 0;
            for (uint32_t i = 0; i < count; ++i)
                if (u16(vm->dex + interfaces + 4u + 2u * i) == desired)
                    return 1;
        }
        uint32_t parent = u32(vm->dex + def + 8u);
        if (parent == type || parent == UINT32_MAX) return 0;
        type = parent;
    }
    return 0;
}

/* newFixedThreadPool(int, ThreadFactory) constructs a ThreadPoolExecutor
 * (core=max=n, keepAlive=0 MILLISECONDS, LinkedBlockingQueue() with Java
 * capacity Integer.MAX_VALUE, supplied factory, AbortPolicy). A fresh pool
 * has zero workers, queued tasks, and completed tasks. Worker threads start
 * LAZILY on execute/submit; those operations require a real scheduler and
 * Runnable dispatcher, which this single-thread DEX VM cannot provide.
 *
 * Host memory bounds do not change Java queue semantics: the queue's Java
 * advertised capacity is Integer.MAX_VALUE, while future host integration
 * must obey a separate 8192-entry VM budget or fail without committing. */
static int invoke_executor(BsDexVM *vm, const char *klass,
                           const char *name, const char *signature,
                           uint8_t kind, const BsDexValue *args,
                           size_t count, BsDexValue *result,
                           const char **exception) {
    if (!vm || !klass || !name || !signature || !result) return 0;
    if (kind == 0x71 && !strcmp(klass, BS_DEX_EXECUTORS) &&
        !strcmp(name, "newFixedThreadPool") &&
        !strcmp(signature, "(ILjava/util/concurrent/ThreadFactory;)"
                           "Ljava/util/concurrent/ExecutorService;") &&
        count == 2 && args[0].kind == BS_DEX_INT) {
        int32_t count_threads = (int32_t)(uint32_t)args[0].bits;
        if (count_threads <= 0) {
            *exception = "Ljava/lang/IllegalArgumentException;";
            return -1;
        }
        if (is_dex_null(args[1])) {
            *exception = "Ljava/lang/NullPointerException;";
            return -1;
        }
        if ((uint32_t)count_threads > BS_DEX_MAX_EXECUTOR_WORKERS) return -3;
        BsDexRef *factory = known_ref(vm, args[1]);
        if (!factory || !factory->owns_instance || factory->backing ||
            !vm->class_definition[factory->type_index] ||
            !dex_class_implements(vm, factory->type_index,
                                  BS_DEX_THREAD_FACTORY))
            return 0;
        uint32_t pool_type, queue_type;
        if (!descriptor_type(vm, BS_DEX_THREAD_POOL_EXECUTOR, &pool_type) ||
            !descriptor_type(vm, BS_DEX_LINKED_BLOCKING_QUEUE, &queue_type) ||
            vm->reference_count > BS_DEX_MAX_OBJECTS - 2u) return -3;
        BsDexRef *queue = allocate_ref(vm, queue_type, NULL, 1, NULL);
        if (!queue) return -3;
        queue->executor_queue_initialized = 1;
        queue->executor_queue_java_capacity = INT32_MAX;
        queue->executor_queue_vm_limit = BS_DEX_MAX_EXECUTOR_TASKS;
        BsDexRef *pool = allocate_ref(vm, pool_type, NULL, 1, NULL);
        if (!pool) return -3;
        pool->executor_factory = args[1];
        pool->executor_queue = ref_value(queue);
        pool->executor_core_size = (uint32_t)count_threads;
        pool->executor_max_size = (uint32_t)count_threads;
        pool->executor_initialized = 1;
        *result = ref_value(pool);
        return 1;
    }
    if ((kind != 0x6e && kind != 0x72) || !count) return 0;
    BsDexRef *pool = known_ref(vm, args[0]);
    if (pool && pool->executor_queue_initialized &&
        !strcmp(type_id(vm, pool->type_index),
                BS_DEX_LINKED_BLOCKING_QUEUE)) {
        /* An untouched LinkedBlockingQueue is empty without requiring any
         * Java worker. Read-only inspection and an empty poll are well
         * defined; attempts to insert tasks or block on them need a real
         * queue/thread dispatcher. */
        if (count == 1 && !strcmp(name, "size") &&
            !strcmp(signature, "()I")) {
            *result = bs_dex_int(0);
            return 1;
        }
        if (count == 1 && !strcmp(name, "isEmpty") &&
            !strcmp(signature, "()Z")) {
            *result = bs_dex_int(1);
            return 1;
        }
        if (count == 1 && !strcmp(name, "remainingCapacity") &&
            !strcmp(signature, "()I")) {
            *result = bs_dex_int((int32_t)pool->executor_queue_java_capacity);
            return 1;
        }
        if (count == 1 && (!strcmp(name, "peek") ||
                           !strcmp(name, "poll")) &&
            !strcmp(signature, "()Ljava/lang/Object;")) {
            *result = bs_dex_null();
            return 1;
        }
        if (count == 1 && !strcmp(name, "clear") &&
            !strcmp(signature, "()V")) return 1;
        return -4;
    }
    if (!pool || !pool->owns_instance || pool->backing ||
        !pool->executor_initialized ||
        strcmp(type_id(vm, pool->type_index),
               BS_DEX_THREAD_POOL_EXECUTOR)) return 0;
    if (count == 1 && !strcmp(signature, "()V") &&
        !strcmp(name, "shutdown")) {
        pool->executor_shutdown = 1;
        /* No tasks were submitted and no worker was ever created. */
        if (!pool->executor_live_workers && !pool->executor_queued_tasks)
            pool->executor_terminated = 1;
        return 1;
    }
    if (count == 1 && !strcmp(signature, "()Z")) {
        if (!strcmp(name, "isShutdown")) {
            *result = bs_dex_int(pool->executor_shutdown);
            return 1;
        }
        if (!strcmp(name, "isTerminated")) {
            *result = bs_dex_int(pool->executor_terminated);
            return 1;
        }
        if (!strcmp(name, "isTerminating")) {
            *result = bs_dex_int(pool->executor_shutdown &&
                                 !pool->executor_terminated);
            return 1;
        }
    }
    if (count == 1 && !strcmp(signature, "()I")) {
        if (!strcmp(name, "getCorePoolSize") ||
            !strcmp(name, "getMaximumPoolSize")) {
            *result = bs_dex_int((int32_t)(!strcmp(name, "getCorePoolSize") ?
                                          pool->executor_core_size :
                                          pool->executor_max_size));
            return 1;
        }
        if (!strcmp(name, "getPoolSize") ||
            !strcmp(name, "getActiveCount") ||
            !strcmp(name, "getLargestPoolSize")) {
            *result = bs_dex_int((int32_t)pool->executor_live_workers);
            return 1;
        }
    }
    if (count == 1 && !strcmp(signature, "()J") &&
        (!strcmp(name, "getTaskCount") ||
         !strcmp(name, "getCompletedTaskCount"))) {
        *result = (BsDexValue){(uintptr_t)pool->executor_completed_tasks,
                               BS_DEX_WIDE};
        return 1;
    }
    if (count == 1 && !strcmp(name, "getQueue") &&
        !strcmp(signature, "()Ljava/util/concurrent/BlockingQueue;")) {
        *result = pool->executor_queue;
        return 1;
    }
    if (count == 1 && !strcmp(name, "getThreadFactory") &&
        !strcmp(signature, "()Ljava/util/concurrent/ThreadFactory;")) {
        *result = pool->executor_factory;
        return 1;
    }
    if ((!strcmp(name, "execute") || !strcmp(name, "submit")) &&
        count >= 2 && is_dex_null(args[1])) {
        *exception = "Ljava/lang/NullPointerException;";
        return -1;
    }
    if ((!strcmp(name, "execute") || !strcmp(name, "submit")) &&
        pool->executor_shutdown) {
        *exception = "Ljava/util/concurrent/RejectedExecutionException;";
        return -1; /* JDK AbortPolicy rejects work after shutdown. */
    }
    /* A Runnable/Callable cannot be submitted until real workers can be
     * scheduled and DEX Runnable.run() can be executed on the right thread.
     * Never enqueue a task that the host is unable to run to completion. */
    if (!strcmp(name, "execute") || !strcmp(name, "submit") ||
        !strcmp(name, "invokeAll") || !strcmp(name, "invokeAny") ||
        !strcmp(name, "shutdownNow") || !strcmp(name, "awaitTermination"))
        return -4;
    return 0;
}

BsDexValue bs_dex_vm_executor_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->executor_initialized ||
            strcmp(type_id(vm, r->type_index),
                   BS_DEX_THREAD_POOL_EXECUTOR)) continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_executor_snapshot(BsDexVM *vm, BsDexValue executor,
                                BsDexExecutorSnapshot *out) {
    BsDexRef *pool = known_ref(vm, executor);
    if (!pool || !out || !pool->owns_instance || pool->backing ||
        !pool->executor_initialized ||
        strcmp(type_id(vm, pool->type_index),
               BS_DEX_THREAD_POOL_EXECUTOR)) return 0;
    BsDexRef *queue = known_ref(vm, pool->executor_queue);
    if (!queue || !queue->executor_queue_initialized ||
        strcmp(type_id(vm, queue->type_index),
               BS_DEX_LINKED_BLOCKING_QUEUE)) return 0;
    *out = (BsDexExecutorSnapshot){
        pool->executor_core_size, pool->executor_max_size,
        pool->executor_live_workers, pool->executor_queued_tasks,
        pool->executor_completed_tasks,
        queue->executor_queue_java_capacity, queue->executor_queue_vm_limit,
        pool->executor_factory, pool->executor_queue,
        pool->executor_shutdown, pool->executor_terminated
    };
    return 1;
}

int bs_dex_vm_executor_diagnostic_call(BsDexVM *vm, BsDexValue executor,
                                       const char *method,
                                       const char *signature,
                                       const BsDexValue *argument,
                                       BsDexValue *result,
                                       const char **exception) {
    if (!vm || !method || !signature || !result || !exception) return 0;
    BsDexValue args[2] = {executor, argument ? *argument : bs_dex_null()};
    *result = bs_dex_null();
    *exception = NULL;
    return invoke_executor(vm, BS_DEX_THREAD_POOL_EXECUTOR, method,
                           signature, 0x6e, args, argument ? 2 : 1,
                           result, exception);
}

int bs_dex_vm_executor_diagnostic_create(BsDexVM *vm, int32_t threads,
                                          BsDexValue thread_factory,
                                          BsDexValue *executor,
                                          const char **exception) {
    if (!vm || !executor || !exception) return 0;
    BsDexValue args[2] = {bs_dex_int(threads), thread_factory};
    *executor = bs_dex_null();
    *exception = NULL;
    return invoke_executor(vm, BS_DEX_EXECUTORS, "newFixedThreadPool",
                 "(ILjava/util/concurrent/ThreadFactory;)"
                 "Ljava/util/concurrent/ExecutorService;",
                 0x71, args, 2, executor, exception);
}

/* A limited Java Looper/Thread identity view, for original DEX code which
 * compares Looper.getMainLooper().getThread() to Thread.currentThread().
 * The host must actually observe a live main loop, its owner, and current
 * native thread. These VM-only tokens do NOT implement Android Looper or
 * Java Thread services. They cannot be used to dispatch a Runnable. */
static int observed_main_loop(BsDexVM *vm, uintptr_t *queue,
                              uint64_t *owner, uint64_t *current) {
    if (!vm || !queue || !owner || !current ||
        !vm->host.observe_main_looper) return 0;
    *queue = 0;
    *owner = 0;
    *current = 0;
    int rc = vm->host.observe_main_looper(vm->host.user, queue,
                                         owner, current);
    if (rc != 1) return 0;
    if (!*queue || !*owner || !*current) return -2;
    /* Android's process main Looper is a singleton for its lifetime.
     * If the host reports a replacement queue or owner, stale Java tokens
     * cannot safely be treated as the same Looper or Thread. */
    if (vm->observed_main_queue_identity &&
        (vm->observed_main_queue_identity != *queue ||
         vm->observed_main_owner_thread_id != *owner))
        return 0;
    return 1;
}

/* Handler() uses Looper.myLooper() on the CONSTRUCTING thread. Only the
 * independently observed native main loop on its actual owning OS thread
 * is in scope for this Quest bootstrap. A valid observer on a different
 * thread cannot authorize a default Handler constructor for that main
 * queue. Missing observers, mismatched owners and replaced queues fail
 * without producing a Java Handler or pretending Runnable scheduling. */
static int observed_handler_main_thread(BsDexVM *vm, uintptr_t *queue,
                                         uint64_t *owner) {
    uint64_t current = 0;
    int rc = observed_main_loop(vm, queue, owner, &current);
    if (rc != 1) return rc;
    if (current != *owner) return 0;
    return 1;
}

static int handler_constructor_gate(BsDexVM *vm, BsDexRef *receiver) {
    if (!vm || !receiver || !receiver->handler_allocated ||
        receiver->handler_initialized || !receiver->backing ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_HANDLER))
        return 0;
    uintptr_t queue = 0;
    uint64_t owner = 0;
    int rc = observed_handler_main_thread(vm, &queue, &owner);
    if (rc != 1) return rc;
    return queue == receiver->handler_main_queue_identity &&
           owner == receiver->handler_owner_thread_id;
}

/* Intentionally ALLOCATION + DEFAULT CONSTRUCTION ONLY. A platform host
 * callback claiming success cannot make Handler.post/removeCallbacks,
 * getLooper, sendMessage or any other Java Handler API executable without
 * original guest Runnable scheduling and an independently canonical Java
 * Looper object. The same gate is exposed read-only to the host fixture. */
static int handler_method_supported(BsDexVM *vm, BsDexRef *receiver,
                                     const char *klass, const char *name,
                                     const char *signature, uint8_t kind,
                                     size_t count) {
    if (!vm || !receiver || !klass || !name || !signature ||
        strcmp(klass, BS_DEX_HANDLER) ||
        strcmp(name, "<init>") || strcmp(signature, "()V") ||
        kind != 0x70 || count != 1) return 0;
    return handler_constructor_gate(vm, receiver);
}

int bs_dex_vm_handler_snapshot(BsDexVM *vm, BsDexValue handler,
                                BsDexHandlerSnapshot *out) {
    BsDexRef *receiver = known_ref(vm, handler);
    if (!receiver || !out || !receiver->handler_allocated ||
        !receiver->backing ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_HANDLER))
        return 0;
    *out = (BsDexHandlerSnapshot){
        receiver->handler_main_queue_identity,
        receiver->handler_owner_thread_id,
        receiver->handler_allocated,
        receiver->handler_initialized
    };
    return 1;
}

int bs_dex_vm_handler_diagnostic_method_gate(
    BsDexVM *vm, BsDexValue handler, const char *klass,
    const char *method, const char *signature,
    uint8_t kind, size_t register_count) {
    BsDexRef *receiver = known_ref(vm, handler);
    return handler_method_supported(vm, receiver, klass, method,
                                    signature, kind, register_count) == 1;
}

static BsDexValue observed_thread_ref(BsDexVM *vm, uint64_t thread_id) {
    if (!vm || !thread_id) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (r->is_observed_thread && r->observed_thread_id == thread_id)
            return ref_value(r);
    }
    uint32_t thread_type;
    if (!descriptor_type(vm, BS_DEX_THREAD, &thread_type))
        return bs_dex_null();
    BsDexRef *ref = allocate_ref(vm, thread_type, NULL, 0, NULL);
    if (!ref) return bs_dex_null();
    ref->is_observed_thread = 1;
    ref->observed_thread_id = thread_id;
    return ref_value(ref);
}

static int invoke_observed_looper_thread(BsDexVM *vm,
                                         const char *klass,
                                         const char *method,
                                         const char *signature,
                                         uint8_t kind,
                                         const BsDexValue *args,
                                         size_t count,
                                         BsDexValue *result) {
    if (!vm || !klass || !method || !signature || !result) return 0;
    const int get_main = !strcmp(klass, BS_DEX_LOOPER) &&
                         !strcmp(method, "getMainLooper") &&
                         !strcmp(signature, "()Landroid/os/Looper;") &&
                         kind == 0x71 && count == 0;
    const int get_thread = !strcmp(klass, BS_DEX_LOOPER) &&
                           !strcmp(method, "getThread") &&
                           !strcmp(signature, "()Ljava/lang/Thread;") &&
                           kind == 0x6e && count == 1;
    const int current_thread = !strcmp(klass, BS_DEX_THREAD) &&
                               !strcmp(method, "currentThread") &&
                               !strcmp(signature, "()Ljava/lang/Thread;") &&
                               kind == 0x71 && count == 0;
    if (!get_main && !get_thread && !current_thread) return 0;
    BsDexRef *receiver = NULL;
    if (get_thread) {
        receiver = known_ref(vm, args[0]);
        if (!receiver || !receiver->is_observed_main_looper ||
            strcmp(type_id(vm, receiver->type_index), BS_DEX_LOOPER))
            return 0;
    }
    uintptr_t queue;
    uint64_t owner, current;
    int rc = observed_main_loop(vm, &queue, &owner, &current);
    if (rc != 1) return rc;
    if (get_main) {
        if (!vm->observed_main_looper.bits) {
            uint32_t looper_type;
            if (!descriptor_type(vm, BS_DEX_LOOPER, &looper_type))
                return -3;
            BsDexRef *looper = allocate_ref(vm, looper_type, NULL, 0, NULL);
            if (!looper) return -3;
            looper->is_observed_main_looper = 1;
            looper->observed_looper_identity = queue;
            looper->observed_thread_id = owner;
            vm->observed_main_looper = ref_value(looper);
            vm->observed_main_queue_identity = queue;
            vm->observed_main_owner_thread_id = owner;
        }
        *result = vm->observed_main_looper;
        return 1;
    }
    if (get_thread &&
        (receiver->observed_looper_identity != queue ||
         receiver->observed_thread_id != owner)) return 0;
    BsDexValue thread = observed_thread_ref(vm, get_thread ? owner : current);
    if (!thread.bits) return -3;
    *result = thread;
    return 1;
}

BsDexValue bs_dex_vm_observed_main_looper(BsDexVM *vm) {
    return vm ? vm->observed_main_looper : bs_dex_null();
}

int bs_dex_vm_observed_thread_id(BsDexVM *vm, BsDexValue java_thread,
                                uint64_t *thread_id) {
    BsDexRef *ref = known_ref(vm, java_thread);
    if (!ref || !thread_id || !ref->is_observed_thread ||
        strcmp(type_id(vm, ref->type_index), BS_DEX_THREAD)) return 0;
    *thread_id = ref->observed_thread_id;
    return 1;
}

/* Boolean.valueOf(boolean) returns the canonical Java Boolean.TRUE/FALSE
 * singleton. These are pure VM-owned Java value objects, never Android
 * state. Exact boxing allows original DEX to store actual Boolean values
 * without claiming any missing Java runtime or platform service. */
static BsDexValue canonical_boolean(BsDexVM *vm, int boolean_value) {
    if (!vm) return bs_dex_null();
    BsDexValue *slot = boolean_value ? &vm->cached_boolean_true :
                                       &vm->cached_boolean_false;
    if (slot->bits) return *slot;
    uint32_t type;
    if (!descriptor_type(vm, BS_DEX_BOOLEAN, &type)) return bs_dex_null();
    BsDexRef *ref = allocate_ref(vm, type, NULL, 1, NULL);
    if (!ref) return bs_dex_null();
    ref->boolean_initialized = 1;
    ref->boolean_value = !!boolean_value;
    *slot = ref_value(ref);
    return *slot;
}

static int invoke_boolean(BsDexVM *vm, const char *name,
                          const char *signature, uint8_t kind,
                          const BsDexValue *args, size_t count,
                          BsDexValue *result) {
    if (!vm || !name || !signature || !result) return 0;
    if (kind == 0x71 && count == 1 &&
        !strcmp(name, "valueOf") &&
        !strcmp(signature, "(Z)Ljava/lang/Boolean;") &&
        args[0].kind == BS_DEX_INT) {
        *result = canonical_boolean(vm, args[0].bits != 0);
        return result->bits ? 1 : -3;
    }
    if (kind != 0x6e || !count) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver || !receiver->boolean_initialized ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_BOOLEAN))
        return 0;
    if (count == 1) {
        if (!strcmp(name, "booleanValue") &&
            !strcmp(signature, "()Z")) {
            *result = bs_dex_int(receiver->boolean_value);
            return 1;
        }
        if (!strcmp(name, "hashCode") &&
            !strcmp(signature, "()I")) {
            *result = bs_dex_int(receiver->boolean_value ? 1231 : 1237);
            return 1;
        }
    }
    if (count == 2 && !strcmp(name, "equals") &&
        !strcmp(signature, "(Ljava/lang/Object;)Z")) {
        BsDexRef *other = known_ref(vm, args[1]);
        *result = bs_dex_int(other && other->boolean_initialized &&
                             receiver->boolean_value == other->boolean_value);
        return 1;
    }
    return 0;
}

BsDexValue bs_dex_vm_cached_boolean(BsDexVM *vm, int value) {
    if (!vm) return bs_dex_null();
    return value ? vm->cached_boolean_true : vm->cached_boolean_false;
}

BsDexValue bs_dex_vm_atomic_integer_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *ref = vm->references[i];
        if (!ref->owns_instance || !ref->atomic_integer_initialized ||
            strcmp(type_id(vm, ref->type_index), BS_DEX_ATOMIC_INT))
            continue;
        if (!ordinal--) return ref_value(ref);
    }
    return bs_dex_null();
}

int bs_dex_vm_atomic_integer_diagnostic_call(BsDexVM *vm,
                                               BsDexValue integer,
                                               const char *method,
                                               const char *signature,
                                               const BsDexValue *arguments,
                                               size_t argument_count,
                                               BsDexValue *result) {
    if (!vm || !method || !signature || !result ||
        argument_count > 2 || (argument_count && !arguments)) return 0;
    BsDexValue args[3] = {integer, bs_dex_int(0), bs_dex_int(0)};
    for (size_t i = 0; i < argument_count; ++i)
        args[i + 1] = arguments[i];
    *result = bs_dex_int(0);
    return invoke_atomic_integer(vm, method, signature, 0x6e,
                                 args, argument_count + 1, result);
}

/* java.lang.Enum is Android/Java library code with no original DEX body.
 * Its base constructor only assigns immutable name and ordinal fields;
 * it runs for an actual VM-allocated enum subclass with arguments supplied
 * by the observed original invocations, never inventing enum constants. */
static int invoke_enum(BsDexVM *vm, const char *name,
                       const char *signature, uint8_t kind,
                       const BsDexValue *args, size_t count,
                       BsDexValue *result, const char **exception) {
    if (!count) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!vm_enum_subclass(vm, receiver)) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !strcmp(signature, "(Ljava/lang/String;I)V") && count == 3 &&
        !receiver->enum_initialized && args[1].kind == BS_DEX_REF &&
        args[1].bits && args[2].kind == BS_DEX_INT &&
        known_ref(vm, args[1]) &&
        !strcmp(bs_dex_reference_type(vm, args[1]), "Ljava/lang/String;")) {
        receiver->enum_name = args[1];
        receiver->enum_ordinal = (int32_t)args[2].bits;
        receiver->enum_initialized = 1;
        return 1;
    }
    if (kind != 0x6e || !receiver->enum_initialized) return 0;
    if (count == 2 && !strcmp(name, "compareTo") &&
        !strcmp(signature, "(Ljava/lang/Enum;)I")) {
        /* Enum.compareTo is final and compares ordinals ONLY within the
         * exact same enum class. The original Lifecycle.State constants
         * are DEX-initialized with concrete type and ordinal, never guessed.
         * Use unsigned subtraction to preserve Java int32 wrap without C
         * signed-overflow UB, even if a malformed DEX supplies extremes. */
        if (is_dex_null(args[1])) {
            *exception = "Ljava/lang/NullPointerException;";
            return -1;
        }
        BsDexRef *other = known_ref(vm, args[1]);
        if (!other || !vm_enum_subclass(vm, other) ||
            !other->enum_initialized ||
            other->type_index != receiver->type_index) {
            *exception = "Ljava/lang/ClassCastException;";
            return -1;
        }
        int32_t difference = (int32_t)((uint32_t)receiver->enum_ordinal -
                                       (uint32_t)other->enum_ordinal);
        *result = bs_dex_int(difference);
        return 1;
    }
    if (count != 1) return 0;
    if (!strcmp(name, "name") &&
        !strcmp(signature, "()Ljava/lang/String;")) {
        *result = receiver->enum_name;
        return 1;
    }
    if (!strcmp(name, "ordinal") && !strcmp(signature, "()I")) {
        *result = bs_dex_int(receiver->enum_ordinal);
        return 1;
    }
    if (!strcmp(name, "toString") &&
        !strcmp(signature, "()Ljava/lang/String;")) {
        *result = receiver->enum_name;
        return 1;
    }
    return 0;
}

/* Return 1 only when Java equals() can be evaluated without executing an
 * unavailable library method. Strings require fully known UTF-8 contents;
 * Enum.equals is final identity and Reference inherits Object.equals.
 * All other query types are left to their real Java implementation. */
static int supported_set_equals(BsDexVM *vm, BsDexValue query,
                                 BsDexValue existing, int *equal) {
    *equal = 0;
    if (is_dex_null(query)) {
        *equal = is_dex_null(existing);
        return 1;
    }
    BsDexRef *q = known_ref(vm, query);
    if (!q) return 0;
    const char *qtype = type_id(vm, q->type_index);
    if (!strcmp(qtype, "Ljava/lang/String;")) {
        if (is_dex_null(existing)) return 1;
        BsDexRef *other = known_ref(vm, existing);
        if (!other) return 0;
        if (strcmp(type_id(vm, other->type_index), "Ljava/lang/String;"))
            return 1;
        const char *left = q->utf8;
        const char *right = other->utf8;
        if (!left || !right ||
            !valid_observed_utf8((const unsigned char *)left, strlen(left)) ||
            !valid_observed_utf8((const unsigned char *)right, strlen(right)))
            return 0;
        *equal = !strcmp(left, right);
        return 1;
    }
    if ((!strcmp(qtype, BS_DEX_WEAK_REFERENCE) ||
         !strcmp(qtype, BS_DEX_ATOMIC_REF) ||
         vm_enum_subclass(vm, q)) && q->owns_instance && !q->backing) {
        *equal = query.bits == existing.bits;
        return 1;
    }
    /* For an original DEX class, prove it inherits Object.equals unchanged.
     * Stop on any declared override or external superclass. */
    if (!q->owns_instance || q->backing) return 0;
    uint32_t type = q->type_index;
    for (unsigned level = 0; level < 48 && type < vm->type_count; ++level) {
        if (!strcmp(type_id(vm, type), "Ljava/lang/Object;")) {
            *equal = query.bits == existing.bits;
            return 1;
        }
        uint32_t def = vm->class_definition[type];
        if (!def) return 0;
        for (uint32_t m = 0; m < vm->method_count; ++m) {
            if (u16(vm->dex + vm->method_off + 8u * m) != type ||
                strcmp(method_name(vm, m), "equals")) continue;
            char proto[128];
            if (!method_signature(vm, m, proto, sizeof proto) ||
                !strcmp(proto, "(Ljava/lang/Object;)Z")) return 0;
        }
        uint32_t parent = u32(vm->dex + def + 8u);
        if (parent == UINT32_MAX || parent == type) return 0;
        type = parent;
    }
    return 0;
}

/* CopyOnWriteArraySet has array-backed unique-element semantics. This VM is
 * single-threaded; no Java thread service exists here. No add/remove commits
 * until every required equals operation is faithfully evaluable. Return -3
 * to stop at the original call when equality needs an external Java method. */
static int invoke_copy_on_write_set(BsDexVM *vm, const char *name,
                                    const char *signature, uint8_t kind,
                                    const BsDexValue *args, size_t count,
                                    BsDexValue *result) {
    if (!count) return 0;
    BsDexRef *set = known_ref(vm, args[0]);
    if (!set || !set->owns_instance || set->backing ||
        strcmp(type_id(vm, set->type_index), BS_DEX_COW_SET)) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !strcmp(signature, "()V") && count == 1 && !set->set_initialized) {
        set->set_initialized = 1;
        return 1;
    }
    if ((kind != 0x6e && kind != 0x72) || !set->set_initialized) return 0;
    if (count == 1 && !strcmp(name, "size") && !strcmp(signature, "()I")) {
        *result = bs_dex_int((int32_t)set->set_count);
        return 1;
    }
    if (count == 1 && !strcmp(name, "isEmpty") && !strcmp(signature, "()Z")) {
        *result = bs_dex_int(set->set_count == 0);
        return 1;
    }
    if (count == 1 && !strcmp(name, "clear") && !strcmp(signature, "()V")) {
        set->set_count = 0;
        return 1;
    }
    int add = count == 2 && !strcmp(name, "add") &&
              !strcmp(signature, "(Ljava/lang/Object;)Z");
    int contains = count == 2 && !strcmp(name, "contains") &&
                   !strcmp(signature, "(Ljava/lang/Object;)Z");
    int remove = count == 2 && !strcmp(name, "remove") &&
                 !strcmp(signature, "(Ljava/lang/Object;)Z");
    if (!add && !contains && !remove) return 0;
    if (!is_dex_null(args[1]) && !known_ref(vm, args[1])) return 0;
    BsDexValue query = is_dex_null(args[1]) ? bs_dex_null() : args[1];
    int32_t found = -1;
    for (uint32_t i = 0; i < set->set_count; ++i) {
        int equal;
        if (!supported_set_equals(vm, query, set->set_elements[i], &equal))
            return -3;
        if (equal) { found = (int32_t)i; break; }
    }
    if (contains) {
        *result = bs_dex_int(found >= 0);
        return 1;
    }
    if (remove) {
        if (found >= 0) {
            size_t tail = (size_t)(set->set_count - (uint32_t)found - 1);
            memmove(set->set_elements + found, set->set_elements + found + 1,
                    tail * sizeof *set->set_elements);
            --set->set_count;
        }
        *result = bs_dex_int(found >= 0);
        return 1;
    }
    if (found >= 0) {
        *result = bs_dex_int(0);
        return 1;
    }
    if (set->set_count == set->set_capacity) {
        uint32_t capacity = set->set_capacity ? set->set_capacity * 2 : 4;
        if (capacity > BS_DEX_MAX_OBJECTS) return 0;
        BsDexValue *storage = realloc(set->set_elements,
                                     capacity * sizeof *storage);
        if (!storage) return 0;
        set->set_elements = storage;
        set->set_capacity = capacity;
    }
    set->set_elements[set->set_count++] = query;
    *result = bs_dex_int(1);
    return 1;
}

/* The VM runs one Java thread, so the array's circular head/size faithfully
 * implement ArrayDeque's observable FIFO/LIFO methods. No null elements are
 * permitted. Its backing array starts at 16 elements and doubles, bounded by
 * the interpreter's memory limit; no external Activity or host array exists.
 * Java Object.equals is required only for contains/remove(Object), where an
 * unprovable dispatch fails closed without mutating the deque. */
static int deque_grow(BsDexRef *deque, uint32_t wanted) {
    if (wanted <= deque->deque_capacity) return 1;
    uint32_t next = deque->deque_capacity ? deque->deque_capacity : 16u;
    while (next < wanted) {
        if (next >= BS_DEX_MAX_DEQUE_CAPACITY) return 0;
        next *= 2u;
    }
    BsDexValue *elements = calloc(next, sizeof *elements);
    if (!elements) return 0;
    for (uint32_t i = 0; i < deque->deque_size; ++i)
        elements[i] = deque->deque_elements[
            (deque->deque_head + i) % deque->deque_capacity];
    free(deque->deque_elements);
    deque->deque_elements = elements;
    deque->deque_capacity = next;
    deque->deque_head = 0;
    return 1;
}

static uint32_t deque_index(const BsDexRef *deque, uint32_t offset) {
    return (deque->deque_head + offset) % deque->deque_capacity;
}

static int invoke_array_deque(BsDexVM *vm, const char *name,
                              const char *signature, uint8_t kind,
                              const BsDexValue *args, size_t count,
                              BsDexValue *result, const char **exception) {
    if (!count) return 0;
    BsDexRef *deque = known_ref(vm, args[0]);
    if (!deque || !deque->owns_instance || deque->backing ||
        strcmp(type_id(vm, deque->type_index), BS_DEX_ARRAY_DEQUE)) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !deque->deque_initialized) {
        uint32_t needed = 16;
        if (count == 2 && !strcmp(signature, "(I)V") &&
            args[1].kind == BS_DEX_INT) {
            int32_t requested = (int32_t)args[1].bits;
            if (requested < 0) {
                *exception = "Ljava/lang/IllegalArgumentException;";
                return -1;
            }
            if ((uint32_t)requested > BS_DEX_MAX_DEQUE_CAPACITY) return 0;
            if ((uint32_t)requested > needed) needed = (uint32_t)requested;
        } else if (count != 1 || strcmp(signature, "()V")) return 0;
        if (!deque_grow(deque, needed)) return 0;
        deque->deque_initialized = 1;
        return 1;
    }
    if (!deque->deque_initialized || (kind != 0x6e && kind != 0x72)) return 0;
    if (count == 1 && !strcmp(signature, "()I") &&
        !strcmp(name, "size")) {
        *result = bs_dex_int((int32_t)deque->deque_size);
        return 1;
    }
    if (count == 1 && !strcmp(signature, "()Z") &&
        !strcmp(name, "isEmpty")) {
        *result = bs_dex_int(deque->deque_size == 0);
        return 1;
    }
    if (count == 1 && !strcmp(signature, "()V") &&
        !strcmp(name, "clear")) {
        for (uint32_t i = 0; i < deque->deque_size; ++i)
            deque->deque_elements[deque_index(deque, i)] = bs_dex_null();
        deque->deque_size = 0;
        deque->deque_head = 0;
        return 1;
    }
    const int add_first = !strcmp(name, "addFirst") ||
                          !strcmp(name, "offerFirst") ||
                          !strcmp(name, "push");
    const int add_last = !strcmp(name, "addLast") ||
                         !strcmp(name, "offerLast") ||
                         !strcmp(name, "add") ||
                         !strcmp(name, "offer");
    if (count == 2 && (add_first || add_last) &&
        !strcmp(signature, (!strcmp(name, "add") ||
                            !strcmp(name, "offer") ||
                            !strcmp(name, "offerFirst") ||
                            !strcmp(name, "offerLast")) ?
                           "(Ljava/lang/Object;)Z" : "(Ljava/lang/Object;)V")) {
        if (is_dex_null(args[1])) {
            *exception = "Ljava/lang/NullPointerException;";
            return -1;
        }
        if (!known_ref(vm, args[1])) return 0;
        if (!deque_grow(deque, deque->deque_size + 1)) return 0;
        if (add_first) {
            deque->deque_head = (deque->deque_head +
                                 deque->deque_capacity - 1) %
                                deque->deque_capacity;
            deque->deque_elements[deque->deque_head] = args[1];
        } else {
            deque->deque_elements[
                deque_index(deque, deque->deque_size)] = args[1];
        }
        ++deque->deque_size;
        if (!strcmp(name, "add") || !strcmp(name, "offer") ||
            !strcmp(name, "offerFirst") || !strcmp(name, "offerLast"))
            *result = bs_dex_int(1);
        return 1;
    }
    const int front = !strcmp(name, "pollFirst") ||
                      !strcmp(name, "removeFirst") ||
                      !strcmp(name, "peekFirst") ||
                      !strcmp(name, "getFirst") ||
                      !strcmp(name, "poll") ||
                      !strcmp(name, "remove") ||
                      !strcmp(name, "peek") ||
                      !strcmp(name, "element") ||
                      !strcmp(name, "pop");
    const int back = !strcmp(name, "pollLast") ||
                     !strcmp(name, "removeLast") ||
                     !strcmp(name, "peekLast") ||
                     !strcmp(name, "getLast");
    if (count == 1 && (front || back) &&
        !strcmp(signature, "()Ljava/lang/Object;")) {
        const int destructive = !strcmp(name, "pollFirst") ||
                                !strcmp(name, "pollLast") ||
                                !strcmp(name, "poll") ||
                                !strcmp(name, "removeFirst") ||
                                !strcmp(name, "removeLast") ||
                                !strcmp(name, "remove") ||
                                !strcmp(name, "pop");
        const int throw_empty = !strcmp(name, "removeFirst") ||
                                !strcmp(name, "removeLast") ||
                                !strcmp(name, "remove") ||
                                !strcmp(name, "getFirst") ||
                                !strcmp(name, "getLast") ||
                                !strcmp(name, "element") ||
                                !strcmp(name, "pop");
        if (!deque->deque_size) {
            if (throw_empty) {
                *exception = "Ljava/util/NoSuchElementException;";
                return -1;
            }
            *result = bs_dex_null();
            return 1;
        }
        uint32_t index = front ? deque->deque_head :
                          deque_index(deque, deque->deque_size - 1);
        *result = deque->deque_elements[index];
        if (destructive) {
            deque->deque_elements[index] = bs_dex_null();
            --deque->deque_size;
            if (front)
                deque->deque_head = (deque->deque_head + 1) %
                                    deque->deque_capacity;
            if (!deque->deque_size) deque->deque_head = 0;
        }
        return 1;
    }
    if (count == 2 && (!strcmp(name, "contains") ||
                       !strcmp(name, "remove")) &&
        !strcmp(signature, "(Ljava/lang/Object;)Z")) {
        if (is_dex_null(args[1])) {
            *result = bs_dex_int(0);
            return 1;
        }
        if (!known_ref(vm, args[1])) return 0;
        uint32_t found = deque->deque_size;
        for (uint32_t i = 0; i < deque->deque_size; ++i) {
            int equal = 0;
            if (!supported_set_equals(vm, args[1],
                      deque->deque_elements[deque_index(deque, i)], &equal))
                return -3;
            if (equal) { found = i; break; }
        }
        const int present = found != deque->deque_size;
        if (present && !strcmp(name, "remove")) {
            for (uint32_t i = found; i + 1 < deque->deque_size; ++i)
                deque->deque_elements[deque_index(deque, i)] =
                    deque->deque_elements[deque_index(deque, i + 1)];
            deque->deque_elements[deque_index(deque, deque->deque_size - 1)] =
                bs_dex_null();
            --deque->deque_size;
            if (!deque->deque_size) deque->deque_head = 0;
        }
        *result = bs_dex_int(present);
        return 1;
    }
    return 0;
}

BsDexValue bs_dex_vm_array_deque_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->owns_instance || !r->deque_initialized ||
            strcmp(type_id(vm, r->type_index), BS_DEX_ARRAY_DEQUE))
            continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_array_deque_diagnostic_call(BsDexVM *vm, BsDexValue deque,
                                          const char *method,
                                          const char *signature,
                                          const BsDexValue *arg,
                                          BsDexValue *result,
                                          const char **exception) {
    if (!vm || !method || !signature || !result || !exception)
        return 0;
    *result = bs_dex_null();
    *exception = NULL;
    BsDexValue args[2] = {deque, bs_dex_null()};
    if (arg) args[1] = *arg;
    return invoke_array_deque(vm, method, signature, 0x6e,
                              args, arg ? 2 : 1, result, exception);
}

/* ArrayList and CopyOnWriteArrayList store strong Java references in order.
 * In this VM's single interpreter thread, the supported collection methods
 * have the same sequential semantics. Iterator snapshots and parallel Java
 * mutation remain unsupported until there is an actual thread host. */
static int invoke_array_list(BsDexVM *vm, const char *name,
                             const char *signature, uint8_t kind,
                             const BsDexValue *args, size_t count,
                             BsDexValue *result, const char **exception) {
    if (!count || count > 3) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver) return 0;
    const int copy_on_write = !strcmp(type_id(vm, receiver->type_index),
                                       BS_DEX_COW_LIST);
    if (!receiver->owns_instance || receiver->backing ||
        (!copy_on_write &&
         strcmp(type_id(vm, receiver->type_index), BS_DEX_ARRAY_LIST))) return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !receiver->list_initialized) {
        uint32_t capacity = copy_on_write ? 0 : 10;
        if (!copy_on_write && count == 2 && !strcmp(signature, "(I)V") &&
            args[1].kind == BS_DEX_INT) {
            int32_t requested = (int32_t)args[1].bits;
            if (requested < 0) {
                *exception = "Ljava/lang/IllegalArgumentException;";
                return -1;
            }
            if ((uint32_t)requested > BS_DEX_MAX_OBJECTS) return 0;
            capacity = (uint32_t)requested;
        } else if (count != 1 || strcmp(signature, "()V")) return 0;
        if (capacity) {
            receiver->array = calloc(capacity, sizeof *receiver->array);
            if (!receiver->array) return 0;
        }
        receiver->list_capacity = capacity;
        receiver->list_initialized = 1;
        return 1;
    }
    if (kind != 0x6e || !receiver->list_initialized) return 0;
    if (count == 1 && !strcmp(name, "size") && !strcmp(signature, "()I")) {
        *result = bs_dex_int((int32_t)receiver->list_size);
        return 1;
    }
    if (count == 1 && !strcmp(name, "isEmpty") && !strcmp(signature, "()Z")) {
        *result = bs_dex_int(receiver->list_size == 0);
        return 1;
    }
    if (count == 1 && !strcmp(name, "clear") && !strcmp(signature, "()V")) {
        receiver->list_size = 0;
        return 1;
    }
    if (count == 2 && !strcmp(name, "add") &&
        !strcmp(signature, "(Ljava/lang/Object;)Z")) {
        if (!is_dex_null(args[1]) && !known_ref(vm, args[1])) return 0;
        if (receiver->list_size == receiver->list_capacity) {
            uint32_t old = receiver->list_capacity;
            uint32_t capacity = old ? old + (old >> 1) + 1 : 1;
            if (capacity > BS_DEX_MAX_OBJECTS) return 0;
            BsDexValue *grown = realloc(receiver->array,
                                         capacity * sizeof *grown);
            if (!grown) return 0;
            receiver->array = grown;
            receiver->list_capacity = capacity;
        }
        receiver->array[receiver->list_size++] =
            is_dex_null(args[1]) ? bs_dex_null() : args[1];
        *result = bs_dex_int(1);
        return 1;
    }
    if (count == 2 && !strcmp(name, "get") &&
        !strcmp(signature, "(I)Ljava/lang/Object;") &&
        args[1].kind == BS_DEX_INT) {
        int32_t index = (int32_t)args[1].bits;
        if (index < 0 || (uint32_t)index >= receiver->list_size) {
            *exception = "Ljava/lang/IndexOutOfBoundsException;";
            return -1;
        }
        *result = receiver->array[index];
        return 1;
    }
    if (copy_on_write && count == 3 && !strcmp(name, "set") &&
        !strcmp(signature, "(ILjava/lang/Object;)Ljava/lang/Object;") &&
        args[1].kind == BS_DEX_INT &&
        (is_dex_null(args[2]) || known_ref(vm, args[2]))) {
        int32_t index = (int32_t)args[1].bits;
        if (index < 0 || (uint32_t)index >= receiver->list_size) {
            *exception = "Ljava/lang/IndexOutOfBoundsException;";
            return -1;
        }
        *result = receiver->array[index];
        receiver->array[index] = is_dex_null(args[2]) ? bs_dex_null() : args[2];
        return 1;
    }
    if (copy_on_write && count == 2 &&
        ((!strcmp(name, "contains") &&
          !strcmp(signature, "(Ljava/lang/Object;)Z")) ||
         (!strcmp(name, "remove") &&
          !strcmp(signature, "(Ljava/lang/Object;)Z")))) {
        if (!is_dex_null(args[1]) && !known_ref(vm, args[1])) return 0;
        BsDexValue query = is_dex_null(args[1]) ? bs_dex_null() : args[1];
        int32_t found = -1;
        for (uint32_t i = 0; i < receiver->list_size; ++i) {
            int equal = 0;
            if (!supported_set_equals(vm, query, receiver->array[i], &equal))
                return -3;
            if (equal) { found = (int32_t)i; break; }
        }
        if (found >= 0 && !strcmp(name, "remove")) {
            size_t following = receiver->list_size - (uint32_t)found - 1;
            memmove(receiver->array + found, receiver->array + found + 1,
                    following * sizeof *receiver->array);
            --receiver->list_size;
        }
        *result = bs_dex_int(found >= 0);
        return 1;
    }
    return 0;
}

/* The VM's single-threaded objects live until teardown. For a native Java
 * object, ONLY the host can tell whether its weak referent is still alive.
 * A stored VM reference identity is a scalar tag, not a Java strong handle;
 * no external referent or JNI global is ever retained by the WeakReference. */
static int weak_reference_get(BsDexVM *vm, BsDexRef *receiver,
                              BsDexValue *result, const char **exception) {
    if (!vm || !receiver || !receiver->weak_initialized || !result) return 0;
    if (!receiver->weak_external_handle) {
        *result = receiver->weak_value;
        return 1;
    }
    if (!vm->host.weak_reference_get) return 0;
    BsDexValue observed = bs_dex_null();
    int rc = vm->host.weak_reference_get(vm->host.user, vm,
                    receiver->weak_external_handle, &observed, exception);
    if (rc != 1) return rc;
    if (is_dex_null(observed)) {
        *result = bs_dex_null();
        return 1;
    }
    BsDexRef *live = known_ref(vm, observed);
    if (!live || !live->backing ||
        observed.bits != receiver->weak_referent_identity) return -2;
    *result = observed;
    return 1;
}

static int weak_reference_clear(BsDexVM *vm, BsDexRef *receiver,
                                const char **exception) {
    if (!vm || !receiver || !receiver->weak_initialized) return 0;
    if (receiver->weak_external_handle) {
        if (!vm->host.weak_reference_clear) return 0;
        int rc = vm->host.weak_reference_clear(vm->host.user, vm,
                          receiver->weak_external_handle, exception);
        if (rc != 1) return rc;
    }
    receiver->weak_value = bs_dex_null();
    receiver->weak_cleared = 1;
    return 1;
}

int bs_dex_vm_weak_reference_get(BsDexVM *vm, BsDexValue weak,
                                 BsDexValue *live_or_null) {
    BsDexRef *r = known_ref(vm, weak);
    if (!r || !live_or_null ||
        strcmp(type_id(vm, r->type_index), BS_DEX_WEAK_REFERENCE)) return 0;
    const char *exception = NULL;
    BsDexValue value = bs_dex_null();
    if (weak_reference_get(vm, r, &value, &exception) != 1) return 0;
    *live_or_null = value;
    return 1;
}

int bs_dex_vm_weak_reference_clear(BsDexVM *vm, BsDexValue weak) {
    BsDexRef *r = known_ref(vm, weak);
    if (!r || strcmp(type_id(vm, r->type_index), BS_DEX_WEAK_REFERENCE))
        return 0;
    const char *exception = NULL;
    return weak_reference_clear(vm, r, &exception) == 1;
}

BsDexValue bs_dex_vm_weak_reference_at(BsDexVM *vm, size_t ordinal) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->owns_instance || !r->weak_initialized ||
            strcmp(type_id(vm, r->type_index), BS_DEX_WEAK_REFERENCE))
            continue;
        if (!ordinal--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_enum_at(BsDexVM *vm, size_t index,
                       BsDexValue *name, int32_t *ordinal) {
    if (!vm || !name || !ordinal) return 0;
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->enum_initialized || !vm_enum_subclass(vm, r)) continue;
        if (!index--) {
            *name = r->enum_name;
            *ordinal = r->enum_ordinal;
            return 1;
        }
    }
    return 0;
}

BsDexValue bs_dex_vm_enum_instance_at(BsDexVM *vm, size_t index) {
    if (!vm) return bs_dex_null();
    for (size_t i = 0; i < vm->reference_count; ++i) {
        BsDexRef *r = vm->references[i];
        if (!r->enum_initialized || !vm_enum_subclass(vm, r)) continue;
        if (!index--) return ref_value(r);
    }
    return bs_dex_null();
}

int bs_dex_vm_enum_compare_diagnostic_call(BsDexVM *vm,
                                            BsDexValue first,
                                            BsDexValue second,
                                            BsDexValue *result,
                                            const char **exception) {
    if (!vm || !result || !exception) return 0;
    BsDexValue args[2] = {first, second};
    *result = bs_dex_int(0);
    *exception = NULL;
    return invoke_enum(vm, "compareTo", "(Ljava/lang/Enum;)I", 0x6e,
                       args, 2, result, exception);
}

static int invoke_weak_reference(BsDexVM *vm, const char *name,
                                 const char *signature, uint8_t kind,
                                 const BsDexValue *args, size_t count,
                                 BsDexValue *result, const char **exception) {
    if (!count || count > 2) return 0;
    BsDexRef *receiver = known_ref(vm, args[0]);
    if (!receiver || !receiver->owns_instance || receiver->backing ||
        strcmp(type_id(vm, receiver->type_index), BS_DEX_WEAK_REFERENCE))
        return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !strcmp(signature, "(Ljava/lang/Object;)V") && count == 2 &&
        !receiver->weak_initialized &&
        (is_dex_null(args[1]) || known_ref(vm, args[1]))) {
        BsDexRef *referent = known_ref(vm, args[1]);
        if (!referent || !referent->backing) {
            receiver->weak_value = is_dex_null(args[1]) ? bs_dex_null() : args[1];
            receiver->weak_initialized = 1;
            return 1;
        }
        if (!vm->host.weak_reference_create ||
            !vm->host.weak_reference_get ||
            !vm->host.weak_reference_clear ||
            !vm->host.weak_reference_release) return 0;
        void *handle = NULL;
        int rc = vm->host.weak_reference_create(vm->host.user, vm, args[1],
                                                &handle, exception);
        if (rc != 1) {
            if (handle) vm->host.weak_reference_release(vm->host.user, vm,
                                                        handle);
            return rc;
        }
        if (!handle) return -2; /* host falsely claimed a usable weak handle */
        receiver->weak_external_handle = handle;
        receiver->weak_referent_identity = args[1].bits;
        receiver->weak_initialized = 1;
        return 1;
    }
    if (kind != 0x6e || !receiver->weak_initialized || count != 1) return 0;
    if (!strcmp(name, "get") && !strcmp(signature, "()Ljava/lang/Object;"))
        return weak_reference_get(vm, receiver, result, exception);
    if (!strcmp(name, "clear") && !strcmp(signature, "()V"))
        return weak_reference_clear(vm, receiver, exception);
    return 0;
}

/* Android's no-argument SpannableStringBuilder() creates an empty, mutable
 * UTF-16 sequence without spans. The VM can faithfully own that initial
 * state. It does not implement text editing, span mutation or reflection;
 * those operations must stop at the original call site. */
static int invoke_empty_spannable(BsDexVM *vm, const char *klass,
                                  const char *name, const char *signature,
                                  uint8_t kind, const BsDexValue *args,
                                  size_t count, BsDexValue *result) {
    if (!vm || !klass || !name || !signature || !args || !result ||
        strcmp(klass, BS_DEX_SPANNABLE_BUILDER) || count != 1)
        return 0;
    BsDexRef *ref = known_ref(vm, args[0]);
    if (!ref || !ref->owns_instance || ref->backing ||
        strcmp(type_id(vm, ref->type_index), BS_DEX_SPANNABLE_BUILDER))
        return 0;
    if (kind == 0x70 && !strcmp(name, "<init>") &&
        !strcmp(signature, "()V") && !ref->empty_spannable_initialized) {
        ref->empty_spannable_initialized = 1;
        *result = bs_dex_null();
        return 1;
    }
    if (kind == 0x6e && ref->empty_spannable_initialized &&
        !strcmp(name, "length") && !strcmp(signature, "()I")) {
        *result = bs_dex_int(0);
        return 1;
    }
    return 0;
}

static int invoke_instruction(BsDexVM *vm, BsDexFrame *frame,
                              uint16_t const *code, uint8_t op) {
    const int is_range = op >= 0x74 && op <= 0x78;
    uint32_t nargs = is_range ? code[0] >> 8 : code[0] >> 12;
    const uint32_t method_index = code[1];
    if (method_index >= vm->method_count || nargs > BS_DEX_MAX_REGISTERS) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "original invoke method index/register count invalid", op);
        return 0;
    }
    BsDexValue args[BS_DEX_MAX_REGISTERS];
    for (uint32_t i = 0; i < nargs; ++i) {
        uint32_t register_index;
        if (is_range) register_index = code[2] + i;
        else if (i == 4) register_index = (code[0] >> 8) & 0xfu;
        else register_index = (code[2] >> (i * 4u)) & 0xfu;
        if (!check_register(frame, register_index)) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "original invoke references invalid register", op);
            return 0;
        }
        args[i] = frame->reg[register_index];
    }
    uint32_t selected_method = method_index;
    const uint8_t kind = (uint8_t)(is_range ? op - 6 : op);
    /* Virtual and interface methods use the ACTUAL object's DEX type.
     * invoke-super/direct/static resolve their selected declarations. */
    if (kind == 0x6e || kind == 0x72) {
        if (!nargs || !args[0].bits || args[0].kind != BS_DEX_REF) {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "NullPointerException on virtual invoke", op);
            vm->stop.exception = "Ljava/lang/NullPointerException;";
            return 0;
        }
        BsDexRef *receiver = known_ref(vm, args[0]);
        if (!receiver) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "virtual call receiver outside the interpreter", op);
            return 0;
        }
        uint32_t found = find_method(vm, receiver->type_index,
                                      method_index, 1);
        if (found != UINT32_MAX) selected_method = found;
    } else if (kind == 0x6f) {
        uint32_t defined = u16(vm->dex + vm->method_off +
                               method_index * 8u);
        uint32_t found = find_method(vm, defined, method_index, 1);
        if (found != UINT32_MAX) selected_method = found;
    } else if (kind == 0x71 && !vm->method_code[selected_method]) {
        /* Java static methods can be inherited. Kotlin's LazyKt facade in
         * the verified APK refers to a static lazy(Function0) method whose
         * ORIGINAL executable DEX body is declared by its superclass
         * LazyKt__LazyJVMKt. Resolve from the actual DEX super chain, with
         * exact name/prototype and ACC_STATIC; execute that body instead of
         * inventing a Lazy result or interpreting a method-ID as a stub. */
        uint32_t declared_type =
            u16(vm->dex + vm->method_off + method_index * 8u);
        uint32_t inherited = find_method(vm, declared_type,
                                         method_index, 1);
        if (inherited != UINT32_MAX &&
            (vm->method_access[inherited] & 0x0008u))
            selected_method = inherited;
    }
    if (kind != 0x71 && (!nargs ||
                          (args[0].kind != BS_DEX_REF && !is_dex_null(args[0])) ||
                          !args[0].bits)) {
        error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                   "NullPointerException on Java invocation", op);
        vm->stop.exception = "Ljava/lang/NullPointerException;";
        return 0;
    }
    if (kind == 0x71) { /* Class initializer must run before invoke-static. */
        /* Inherited static method belongs to its declaring superclass;
         * Java initializes that owner, not an uninvoked subclass facade. */
        uint32_t type = u16(vm->dex + vm->method_off +
                            selected_method * 8u);
        int ready = ensure_class_initialized(vm, type);
        if (ready == 0) return 2;
        if (ready != 1) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "static method class initialization failed", op);
            return 0;
        }
    }
    if (vm->method_code[selected_method]) {
        /* The DEX has the implementation: execute it, including subclasses,
         * even if a platform callback pretends to implement the same name. */
        if (!push_frame(vm, selected_method, args, nargs)) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "DEX method arguments do not match code_item", op);
            return 0;
        }
        frame->dex_pc += 3;
        frame->pending_result_valid = 0;
        return 1;
    }
    /* The referenced Java platform/native method has no original code.
     * The host must perform it and provide the actual result. */
    const char *klass = method_class(vm, selected_method);
    const char *name = method_name(vm, selected_method);
    char proto[512];
    if (!method_signature(vm, selected_method, proto, sizeof proto)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "invalid DEX method prototype", op);
        return 0;
    }
    BsDexRef *handler_receiver =
        kind != 0x71 && nargs ? known_ref(vm, args[0]) : NULL;
    const int tracked_handler = handler_receiver &&
                                handler_receiver->handler_allocated;
    const int handler_declared = !strcmp(klass, BS_DEX_HANDLER);
    if (tracked_handler || handler_declared) {
        int allowed = handler_method_supported(vm, handler_receiver,
                            klass, name, proto, kind, nargs);
        if (allowed != 1) {
            host_stop(vm, klass, name, NULL, op,
                "Handler requires live original main Looper/owner; "
                "Runnable scheduling and canonical Java Looper unavailable");
            return 0;
        }
    }
    /* java.lang.Object.<init>()V is specified to have no body-level state
     * changes for a freshly allocated Java object. Its sole requirement is
     * a valid allocated receiver, which this interpreter already enforces.
     * Android View/Thread/etc. still require the host to perform their
     * observable initialization. */
    if (!strcmp(klass, "Ljava/lang/Object;") &&
        !strcmp(name, "<init>") && !strcmp(proto, "()V") &&
        kind == 0x70 && nargs == 1) {
        BsDexRef *receiver = known_ref(vm, args[0]);
        if (receiver && receiver->owns_instance) {
            frame->dex_pc += 3;
            frame->pending_result_valid = 0;
            return 1;
        }
    }
    BsDexValue result = bs_dex_null();
    const char *exception = NULL;
    const int object_equals = !strcmp(klass, BS_DEX_OBJECT) &&
                              !strcmp(name, "equals") &&
                              !strcmp(proto, "(Ljava/lang/Object;)Z") &&
                              kind == 0x6e && nargs == 2;
    int rc = 0;
    if (object_equals) {
        BsDexRef *receiver = known_ref(vm, args[0]);
        if (receiver && !receiver->backing) {
            /* Object.equals uses identity for DEX-owned classes that have
             * no equals/hashCode override through their complete known
             * superclass chain. java.lang.String uses exact measured UTF16
             * content equality. Unknown overrides cannot return guessed
             * results to the real DEX interpreter. */
            int same = 0;
            if (!known_map_key_equals(vm, args[0], args[1], &same)) {
                host_stop(vm, klass, name, NULL, op,
                          "Object.equals requires unimplemented override");
                return 0;
            }
            result = bs_dex_int(same);
            rc = 1;
        }
    }
    BsDexRef *thread_receiver = kind == 0x71 || !nargs ?
                                NULL : known_ref(vm, args[0]);
    const int observed_identity_receiver = thread_receiver &&
        (thread_receiver->is_observed_main_looper ||
         thread_receiver->is_observed_thread);
    const int observed_identity_call =
        (!strcmp(klass, BS_DEX_LOOPER) &&
         (!strcmp(name, "getMainLooper") || !strcmp(name, "getThread"))) ||
        (!strcmp(klass, BS_DEX_THREAD) &&
         !strcmp(name, "currentThread"));
    const int boxed_boolean = !strcmp(klass, BS_DEX_BOOLEAN) &&
                              (!strcmp(name, "valueOf") ||
                               (thread_receiver &&
                                thread_receiver->boolean_initialized));
    if (boxed_boolean) {
        rc = invoke_boolean(vm, name, proto, kind, args, nargs, &result);
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "cannot allocate VM Boolean singleton", op);
            return 0;
        }
        if (!rc) {
            host_stop(vm, klass, name, NULL, op,
                      "Boolean method not backed by observed Java value");
            return 0;
        }
    }
    if (observed_identity_call || observed_identity_receiver) {
        rc = invoke_observed_looper_thread(vm, klass, name, proto,
                                            kind, args, nargs, &result);
        if (rc == -2) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host reported invalid main Looper/thread identity", op);
            return 0;
        }
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "observed native thread token allocation limit", op);
            return 0;
        }
        if (!rc) {
            host_stop(vm, klass, name, NULL, op,
                      "live main Looper/native thread identity unavailable");
            return 0;
        }
    }
    const int exact_get_class = !strcmp(klass, BS_DEX_OBJECT) &&
                                !strcmp(name, "getClass") &&
                                !strcmp(proto, "()Ljava/lang/Class;") &&
                                kind == 0x6e && nargs == 1;
    if (!rc && exact_get_class) {
        rc = exact_vm_object_class(vm, args[0], &result);
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "cannot allocate canonical original DEX Class", op);
            return 0;
        }
        BsDexRef *receiver = known_ref(vm, args[0]);
        if (!rc && receiver && !receiver->backing) {
            host_stop(vm, klass, name, NULL, op,
                      "VM-owned object lacks verified exact DEX class metadata");
            return 0;
        }
    }
    BsDexRef *executor_receiver = kind == 0x71 || !nargs ?
                                  NULL : known_ref(vm, args[0]);
    const int owned_executor = executor_receiver &&
                               (executor_receiver->executor_initialized ||
                                executor_receiver->executor_queue_initialized);
    const int fixed_pool_call = !strcmp(klass, BS_DEX_EXECUTORS) &&
                                !strcmp(name, "newFixedThreadPool") &&
                                kind == 0x71;
    if (!rc && (fixed_pool_call || owned_executor))
        rc = invoke_executor(vm, klass, name, proto, kind, args, nargs,
                             &result, &exception);
    if (rc == -3) {
        error_stop(vm, BS_DEX_STOP_LIMIT,
                   "bounded VM cannot allocate requested Java thread pool", op);
        return 0;
    }
    if (rc == -4 || (!rc && (fixed_pool_call || owned_executor))) {
        host_stop(vm, klass, name, NULL, op,
                  "Java ExecutorService needs real worker lifecycle and Runnable scheduling");
        return 0;
    }
    if (!rc && !strcmp(klass, BS_DEX_ATOMIC_REF))
        rc = invoke_atomic_reference(vm, name, proto, kind, args, nargs, &result);
    if (!rc && !strcmp(klass, BS_DEX_ATOMIC_INT))
        rc = invoke_atomic_integer(vm, name, proto, kind,
                                   args, nargs, &result);
    if (!rc && !strcmp(klass, BS_DEX_RANDOM)) {
        rc = invoke_random(vm, name, proto, kind, args, nargs,
                           &result, &exception);
        /* A host with arbitrary Java invoke stubs must never override a
         * failed RNG seed observation or silently seed a VM-owned Random. */
        if (!rc) {
            host_stop(vm, klass, name, NULL, op,
                      "Random operation unsupported or monotonic clock missing");
            return 0;
        }
    }
    if (!rc && !strcmp(klass, BS_DEX_BUNDLE)) {
        rc = invoke_bundle(vm, name, proto, kind, args, nargs, &result);
        if (!rc && kind != 0x71 && nargs) {
            BsDexRef *receiver = known_ref(vm, args[0]);
            if (receiver && receiver->owns_instance && !receiver->backing &&
                !strcmp(type_id(vm, receiver->type_index), BS_DEX_BUNDLE)) {
                host_stop(vm, klass, name, NULL, op,
                          "VM-owned Bundle needs unsupported Android Parcel or type operation");
                return 0;
            }
        }
    }
    BsDexRef *sync_receiver = kind == 0x71 || !nargs ?
                              NULL : known_ref(vm, args[0]);
    const int synchronized_map_factory =
        kind == 0x71 && !strcmp(klass, BS_DEX_COLLECTIONS) &&
        !strcmp(name, "synchronizedMap");
    const int synchronized_map_receiver =
        sync_receiver && sync_receiver->synchronized_map_initialized;
    if (!rc && (synchronized_map_factory || synchronized_map_receiver)) {
        rc = invoke_synchronized_map(vm, klass, name, proto, kind,
                                     args, nargs, &result, &exception);
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "bounded Java synchronizedMap wrapper allocation/monitor limit",
                       op);
            return 0;
        }
        if (rc == -4 || !rc) {
            host_stop(vm, klass, name, NULL, op,
                      "synchronizedMap requires unavailable Java Map "
                      "behavior, reflection, or guest thread scheduling");
            return 0;
        }
    }
    BsDexRef *map_receiver = (kind == 0x6e || kind == 0x72) && nargs ?
                             known_ref(vm, args[0]) : NULL;
    const int owned_hash_map = map_receiver &&
        map_receiver->owns_instance && !map_receiver->backing &&
        !strcmp(type_id(vm, map_receiver->type_index), BS_DEX_HASH_MAP);
    const int owned_weak_map = map_receiver &&
        map_receiver->owns_instance && !map_receiver->backing &&
        !strcmp(type_id(vm, map_receiver->type_index), BS_DEX_WEAK_MAP);
    if (!rc && owned_hash_map) {
        rc = invoke_hash_map(vm, name, proto, kind, args, nargs, &result);
        if (rc == -4) {
            host_stop(vm, klass, name, NULL, op,
                      "HashMap key needs unavailable Java hashCode/equals");
            return 0;
        }
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "bounded VM HashMap entry budget exhausted", op);
            return 0;
        }
        if (!rc) {
            host_stop(vm, klass, name, NULL, op,
                      "VM-owned HashMap method requires unimplemented Java behavior");
            return 0;
        }
    }
    if (!rc && owned_weak_map) {
        rc = invoke_weak_hash_map(vm, name, proto, kind, args, nargs,
                                  &result, &exception);
        if (rc == -2) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host returned invalid WeakHashMap weak key identity", op);
            return 0;
        }
        if (rc == -3) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "bounded VM WeakHashMap entry budget exhausted", op);
            return 0;
        }
        if (rc == -4 || rc == -5 || rc == 0) {
            host_stop(vm, klass, name, NULL, op,
                      rc == -5 ?
                      "WeakHashMap key requires verified GC/ReferenceQueue lifetime" :
                      rc == -4 ?
                      "WeakHashMap key requires exact Java hashCode/equals" :
                      "VM-owned WeakHashMap method requires unavailable Java behavior");
            return 0;
        }
    }
    BsDexRef *enum_receiver = kind == 0x71 || !nargs ?
                              NULL : known_ref(vm, args[0]);
    if (!rc && (!strcmp(klass, BS_DEX_ENUM) ||
                (enum_receiver && vm_enum_subclass(vm, enum_receiver) &&
                 !strcmp(name, "compareTo"))))
        rc = invoke_enum(vm, name, proto, kind, args, nargs,
                         &result, &exception);
    if (!rc && (!strcmp(klass, BS_DEX_COW_SET) ||
                !strcmp(klass, "Ljava/util/Set;")))
        rc = invoke_copy_on_write_set(vm, name, proto, kind, args, nargs,
                                      &result);
    if (rc == -3) {
        host_stop(vm, klass, name, NULL, op,
                  "CopyOnWriteArraySet requires unimplemented Java equals");
        return 0;
    }
    if (!rc && (!strcmp(klass, BS_DEX_ARRAY_LIST) ||
                !strcmp(klass, BS_DEX_COW_LIST)))
        rc = invoke_array_list(vm, name, proto, kind, args, nargs,
                               &result, &exception);
    if (rc == -3) {
        host_stop(vm, klass, name, NULL, op,
                  "CopyOnWriteArrayList requires unimplemented Java equals");
        return 0;
    }
    if (!rc && (!strcmp(klass, BS_DEX_ARRAY_DEQUE) ||
                !strcmp(klass, "Ljava/util/Deque;") ||
                !strcmp(klass, "Ljava/util/Queue;"))) {
        rc = invoke_array_deque(vm, name, proto, kind, args, nargs,
                                &result, &exception);
        if (rc == -3) {
            host_stop(vm, klass, name, NULL, op,
                      "ArrayDeque requires unavailable Java equals");
            return 0;
        }
    }
    BsDexRef *weak_receiver = kind == 0x71 || !nargs ? NULL :
                              known_ref(vm, args[0]);
    const int weak_method = weak_receiver &&
                            !strcmp(type_id(vm, weak_receiver->type_index),
                                    BS_DEX_WEAK_REFERENCE) &&
                            (!strcmp(klass, BS_DEX_WEAK_REFERENCE) ||
                             !strcmp(klass, BS_DEX_REFERENCE)) &&
                            ((kind == 0x70 && !strcmp(name, "<init>")) ||
                             (kind == 0x6e && (!strcmp(name, "get") ||
                                              !strcmp(name, "clear"))));
    if (!rc && weak_method)
        rc = invoke_weak_reference(vm, name, proto, kind, args, nargs,
                                   &result, &exception);
    if (weak_method && rc == -2) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "weak-reference host returned an invalid live referent", op);
        return 0;
    }
    if (weak_method && !rc) {
        host_stop(vm, klass, name, NULL, op,
                  "external Java weak-reference lifetime callback missing");
        return 0;
    }
    if (!rc && !strcmp(name, "<init>") &&
        (!strcmp(klass, BS_DEX_WEAK_MAP) || !strcmp(klass, BS_DEX_HASH_MAP)))
        rc = construct_map(vm, klass, proto, kind, args, nargs);
    if (!rc && !strcmp(klass, BS_DEX_SPANNABLE_BUILDER)) {
        rc = invoke_empty_spannable(vm, klass, name, proto, kind,
                                    args, nargs, &result);
        /* An owned Android Editable may have stateful operations, so
         * unsupported calls must never fall through to a generic host. */
        if (!rc) {
            host_stop(vm, klass, name, NULL, op,
                      "SpannableStringBuilder mutation/span operation not implemented");
            return 0;
        }
    }
    if (!rc && !vm->host.invoke_platform) {
        host_stop(vm, klass, name, NULL, op,
                  "original Java method has no body or host implementation");
        return 0;
    }
    if (!rc)
        rc = vm->host.invoke_platform(vm->host.user, vm, klass, name,
                                      proto, op, args, nargs, &result,
                                      &exception);
    if (rc != 1) {
        host_failure(vm, rc, klass, name, exception, op,
                     "Java host/native method did not execute");
        return 0;
    }
    if (tracked_handler) {
        /* The backend may have witnessed Handler.<init> but a queue may
         * have been torn down during the callback. The constructor is
         * committed only with unchanged, independently observed native
         * main queue/OS-thread owner identity. */
        if (handler_constructor_gate(vm, handler_receiver) != 1) {
            host_stop(vm, klass, name, NULL, op,
                      "Handler main Looper expired during construction");
            return 0;
        }
        handler_receiver->handler_initialized = 1;
    }
    const char *result_desc = strchr(proto, ')');
    if (!result_desc || !result_desc[1]) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "invalid Java method return descriptor", op);
        return 0;
    }
    ++result_desc;
    if (result_desc[0] == 'L' || result_desc[0] == '[') {
        if (is_dex_null(result)) result = bs_dex_null();
        if (result.kind != BS_DEX_REF) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host returned primitive from Java reference method", op);
            return 0;
        }
    } else if (result_desc[0] == 'J' || result_desc[0] == 'D') {
        if (result.kind != BS_DEX_WIDE) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host returned wrong wide Java primitive", op);
            return 0;
        }
    } else if (result_desc[0] != 'V' && result.kind != BS_DEX_INT) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "host returned reference from Java primitive method", op);
        return 0;
    }
    if (result.kind == BS_DEX_REF && result.bits && !known_ref(vm, result)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "Java host returned untracked reference", op);
        return 0;
    }
    frame->pending_result = result;
    frame->pending_result_valid = 1;
    frame->dex_pc += 3;
    return 1;
}

static int new_instance_instruction(BsDexVM *vm, BsDexFrame *frame,
                                    uint8_t opcode, uint16_t destination,
                                    uint16_t type) {
    if (type >= vm->type_count || !check_register(frame, destination)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "new-instance destination/type out of range", opcode);
        return 0;
    }
    int ready = ensure_class_initialized(vm, type);
    if (ready == 0) return 2;
    if (ready != 1) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "new-instance class initialization failed", opcode);
        return 0;
    }
    BsDexValue object = bs_dex_null();
    const int constructing_handler =
        !strcmp(type_id(vm, type), BS_DEX_HANDLER);
    uintptr_t handler_queue = 0;
    uint64_t handler_owner = 0;
    size_t original_ref_count = vm->reference_count;
    if (constructing_handler) {
        int rc = observed_handler_main_thread(vm, &handler_queue,
                                               &handler_owner);
        if (rc == -2) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host reported invalid native Handler main Looper", opcode);
            return 0;
        }
        if (rc != 1) {
            host_stop(vm, type_id(vm, type), "new-instance", NULL,
                      opcode, "Handler needs independently observed native "
                      "main Looper on its owning OS thread");
            return 0;
        }
    }
    if (vm->class_definition[type] ||
        !strcmp(type_id(vm, type), BS_DEX_OBJECT) ||
        !strcmp(type_id(vm, type), BS_DEX_ATOMIC_REF) ||
        !strcmp(type_id(vm, type), BS_DEX_ATOMIC_INT) ||
        !strcmp(type_id(vm, type), BS_DEX_RANDOM) ||
        !strcmp(type_id(vm, type), BS_DEX_BUNDLE) ||
        !strcmp(type_id(vm, type), BS_DEX_ARRAY_LIST) ||
        !strcmp(type_id(vm, type), BS_DEX_WEAK_REFERENCE) ||
        !strcmp(type_id(vm, type), BS_DEX_COW_SET) ||
        !strcmp(type_id(vm, type), BS_DEX_COW_LIST) ||
        !strcmp(type_id(vm, type), BS_DEX_ARRAY_DEQUE) ||
        !strcmp(type_id(vm, type), BS_DEX_WEAK_MAP) ||
        !strcmp(type_id(vm, type), BS_DEX_HASH_MAP) ||
        !strcmp(type_id(vm, type), BS_DEX_SPANNABLE_BUILDER)) {
        BsDexRef *created = allocate_ref(vm, type, NULL, 1, NULL);
        if (!created) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "Java object allocation limit exceeded", opcode);
            return 0;
        }
        object = ref_value(created);
    } else {
        if (!vm->host.new_platform_object) {
            host_stop(vm, type_id(vm, type), "new-instance", NULL,
                      opcode, "Android class allocation unavailable");
            return 0;
        }
        const char *exception = NULL;
        int rc = vm->host.new_platform_object(vm->host.user, vm,
                    type_id(vm, type), &object, &exception);
        if (rc != 1) {
            host_failure(vm, rc, type_id(vm, type), "new-instance",
                         exception, opcode,
                         "platform Java allocation did not execute");
            return 0;
        }
        BsDexRef *r = known_ref(vm, object);
        if (!r || !r->backing || r->type_index != type) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "host did not return the allocated Java class", opcode);
            return 0;
        }
        if (constructing_handler) {
            int is_fresh = 0;
            for (size_t i = original_ref_count;
                 i < vm->reference_count; ++i) {
                if (vm->references[i] == r) {
                    is_fresh = 1;
                    break;
                }
            }
            if (!is_fresh || r->owns_instance || r->handler_allocated) {
                error_stop(vm, BS_DEX_STOP_CORRUPT,
                           "Handler.new-instance requires fresh backed object",
                           opcode);
                return 0;
            }
            r->handler_allocated = 1;
            r->handler_main_queue_identity = handler_queue;
            r->handler_owner_thread_id = handler_owner;
        }
    }
    frame->reg[destination] = object;
    frame->dex_pc += 2;
    return 1;
}

static int change_pc(BsDexVM *vm, BsDexFrame *frame,
                     int32_t relative, uint8_t op) {
    int64_t destination = (int64_t)frame->dex_pc + relative;
    if (destination < 0 || destination >= frame->instruction_count) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "DEX branch jumps outside executable method", op);
        return 0;
    }
    frame->dex_pc = (uint32_t)destination;
    return 1;
}

static int checked_words(BsDexVM *vm, BsDexFrame *f, uint16_t *w,
                         unsigned size, uint8_t op) {
    if (read_words(vm, f, w, size)) return 1;
    error_stop(vm, BS_DEX_STOP_CORRUPT,
               "truncated DEX instruction or code_item", op);
    return 0;
}

static int assign_register(BsDexVM *vm, BsDexFrame *f, unsigned to,
                           unsigned from, uint8_t op) {
    if (!check_register(f, to) || !check_register(f, from)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "DEX register index outside code_item", op);
        return 0;
    }
    f->reg[to] = f->reg[from];
    return 1;
}

static int field_instruction(BsDexVM *vm, BsDexFrame *f,
                             const uint16_t w[2], uint8_t op) {
    const int is_static = op >= 0x60;
    const int is_write = is_static ? op >= 0x67 : op >= 0x59;
    unsigned a, b = 0;
    if (is_static) a = w[0] >> 8;
    else {
        a = (w[0] >> 8) & 15u;
        b = (w[0] >> 12) & 15u;
    }
    const uint32_t field = w[1];
    if (field >= vm->field_count || !check_register(f, a) ||
        (!is_static && !check_register(f, b))) {
        error_stop(vm, BS_DEX_STOP_CORRUPT,
                   "field opcode refers to an invalid DEX field/register", op);
        return 0;
    }
    if (is_static) {
        uint32_t class_type = u16(vm->dex + vm->field_off + field * 8u);
        int initialized = ensure_class_initialized(vm, class_type);
        if (initialized == 0) return 2;
        if (initialized != 1) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "field class initializer unavailable", op);
            return 0;
        }
    }
    BsDexValue value = is_write ? f->reg[a] : bs_dex_null();
    if (!field_operation(vm, is_static ? bs_dex_null() : f->reg[b],
                         field, is_static, is_write, &value, op)) return 0;
    if (!is_write) f->reg[a] = value;
    if (is_static && is_write && f->code_off == BS_UJP_UNITY_PLAYER_CTOR_CODE) {
        const char *klass = field_class(vm, field);
        const char *name = field_name(vm, field);
        if (klass && name &&
            !strcmp(klass, "Lcom/unity3d/player/UnityPlayer;")) {
            if (f->dex_pc == BS_UJP_UNITY_CONTEXT_WRITE_PC &&
                !strcmp(name, "currentContext"))
                vm->current_context = value;
            if (f->dex_pc == BS_UJP_UNITY_ACTIVITY_WRITE_PC &&
                !strcmp(name, "currentActivity")) {
                if (value.kind != BS_DEX_REF ||
                    value.bits != vm->activity.bits ||
                    vm->current_context.bits != vm->activity.bits) {
                    error_stop(vm, BS_DEX_STOP_CORRUPT,
                               "original Activity write did not match live Context", op);
                    return 0;
                }
                vm->current_activity = value;
                f->dex_pc += 2;
                error_stop(vm, BS_DEX_STOP_CHECKPOINT,
                           "original currentActivity sput-object executed", op);
                vm->stop.dex_pc = BS_UJP_UNITY_ACTIVITY_WRITE_PC;
                return 3;
            }
        }
    }
    f->dex_pc += 2;
    return 1;
}

static int array_instruction(BsDexVM *vm, BsDexFrame *f,
                             const uint16_t w[2], uint8_t op) {
    unsigned a = w[0] >> 8;
    unsigned b = w[1] & 0xff;
    unsigned c = w[1] >> 8;
    if (!check_register(f, a) || !check_register(f, b) ||
        !check_register(f, c)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT, "array register out of bounds", op);
        return 0;
    }
    BsDexRef *array = known_ref(vm, f->reg[b]);
    if (!array || !array->array) {
        if (!array && f->reg[b].kind == BS_DEX_REF && !f->reg[b].bits) {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "NullPointerException on array", op);
            vm->stop.exception = "Ljava/lang/NullPointerException;";
        } else host_stop(vm, bs_dex_reference_type(vm, f->reg[b]),
                         "aget/aput", NULL, op,
                         "external Java array access requires host backing");
        return 0;
    }
    int32_t index = (int32_t)f->reg[c].bits;
    if (index < 0 || (uint32_t)index >= array->array_count) {
        error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                   "ArrayIndexOutOfBoundsException", op);
        vm->stop.exception = "Ljava/lang/ArrayIndexOutOfBoundsException;";
        return 0;
    }
    if (op >= 0x4b) array->array[index] = f->reg[a];
    else f->reg[a] = array->array[index];
    f->dex_pc += 2;
    return 1;
}

static int arithmetic_instruction(BsDexVM *vm, BsDexFrame *f,
                                   const uint16_t w[2], uint8_t op) {
    const unsigned a = w[0] >> 8;
    const unsigned b = w[1] & 0xff;
    const unsigned c = w[1] >> 8;
    if (!check_register(f, a) || !check_register(f, b) ||
        !check_register(f, c)) {
        error_stop(vm, BS_DEX_STOP_CORRUPT, "arithmetic register out of range", op);
        return 0;
    }
    const int32_t lhs = (int32_t)f->reg[b].bits;
    const int32_t rhs = (int32_t)f->reg[c].bits;
    int32_t result;
    switch (op) {
    case 0x90: result = (int32_t)((uint32_t)lhs + (uint32_t)rhs); break;
    case 0x91: result = (int32_t)((uint32_t)lhs - (uint32_t)rhs); break;
    case 0x92: result = (int32_t)((uint32_t)lhs * (uint32_t)rhs); break;
    case 0x95:
        result = lhs & rhs;
        break;
    case 0x96: result = lhs | rhs; break;
    case 0x97: result = lhs ^ rhs; break;
    case 0x98: result = (int32_t)((uint32_t)lhs << (rhs & 31)); break;
    case 0x99: result = lhs >> (rhs & 31); break;
    case 0x9a: result = (int32_t)((uint32_t)lhs >> (rhs & 31)); break;
    default:
        error_stop(vm, BS_DEX_STOP_OPCODE_MISSING,
                   "original Dalvik arithmetic operation not implemented", op);
        return 0;
    }
    f->reg[a] = bs_dex_int(result);
    f->dex_pc += 2;
    return 1;
}

static int run_instruction(BsDexVM *vm) {
    BsDexFrame *f = &vm->frames[vm->depth - 1];
    uint16_t w[5] = {0};
    if (!checked_words(vm, f, w, 1, 0)) return 0;
    const uint8_t op = (uint8_t)w[0];
    const unsigned high = w[0] >> 8;
    /* DEX move-result must immediately follow its producing invocation.
     * A later, unrelated move-result may not read an old cached return. */
    if (op != 0x0a && op != 0x0b && op != 0x0c)
        f->pending_result_valid = 0;
    unsigned a, b;
    switch (op) {
    case 0x00: /* nop */
        f->dex_pc++;
        return 1;
    case 0x01: case 0x04: case 0x07: /* move variants 12x */
        a = high & 15u; b = high >> 4;
        if (!assign_register(vm, f, a, b, op)) return 0;
        f->dex_pc++;
        return 1;
    case 0x02: case 0x05: case 0x08: /* move/from16 */
        if (!checked_words(vm, f, w, 2, op) ||
            !assign_register(vm, f, high, w[1], op)) return 0;
        f->dex_pc += 2;
        return 1;
    case 0x03: case 0x06: case 0x09: /* move/16 */
        if (!checked_words(vm, f, w, 3, op) ||
            !assign_register(vm, f, w[1], w[2], op)) return 0;
        f->dex_pc += 3;
        return 1;
    case 0x0a: case 0x0b: case 0x0c: /* move-result */
        if (!f->pending_result_valid || !check_register(f, high)) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "move-result without preceding completed invocation", op);
            return 0;
        }
        f->reg[high] = f->pending_result;
        f->pending_result_valid = 0;
        f->dex_pc++;
        return 1;
    case 0x0e: /* return-void */
        return return_frame(vm, bs_dex_int(0));
    case 0x0f: case 0x10: case 0x11: /* return a register */
        if (!check_register(f, high)) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "return uses invalid register", op);
            return 0;
        }
        return return_frame(vm, f->reg[high]);
    case 0x12: /* const/4 (signed nibble) */
        a = high & 15u;
        if (!check_register(f, a)) break;
        f->reg[a] = bs_dex_int(((int)(w[0] >> 12) ^ 8) - 8);
        f->dex_pc++;
        return 1;
    case 0x13: case 0x16: /* const/16, const-wide/16 */
        if (!checked_words(vm, f, w, 2, op) || !check_register(f, high)) return 0;
        f->reg[high] = op == 0x16 ?
             (BsDexValue){(uintptr_t)(int64_t)(int16_t)w[1], BS_DEX_WIDE} :
             bs_dex_int((int16_t)w[1]);
        f->dex_pc += 2;
        return 1;
    case 0x14: case 0x17: /* const, const-wide/32 */
        if (!checked_words(vm, f, w, 3, op) || !check_register(f, high)) return 0;
        f->reg[high] = op == 0x17 ?
            (BsDexValue){(uintptr_t)(int64_t)(int32_t)
                 ((uint32_t)w[1] | (uint32_t)w[2] << 16), BS_DEX_WIDE} :
            bs_dex_int((int32_t)((uint32_t)w[1] | (uint32_t)w[2] << 16));
        f->dex_pc += 3;
        return 1;
    case 0x15: case 0x19: /* const/high16, const-wide/high16 */
        if (!checked_words(vm, f, w, 2, op) || !check_register(f, high)) return 0;
        f->reg[high] = op == 0x19 ?
             (BsDexValue){(uintptr_t)((uint64_t)w[1] << 48), BS_DEX_WIDE} :
             bs_dex_int((int32_t)((uint32_t)w[1] << 16));
        f->dex_pc += 2;
        return 1;
    case 0x1a: case 0x1b: { /* original DEX const-string */
        unsigned words = op == 0x1a ? 2 : 3;
        if (!checked_words(vm, f, w, words, op) ||
            !check_register(f, high)) return 0;
        uint32_t index = w[1];
        if (op == 0x1b) index |= (uint32_t)w[2] << 16;
        if (index >= vm->string_count) break;
        BsDexValue literal = literal_string(vm, index);
        if (!literal.bits) {
            error_stop(vm, BS_DEX_STOP_LIMIT, "DEX string allocation failed", op);
            return 0;
        }
        f->reg[high] = literal;
        f->dex_pc += words;
        return 1;
    }
    case 0x1c: { /* const-class 21c; class resolution, not initialization */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        if (!check_register(f, high) || w[1] >= vm->type_count) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "const-class register/type index invalid", op);
            return 0;
        }
        if (!vm->class_definition[w[1]]) {
            host_stop(vm, type_id(vm, w[1]), "const-class", NULL, op,
                      "platform Class requires actual Android ClassLoader");
            return 0;
        }
        BsDexValue klass = resolve_dex_class_literal(vm, w[1]);
        if (!klass.bits) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "DEX Class literal allocation or type unavailable", op);
            return 0;
        }
        f->reg[high] = klass;
        f->dex_pc += 2;
        return 1;
    }
    case 0x1d: case 0x1e: { /* monitor-enter/exit; single-thread VM */
        if (!check_register(f, high)) break;
        BsDexRef *r = known_ref(vm, f->reg[high]);
        if (!r) {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "NullPointerException on Java monitor", op);
            vm->stop.exception = "Ljava/lang/NullPointerException;";
            return 0;
        }
        if (op == 0x1d && r->monitor_depth != UINT16_MAX) ++r->monitor_depth;
        else if (op == 0x1e && r->monitor_depth) --r->monitor_depth;
        else {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "IllegalMonitorStateException", op);
            vm->stop.exception = "Ljava/lang/IllegalMonitorStateException;";
            return 0;
        }
        f->dex_pc++;
        return 1;
    }
    case 0x1f: case 0x20: { /* check-cast 21c / instance-of 22c */
        if (!checked_words(vm, f, w, 2, op) || w[1] >= vm->type_count) return 0;
        a = op == 0x1f ? high : (high & 15u);
        b = op == 0x1f ? a : high >> 4;
        if (!check_register(f, a) || !check_register(f, b)) break;
        if (op == 0x1f && is_dex_null(f->reg[b])) {
            f->reg[b] = bs_dex_null();
            f->dex_pc += 2;
            return 1; /* Java allows check-cast of null to any type. */
        }
        const char *exception = NULL;
        int matches = 0;
        int rc = object_instance_of(vm, f->reg[b], w[1], &matches, &exception);
        if (rc != 1) {
            host_failure(vm, rc, type_id(vm, w[1]), "instance-of",
                         exception, op, "Android class hierarchy unavailable");
            return 0;
        }
        if (op == 0x1f && !matches) {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "ClassCastException in original check-cast", op);
            vm->stop.exception = "Ljava/lang/ClassCastException;";
            return 0;
        }
        if (op == 0x20) {
            f->reg[a] = bs_dex_int(matches);
            if (f->code_off == BS_UJP_UNITY_PLAYER_CTOR_CODE &&
                f->dex_pc == 0x92 &&
                !strcmp(type_id(vm, w[1]), "Landroid/app/Activity;"))
                vm->activity_instanceof_observed = matches &&
                    f->reg[b].bits == vm->activity.bits;
        }
        f->dex_pc += 2;
        return 1;
    }
    case 0x21: { /* array-length */
        a = high & 15u; b = high >> 4;
        if (!check_register(f, a) || !check_register(f, b)) break;
        BsDexRef *r = known_ref(vm, f->reg[b]);
        if (!r || !r->array) {
            host_stop(vm, bs_dex_reference_type(vm, f->reg[b]),
                      "array-length", NULL, op,
                      "array must be allocated and backed by VM");
            return 0;
        }
        f->reg[a] = bs_dex_int((int32_t)r->array_count);
        f->dex_pc++;
        return 1;
    }
    case 0x22: /* new-instance */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        return new_instance_instruction(vm, f, op, (uint16_t)high, w[1]);
    case 0x23: { /* new-array */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        a = high & 15u; b = high >> 4;
        if (!check_register(f, a) || !check_register(f, b) ||
            w[1] >= vm->type_count) break;
        int32_t length = (int32_t)f->reg[b].bits;
        if (length < 0) {
            error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                       "NegativeArraySizeException", op);
            vm->stop.exception = "Ljava/lang/NegativeArraySizeException;";
            return 0;
        }
        if (length > 8192) {
            error_stop(vm, BS_DEX_STOP_LIMIT, "Java array exceeds bounded VM", op);
            return 0;
        }
        BsDexRef *r = allocate_ref(vm, w[1], NULL, 1, NULL);
        if (!r) {
            error_stop(vm, BS_DEX_STOP_LIMIT, "Java array allocation failed", op);
            return 0;
        }
        r->array_count = (uint32_t)length;
        r->array = calloc((size_t)(length ? length : 1), sizeof *r->array);
        if (!r->array) {
            error_stop(vm, BS_DEX_STOP_LIMIT, "Java array storage failed", op);
            return 0;
        }
        const char *desc = type_id(vm, w[1]);
        for (int32_t i = 0; i < length; ++i)
            r->array[i] = desc && desc[1] == 'L' ? bs_dex_null() : bs_dex_int(0);
        f->reg[a] = ref_value(r);
        f->dex_pc += 2;
        return 1;
    }
    case 0x24: case 0x25: { /* filled-new-array 35c / range 3rc */
        if (!checked_words(vm, f, w, 3, op)) return 0;
        uint32_t count = op == 0x24 ? w[0] >> 12 : high;
        uint32_t array_type = w[1];
        const char *descriptor = type_id(vm, array_type);
        if (!descriptor || descriptor[0] != '[' ||
            (op == 0x24 && count > 5)) {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "filled-new-array has invalid type/count", op);
            return 0;
        }
        const char *element = descriptor + 1;
        if (element[0] == 'J' || element[0] == 'D') {
            error_stop(vm, BS_DEX_STOP_CORRUPT,
                       "wide component in filled-new-array", op);
            return 0;
        }
        const int reference_elements = element[0] == 'L' || element[0] == '[';
        uint32_t element_type = 0;
        if (reference_elements && !descriptor_type(vm, element, &element_type)) {
            host_stop(vm, descriptor, "filled-new-array", NULL, op,
                      "reference component type not present in original DEX");
            return 0;
        }
        BsDexValue elements[256];
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t reg = op == 0x25 ? (uint32_t)w[2] + i :
                           i == 4 ? (high & 15u) :
                           (w[2] >> (i * 4u)) & 15u;
            if (!check_register(f, reg)) {
                error_stop(vm, BS_DEX_STOP_CORRUPT,
                           "filled-new-array register outside code_item", op);
                return 0;
            }
            BsDexValue value = f->reg[reg];
            if (reference_elements) {
                if (is_dex_null(value)) value = bs_dex_null();
                else {
                    if (!known_ref(vm, value)) {
                        error_stop(vm, BS_DEX_STOP_CORRUPT,
                                   "filled-new-array contains unknown reference", op);
                        return 0;
                    }
                    int matches = 0;
                    const char *exception = NULL;
                    int rc = object_instance_of(vm, value, element_type,
                                                &matches, &exception);
                    if (rc != 1) {
                        host_failure(vm, rc, element, "filled-new-array",
                                     exception, op,
                                     "array element type needs real class hierarchy");
                        return 0;
                    }
                    if (!matches) {
                        error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                                   "ArrayStoreException on filled-new-array", op);
                        vm->stop.exception = "Ljava/lang/ArrayStoreException;";
                        return 0;
                    }
                }
            } else if (value.kind != BS_DEX_INT) {
                error_stop(vm, BS_DEX_STOP_CORRUPT,
                           "filled-new-array primitive type mismatch", op);
                return 0;
            }
            elements[i] = value;
        }
        BsDexRef *array = allocate_ref(vm, array_type, NULL, 1, NULL);
        if (!array) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "filled-new-array object allocation failed", op);
            return 0;
        }
        array->array = calloc(count ? count : 1, sizeof *array->array);
        if (!array->array) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "filled-new-array storage allocation failed", op);
            return 0;
        }
        array->array_count = count;
        for (uint32_t i = 0; i < count; ++i) array->array[i] = elements[i];
        f->pending_result = ref_value(array);
        f->pending_result_valid = 1;
        f->dex_pc += 3;
        return 1;
    }
    case 0x27: /* throw */
        if (!check_register(f, high)) break;
        error_stop(vm, BS_DEX_STOP_JAVA_EXCEPTION,
                   "original throw (exception dispatch needs catch tables)", op);
        vm->stop.exception = bs_dex_reference_type(vm, f->reg[high]);
        return 0;
    case 0x28: return change_pc(vm, f, (int8_t)high, op);
    case 0x29: /* goto/16 */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        return change_pc(vm, f, (int16_t)w[1], op);
    case 0x2a: /* goto/32 */
        if (!checked_words(vm, f, w, 3, op)) return 0;
        return change_pc(vm, f,
                 (int32_t)((uint32_t)w[1] | (uint32_t)w[2] << 16), op);
    case 0x31: { /* cmp-long 23x */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        a = high; b = w[1] & 0xffu;
        unsigned c = w[1] >> 8;
        if (!check_register(f, a) || !check_register(f, b) ||
            !check_register(f, c)) break;
        int64_t lhs = (int64_t)f->reg[b].bits;
        int64_t rhs = (int64_t)f->reg[c].bits;
        f->reg[a] = bs_dex_int(lhs < rhs ? -1 : lhs > rhs ? 1 : 0);
        f->dex_pc += 2;
        return 1;
    }
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
        if (!checked_words(vm, f, w, 2, op)) return 0;
        a = high & 15u; b = high >> 4;
        if (!check_register(f, a) || !check_register(f, b)) break;
        int32_t lhs = (int32_t)f->reg[a].bits;
        int32_t rhs = (int32_t)f->reg[b].bits;
        int take = op == 0x32 ? f->reg[a].bits == f->reg[b].bits :
                   op == 0x33 ? f->reg[a].bits != f->reg[b].bits :
                   op == 0x34 ? lhs < rhs : op == 0x35 ? lhs >= rhs :
                   op == 0x36 ? lhs > rhs : lhs <= rhs;
        if (take) return change_pc(vm, f, (int16_t)w[1], op);
        f->dex_pc += 2;
        return 1;
    }
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
        if (!checked_words(vm, f, w, 2, op)) return 0;
        if (!check_register(f, high)) break;
        int32_t lhs = (int32_t)f->reg[high].bits;
        int take = op == 0x38 ? !f->reg[high].bits :
                   op == 0x39 ? !!f->reg[high].bits :
                   op == 0x3a ? lhs < 0 : op == 0x3b ? lhs >= 0 :
                   op == 0x3c ? lhs > 0 : lhs <= 0;
        if (take) return change_pc(vm, f, (int16_t)w[1], op);
        f->dex_pc += 2;
        return 1;
    }
    default: break;
    }

    if (op >= 0x52 && op <= 0x6d) {
        if (!checked_words(vm, f, w, 2, op)) return 0;
        return field_instruction(vm, f, w, op);
    }
    if ((op >= 0x6e && op <= 0x72) ||
        (op >= 0x74 && op <= 0x78)) {
        if (!checked_words(vm, f, w, 3, op)) return 0;
        return invoke_instruction(vm, f, w, op);
    }
    if (op >= 0x44 && op <= 0x51) {
        if (!checked_words(vm, f, w, 2, op)) return 0;
        return array_instruction(vm, f, w, op);
    }
    if (op >= 0x90 && op <= 0x9a) {
        if (!checked_words(vm, f, w, 2, op)) return 0;
        return arithmetic_instruction(vm, f, w, op);
    }
    if (op >= 0xd8 && op <= 0xe2) { /* 22b: binary int/lit8 */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        a = high; b = w[1] & 0xffu;
        if (!check_register(f, a) || !check_register(f, b)) goto unsupported;
        int32_t lhs = (int32_t)f->reg[b].bits;
        int32_t rhs = (int8_t)(w[1] >> 8);
        int32_t value;
        switch (op) {
        case 0xd8: value = (int32_t)((uint32_t)lhs + (uint32_t)rhs); break;
        case 0xd9: value = (int32_t)((uint32_t)rhs - (uint32_t)lhs); break;
        case 0xda: value = (int32_t)((uint32_t)lhs * (uint32_t)rhs); break;
        case 0xdd: value = lhs & rhs; break;
        case 0xde: value = lhs | rhs; break;
        case 0xdf: value = lhs ^ rhs; break;
        case 0xe0: value = (int32_t)((uint32_t)lhs << (rhs & 31)); break;
        case 0xe1: value = lhs >> (rhs & 31); break;
        case 0xe2: value = (int32_t)((uint32_t)lhs >> (rhs & 31)); break;
        default: goto unsupported;
        }
        f->reg[a] = bs_dex_int(value);
        f->dex_pc += 2;
        return 1;
    }
    if (op >= 0xb0 && op <= 0xcf) { /* 12x: binary/2addr */
        a = high & 15u; b = high >> 4;
        if (!check_register(f, a) || !check_register(f, b)) goto unsupported;
        int32_t lhs = (int32_t)f->reg[a].bits;
        int32_t rhs = (int32_t)f->reg[b].bits;
        int32_t value;
        switch (op) {
        case 0xb0: value = (int32_t)((uint32_t)lhs + (uint32_t)rhs); break;
        case 0xb1: value = (int32_t)((uint32_t)lhs - (uint32_t)rhs); break;
        case 0xb2: value = (int32_t)((uint32_t)lhs * (uint32_t)rhs); break;
        case 0xb5: value = lhs & rhs; break;
        case 0xb6: value = lhs | rhs; break;
        case 0xb7: value = lhs ^ rhs; break;
        default: goto unsupported;
        }
        f->reg[a] = bs_dex_int(value);
        f->dex_pc++;
        return 1;
    }
    if (op >= 0xd0 && op <= 0xd7) { /* 22s: binary int/lit16 */
        if (!checked_words(vm, f, w, 2, op)) return 0;
        a = high & 15u; b = high >> 4;
        if (!check_register(f, a) || !check_register(f, b)) goto unsupported;
        int32_t lhs = (int32_t)f->reg[b].bits;
        int32_t rhs = (int16_t)w[1];
        int32_t value;
        switch (op) {
        case 0xd0: value = (int32_t)((uint32_t)lhs + (uint32_t)rhs); break;
        case 0xd1: value = (int32_t)((uint32_t)rhs - (uint32_t)lhs); break;
        case 0xd2: value = (int32_t)((uint32_t)lhs * (uint32_t)rhs); break;
        case 0xd5: value = lhs & rhs; break;
        case 0xd6: value = lhs | rhs; break;
        case 0xd7: value = lhs ^ rhs; break;
        default: goto unsupported;
        }
        f->reg[a] = bs_dex_int(value);
        f->dex_pc += 2;
        return 1;
    }
unsupported:
    error_stop(vm, BS_DEX_STOP_OPCODE_MISSING,
               "original Dalvik opcode has no interpreter implementation", op);
    return 0;
}

BsDexStop bs_dex_vm_run(BsDexVM *vm, uint32_t instruction_budget) {
    if (!vm || !vm->started || !vm->depth) {
        BsDexStop empty = {0};
        if (vm && vm->stop.kind) return vm->stop;
        empty.kind = BS_DEX_STOP_INVALID;
        empty.detail = "no interpreted Java Activity stack is active";
        return empty;
    }
    /* Host dependencies may become genuinely available between runs. The
     * failing instruction was never consumed, and its Java stack is intact. */
    if (vm->stop.kind != BS_DEX_STOP_INVALID &&
        vm->stop.kind != BS_DEX_STOP_LIMIT &&
        vm->stop.kind != BS_DEX_STOP_HOST_MISSING) return vm->stop;
    vm->stop = (BsDexStop){0};
    uint32_t completed = 0;
    while (completed < instruction_budget && vm->depth) {
        if (vm->instruction_limit >= BS_DEX_MAX_STEPS) {
            error_stop(vm, BS_DEX_STOP_LIMIT,
                       "original DEX instruction budget exhausted", 0);
            break;
        }
        if (vm->initializing_entry_type && vm->depth == 1) {
            int initialized = ensure_class_initialized(
                vm, vm->initializing_entry_type - 1);
            if (initialized == 0) continue;
            if (initialized < 0) {
                error_stop(vm, BS_DEX_STOP_CORRUPT,
                           "original Activity class initialization failed", 0);
                break;
            }
            vm->initializing_entry_type = 0;
        }
        int result = run_instruction(vm);
        if (result == 0 || result == 3) break;
        /* A class initializer was pushed before the awaiting opcode. Only
         * the instructions that truly executed count toward progress. */
        if (result == 2) continue;
        ++completed;
        ++vm->instruction_limit;
    }
    vm->stop.steps = vm->instruction_limit;
    if (vm->stop.kind == BS_DEX_STOP_INVALID) {
        error_stop(vm, BS_DEX_STOP_LIMIT,
                   "per-call instruction budget reached", 0);
        vm->stop.steps = vm->instruction_limit;
    }
    return vm->stop;
}

int bs_dex_vm_start_activity_constructor(BsDexVM *vm) {
    if (!vm || vm->started) return 0;
    uint32_t type;
    if (!descriptor_type(vm, "Lcom/unity3d/player/UnityPlayerGameActivity;",
                         &type) || !vm->class_definition[type]) return 0;
    for (uint32_t method = 0; method < vm->method_count; ++method) {
        if (u16(vm->dex + vm->method_off + method * 8u) != type ||
            !vm->method_code[method] ||
            strcmp(method_name(vm, method), "<init>")) continue;
        char signature[64];
        if (!method_signature(vm, method, signature, sizeof signature) ||
            strcmp(signature, "()V")) continue;
        BsDexValue receiver = ref_value(allocate_ref(vm, type, NULL, 1, NULL));
        if (!receiver.bits || !push_frame(vm, method, &receiver, 1)) return 0;
        vm->activity_under_construction = receiver;
        vm->initializing_entry_type = type + 1;
        vm->started = 1;
        return 1;
    }
    return 0;
}

int bs_dex_vm_start_activity_oncreate(BsDexVM *vm,
                                      void *actual_activity,
                                      BsDexValue saved_state) {
    /* An Android Activity's onCreate is an entry event, not a reentrant
     * method call. Starting it twice, or while the constructor is still
     * active, would replace the VM's real call stack and publish state from
     * an impossible lifecycle. A new Activity needs a distinct VM. */
    if (!vm || !actual_activity || vm->oncreate_started || vm->depth ||
        (saved_state.bits &&
         (saved_state.kind != BS_DEX_REF || !known_ref(vm, saved_state))) ||
        (!saved_state.bits && !is_dex_null(saved_state)))
        return 0;
    BsDexValue activity = vm->activity_under_construction;
    if (vm->started &&
        (!activity.bits || vm->depth || vm->stop.kind != BS_DEX_STOP_RETURNED ||
         bs_dex_host_pointer(vm, activity) != actual_activity))
        return 0;
    if (vm->host.validate_activity(vm->host.user, actual_activity) != 1)
        return 0;
    const char *activity_type = "Lcom/unity3d/player/UnityPlayerGameActivity;";
    if (!activity.bits)
        activity = bs_dex_wrap_platform(vm, actual_activity, activity_type);
    if (!activity.bits) return 0;
    /* Dispatch original onCreate with the attached Activity receiver.
     * A VM-constructed instance keeps its reference and instance fields. */
    uint32_t candidate = UINT32_MAX;
    for (uint32_t m = 0; m < vm->method_count; ++m)
        if (vm->method_code[m] == UINT32_C(0x23794c)) candidate = m;
    if (candidate == UINT32_MAX) return 0;
    BsDexValue args[] = {activity, saved_state};
    if (!push_frame(vm, candidate, args, 2)) return 0;
    vm->activity = activity;
    vm->activity_under_construction = bs_dex_null();
    vm->stop = (BsDexStop){0};
    vm->started = 1;
    vm->oncreate_started = 1;
    return 1;
}

BsDexStop bs_dex_vm_stop(const BsDexVM *vm) {
    return vm ? vm->stop : (BsDexStop){0};
}

static int read_named_instance_field(const BsDexVM *vm, BsDexValue receiver,
                                      const char *owner, const char *name,
                                      const char *descriptor,
                                      BsDexValue *result) {
    BsDexRef *ref = known_ref(vm, receiver);
    if (!ref || !owner || !name || !descriptor || !result) return 0;
    for (uint32_t i = 0; i < vm->field_count; ++i) {
        if (strcmp(field_class(vm, i), owner) ||
            strcmp(field_name(vm, i), name) ||
            strcmp(field_type(vm, i), descriptor)) continue;
        return find_field_value(ref, i, result);
    }
    return 0;
}

int bs_dex_vm_checkpoint_snapshot(BsDexVM *vm,
                                   BsDexCheckpointSnapshot *out) {
    if (!vm || !out || vm->stop.kind != BS_DEX_STOP_CHECKPOINT ||
        !vm->activity_instanceof_observed || !vm->depth) return 0;
    BsDexFrame *base = &vm->frames[vm->depth - 1];
    if (base->code_off != BS_UJP_UNITY_PLAYER_CTOR_CODE ||
        vm->stop.dex_pc != BS_UJP_UNITY_ACTIVITY_WRITE_PC ||
        base->reg_count < 10) return 0;
    BsDexValue player = base->reg[6];
    BsDexValue ctor_context = base->reg[7];
    BsDexRef *instance = known_ref(vm, player);
    if (!instance || !instance->owns_instance ||
        ctor_context.bits != vm->activity.bits ||
        vm->current_context.bits != vm->activity.bits ||
        vm->current_activity.bits != vm->activity.bits) return 0;
    BsDexCheckpointSnapshot snap = {0};
    snap.unity_player = player;
    snap.static_current_context = vm->current_context;
    snap.static_current_activity = vm->current_activity;
    const char *owner = "Lcom/unity3d/player/UnityPlayer;";
    if (!read_named_instance_field(vm, player, owner, "mContext",
                                   "Landroid/content/Context;",
                                   &snap.player_context) ||
        !read_named_instance_field(vm, player, owner, "mActivity",
                                   "Landroid/app/Activity;",
                                   &snap.player_activity) ||
        !read_named_instance_field(vm, player, owner, "mHandler",
                                   "Landroid/os/Handler;", &snap.handler) ||
        !read_named_instance_field(vm, player, owner, "m_UIThread",
                                   "Ljava/lang/Thread;", &snap.ui_thread)) return 0;
    if (snap.player_context.bits != vm->activity.bits ||
        snap.player_activity.bits != vm->activity.bits ||
        !snap.handler.bits || !snap.ui_thread.bits) return 0;
    int found_game_ctor = 0;
    for (size_t i = 0; i < vm->depth; ++i) {
        BsDexFrame *f = &vm->frames[i];
        if (f->code_off != BS_UJP_GAME_PLAYER_CTOR_CODE ||
            f->reg_count < 7) continue;
        if (f->reg[1].bits != player.bits ||
            f->reg[2].bits != vm->activity.bits) return 0;
        snap.frame_layout = f->reg[3];
        snap.surface_view = f->reg[4];
        found_game_ctor = 1;
        break;
    }
    if (!found_game_ctor || snap.frame_layout.kind != BS_DEX_REF ||
        !snap.frame_layout.bits || snap.surface_view.kind != BS_DEX_REF ||
        !snap.surface_view.bits) return 0;
    uint32_t unity_type;
    if (!descriptor_type(vm, owner, &unity_type)) return 0;
    snap.verified_activity_branch = vm->activity_instanceof_observed;
    snap.unity_class_initialized = vm->class_state[unity_type] == 2;
    /* A live instance of UnityPlayerGameActivity necessarily completed its
     * original <clinit> and System.loadLibrary("game") before construction.
     * The host's validate_activity callback explicitly certifies that fact. */
    snap.original_activity_instance_validated = vm->activity.bits != 0;
    if (!snap.unity_class_initialized) return 0;
    *out = snap;
    return 1;
}

int bs_dex_vm_trace_frame(const BsDexVM *vm, size_t index,
                           BsDexTraceFrame *out) {
    if (!vm || !out || index >= vm->depth) return 0;
    const BsDexFrame *f = &vm->frames[index];
    *out = (BsDexTraceFrame){
        .method_id = f->method_index,
        .code_off = f->code_off,
        .dex_pc = f->dex_pc,
        .class_descriptor = method_class(vm, f->method_index),
        .method_name = method_name(vm, f->method_index)
    };
    return 1;
}

BsDexValue bs_dex_vm_current_context(const BsDexVM *vm) {
    return vm ? vm->current_context : bs_dex_null();
}

BsDexValue bs_dex_vm_current_activity(const BsDexVM *vm) {
    return vm ? vm->current_activity : bs_dex_null();
}
