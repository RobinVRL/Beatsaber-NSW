/* Host tests for the original Android eventfd/epoll ABI. Uses real threads
 * and POSIX pipe FDs; the Switch build uses ALooper's bs_poll_wait instead.
 * gcc -std=gnu11 -Wall -Wextra -Werror -pthread -Isource
 *     tests/test_bionic_event_epoll.c source/bionic_event_epoll.c -o /tmp/test_event
 */
#include "bionic_event_epoll.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    EP_IN = 1, EP_OUT = 4, EP_ET = 0x80000000u,
    EP_ONESHOT = 0x40000000, ADD = 1, DEL = 2, MOD = 3
};

static void test_counter_and_poll(void) {
    errno = 0;
    assert(bs_bionic_eventfd(0, 0x400) == -1 && errno == EINVAL);
    int fd = bs_bionic_eventfd(0, 0x800 | 0x80000);
    assert(fd >= 0x50000000);
    uint64_t value = 0;
    errno = 0;
    assert(bs_bionic_event_read(fd, &value, 8) == -1 && errno == EAGAIN);
    errno = 0;
    assert(bs_bionic_event_read(fd, &value, 4) == -1 && errno == EINVAL);
    assert(bs_bionic_event_fcntl(fd, 3) & 0x800);
    assert(bs_bionic_event_fcntl(fd, 1) == 1);
    value = 7;
    assert(bs_bionic_event_write(fd, &value, 8) == 8);
    struct { int fd; int16_t events, revents; } p = {fd, EP_IN, 0};
    assert(bs_bionic_event_poll(&p, 1, 0) == 1 && (p.revents & EP_IN));
    value = 0;
    assert(bs_bionic_event_read(fd, &value, 8) == 8 && value == 7);
    assert(bs_bionic_event_poll(&p, 1, 0) == 0);
    assert(bs_bionic_event_close(fd) == 0);
    errno = 0;
    assert(bs_bionic_event_close(fd) == -1 && errno == EBADF);

    int semaphore = bs_bionic_eventfd(2, 1 | 0x800);
    assert(semaphore >= 0);
    assert(bs_bionic_event_read(semaphore, &value, 8) == 8 && value == 1);
    assert(bs_bionic_event_read(semaphore, &value, 8) == 8 && value == 1);
    errno = 0;
    assert(bs_bionic_event_read(semaphore, &value, 8) == -1 && errno == EAGAIN);
    value = UINT64_MAX;
    errno = 0;
    assert(bs_bionic_event_write(semaphore, &value, 8) == -1 && errno == EINVAL);
    assert(bs_bionic_event_close(semaphore) == 0);
}

static void test_epoll(void) {
    errno = 0;
    assert(bs_bionic_epoll_create(0) == -1 && errno == EINVAL);
    int epfd = bs_bionic_epoll_create1(0x80000);
    int fd = bs_bionic_eventfd(0, 0x800);
    assert(epfd >= 0 && fd >= 0);
    BsBionicEpollEvent in = {.events = EP_IN, .data = UINT64_C(0x123456789abcdef0)};
    BsBionicEpollEvent events[4] = {{0}};
    assert(bs_bionic_epoll_ctl(epfd, ADD, fd, &in) == 0);
    errno = 0;
    assert(bs_bionic_epoll_ctl(epfd, ADD, fd, &in) == -1 && errno == EEXIST);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 0);
    uint64_t value = 3;
    assert(bs_bionic_event_write(fd, &value, 8) == 8);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 20) == 1);
    assert(events[0].events == EP_IN && events[0].data == in.data);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 1);

    in.events = EP_IN | EP_ONESHOT;
    assert(bs_bionic_epoll_ctl(epfd, MOD, fd, &in) == 0);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 1);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 0);
    assert(bs_bionic_epoll_ctl(epfd, MOD, fd, &in) == 0);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 1);

    in.events = EP_IN | EP_ET;
    assert(bs_bionic_epoll_ctl(epfd, MOD, fd, &in) == 0);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 1);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 0);
    value = 1;
    assert(bs_bionic_event_write(fd, &value, 8) == 8);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 1);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 0);

    assert(bs_bionic_epoll_ctl(epfd, DEL, fd, NULL) == 0);
    assert(bs_bionic_epoll_wait(epfd, events, 4, 0) == 0);
    errno = 0;
    assert(bs_bionic_epoll_ctl(epfd, DEL, fd, NULL) == -1 && errno == ENOENT);
    assert(bs_bionic_event_close(fd) == 0);
    assert(bs_bionic_event_close(epfd) == 0);
}

static void test_native_pipe_watch(void) {
    int pipes[2];
    assert(pipe(pipes) == 0);
    int epfd = bs_bionic_epoll_create(1);
    assert(epfd > 0);
    BsBionicEpollEvent in = {.events = EP_IN, .data = 19};
    assert(bs_bionic_epoll_ctl(epfd, ADD, pipes[0], &in) == 0);
    BsBionicEpollEvent ready = {0};
    assert(bs_bionic_epoll_wait(epfd, &ready, 1, 0) == 0);
    assert(bs_bionic_event_write(pipes[1], "h", 1) == 1);
    assert(bs_bionic_epoll_wait(epfd, &ready, 1, 20) == 1);
    assert(ready.data == 19 && ready.events == EP_IN);
    char ch = 0;
    assert(bs_bionic_event_read(pipes[0], &ch, 1) == 1 && ch == 'h');
    assert(bs_bionic_epoll_wait(epfd, &ready, 1, 0) == 0);
    in.events |= EP_ET;
    errno = 0;
    assert(bs_bionic_epoll_ctl(epfd, MOD, pipes[0], &in) == -1 &&
           errno == ENOTSUP);
    assert(bs_bionic_event_close(pipes[0]) == 0);
    assert(bs_bionic_event_close(pipes[1]) == 0);
    assert(bs_bionic_event_close(epfd) == 0);
}

typedef struct { int fd; uint64_t observed; int result; } WaitCase;
static void *blocking_reader(void *ctx) {
    WaitCase *test = ctx;
    test->result = (int)bs_bionic_event_read(test->fd, &test->observed, 8);
    return NULL;
}
static void *blocking_epoll(void *ctx) {
    WaitCase *test = ctx;
    BsBionicEpollEvent observed = {0};
    test->result = bs_bionic_epoll_wait(test->fd, &observed, 1, 500);
    test->observed = observed.data;
    return NULL;
}
static void test_concurrent_wakeup(void) {
    int fd = bs_bionic_eventfd(0, 0);
    WaitCase reader = {.fd = fd};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, blocking_reader, &reader) == 0);
    struct timespec delay = {.tv_nsec = 10000000};
    nanosleep(&delay, NULL);
    uint64_t value = 10;
    assert(bs_bionic_event_write(fd, &value, 8) == 8);
    assert(pthread_join(worker, NULL) == 0);
    assert(reader.result == 8 && reader.observed == 10);

    int epfd = bs_bionic_epoll_create(1);
    BsBionicEpollEvent in = {.events = EP_IN, .data = 0xface};
    assert(bs_bionic_epoll_ctl(epfd, ADD, fd, &in) == 0);
    WaitCase wait = {.fd = epfd};
    assert(pthread_create(&worker, NULL, blocking_epoll, &wait) == 0);
    nanosleep(&delay, NULL);
    value = 1;
    assert(bs_bionic_event_write(fd, &value, 8) == 8);
    assert(pthread_join(worker, NULL) == 0);
    assert(wait.result == 1 && wait.observed == in.data);
    assert(bs_bionic_event_close(fd) == 0);
    assert(bs_bionic_epoll_wait(epfd, &in, 1, 0) == 0);
    assert(bs_bionic_event_close(epfd) == 0);
}

int main(void) {
    test_counter_and_poll();
    test_epoll();
    test_native_pipe_watch();
    test_concurrent_wakeup();
    puts("Bionic eventfd/epoll host tests passed");
    return 0;
}
