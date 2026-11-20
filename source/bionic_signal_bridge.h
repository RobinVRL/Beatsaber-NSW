#ifndef BS_BIONIC_SIGNAL_BRIDGE_H
#define BS_BIONIC_SIGNAL_BRIDGE_H

#include <stdint.h>

typedef uint64_t BsBionicSigset;
typedef void (*BsBionicSignalHandler)(int);

int bs_bionic_sigemptyset(BsBionicSigset *set);
int bs_bionic_sigfillset(BsBionicSigset *set);
int bs_bionic_sigaddset(BsBionicSigset *set, int signal_number);
int bs_bionic_sigdelset(BsBionicSigset *set, int signal_number);
int bs_bionic_sigprocmask(int how, const BsBionicSigset *set, BsBionicSigset *old);
int bs_bionic_pthread_sigmask(int how, const BsBionicSigset *set, BsBionicSigset *old);
int bs_bionic_sigaction(int signal_number, const void *action, void *old);
BsBionicSignalHandler bs_bionic_signal(int signal_number, BsBionicSignalHandler handler);
/* Stored guest handler for a signal (NULL = SIG_DFL), with its sa_flags. */
void *bs_bionic_signal_handler(int signal_number, int *flags);
int bs_bionic_sigaltstack(const void *stack, void *old);
int bs_bionic_sigsuspend(const BsBionicSigset *mask);

#ifdef __aarch64__
int bs_bionic_setjmp(void *buffer) __attribute__((returns_twice));
int bs_bionic_sigsetjmp(void *buffer, int save_mask) __attribute__((returns_twice));
void bs_bionic_longjmp(void *buffer, int value) __attribute__((noreturn));
#endif

#endif
