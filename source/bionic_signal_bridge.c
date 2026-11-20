#include "bionic_signal_bridge.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* These values are the guest Linux ARM64 ABI, including errno values.
 * Horizon has no POSIX signal delivery. The guest mask is thread-local
 * state shared by the two mask APIs and the non-local jump implementation. */
enum { BS_LINUX_EFAULT = 14, BS_LINUX_EINVAL = 22, BS_LINUX_ENOSYS = 38 };
static _Thread_local BsBionicSigset guest_mask;

static int signal_error(int error) {
    errno = error;
    return -1;
}

static uint64_t signal_bit(int number) {
    if (number < 1 || number > 64) return 0;
    return UINT64_C(1) << (number - 1);
}

int bs_bionic_sigemptyset(BsBionicSigset *set) {
    if (!set) return signal_error(BS_LINUX_EFAULT);
    *set = 0;
    return 0;
}

int bs_bionic_sigfillset(BsBionicSigset *set) {
    if (!set) return signal_error(BS_LINUX_EFAULT);
    *set = UINT64_MAX;
    return 0;
}

int bs_bionic_sigaddset(BsBionicSigset *set, int signal_number) {
    if (!set) return signal_error(BS_LINUX_EFAULT);
    uint64_t bit = signal_bit(signal_number);
    if (!bit) return signal_error(BS_LINUX_EINVAL);
    *set |= bit;
    return 0;
}

int bs_bionic_sigdelset(BsBionicSigset *set, int signal_number) {
    if (!set) return signal_error(BS_LINUX_EFAULT);
    uint64_t bit = signal_bit(signal_number);
    if (!bit) return signal_error(BS_LINUX_EINVAL);
    *set &= ~bit;
    return 0;
}

int bs_bionic_pthread_sigmask(int how, const BsBionicSigset *set, BsBionicSigset *old) {
    BsBionicSigset previous = guest_mask;
    if (set) {
        switch (how) {
            case 0: guest_mask |= *set; break; /* SIG_BLOCK */
            case 1: guest_mask &= ~*set; break; /* SIG_UNBLOCK */
            case 2: guest_mask = *set; break; /* SIG_SETMASK */
            default: return BS_LINUX_EINVAL;
        }
        /* SIGKILL and SIGSTOP may never be blocked. */
        guest_mask &= ~(signal_bit(9) | signal_bit(19));
    }
    if (old) *old = previous;
    return 0;
}

int bs_bionic_sigprocmask(int how, const BsBionicSigset *set, BsBionicSigset *old) {
    int error = bs_bionic_pthread_sigmask(how, set, old);
    return error ? signal_error(error) : 0;
}

/* Bionic AArch64 struct sigaction: {handler, sa_mask(8), sa_flags(int),
 * sa_restorer}. Horizon never delivers asynchronous signals, but guests
 * (Boehm GC, crash reporters) install handlers at startup and abort if the
 * install fails. Store the dispositions so query/restore round-trips are
 * exact; delivery is handled explicitly by bs_bionic_signal_handler(). */
typedef struct {
    void *handler;
    uint64_t mask;
    int32_t flags;
    int32_t pad;
    void *restorer;
} BsBionicSigaction;
static BsBionicSigaction g_actions[65];

int bs_bionic_sigaction(int signal_number, const void *action, void *old) {
    if (!signal_bit(signal_number)) return signal_error(BS_LINUX_EINVAL);
    /* SIGKILL and SIGSTOP dispositions cannot be changed. */
    if (action && (signal_number == 9 || signal_number == 19))
        return signal_error(BS_LINUX_EINVAL);
    BsBionicSigaction previous = g_actions[signal_number];
    if (action) {
        BsBionicSigaction next;
        memcpy(&next, action, sizeof next);
        next.pad = 0;
        g_actions[signal_number] = next;
    }
    if (old) memcpy(old, &previous, sizeof previous);
    return 0;
}

void *bs_bionic_signal_handler(int signal_number, int *flags) {
    if (!signal_bit(signal_number)) return NULL;
    if (flags) *flags = g_actions[signal_number].flags;
    return g_actions[signal_number].handler;
}

BsBionicSignalHandler bs_bionic_signal(int signal_number, BsBionicSignalHandler handler) {
    BsBionicSigaction next = {0}, old;
    next.handler = (void *)handler;
    next.flags = 0x10000000; /* SA_RESTART, as bionic's signal() */
    if (bs_bionic_sigaction(signal_number, &next, &old) != 0)
        return (BsBionicSignalHandler)(intptr_t)-1;
    return (BsBionicSignalHandler)old.handler;
}

int bs_bionic_sigaltstack(const void *stack, void *old) {
    (void)stack;
    (void)old;
    return signal_error(BS_LINUX_ENOSYS);
}

int bs_bionic_sigsuspend(const BsBionicSigset *mask) {
    return signal_error(mask ? BS_LINUX_ENOSYS : BS_LINUX_EFAULT);
}

#ifdef __aarch64__
/* Opaque Bionic jmp_buf has 32 64-bit words. This private format fits both
 * jump types and stores the complete PCS nonvolatile context plus Android's
 * platform register x18. These are assembly entries so the saved SP/LR point
 * directly at the guest caller. No host setjmp buffer enters the guest ABI. */
__attribute__((used)) static uint64_t bs_jump_read_mask(void) { return guest_mask; }
__attribute__((used)) static void bs_jump_write_mask(uint64_t mask) { guest_mask = mask; }

__asm__(
    ".pushsection .text.bs_bionic_jump,\"ax\",%progbits\n"
    ".p2align 2\n"
    ".global bs_bionic_setjmp\n"
    ".type bs_bionic_setjmp, %function\n"
    "bs_bionic_setjmp:\n"
    "  mov w1, #1\n"
    "  b bs_bionic_sigsetjmp\n"
    ".size bs_bionic_setjmp, .-bs_bionic_setjmp\n"
    ".global bs_bionic_sigsetjmp\n"
    ".type bs_bionic_sigsetjmp, %function\n"
    "bs_bionic_sigsetjmp:\n"
    "  mov x2, sp\n"
    "  stp x30, x2, [x0, #16]\n"
    "  stp x18, x19, [x0, #32]\n"
    "  stp x20, x21, [x0, #48]\n"
    "  stp x22, x23, [x0, #64]\n"
    "  stp x24, x25, [x0, #80]\n"
    "  stp x26, x27, [x0, #96]\n"
    "  stp x28, x29, [x0, #112]\n"
    "  stp d8, d9, [x0, #128]\n"
    "  stp d10, d11, [x0, #144]\n"
    "  stp d12, d13, [x0, #160]\n"
    "  stp d14, d15, [x0, #176]\n"
    "  stp xzr, xzr, [x0]\n"
    "  cbz w1, 1f\n"
    "  stp x0, x30, [sp, #-16]!\n"
    "  bl bs_jump_read_mask\n"
    "  ldp x2, x30, [sp], #16\n"
    "  mov x1, #1\n"
    "  stp x1, x0, [x2]\n"
    "  ldr x18, [x2, #32]\n"
    "1:\n"
    "  mov w0, #0\n"
    "  ret\n"
    ".size bs_bionic_sigsetjmp, .-bs_bionic_sigsetjmp\n"
    ".global bs_bionic_longjmp\n"
    ".type bs_bionic_longjmp, %function\n"
    "bs_bionic_longjmp:\n"
    "  ldp x2, x3, [x0]\n"
    "  cbz x2, 2f\n"
    "  stp x0, x1, [sp, #-16]!\n"
    "  mov x0, x3\n"
    "  bl bs_jump_write_mask\n"
    "  ldp x0, x1, [sp], #16\n"
    "2:\n"
    "  ldp d8, d9, [x0, #128]\n"
    "  ldp d10, d11, [x0, #144]\n"
    "  ldp d12, d13, [x0, #160]\n"
    "  ldp d14, d15, [x0, #176]\n"
    "  ldp x18, x19, [x0, #32]\n"
    "  ldp x20, x21, [x0, #48]\n"
    "  ldp x22, x23, [x0, #64]\n"
    "  ldp x24, x25, [x0, #80]\n"
    "  ldp x26, x27, [x0, #96]\n"
    "  ldp x28, x29, [x0, #112]\n"
    "  ldp x30, x2, [x0, #16]\n"
    "  mov sp, x2\n"
    "  cmp w1, #0\n"
    "  csinc w0, w1, wzr, ne\n"
    "  ret\n"
    ".size bs_bionic_longjmp, .-bs_bionic_longjmp\n"
    ".popsection\n");
#endif
