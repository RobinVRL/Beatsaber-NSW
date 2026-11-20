#ifndef BS_ANDROID_WINDOW_BRIDGE_H
#define BS_ANDROID_WINDOW_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* NDK window glue. The underlying object is a live libnx NWindow, which
 * Nintendo Switch Mesa can accept as an EGLNativeWindowType.
 */
void *bs_window_from_surface(void *jni, void *surface);
void bs_window_acquire(void *window);
void bs_window_release(void *window);
int32_t bs_window_get_width(void *window);
int32_t bs_window_get_height(void *window);
int32_t bs_window_get_format(void *window);
int32_t bs_window_set_geometry(void *window, int32_t w, int32_t h,
                               int32_t format);

void *bs_looper_prepare(int options);
void *bs_looper_for_thread(void);
void bs_looper_acquire(void *looper);
void bs_looper_release(void *looper);
void bs_looper_wake(void *looper);
int bs_looper_poll_once(int timeout_ms, int *fd, int *events, void **data);
/* Diagnostic counters for the scheduler watchdog. */
void bs_looper_poll_stats(uint64_t *calls, uint64_t *zero_timeout_calls,
                          uint64_t *last_tid);
int bs_looper_add_fd(void *looper, int fd, int ident, int events,
                      void *callback, void *data);
int bs_looper_remove_fd(void *looper, int fd);
int bs_pipe(int fds[2]);
int bs_pipe_fcntl(int fd, int command, ...);
ssize_t bs_pipe_read(int fd, void *buffer, size_t bytes);
ssize_t bs_pipe_write(int fd, const void *buffer, size_t bytes);
int bs_pipe_close(int fd);
typedef struct BsBionicPollfd {
    int32_t fd;
    int16_t events, revents;
} BsBionicPollfd;
int bs_poll_wait(BsBionicPollfd *fds, uint32_t count, uint64_t timeout_ns);
int bs_pipe_ioctl(int fd, int request, ...);

#endif
