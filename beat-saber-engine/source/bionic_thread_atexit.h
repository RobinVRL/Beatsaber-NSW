/* Bionic's per-thread C++ destructor registration for original Quest ELF.
 * Calls run on the registering thread, before its guest TLS is released.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_THREAD_ATEXIT_H
#define BS_BIONIC_THREAD_ATEXIT_H

int bs_bionic_cxa_thread_atexit_impl(void (*destructor)(void *),
                                     void *object, void *dso_handle);

/* Called by the guest pthread-key dispatcher before it releases the
 * original AArch64 thread-local storage. Safe when nothing is registered.
 * Native pthreads without guest keys are covered by the native TLS callback.
 */
void bs_bionic_cxa_thread_finalize(void);

#endif
