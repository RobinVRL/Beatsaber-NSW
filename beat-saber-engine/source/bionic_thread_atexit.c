/* Android/Bionic __cxa_thread_atexit_impl support for original C++ TLS.
 * Native pthread TLS only stores an owned stack of guest destructor records;
 * no Android guest data layout is ever cast to a native pthread object.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_thread_atexit.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct BsThreadExitEntry {
    void (*destructor)(void *);
    void *object;
    void *dso_handle;
    struct BsThreadExitEntry *next;
} BsThreadExitEntry;

static pthread_key_t g_thread_exit_key;
static pthread_once_t g_thread_exit_once = PTHREAD_ONCE_INIT;
static int g_thread_exit_error;
static int g_thread_exit_ready;

static void bs_thread_exit_native_callback(void *value);

static void bs_thread_exit_initialize(void) {
    int rc = pthread_key_create(&g_thread_exit_key,
                                bs_thread_exit_native_callback);
    if (rc) g_thread_exit_error = rc;
    else g_thread_exit_ready = 1;
}

static int bs_thread_exit_ensure_key(void) {
    int rc = pthread_once(&g_thread_exit_once, bs_thread_exit_initialize);
    if (rc) return rc;
    return g_thread_exit_ready ? 0 :
           (g_thread_exit_error ? g_thread_exit_error : EINVAL);
}

int bs_bionic_cxa_thread_atexit_impl(void (*destructor)(void *),
                                     void *object, void *dso_handle) {
    if (!destructor) { errno = EINVAL; return -1; }
    int rc = bs_thread_exit_ensure_key();
    if (rc) { errno = rc; return -1; }
    BsThreadExitEntry *node = malloc(sizeof *node);
    if (!node) { errno = ENOMEM; return -1; }
    node->destructor = destructor;
    node->object = object;
    node->dso_handle = dso_handle;
    node->next = pthread_getspecific(g_thread_exit_key);
    rc = pthread_setspecific(g_thread_exit_key, node);
    if (rc) { free(node); errno = rc; return -1; }
    return 0;
}

void bs_bionic_cxa_thread_finalize(void) {
    if (!g_thread_exit_ready) return;
    /* Pop before each call. A destructor may register another TLS object,
     * which then must be destroyed before the older queued objects. */
    for (;;) {
        BsThreadExitEntry *node = pthread_getspecific(g_thread_exit_key);
        if (!node) return;
        if (pthread_setspecific(g_thread_exit_key, node->next)) abort();
        void (*destructor)(void *) = node->destructor;
        void *object = node->object;
        free(node);
        destructor(object);
    }
}

static void bs_thread_exit_native_callback(void *value) {
    /* pthreads clears the key before calling its destructor. Restore the
     * captured list so a nested registration will be ordered correctly. */
    if (!value) return;
    if (pthread_setspecific(g_thread_exit_key, value)) abort();
    bs_bionic_cxa_thread_finalize();
}
