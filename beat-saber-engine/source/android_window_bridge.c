/* Switch NWindow and Android NDK looper primitives for Unity's own EGL/GLES
 * path. Adapted from MIT source android_native_unity.c in pvz_fusion_en_nx.
 * No original Beat Saber game data or graphics engine code is compiled here.
 */
#include <switch.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "android_window_bridge.h"
#include "block_trace.h"
#include "android_choreographer.h"
#include "android_sensor_bridge.h"
#include "jni_bridge.h"
#include "util.h"

static u32 g_width = 1280, g_height = 720;
static void refresh_size(void) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        g_width = 1920;
        g_height = 1080;
    } else {
        g_width = 1280;
        g_height = 720;
    }
}
void *bs_window_from_surface(void *jni, void *surface) {
    /* An arbitrary nonnull jobject cannot be treated as the primary Horizon
     * window. The JNI bridge must have issued and verified this specific
     * Surface token for an actual live NWindow first. Return the EXACT native
     * window that backs this token, never an unrelated default NWindow. */
    return bs_jni_backed_surface_nwindow(jni, surface);
}
void bs_window_acquire(void *window) {
    (void)window; /* nwindowGetDefault is applet-owned singleton. */
}
void bs_window_release(void *window) { (void)window; }
int32_t bs_window_get_width(void *window) {
    if (!window || !nwindowIsValid((NWindow *)window)) return 0;
    u32 width=0, height=0;
    if (R_SUCCEEDED(nwindowGetDimensions((NWindow *)window,&width,&height)) && width)
        return width;
    refresh_size();
    return g_width;
}
int32_t bs_window_get_height(void *window) {
    if (!window || !nwindowIsValid((NWindow *)window)) return 0;
    u32 width=0, height=0;
    if (R_SUCCEEDED(nwindowGetDimensions((NWindow *)window,&width,&height)) && height)
        return height;
    refresh_size();
    return g_height;
}
int32_t bs_window_get_format(void *window) {
    return window && nwindowIsValid((NWindow *)window) ? 1 : 0;
}
int32_t bs_window_set_geometry(void *window, int32_t width, int32_t height,
                               int32_t format) {
    (void)format;
    if (!window || !nwindowIsValid((NWindow *)window)) return -EINVAL;
    if (!width || !height) { refresh_size(); width=g_width; height=g_height; }
    if (width <= 0 || height <= 0) return -EINVAL;
    Result rc = nwindowSetDimensions((NWindow *)window, (u32)width,(u32)height);
    if (R_FAILED(rc)) {
        debugPrintf("[window] unable to set geometry %dx%d rc=0x%x\n",
                    width,height,rc);
        return -EINVAL;
    }
    nwindowSetCrop((NWindow *)window,0,0,width,height);
    return 0;
}

/* Android GameActivity uses local command pipes with ALooper. The same
 * condition variable covers both explicit wakeups and pipe state changes.
 */
#define BS_MAX_LOOPERS 64
#define BS_MAX_PIPES 16
#define BS_MAX_WATCHES 16
#define BS_PIPE_CAPACITY 4096
#define BS_PIPE_FD_BASE 0x4000
enum { BS_F_GETFL = 3, BS_F_SETFL = 4, BS_O_NONBLOCK = 0x800 };
enum { BS_EVENT_INPUT = 1, BS_EVENT_ERROR = 4, BS_EVENT_HANGUP = 8, BS_EVENT_INVALID = 16 };
enum { BS_LOOPER_WAKE = -1, BS_LOOPER_CALLBACK = -2, BS_LOOPER_TIMEOUT = -3, BS_LOOPER_ERROR = -4 };

typedef struct BsPipe {
    int read_fd, write_fd;
    bool read_nonblocking, write_nonblocking;
    size_t used;
    uint8_t bytes[BS_PIPE_CAPACITY];
} BsPipe;

typedef int (*BsLooperCallback)(int, int, void *);
typedef struct BsWatch {
    int fd, ident, events;
    BsLooperCallback callback;
    void *data;
} BsWatch;

typedef struct BsLooper {
    u64 owner;
    int references;
    bool used, signalled, allow_non_callbacks;
    BsWatch watches[BS_MAX_WATCHES];
} BsLooper;
static BsLooper g_loopers[BS_MAX_LOOPERS];
static BsPipe g_pipes[BS_MAX_PIPES];
static Mutex g_loopers_lock;
static CondVar g_looper_changed;
static int g_next_pipe_fd = BS_PIPE_FD_BASE;

static BsPipe *find_pipe(int fd) {
    for (size_t i = 0; i < BS_MAX_PIPES; ++i)
        if (g_pipes[i].read_fd == fd || g_pipes[i].write_fd == fd)
            return &g_pipes[i];
    return NULL;
}

static int pipe_error(int value) {
    errno = value;
    return -1;
}

int bs_pipe(int fds[2]) {
    if (!fds) return pipe_error(EFAULT);
    mutexLock(&g_loopers_lock);
    if (g_next_pipe_fd < INT_MAX - 2) {
        for (size_t i = 0; i < BS_MAX_PIPES; ++i) {
            BsPipe *pipe = &g_pipes[i];
            if (pipe->read_fd || pipe->write_fd) continue;
            *pipe = (BsPipe){0};
            fds[0] = pipe->read_fd = g_next_pipe_fd++;
            fds[1] = pipe->write_fd = g_next_pipe_fd++;
            mutexUnlock(&g_loopers_lock);
            return 0;
        }
    }
    mutexUnlock(&g_loopers_lock);
    return pipe_error(EMFILE);
}

int bs_pipe_fcntl(int fd, int command, ...) {
    if (fd < BS_PIPE_FD_BASE) return pipe_error(ENOTSUP);
    mutexLock(&g_loopers_lock);
    BsPipe *pipe = find_pipe(fd);
    int result = 0;
    if (!pipe) {
        result = pipe_error(EBADF);
    } else {
        bool writing = pipe->write_fd == fd;
        bool *nonblocking = writing ? &pipe->write_nonblocking : &pipe->read_nonblocking;
        if (command == BS_F_GETFL) {
            result = (writing ? 1 : 0) | (*nonblocking ? BS_O_NONBLOCK : 0);
        } else if (command == BS_F_SETFL) {
            va_list args;
            va_start(args, command);
            *nonblocking = (va_arg(args, int) & BS_O_NONBLOCK) != 0;
            va_end(args);
            condvarWakeAll(&g_looper_changed);
        } else {
            result = pipe_error(ENOTSUP);
        }
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}

static ssize_t pipe_read_impl(int fd, void *buffer, size_t bytes) {
    if (fd < BS_PIPE_FD_BASE) return read(fd, buffer, bytes);
    mutexLock(&g_loopers_lock);
    ssize_t result;
    for (;;) {
        BsPipe *pipe = find_pipe(fd);
        if (!pipe || pipe->read_fd != fd) { result = pipe_error(EBADF); break; }
        if (!bytes) { result = 0; break; }
        if (!buffer) { result = pipe_error(EFAULT); break; }
        if (pipe->used) {
            size_t count = bytes < pipe->used ? bytes : pipe->used;
            memcpy(buffer, pipe->bytes, count);
            pipe->used -= count;
            memmove(pipe->bytes, pipe->bytes + count, pipe->used);
            condvarWakeAll(&g_looper_changed);
            result = (ssize_t)count;
            break;
        }
        if (!pipe->write_fd) { result = 0; break; }
        if (pipe->read_nonblocking) { result = pipe_error(EAGAIN); break; }
        condvarWait(&g_looper_changed, &g_loopers_lock);
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}

ssize_t bs_pipe_write(int fd, const void *buffer, size_t bytes) {
    if (fd < BS_PIPE_FD_BASE) return write(fd, buffer, bytes);
    mutexLock(&g_loopers_lock);
    ssize_t result;
    for (;;) {
        BsPipe *pipe = find_pipe(fd);
        if (!pipe || pipe->write_fd != fd) { result = pipe_error(EBADF); break; }
        if (!bytes) { result = 0; break; }
        if (!buffer) { result = pipe_error(EFAULT); break; }
        if (!pipe->read_fd) { result = pipe_error(EPIPE); break; }
        size_t room = BS_PIPE_CAPACITY - pipe->used;
        if (room >= bytes || (bytes > BS_PIPE_CAPACITY && room)) {
            size_t count = bytes < room ? bytes : room;
            memcpy(pipe->bytes + pipe->used, buffer, count);
            pipe->used += count;
            condvarWakeAll(&g_looper_changed);
            result = (ssize_t)count;
            break;
        }
        if (pipe->write_nonblocking) { result = pipe_error(EAGAIN); break; }
        condvarWait(&g_looper_changed, &g_loopers_lock);
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}

int bs_pipe_close(int fd) {
    if (fd < BS_PIPE_FD_BASE) return close(fd);
    mutexLock(&g_loopers_lock);
    BsPipe *pipe = find_pipe(fd);
    if (pipe) {
        if (pipe->read_fd == fd) pipe->read_fd = 0;
        else pipe->write_fd = 0;
        for (size_t i = 0; i < BS_MAX_LOOPERS; ++i)
            for (size_t j = 0; j < BS_MAX_WATCHES; ++j)
                if (g_loopers[i].watches[j].fd == fd)
                    g_loopers[i].watches[j].fd = 0;
        condvarWakeAll(&g_looper_changed);
    }
    mutexUnlock(&g_loopers_lock);
    return pipe ? 0 : pipe_error(EBADF);
}

enum {
    BS_POLLIN = 0x001, BS_POLLOUT = 0x004, BS_POLLERR = 0x008,
    BS_POLLHUP = 0x010, BS_POLLNVAL = 0x020, BS_POLLRDNORM = 0x040,
    BS_POLLWRNORM = 0x100, BS_FIONREAD = 0x541b
};

static int pipe_poll_events(const BsPipe *pipe, int fd) {
    if (!pipe) return BS_POLLNVAL;
    if (pipe->read_fd == fd) {
        int events = pipe->used ? BS_POLLIN | BS_POLLRDNORM : 0;
        return events | (pipe->write_fd ? 0 : BS_POLLHUP);
    }
    int events = pipe->used < BS_PIPE_CAPACITY ? BS_POLLOUT | BS_POLLWRNORM : 0;
    return events | (pipe->read_fd ? 0 : BS_POLLERR);
}

static int poll_wait_impl(BsBionicPollfd *fds, uint32_t count, uint64_t timeout_ns) {
    _Static_assert(sizeof(BsBionicPollfd) == 8, "Bionic pollfd ABI");
    if (count > INT_MAX) return pipe_error(EINVAL);
    if (count && !fds) return pipe_error(EFAULT);
    u64 start = armGetSystemTick();
    mutexLock(&g_loopers_lock);
    int result;
    for (;;) {
        result = 0;
        for (uint32_t i = 0; i < count; ++i) {
            BsBionicPollfd *entry = &fds[i];
            entry->revents = 0;
            if (entry->fd < 0) continue;
            int available;
            if (entry->fd >= BS_PIPE_FD_BASE) {
                available = pipe_poll_events(find_pipe(entry->fd), entry->fd);
            } else {
                struct stat st;
                if (fstat(entry->fd, &st) < 0) {
                    available = BS_POLLNVAL;
                } else if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
                    available = BS_POLLIN | BS_POLLRDNORM | BS_POLLOUT | BS_POLLWRNORM;
                } else {
                    /* No notification backend for other devoptab devices. */
                    mutexUnlock(&g_loopers_lock);
                    return pipe_error(38); /* Linux ENOSYS */
                }
            }
            entry->revents = available & (entry->events | BS_POLLERR | BS_POLLHUP | BS_POLLNVAL);
            result += entry->revents != 0;
        }
        if (result) break;
        if (timeout_ns == UINT64_MAX) {
            condvarWait(&g_looper_changed, &g_loopers_lock);
            continue;
        }
        u64 elapsed = armTicksToNs(armGetSystemTick() - start);
        if (elapsed >= timeout_ns) break;
        if (R_FAILED(condvarWaitTimeout(&g_looper_changed, &g_loopers_lock,
                                       timeout_ns - elapsed)))
            break;
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}

int bs_pipe_ioctl(int fd, int request, ...) {
    if (fd < BS_PIPE_FD_BASE) {
        struct stat st;
        if (fstat(fd, &st) < 0) return -1;
        if (request != BS_FIONREAD || !S_ISREG(st.st_mode)) return pipe_error(ENOTTY);
        off_t offset = lseek(fd, 0, SEEK_CUR);
        if (offset < 0) return -1;
        va_list args;
        va_start(args, request);
        int *available = va_arg(args, int *);
        va_end(args);
        if (!available) return pipe_error(EFAULT);
        off_t left = st.st_size > offset ? st.st_size - offset : 0;
        *available = left > INT_MAX ? INT_MAX : (int)left;
        return 0;
    }
    mutexLock(&g_loopers_lock);
    BsPipe *pipe = find_pipe(fd);
    int result = 0;
    if (!pipe) result = pipe_error(EBADF);
    else if (request != BS_FIONREAD) result = pipe_error(ENOTTY);
    else {
        va_list args;
        va_start(args, request);
        int *available = va_arg(args, int *);
        va_end(args);
        if (!available) result = pipe_error(EFAULT);
        else *available = (int)pipe->used;
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}

static u64 current_thread(void) {
    u64 tid=0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    return tid;
}
static BsLooper *find_looper(bool create) {
    u64 tid=current_thread();
    mutexLock(&g_loopers_lock);
    BsLooper *found=NULL;
    for (int i=0;i<BS_MAX_LOOPERS;i++)
        if (g_loopers[i].used && g_loopers[i].owner==tid) {
            found=&g_loopers[i]; break;
        }
    if (!found && create)
        for (int i=0;i<BS_MAX_LOOPERS;i++) if (!g_loopers[i].used) {
            found=&g_loopers[i];
            memset(found,0,sizeof(*found));
            found->used=true;
            found->owner=tid;
            found->references=1;
            break;
        }
    mutexUnlock(&g_loopers_lock);
    return found;
}
void *bs_looper_prepare(int options) {
    BsLooper *looper = find_looper(true);
    if (looper && (options & 1)) {
        mutexLock(&g_loopers_lock);
        looper->allow_non_callbacks = true;
        mutexUnlock(&g_loopers_lock);
    }
    return looper;
}
void *bs_looper_for_thread(void) {
    return find_looper(false);
}
void bs_looper_acquire(void *pointer) {
    BsLooper *looper=pointer;
    if (!looper) return;
    mutexLock(&g_loopers_lock);
    if (looper->used) looper->references++;
    mutexUnlock(&g_loopers_lock);
}
void bs_looper_release(void *pointer) {
    BsLooper *looper=pointer;
    if (!looper) return;
    u64 released_owner = 0;
    mutexLock(&g_loopers_lock);
    if (looper->used && --looper->references<=0) {
        released_owner = looper->owner;
        looper->used=false;
        condvarWakeAll(&g_looper_changed);
    }
    mutexUnlock(&g_loopers_lock);
    if (released_owner) {
        bs_choreographer_release_looper(released_owner);
        bs_android_sensor_release_looper(released_owner);
    }
}
void bs_looper_wake(void *pointer) {
    BsLooper *looper=pointer;
    if (!looper) return;
    mutexLock(&g_loopers_lock);
    if (looper->used) {
        looper->signalled=true;
        condvarWakeAll(&g_looper_changed);
    }
    mutexUnlock(&g_loopers_lock);
}
static u64 g_poll_calls, g_poll_zero_calls, g_poll_last_tid;
void bs_looper_poll_stats(uint64_t *calls, uint64_t *zero_timeout_calls,
                          uint64_t *last_tid) {
    if (calls) *calls = __atomic_load_n(&g_poll_calls, __ATOMIC_RELAXED);
    if (zero_timeout_calls)
        *zero_timeout_calls = __atomic_load_n(&g_poll_zero_calls, __ATOMIC_RELAXED);
    if (last_tid) *last_tid = __atomic_load_n(&g_poll_last_tid, __ATOMIC_RELAXED);
}
static int looper_poll_once_impl(int timeout_ms, int *fd,int *events,void **data) {
    __atomic_add_fetch(&g_poll_calls, 1, __ATOMIC_RELAXED);
    if (!timeout_ms) __atomic_add_fetch(&g_poll_zero_calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_poll_last_tid, current_thread(), __ATOMIC_RELAXED);
    BsLooper *looper=find_looper(false);
    if (fd) *fd=-1;
    if (events) *events=0;
    if (data) *data=NULL;
    if (!looper) return BS_LOOPER_ERROR;
    u64 start = armGetSystemTick();
    mutexLock(&g_loopers_lock);
    int result = BS_LOOPER_ERROR;
    while (looper->used) {
        /* Android's Choreographer callbacks belong to this Looper thread.
         * Dispatch only after a real VI display vsync has been signalled;
         * callbacks may post another callback for the NEXT vsync. */
        mutexUnlock(&g_loopers_lock);
        bs_android_sensor_poll_current();
        int frames_dispatched = bs_choreographer_dispatch_current();
        bool wants_sensor = bs_android_sensor_has_enabled_current();
        mutexLock(&g_loopers_lock);
        if (frames_dispatched > 0) {
            result = BS_LOOPER_CALLBACK;
            break;
        }
        if (looper->signalled) {
            looper->signalled = false;
            result = BS_LOOPER_WAKE;
            break;
        }
        for (size_t i = 0; i < BS_MAX_WATCHES; ++i) {
            BsWatch watch = looper->watches[i];
            if (!watch.fd) continue;
            BsPipe *pipe = find_pipe(watch.fd);
            int ready = !pipe ? BS_EVENT_INVALID :
                ((pipe->used ? BS_EVENT_INPUT : 0) |
                 (pipe->write_fd ? 0 : BS_EVENT_HANGUP));
            ready &= watch.events | BS_EVENT_ERROR | BS_EVENT_HANGUP | BS_EVENT_INVALID;
            if (!ready) continue;
            mutexUnlock(&g_loopers_lock);
            if (watch.callback) {
                static int traced;
                if (traced++ < 40)
                    debugPrintf("[looper] callback fd=%d ready=0x%x bytes=%zu tid=%lu head_what=%d\n",
                                watch.fd, ready, pipe ? (size_t)pipe->used : 0,
                                (unsigned long)current_thread(),
                                pipe && pipe->used >= 12 ? *(const int *)(pipe->bytes + 8) : -1);
                if (!watch.callback(watch.fd, ready, watch.data))
                    bs_looper_remove_fd(looper, watch.fd);
                return BS_LOOPER_CALLBACK;
            }
            {
                /* A watch that is ready on every poll starves a timeout-0
                 * render loop (it only renders on ALOOPER_POLL_TIMEOUT). */
                static int last_fd, repeats, reported;
                repeats = watch.fd == last_fd ? repeats + 1 : 0;
                last_fd = watch.fd;
                if (repeats == 10000 && reported++ < 4)
                    debugPrintf("[looper] fd=%d ident=%d ready=0x%x data=%p "
                                "pipe_bytes=%zu writer=%d returned 10000x\n",
                                watch.fd, watch.ident, ready, watch.data,
                                pipe ? (size_t)pipe->used : 0,
                                pipe ? pipe->write_fd : -1);
            }
            if (fd) *fd = watch.fd;
            if (events) *events = ready;
            if (data) *data = watch.data;
            /* The caller processes a native_app_glue pipe event by reading
             * that same pipe, which takes g_loopers_lock. Ownership was
             * already released above; unlocking again would release a
             * lock now held by another thread (libnx Mutex has no owner
             * check on unlock). */
            return watch.ident;
        }
        /* Poll the VI event while frame callbacks are pending. Other loopers
         * may sleep on a condition variable indefinitely as before. */
        bool wants_vsync = bs_choreographer_has_pending_current();
        /* An enabled physical Joy-Con sensor needs periodic HID sampling
         * even when the Android looper's timeout is infinite. */
        bool needs_probe = wants_vsync || wants_sensor;
        const u64 vsync_probe_ns = 4000000ull;
        if (timeout_ms < 0) {
            if (needs_probe)
                condvarWaitTimeout(&g_looper_changed, &g_loopers_lock,
                                   vsync_probe_ns);
            else
                condvarWait(&g_looper_changed, &g_loopers_lock);
        } else {
            u64 limit = (u64)timeout_ms * 1000000ull;
            u64 elapsed = armTicksToNs(armGetSystemTick() - start);
            if (elapsed >= limit) {
                result = BS_LOOPER_TIMEOUT;
                break;
            }
            u64 remaining = limit - elapsed;
            if (needs_probe && remaining > vsync_probe_ns)
                remaining = vsync_probe_ns;
            Result wait_rc = condvarWaitTimeout(&g_looper_changed,
                                                 &g_loopers_lock, remaining);
            if (R_FAILED(wait_rc) &&
                !needs_probe && remaining == limit - elapsed) {
                result = BS_LOOPER_TIMEOUT;
                break;
            }
        }
    }
    mutexUnlock(&g_loopers_lock);
    return result;
}
int bs_looper_add_fd(void *looper,int fd,int ident,int events,
                     void *callback,void *data) {
    BsLooper *target = looper;
    if (!target || fd < BS_PIPE_FD_BASE || events != BS_EVENT_INPUT)
        return pipe_error(ENOTSUP);
    mutexLock(&g_loopers_lock);
    BsPipe *pipe = find_pipe(fd);
    if (!target->used || !pipe || pipe->read_fd != fd ||
        (!callback && (!target->allow_non_callbacks || ident < 0))) {
        mutexUnlock(&g_loopers_lock);
        return pipe_error(EINVAL);
    }
    BsWatch *entry = NULL;
    for (size_t i = 0; i < BS_MAX_WATCHES; ++i) {
        BsWatch *current = &target->watches[i];
        if (current->fd == fd) { entry = current; break; }
        if (!entry && !current->fd) entry = current;
    }
    if (entry) {
        debugPrintf("[looper] addFd fd=%d ident=%d cb=%p data=%p owner=%lu by tid=%lu\n",
                    fd, ident, callback, data, (unsigned long)target->owner,
                    (unsigned long)current_thread());
        *entry = (BsWatch){fd, ident, events, (BsLooperCallback)callback, data};
        condvarWakeAll(&g_looper_changed);
    }
    mutexUnlock(&g_loopers_lock);
    return entry ? 1 : pipe_error(ENOMEM);
}
int bs_looper_remove_fd(void *looper,int fd) {
    BsLooper *target = looper;
    if (!target || fd < BS_PIPE_FD_BASE) return pipe_error(EINVAL);
    int removed = 0;
    mutexLock(&g_loopers_lock);
    if (target->used) {
        for (size_t i = 0; i < BS_MAX_WATCHES; ++i) {
            if (target->watches[i].fd != fd) continue;
            target->watches[i].fd = 0;
            removed = 1;
            condvarWakeAll(&g_looper_changed);
            break;
        }
    }
    mutexUnlock(&g_loopers_lock);
    return removed;
}


/* Public bridged entry points: record blocking waits for the watchdog. The
 * caller address is the original guest call site. */
ssize_t bs_pipe_read(int fd, void *buffer, size_t bytes) {
    if (fd < BS_PIPE_FD_BASE) {
        return read(fd, buffer, bytes);
    }
    unsigned cookie = bs_block_enter(BS_BLOCK_PIPE_READ, (void *)(intptr_t)fd,
                                     __builtin_return_address(0));
    ssize_t result = pipe_read_impl(fd, buffer, bytes);
    bs_block_leave(cookie);
    return result;
}
int bs_poll_wait(BsBionicPollfd *fds, uint32_t count, uint64_t timeout_ns) {
    unsigned cookie = timeout_ns ?
        bs_block_enter(BS_BLOCK_POLL, fds, __builtin_return_address(0)) : 0;
    int result = poll_wait_impl(fds, count, timeout_ns);
    bs_block_leave(cookie);
    return result;
}
int bs_looper_poll_once(int timeout_ms, int *fd, int *events, void **data) {
    unsigned cookie = timeout_ms ?
        bs_block_enter(BS_BLOCK_LOOPER_POLL, NULL,
                       __builtin_return_address(0)) : 0;
    int result = looper_poll_once_impl(timeout_ms, fd, events, data);
    bs_block_leave(cookie);
    return result;
}