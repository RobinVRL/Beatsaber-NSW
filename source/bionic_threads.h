#ifndef BEAT_SABER_BIONIC_THREADS_H
#define BEAT_SABER_BIONIC_THREADS_H

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>

/* These ABI adapters receive the pointer to the Android pthread type. Bionic
 * stores mutex/cond/rwlock state inline; the Switch newlib implementation
 * has a different layout. A four-byte tagged handle references a native
 * object in the adapter's registry. Never write a native pointer into a
 * guest four-byte pthread_mutex_t or pthread_cond_t.
 */
int bs_mutex_init(void **m, const int *attr);
int bs_mutex_destroy(void **m);
int bs_mutex_lock(void **m);
int bs_mutex_trylock(void **m);
int bs_mutex_unlock(void **m);
int bs_mutexattr_init(intptr_t *attr);
int bs_mutexattr_destroy(intptr_t *attr);
int bs_mutexattr_settype(intptr_t *attr, int type);

int bs_cond_init(void **c, const int *attr);
int bs_cond_destroy(void **c);
int bs_cond_signal(void **c);
int bs_cond_broadcast(void **c);
int bs_cond_wait(void **c, void **m);
int bs_cond_timedwait(void **c, void **m, const struct timespec *abs);
int bs_condattr_init(intptr_t *attr);
int bs_condattr_destroy(intptr_t *attr);
int bs_condattr_setclock(intptr_t *attr, int clockid);

int bs_rwlock_init(void **rw, const void *attr);
int bs_rwlock_rdlock(void **rw);
int bs_rwlock_wrlock(void **rw);
int bs_rwlock_unlock(void **rw);
int bs_rwlock_destroy(void **rw);

int bs_thread_attr_init(void *a);
int bs_thread_attr_destroy(void *a);
int bs_thread_attr_setdetachstate(void *a, int state);
int bs_thread_attr_setstacksize(void *a, size_t size);
int bs_thread_attr_getstack(const void *a, void **stack, size_t *size);
int bs_thread_getattr_np(pthread_t thread, void *a);
/* After the guest_tls PT_TLS template is installed on any thread (including
 * main), populate the AArch64 Bionic canary and validate the active TP. */
int bs_bionic_tls_finalize_current_thread(void);
int bs_thread_create(pthread_t *out, const void *attr,
                     void *(*start)(void *), void *arg);
int bs_pthread_once(uint32_t *state, void (*initializer)(void));
int bs_pthread_setname_np(pthread_t thread, const char *name);
int bs_pthread_kill(pthread_t thread, int signal_number);
int bs_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
int bs_key_create(unsigned *key, void (*destructor)(void *));
int bs_key_delete(unsigned key);
void *bs_key_getspecific(unsigned key);
int bs_key_setspecific(unsigned key, const void *value);

int bs_sem_init(void **slot, int shared, unsigned initial);
int bs_sem_destroy(void **slot);
int bs_sem_post(void **slot);
int bs_sem_wait(void **slot);
int bs_sem_timedwait(void **slot, const struct timespec *deadline);
int bs_sem_getvalue(void **slot, int *out);

long bs_linux_syscall(long number, ...);
int bs_gettid(void);
long bs_sysconf(int name);

#endif
