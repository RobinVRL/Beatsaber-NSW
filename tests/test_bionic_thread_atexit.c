#include "../source/bionic_thread_atexit.h"
#include <assert.h>
#include <pthread.h>
#include <stddef.h>

static int events[16];
static int count;

static void record(void *argument) {
    int value = *(int *)argument;
    events[count++] = value;
    if (value == 2) {
        static int nested = 4;
        assert(bs_bionic_cxa_thread_atexit_impl(record, &nested, NULL) == 0);
    }
}

static void *run_thread(void *argument) {
    static int one = 1, two = 2, three = 3;
    (void)argument;
    assert(bs_bionic_cxa_thread_atexit_impl(record, &one, NULL) == 0);
    assert(bs_bionic_cxa_thread_atexit_impl(record, &two, NULL) == 0);
    assert(bs_bionic_cxa_thread_atexit_impl(record, &three, NULL) == 0);
    /* Simulate the guest pthread dispatcher draining C++ TLS first. */
    bs_bionic_cxa_thread_finalize();
    assert(count == 4);
    assert(events[0] == 3 && events[1] == 2 &&
           events[2] == 4 && events[3] == 1);
    return NULL;
}

int main(void) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(count == 4); /* Native TLS callback must not run entries twice. */
    return 0;
}
