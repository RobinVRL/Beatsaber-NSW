/* Focused libnx-side ABI checks. Call these from a hardware test harness.
 * They do not execute or emulate any Quest game code.
 * SPDX-License-Identifier: MIT
 */
#include <switch.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

#include "../source/bionic_threads.h"
#include "../source/guest_tls.h"
#include "../source/so_util.h"
#include "../source/util.h"

typedef struct GuestAttrPrefix {
    uint32_t flags;
    uint32_t padding;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t policy;
    int32_t priority;
} GuestAttrPrefix;
_Static_assert(sizeof(GuestAttrPrefix)==40, "AArch64 Bionic attr prefix size");
_Static_assert(offsetof(GuestAttrPrefix,stack_base)==8, "Bionic attr stack base");
_Static_assert(offsetof(GuestAttrPrefix,stack_size)==16, "Bionic attr stack size");

typedef struct GuardedWord {
    uint32_t guest_state;
    uint32_t canary;
} GuardedWord;

int bs_test_bionic_thread_abi(void) {
    int failures=0;
    const uint32_t guard=0xcd38a471u;
    GuardedWord mutex={0,guard};
    if (bs_mutex_init((void **)&mutex.guest_state,NULL) ||
        mutex.canary!=guard ||
        bs_mutex_lock((void **)&mutex.guest_state) ||
        bs_mutex_unlock((void **)&mutex.guest_state) ||
        bs_mutex_destroy((void **)&mutex.guest_state) ||
        mutex.canary!=guard) ++failures;

    GuardedWord cond={0,guard};
    if (bs_cond_init((void **)&cond.guest_state,NULL) ||
        cond.canary!=guard ||
        bs_cond_signal((void **)&cond.guest_state) ||
        bs_cond_destroy((void **)&cond.guest_state) ||
        cond.canary!=guard) ++failures;

    GuardedWord sem={0,guard};
    int value=-1;
    if (bs_sem_init((void **)&sem.guest_state,0,0) ||
        sem.canary!=guard ||
        bs_sem_post((void **)&sem.guest_state) ||
        bs_sem_getvalue((void **)&sem.guest_state,&value) ||
        value!=1 ||
        bs_sem_wait((void **)&sem.guest_state) ||
        bs_sem_destroy((void **)&sem.guest_state) ||
        sem.canary!=guard) ++failures;

    struct {
        GuestAttrPrefix prefix;
        uint64_t canary;
    } attr={{0},0x3a9ab2cc47105fe1ULL};
    if (bs_thread_attr_init(&attr.prefix) ||
        attr.canary!=0x3a9ab2cc47105fe1ULL ||
        attr.prefix.stack_size==0 ||
        bs_thread_attr_setdetachstate(&attr.prefix,PTHREAD_CREATE_DETACHED) ||
        !(attr.prefix.flags&1u)) ++failures;
    if (bs_thread_attr_setstacksize(&attr.prefix,0)!=EINVAL) ++failures;
    intptr_t mutexattr=0;
    if (bs_mutexattr_init(&mutexattr) ||
        bs_mutexattr_settype(&mutexattr,1) ||
        mutexattr!=1 ||
        bs_mutexattr_destroy(&mutexattr)) ++failures;
    if (bs_pthread_kill(pthread_self(),0)) ++failures;
    return failures;
}

static void *guest_tls_worker(void *opaque) {
    int *observed=(int *)opaque;
    uintptr_t *tp=(uintptr_t *)armGetTlsRw();
    *observed=(bs_guest_tls_is_thread_attached() && tp &&
               !((uintptr_t)tp&15) && tp[5]) ? 1 : -1;
    return opaque;
}

/* Requires the original modules' PT_TLS registrations to be sealed first. */
int bs_test_bionic_guest_thread_tls(void) {
    if (!bs_guest_tls_is_ready()) return ENOTSUP;
    int result=0;
    pthread_t worker;
    int rc=bs_thread_create(&worker,NULL,guest_tls_worker,&result);
    if (rc) return rc;
    void *retval=NULL;
    rc=pthread_join(worker,&retval);
    if (rc) return rc;
    return result==1 && retval==&result ? 0 : EINVAL;
}

typedef struct GuestAttachCase {
    int result;
    uintptr_t native_tp;
    uintptr_t guest_tp;
    uintptr_t first_module_slot;
    uintptr_t initial_guard;
} GuestAttachCase;

static void *probe_native_guest_attach(void *opaque) {
    GuestAttachCase *probe = (GuestAttachCase *)opaque;
    void *buffer = memalign(16, BIONIC_TLS_SIZE);
    if (!buffer) { probe->result = ENOMEM; return NULL; }
    memset(buffer, 0xA5, BIONIC_TLS_SIZE);
    probe->native_tp = (uintptr_t)armGetTlsRw();
    void *native_ro = armGetTls();

    int rc = bs_guest_tls_attach_thread(buffer);
    if (!rc) {
        uintptr_t *tp = (uintptr_t *)armGetTlsRw();
        probe->guest_tp = (uintptr_t)tp;
        probe->initial_guard = tp[5];
        if (!bs_guest_tls_is_thread_attached() ||
            (uintptr_t)tp != (uintptr_t)buffer + BIONIC_TLS_TP_OFFSET ||
            tp[5] != 0 || armGetTls() != native_ro)
            rc = EINVAL;
        if (!rc && bs_guest_tls_attach_thread(buffer) != EBUSY)
            rc = EINVAL;
        if (!rc && bs_bionic_tls_finalize_current_thread() != 0)
            rc = EINVAL;
        if (!rc && (!tp[5] || bs_bionic_tls_finalize_current_thread()))
            rc = EINVAL;
        if (!rc && bs_guest_tls_module_count()) {
            BsGuestTlsIndex index={1, 0};
            uint8_t *value=bs_guest_tls_get_addr(&index);
            probe->first_module_slot=(uintptr_t)value;
            if (!value || value < (uint8_t *)tp + 0x100 ||
                value >= (uint8_t *)buffer + BIONIC_TLS_SIZE)
                rc = EINVAL;
            index.module_id=bs_guest_tls_module_count()+1;
            if (bs_guest_tls_get_addr(&index)) rc=EINVAL;
        }
        bs_guest_tls_detach_thread();
        if (bs_guest_tls_is_thread_attached() ||
            armGetTlsRw() != (void *)probe->native_tp)
            rc=EINVAL;
        bs_guest_tls_detach_thread(); /* idempotent detach */
    }
    probe->result=rc;
    free(buffer);
    return NULL;
}

/* Run after seal and relocated PT_TLS template snapshot. It uses an actual
 * newlib thread, never invokes the original Quest engine binary, and checks
 * that TPIDR_EL0 changes only for the attach lifetime. */
int bs_test_bionic_guest_attach_detach(void) {
    if (!bs_guest_tls_is_ready()) return ENOTSUP;
    GuestAttachCase probe={0};
    pthread_t thread;
    int rc=pthread_create(&thread,NULL,probe_native_guest_attach,&probe);
    if (rc) return rc;
    rc=pthread_join(thread,NULL);
    return rc ? rc : probe.result;
}

/* Validate a known original Quest GOT descriptor after every library has
 * been relocated and the current thread's TLS and stack guard installed.
 * Examples from the supplied APK:
 *   libopenxr_loader.so     0x75738, TLS offset 24
 *   libOVRPlugin.so        0x5098c8, TLS offset 8
 *   libovrplatformloader.so 0x5f070, TLS offset 0
 * The real original DSO code is never invoked. */
int bs_test_original_quest_tls_descriptor(const so_module *module,
                                          uintptr_t descriptor_vaddr,
                                          size_t variable_offset) {
    if (!bs_guest_tls_is_thread_attached()) return ENOTSUP;
    if (!bs_guest_tls_validate_original_desc(module,descriptor_vaddr,
                                             variable_offset))
        return EINVAL;
    /* Two intentional failures prove that the validator checks the offset
     * and ELF bounds instead of merely accepting a mapped pointer. */
    if (bs_guest_tls_validate_original_desc(module,descriptor_vaddr,
                                            SIZE_MAX))
        return EINVAL;
    if (bs_guest_tls_validate_original_desc(module,SIZE_MAX,
                                            variable_offset))
        return EINVAL;
    /* A reserved load_virtbase is not proof that so_finalize succeeded.
     * Before finalization the dynamic symbol and string tables still point
     * into the staging load_base, whose pages later become inaccessible.
     * These copies must fail before any GOT access. */
    so_module unfinalized = *module;
    unfinalized.syms = (Elf64_Sym *)unfinalized.load_base;
    unfinalized.dynstrtab = (char *)unfinalized.load_base;
    if (bs_guest_tls_validate_original_desc(&unfinalized,descriptor_vaddr,
                                            variable_offset))
        return EINVAL;
    unfinalized = *module;
    unfinalized.load_virtbase = NULL;
    if (bs_guest_tls_validate_original_desc(&unfinalized,descriptor_vaddr,
                                            variable_offset))
        return EINVAL;
    return 0;
}
