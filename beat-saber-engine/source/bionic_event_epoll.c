/* Event counters and epoll sets for guest Android AArch64 (Linux ABI).
 * Existing GameActivity pipes and regular fsdev files are observed with the
 * actual bs_poll_wait backend. Unsupported descriptor types fail ENOTSUP.
 * SPDX-License-Identifier: MIT
 */
#include "bionic_event_epoll.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include "android_window_bridge.h"
#else
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
typedef struct { int fd; int16_t events, revents; } BsBionicPollfd;
#endif

enum {
    BS_EFD_SEMAPHORE = 1, BS_EFD_NONBLOCK = 0x800,
    BS_EFD_CLOEXEC = 0x80000,
    BS_EPOLLIN = 0x001, BS_EPOLLPRI = 0x002, BS_EPOLLOUT = 0x004,
    BS_EPOLLERR = 0x008, BS_EPOLLHUP = 0x010, BS_EPOLLRDHUP = 0x2000,
    BS_EPOLLONESHOT = 0x40000000, BS_EPOLLET = 0x80000000u,
    BS_EPOLL_CTL_ADD = 1, BS_EPOLL_CTL_DEL = 2, BS_EPOLL_CTL_MOD = 3,
    BS_F_GETFD = 1, BS_F_SETFD = 2, BS_F_GETFL = 3, BS_F_SETFL = 4
};
#define BS_EVENT_FIRST_FD 0x50000000
#define BS_EVENT_CAP 64
#define BS_EPOLL_CAP 24
#define BS_WATCH_CAP 64
#define BS_POLL_QUANTUM_NS 4000000ULL
#define BS_EVENT_MAX_COUNTER (UINT64_MAX - 1)

typedef struct {
    int fd;
    uint64_t counter;
    uint64_t revision;
    bool semaphore, nonblock, cloexec;
} BsEventCounter;
typedef struct {
    int fd;
    uint32_t mask;
    uint64_t data;
    uint64_t last_revision;
    uint32_t last_ready;
    bool used, armed;
} BsEpollWatch;
typedef struct {
    int fd;
    bool cloexec;
    unsigned cursor;
    BsEpollWatch watches[BS_WATCH_CAP];
} BsEpollSet;

_Static_assert(sizeof(BsBionicEpollEvent) == 12,
               "Android/Linux AArch64 epoll_event is packed");
_Static_assert(offsetof(BsBionicEpollEvent, data) == 4,
               "Android/Linux epoll data offset");
_Static_assert(sizeof(BsBionicPollfd) == 8, "Android/Linux pollfd layout");

static BsEventCounter g_counters[BS_EVENT_CAP];
static BsEpollSet g_sets[BS_EPOLL_CAP];
static int g_next_fd = BS_EVENT_FIRST_FD;
#ifdef __SWITCH__
static Mutex g_lock;
static CondVar g_changed;
static void event_lock(void) { mutexLock(&g_lock); }
static void event_unlock(void) { mutexUnlock(&g_lock); }
static void event_wake(void) { condvarWakeAll(&g_changed); }
static void event_wait(void) { condvarWait(&g_changed, &g_lock); }
static void event_wait_slice(uint64_t ns) {
    (void)condvarWaitTimeout(&g_changed, &g_lock, ns);
}
static uint64_t event_clock(void) { return armTicksToNs(armGetSystemTick()); }
static int native_poll(BsBionicPollfd *fds, size_t count) {
    return bs_poll_wait(fds, (uint32_t)count, 0);
}
static ssize_t native_read(int fd, void *buf, size_t count) {
    return bs_pipe_read(fd, buf, count);
}
static ssize_t native_write(int fd, const void *buf, size_t count) {
    return bs_pipe_write(fd, buf, count);
}
static int native_close(int fd) { return bs_pipe_close(fd); }
static int native_fcntl(int fd, int cmd, int arg) {
    return bs_pipe_fcntl(fd, cmd, arg);
}
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
static void event_lock(void) { pthread_mutex_lock(&g_lock); }
static void event_unlock(void) { pthread_mutex_unlock(&g_lock); }
static void event_wake(void) { pthread_cond_broadcast(&g_changed); }
static void event_wait(void) { pthread_cond_wait(&g_changed, &g_lock); }
static void event_wait_slice(uint64_t ns) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t total = (uint64_t)now.tv_nsec + ns;
    struct timespec deadline = {
        .tv_sec = now.tv_sec + (time_t)(total / 1000000000ULL),
        .tv_nsec = (long)(total % 1000000000ULL)
    };
    (void)pthread_cond_timedwait(&g_changed, &g_lock, &deadline);
}
static uint64_t event_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static int native_poll(BsBionicPollfd *fds, size_t count) {
    return poll((struct pollfd *)fds, (nfds_t)count, 0);
}
static ssize_t native_read(int fd, void *buf, size_t count) {
    return read(fd, buf, count);
}
static ssize_t native_write(int fd, const void *buf, size_t count) {
    return write(fd, buf, count);
}
static int native_close(int fd) { return close(fd); }
static int native_fcntl(int fd, int cmd, int arg) {
    return fcntl(fd, cmd, arg);
}
#endif

static int fd_error(int error) { errno = error; return -1; }
static bool virtual_fd(int fd) { return fd >= BS_EVENT_FIRST_FD; }

static BsEventCounter *counter_locked(int fd) {
    for (size_t i = 0; i < BS_EVENT_CAP; ++i)
        if (g_counters[i].fd == fd) return &g_counters[i];
    return NULL;
}
static BsEpollSet *epoll_locked(int fd) {
    for (size_t i = 0; i < BS_EPOLL_CAP; ++i)
        if (g_sets[i].fd == fd) return &g_sets[i];
    return NULL;
}
static int alloc_fd_locked(void) {
    if (g_next_fd < BS_EVENT_FIRST_FD || g_next_fd == INT_MAX)
        return -1;
    return g_next_fd++;
}

int bs_bionic_eventfd(unsigned initial_value, int flags) {
    if (flags & ~(BS_EFD_SEMAPHORE | BS_EFD_NONBLOCK | BS_EFD_CLOEXEC))
        return fd_error(EINVAL);
    event_lock();
    int result = -1;
    for (size_t i = 0; i < BS_EVENT_CAP; ++i) {
        if (g_counters[i].fd) continue;
        result = alloc_fd_locked();
        if (result > 0) {
            g_counters[i] = (BsEventCounter){
                .fd = result, .counter = initial_value, .revision = 1,
                .semaphore = (flags & BS_EFD_SEMAPHORE) != 0,
                .nonblock = (flags & BS_EFD_NONBLOCK) != 0,
                .cloexec = (flags & BS_EFD_CLOEXEC) != 0
            };
        }
        break;
    }
    event_unlock();
    return result < 0 ? fd_error(EMFILE) : result;
}
int bs_bionic_eventfd2(unsigned initial_value, int flags) {
    return bs_bionic_eventfd(initial_value, flags);
}

int bs_bionic_epoll_create1(int flags) {
    if (flags & ~BS_EFD_CLOEXEC) return fd_error(EINVAL);
    event_lock();
    int result = -1;
    for (size_t i = 0; i < BS_EPOLL_CAP; ++i) {
        if (g_sets[i].fd) continue;
        result = alloc_fd_locked();
        if (result > 0) {
            memset(&g_sets[i], 0, sizeof(g_sets[i]));
            g_sets[i].fd = result;
            g_sets[i].cloexec = (flags & BS_EFD_CLOEXEC) != 0;
        }
        break;
    }
    event_unlock();
    return result < 0 ? fd_error(EMFILE) : result;
}
int bs_bionic_epoll_create(int size) {
    return size <= 0 ? fd_error(EINVAL) : bs_bionic_epoll_create1(0);
}

ssize_t bs_bionic_event_read(int fd, void *buffer, size_t count) {
    if (!virtual_fd(fd)) return native_read(fd, buffer, count);
    if (!buffer) return fd_error(EFAULT);
    if (count != sizeof(uint64_t)) return fd_error(EINVAL);
    event_lock();
    BsEventCounter *counter = counter_locked(fd);
    if (!counter) {
        int error = epoll_locked(fd) ? EINVAL : EBADF;
        event_unlock();
        return fd_error(error);
    }
    while (counter->fd == fd && counter->counter == 0) {
        if (counter->nonblock) {
            event_unlock();
            return fd_error(EAGAIN);
        }
        event_wait();
    }
    if (counter->fd != fd) {
        event_unlock();
        return fd_error(EBADF);
    }
    uint64_t value = counter->semaphore ? 1 : counter->counter;
    counter->counter -= value;
    ++counter->revision;
    memcpy(buffer, &value, sizeof(value));
    event_wake();
    event_unlock();
    return sizeof(value);
}

ssize_t bs_bionic_event_write(int fd, const void *buffer, size_t count) {
    if (!virtual_fd(fd)) return native_write(fd, buffer, count);
    if (!buffer) return fd_error(EFAULT);
    if (count != sizeof(uint64_t)) return fd_error(EINVAL);
    uint64_t value;
    memcpy(&value, buffer, sizeof(value));
    if (value == UINT64_MAX) return fd_error(EINVAL);
    event_lock();
    BsEventCounter *counter = counter_locked(fd);
    if (!counter) {
        int error = epoll_locked(fd) ? EINVAL : EBADF;
        event_unlock();
        return fd_error(error);
    }
    while (counter->fd == fd && value > BS_EVENT_MAX_COUNTER - counter->counter) {
        if (counter->nonblock) {
            event_unlock();
            return fd_error(EAGAIN);
        }
        event_wait();
    }
    if (counter->fd != fd) {
        event_unlock();
        return fd_error(EBADF);
    }
    counter->counter += value;
    ++counter->revision;
    event_wake();
    event_unlock();
    return sizeof(value);
}

int bs_bionic_event_close(int fd) {
    if (!virtual_fd(fd)) return native_close(fd);
    event_lock();
    BsEventCounter *counter = counter_locked(fd);
    BsEpollSet *set = epoll_locked(fd);
    if (!counter && !set) {
        event_unlock();
        return fd_error(EBADF);
    }
    if (counter) counter->fd = 0;
    if (set) {
        set->fd = 0;
        memset(set->watches, 0, sizeof(set->watches));
    }
    for (size_t i = 0; i < BS_EPOLL_CAP; ++i)
        for (size_t j = 0; j < BS_WATCH_CAP; ++j)
            if (g_sets[i].fd && g_sets[i].watches[j].used &&
                g_sets[i].watches[j].fd == fd)
                g_sets[i].watches[j].used = false;
    event_wake();
    event_unlock();
    return 0;
}

int bs_bionic_event_fcntl(int fd, int command, ...) {
    int argument = 0;
    if (command == BS_F_SETFD || command == BS_F_SETFL) {
        va_list args;
        va_start(args, command);
        argument = va_arg(args, int);
        va_end(args);
    }
    if (!virtual_fd(fd)) return native_fcntl(fd, command, argument);
    event_lock();
    BsEventCounter *counter = counter_locked(fd);
    BsEpollSet *set = epoll_locked(fd);
    if (!counter && !set) {
        event_unlock();
        return fd_error(EBADF);
    }
    int result = 0;
    switch (command) {
        case BS_F_GETFL: result = 2 | (counter && counter->nonblock ?
                                                  BS_EFD_NONBLOCK : 0); break;
        case BS_F_GETFD: result = (counter ? counter->cloexec : set->cloexec)
                                  ? 1 : 0; break;
        case BS_F_SETFL:
            if (!counter) result = fd_error(EINVAL);
            else counter->nonblock = (argument & BS_EFD_NONBLOCK) != 0;
            break;
        case BS_F_SETFD:
            if (counter) counter->cloexec = (argument & 1) != 0;
            else set->cloexec = (argument & 1) != 0;
            break;
        default: result = fd_error(ENOTSUP); break;
    }
    event_wake();
    event_unlock();
    return result;
}

/* Sample native GameActivity pipes and fsdev files through the same backend
 * that drives ALooper/poll. The timeout is zero to avoid holding our lock
 * while the Android pipe backend blocks on its own condition variable. */
static int sample_native_locked(int fd, uint32_t interests, uint32_t *ready) {
    BsBionicPollfd pollfd = {
        .fd = fd,
        .events = (int16_t)(interests & 0xffffu),
        .revents = 0
    };
    if (native_poll(&pollfd, 1) < 0) {
        if (errno == ENOSYS) errno = ENOTSUP;
        return -1;
    }
    if (pollfd.revents & 0x20) return fd_error(EBADF);
    *ready = (uint16_t)pollfd.revents;
    return 0;
}

static bool has_native_watches(const BsEpollSet *set) {
    for (size_t i = 0; i < BS_WATCH_CAP; ++i)
        if (set->watches[i].used && set->watches[i].armed &&
            !virtual_fd(set->watches[i].fd))
            return true;
    return false;
}

int bs_bionic_epoll_ctl(int epfd, int operation, int fd,
                       const BsBionicEpollEvent *event) {
    if (operation != BS_EPOLL_CTL_ADD && operation != BS_EPOLL_CTL_DEL &&
        operation != BS_EPOLL_CTL_MOD) return fd_error(EINVAL);
    if (operation != BS_EPOLL_CTL_DEL && !event) return fd_error(EFAULT);
    if (fd == epfd) return fd_error(EINVAL);
    uint32_t mask = 0;
    uint64_t data = 0;
    if (event) {
        memcpy(&mask, &event->events, sizeof(mask));
        memcpy(&data, &event->data, sizeof(data));
    }
    const uint32_t allowed = BS_EPOLLIN | BS_EPOLLPRI | BS_EPOLLOUT |
        BS_EPOLLERR | BS_EPOLLHUP | BS_EPOLLRDHUP |
        BS_EPOLLONESHOT | BS_EPOLLET;
    if (operation != BS_EPOLL_CTL_DEL && (mask & ~allowed))
        return fd_error(ENOTSUP);
    event_lock();
    BsEpollSet *set = epoll_locked(epfd);
    if (!set) {
        event_unlock();
        return fd_error(EBADF);
    }
    if (epoll_locked(fd)) {
        event_unlock();
        return fd_error(ELOOP);
    }
    if (!counter_locked(fd)) {
        /* Linux epoll_ctl rejects ordinary regular files and directories:
         * readiness for these through poll() does not make them epollable. */
        if (fd >= 0 && fd < 0x4000) {
            struct stat st;
            if (fstat(fd, &st) < 0) {
                int error = errno;
                event_unlock();
                return fd_error(error);
            }
            if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
                event_unlock();
                return fd_error(EPERM);
            }
        }
        uint32_t ready = 0;
        if (sample_native_locked(fd, mask, &ready) < 0) {
            int error = errno;
            event_unlock();
            return fd_error(error);
        }
        /* Edge-triggering cannot be tracked across write/read operations in
         * the separate GameActivity pipe implementation. Reject it here. */
        if ((mask & BS_EPOLLET) && operation != BS_EPOLL_CTL_DEL) {
            event_unlock();
            return fd_error(ENOTSUP);
        }
    }
    BsEpollWatch *existing = NULL;
    BsEpollWatch *free_watch = NULL;
    for (size_t i = 0; i < BS_WATCH_CAP; ++i) {
        BsEpollWatch *w = &set->watches[i];
        if (w->used && w->fd == fd) existing = w;
        if (!w->used && !free_watch) free_watch = w;
    }
    int result = 0;
    if (operation == BS_EPOLL_CTL_DEL) {
        if (!existing) result = fd_error(ENOENT);
        else memset(existing, 0, sizeof(*existing));
    } else if (operation == BS_EPOLL_CTL_ADD) {
        if (existing) result = fd_error(EEXIST);
        else if (!free_watch) result = fd_error(ENOSPC);
        else *free_watch = (BsEpollWatch){
            .fd = fd, .mask = mask, .data = data, .used = true, .armed = true
        };
    } else {
        if (!existing) result = fd_error(ENOENT);
        else {
            existing->mask = mask;
            existing->data = data;
            existing->last_ready = 0;
            existing->last_revision = 0;
            existing->armed = true;
        }
    }
    event_wake();
    event_unlock();
    return result;
}

static uint32_t sample_watch_locked(BsEpollWatch *watch,
                                    uint64_t *revision) {
    BsEventCounter *counter = counter_locked(watch->fd);
    if (counter) {
        uint32_t ready = 0;
        if (counter->counter) ready |= BS_EPOLLIN;
        if (counter->counter < BS_EVENT_MAX_COUNTER) ready |= BS_EPOLLOUT;
        *revision = counter->revision;
        return ready;
    }
    if (virtual_fd(watch->fd)) {
        watch->used = false;
        return 0;
    }
    uint32_t ready = 0;
    if (sample_native_locked(watch->fd, watch->mask, &ready) < 0) {
        /* A closed native descriptor is no longer a valid epoll target. */
        if (errno == EBADF) watch->used = false;
        return 0;
    }
    return ready;
}

static int collect_epoll_locked(BsEpollSet *set,
                                BsBionicEpollEvent *events, int capacity) {
    int total = 0;
    unsigned start = set->cursor % BS_WATCH_CAP;
    for (unsigned n = 0; n < BS_WATCH_CAP && total < capacity; ++n) {
        unsigned i = (start + n) % BS_WATCH_CAP;
        BsEpollWatch *watch = &set->watches[i];
        if (!watch->used || !watch->armed) continue;
        uint64_t revision = 0;
        uint32_t ready = sample_watch_locked(watch, &revision);
        uint32_t matched = ready & (watch->mask | BS_EPOLLERR | BS_EPOLLHUP);
        bool edge = (watch->mask & BS_EPOLLET) != 0;
        bool changed = (matched & ~watch->last_ready) != 0;
        if (edge && counter_locked(watch->fd) &&
            matched && revision != watch->last_revision)
            changed = true;
        watch->last_ready = matched;
        watch->last_revision = revision;
        if (!matched || (edge && !changed)) continue;
        BsBionicEpollEvent result = {.events = matched, .data = watch->data};
        memcpy(&events[total++], &result, sizeof(result));
        if (watch->mask & BS_EPOLLONESHOT) watch->armed = false;
        set->cursor = (i + 1) % BS_WATCH_CAP;
    }
    return total;
}

int bs_bionic_epoll_wait(int epfd, BsBionicEpollEvent *events,
                        int maxevents, int timeout_ms) {
    if (maxevents <= 0 || timeout_ms < -1) return fd_error(EINVAL);
    if (!events) return fd_error(EFAULT);
    const uint64_t start = event_clock();
    const uint64_t budget = timeout_ms < 0 ? UINT64_MAX :
                            (uint64_t)timeout_ms * 1000000ULL;
    event_lock();
    for (;;) {
        BsEpollSet *set = epoll_locked(epfd);
        if (!set) {
            event_unlock();
            return fd_error(EBADF);
        }
        int ready = collect_epoll_locked(set, events, maxevents);
        if (ready || timeout_ms == 0) {
            event_unlock();
            return ready;
        }
        uint64_t elapsed = event_clock() - start;
        if (elapsed >= budget) {
            event_unlock();
            return 0;
        }
        uint64_t rest = budget == UINT64_MAX ? BS_POLL_QUANTUM_NS :
                        budget - elapsed;
        /* A pure eventfd set sleeps until a real counter/control change.
         * Native GameActivity pipes need bounded polling because their
         * condition variable is private to the existing ALooper backend. */
        if (!has_native_watches(set) && budget == UINT64_MAX)
            event_wait();
        else if (!has_native_watches(set))
            event_wait_slice(rest);
        else
            event_wait_slice(rest < BS_POLL_QUANTUM_NS ? rest :
                             BS_POLL_QUANTUM_NS);
    }
}

int bs_bionic_event_poll(void *opaque, size_t count, int timeout_ms) {
    if (count > 1024) return fd_error(EINVAL);
    if (count && !opaque) return fd_error(EFAULT);
    if (timeout_ms < -1) return fd_error(EINVAL);
    BsBionicPollfd *fds = opaque;
    bool has_event = false;
    for (size_t i = 0; i < count; ++i)
        if (virtual_fd(fds[i].fd)) has_event = true;
    if (!has_event) {
#ifdef __SWITCH__
        return bs_poll_wait(fds, (uint32_t)count,
            timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms * 1000000ULL);
#else
        return poll((struct pollfd *)fds, (nfds_t)count, timeout_ms);
#endif
    }
    const uint64_t start = event_clock();
    const uint64_t budget = timeout_ms < 0 ? UINT64_MAX :
                            (uint64_t)timeout_ms * 1000000ULL;
    event_lock();
    for (;;) {
        int found = 0;
        for (size_t i = 0; i < count; ++i) {
            BsBionicPollfd *entry = &fds[i];
            entry->revents = 0;
            if (entry->fd < 0) continue;
            uint32_t ready = 0;
            if (virtual_fd(entry->fd)) {
                BsEventCounter *counter = counter_locked(entry->fd);
                if (!counter) ready = 0x20; /* POLLNVAL */
                else {
                    if (counter->counter) ready |= BS_EPOLLIN;
                    if (counter->counter < BS_EVENT_MAX_COUNTER)
                        ready |= BS_EPOLLOUT;
                }
            } else if (sample_native_locked(entry->fd, entry->events, &ready) < 0) {
                if (errno == EBADF) ready = 0x20;
                else {
                    int error = errno;
                    event_unlock();
                    return fd_error(error);
                }
            }
            entry->revents = (int16_t)(ready &
                       ((uint16_t)entry->events | BS_EPOLLERR |
                        BS_EPOLLHUP | 0x20));
            found += entry->revents != 0;
        }
        if (found || timeout_ms == 0) {
            event_unlock();
            return found;
        }
        uint64_t elapsed = event_clock() - start;
        if (elapsed >= budget) {
            event_unlock();
            return 0;
        }
        uint64_t rest = budget == UINT64_MAX ? BS_POLL_QUANTUM_NS :
                        budget - elapsed;
        bool has_native = false;
        for (size_t i = 0; i < count; ++i)
            if (fds[i].fd >= 0 && !virtual_fd(fds[i].fd))
                has_native = true;
        if (!has_native && budget == UINT64_MAX)
            event_wait();
        else if (!has_native)
            event_wait_slice(rest);
        else
            event_wait_slice(rest < BS_POLL_QUANTUM_NS ? rest :
                             BS_POLL_QUANTUM_NS);
    }
}

uintptr_t bs_bionic_event_dispatch_symbol(const char *name) {
    if (!name) return 0;
    static const struct { const char *name; uintptr_t address; } table[] = {
        {"read", (uintptr_t)&bs_bionic_event_read},
        {"write", (uintptr_t)&bs_bionic_event_write},
        {"close", (uintptr_t)&bs_bionic_event_close},
        {"fcntl", (uintptr_t)&bs_bionic_event_fcntl},
        {"poll", (uintptr_t)&bs_bionic_event_poll}
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if (strcmp(name, table[i].name) == 0) return table[i].address;
    return 0;
}
