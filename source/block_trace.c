#include "block_trace.h"

#include <switch.h>

/* Each thread claims one record per blocking call by CAS on thread_id.
 * Readers may observe a torn record; that is acceptable for diagnostics. */
#define BS_BLOCK_RECORDS 64

typedef struct BsBlockRecord {
    uint64_t thread_id; /* 0 = free */
    int op;
    uintptr_t object;
    uintptr_t caller;
    uint64_t since_tick;
} BsBlockRecord;

static BsBlockRecord g_records[BS_BLOCK_RECORDS];

static uint64_t current_thread_id(void) {
    u64 tid = 0;
    if (R_FAILED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) return 0;
    return tid;
}

unsigned bs_block_enter(int op, const void *object, const void *caller) {
    uint64_t tid = current_thread_id();
    if (!tid) return 0;
    for (unsigned i = 0; i < BS_BLOCK_RECORDS; ++i) {
        uint64_t expected = 0;
        if (!__atomic_compare_exchange_n(&g_records[i].thread_id, &expected,
                                         tid, false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED))
            continue;
        __atomic_store_n(&g_records[i].op, op, __ATOMIC_RELAXED);
        __atomic_store_n(&g_records[i].object, (uintptr_t)object,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_records[i].caller, (uintptr_t)caller,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_records[i].since_tick, armGetSystemTick(),
                         __ATOMIC_RELEASE);
        return i + 1;
    }
    return 0;
}

void bs_block_leave(unsigned cookie) {
    if (!cookie || cookie > BS_BLOCK_RECORDS) return;
    __atomic_store_n(&g_records[cookie - 1].thread_id, 0, __ATOMIC_RELEASE);
}

size_t bs_block_snapshot(BsBlockInfo *out, size_t capacity) {
    size_t count = 0;
    for (unsigned i = 0; i < BS_BLOCK_RECORDS && count < capacity; ++i) {
        uint64_t tid = __atomic_load_n(&g_records[i].thread_id,
                                       __ATOMIC_ACQUIRE);
        if (!tid) continue;
        out[count].thread_id = tid;
        out[count].op = __atomic_load_n(&g_records[i].op, __ATOMIC_RELAXED);
        out[count].object = __atomic_load_n(&g_records[i].object,
                                            __ATOMIC_RELAXED);
        out[count].caller = __atomic_load_n(&g_records[i].caller,
                                            __ATOMIC_RELAXED);
        out[count].since_tick = __atomic_load_n(&g_records[i].since_tick,
                                                __ATOMIC_ACQUIRE);
        ++count;
    }
    return count;
}

#define BS_OWNER_RECORDS 512
typedef struct BsOwnerRecord {
    uintptr_t object;
    uint64_t thread_id;
    uintptr_t caller;
} BsOwnerRecord;
static BsOwnerRecord g_owners[BS_OWNER_RECORDS];

static unsigned owner_index(uintptr_t object) {
    return (unsigned)((object >> 3) * 0x9E3779B1u) % BS_OWNER_RECORDS;
}

void bs_block_note_owner(const void *object, const void *caller) {
    BsOwnerRecord *r = &g_owners[owner_index((uintptr_t)object)];
    __atomic_store_n(&r->thread_id, current_thread_id(), __ATOMIC_RELAXED);
    __atomic_store_n(&r->caller, (uintptr_t)caller, __ATOMIC_RELAXED);
    __atomic_store_n(&r->object, (uintptr_t)object, __ATOMIC_RELEASE);
}

void bs_block_clear_owner(const void *object) {
    BsOwnerRecord *r = &g_owners[owner_index((uintptr_t)object)];
    if (__atomic_load_n(&r->object, __ATOMIC_ACQUIRE) == (uintptr_t)object)
        __atomic_store_n(&r->thread_id, 0, __ATOMIC_RELEASE);
}

int bs_block_lookup_owner(uintptr_t object, uint64_t *thread_id,
                          uintptr_t *caller) {
    BsOwnerRecord *r = &g_owners[owner_index(object)];
    if (__atomic_load_n(&r->object, __ATOMIC_ACQUIRE) != object) return 0;
    *thread_id = __atomic_load_n(&r->thread_id, __ATOMIC_RELAXED);
    *caller = __atomic_load_n(&r->caller, __ATOMIC_RELAXED);
    return 1;
}

const char *bs_block_op_name(int op) {
    static const char *const names[] = {
        "none", "mutex", "cond_wait", "cond_timedwait", "sem_wait",
        "looper_poll", "pipe_read", "pipe_write", "poll", "futex", "join",
    };
    return op >= 0 && (size_t)op < sizeof names / sizeof names[0] ?
           names[op] : "?";
}
