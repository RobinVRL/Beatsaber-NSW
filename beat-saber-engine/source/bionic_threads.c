/* Android Bionic -> libnx/newlib threading ABI compatibility.
 * Adapted from the MIT-licensed thread strategy in pvz_fusion_en_nx, with
 * bounded pointer state, POSIX blocking semantics and keyed futex wakeups.
 */
#include <switch.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <stdatomic.h>

#include "bionic_threads.h"
#include "thread_sampler.h"
#include "util.h"
#include "guest_tls.h"
#include "bionic_thread_atexit.h"
#include "block_trace.h"
#include "bionic_errno.h"
#include "bionic_time_bridge.h"

static Mutex g_adapter_lock;

/* Bionic AArch64 mutexes and condition variables occupy four bytes, so
 * storing an eight-byte libnx pointer in a guest object corrupts memory. */
#define BS_HANDLES_MAX 4095u
#define BS_MUTEX_TOKEN 0xa1000000u
#define BS_COND_TOKEN  0xa2000000u
#define BS_RW_TOKEN    0xa3000000u
#define BS_SEM_TOKEN   0xa4000000u
typedef struct BsHandle {
    void *object;
    uint32_t generation;
} BsHandle;
static BsHandle g_mutex_handles[BS_HANDLES_MAX];
static BsHandle g_cond_handles[BS_HANDLES_MAX];
static BsHandle g_rw_handles[BS_HANDLES_MAX];
static BsHandle g_sem_handles[BS_HANDLES_MAX];

static void *lookup_handle_locked(BsHandle *table, uint32_t kind,
                                   uint32_t token) {
    if ((token & 0xff000000u) != kind) return NULL;
    uint32_t index = token & 0xfffu;
    if (!index || index > BS_HANDLES_MAX) return NULL;
    BsHandle *entry = &table[index - 1];
    if ((token & 0xfff000u) != (entry->generation << 12))
        return NULL;
    return entry->object;
}
static int register_handle_locked(BsHandle *table, uint32_t kind,
                                  void *object, void *guest_slot) {
    for (size_t i = 0; i < BS_HANDLES_MAX; ++i) {
        if (table[i].object) continue;
        uint32_t generation = (table[i].generation + 1) & 0xfffu;
        if (!generation) generation = 1;
        table[i].generation = generation;
        table[i].object = object;
        *(uint32_t *)guest_slot = kind | (generation << 12) | (uint32_t)(i+1);
        return 0;
    }
    return EAGAIN;
}
static void unregister_handle_locked(BsHandle *table, uint32_t token) {
    table[(token & 0xfffu)-1].object = NULL;
}

/* Guest mutexes are plain native mutexes plus explicit owner/recursion
 * state. newlib's pthread_cond_wait cannot release a native recursive
 * mutex (it fails immediately, turning guest wait loops into spins while
 * the lock stays held), so recursion is tracked here and fully unwound
 * around every condition wait. */
typedef struct BsMutex {
    pthread_mutex_t native;
    int type;                 /* 0 normal, 1 recursive, 2 errorcheck */
    _Atomic uintptr_t owner;  /* TLS base of the owning thread, 0 if free */
    uint32_t count;
} BsMutex;

static inline uintptr_t bs_self(void) { return (uintptr_t)armGetTls(); }

static int make_mutex(void *slot, int type) {
    if (!slot) return EINVAL;
    BsMutex *mutex = calloc(1, sizeof(*mutex));
    if (!mutex) return ENOMEM;
    int rc = pthread_mutex_init(&mutex->native, NULL);
    if (rc) { free(mutex); return rc; }
    mutex->type = type;
    rc = register_handle_locked(g_mutex_handles, BS_MUTEX_TOKEN, mutex, slot);
    if (rc) { pthread_mutex_destroy(&mutex->native); free(mutex); }
    return rc;
}

static int find_mutex(void **slot, BsMutex **native, bool create) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    int rc = 0;
    uint32_t state = *(uint32_t *)slot;
    if (state < 0x10000 && create) {
        if (state & 0x2000) rc = ENOTSUP; /* process shared */
        else rc = make_mutex(slot, (state & 0xc000u) >> 14);
        state = *(uint32_t *)slot;
    }
    if (!rc) {
        *native=lookup_handle_locked(g_mutex_handles,BS_MUTEX_TOKEN,state);
        if (!*native) rc = EINVAL;
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}

int bs_mutex_init(void **slot, const int *attr) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    if (lookup_handle_locked(g_mutex_handles, BS_MUTEX_TOKEN,
                             *(uint32_t *)slot)) {
        mutexUnlock(&g_adapter_lock);
        return EBUSY;
    }
    int type = attr ? (int)*(const intptr_t *)attr : 0;
    int rc = type >= 0 && type <= 2 ? make_mutex(slot, type) : EINVAL;
    mutexUnlock(&g_adapter_lock);
    return rc;
}

int bs_mutex_destroy(void **slot) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    uint32_t state = *(uint32_t *)slot;
    BsMutex *mutex=lookup_handle_locked(g_mutex_handles,BS_MUTEX_TOKEN,state);
    if (!mutex) {
        mutexUnlock(&g_adapter_lock);
        return state < 0x10000 ? 0 : EINVAL;
    }
    if (atomic_load(&mutex->owner)) {
        mutexUnlock(&g_adapter_lock);
        return bs_errno_to_bionic(EBUSY);
    }
    int rc = pthread_mutex_destroy(&mutex->native);
    if (!rc) {
        unregister_handle_locked(g_mutex_handles,state);
        *(uint32_t *)slot=0;
        free(mutex);
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}

/* Returns -1 when the native lock must still be taken. */
static int mutex_relock(BsMutex *mutex) {
    if (mutex->type == 0 || atomic_load(&mutex->owner) != bs_self()) return -1;
    if (mutex->type == 2) return EDEADLK;
    if (mutex->count == UINT32_MAX) return EAGAIN;
    ++mutex->count;
    return 0;
}
static void mutex_acquired(BsMutex *mutex) {
    atomic_store(&mutex->owner, bs_self());
    mutex->count = 1;
}

int bs_mutex_lock(void **slot) {
    BsMutex *mutex=NULL;
    int rc = find_mutex(slot,&mutex,true);
    if (rc) return bs_errno_to_bionic(rc);
    rc = mutex_relock(mutex);
    if (rc >= 0) return bs_errno_to_bionic(rc);
    /* Only a contended acquisition is recorded for the watchdog. */
    rc = pthread_mutex_trylock(&mutex->native);
    if (rc == EBUSY) {
        unsigned cookie = bs_block_enter(BS_BLOCK_MUTEX, slot,
                                         __builtin_return_address(0));
        rc = pthread_mutex_lock(&mutex->native);
        bs_block_leave(cookie);
    }
    if (!rc) {
        mutex_acquired(mutex);
        bs_block_note_owner(slot, __builtin_return_address(0));
    }
    return bs_errno_to_bionic(rc);
}
int bs_mutex_trylock(void **slot) {
    BsMutex *mutex=NULL;
    int rc = find_mutex(slot,&mutex,true);
    if (rc) return bs_errno_to_bionic(rc);
    rc = mutex_relock(mutex);
    if (rc == EDEADLK) rc = EBUSY;
    if (rc >= 0) return bs_errno_to_bionic(rc);
    rc = pthread_mutex_trylock(&mutex->native);
    if (!rc) {
        mutex_acquired(mutex);
        bs_block_note_owner(slot, __builtin_return_address(0));
    }
    return bs_errno_to_bionic(rc);
}
int bs_mutex_unlock(void **slot) {
    BsMutex *mutex=NULL;
    int rc = find_mutex(slot,&mutex,false);
    if (rc) return bs_errno_to_bionic(rc);
    if (atomic_load(&mutex->owner) != bs_self()) {
        /* Bionic normal mutexes tolerate foreign unlocks; others fail. */
        if (mutex->type != 0) return bs_errno_to_bionic(EPERM);
    } else if (--mutex->count > 0) {
        return 0;
    }
    mutex->count = 0;
    atomic_store(&mutex->owner, 0);
    rc = pthread_mutex_unlock(&mutex->native);
    if (!rc) bs_block_clear_owner(slot);
    return bs_errno_to_bionic(rc);
}int bs_mutexattr_init(intptr_t *attr) {
    if (!attr) return EINVAL;
    *attr = 0;
    return 0;
}
int bs_mutexattr_destroy(intptr_t *attr) { return attr ? 0 : EINVAL; }
int bs_mutexattr_settype(intptr_t *attr, int type) {
    if (!attr) return EINVAL;
    if (type < 0 || type > 2) return EINVAL;
    *attr = type;
    return 0;
}

/* Per-handle clock of each bridged condition variable. libnx exposes no
 * pthread_condattr_setclock, so every native cond waits on CLOCK_REALTIME
 * and a guest CLOCK_MONOTONIC deadline is converted at each timed wait. */
static bool g_cond_monotonic[BS_HANDLES_MAX];

/* linux_clock is the Bionic/Linux clock ID (0 realtime, 1 monotonic) used
 * only when the cond is created lazily or explicitly here. */
static int find_cond(void **slot, pthread_cond_t **native, bool create,
                     int linux_clock, bool *monotonic) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    int rc = 0;
    uint32_t state=*(uint32_t *)slot;
    if (state < 0x10000 && create) {
        pthread_cond_t *object = calloc(1, sizeof(*object));
        if (!object) rc = ENOMEM;
        else {
            rc=pthread_cond_init(object,NULL);
            if (!rc) {
                rc=register_handle_locked(g_cond_handles,BS_COND_TOKEN,
                                          object,slot);
                if (rc) pthread_cond_destroy(object);
            }
            if (rc) free(object);
            state=*(uint32_t *)slot;
            if (!rc)
                g_cond_monotonic[(state & 0xfffu)-1] =
                    linux_clock == BS_LINUX_CLOCK_MONOTONIC;
        }
    }
    if (!rc) {
        *native=lookup_handle_locked(g_cond_handles,BS_COND_TOKEN,state);
        if (!*native) rc=EINVAL;
        else if (monotonic) *monotonic=g_cond_monotonic[(state & 0xfffu)-1];
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}

int bs_cond_init(void **slot, const int *attr) {
    if (!slot) return EINVAL;
    pthread_cond_t *native=NULL;
    /* Bionic pthread_condattr_t is a long; ours holds the Linux clock ID. */
    int clockid=attr ? (int)*(const intptr_t *)attr : BS_LINUX_CLOCK_REALTIME;
    if (clockid!=BS_LINUX_CLOCK_REALTIME && clockid!=BS_LINUX_CLOCK_MONOTONIC)
        return EINVAL;
    mutexLock(&g_adapter_lock);
    bool exists=lookup_handle_locked(g_cond_handles,BS_COND_TOKEN,
                                    *(uint32_t *)slot)!=NULL;
    mutexUnlock(&g_adapter_lock);
    if (exists) return EBUSY;
    /* init may target uninitialized storage (stack garbage): start clean so
     * the lazy-creation path below always allocates a fresh handle. */
    *(uint32_t *)slot = 0;
    return find_cond(slot,&native,true,clockid,NULL);
}
int bs_cond_destroy(void **slot) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    uint32_t state=*(uint32_t *)slot;
    pthread_cond_t *native=lookup_handle_locked(g_cond_handles,
                                                BS_COND_TOKEN,state);
    if (!native) {
        mutexUnlock(&g_adapter_lock);
        return state < 0x10000 ? 0 : EINVAL;
    }
    int rc = pthread_cond_destroy(native);
    if (!rc) {
        unregister_handle_locked(g_cond_handles,state);
        *(uint32_t *)slot=0;
        free(native);
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}
int bs_cond_signal(void **slot) {
    pthread_cond_t *native=NULL;
    int rc=find_cond(slot,&native,true,BS_LINUX_CLOCK_REALTIME,NULL);
    return rc ? rc : pthread_cond_signal(native);
}
int bs_cond_broadcast(void **slot) {
    pthread_cond_t *native=NULL;
    int rc=find_cond(slot,&native,true,BS_LINUX_CLOCK_REALTIME,NULL);
    return rc ? rc : pthread_cond_broadcast(native);
}
/* Guest-facing: results use Bionic errno numbers (ETIMEDOUT=110). */
int bs_cond_wait(void **cond, void **mutex) {
    pthread_cond_t *cv=NULL;
    BsMutex *mtx=NULL;
    int rc=find_cond(cond,&cv,true,BS_LINUX_CLOCK_REALTIME,NULL);
    if (!rc) rc=find_mutex(mutex,&mtx,true);
    if (rc) return bs_errno_to_bionic(rc);
    unsigned cookie = bs_block_enter(BS_BLOCK_COND_WAIT, cond,
                                     __builtin_return_address(0));
    bs_block_clear_owner(mutex);
    uint32_t depth = mtx->count;
    mtx->count = 0;
    atomic_store(&mtx->owner, 0);
    rc = pthread_cond_wait(cv,&mtx->native);
    atomic_store(&mtx->owner, bs_self());
    mtx->count = depth ? depth : 1;
    bs_block_note_owner(mutex, __builtin_return_address(0));
    bs_block_leave(cookie);
    return bs_errno_to_bionic(rc);
}
int bs_cond_timedwait(void **cond, void **mutex, const struct timespec *abs) {
    if (!abs || abs->tv_sec<0 || abs->tv_nsec<0 ||
        abs->tv_nsec>=1000000000L) return EINVAL;
    pthread_cond_t *cv=NULL;
    BsMutex *mtx=NULL;
    bool monotonic=false;
    int rc=find_cond(cond,&cv,true,BS_LINUX_CLOCK_REALTIME,&monotonic);
    if (!rc) rc=find_mutex(mutex,&mtx,true);
    if (rc) return bs_errno_to_bionic(rc);
    struct timespec deadline=*abs;
    if (monotonic)
        bs_host_deadline_convert(CLOCK_MONOTONIC,abs,CLOCK_REALTIME,&deadline);
    unsigned cookie = bs_block_enter(BS_BLOCK_COND_TIMEDWAIT, cond,
                                     __builtin_return_address(0));
    bs_block_clear_owner(mutex);
    uint32_t depth = mtx->count;
    mtx->count = 0;
    atomic_store(&mtx->owner, 0);
    rc = pthread_cond_timedwait(cv,&mtx->native,&deadline);
    atomic_store(&mtx->owner, bs_self());
    mtx->count = depth ? depth : 1;
    bs_block_note_owner(mutex, __builtin_return_address(0));
    bs_block_leave(cookie);
    return bs_errno_to_bionic(rc);
}
int bs_condattr_init(intptr_t *attr) {
    if (!attr) return EINVAL;
    *attr = BS_LINUX_CLOCK_REALTIME;
    return 0;
}
int bs_condattr_destroy(intptr_t *attr) { return attr ? 0 : EINVAL; }
/* clockid is a Linux clock ID; Bionic accepts only REALTIME/MONOTONIC. */
int bs_condattr_setclock(intptr_t *attr, int clockid) {
    if (!attr || (clockid != BS_LINUX_CLOCK_REALTIME &&
                  clockid != BS_LINUX_CLOCK_MONOTONIC))
        return EINVAL;
    *attr = clockid;
    return 0;
}
static int find_rwlock(void **slot, pthread_rwlock_t **native, bool create) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    int rc = 0;
    uint32_t state=*(uint32_t *)slot;
    if (state < 0x10000 && create) {
        pthread_rwlock_t *object = calloc(1, sizeof(*object));
        if (!object) rc = ENOMEM;
        else {
            rc = pthread_rwlock_init(object, NULL);
            if (!rc) {
                rc=register_handle_locked(g_rw_handles,BS_RW_TOKEN,object,slot);
                if (rc) pthread_rwlock_destroy(object);
            }
            if (rc) free(object);
            state=*(uint32_t *)slot;
        }
    }
    if (!rc) {
        *native=lookup_handle_locked(g_rw_handles,BS_RW_TOKEN,state);
        if (!*native) rc=EINVAL;
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}
int bs_rwlock_init(void **slot, const void *attr) {
    (void)attr;
    if (!slot) return EINVAL;
    pthread_rwlock_t *native=NULL;
    *(uint32_t *)slot = 0; /* storage may be uninitialized */
    return find_rwlock(slot,&native,true);
}
int bs_rwlock_rdlock(void **slot) {
    pthread_rwlock_t *native=NULL;
    int rc=find_rwlock(slot,&native,true);
    return rc ? rc : pthread_rwlock_rdlock(native);
}
int bs_rwlock_wrlock(void **slot) {
    pthread_rwlock_t *native=NULL;
    int rc=find_rwlock(slot,&native,true);
    return rc ? rc : pthread_rwlock_wrlock(native);
}
int bs_rwlock_unlock(void **slot) {
    pthread_rwlock_t *native=NULL;
    int rc=find_rwlock(slot,&native,false);
    return rc ? rc : pthread_rwlock_unlock(native);
}
int bs_rwlock_destroy(void **slot) {
    if (!slot) return EINVAL;
    mutexLock(&g_adapter_lock);
    uint32_t state=*(uint32_t *)slot;
    pthread_rwlock_t *native=lookup_handle_locked(g_rw_handles,
                                                  BS_RW_TOKEN,state);
    if (!native) {
        mutexUnlock(&g_adapter_lock);
        return state < 0x10000 ? 0 : EINVAL;
    }
    int rc = pthread_rwlock_destroy(native);
    if (!rc) {
        unregister_handle_locked(g_rw_handles,state);
        *(uint32_t *)slot=0;
        free(native);
    }
    mutexUnlock(&g_adapter_lock);
    return rc;
}

/* Bionic's AArch64 pthread_attr_t fields are public ABI, unlike newlib's.
 * The initial 40 bytes are fixed: flags, padding, stack_base, stack_size,
 * guard_size, sched_policy and sched_priority. Recent Bionic releases append
 * reserved bytes, which these functions intentionally never touch.
 */
#define BS_ATTR_DETACHED 1u
#define BS_DEFAULT_THREAD_STACK (1024u * 1024u)
#define BS_MIN_THREAD_STACK (2048u * 1024u)
typedef struct BsAttr {
    uint32_t flags;
    uint32_t reserved_pad;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} BsAttr;
_Static_assert(sizeof(BsAttr) == 40, "Bionic AArch64 pthread_attr ABI");
_Static_assert(offsetof(BsAttr, stack_base) == 8, "Bionic stack_base offset");
_Static_assert(offsetof(BsAttr, stack_size) == 16, "Bionic stack_size offset");
int bs_thread_attr_init(void *opaque) {
    if (!opaque) return EINVAL;
    BsAttr *a = opaque;
    *a = (BsAttr){.stack_size=BS_DEFAULT_THREAD_STACK,
                  .guard_size=4096};
    return 0;
}
int bs_thread_attr_destroy(void *opaque) {
    if (!opaque) return EINVAL;
    memset(opaque, 0x42, sizeof(BsAttr));
    return 0;
}
int bs_thread_attr_setdetachstate(void *opaque, int state) {
    BsAttr *a = opaque;
    if (!a || (state != PTHREAD_CREATE_JOINABLE &&
               state != PTHREAD_CREATE_DETACHED)) return EINVAL;
    if (state == PTHREAD_CREATE_DETACHED) a->flags |= BS_ATTR_DETACHED;
    else a->flags &= ~BS_ATTR_DETACHED;
    return 0;
}
int bs_thread_attr_setstacksize(void *opaque, size_t size) {
    BsAttr *a = opaque;
    if (!a || size < 16384 || size > SIZE_MAX - 4095) return EINVAL;
    a->stack_size = size;
    return 0;
}
int bs_thread_attr_getstack(const void *opaque, void **stack, size_t *size) {
    const BsAttr *a = opaque;
    if (!a || !stack || !size) return EINVAL;
    *stack = a->stack_base;
    *size = a->stack_size;
    return 0;
}

/* IL2CPP queries its own worker stack to locate GC roots. libnx owns the
 * current thread's mapped stack; only this case has a validated address.
 * Threads not represented by threadGetSelf() fail explicitly. */
int bs_thread_getattr_np(pthread_t thread, void *opaque) {
    if (!opaque) return EINVAL;
    if (!pthread_equal(thread,pthread_self())) return ENOTSUP;
    Thread *current=threadGetSelf();
    if (!current || !current->stack_mirror || !current->stack_sz)
        return ENOTSUP;
    BsAttr *attr=(BsAttr *)opaque;
    *attr=(BsAttr){
        .stack_base=current->stack_mirror,
        .stack_size=current->stack_sz,
        .guard_size=4096,
    };
    return 0;
}

int bs_bionic_tls_finalize_current_thread(void) {
    if (!bs_guest_tls_is_thread_attached()) return ENOTSUP;
    uintptr_t *tp=(uintptr_t *)armGetTlsRw();
    if (!tp || ((uintptr_t)tp & 15)) return EINVAL;
    /* Android AArch64 TLS_SLOT_STACK_GUARD == 5, TP + 0x28. */
    if (!tp[5]) {
        /* libnx randomGet64 uses OS-seeded ChaCha. A deterministic constant
         * derived only from TP and thread ID is not a stack-protector canary. */
        uintptr_t guard=(uintptr_t)randomGet64();
        if (!guard) guard=0x52f984c2a10e761bull ^ (uintptr_t)tp ^
                          ((uintptr_t)(uint32_t)bs_gettid() << 32);
        tp[5]=guard ? guard : (uintptr_t)0xa5c3d17bULL;
    }
    return 0;
}

typedef struct BsStart {
    void *(*fn)(void *);
    void *arg;
    void *native_tp;
    Mutex start_mutex;
    CondVar start_cond;
    bool ready;
    bool acknowledged;
    bool tracked;
    int status;
    uint8_t tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
} BsStart;

static int bs_track_guest_thread(BsStart *start);

static void *bs_thread_start(void *opaque) {
    BsStart *start = opaque;
    start->native_tp=armGetTlsRw();
    int status=bs_track_guest_thread(start);
    if (!status) {
        if (!bs_guest_tls_install_current_thread(start->tls) ||
            !bs_guest_tls_is_thread_attached()) status=ENOTSUP;
        else status=bs_bionic_tls_finalize_current_thread();
    }
    mutexLock(&start->start_mutex);
    start->status=status;
    start->ready=true;
    condvarWakeOne(&start->start_cond);
    while (!start->acknowledged)
        condvarWait(&start->start_cond,&start->start_mutex);
    mutexUnlock(&start->start_mutex);
    if (status) {
        debugPrintf("[tls] refusing guest start: bootstrap error %d\n",status);
        if (!start->tracked) free(start);
        return NULL;
    }
    bs_sampler_register_current(bs_gettid());
    void *result = start->fn(start->arg);
    bs_sampler_unregister_current();
    return result;
}
int bs_thread_create(pthread_t *out, const void *opaque_attr,
                     void *(*fn)(void *), void *arg) {
    if (!out || !fn) return EINVAL;
    if (!bs_guest_tls_is_ready()) return ENOTSUP;
    BsStart *start = calloc(1, sizeof(*start));
    if (!start) return ENOMEM;
    start->fn = fn;
    start->arg = arg;
    pthread_attr_t native;
    int rc = pthread_attr_init(&native);
    if (rc) { free(start); return rc; }
    const BsAttr *attr = opaque_attr;
    if (!attr) rc = pthread_attr_setstacksize(&native, BS_MIN_THREAD_STACK);
    if (rc) { pthread_attr_destroy(&native); free(start); return rc; }
    if (attr) {
        if (attr->stack_base) {
            /* Explicit caller stacks require a compatible newlib mapping.
             * Do not silently create a thread on the wrong stack. */
            pthread_attr_destroy(&native);
            free(start);
            return ENOTSUP;
        }
        /* Android main/render threads get MiB-scale stacks; Mesa's shader
         * compiler runs on Unity's render thread. Never go below 2 MiB. */
        size_t size = attr->stack_size;
        if (size < BS_MIN_THREAD_STACK) size = BS_MIN_THREAD_STACK;
        rc = pthread_attr_setstacksize(&native, size);
        if (!rc && (attr->flags & BS_ATTR_DETACHED))
            rc = pthread_attr_setdetachstate(&native, PTHREAD_CREATE_DETACHED);
        if (rc) {
            pthread_attr_destroy(&native);
            free(start);
            return rc;
        }
    }
    rc = pthread_create(out, &native, bs_thread_start, start);
    pthread_attr_destroy(&native);
    if (rc) { free(start); return rc; }
    /* Wait for the actual child TP/TLS setup. A successful pthread_create
     * must never conceal a guest TLS installation failure. */
    mutexLock(&start->start_mutex);
    while (!start->ready)
        condvarWait(&start->start_cond,&start->start_mutex);
    rc=start->status;
    start->acknowledged=true;
    condvarWakeOne(&start->start_cond);
    mutexUnlock(&start->start_mutex);
    if (rc && !(opaque_attr && (((const BsAttr *)opaque_attr)->flags &
                               BS_ATTR_DETACHED)))
        pthread_join(*out,NULL);
    return rc;
}
int bs_pthread_once(uint32_t *state, void (*initializer)(void)) {
    if (!state || !initializer) return EINVAL;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(state, &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        initializer();
        __atomic_store_n(state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2)
            svcSleepThread(1000000);
    }
    return 0;
}
int bs_pthread_setname_np(pthread_t thread, const char *name) {
    (void)thread;
    (void)name;
    /* libnx exposes a separate thread naming mechanism; bridge via native
     * Thread handle when a validated pthread_t to Thread map is available.
     */
    return ENOTSUP;
}

/* Linux/Bionic numbers used by IL2CPP's Boehm GC stop-the-world. */
#define BS_SIG_GC_SUSPEND 30 /* SIGPWR */
#define BS_SIG_GC_RESTART 24 /* SIGXCPU */
static _Atomic uint32_t g_gc_suspends, g_gc_restarts, g_gc_unknown_sig;
int bs_pthread_kill(pthread_t thread, int signal_number) {
    /* Horizon has no asynchronous signal delivery, so Boehm's suspend
     * handler can never run on the target thread. Emulate its effect: the
     * suspend signal pauses the target with svcSetThreadActivity and the
     * restart signal resumes it. Both report ESRCH, the only error Boehm
     * tolerates, so GC_stop_world does not wait for a handler ack that can
     * never arrive (GC_DONT_GC=1 keeps stack scanning out of the picture;
     * the stop-world users left are Unity's liveness pass on asset unload,
     * e.g. after a level ends). Nothing here may log or allocate while
     * another thread is paused: it could hold the log/malloc lock. */
    bool self=pthread_equal(thread,pthread_self());
    Handle h=(Handle)bs_sampler_handle_for_pthread((uintptr_t)thread);
    if (signal_number==0) return (self||h) ? 0 : ESRCH;
    if (signal_number==BS_SIG_GC_SUSPEND) {
        if (atomic_load(&g_gc_suspends)==0 && atomic_load(&g_gc_restarts)==0)
            debugPrintf("[gc] first stop-world: emulating SIGPWR with "
                        "svcSetThreadActivity (handle=%#x)\n",(unsigned)h);
        if (!self && h &&
            R_SUCCEEDED(svcSetThreadActivity(h,ThreadActivity_Paused)))
            atomic_fetch_add(&g_gc_suspends,1);
        return ESRCH;
    }
    if (signal_number==BS_SIG_GC_RESTART) {
        if (!self && h) svcSetThreadActivity(h,ThreadActivity_Runnable);
        atomic_fetch_add(&g_gc_restarts,1);
        return ESRCH;
    }
    if (atomic_fetch_add(&g_gc_unknown_sig,1)<8)
        debugPrintf("[gc] pthread_kill(sig=%d) unsupported -> ESRCH\n",
                    signal_number);
    return ESRCH;
}

int bs_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    (void)how; (void)set; (void)oldset;
    return ENOTSUP;
}

/* Android's Bionic has far more pthread TLS keys than the small libnx/newlib
 * thread-key pool. Unity 6 uses dozens of keys for GC, FMOD and JobSystem.
 * Store up to 128 Android key slots inside ONE true pthread TLS key, with
 * POSIX destructor iteration on thread exit. Copied conceptually from the
 * MIT PvZ Fusion Switch wrapper's imports.c.
 */
#define BS_KEY_MAX 128
typedef struct BsKeySlot {
    bool used;
    void (*destructor)(void *);
    uint32_t generation;
} BsKeySlot;
typedef struct BsKeyValues {
    void *value[BS_KEY_MAX];
    uint32_t generation[BS_KEY_MAX];
    BsStart *guest_start;
} BsKeyValues;
static BsKeySlot g_key_slots[BS_KEY_MAX];
static pthread_key_t g_native_master_key;
static bool g_native_key_initialized;
static Mutex g_key_lock;

static void bs_key_destructor(void *pointer) {
    BsKeyValues *values = pointer;
    if (!values) return;
    /* Keep the same multiplexer during destructor iterations. POSIX permits
     * a destructor to re-establish another key's value. */
    pthread_setspecific(g_native_master_key,values);
    for (int iteration=0; iteration<4; ++iteration) {
        bool invoked=false;
        for (int i=0; i<BS_KEY_MAX; ++i) {
            void *v=values->value[i];
            void (*destructor)(void *)=NULL;
            mutexLock(&g_key_lock);
            if (v && g_key_slots[i].used &&
                values->generation[i]==g_key_slots[i].generation)
                destructor=g_key_slots[i].destructor;
            values->value[i]=NULL;
            mutexUnlock(&g_key_lock);
            if (destructor) {
                destructor(v);
                invoked=true;
            }
        }
        if (!invoked) break;
    }
    pthread_setspecific(g_native_master_key,NULL);
    /* Original guest C++ thread_local destructors may still read their own
     * ELF TLS. Drain them while the guest thread pointer is installed. */
    bs_bionic_cxa_thread_finalize();
    if (values->guest_start) {
        BsStart *start=values->guest_start;
        /* All guest pthread_key callbacks have now run with the guest TP.
         * The dedicated ELF TLS provider releases its thread module data. */
        bs_guest_tls_detach_thread();
        armSetTlsRw(start->native_tp);
        free(start);
    }
    free(values);
}

static int bs_ensure_master_key_locked(void) {
    if (g_native_key_initialized) return 0;
    int rc=pthread_key_create(&g_native_master_key,bs_key_destructor);
    if (!rc) g_native_key_initialized=true;
    return rc;
}

static int bs_track_guest_thread(BsStart *start) {
    mutexLock(&g_key_lock);
    int rc=bs_ensure_master_key_locked();
    mutexUnlock(&g_key_lock);
    if (rc) return rc;
    BsKeyValues *values=pthread_getspecific(g_native_master_key);
    if (!values) {
        values=calloc(1,sizeof(*values));
        if (!values) return ENOMEM;
        rc=pthread_setspecific(g_native_master_key,values);
        if (rc) { free(values); return rc; }
    }
    if (values->guest_start) return EBUSY;
    values->guest_start=start;
    start->tracked=true;
    return 0;
}

int bs_key_create(unsigned *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    mutexLock(&g_key_lock);
    int rc=bs_ensure_master_key_locked();
    if (rc) { mutexUnlock(&g_key_lock); return rc; }
    for (unsigned i=0; i<BS_KEY_MAX; ++i) {
        if (!g_key_slots[i].used) {
            g_key_slots[i].used=true;
            g_key_slots[i].destructor=destructor;
            ++g_key_slots[i].generation;
            if (!g_key_slots[i].generation) ++g_key_slots[i].generation;
            *key=i+1;
            mutexUnlock(&g_key_lock);
            return 0;
        }
    }
    mutexUnlock(&g_key_lock);
    return EAGAIN;
}

int bs_key_delete(unsigned key) {
    if (key==0 || key>BS_KEY_MAX) return EINVAL;
    mutexLock(&g_key_lock);
    if (!g_key_slots[key-1].used) {
        mutexUnlock(&g_key_lock);
        return EINVAL;
    }
    g_key_slots[key-1].used=false;
    g_key_slots[key-1].destructor=NULL;
    mutexUnlock(&g_key_lock);
    return 0;
}

void *bs_key_getspecific(unsigned key) {
    if (key==0 || key>BS_KEY_MAX || !g_native_key_initialized) return NULL;
    BsKeyValues *values=pthread_getspecific(g_native_master_key);
    if (!values) return NULL;
    mutexLock(&g_key_lock);
    void *value=g_key_slots[key-1].used &&
                values->generation[key-1]==g_key_slots[key-1].generation
                ? values->value[key-1] : NULL;
    mutexUnlock(&g_key_lock);
    return value;
}

int bs_key_setspecific(unsigned key, const void *value) {
    if (key==0 || key>BS_KEY_MAX || !g_native_key_initialized) return EINVAL;
    BsKeyValues *values=pthread_getspecific(g_native_master_key);
    if (!values) {
        values=calloc(1,sizeof(*values));
        if (!values) return ENOMEM;
        int rc=pthread_setspecific(g_native_master_key,values);
        if (rc) { free(values); return rc; }
    }
    mutexLock(&g_key_lock);
    if (!g_key_slots[key-1].used) {
        mutexUnlock(&g_key_lock);
        return EINVAL;
    }
    values->value[key-1]=(void *)value;
    values->generation[key-1]=g_key_slots[key-1].generation;
    mutexUnlock(&g_key_lock);
    return 0;
}

/* Android sem_t has an opaque, different layout from newlib. */
typedef struct BsSem {
    Semaphore nx;
} BsSem;
static BsSem *sem_handle(void **slot) {
    if (!slot) return NULL;
    mutexLock(&g_adapter_lock);
    BsSem *s=lookup_handle_locked(g_sem_handles,BS_SEM_TOKEN,
                                  *(uint32_t *)slot);
    mutexUnlock(&g_adapter_lock);
    return s;
}
int bs_sem_init(void **slot, int shared, unsigned initial) {
    if (!slot) { errno = EINVAL; return -1; }
    if (shared) { errno = ENOTSUP; return -1; }
    BsSem *s = calloc(1, sizeof(*s));
    if (!s) { errno = ENOMEM; return -1; }
    semaphoreInit(&s->nx, initial);
    mutexLock(&g_adapter_lock);
    int rc=register_handle_locked(g_sem_handles,BS_SEM_TOKEN,s,slot);
    mutexUnlock(&g_adapter_lock);
    if (rc) { free(s); errno=rc;return -1; }
    return 0;
}
int bs_sem_destroy(void **slot) {
    if (!slot) { errno=EINVAL;return -1; }
    mutexLock(&g_adapter_lock);
    uint32_t token=*(uint32_t *)slot;
    BsSem *s=lookup_handle_locked(g_sem_handles,BS_SEM_TOKEN,token);
    if (s) {
        unregister_handle_locked(g_sem_handles,token);
        *(uint32_t *)slot=0;
    }
    mutexUnlock(&g_adapter_lock);
    if (!s) { errno = EINVAL; return -1; }
    free(s);
    return 0;
}
int bs_sem_post(void **slot) {
    BsSem *s = sem_handle(slot);
    if (!s) { errno = EINVAL; return -1; }
    semaphoreSignal(&s->nx);
    return 0;
}
int bs_sem_wait(void **slot) {
    BsSem *s = sem_handle(slot);
    if (!s) { errno = EINVAL; return -1; }
    if (semaphoreTryWait(&s->nx)) return 0;
    unsigned cookie = bs_block_enter(BS_BLOCK_SEM_WAIT, slot,
                                     __builtin_return_address(0));
    semaphoreWait(&s->nx);
    bs_block_leave(cookie);
    return 0;
}
int bs_sem_timedwait(void **slot, const struct timespec *deadline) {
    BsSem *s = sem_handle(slot);
    if (!s || !deadline) { errno = EINVAL; return -1; }
    while (!semaphoreTryWait(&s->nx)) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
            errno = ETIMEDOUT;
            return -1;
        }
        svcSleepThread(1000000);
    }
    return 0;
}
int bs_sem_getvalue(void **slot, int *out) {
    BsSem *s = sem_handle(slot);
    if (!s || !out) { errno = EINVAL; return -1; }
    mutexLock(&s->nx.mutex);
    *out = s->nx.count > INT_MAX ? INT_MAX : (int)s->nx.count;
    mutexUnlock(&s->nx.mutex);
    return 0;
}

/* Keyed futex waiters: unlike a global condition variable, FUTEX_WAKE
 * signals only waiters sleeping on the requested address. Threads may wake
 * spuriously, as allowed by Linux futex semantics.
 */
#define BS_FUTEX_BUCKETS 128
typedef struct BsFutexWaiter {
    struct BsFutexWaiter *next;
    volatile int32_t *addr;
    uint32_t bitset;
    CondVar cond;
    bool notified;
} BsFutexWaiter;
typedef struct BsFutexBucket {
    Mutex mutex;
    BsFutexWaiter *head;
} BsFutexBucket;
static BsFutexBucket g_futex[BS_FUTEX_BUCKETS];

/* Futex diagnostics read by the watchdog. */
_Atomic unsigned long bs_futex_wait_calls, bs_futex_wake_calls,
    bs_futex_wake_hits, bs_futex_wait_eagain;
volatile uintptr_t bs_futex_last_wake[8];
_Atomic unsigned bs_futex_last_wake_pos;

static BsFutexBucket *futex_bucket(volatile int32_t *addr) {
    uintptr_t key = ((uintptr_t)addr >> 3) ^ ((uintptr_t)addr >> 11);
    return &g_futex[key & (BS_FUTEX_BUCKETS - 1)];
}
/* absolute deadlines are on host clock bsolute_clock (FUTEX_WAIT_BITSET\n * uses CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME is set). */
static long bs_futex_wait(volatile int32_t *addr, int expected,
                          const struct timespec *timeout, bool absolute,
                          clockid_t absolute_clock, uint32_t bitset) {
    if (!addr || ((uintptr_t)addr & 3) || !bitset) {
        errno = EINVAL; return -1;
    }
    BsFutexBucket *bucket = futex_bucket(addr);
    BsFutexWaiter waiter = {.addr=addr, .bitset=bitset};
    ++bs_futex_wait_calls;
    mutexLock(&bucket->mutex);
    if (*addr != expected) {
        ++bs_futex_wait_eagain;
        mutexUnlock(&bucket->mutex);
        errno = EAGAIN;
        return -1;
    }
    waiter.next = bucket->head;
    bucket->head = &waiter;
    u64 ns = UINT64_MAX;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L) {
            bucket->head = waiter.next;
            mutexUnlock(&bucket->mutex);
            errno = EINVAL;
            return -1;
        }
        if ((u64)timeout->tv_sec >
            (UINT64_MAX-(u64)timeout->tv_nsec)/1000000000ull) {
            bucket->head = waiter.next;
            mutexUnlock(&bucket->mutex);
            errno=EINVAL; return -1;
        }
        ns = (u64)timeout->tv_sec * 1000000000ull + (u64)timeout->tv_nsec;
        if (absolute) {
            struct timespec now;
            clock_gettime(absolute_clock, &now);
            u64 now_ns = (u64)now.tv_sec * 1000000000ull + (u64)now.tv_nsec;
            ns = ns > now_ns ? ns - now_ns : 0;
        }
    }
    Result rc = timeout ?
        condvarWaitTimeout(&waiter.cond, &bucket->mutex, ns) :
        (condvarWait(&waiter.cond, &bucket->mutex), 0);
    /* The bucket mutex is held again. Remove from the linked wait queue. */
    BsFutexWaiter **p = &bucket->head;
    while (*p && *p != &waiter) p = &(*p)->next;
    if (*p == &waiter) *p = waiter.next;
    bool woke = waiter.notified;
    mutexUnlock(&bucket->mutex);
    if (!woke && timeout && R_FAILED(rc)) { errno = ETIMEDOUT; return -1; }
    return 0;
}
static long bs_futex_wake(volatile int32_t *addr, int max_wake,
                          uint32_t bitset) {
    if (!addr || ((uintptr_t)addr & 3) || max_wake < 0 || !bitset) {
        errno = EINVAL; return -1;
    }
    ++bs_futex_wake_calls;
    bs_futex_last_wake[bs_futex_last_wake_pos++ & 7] = (uintptr_t)addr;
    BsFutexBucket *bucket = futex_bucket(addr);
    mutexLock(&bucket->mutex);
    int signalled = 0;
    for (BsFutexWaiter *w = bucket->head; w && signalled < max_wake; w = w->next) {
        if (w->addr == addr && !w->notified && (w->bitset & bitset)) {
            w->notified = true;
            condvarWakeOne(&w->cond);
            ++signalled;
        }
    }
    mutexUnlock(&bucket->mutex);
    bs_futex_wake_hits += (unsigned long)signalled;
    return signalled;
}

int bs_gettid(void) {
    u64 tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)))
        return (int)(tid & 0x7fffffff);
    return 0;
}
long bs_linux_syscall(long number, ...) {
    if (number == 178) return bs_gettid(); /* __NR_gettid on AArch64 */
    if (number == 98) { /* __NR_futex */
        va_list ap;
        va_start(ap, number);
        volatile int32_t *addr = va_arg(ap, volatile int32_t *);
        int op = va_arg(ap, int);
        int val = va_arg(ap, int);
        const struct timespec *timeout = NULL;
        uint32_t bitset = UINT32_MAX;
        /* Strip FUTEX_PRIVATE_FLAG (0x80) and FUTEX_CLOCK_REALTIME (0x100). */
        int command = op & 0x7f;
        /* FUTEX_WAKE callers can supply only three arguments. The *_BITSET
         * operations pass (timeout, uaddr2, val3=bitset). */
        if (command == 0 || command == 9 || command == 10)
            timeout = va_arg(ap, const struct timespec *);
        if (command == 9 || command == 10) {
            (void)va_arg(ap, void *);
            bitset = (uint32_t)va_arg(ap, int);
        }
        va_end(ap);
        /* FUTEX_CLOCK_REALTIME (0x100) selects the WAIT_BITSET clock. */
        clockid_t clock = (op & 0x100) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        long result;
        unsigned cookie;
        switch (command) {
            case 0:
            case 9:
                cookie = bs_block_enter(BS_BLOCK_FUTEX, (const void *)addr,
                                        __builtin_return_address(0));
                result = bs_futex_wait(addr, val, timeout, command == 9,
                                       clock, bitset);
                bs_block_leave(cookie);
                return result;
            case 1: return bs_futex_wake(addr, val, UINT32_MAX);
            case 10: return bs_futex_wake(addr, val, bitset);
            default: break;
        }
    }
    errno = ENOSYS;
    return -1;
}
long bs_sysconf(int name) {
    switch (name) {
        case 39: case 40: return 4096; /* Bionic _SC_PAGESIZE */
        case 96: case 97: return 4;    /* Bionic _SC_NPROCESSORS_* */
        case 98: {
            u64 total = 0;
            if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize,
                                       CUR_PROCESS_HANDLE, 0)))
                return (long)(total / 4096);
            break;
        }
        default: break;
    }
    errno = EINVAL;
    return -1;
}
