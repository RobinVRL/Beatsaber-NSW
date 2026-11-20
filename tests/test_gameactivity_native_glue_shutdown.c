/* Threaded regression for Quest GameActivity native_app_glue shutdown.
 * Compile on Windows with -DBS_GA_HOST_TEST alongside the real glue source.
 * No Java Activity, Android lifecycle or original guest code is simulated.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../source/gameactivity_native_glue.h"

static DWORD owner_thread;
static int looper_token;
static CRITICAL_SECTION guest_mutex, pipe_mutex;
static int pipe_mutex_initialized;
static uint8_t pipe_bytes[64];
static size_t pipe_start, pipe_count;
static HANDLE entered_mutex, release_mutex;
static volatile LONG stall_foreign, producer_result, state_result, saw_closed_gate;
static BsGameActivityGlue glue;
static int real_config, real_assets;

void *bs_looper_for_thread(void) {
    return owner_thread == GetCurrentThreadId() ? &looper_token : NULL;
}
void *bs_looper_prepare(int options) {
    assert(options == 1 && !owner_thread);
    owner_thread = GetCurrentThreadId();
    return &looper_token;
}
void bs_looper_release(void *looper) {
    assert(looper == &looper_token);
    owner_thread = 0;
}
int bs_looper_add_fd(void *looper, int fd, int ident, int events,
                     void *callback, void *data) {
    assert(looper == &looper_token && fd == 1001);
    assert(ident == 1 && events == 1 && callback == NULL && data);
    return 1;
}
int bs_looper_remove_fd(void *looper, int fd) {
    assert(looper == &looper_token && fd == 1001);
    return 1;
}
int bs_pipe(int fds[2]) {
    if (!pipe_mutex_initialized) {
        InitializeCriticalSection(&pipe_mutex);
        pipe_mutex_initialized = 1;
    }
    assert(!pipe_count);
    fds[0] = 1001;
    fds[1] = 1002;
    return 0;
}
int bs_pipe_fcntl(int fd, int command, ...) {
    assert((fd == 1001 || fd == 1002) && command == 4);
    return 0;
}
typedef intptr_t bs_host_ssize_t;
bs_host_ssize_t bs_pipe_read(int fd, void *buffer, size_t bytes) {
    assert(fd == 1001 && bytes == 1);
    EnterCriticalSection(&pipe_mutex);
    if (!pipe_count) {
        LeaveCriticalSection(&pipe_mutex);
        return -1;
    }
    *(uint8_t *)buffer = pipe_bytes[pipe_start++ % sizeof(pipe_bytes)];
    --pipe_count;
    LeaveCriticalSection(&pipe_mutex);
    return 1;
}
bs_host_ssize_t bs_pipe_write(int fd, const void *buffer, size_t bytes) {
    assert(fd == 1002 && bytes == 1);
    EnterCriticalSection(&pipe_mutex);
    if (pipe_count == sizeof(pipe_bytes)) {
        LeaveCriticalSection(&pipe_mutex);
        return -1;
    }
    pipe_bytes[(pipe_start + pipe_count) % sizeof(pipe_bytes)] =
        *(const uint8_t *)buffer;
    ++pipe_count;
    LeaveCriticalSection(&pipe_mutex);
    return 1;
}
int bs_pipe_close(int fd) {
    assert(fd == 1001 || fd == 1002);
    EnterCriticalSection(&pipe_mutex);
    pipe_count = 0;
    pipe_start = 0;
    LeaveCriticalSection(&pipe_mutex);
    return 0;
}
int bs_mutex_init(void **slot, const int *attr) {
    (void)slot;
    (void)attr;
    InitializeCriticalSection(&guest_mutex);
    return 0;
}
int bs_mutex_destroy(void **slot) {
    (void)slot;
    DeleteCriticalSection(&guest_mutex);
    return 0;
}
int bs_mutex_lock(void **slot) {
    (void)slot;
    if (GetCurrentThreadId() != owner_thread &&
        InterlockedCompareExchange(&stall_foreign, 0, 1) == 1) {
        assert(SetEvent(entered_mutex));
        assert(WaitForSingleObject(release_mutex, 10000) == WAIT_OBJECT_0);
    }
    EnterCriticalSection(&guest_mutex);
    return 0;
}
int bs_mutex_unlock(void **slot) {
    (void)slot;
    LeaveCriticalSection(&guest_mutex);
    return 0;
}
int bs_cond_init(void **slot, const int *attr) {
    (void)slot;
    (void)attr;
    return 0;
}
int bs_cond_destroy(void **slot) { (void)slot; return 0; }
int bs_cond_broadcast(void **slot) { (void)slot; return 0; }
void bs_android_configuration_from_assetmanager(void *config, void *manager) {
    (void)config;
    (void)manager;
}
void *bs_ga_test_native_window(uint32_t *width, uint32_t *height) {
    (void)width;
    (void)height;
    return NULL;
}
int bs_ga_test_native_focus(void) { return 2; }
int bs_ga_test_native_input(BsGaNativeInputSnapshot *out) {
    (void)out;
    return -ENODEV;
}

static DWORD WINAPI foreign_state_writer(void *arg) {
    (void)arg;
    LONG result = bs_gameactivity_glue_copy_saved_state(&glue, "foreign", 7);
    InterlockedExchange(&state_result, result);
    return 0;
}
static DWORD WINAPI foreign_command_producer(void *arg) {
    (void)arg;
    LONG result = bs_gameactivity_glue_bind_configuration(
        &glue, &real_config, &real_assets);
    InterlockedExchange(&producer_result, result);
    return 0;
}
static DWORD WINAPI release_after_close(void *arg) {
    (void)arg;
    assert(WaitForSingleObject(entered_mutex, 5000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 5000; ++i) {
        if (atomic_load_explicit(&glue.producerGate, memory_order_acquire)
            & (1u << 31)) {
            InterlockedExchange(&saw_closed_gate, 1);
            break;
        }
        Sleep(1);
    }
    assert(SetEvent(release_mutex));
    return 0;
}
static void join_thread(HANDLE thread) {
    assert(thread != NULL);
    assert(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
}

int main(void) {
    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    assert(bs_gameactivity_glue_copy_saved_state(&glue, "native", 6) == 0);
    HANDLE foreign = CreateThread(NULL, 0, foreign_state_writer, NULL, 0, NULL);
    join_thread(foreign);
    assert(state_result == -EPERM);
    assert(glue.app.savedStateSize == 6);
    assert(memcmp(glue.app.savedState, "native", 6) == 0);

    assert(bs_gameactivity_glue_test_enqueue(
        &glue, BS_GA_CMD_DESTROY, NULL, 0, 0) == 0);
    /* Queueing DESTROY seals saved state before the owner dispatches it. */
    assert(bs_gameactivity_glue_copy_saved_state(&glue, "queued", 6) == -EPIPE);
    glue.app.cmdPollSource.process(&glue.app, &glue.app.cmdPollSource);
    assert(glue.app.destroyRequested == 1);
    assert(bs_gameactivity_glue_copy_saved_state(&glue, "late", 4) == -EPIPE);
    assert(bs_gameactivity_glue_close_native(&glue) == 0);

    assert(bs_gameactivity_glue_prepare_native(&glue) == 0);
    entered_mutex = CreateEventA(NULL, TRUE, FALSE, NULL);
    release_mutex = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(entered_mutex && release_mutex);
    InterlockedExchange(&stall_foreign, 1);
    foreign = CreateThread(NULL, 0, foreign_command_producer, NULL, 0, NULL);
    HANDLE releaser = CreateThread(NULL, 0, release_after_close, NULL, 0, NULL);
    assert(foreign && releaser);
    assert(WaitForSingleObject(entered_mutex, 5000) == WAIT_OBJECT_0);

    /* Once admitted, the foreign producer may complete while close seals
     * admissions; the pipe and mutex must survive until that producer exits. */
    assert(bs_gameactivity_glue_close_native(&glue) == 0);
    join_thread(foreign);
    join_thread(releaser);
    assert(saw_closed_gate == 1 && producer_result == 0);
    assert(bs_gameactivity_glue_bind_configuration(
        &glue, &real_config, &real_assets) == -EPIPE);
    assert(bs_gameactivity_glue_copy_saved_state(&glue, "late", 4) == -EPIPE);
    assert(CloseHandle(entered_mutex) && CloseHandle(release_mutex));
    DeleteCriticalSection(&pipe_mutex);
    puts("PASS: GameActivity saved-state owner, destroy seal, and in-flight producer shutdown");
    return 0;
}
