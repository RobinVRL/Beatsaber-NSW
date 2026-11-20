/* Linux AArch64 eventfd/epoll emulation backed by real Switch looper state.
 * A high virtual FD range is private to this adapter; all other descriptors
 * continue through the existing GameActivity pipe/fsdev bridge.
 * SPDX-License-Identifier: MIT
 */
#ifndef BS_BIONIC_EVENT_EPOLL_H
#define BS_BIONIC_EVENT_EPOLL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct __attribute__((packed)) BsBionicEpollEvent {
    uint32_t events;
    uint64_t data;
} BsBionicEpollEvent;

int bs_bionic_eventfd(unsigned initial_value, int flags);
int bs_bionic_eventfd2(unsigned initial_value, int flags);
int bs_bionic_epoll_create(int size);
int bs_bionic_epoll_create1(int flags);
int bs_bionic_epoll_ctl(int epfd, int operation, int fd,
                       const BsBionicEpollEvent *event);
int bs_bionic_epoll_wait(int epfd, BsBionicEpollEvent *events,
                        int maxevents, int timeout_ms);

/* Read/write/close/fcntl/poll entrypoints used for all guest descriptors.
 * They dispatch only this adapter's high-range virtual descriptors internally.
 */
ssize_t bs_bionic_event_read(int fd, void *buffer, size_t count);
ssize_t bs_bionic_event_write(int fd, const void *buffer, size_t count);
int bs_bionic_event_close(int fd);
int bs_bionic_event_fcntl(int fd, int command, ...);
int bs_bionic_event_poll(void *fds, size_t count, int timeout_ms);

/* Only names requiring descriptor dispatch are intercepted before the
 * original platform map. The eventfd/epoll entrypoints are in the additional
 * platform table instead. */
uintptr_t bs_bionic_event_dispatch_symbol(const char *name);

#endif
