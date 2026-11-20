/* Beat Saber Quest Unity 6 ARM64 native loader: memory-map/relocation milestone.
 * Original game ELF libraries are loaded from the owner's SD card at runtime.
 * This diagnostic maps the real Unity modules and checks genuine entrypoints.
 * A narrowly pinned original libmain JNI_OnLoad registration may be run
 * after safe loading; Unity/GameActivity and XR startup remain gated.
 *
 * Copyright (c) 2026 Beat Saber NX homebrew contributors.
 * MIT license; see ../LICENSE.
 */
#include <switch.h>
#include <elf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/iosupport.h>

#include "so_util.h"
#include "util.h"
#include "platform_bridge.h"
#include "egl_bridge.h"
#include "thread_sampler.h"
#include "runtime_selftest.h"
#include "headset_pose.h"
#include "openxr_runtime.h"
#include "guest_tls.h"
#include "elf_dependency_order.h"
#include "jni_bridge.h"
#include "unity_lifecycle.h"
#include "bionic_threads.h"
#include "asset_fs_bridge.h"
#include "android_media_format.h"
#include "android_audio_bridge.h"
#include "offline_network_bridge.h"
#include "bionic_locale_bridge.h"
#include "joycon_bridge.h"
#include "gameactivity_native_glue.h"
#include "android_window_bridge.h"
#include "vulkan_bridge.h"
#include "unity_apk_dex_loader.h"
#include "unity_dex_android_host.h"
#include "unity_dex_android_text_input_host.h"
#include "unity_dex_horizon_activity_host.h"
#include "unity_dex_horizon_java_runtime.h"
#include "unity_dex_horizon_services.h"
#include "unity_dex_quest_package_host.h"
#include "unity_dex_horizon_context_objects.h"
#include "unity_dex_horizon_main_looper.h"
#include "unity_dex_horizon_android_attachment.h"
#include "unity_dex_horizon_java_resources.h"
#include "unity_dex_horizon_java_handler.h"
#include "unity_dex_horizon_surface_view_backend.h"
#include "unity_dex_horizon_java_attach_runtime.h"
#include "unity_dex_quest_arsc_resolver.h"
#include "unity_dex_preinit_executor.h"
#include "unity_dex_vm.h"
#include "block_trace.h"
#include "dex_class_index.h"

#define BS_GAME_ROOT "sdmc:/switch/beat-saber-quest/"
static bool unity_window_graphics;
/* Host Mesa created a GLES 3.2 context before guest code ran. */
static bool g_egl_host_ready;

/* The libnx console renders through a framebuffer on the default NWindow.
 * Unity's EGL window surface needs that same NWindow, so the console must be
 * closed (its buffers released) before Unity receives APP_CMD_INIT_WINDOW.
 * Afterwards stdout/stderr are discarded; loader.log keeps all diagnostics. */
static atomic_bool g_console_active;
static ssize_t bs_null_write(struct _reent *r, void *fd, const char *p, size_t n) {
    (void)r; (void)fd; (void)p;
    return (ssize_t)n;
}
static const devoptab_t g_null_devoptab = {
    .name = "null", .write_r = bs_null_write,
};
static void bs_console_update(void) {
    if (atomic_load(&g_console_active)) consoleUpdate(NULL);
}
void bs_host_console_release_for_graphics(void) {
    if (!atomic_exchange(&g_console_active, false)) return;
    fflush(stdout);
    fflush(stderr);
    devoptab_list[STD_OUT] = &g_null_devoptab;
    devoptab_list[STD_ERR] = &g_null_devoptab;
    consoleExit(NULL);
    debugPrintf("[graphics] host console released; default NWindow handed to Unity\n");
}
#define MAX_MODULE_IMAGE ((size_t)256 * 1024 * 1024)
#define MAX_MODULE_MAPPING ((size_t)384 * 1024 * 1024)
#define CORE_MODULE_COUNT 5
#define MODULE_COUNT 15

/* Stage the actual original Quest ARM64 libraries together, so a Unity import
 * can bind a symbol exported by a plugin loaded later in the dependency graph.
 * Engine initialization is separately gated on the compatibility runtime.
 */
static const char *module_names[MODULE_COUNT] = {
    "libmain.so", "libc++_shared.so", "libunity.so",
    "libil2cpp.so", "libgame.so",
    "lib_burst_generated.so", "libopenxr_loader.so",
    "libUnityOpenXR.so", "libOVRPlugin.so",
    "libOVRMetricsTool.so", "libovrplatformloader.so",
    "libhaptics_sdk.so", "libenet.so", "libcrypto.so", "libssl.so"
};
static so_module modules[MODULE_COUNT];
static void *module_mem[MODULE_COUNT];
static size_t module_mapping_bytes[MODULE_COUNT];
static void *main_guest_tls;
/* The verified topology is derived from every staged Quest DT_NEEDED before
 * so_free_temp releases original ELF metadata. It persists for the guarded
 * constructor bootstrap, which runs only after actual platform readiness. */
static size_t original_constructor_order[MODULE_COUNT];
static bool original_constructor_order_valid;
/* Own a real native NDK command transport on the loader's event-loop thread.
 * Original Java GameActivity/NativeCode is still independently required. */
static BsGameActivityGlue native_activity_transport;
static bool native_activity_transport_ready;
/* Owned NDK configuration must outlive any pending CONFIG_CHANGED command
 * and be released only after the native looper has stopped dispatching. */
static void *native_activity_configuration;
/* AAudio permits one output stream. Its single callback worker owns this
 * allocation, which is detached and freed on that same thread at close. */
static void *audio_worker_guest_tls;

static int bs_audio_worker_attach_guest(void *context) {
    (void)context;
    if (audio_worker_guest_tls || !bs_guest_tls_is_ready()) return 0;
    void *buffer = memalign(16, BIONIC_TLS_SIZE);
    if (!buffer) return 0;
    if (!bs_guest_tls_install_current_thread(buffer)) {
        free(buffer);
        return 0;
    }
    if (bs_bionic_tls_finalize_current_thread() != 0) {
        bs_guest_tls_detach_thread();
        free(buffer);
        return 0;
    }
    audio_worker_guest_tls = buffer;
    return 1;
}

static void bs_audio_worker_detach_guest(void *context) {
    (void)context;
    if (!audio_worker_guest_tls) return;
    bs_guest_tls_detach_thread();
    free(audio_worker_guest_tls);
    audio_worker_guest_tls = NULL;
}
/* Static storage zero-initializes the C11 atomic_bool to false. */
static atomic_bool gameactivity_exit_requested;

/* ---- Scheduler watchdog ------------------------------------------------
 * Horizon does not time-slice threads of equal priority on one core, so a
 * guest thread that busy-polls (legal on Android) can starve a lock holder
 * and stall the host main loop with no log output. This higher-priority
 * thread records main-loop progress into its own file WITHOUT taking the
 * shared debugPrintf lock, so a stall is still diagnosable. */
static _Atomic unsigned watchdog_main_iterations;
static _Atomic int watchdog_main_stage;
static Thread watchdog_thread;
static bool watchdog_running;
static atomic_bool watchdog_stop;

/* Uses only raw libnx FsFile IPC and a stack buffer: no newlib FILE lock,
 * malloc lock or debugPrintf lock, so it cannot join a deadlock it is
 * meant to observe. */
typedef struct { FsFile *file; s64 *offset; } BsWatchdogSink;
static void watchdog_sample_line(void *user, const char *line, size_t n) {
    BsWatchdogSink *sink = user;
    if (R_SUCCEEDED(fsFileWrite(sink->file, *sink->offset, line, (u64)n,
                                FsWriteOption_Flush)))
        *sink->offset += (s64)n;
}

static void watchdog_main(void *arg) {
    (void)arg;
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd) return;
    static const char path[] = "/switch/beat-saber-quest/watchdog.log";
    fsFsDeleteFile(sd, path);
    FsFile file;
    if (R_FAILED(fsFsCreateFile(sd, path, 0, 0)) ||
        R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append,
                              &file)))
        return;
    s64 offset = 0;
    unsigned last_iterations = 0;
    while (!atomic_load(&watchdog_stop)) {
        svcSleepThread(5000000000ull);
        unsigned iterations = atomic_load(&watchdog_main_iterations);
        uint64_t polls = 0, zero_polls = 0, poll_tid = 0;
        bs_looper_poll_stats(&polls, &zero_polls, &poll_tid);
        char line[256];
        int n = snprintf(line, sizeof line,
                         "[watchdog] t=%lus main_iter=%u (+%u) stage=%d driver=%d "
                         "log_lock=0x%08x looper_polls=%lu zero_timeout=%lu "
                         "last_poll_tid=%lu\n",
                         (unsigned long)(armTicksToNs(armGetSystemTick()) /
                                         1000000000ull),
                         iterations, iterations - last_iterations,
                         atomic_load(&watchdog_main_stage),
                         (int)bs_gameactivity_java_driver_stage,
                         (unsigned)debugLogLockTag(), (unsigned long)polls,
                         (unsigned long)zero_polls, (unsigned long)poll_tid);
        if (n > 0 && (size_t)n < sizeof line &&
            R_SUCCEEDED(fsFileWrite(&file, offset, line, (u64)n,
                                    FsWriteOption_Flush)))
            offset += n;
        /* Blocked-thread table (lock-free snapshot). so_list is only
         * mutated during loading, before the guest runs, so an unlocked
         * walk is safe here. */
        BsBlockInfo blocks[24];
        size_t block_count = bs_block_snapshot(blocks, 24);
        uint64_t now_ns = armTicksToNs(armGetSystemTick());
        for (size_t i = 0; i < block_count; ++i) {
            const BsBlockInfo *b = &blocks[i];
            so_module *m = so_find_module_by_addr((const void *)b->caller);
            uintptr_t off = m ? b->caller - (uintptr_t)m->load_virtbase
                              : b->caller;
            uint64_t age_ms =
                (now_ns - armTicksToNs(b->since_tick)) / 1000000ull;
            n = snprintf(line, sizeof line,
                         "[watchdog]   blocked tid=%lu op=%s obj=0x%lx "
                         "caller=%s+0x%lx age=%lums val=%d\n",
                         (unsigned long)b->thread_id, bs_block_op_name(b->op),
                         (unsigned long)b->object, m ? m->name : "host",
                         (unsigned long)off, (unsigned long)age_ms,
                         b->op == BS_BLOCK_FUTEX && b->object
                             ? *(const volatile int *)b->object : 0);
                if (n > 0 && (size_t)n < sizeof line &&
                R_SUCCEEDED(fsFileWrite(&file, offset, line, (u64)n,
                                        FsWriteOption_Flush)))
                offset += n;
            uint64_t owner_tid = 0;
            uintptr_t owner_caller = 0;
            if (b->op != BS_BLOCK_MUTEX ||
                !bs_block_lookup_owner(b->object, &owner_tid, &owner_caller))
                continue;
            so_module *om = so_find_module_by_addr((const void *)owner_caller);
            n = snprintf(line, sizeof line,
                         "[watchdog]     owner tid=%lu locked_at=%s+0x%lx\n",
                         (unsigned long)owner_tid, om ? om->name : "host",
                         (unsigned long)(om ? owner_caller -
                                         (uintptr_t)om->load_virtbase
                                            : owner_caller));
            if (n > 0 && (size_t)n < sizeof line &&
                R_SUCCEEDED(fsFileWrite(&file, offset, line, (u64)n,
                                        FsWriteOption_Flush)))
                offset += n;
        }
        BsWatchdogSink sink = {&file, &offset};
        bs_sampler_dump(watchdog_sample_line, &sink);
        /* libgame's UnityApplication (global at libgame+0x30a30) holds the
         * libunity callbacks that gate each frame of Unity's GameActivity
         * loop; report where they point and the loop's state flags. */
        so_module *game = so_find_module_by_name("libgame.so");
        uintptr_t app = game ? *(const uintptr_t *)((uintptr_t)game->load_virtbase + 0x30a30) : 0;
        if (app) {
            static const unsigned slots[] = {352, 480, 488, 496, 504, 512, 520, 528};
            for (size_t s = 0; s < sizeof slots / sizeof slots[0]; ++s) {
                uintptr_t fn = *(const uintptr_t *)(app + slots[s]);
                so_module *fm = so_find_module_by_addr((const void *)fn);
                n = snprintf(line, sizeof line, "[watchdog] app+%u -> %s+0x%lx\n",
                             slots[s], fm ? fm->name : "?",
                             (unsigned long)(fm ? fn - (uintptr_t)fm->load_virtbase : fn));
                if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
            }
            const uint8_t *flags = (const uint8_t *)(app + 728);
            n = snprintf(line, sizeof line, "[watchdog] app flags 728..733 = %u %u %u %u %u %u\n",
                         flags[0], flags[1], flags[2], flags[3], flags[4], flags[5]);
            if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
        }
        /* libunity player pause/resume state (0x17db9c0..) and the vsync
         * counter/provider used by the frame pacer. */
        {
            extern _Atomic unsigned long bs_futex_wait_calls, bs_futex_wake_calls,
                bs_futex_wake_hits, bs_futex_wait_eagain;
            extern volatile uintptr_t bs_futex_last_wake[8];
            extern _Atomic unsigned long bs_gl_clears, bs_gl_draws,
                bs_gl_fbo_binds, bs_gl_flushes;
            n = snprintf(line, sizeof line,
                         "[watchdog] gl clears=%lu draws=%lu fbo_binds=%lu flushes=%lu\n",
                         (unsigned long)bs_gl_clears, (unsigned long)bs_gl_draws,
                         (unsigned long)bs_gl_fbo_binds, (unsigned long)bs_gl_flushes);
            if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
            n = snprintf(line, sizeof line,
                         "[watchdog] futex waits=%lu eagain=%lu wakes=%lu woke=%lu last=",
                         (unsigned long)bs_futex_wait_calls,
                         (unsigned long)bs_futex_wait_eagain,
                         (unsigned long)bs_futex_wake_calls,
                         (unsigned long)bs_futex_wake_hits);
            for (int i = 0; i < 8 && n > 0 && (size_t)n < sizeof line - 32; ++i)
                n += snprintf(line + n, sizeof line - n, "%#lx ",
                              (unsigned long)bs_futex_last_wake[i]);
            if (n > 0 && (size_t)n < sizeof line - 1) {
                line[n++] = '\n';
                watchdog_sample_line(&sink, line, (size_t)n);
            }
        }
        so_module *unity = so_find_module_by_name("libunity.so");
        if (unity) {
            const uint8_t *u = (const uint8_t *)unity->load_virtbase;
            n = snprintf(line, sizeof line,
                         "[watchdog] unity 2496=%u 2512=%u 2513=%u 2520=%u 2532=%u "
                         "2544=%u 2549=%u 2552=%u 2553=%u 2556=%u 2625=%u state=%d "
                         "vsync=%ld pacing_base=%ld provider_holder=%#lx\n",
                         u[0x17db9c0], u[0x17db9d0], u[0x17db9d1], u[0x17db9d8],
                         u[0x17db9e4], u[0x17db9f0], u[0x17db9f5], u[0x17db9f8],
                         u[0x17db9f9], u[0x17db9fc], u[0x17dba41],
                         *(const int32_t *)(u + 0x17db9d4),
                         *(const long *)(u + 0x17da120),
                         *(const long *)(u + 0x17d4f00),
                         *(const unsigned long *)(u + 0x17da140));
            if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
            {
                n = snprintf(line, sizeof line,
                             "[watchdog] unity swappy enabled=%u swap=%#lx wrapper=%#lx\n",
                             u[0x17d6084], *(const unsigned long *)(u + 0x17d60b0),
                             *(const unsigned long *)(u + 0x17d6310));
                if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                /* Present gate: 0x8f58ac skips eglSwapBuffers while display 0
                 * (0x17d7200, stride 0x298) has a pending size change. */
                const uint8_t *d0 = u + 0x17d7200;
                const uint8_t *ctx = u + 0x17e99c8;
                n = snprintf(line, sizeof line,
                             "[watchdog] unity present gate8ac0=%u disp pend=%u cur=%dx%d "
                             "req=%dx%d ctx surf=%#lx egl_ctx=%#lx size=%dx%d\n",
                             u[0x17d8ac0], d0[544],
                             *(const int32_t *)(d0 + 548), *(const int32_t *)(d0 + 552),
                             *(const int32_t *)(d0 + 556), *(const int32_t *)(d0 + 560),
                             *(const unsigned long *)(ctx + 16),
                             *(const unsigned long *)(ctx + 40),
                             *(const int32_t *)(ctx + 176), *(const int32_t *)(ctx + 180));
                if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                {
                    const uint8_t *d8 = u + 0x17d7200 + 8 * 0x298;
                    n = snprintf(line, sizeof line,
                                 "[watchdog] unity disp0 init656=%u disp8 pend=%u cur=%dx%d "
                                 "req=%dx%d init656=%u ctx196=%u ctx64=%u\n",
                                 d0[656], d8[544],
                                 *(const int32_t *)(d8 + 548), *(const int32_t *)(d8 + 552),
                                 *(const int32_t *)(d8 + 556), *(const int32_t *)(d8 + 560),
                                 d8[656], ctx[196], ctx[64]);
                    if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                    /* 0x8f61e0: XR owns presentation when the XR display
                     * subsystem (0x17c7008) or VR device (0x17c6b28) says so. */
                    const uintptr_t xa = *(const uintptr_t *)(u + 0x17c7008);
                    const uintptr_t xb = *(const uintptr_t *)(u + 0x17c6b28);
                    const uintptr_t xbv = xb ? *(const uintptr_t *)xb : 0;
                    const uintptr_t xav = xa ? *(const uintptr_t *)xa : 0;
                    n = snprintf(line, sizeof line,
                                 "[watchdog] unity xr display=%#lx vt16=%#lx vrdev=%#lx vt304=%#lx\n",
                                 (unsigned long)xa,
                                 xav ? (unsigned long)(*(const uintptr_t *)(xav + 16) - (uintptr_t)u) : 0ul,
                                 (unsigned long)xb,
                                 xbv ? (unsigned long)(*(const uintptr_t *)(xbv + 304) - (uintptr_t)u) : 0ul);
                    if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                    if (xa) {
                        const uintptr_t fn = *(const uintptr_t *)(xa + 16);
                        so_module *fm = so_find_module_by_addr((const void *)fn);
                        const uint64_t flags = 0;
                        n = snprintf(line, sizeof line,
                                     "[watchdog] unity xr flags fn=%s+%#lx user=%#lx flags=%#llx\n",
                                     fm ? fm->name : "?",
                                     fm ? (unsigned long)(fn - (uintptr_t)fm->load_virtbase) : (unsigned long)fn,
                                     (unsigned long)*(const uintptr_t *)(xa + 8),
                                     (unsigned long long)flags);
                        if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                    }
                }
                const int32_t *topo = (const int32_t *)(u + 0x17dacb8);
                n = snprintf(line, sizeof line,
                             "[watchdog] unity cpu topology %d %d %d %d affinity=%#x\n",
                             topo[0], topo[1], topo[2], topo[3],
                             *(const uint32_t *)(u + 0x1754088));
                if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
                const uint32_t *ts = *(const uint32_t *const *)(u + 0x17d6840);
                if (ts) {
                    n = snprintf(line, sizeof line, "[watchdog] unity cpu table:");
                    for (int k = 0; k < 2 + 8 * 4 && n < (int)sizeof line - 12; ++k)
                        n += snprintf(line + n, sizeof line - n, " %x", ts[k]);
                    if (n > 0 && n < (int)sizeof line - 1) {
                        line[n++] = '\n';
                        watchdog_sample_line(&sink, line, (size_t)n);
                    }
                }
            }
            const uint8_t *vp = *(const uint8_t *const *)(u + 0x17da240);
            if (vp) {
                n = snprintf(line, sizeof line,
                             "[watchdog] vsync provider %p started=%u choreo=%#lx "
                             "thread=%#lx use64=%u stopped=%u\n", (const void *)vp,
                             vp[8], *(const unsigned long *)(vp + 376),
                             *(const unsigned long *)(vp + 384), vp[392], vp[393]);
                if (n > 0) watchdog_sample_line(&sink, line, (size_t)n);
            }
        }
        last_iterations = iterations;
    }
    fsFileClose(&file);
}

/* Guest crash report: libnx calls this on the exception stack for any
 * unhandled CPU exception. Raw FsFile IPC only (no newlib locks). */
__attribute__((aligned(16))) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
static void crash_describe(char *out, size_t size, const char *tag, u64 addr) {
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
    if (m)
        snprintf(out, size, "%s=%s+0x%lx ", tag, m->name,
                 (unsigned long)(addr - (uintptr_t)m->load_virtbase));
    else
        snprintf(out, size, "%s=0x%lx ", tag, (unsigned long)addr);
}
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    static const char path[] = "/switch/beat-saber-quest/crash.log";
    static char line[12288];
    size_t n = 0;
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    char part[200];
    n += snprintf(line + n, sizeof line - n, "[crash] desc=0x%x tid=%lu ",
                  ctx->error_desc, (unsigned long)tid);
    crash_describe(part, sizeof part, "pc", ctx->pc.x);
    n += snprintf(line + n, sizeof line - n, "%s", part);
    crash_describe(part, sizeof part, "lr", ctx->lr.x);
    n += snprintf(line + n, sizeof line - n, "%s", part);
    n += snprintf(line + n, sizeof line - n, "far=0x%lx sp=0x%lx\n",
                  (unsigned long)ctx->far.x, (unsigned long)ctx->sp.x);
    for (int i = 0; i < 29 && n < sizeof line - 64; ++i) {
        crash_describe(part, sizeof part, "x", ctx->cpu_gprs[i].x);
        n += snprintf(line + n, sizeof line - n, "  x%d %s\n", i, part + 2);
    }
    /* Return-address scan of the stack for a rough backtrace. */
    const u64 *sp = (const u64 *)(uintptr_t)ctx->sp.x;
    for (int i = 0; sp && i < 256 && n < sizeof line - 80; ++i) {
        so_module *m = so_find_module_by_addr((const void *)(uintptr_t)sp[i]);
        if (!m) continue;
        n += snprintf(line + n, sizeof line - n, "  sp+0x%x %s+0x%lx\n", i * 8,
                      m->name, (unsigned long)(sp[i] - (uintptr_t)m->load_virtbase));
    }
    if (n > sizeof line) n = sizeof line;
    if (sd) {
        fsFsDeleteFile(sd, path);
        FsFile file;
        if (R_SUCCEEDED(fsFsCreateFile(sd, path, 0, 0)) &&
            R_SUCCEEDED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append,
                                     &file))) {
            fsFileWrite(&file, 0, line, n, FsWriteOption_Flush);
            fsFileClose(&file);
        }
    }
    svcExitProcess();
}

static void watchdog_start(void) {
    /* 0x2B outranks the main thread (0x2C) and guest pthreads; -2 keeps it
     * on the default core where the starvation would occur. */
    if (R_FAILED(threadCreate(&watchdog_thread, watchdog_main, NULL, NULL,
                              0x8000, 0x2B, -2)))
        return;
    if (R_FAILED(threadStart(&watchdog_thread))) {
        threadClose(&watchdog_thread);
        return;
    }
    watchdog_running = true;
}

static void watchdog_shutdown(void) {
    if (!watchdog_running) return;
    atomic_store(&watchdog_stop, true);
    threadWaitForExit(&watchdog_thread);
    threadClose(&watchdog_thread);
    watchdog_running = false;
}
static void *gameactivity_native_proxy;
static void *gameactivity_java_configuration;
static BsGameActivityJavaDriver original_java_activity;
static void *original_java_surface;
static void *original_java_window;
static uint32_t original_java_width, original_java_height;
static uint32_t original_java_format;
static bool original_java_started;
static BsUnityStartProgress original_startup;
static BsUnityStartCapabilities original_engine_caps;
static BsGameActivityJavaLaunch original_java_launch;
/* This becomes true ONLY after actually parsing the pinned original DEX
 * with the bounded bytecode VM. It does not mean a Java Activity exists. */
static bool original_java_dex_parsed;

static bool bs_continue_original_startup(int count) {
    original_java_started = bs_unity_start_original_game(
        modules, (size_t)count, &original_engine_caps);
    debugPrintf("[startup] original libraries: constructors=%lu/%d JNI=%u/7 "
                "waiting_for_activity=%d\n",
                (unsigned long)original_startup.constructors_completed, count,
                original_startup.jni_completed,
                original_startup.waiting_for_activity);
    return original_java_started;
}

static int reject_existing_activity(void *user, void *activity) {
    (void)user;
    (void)activity;
    return 0;
}

static int game_library_already_initialized(void *user) {
    (void)user;
    /* This probe only runs after the original libgame constructors and
     * verified JNI registration. Java's repeat load is idempotent. */
    return 1;
}

static int horizon_monotonic_nanoseconds(void *user, uint64_t *out) {
    (void)user;
    if (!out) return 0;
    /* libnx's clock conversion reads the current Horizon system tick. A
     * fresh observation seeds the original Java Random() implementation. */
    *out = armTicksToNs(armGetSystemTick());
    return *out != 0;
}

/* An Android Java Log.d completes only after an actual Switch filesystem
 * write succeeded. The log is deliberately offline, like the native guest
 * __android_log_* bridge, and the number returned is the sink's actual bytes
 * written. There is no fabricated success if the SD log is unavailable. */
static int horizon_dex_debug_log(void *user, const char *tag,
                                 const char *message, int32_t *written) {
    BsDexHorizonServices *services = user;
    if (written) *written = 0;
    if (!written || !tag || !message || !services ||
        !bs_dex_horizon_services_on_applet_thread(services)) return 0;
    FILE *file = fopen(BS_GAME_ROOT "loader.log", "a");
    if (!file) return 0;
    int count = fprintf(file, "[java:android-log:%s] %s\n", tag, message);
    int flushed = fflush(file);
    int closed = fclose(file);
    if (count < 0 || flushed != 0 || closed != 0) return 0;
    *written = (int32_t)count;
    return 1;
}

static int horizon_dex_text_ui_thread(void *user) {
    return bs_dex_horizon_services_on_applet_thread(user);
}

static int horizon_observe_java_main_looper(void *user,
                                             uintptr_t *queue_identity,
                                             uint64_t *owner_thread_id,
                                             uint64_t *current_thread_id);

static void trace_activity_stop(const char *stage, BsDexStop stop) {
    debugPrintf("[java:vm] original Activity %s stop=%d steps=%u "
                "code=0x%x pc=0x%x opcode=0x%02x depth=%u "
                "original_method=%s.%s host=%s.%s exception=%s detail=%s\n",
                stage, stop.kind, stop.steps, stop.code_off, stop.dex_pc,
                stop.opcode, stop.depth,
                stop.method_class ? stop.method_class : "(none)",
                stop.method_name ? stop.method_name : "(none)",
                stop.host_class ? stop.host_class : "(none)",
                stop.host_method ? stop.host_method : "(none)",
                stop.exception ? stop.exception : "(none)",
                stop.detail ? stop.detail : "(none)");
}

static struct {
    BsUnityOriginalDexImage dex;
    BsDexAndroidContextHost context;
    BsDexAndroidSurfaceHost surface;
    BsDexAndroidTextInputHost text_input;
    BsDexHorizonSurfaceViewBackend surface_backend;
    BsDexAndroidHost platform;
    BsDexHorizonServices native_services;
    BsDexHorizonJavaRuntime java_runtime;
    BsDexHorizonActivityHost horizon_host;
    BsDexQuestPackageHost quest_package;
    BsDexHorizonContextProduction context_production;
    BsDexHorizonContextObjects native_context;
    BsDexHorizonMainLooperProduction main_looper_production;
    BsDexHorizonMainLooper main_looper;
    BsDexHorizonAndroidAttachment attachment;
    BsDexHorizonJavaResources native_resources;
    BsDexHorizonJavaAttachRuntime java_attach_runtime;
    BsDexHorizonJavaHandlerHost handler;
    BsQuestArscResolver native_resource_table;
    BsDexVM *vm;
} activity_preinit;

/* Looper.getMainLooper is reached DURING the original DEX constructor, after
 * Activity.<init>. Observe the actual native queue lazily at that call, when
 * the applet Activity owner, ALooper and main thread are all live. This does
 * not construct a Java Handler or enable arbitrary Runnable dispatch. */
static int horizon_observe_java_main_looper(void *user,
                                            uintptr_t *queue_identity,
                                            uint64_t *owner_thread_id,
                                            uint64_t *current_thread_id) {
    (void)user;
    if (queue_identity) *queue_identity = 0;
    if (owner_thread_id) *owner_thread_id = 0;
    if (current_thread_id) *current_thread_id = 0;
    if (!queue_identity || !owner_thread_id || !current_thread_id ||
        !bs_dex_horizon_java_runtime_native_base_ready(
            &activity_preinit.java_runtime)) return 0;
    BsDexHorizonMainLooperNative native =
        bs_dex_horizon_main_looper_production_callbacks(
            &activity_preinit.main_looper_production);
    int already_observed =
        activity_preinit.main_looper.observed_native_looper != 0;
    if (!bs_dex_horizon_main_looper_observe_main_loop(
            &activity_preinit.main_looper, &native) ||
        !bs_dex_horizon_main_looper_native_ready(
            &activity_preinit.main_looper) ||
        !bs_dex_horizon_main_looper_owner_thread_id(
            &activity_preinit.main_looper, owner_thread_id) ||
        !activity_preinit.main_looper.native_looper_handle ||
        bs_looper_for_thread() !=
            activity_preinit.main_looper.native_looper_handle)
        return 0;
    u64 current = 0;
    if (R_FAILED(svcGetThreadId(&current, CUR_THREAD_HANDLE)) || !current)
        return 0;
    *current_thread_id = (uint64_t)current;
    *queue_identity = (uintptr_t)
        activity_preinit.main_looper.native_looper_handle;
    if (!already_observed)
        debugPrintf("[java:looper] live Horizon ALooper observed "
                    "owner=%llu current=%llu queue=%p "
                    "(Java Handler unavailable)\n",
                    (unsigned long long)*owner_thread_id,
                    (unsigned long long)*current_thread_id,
                    activity_preinit.main_looper.native_looper_handle);
    return *queue_identity && *owner_thread_id && *current_thread_id;
}

static void release_activity_preinit(void) {
    bs_dex_horizon_java_attach_runtime_clear(
        &activity_preinit.java_attach_runtime);
    /* Close native AssetManager streams and any verified Java Resources
     * BEFORE revoking their live Activity/Looper/Context dependencies. */
    if (activity_preinit.native_resources.initialized &&
        !bs_dex_horizon_java_resources_shutdown(
            &activity_preinit.native_resources))
        debugPrintf("[java:resources] native stream cleanup incomplete\n");
    bs_quest_arsc_release(&activity_preinit.native_resource_table);
    /* Native ALooper ownership is verified against the STILL-LIVE native
     * Activity base. Release it before invalidating that owner and before
     * destroying any VM-owned Runnable references. A future real Java attach
     * needs a separate pre-shutdown Java detach phase, not a reordering that
     * silently leaks the currently observed native ALooper. */
    if (activity_preinit.main_looper.observed_native_looper &&
        !bs_dex_horizon_main_looper_shutdown(&activity_preinit.main_looper))
        debugPrintf("[java:looper] native ALooper owner cleanup incomplete\n");
    bs_dex_vm_destroy(activity_preinit.vm);
    activity_preinit.vm = NULL;
    bs_dex_android_text_input_reset(&activity_preinit.text_input);
    bs_dex_horizon_java_handler_shutdown(&activity_preinit.handler);
    if (!bs_dex_android_surface_host_reset(&activity_preinit.surface))
        debugPrintf("[java:vm] platform view cleanup incomplete\n");
    if (!bs_dex_horizon_surface_view_backend_reset(
            &activity_preinit.surface_backend))
        debugPrintf("[java:vm] native child view cleanup incomplete\n");
    if (activity_preinit.context.activity &&
        activity_preinit.platform.discard_activity) {
        activity_preinit.platform.discard_activity(
            activity_preinit.platform.user, activity_preinit.context.activity);
        activity_preinit.context.activity = NULL;
    }
    if (activity_preinit.horizon_host.activity.constructed &&
        !bs_dex_horizon_activity_release(&activity_preinit.horizon_host))
        debugPrintf("[java:horizon] Activity owner could not be released\n");
    if (activity_preinit.attachment.snapshot &&
        !bs_dex_horizon_android_attachment_release(&activity_preinit.attachment))
        debugPrintf("[java:attachment] native graph cleanup incomplete\n");
    if (activity_preinit.native_context.initialized &&
        !bs_dex_horizon_context_objects_release(&activity_preinit.native_context))
        debugPrintf("[java:horizon] native Context data cleanup incomplete\n");
    bs_dex_quest_package_close(&activity_preinit.quest_package);
    if (activity_preinit.java_runtime.native.subscribe &&
        !bs_dex_horizon_java_runtime_shutdown(&activity_preinit.java_runtime))
        debugPrintf("[java:horizon] applet hook/runtime teardown incomplete\n");
    bs_dex_android_context_clear_attachment(&activity_preinit.context);
    bs_unity_apk_free_original_dex(&activity_preinit.dex);
}

static void probe_activity_constructor(void) {
    if (!bs_unity_apk_load_original_dex(BS_GAME_ROOT "beat-saber.apk",
                                       &activity_preinit.dex))
        return;
    /* Observe the actual applet's native UI thread, NWindow, JNI Surface,
     * AssetManager file access and SD paths. These observations do not
     * establish an Android Activity, Intent, Looper, or Java View. */
    int native_ready = bs_dex_horizon_services_observe(
        &activity_preinit.native_services);
    int surface_ready = bs_dex_horizon_services_observe_surface(
        &activity_preinit.native_services, original_java_surface);
    debugPrintf("[java:horizon] applet native services=%d main_thread=%d "
                "window=%d JNI_surface=%d assets=%d paths=%d/%d/%d; "
                "Java Activity=0\n",
                native_ready,
                activity_preinit.native_services.observed.applet_main_thread_observed,
                activity_preinit.native_services.observed.native_window_valid,
                surface_ready,
                activity_preinit.native_services.observed.native_asset_opened,
                activity_preinit.native_services.observed.files_dir_exists,
                activity_preinit.native_services.observed.obb_dir_exists,
                activity_preinit.native_services.observed.external_files_dir_exists);
    /* Bind the real Horizon child-view registry to the DEX dispatcher.
     * No provider is registered until an actual Activity-attached Java View
     * hierarchy can produce and independently verify a child NWindow. */
    int surface_backend_ready = bs_dex_horizon_surface_view_backend_init(
        &activity_preinit.surface_backend, &activity_preinit.horizon_host,
        &activity_preinit.native_services, NULL);
    BsDexAndroidSurfaceServices surface_services =
        bs_dex_horizon_surface_view_services(
            surface_backend_ready ? &activity_preinit.surface_backend : NULL);
    debugPrintf("[java:surface] native child registry=%d "
                "Java child provider=0 (unavailable)\n",
                surface_backend_ready);
    /* The original DEX supplies the concrete Activity class hierarchy.
     * Only the separately implemented native services can construct/attach
     * an Android-compatible Activity and native child SurfaceView. */
    bs_dex_android_surface_host_init(&activity_preinit.surface,
                                     &surface_services);
    const BsDexAndroidTextNative text_native = {
        .user = &activity_preinit.native_services,
        .on_ui_thread = horizon_dex_text_ui_thread,
        .write_debug_log = horizon_dex_debug_log
        /* Horizon has no Android InputMethodManager service yet.
         * Supplying only libnx Swkbd or HID would not be an independently
         * verified Android IME, so native observe/verify remain NULL. */
    };
    bs_dex_android_text_input_init(
        &activity_preinit.text_input, &activity_preinit.context,
        &activity_preinit.surface, &text_native);
    activity_preinit.platform = (BsDexAndroidHost){
        .context = &activity_preinit.context,
        .surface = &activity_preinit.surface,
        .text_input = &activity_preinit.text_input,
        .resource_user = &activity_preinit.java_attach_runtime,
        .resource_invoke = bs_dex_horizon_java_attach_runtime_invoke,
        .load_game_library = game_library_already_initialized,
        .monotonic_nanoseconds = horizon_monotonic_nanoseconds,
        .observe_main_looper = horizon_observe_java_main_looper,
    };
    /* Register native applet lifecycle callbacks as a minimal observable
     * pre-attach Activity lifetime. This host never supplies Android
     * Activity.attach, Intent, Handler/Looper or Resources; those continue
     * to block original onCreate and JNI UnityPlayer.currentActivity. */
    int runtime_available = bs_dex_horizon_java_runtime_init(
        &activity_preinit.java_runtime, &activity_preinit.native_services);
    activity_preinit.main_looper_production =
        (BsDexHorizonMainLooperProduction){
            .activity = &activity_preinit.java_runtime,
            .horizon = &activity_preinit.native_services
        };
    int handler_host_ready = bs_dex_horizon_java_handler_init(
        &activity_preinit.handler, &activity_preinit.main_looper,
        horizon_observe_java_main_looper, NULL);
    if (handler_host_ready) {
        activity_preinit.platform.handler = &activity_preinit.handler;
        activity_preinit.platform.handler_new =
            bs_dex_horizon_java_handler_new_platform_object;
        activity_preinit.platform.handler_invoke =
            bs_dex_horizon_java_handler_invoke_platform;
        activity_preinit.platform.handler_instance_of =
            bs_dex_horizon_java_handler_instance_of;
    }
    BsDexHorizonActivityServices platform_services = runtime_available ?
        bs_dex_horizon_java_runtime_callbacks(&activity_preinit.java_runtime) :
        bs_dex_horizon_services_callbacks(&activity_preinit.native_services);
    int class_host = bs_dex_horizon_activity_init(
        &activity_preinit.horizon_host, activity_preinit.dex.data,
        activity_preinit.dex.size, &platform_services);
    if (class_host && runtime_available)
        class_host = bs_dex_horizon_java_runtime_bind_verified_host(
            &activity_preinit.java_runtime, &activity_preinit.horizon_host);
    if (class_host)
        class_host = bs_dex_horizon_activity_bind(
            &activity_preinit.horizon_host, &activity_preinit.platform,
            &activity_preinit.context);
    debugPrintf("[java:horizon] original DEX class lineage host=%d "
                "native services=%d applet lifetime=%d "
                "(Android Activity.attach unavailable)\n",
                class_host, native_ready, runtime_available);
    BsDexHost host = {0};
    if (class_host &&
        bs_dex_android_host_callbacks(&activity_preinit.platform, &host))
        activity_preinit.vm = bs_dex_vm_create(activity_preinit.dex.data,
                                                activity_preinit.dex.size, &host);
    BsDexVM *vm = activity_preinit.vm;
    if (!bs_dex_vm_start_activity_constructor(vm)) {
        debugPrintf("[java:vm] could not start original Activity constructor\n");
        release_activity_preinit();
        return;
    }
    BsDexStop stop = bs_dex_vm_run(vm, 10000);
    trace_activity_stop("constructor", stop);
    debugPrintf("[java:handler] backed native main-loop Handler constructors=%lu "
                "Java scheduling=0\n",
                (unsigned long)bs_dex_horizon_java_handler_constructed_count(
                    &activity_preinit.handler));
    /* Pin the exact original Quest manifest/resource bytes and observe native
     * package paths while the real Horizon applet Activity owner is alive.
     * These are typed native facts, never Java Context/Intent/Resources
     * objects, and cannot by themselves complete Android Activity.attach. */
    if (bs_dex_horizon_java_runtime_native_base_ready(
            &activity_preinit.java_runtime)) {
        int package_ready = bs_dex_quest_package_open(
            &activity_preinit.quest_package,
            BS_GAME_ROOT "beat-saber.apk");
        int native_paths_ready = package_ready &&
            bs_dex_quest_package_observe_native_paths(
                &activity_preinit.quest_package,
                BS_GAME_ROOT, BS_GAME_ROOT "files");
        int context_ready = 0;
        if (native_paths_ready) {
            activity_preinit.context_production =
                (BsDexHorizonContextProduction){
                    .native_activity = &activity_preinit.java_runtime,
                    .horizon = &activity_preinit.native_services
                };
            BsDexHorizonContextNative callbacks =
                bs_dex_horizon_context_production_callbacks(
                    &activity_preinit.context_production);
            context_ready = bs_dex_horizon_context_objects_init(
                &activity_preinit.native_context,
                &activity_preinit.quest_package, &callbacks,
                BS_GAME_ROOT "files", BS_GAME_ROOT "obb",
                BS_GAME_ROOT "external-files");
        }
        debugPrintf("[java:package] Quest manifest/resources=%d "
                    "native ELF/data paths=%d Horizon Context facts=%d "
                    "(Java Context=unavailable)\n",
                    package_ready, native_paths_ready, context_ready);
        if (context_ready) {
            BsDexHorizonContextConfig config = {0};
            const char *native_lib_dir = NULL;
            if (bs_dex_horizon_context_read_configuration(
                    &activity_preinit.native_context, &config) &&
                bs_dex_horizon_context_native_library_dir(
                    &activity_preinit.native_context, &native_lib_dir))
                debugPrintf("[java:package] native configuration "
                            "pixels=%ux%u language=%s native_libs=%s\n",
                            config.window_width_px, config.window_height_px,
                            config.horizon_language_tag, native_lib_dir);
            /* Native typed objects are not Java Context/Intent/Resources.
             * Only an independently backed launch and real Java attachment
             * implementation may ever lift the strict onCreate gate. */
            BsDexHorizonAttachmentNative native_attachment =
                bs_dex_horizon_android_attachment_production_native(
                    &activity_preinit.native_services);
            int prepared = bs_dex_horizon_android_attachment_prepare(
                &activity_preinit.attachment, &activity_preinit.quest_package,
                &activity_preinit.native_context,
                &activity_preinit.java_runtime,
                &activity_preinit.main_looper, &native_attachment);
            int installed = prepared &&
                bs_dex_horizon_android_attachment_install(
                    &activity_preinit.attachment,
                    &activity_preinit.horizon_host);
            debugPrintf("[java:attachment] original Quest typed native graph=%d "
                        "Activity callback installed=%d Java attached=%d\n",
                        prepared, installed,
                        bs_dex_horizon_android_attachment_java_attached(
                            &activity_preinit.attachment));
            if (installed) {
                BsDexHorizonJavaResourcesNative resource_native =
                    bs_dex_horizon_java_resources_production_native(
                        &activity_preinit.attachment);
                int resources_ready = bs_dex_horizon_java_resources_init(
                    &activity_preinit.native_resources,
                    &activity_preinit.attachment,
                    &activity_preinit.native_context,
                    &activity_preinit.quest_package, &resource_native);
                debugPrintf("[java:resources] original Quest native assets="
                            "%d Java Resources=0 (unimplemented)\n",
                            resources_ready &&
                            bs_dex_horizon_java_resources_native_ready(
                                &activity_preinit.native_resources));
                if (resources_ready) {
                    /* Exercise a REAL Quest asset via the newly installed
                     * NDK stream transport. This does not assert a Java
                     * AssetManager, Resources or parsed config. */
                    BsDexHorizonAssetStream stream = {0};
                    uint64_t asset_length = 0;
                    int opened = bs_dex_horizon_java_resources_open(
                        &activity_preinit.native_resources,
                        "bin/Data/boot.config", &stream, &asset_length);
                    uint8_t prefix[16] = {0};
                    size_t bytes_read = 0;
                    int read_ok = opened &&
                        bs_dex_horizon_java_resources_read(
                            stream, prefix, sizeof prefix, &bytes_read);
                    int closed = opened &&
                        bs_dex_horizon_java_resources_close(stream);
                    debugPrintf("[java:resources] original Quest asset stream "
                                "open=%d size=%llu read=%lu close=%d\n",
                                opened, (unsigned long long)asset_length,
                                (unsigned long)(read_ok ? bytes_read : 0),
                                closed);
                    /* Resolve an actual typed Quest resources.arsc entry,
                     * using only OS-observed locale. This remains native
                     * resource evidence, NOT a Java Resources jobject. */
                    int parsed = bs_quest_arsc_open_apk(
                        BS_GAME_ROOT "beat-saber.apk",
                        &activity_preinit.native_resource_table);
                    BsDexHorizonObservedConfiguration native_config = {0};
                    BsQuestArscConfiguration observed_config = {0};
                    char app_name[128] = {0};
                    int config_ready = parsed &&
                        bs_dex_horizon_java_resources_configuration(
                            &activity_preinit.native_resources, &native_config) &&
                        bs_quest_arsc_set_observed_language(
                            &observed_config, native_config.language_tag);
                    int app_name_ready = config_ready &&
                        bs_quest_arsc_get_string(
                            &activity_preinit.native_resource_table,
                            UINT32_C(0x7f0d0021), &observed_config,
                            app_name, sizeof app_name);
                    debugPrintf("[java:resources] pinned native ARSC="
                                "%d observed locale=%d typed app_name=%d%s%s\n",
                                parsed, config_ready, app_name_ready,
                                app_name_ready ? " value=" : "",
                                app_name_ready ? app_name : "");
                    if (parsed && config_ready) {
                        int bound =
                            bs_dex_horizon_java_resources_bind_native_arsc(
                                &activity_preinit.native_resources,
                                &activity_preinit.native_resource_table);
                        const char *native_name = NULL;
                        int32_t original_duration_ms = 0;
                        int name_ok = bound &&
                            bs_dex_horizon_java_resources_get_string(
                                &activity_preinit.native_resources,
                                UINT32_C(0x7f0d0021), &native_name);
                        int duration_ok = bound &&
                            bs_dex_horizon_java_resources_get_integer(
                                &activity_preinit.native_resources,
                                UINT32_C(0x7f090000), &original_duration_ms);
                        debugPrintf("[java:resources] authenticated native "
                                    "resolver=%d app_name=%s duration=%ld "
                                    "Java resources unavailable\n",
                                    bound, name_ok ? native_name : "(none)",
                                    (long)(duration_ok ? original_duration_ms : -1));
                    }
                }
            }
        }
    }
    debugPrintf("[java:horizon] native Activity constructor gate=%lu "
                "base_constructed=%d event_subscribed=%d generation=%llu "
                "native_ready=%d\n",
                (unsigned long)activity_preinit.java_runtime.last_construct_gate,
                activity_preinit.java_runtime.native_base_constructed,
                activity_preinit.java_runtime.event_token != NULL,
                (unsigned long long)activity_preinit.horizon_host.activity.generation,
                bs_dex_horizon_services_native_ready(
                    &activity_preinit.native_services));
    debugPrintf("[java:horizon] post-constructor native_base=%d cookie=%d "
                "activity_constructed=%d alive=%d weak=%lu "
                "main_thread=%d native_ready=%d\n",
                activity_preinit.java_runtime.native_base_constructed,
                activity_preinit.java_runtime.event_token != NULL,
                activity_preinit.horizon_host.activity.constructed,
                activity_preinit.horizon_host.activity.alive,
                (unsigned long)activity_preinit.horizon_host.weak_outstanding,
                bs_dex_horizon_services_on_applet_thread(
                    &activity_preinit.native_services),
                bs_dex_horizon_services_native_ready(
                    &activity_preinit.native_services));
    void *activity = activity_preinit.context.activity;
    if (stop.kind == BS_DEX_STOP_RETURNED &&
        activity) {
        const char *attachment_exception = NULL;
        int attached = bs_dex_android_host_complete_attachment(
            &activity_preinit.platform, vm, &attachment_exception);
        if (attached != 1) {
            debugPrintf("[java:vm] original Activity.attach unavailable "
                        "(rc=%d exception=%s); onCreate blocked\n",
                        attached, attachment_exception ? attachment_exception : "none");
            release_activity_preinit();
            return;
        }
        /* The Java resources service must be observed from the same actual
         * Activity.attach as the Context/Window, with native ARSC ownership
         * and exact Java Resources class identity independently verified. */
        if (!bs_dex_horizon_java_attach_runtime_bind(
                &activity_preinit.java_attach_runtime,
                &activity_preinit.attachment,
                &activity_preinit.native_resources)) {
            debugPrintf("[java:resources] attached Java Resources cannot be "
                        "verified; onCreate blocked\n");
            release_activity_preinit();
            return;
        }
    }
    if (stop.kind == BS_DEX_STOP_RETURNED && activity &&
        bs_dex_vm_start_activity_oncreate(vm, activity, bs_dex_null())) {
        stop = bs_dex_vm_run(vm, 50000);
        trace_activity_stop("onCreate", stop);
        BsUnityJavaPreinitObservation original = {0};
        if (bs_dex_preinit_readback(vm, &host, activity, &original) &&
            bs_jni_unityplayer_observe_current_context(
                original.static_current_context) == BS_JNI_OK) {
            if (bs_jni_unityplayer_observe_current_activity(
                    original.static_current_activity) == BS_JNI_OK)
                return;
            bs_jni_unityplayer_observe_current_context(NULL);
        }
    }
    release_activity_preinit();
}

/* Host GameActivity fallback for UnityPlayer.currentContext/currentActivity.
 *
 * Android's UnityPlayer.<init>(Context) stores currentContext and then, when
 * the Context is an Activity, currentActivity. On Quest that Context is the
 * very same UnityPlayerGameActivity instance whose initializeNativeCode later
 * starts NativeCode. Executing the full Java onCreate requires a large part of
 * the Android framework (Activity.attach, PhoneWindow, IME, View tree) that
 * the DEX VM does not have yet. Like other Android-on-Switch so-loader ports,
 * the loader can instead publish the bound native GameActivity receiver for
 * both fields, in the original write order. That receiver is the exact object
 * passed to the original initializeNativeCode, so Unity sees one consistent
 * Activity identity.
 *
 * This is a HOST-PROVIDED Activity: the original Java onCreate bytecode is not
 * executed. Create sdmc:/switch/beat-saber-quest/strict-java-activity to keep
 * the strict DEX-only behaviour (libUnityOpenXR JNI_OnLoad then waits). */
#define BS_STRICT_JAVA_ACTIVITY_FLAG BS_GAME_ROOT "strict-java-activity"

static bool host_activity_fallback_used;

static void probe_activity_constructor_or_host(void) {
    probe_activity_constructor();
    if (bs_jni_unityplayer_has_current_activity()) {
        debugPrintf("[java:unityplayer] currentActivity published by original "
                    "DEX onCreate\n");
        return;
    }
    struct stat flag;
    if (stat(BS_STRICT_JAVA_ACTIVITY_FLAG, &flag) == 0) {
        debugPrintf("[java:unityplayer] strict Java Activity mode (%s present); "
                    "host GameActivity fallback disabled\n",
                    BS_STRICT_JAVA_ACTIVITY_FLAG);
        return;
    }
    void *receiver = gameactivity_native_proxy;
    if (!receiver ||
        !bs_jni_gameactivity_launch_matches(receiver, bs_jni_asset_manager(),
                                            gameactivity_java_configuration)) {
        debugPrintf("[java:unityplayer] host fallback unavailable: GameActivity "
                    "receiver is not bound to live assets/Configuration/Surface\n");
        return;
    }
    /* Same order as original UnityPlayer.<init>: Context, then Activity. */
    if (bs_jni_unityplayer_observe_current_context(receiver) != BS_JNI_OK) {
        debugPrintf("[java:unityplayer] host currentContext rejected: %s\n",
                    bs_jni_last_error());
        return;
    }
    if (bs_jni_unityplayer_observe_current_activity(receiver) != BS_JNI_OK) {
        debugPrintf("[java:unityplayer] host currentActivity rejected: %s\n",
                    bs_jni_last_error());
        bs_jni_unityplayer_observe_current_context(NULL);
        return;
    }
    host_activity_fallback_used = true;
    debugPrintf("[java:unityplayer] HOST GameActivity receiver %p published as "
                "UnityPlayer.currentContext/currentActivity (original Java "
                "onCreate NOT executed)\n", receiver);
}

static uintptr_t registered_native(const char *class_name, const char *name,
                                   const char *signature) {
    uintptr_t pointer = 0;
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *method = bs_jni_registration_at(i);
        if (!method || strcmp(method->class_name, class_name) ||
            strcmp(method->method_name, name) ||
            strcmp(method->signature, signature)) continue;
        if (pointer) return 0; /* ambiguous duplicate registration */
        pointer = method->function_address;
    }
    return pointer;
}

/* ART resolution order for a native method: RegisterNatives binding, else
 * the short JNI export name found by dlsym in the loaded libraries. */
static uintptr_t java_native(const char *class_name, const char *name,
                             const char *signature, const char *jni_export) {
    uintptr_t pointer = registered_native(class_name, name, signature);
    if (pointer) return pointer;
    uintptr_t found = 0;
    for (int i = 0; i < MODULE_COUNT; ++i) {
        uintptr_t entry = so_find_module_export(&modules[i], jni_export);
        if (!entry) continue;
        if (found) return 0; /* ambiguous across libraries */
        found = entry;
        debugPrintf("[java:unityplayer] %s.%s resolved by JNI export in %s\n",
                    class_name, name, module_names[i]);
    }
    return found;
}

static uintptr_t jni_env_slot(void *env, size_t slot) {
    uintptr_t table = 0, fn = 0;
    memcpy(&table, env, sizeof table);
    if (!table) return 0;
    memcpy(&fn, (const void *)(table + slot * sizeof(void *)), sizeof fn);
    return fn;
}

static bool jni_take_exception(void *env, const char *where) {
    uintptr_t check = jni_env_slot(env, 228), describe = jni_env_slot(env, 16);
    if (!check || !((uint8_t (*)(void *))check)(env)) return false;
    debugPrintf("[java:unityplayer] Java exception pending after %s\n", where);
    if (describe) ((void (*)(void *))describe)(env);
    return true;
}

/* Original UnityPlayerForGameActivity.<init> (disassembled from the
 * supplied classes.dex):
 *   UnityPlayer.<init>(activity, ContextType.GameActivity(a=1), events)
 *     -> currentContext/currentActivity
 *   UnityPlayer.initialize(frame, "")
 *     -> loadNative (libmain + NativeLoader.load; sets S.e=true)
 *     -> initJni(mContext, mContextType.a, "")
 *   nativeUnityPlayerForGameActivityInitialized(S.e ^ 1)
 * GameActivity.onCreate only calls initializeNativeCode afterwards. With the
 * host Activity fallback, invoke those two original natives with a host
 * UnityPlayerForGameActivity receiver in the same order. */
static bool unityplayer_natives_done;

static int unityplayer_constructor_natives(void) {
    if (unityplayer_natives_done) return 0;
    if (!host_activity_fallback_used) return 0; /* original DEX path owns it */
    void *env = bs_jni_env();
    void *player = bs_jni_unityplayer_host_object();
    void *context = gameactivity_native_proxy;
    if (!env || !player || !context) {
        debugPrintf("[java:unityplayer] constructor natives unavailable: env=%p "
                    "player=%p context=%p\n", env, player, context);
        return -EINVAL;
    }
    uintptr_t init_jni = java_native(
        "com/unity3d/player/UnityPlayer", "initJni",
        "(Landroid/content/Context;ILjava/lang/String;)V",
        "Java_com_unity3d_player_UnityPlayer_initJni");
    uintptr_t initialized = java_native(
        "com/unity3d/player/UnityPlayerForGameActivity",
        "nativeUnityPlayerForGameActivityInitialized", "(I)V",
        "Java_com_unity3d_player_UnityPlayerForGameActivity_"
        "nativeUnityPlayerForGameActivityInitialized");
    uintptr_t new_string = jni_env_slot(env, 167);
    uintptr_t delete_local = jni_env_slot(env, 23);
    if (!init_jni || !initialized || !new_string || !delete_local) {
        debugPrintf("[java:unityplayer] constructor natives not registered: "
                    "initJni=%#lx initialized=%#lx\n",
                    (unsigned long)init_jni, (unsigned long)initialized);
        return -ENOENT;
    }
    void *command_line = ((void *(*)(void *, const char *))new_string)(env, "");
    if (!command_line || jni_take_exception(env, "NewStringUTF")) return -EIO;
    debugPrintf("[java:unityplayer] HOST UnityPlayerForGameActivity %p: "
                "original initJni(context=%p, GameActivity=1, \"\")\n",
                player, context);
    ((void (*)(void *, void *, void *, int32_t, void *))init_jni)(
        env, player, context, 1, command_line);
    ((void (*)(void *, void *))delete_local)(env, command_line);
    if (jni_take_exception(env, "initJni")) return -EIO;
    debugPrintf("[java:unityplayer] original initJni returned; calling "
                "nativeUnityPlayerForGameActivityInitialized(0)\n");
    ((void (*)(void *, void *, int32_t))initialized)(env, player, 0);
    if (jni_take_exception(env, "nativeUnityPlayerForGameActivityInitialized"))
        return -EIO;
    unityplayer_natives_done = true;
    debugPrintf("[java:unityplayer] UnityPlayerForGameActivity constructor "
                "natives complete (host receiver; Java <init> NOT executed)\n");
    return 0;
}

/* UnityPlayer's Java resume/pause path reports the player's run state with
 * nativeUnityPlayerSetRunning(Z). libunity stores it in a flag that
 * UnityApplication::Loop (libgame) checks before every player-loop frame,
 * so without Java nothing renders until the host reports it. */
static bool unityplayer_running;

static void unityplayer_set_running(bool running) {
    if (unityplayer_running == running) return;
    void *env = bs_jni_env();
    void *player = bs_jni_unityplayer_host_object();
    /* libunity registers it on whichever UnityPlayer class owns the run
     * state for this context type; match on the method alone. */
    uintptr_t set_running = 0;
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *m = bs_jni_registration_at(i);
        if (!m || strcmp(m->method_name, "nativeUnityPlayerSetRunning") ||
            strcmp(m->signature, "(Z)V")) continue;
        if (!set_running)
            debugPrintf("[java:unityplayer] nativeUnityPlayerSetRunning on %s\n",
                        m->class_name);
        set_running = m->function_address;
    }
    if (!env || !player || !set_running) {
        debugPrintf("[java:unityplayer] nativeUnityPlayerSetRunning unavailable: "
                    "env=%p player=%p fn=%#lx\n", env, player,
                    (unsigned long)set_running);
        return;
    }
    debugPrintf("[java:unityplayer] original nativeUnityPlayerSetRunning(%d)\n",
                (int)running);
    ((void (*)(void *, void *, uint8_t))set_running)(env, player, running);
    if (jni_take_exception(env, "nativeUnityPlayerSetRunning")) return;
    unityplayer_running = running;
}

/* ---- Host UnityPlayerForGameActivity Java methods ---------------------
 * Each handler reproduces one original classes.dex method body for the host
 * receiver. Methods without a handler still fail as NoSuchMethodError. */
#define BS_UPGA "com/unity3d/player/UnityPlayerForGameActivity"

static void *unityplayer_main_thread;

/* UnityPlayerForGameActivity.setMainThread()V:
 *   if (m_MainThread != null) throw RuntimeException; m_MainThread = current */
static int host_upga_set_main_thread(void *user, void *env, void *receiver,
                                     const BSJNIValue *args, size_t argc,
                                     BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc; (void)result;
    if (!bs_jni_is_unityplayer_host(receiver)) return -EINVAL;
    void *current = armGetTls();
    if (unityplayer_main_thread && unityplayer_main_thread != current)
        return -EBUSY;
    unityplayer_main_thread = current;
    debugPrintf("[java:unityplayer] setMainThread -> thread tls=%p\n", current);
    return 0;
}

/* UnityPlayerForGameActivity.runningOnMainThread()Z */
static int host_upga_running_on_main_thread(void *user, void *env,
                                            void *receiver,
                                            const BSJNIValue *args,
                                            size_t argc, BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_unityplayer_host(receiver)) return -EINVAL;
    if (!unityplayer_main_thread) return -EINVAL; /* Java throws */
    result->z = unityplayer_main_thread == armGetTls();
    return 0;
}

/* UnityPlayer.shouldSetGameState()Z (declared on UnityPlayer):
 *   if (!PlatformSupport.TIRAMISU_SUPPORT) return false; ...
 * TIRAMISU_SUPPORT is Build.VERSION.SDK_INT >= 33. The bridge reports
 * ro.build.version.sdk = 32 (Quest manifest min/target 32), so the original
 * body returns false at @0005 without touching mActivity/isUaaLUseCase. */
#define BS_HOST_REPORTED_SDK_INT 32
static int host_up_should_set_game_state(void *user, void *env, void *receiver,
                                         const BSJNIValue *args, size_t argc,
                                         BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_unityplayer_host(receiver)) return -EINVAL;
    if (BS_HOST_REPORTED_SDK_INT >= 33) return -ENOSYS; /* later path unported */
    result->z = 0;
    return 0;
}

/* ---- APK class existence (PathClassLoader) ----------------------------- */
static bool bs_gameactivity_prepare_directory(const char *path);
static int app_class_resolver(void *user, const char *internal_name,
                              char *super_out, size_t super_cap) {
    (void)user;
    return bs_dex_class_index_lookup(internal_name, super_out, super_cap);
}

static void install_app_class_resolver(void) {
    BsUnityOriginalDexImage dex = {0};
    size_t classes = 0;
    if (bs_unity_apk_load_original_dex(BS_GAME_ROOT "beat-saber.apk", &dex)) {
        classes = bs_dex_class_index_build(dex.data, dex.size);
        bs_unity_apk_free_original_dex(&dex);
    }
    debugPrintf("[jni:class] original APK class_defs indexed=%lu\n",
                (unsigned long)classes);
    if (classes) bs_jni_set_app_class_resolver(app_class_resolver, NULL);
    /* Guest paths understood by asset_fs_bridge: /switch/beat-saber-quest
     * maps onto the staged APK; the Android external files dir maps onto
     * BS_GAME_ROOT/persistent. */
    (void)bs_gameactivity_prepare_directory(BS_GAME_ROOT "persistent");
    if (bs_jni_set_package_paths(
            "/switch/beat-saber-quest/beat-saber.apk",
            "/storage/emulated/0/Android/data/com.beatgames.beatsaber/files")
        != BS_JNI_OK)
        debugPrintf("[jni:class] package paths not published\n");
}

/* PlayAssetDeliveryUnityWrapper.playCoreApiMissing()Z: return a == null.
 * The host instance is only created with a == null (see host_pad_init). */
static int host_pad_play_core_api_missing(void *user, void *env,
                                          void *receiver,
                                          const BSJNIValue *args, size_t argc,
                                          BSJNIValue *result) {
    (void)user; (void)env; (void)args; (void)argc;
    if (!bs_jni_is_host_instance(receiver,
            "com/unity3d/player/PlayAssetDeliveryUnityWrapper"))
        return -EINVAL;
    result->z = 1;
    return 0;
}

/* UnityCoreAssetPacksStatusCallbacks.<init>()V: body is Object.<init>(). */
static int host_trivial_constructor(void *user, void *env, void *receiver,
                                    const BSJNIValue *args, size_t argc,
                                    BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)args; (void)argc;
    (void)result;
    return 0;
}

/* ---- Host PlayAssetDeliveryUnityWrapper --------------------------------- */
#define BS_PAD_WRAPPER "com/unity3d/player/PlayAssetDeliveryUnityWrapper"
static void *pad_wrapper_instance;

/* static PlayAssetDeliveryUnityWrapper.init(UnityPlayer, Context):
 *   synchronized: if (b != null) throw RuntimeException("...called only once");
 *   b = new PlayAssetDeliveryUnityWrapper(player, ctx); notifyAll(); return b;
 * <init>: a = null; getClass().getClassLoader().loadClass(
 *   "com.google.android.play.core.assetpacks.AssetPackManager") ...;
 *   ClassNotFoundException/NoSuchMethodException/SecurityException are caught
 *   (try @000a..@003c -> @003c return), leaving a == null.
 * The Quest APK does not define AssetPackManager, so loadClass throws and the
 * constructed wrapper keeps a == null. If the APK ever defined it, the
 * reflective path would be required: fail closed. */
static int host_pad_init(void *user, void *env, void *receiver,
                         const BSJNIValue *args, size_t argc,
                         BSJNIValue *result) {
    (void)user; (void)env; (void)receiver; (void)argc;
    if (pad_wrapper_instance) {
        return bs_jni_host_throw("java/lang/RuntimeException",
            "PlayAssetDeliveryUnityWrapper.init() should be called only "
            "once. Use getInstance() instead.") == BS_JNI_OK ? 0 : -EINVAL;
    }
    if (!bs_jni_is_unityplayer_host(args[0].l)) return -EINVAL;
    if (bs_dex_class_index_lookup(
            "com/google/android/play/core/assetpacks/AssetPackManager",
            NULL, 0))
        return -ENOSYS;
    void *instance = bs_jni_new_host_instance(BS_PAD_WRAPPER);
    if (!instance) return -ENOMEM;
    pad_wrapper_instance = instance;
    debugPrintf("[java:pad] PlayAssetDeliveryUnityWrapper.init -> %p "
                "(AssetPackManager absent from APK; play core wrapper=null)\n",
                instance);
    result->l = instance;
    return 0;
}

static void register_unityplayer_host_methods(void) {
    if (bs_jni_register_host_method(
            BS_PAD_WRAPPER, "init",
            "(Lcom/unity3d/player/UnityPlayer;Landroid/content/Context;)L"
            BS_PAD_WRAPPER ";", 1, host_pad_init, NULL) != BS_JNI_OK)
        debugPrintf("[java:pad] host init not registered\n");
    if (bs_jni_register_host_method(
            BS_PAD_WRAPPER, "playCoreApiMissing", "()Z", 0,
            host_pad_play_core_api_missing, NULL) != BS_JNI_OK ||
        bs_jni_register_host_method(
            "com/unity3d/player/UnityCoreAssetPacksStatusCallbacks", "<init>",
            "()V", 0, host_trivial_constructor, NULL) != BS_JNI_OK)
        debugPrintf("[java:pad] asset pack host methods not registered\n");
    static const struct {
        const char *klass, *name, *signature;
        BSJNIHostMethodFn fn;
    } methods[] = {
        {BS_UPGA, "setMainThread", "()V", host_upga_set_main_thread},
        {BS_UPGA, "runningOnMainThread", "()Z",
         host_upga_running_on_main_thread},
        {"com/unity3d/player/UnityPlayer", "shouldSetGameState", "()Z",
         host_up_should_set_game_state},
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; ++i)
        if (bs_jni_register_host_method(methods[i].klass, methods[i].name,
                                        methods[i].signature, 0,
                                        methods[i].fn, NULL) != BS_JNI_OK)
            debugPrintf("[java:unityplayer] host method %s%s not registered\n",
                        methods[i].name, methods[i].signature);
}

static bool bs_gameactivity_prepare_directory(const char *path) {
    if (mkdir(path, 0777) == 0) return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int bs_gameactivity_host_finish(void *user) {
    (void)user;
    /* A live homebrew Activity.finish request ends this actual applet's main
     * loop. Its original guest callback retains proper thread independence. */
    atomic_store_explicit(&gameactivity_exit_requested, true,
                          memory_order_release);
    return 0;
}

static int bs_gameactivity_host_window_flags(void *user, int32_t flags,
                                              int32_t mask) {
    (void)user;
    /* Android WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON (0x80) is
     * backed by Horizon's applet automatic-sleep setting. Other Android
     * window flags do not have verified Switch service equivalents. */
    const int32_t keep_screen_on = 0x80;
    if (!mask || (mask & ~keep_screen_on) || (flags & ~mask))
        return -ENOTSUP;
    Result rc = appletSetAutoSleepDisabled((flags & keep_screen_on) != 0);
    return R_SUCCEEDED(rc) ? 0 : -EIO;
}

static void *bs_gameactivity_observe_configuration(NWindow *window) {
    u32 width = 0, height = 0;
    if (!window || !nwindowIsValid(window) ||
        R_FAILED(nwindowGetDimensions(window, &width, &height)) ||
        !width || !height || R_FAILED(setInitialize()))
        return NULL;
    u64 language_code = 0;
    Result result = setGetSystemLanguage(&language_code);
    setExit();
    if (R_FAILED(result)) return NULL;

    /* Horizon reports a BCP-47 language tag such as en-US or zh-Hans. */
    char tag[9] = {0};
    memcpy(tag, &language_code, sizeof(language_code));
    char *suffix = strchr(tag, '-');
    BSJNIObservedLocale locale = {tag, "", "", ""};
    if (suffix) {
        *suffix++ = '\0';
        const size_t length = strlen(suffix);
        if (length == 2) locale.country = suffix;
        else if (length == 4) locale.script = suffix;
        else return NULL;
    }
    return bs_jni_configuration_from_host_display(width, height, &locale);
}

static int bs_gameactivity_host_window_insets(void *user, int32_t type_mask,
                                               int32_t edges[4]) {
    (void)user;
    /* A focused fullscreen application with a viewport covering the display
     * has no status, navigation or caption bars. Other masks depend on input
     * and display geometry data outside this viewport observation. */
    const int32_t bars = 0x07;
    if (!type_mask || (type_mask & ~bars) ||
        appletGetFocusState() != AppletFocusState_InFocus ||
        appletGetAppletType() != AppletType_Application)
        return -ENOTSUP;
    NWindow *window = nwindowGetDefault();
    s32 display_width = 0, display_height = 0;
    u32 width = 0, height = 0;
    if (!window || !nwindowIsValid(window) ||
        R_FAILED(appletGetDefaultDisplayResolution(&display_width,
                                                    &display_height)) ||
        R_FAILED(nwindowGetDimensions(window, &width, &height)) ||
        display_width <= 0 || display_height <= 0 ||
        width != (u32)display_width || height != (u32)display_height)
        return -ENOTSUP;
    memset(edges, 0, 4 * sizeof edges[0]);
    return 0;
}

/* Only dispatch the original Java->NativeCode callbacks after the verified
 * original initializeNativeCode has created its own native worker. Horizon
 * window/focus observations are fed into the exact registered JNI functions;
 * the independent native transport remains diagnostic. */
static void bs_pump_original_java_activity(bool running) {
    BsGameActivityJavaDriver *driver = &original_java_activity;
    if (!driver->native_code || driver->phase == BS_GA_JAVA_FAILED ||
        driver->phase == BS_GA_JAVA_DESTROYED) return;
    int rc = 0;
    if (!running) {
        if (driver->focused)
            rc = bs_gameactivity_java_driver_focus(driver, false);
        if (!rc && driver->phase == BS_GA_JAVA_RESUMED)
            rc = bs_gameactivity_java_driver_pause(driver);
        if (!rc && (driver->phase == BS_GA_JAVA_PAUSED ||
                    driver->phase == BS_GA_JAVA_STARTED))
            rc = bs_gameactivity_java_driver_stop(driver);
        if (!rc && driver->surface_attached)
            rc = bs_gameactivity_java_driver_surface_destroyed(driver);
        if (!rc && original_java_surface)
            bs_jni_revoke_surface(original_java_surface);
        if (!rc) {
            original_java_surface = NULL;
            original_java_window = NULL;
        }
        if (rc)
            debugPrintf("[gameactivity:java] stop callbacks failed %d\n", rc);
        return;
    }
    int focus = (int)appletGetFocusState();
    if (focus < 1 || focus > 3) return;
    if (driver->phase == BS_GA_JAVA_CREATED ||
        (driver->phase == BS_GA_JAVA_STOPPED && focus != 3)) {
        atomic_store(&watchdog_main_stage, 10);
        rc = bs_gameactivity_java_driver_start(driver);
    }
    if (rc) goto failed;
    NWindow *window = nwindowGetDefault();
    if (!window || !nwindowIsValid(window)) window = NULL;
    if (original_java_window && original_java_window != window) {
        if (driver->surface_attached)
            rc = bs_gameactivity_java_driver_surface_destroyed(driver);
        if (rc) goto failed;
        if (original_java_surface) bs_jni_revoke_surface(original_java_surface);
        original_java_window = NULL;
        original_java_surface = NULL;
    }
    if (window && driver->phase != BS_GA_JAVA_STOPPED) {
        u32 width = 0, height = 0;
        /* After the console framebuffer is released, the NWindow has no
         * configured buffer format until Mesa creates its surface; Android
         * reports such a window as RGBA_8888 (1). */
        const u32 window_format = window->format ? window->format : 1;
        if (R_SUCCEEDED(nwindowGetDimensions(window, &width, &height)) &&
            width && height) {
            if (!driver->surface_attached) {
                void *surface = original_java_surface;
                if (!surface) surface = bs_jni_surface_from_nwindow(window);
                if (!surface) return;
                atomic_store(&watchdog_main_stage, 11);
                rc = bs_gameactivity_java_driver_surface_created(
                    driver, surface, (int32_t)window_format,
                    (int32_t)width, (int32_t)height);
                /* onSurfaceCreatedNative can succeed even when the following
                 * onSurfaceChangedNative callback fails. In that case libgame
                 * still owns the Surface; retain its host reference until the
                 * original onSurfaceDestroyedNative/terminate path releases it. */
                if (!rc || driver->surface_attached) {
                    original_java_surface = surface;
                    original_java_window = window;
                } else {
                    bs_jni_revoke_surface(surface);
                    original_java_surface = NULL;
                    original_java_window = NULL;
                }
            } else if (width != original_java_width ||
                       height != original_java_height ||
                       window_format != original_java_format) {
                rc = bs_gameactivity_java_driver_surface_changed(
                    driver, (int32_t)window_format, (int32_t)width,
                    (int32_t)height);
            }
            if (!rc) {
                original_java_width = width;
                original_java_height = height;
                original_java_format = window_format;
            }
        }
    } else if (driver->surface_attached) {
        rc = bs_gameactivity_java_driver_surface_destroyed(driver);
        if (!rc && original_java_surface) {
            bs_jni_revoke_surface(original_java_surface);
            original_java_surface = NULL;
            original_java_window = NULL;
        }
    }
    if (rc) goto failed;
    if (focus == 1) {
        if (driver->phase == BS_GA_JAVA_STARTED ||
            driver->phase == BS_GA_JAVA_PAUSED) {
            atomic_store(&watchdog_main_stage, 12);
            rc = bs_gameactivity_java_driver_resume(driver);
        }
        atomic_store(&watchdog_main_stage, 13);
        if (!rc) rc = bs_gameactivity_java_driver_focus(driver, true);
        if (!rc) unityplayer_set_running(true);
    } else {
        unityplayer_set_running(false);
        if (driver->focused)
            rc = bs_gameactivity_java_driver_focus(driver, false);
        if (!rc && driver->phase == BS_GA_JAVA_RESUMED)
            rc = bs_gameactivity_java_driver_pause(driver);
        if (!rc && focus == 3 &&
            (driver->phase == BS_GA_JAVA_STARTED ||
             driver->phase == BS_GA_JAVA_PAUSED))
            rc = bs_gameactivity_java_driver_stop(driver);
    }
failed:
    if (rc)
        debugPrintf("[gameactivity:java] original lifecycle failed: %d phase=%d\n",
                    rc, driver->phase);
}

static void bs_pump_native_activity(bool running) {
    if (!native_activity_transport_ready) return;
    int lifecycle = bs_gameactivity_glue_observe_switch_lifecycle(
        &native_activity_transport, running);
    if (lifecycle < 0)
        debugPrintf("[gameactivity:transport] lifecycle observe=%d\n", lifecycle);
    if (running) {
        int window = bs_gameactivity_glue_observe_switch_window(
            &native_activity_transport);
        int focus = bs_gameactivity_glue_observe_switch_focus(
            &native_activity_transport);
        if (window < 0 || focus < 0)
            debugPrintf("[gameactivity:transport] window=%d focus=%d\n",
                        window, focus);
    }
    /* The original app source is registered with ALooper as identifier 1;
     * process its actual queued pipe commands on the owning thread. No guest
     * onAppCmd is installed until original Java Activity construction. */
    for (unsigned i = 0; i < BS_GA_COMMAND_QUEUE_CAPACITY; ++i) {
        void *data = NULL;
        int result = bs_looper_poll_once(0, NULL, NULL, &data);
        if (result == BS_GA_LOOPER_ID_MAIN &&
            data == &native_activity_transport.app.cmdPollSource) {
            BsGameActivityPollSource *source = (BsGameActivityPollSource *)data;
            source->process(&native_activity_transport.app, source);
        } else if (result == -1 || result == -2) {
            /* ALooper wake/callback may precede the queued command FD. */
            continue;
        } else if (result == -3 || result == -4) {
            break;
        } else if (result != BS_GA_LOOPER_ID_MAIN || !data) {
            /* Other native looper sources may exist. They retain ownership
             * of their callbacks and are never dispatched as GameActivity. */
            break;
        }
    }
    /* The original Java Handler is not constructed by a native ALooper.
     * Only a future VM-backed Handler binding with strong Runnable refs and
     * executable original Runnable.run may enable this owner-thread queue.
     * Keep actual dispatch in the same applet/ALooper pump as GameActivity. */
    if (bs_dex_horizon_main_looper_java_ready(
            &activity_preinit.main_looper)) {
        size_t executed = 0;
        if (!bs_dex_horizon_main_looper_drain(
                &activity_preinit.main_looper, 16, &executed))
            debugPrintf("[java:looper] original Runnable dispatch unavailable "
                        "or failed; queue halted without synthetic completion\n");
    }
}

static void jni_log(void *context, const char *message) {
    (void)context;
    debugPrintf("%s\n", message);
}

/* Verify a real original Quest Unity file through the same JNI AssetManager
 * contract that the Android engine uses. This exercises the actual staged
 * APK/OBB SD mapping; no asset bytes are logged or replaced. */
static bool bs_probe_original_asset_manager(void) {
    void *manager = bs_android_assetmanager_from_java(
        bs_jni_env(), bs_jni_asset_manager());
    if (!manager) {
        debugPrintf("[assets] genuine JNI AssetManager conversion failed\n");
        return false;
    }
    void *asset = bs_android_assetmanager_open(manager, "bin/Data/boot.config", 0);
    if (!asset) {
        debugPrintf("[assets] Unity boot.config cannot be opened through AAssetManager\n");
        return false;
    }
    const int64_t length = bs_android_asset_length64(asset);
    unsigned char buffer[32];
    const int read_count = bs_android_asset_read(asset, buffer, sizeof buffer);
    const int64_t reset = bs_android_asset_seek64(asset, 0, SEEK_SET);
    const void *asset_bytes = bs_android_asset_get_buffer(asset);
    const bool buffer_matches = asset_bytes && read_count > 0 &&
        memcmp(asset_bytes, buffer, (size_t)read_count) == 0 &&
        bs_android_asset_length(asset) == length;
    bs_android_asset_close(asset);
    if (length <= 0 || read_count <= 0 || reset != 0 || !buffer_matches) {
        debugPrintf("[assets] original Unity boot.config read/seek/getBuffer "
                    "failed (length=%lld, read=%d, seek=%lld, buffer=%d)\n",
                    (long long)length, read_count, (long long)reset,
                    (int)buffer_matches);
        return false;
    }
    void *directory = bs_android_assetmanager_open_dir(manager, "bin/Data");
    if (!directory) {
        debugPrintf("[assets] Unity bin/Data directory unavailable through "
                    "AAssetManager_openDir\n");
        return false;
    }
    bool found_boot = false;
    int visited = 0;
    const char *name;
    while (visited < 10000 &&
           (name = bs_android_asset_dir_get_next_file_name(directory))) {
        ++visited;
        if (!strcmp(name, "boot.config")) {
            found_boot = true;
            break;
        }
    }
    bs_android_asset_dir_close(directory);
    if (!found_boot) {
        debugPrintf("[assets] original Unity boot.config missing from real "
                    "asset directory enumeration (visited=%d)\n", visited);
        return false;
    }
    debugPrintf("[assets] original Unity boot.config verified through "
                "JNI AssetManager/AAsset read/getBuffer and directory "
                "listing (length=%lld, listed=%d)\n",
                (long long)length, visited);
    return true;
}

/* The original Quest Unity ELF expects these 15 MediaNDK symbols to be
 * GLOB_DAT pointers to const-char-pointer VARIABLES. This is an ABI check
 * against the actual relocated Unity GOT, not a fabricated media decoder.
 */
static bool bs_probe_original_unity_media_keys(void) {
    static const struct {
        const char *name;
        uintptr_t slot;
    } keys[] = {
        {"AMEDIAFORMAT_KEY_DURATION", 0x17499a8},
        {"AMEDIAFORMAT_KEY_WIDTH", 0x17499b0},
        {"AMEDIAFORMAT_KEY_HEIGHT", 0x17499b8},
        {"AMEDIAFORMAT_KEY_MIME", 0x17499c0},
        {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", 0x17499d0},
        {"AMEDIAFORMAT_KEY_SAMPLE_RATE", 0x17499d8},
        {"AMEDIAFORMAT_KEY_COLOR_FORMAT", 0x17499e0},
        {"AMEDIAFORMAT_KEY_COLOR_STANDARD", 0x17499e8},
        {"AMEDIAFORMAT_KEY_COLOR_RANGE", 0x17499f0},
        {"AMEDIAFORMAT_KEY_FRAME_RATE", 0x17499f8},
        {"AMEDIAFORMAT_KEY_ROTATION", 0x1749a00},
        {"AMEDIAFORMAT_KEY_LANGUAGE", 0x1749a08},
        {"AMEDIAFORMAT_KEY_ENCODER_DELAY", 0x1749a10},
        {"AMEDIAFORMAT_KEY_STRIDE", 0x1749a18},
        {"AMEDIAFORMAT_KEY_SLICE_HEIGHT", 0x1749a20}
    };
    const so_module *unity = &modules[2];
    if (!unity->load_virtbase) return false;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        if (keys[i].slot > unity->load_size ||
            sizeof(uintptr_t) > unity->load_size - keys[i].slot) {
            debugPrintf("[mediandk] original Unity DATA relocation "
                        "out of mapping: %s\n", keys[i].name);
            return false;
        }
        uintptr_t expected = bs_android_media_format_find_export(keys[i].name);
        uintptr_t actual = 0;
        memcpy(&actual, (const uint8_t *)unity->load_virtbase +
                        keys[i].slot, sizeof actual);
        if (!expected || actual != expected ||
            !*(const char *const *)actual) {
            debugPrintf("[mediandk] original Unity GLOB_DAT ABI mismatch "
                        "%s expected=%p found=%p\n",
                        keys[i].name, (void *)expected, (void *)actual);
            return false;
        }
    }
    AMediaFormat *format = AMediaFormat_new();
    if (!format) return false;
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    int32_t width = 0;
    bool format_ok = AMediaFormat_getInt32(
        format, AMEDIAFORMAT_KEY_WIDTH, &width) && width == 1920;
    if (AMediaFormat_delete(format) != BS_AMEDIA_OK)
        format_ok = false;
    if (!format_ok) {
        debugPrintf("[mediandk] native media format typed metadata failed\n");
        return false;
    }
    debugPrintf("[mediandk] validated 15 original Unity MediaNDK GLOB_DAT "
                "pointer-variable bindings and metadata operations\n");
    return true;
}

/* Exercise genuine relocated Quest TLSDESC GOT entries through the native
 * guest TLS module's validated AAPCS64 resolver, one witness per original
 * PT_TLS module. No unsafe original game entrypoint is called by this probe.
 */
static bool bs_probe_original_tls_descriptors(void) {
    static const struct {
        unsigned module_index;
        uintptr_t descriptor_vaddr;
        uintptr_t variable_offset;
    } samples[] = {
        {6,  0x75738u, 24u}, /* original libopenxr_loader.so */
        {8,  0x5098c8u, 8u}, /* original libOVRPlugin.so */
        {10, 0x5f070u, 0u},  /* original libovrplatformloader.so */
    };
    if (!bs_guest_tls_is_thread_attached()) {
        debugPrintf("[tls:runtime] failed: main thread has no Bionic PT_TLS\n");
        return false;
    }
    unsigned verified = 0;
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
        const so_module *module = &modules[samples[i].module_index];
        if (!bs_guest_tls_validate_original_desc(
                module, samples[i].descriptor_vaddr,
                samples[i].variable_offset)) {
            debugPrintf("[tls:runtime] real Quest TLSDESC witness failed "
                        "for %s\n", module->name);
            return false;
        }
        ++verified;
        debugPrintf("[tls:runtime] validated genuine %s TLSDESC slot=0x%lx\n",
                    module->name, (unsigned long)samples[i].descriptor_vaddr);
    }
    debugPrintf("[tls:runtime] PASS: %u genuine mapped Quest TLSDESC "
                "resolver invocations on initialized thread\n", verified);
    return verified == sizeof samples / sizeof samples[0];
}

typedef struct ElfScan {
    size_t file_size;
    size_t mapping_size;
    size_t load_segments;
    unsigned relocs;
    char error[140];
} ElfScan;

static void set_error(ElfScan *scan, const char *message) {
    snprintf(scan->error, sizeof(scan->error), "%s", message);
}

/* No game-specific offsets. Verify file size, ELF architecture and every
 * PT_LOAD range before passing the owner's files to the mapped .so loader.
 */
static bool inspect_arm64_elf(const char *filename, ElfScan *scan) {
    memset(scan, 0, sizeof(*scan));
    FILE *fp = fopen(filename, "rb");
    if (!fp) { set_error(scan, "missing / unreadable library"); return false; }
    bool good = false;
    if (fseek(fp, 0, SEEK_END) != 0) {
        set_error(scan, "cannot seek to end of ELF");
        goto finish;
    }
    long filesize = ftell(fp);
    if (filesize < (long)sizeof(Elf64_Ehdr) || (size_t)filesize > MAX_MODULE_IMAGE) {
        set_error(scan, "invalid or oversized .so image");
        goto finish;
    }
    scan->file_size = (size_t)filesize;
    if (fseek(fp, 0, SEEK_SET) != 0) {
        set_error(scan, "cannot rewind ELF");
        goto finish;
    }
    Elf64_Ehdr elf;
    if (fread(&elf, 1, sizeof(elf), fp) != sizeof(elf)) {
        set_error(scan, "truncated ELF header");
        goto finish;
    }
    if (memcmp(elf.e_ident, ELFMAG, SELFMAG) ||
        elf.e_ident[EI_CLASS] != ELFCLASS64 ||
        elf.e_ident[EI_DATA] != ELFDATA2LSB ||
        elf.e_machine != EM_AARCH64 || elf.e_type != ET_DYN ||
        elf.e_phentsize != sizeof(Elf64_Phdr) ||
        elf.e_phnum == 0 || elf.e_phnum > SO_MAX_SEGMENTS * 2) {
        set_error(scan, "expected 64-bit little-endian AArch64 shared ELF");
        goto finish;
    }
    /* The reused loader requires section headers and the shstrtab. */
    if (elf.e_shentsize != sizeof(Elf64_Shdr) ||
        elf.e_shnum == 0 || elf.e_shstrndx >= elf.e_shnum ||
        elf.e_shoff > scan->file_size ||
        elf.e_shnum > (scan->file_size - elf.e_shoff) / sizeof(Elf64_Shdr) ||
        elf.e_phoff > scan->file_size ||
        elf.e_phnum > (scan->file_size - elf.e_phoff) / sizeof(Elf64_Phdr)) {
        set_error(scan, "invalid ELF section/program header table");
        goto finish;
    }
    size_t last = 0;
    for (unsigned i = 0; i < elf.e_phnum; ++i) {
        Elf64_Phdr p;
        if (fseek(fp, (long)(elf.e_phoff + (uint64_t)i * sizeof(p)), SEEK_SET) != 0 ||
            fread(&p, 1, sizeof(p), fp) != sizeof(p)) {
            set_error(scan, "truncated program header");
            goto finish;
        }
        if (p.p_type != PT_LOAD) continue;
        ++scan->load_segments;
        if (p.p_memsz < p.p_filesz ||
            p.p_offset > scan->file_size ||
            p.p_filesz > scan->file_size - p.p_offset ||
            p.p_vaddr > MAX_MODULE_MAPPING ||
            p.p_memsz > MAX_MODULE_MAPPING - p.p_vaddr) {
            set_error(scan, "PT_LOAD segment escapes image or memory cap");
            goto finish;
        }
        size_t end = (size_t)(p.p_vaddr + p.p_memsz);
        if (end > last) last = end;
    }
    if (!scan->load_segments || !last) {
        set_error(scan, "ELF has no loadable segments");
        goto finish;
    }
    scan->mapping_size = (last + 4095) & ~(size_t)4095;
    good = true;
finish:
    fclose(fp);
    return good;
}

static void show_meminfo(void) {
    u64 total = 0, used = 0;
    Result total_result = svcGetInfo(&total, InfoType_TotalMemorySize,
                                     CUR_PROCESS_HANDLE, 0);
    Result used_result = svcGetInfo(&used, InfoType_UsedMemorySize,
                                    CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(total_result) && R_SUCCEEDED(used_result) && total >= used)
        printf("Process memory: %llu / %llu MiB used\n",
               (unsigned long long)(used >> 20),
               (unsigned long long)(total >> 20));
}

/* Linker mapping milestone. This executes no Unity/GameActivity initialization.
 * Do not invoke any engine export until all Android imports and TLS are bridged.
 */
static bool stage_library(int index) {
    char path[256];
    snprintf(path, sizeof(path), BS_GAME_ROOT "%s", module_names[index]);
    ElfScan scan;
    if (!inspect_arm64_elf(path, &scan)) {
        printf("FAIL %s: %s\n", module_names[index], scan.error);
        debugPrintf("FAIL preflight %s: %s\n", module_names[index], scan.error);
        return false;
    }
    printf("Staging %s: file %lu MiB, map %lu MiB\n", module_names[index],
           (unsigned long)(scan.file_size >> 20),
           (unsigned long)(scan.mapping_size >> 20));
    bs_console_update();
    debugPrintf("preflight %s file=%lu map=%lu segments=%u\n",
                module_names[index], (unsigned long)scan.file_size,
                (unsigned long)scan.mapping_size, (unsigned)scan.load_segments);

    module_mem[index] = memalign(0x1000, scan.mapping_size);
    if (!module_mem[index]) {
        printf("FAIL %s: no free memory for PT_LOAD mapping\n", module_names[index]);
        debugPrintf("FAIL allocation %s (%lu bytes)\n", module_names[index],
                    (unsigned long)scan.mapping_size);
        return false;
    }
    module_mapping_bytes[index] = scan.mapping_size;
    int rc = so_load(&modules[index], path, module_mem[index],
                     module_mapping_bytes[index]);
    if (rc) {
        printf("FAIL %s: so_load returned %d\n", module_names[index], rc);
        debugPrintf("FAIL so_load %s rc=%d\n", module_names[index], rc);
        return false;
    }
    if (!bs_guest_tls_register_module(&modules[index])) {
        printf("FAIL %s: unsupported PT_TLS memory layout\n", module_names[index]);
        debugPrintf("[tls] failed to register PT_TLS for %s\n", module_names[index]);
        return false;
    }
    printf("Staged %s: %p\n", module_names[index], modules[index].load_virtbase);
    debugPrintf("STAGED %s @ %p\n", module_names[index], modules[index].load_virtbase);
    return true;
}

/* ELF relocation order: reserve+stage all images, relocate all of them,
 * resolve cross-image symbols, then apply executable permissions. The
 * previous one-image-at-a-time routine could never bind imports from a
 * later-loaded shared object, even when that symbol existed in the APK.
 */
static bool complete_link(int count) {
    /* Offline-only port: do not initialize Horizon's bsd:u service.
     * The original guest socket imports are bound to explicit ENETDOWN/
     * EAI_FAIL handlers; no network connection or DNS lookup is attempted. */
    errno = 0;
    const bool sockets_disabled =
        bs_offline_socket(2, 1, 0) == -1 && errno == ENETDOWN;
    struct addrinfo *offline_result = (struct addrinfo *)(uintptr_t)1;
    const bool dns_disabled =
        bs_offline_getaddrinfo("example.invalid", NULL, NULL,
                               &offline_result) == EAI_FAIL &&
        offline_result == NULL;
    const bool offline_contract_verified = sockets_disabled && dns_disabled;
    const bool locale_abi_verified = bs_bionic_locale_abi_selftest() != 0;
    debugPrintf("[startup] offline mode: native bsd:u never initialized; "
                "guest sockets and DNS deliberately unavailable\n");
    if (!offline_contract_verified) {
        debugPrintf("[offline] FATAL: native socket/DNS disable contract "
                    "failed; refusing to relocate guest network imports\n");
        return false;
    }
    if (!locale_abi_verified) {
        debugPrintf("[locale:abi] warning: native C locale bridge unavailable; "
                    "original libc++ locale-dependent methods will fail\n");
    }
    if (bs_guest_tls_module_count() != 3) {
        printf("FAIL: expected three PT_TLS modules, got %lu\n",
               (unsigned long)bs_guest_tls_module_count());
        debugPrintf("[tls] invalid original PT_TLS module count\n");
        return false;
    }
    if (!bs_guest_tls_seal()) {
        debugPrintf("[tls] could not initialize per-thread TLS state\n");
        return false;
    }
    debugPrintf("[tls] statically assigned %lu genuine PT_TLS modules\n",
                (unsigned long)bs_guest_tls_module_count());
    debugPrintf("[link] starting relocation pass for %d libraries\n", count);
    printf("\nApplying ARM64 RELA/RELR relocations...\n");
    bs_console_update();
    for (int i = 0; i < count; ++i) {
        debugPrintf("[link] relocating %s\n", module_names[i]);
        int rc = so_relocate(&modules[i]);
        if (rc != 0) {
            printf("FAIL %s: relocation error %d\n", module_names[i], rc);
            debugPrintf("FAIL so_relocate %s rc=%d\n", module_names[i], rc);
            return false;
        }
    }
    /* Original libOVRPlugin .tdata has RELR entries. Snapshot only after
     * relocation, then install the Bionic TP block before invoking any guest
     * code. Otherwise all threads inherit a stale pre-relocation pointer. */
    if (!bs_guest_tls_snapshot_relocated_templates()) {
        debugPrintf("[tls] failed to capture relocated PT_TLS initializers\n");
        return false;
    }
    BsElfDependencyError dependency_error = {0};
    BsElfDependencyStatus dependency_status =
        bs_elf_dependency_order_modules(
            modules, (size_t)count, &bs_elf_dep_quest_android_policy,
            original_constructor_order, &dependency_error);
    if (dependency_status != BS_ELF_DEP_OK) {
        debugPrintf("[deps] original Quest DT_NEEDED ordering FAILED: "
                    "%s module=%lu dependency=%s\n",
                    bs_elf_dependency_status_string(dependency_status),
                    (unsigned long)dependency_error.module_index,
                    dependency_error.dependency);
        printf("FAIL: original ELF dependencies invalid. See loader.log\n");
        return false;
    }
    original_constructor_order_valid = true;
    debugPrintf("[deps] validated dependency-first constructor order for "
                "%d original Quest modules (constructors gated)\n", count);
    for (int i = 0; i < count; ++i)
        debugPrintf("[deps] %02d: %s\n", i,
                    modules[original_constructor_order[i]].name);
    main_guest_tls = memalign(0x1000, BIONIC_TLS_SIZE);
    if (!main_guest_tls || !bs_guest_tls_install_current_thread(main_guest_tls) ||
        bs_bionic_tls_finalize_current_thread() != 0) {
        debugPrintf("[tls] failed to initialize main thread guest TLS/stack guard\n");
        return false;
    }
    /* Original FMOD/Unity AAudio data callbacks run from an audout worker.
     * Install the relocated original Bionic PT_TLS and stack canary on that
     * worker before it can invoke any guest AArch64 callback. */
    bs_android_audio_set_guest_thread_lifecycle(
        bs_audio_worker_attach_guest, bs_audio_worker_detach_guest, NULL);
    debugPrintf("[link] relocation pass complete; resolving imports\n");
    debugPrintf("[tls] completed %lu genuine AArch64 TLSDESC relocations\n",
                (unsigned long)bs_guest_tls_relocation_count());
    printf("Resolving game exports and Android ABI bindings...\n");
    bs_console_update();
    unsigned unresolved_total = 0;
    unsigned unresolved_by_module[MODULE_COUNT] = {0};
    bs_platform_bind_graphics();
    for (int i = 0; i < count; ++i) {
        int unresolved = so_resolve(&modules[i], bs_platform_symbols,
                                    bs_platform_symbols_count, 0);
        if (unresolved < 0) {
            printf("FAIL %s: symbol resolution error %d\n",
                   module_names[i], unresolved);
            return false;
        }
        unresolved_total += (unsigned)unresolved;
        unresolved_by_module[i] = (unsigned)unresolved;
        printf("  %-23s %u missing relocation(s)\n",
               module_names[i], (unsigned)unresolved);
        debugPrintf("UNRESOLVED %s: %d relocation(s)\n",
                    module_names[i], unresolved);
    }
    debugPrintf("[link] resolved imports for all modules; %u relocations unresolved\n",
                unresolved_total);
    /* Inspect the actual Quest JNI entrypoints without entering guest code.
     * The bridge models the JNI function-table ABI, but Unity/GameActivity
     * execution remains forbidden while any platform service is missing.
     */
    if (bs_jni_init(jni_log, NULL) != BS_JNI_OK) {
        printf("FAIL: JNI bootstrap table initialization\n");
        return false;
    }
    const BSJNIGameActivityHostServices host_services = {
        .finish = bs_gameactivity_host_finish,
        .set_window_flags = bs_gameactivity_host_window_flags,
        .get_window_insets = bs_gameactivity_host_window_insets,
    };
    gameactivity_native_proxy = bs_jni_gameactivity_native_proxy(&host_services);
    install_app_class_resolver();
    register_unityplayer_host_methods();
    debugPrintf("[gameactivity:host] native method proxy=%p; "
                "Horizon finish, keep-screen-awake and fullscreen bar insets\n",
                gameactivity_native_proxy);
    unsigned jni_entrypoints = 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t entry = so_find_module_export(&modules[i], "JNI_OnLoad");
        if (entry) {
            ++jni_entrypoints;
            debugPrintf("[jni] %s JNI_OnLoad=%p (inspection only)\n",
                        module_names[i], (void *)entry);
        }
    }
    debugPrintf("[jni] detected %u original JNI_OnLoad exports; "
                "guest calls disabled, unresolved=%u\n",
                jni_entrypoints, unresolved_total);
    printf("Finalizing executable memory mappings...\n");
    bs_console_update();
    for (int i = 0; i < count; ++i) {
        /* The Android OpenXR loader finds no runtime on Horizon: route all
         * of its exports to the in-process Switch runtime instead. */
        if (!strcmp(module_names[i], "libopenxr_loader.so"))
            bs_openxr_hook_loader(&modules[i]);
        if (!strcmp(module_names[i], "libOVRPlugin.so"))
            bs_openxr_hook_ovrplugin(&modules[i]);
#ifdef BS_FLAT_PRESENT
        /* Without an OpenXR runtime libUnityOpenXR's display
         * subsystem stays registered after "System Shutdown" and its flags
         * callback (0xa1138) always reports 0x14; bit 2 (XR owns presentation)
         * stops Unity from initialising/presenting the flat display. Keep only
         * bit 4 so the original flat EGL present path runs. */
        if (!strcmp(module_names[i], "libUnityOpenXR.so")) {
            uint32_t *w = (uint32_t *)((uintptr_t)modules[i].load_base + 0xa114c);
            if (*w == 0x5280028bu) {
                *w = 0x5280020bu; /* mov w11,#0x14 -> mov w11,#0x10 */
                debugPrintf("[xr] flat presentation: patched UnityOpenXR display flags\n");
            } else {
                debugPrintf("[xr] UnityOpenXR flags word mismatch %08x; not patched\n", *w);
            }
        }
#endif
        debugPrintf("[link] finalizing %s\n", module_names[i]);
        Result rc = so_finalize(&modules[i]);
        if (R_FAILED(rc)) {
            printf("FAIL %s: executable mapping error 0x%08x\n", module_names[i], rc);
            debugPrintf("[link] FAIL %s: executable mapping error 0x%08x\n",
                        module_names[i], rc);
            return false;
        }
        so_flush_caches(&modules[i]);
        so_free_temp(&modules[i]);
        debugPrintf("MAPPED %s @ %p\n",
                    module_names[i], modules[i].load_virtbase);
    }
    /* Mesa maps heap pages into the GPU address space; heap memory it touched
     * cannot later back svcMapProcessCodeMemory. Probe only after every
     * module's executable mapping is final, still before any guest code. */
    g_egl_host_ready = bs_egl_host_preflight() == 0;
    debugPrintf("[egl:preflight] host GLES context ready=%d\n", g_egl_host_ready);
    /* An exact match of the small original libmain.so JNI_OnLoad is the
     * first real Quest native entrypoint we can validate independently. It
     * only registers the game's original native methods through JNI tables.
     * The exact ELF size, CRC and entrypoint offset are checked in the
     * lifecycle bridge before invoking the original function pointer.
     */
    const bool original_tls_verified = bs_probe_original_tls_descriptors();
    const bool bootstrap_abi_verified = original_tls_verified &&
        bs_runtime_probe_bootstrap_abi();
    const bool unity_media_keys_verified = bs_probe_original_unity_media_keys();
    /* Exercise the actual FFmpeg extractor/decoder using a known G.711
     * stream. Metadata-only MediaNDK implementations cannot pass this gate. */
    const bool media_decoder_verified = unity_media_keys_verified &&
        bs_android_media_decode_selftest();
    debugPrintf("[mediandk] original exports=%d extractor/decoder=%d\n",
                unity_media_keys_verified, media_decoder_verified);
    bool native_registered = original_tls_verified &&
        bs_unity_start_verified_native_loader(&modules[0],
                                               unresolved_by_module[0]);
    debugPrintf("[startup] original libmain JNI_OnLoad %s\n",
                native_registered ? "completed" : "blocked/failed");
    const bool original_assets_ready = bs_probe_original_asset_manager();
    if (original_assets_ready && native_activity_transport_ready) {
        void *manager = bs_android_assetmanager_from_java(
            bs_jni_env(), bs_jni_asset_manager());
        void *configuration = manager ? bs_android_configuration_new() : NULL;
        if (configuration && bs_gameactivity_glue_bind_configuration(
                                 &native_activity_transport, configuration,
                                 manager) == 0) {
            native_activity_configuration = configuration;
            /* Process the real NDK configuration command before inspecting
             * the guest android_app. The Java Activity is a separate gate. */
            bs_pump_native_activity(true);
            char language[4] = {0}, country[4] = {0};
            bs_android_configuration_get_language(configuration, language);
            bs_android_configuration_get_country(configuration, country);
            debugPrintf("[gameactivity:config] native AConfiguration "
                        "bound to original AssetManager; locale=%s-%s "
                        "sdk=%d (Horizon Settings observed when available)\n",
                        language[0] ? language : "unknown",
                        country[0] ? country : "unknown",
                        bs_android_configuration_get_sdk_version(configuration));
        } else {
            if (configuration) bs_android_configuration_delete(configuration);
            debugPrintf("[gameactivity:config] native configuration "
                        "binding unavailable\n");
        }
    }
    bool il2cpp_onload = native_registered &&
        bs_unity_start_verified_il2cpp_onload(&modules[3]);
    debugPrintf("[startup] original libil2cpp JNI_OnLoad %s\n",
                il2cpp_onload ? "completed" : "blocked/failed");
    /* Inspect the exact original libgame GameActivity_register call graph,
     * its JNI table slots, and its own imports without entering guest code.
     * Real Java Activity/lifecycle methods are still missing; the preflight
     * always preserves that explicit blocker instead of manufacturing JNI
     * handles or a fake android_app to claim Unity has started. */
    BsGameActivityPreflight game_activity = {0};
    const bool game_activity_registered =
        bs_unity_preflight_original_gameactivity(&modules[4],
            unresolved_by_module[4], &game_activity);
    debugPrintf("[startup] original GameActivity preflight: %s "
                "blocks=0x%x missing_JNI_slots=0x%x own_imports=%lu "
                "source=%d code=%d plt=%d\n",
                game_activity_registered ? "ready" : "blocked",
                (unsigned)game_activity.blockers,
                (unsigned)game_activity.missing_jni_slots,
                (unsigned long)game_activity.own_unresolved_relocations,
                (int)game_activity.source_verified,
                (int)game_activity.executable_verified,
                (int)game_activity.local_plt_verified);
    /* A separate, stricter audit compares the fully mapped original
     * GameActivity, native app glue and Unity-facing entrypoints against all
     * eleven pinned signatures and executable code spans. This remains
     * useful even before the Java Activity host is executable. */
    BsGameActivityTopology activity_topology = {0};
    const bool topology_verified =
        bs_unity_preflight_gameactivity_topology(&modules[4],
                                                 &activity_topology);
    debugPrintf("[startup] original GameActivity native topology: %s "
                "verified=%lu/%lu mismatch=0x%08lx blockers=0x%08lx\n",
                topology_verified ? "verified" : "blocked",
                (unsigned long)activity_topology.entrypoints_verified,
                (unsigned long)activity_topology.entrypoints_checked,
                (unsigned long)activity_topology.entrypoint_mismatch,
                (unsigned long)activity_topology.blockers);
    /* The original GameActivity JNI_OnLoad contains only Java registration,
     * class/member lookup and Android logging, as established from its
     * pinned ARM64 call graph. This isolated step may record the game's 24
     * real native callbacks without invoking initializeNativeCode, onCreate,
     * android_main or any Java Activity method. It needs neither XR nor
     * media readiness, but it still needs zero strong libgame imports. */
    const bool native_activity_registered = topology_verified &&
        bs_unity_start_verified_gameactivity_registration(
            &modules[4], unresolved_by_module[4]);
    debugPrintf("[startup] original GameActivity JNI registration: %s "
                "(actual Java lifecycle remains separately gated)\n",
                native_activity_registered ? "registered" : "blocked/failed");
    NWindow *initial_window = nwindowGetDefault();
    original_java_surface = bs_jni_surface_from_nwindow(initial_window);
    if (original_java_surface) original_java_window = initial_window;
    gameactivity_java_configuration =
        bs_gameactivity_observe_configuration(initial_window);
    void *asset_manager = bs_jni_asset_manager();
    const bool lifecycle_host_bound = original_assets_ready &&
        bs_looper_for_thread() &&
        bs_jni_gameactivity_bind_host(gameactivity_native_proxy, asset_manager,
                                     gameactivity_java_configuration,
                                     original_java_surface) == BS_JNI_OK;
    BSJNIGameActivityEvidence java_evidence = {0};
    const bool java_evidence_available =
        bs_jni_gameactivity_readiness(&java_evidence) == BS_JNI_OK;
    if (java_evidence_available) {
        debugPrintf("[gameactivity] Java host prerequisites: blockers=0x%08x "
                    "original_methods=%lu/%lu config_fields=%lu observed "
                    "%lu defaulted locales=%lu "
                    "surfaces=%d host_services=0x%02x first=%s\n",
                    (unsigned)java_evidence.blocker_flags,
                    (unsigned long)java_evidence.captured_native_methods,
                    (unsigned long)java_evidence.expected_native_methods,
                    (unsigned long)java_evidence.observed_configuration_fields,
                    (unsigned long)java_evidence.defaulted_configuration_fields,
                    (unsigned long)java_evidence.observed_locale_count,
                    java_evidence.verified_native_surface_count,
                    (unsigned)java_evidence.most_complete_host_service_mask,
                    java_evidence.first_blocker);
    }
    /* A Java GameActivity NativeCode uses a DIFFERENT android_app allocated
     * by original libgame.so. The already-running native-only transport cannot
     * be passed to its android_main. */
    const bool native_glue_ready = native_activity_transport_ready &&
        bs_gameactivity_glue_app(&native_activity_transport) != NULL;
    const bool jni_activity_host_ready = java_evidence_available &&
        java_evidence.blocker_flags == 0 &&
        java_evidence.actual_java_activity_ready &&
        bs_jni_gameactivity_java_ready();
    debugPrintf("[gameactivity:integration] original natives=%d "
                "native transport=%d JNI Activity proxy ready=%d "
                "Java configuration=%p Java receiver=%p\n",
                native_activity_registered, native_glue_ready,
                jni_activity_host_ready, gameactivity_java_configuration,
                gameactivity_native_proxy);
    /* DEX GameActivity.onCreate @0x22ef52..0x22ef94 passes getFilesDir(),
     * getObbDir(), getExternalFilesDirs(null)[0], getAssets() and the
     * Resources Configuration, in this exact order. These are explicit
     * directory mappings on Switch, provisioned before allowing initialization. */
    original_java_launch = (BsGameActivityJavaLaunch){
        .activity = gameactivity_native_proxy,
        .asset_manager = asset_manager,
        .configuration = gameactivity_java_configuration,
        .first_path = BS_GAME_ROOT "files/",
        .second_path = BS_GAME_ROOT "obb/",
        .third_path = BS_GAME_ROOT "external-files/"
    };
    const bool game_activity_host_ready = original_java_dex_parsed &&
        native_activity_registered &&
        topology_verified && lifecycle_host_bound && jni_activity_host_ready &&
        bs_gameactivity_prepare_directory(original_java_launch.first_path) &&
        bs_gameactivity_prepare_directory(original_java_launch.second_path) &&
        bs_gameactivity_prepare_directory(original_java_launch.third_path);
    /* Vulkan handoff preflight is intentionally read-only: no instance,
     * surface, queue, swapchain or draw call. A valid VI window and published
     * native extensions are necessary, but not sufficient, for Unity XR. */
    BsVulkanPregraphicsProbe graphics_probe = {0};
    /* Unity 6000.3 on this package selects GLES (it calls eglGetDisplay and
     * never loads libvulkan), so a verified host GLES context satisfies the
     * graphics prerequisite. The Vulkan probe is kept as telemetry. */
    const bool vulkan_entry =
        bs_vulkan_probe_pregraphics(nwindowGetDefault(), &graphics_probe);
    const bool native_graphics_entry = vulkan_entry || g_egl_host_ready;
    debugPrintf("[graphics:handoff] NVK=%d entrypoints=%d window=%d "
                "VK_KHR_surface=%d VK_NN_vi_surface=%d extensions=%lu "
                "result=%d boundary=%s (VkInstance NOT created)\n",
                graphics_probe.nvk_available,
                graphics_probe.instance_functions_available,
                graphics_probe.window_valid,
                graphics_probe.khr_surface_extension,
                graphics_probe.vi_surface_extension,
                (unsigned long)graphics_probe.extension_count,
                graphics_probe.enumeration_result,
                vulkan_entry ? "preflight-ready" :
                g_egl_host_ready ? "gles-ready" : "blocked");
    /* The Vulkan probe only verifies that the guarded boundary is available.
     * The guest must reach it before any instance or surface is created. */
    original_engine_caps = (BsUnityStartCapabilities){
        .progress = &original_startup,
        .unresolved_relocations = unresolved_total,
        .constructor_order = original_constructor_order_valid ?
                             original_constructor_order : NULL,
        .constructor_order_count = original_constructor_order_valid ?
                                   (size_t)count : 0,
        .assets_ready = original_assets_ready,
        .bionic_abi_verified = bootstrap_abi_verified,
        .game_activity_ready = game_activity_host_ready,
        .media_ndk_ready = media_decoder_verified,
        .vulkan_preflight_ready = native_graphics_entry,
        .unity_window_graphics = unity_window_graphics,
        .java_driver = &original_java_activity,
        .java_launch = &original_java_launch,
        .probe_activity_constructor = probe_activity_constructor_or_host,
        .before_native_code = unityplayer_constructor_natives,
    };
    const bool engine_started = bs_continue_original_startup(count);
    const bool boundary_seen = bs_vulkan_graphics_boundary_seen();
    if (!engine_started && !boundary_seen)
        debugPrintf("[startup] original libunity/libgame bootstrap gated on missing services\n");
    const char *native_code_status = engine_started ? "started" :
        (boundary_seen ? "stopped at graphics boundary" : "gated");
    debugPrintf("COMPLETE modules=%d unresolved=%u (original Unity NativeCode %s)\n",
                count, unresolved_total, native_code_status);
    printf("\nMapped %d original game libraries\n", count);
    printf("Unresolved relocations: %u\n", unresolved_total);
    printf("Verified original DT_NEEDED constructor order: %s\n",
           original_constructor_order_valid ? "yes" : "failed");
    printf("TLS descriptors: %lu; modules: %lu\n",
           (unsigned long)bs_guest_tls_relocation_count(),
           (unsigned long)bs_guest_tls_module_count());
    printf("Original mapped TLS descriptor ABI: %s\n",
           original_tls_verified ? "validated" : "blocked/failed");
    printf("Original Unity MediaNDK key ABI: %s (decoder %s)\n",
           unity_media_keys_verified ? "validated" : "blocked/failed",
           media_decoder_verified ? "verified" : "blocked/failed");
    printf("Original JNI entrypoints found: %u (engine JNI gated)\n", jni_entrypoints);
    printf("Original libmain JNI registration: %s\n",
           native_registered ? "original JNI_OnLoad returned" : "blocked/failed");
    printf("Original IL2CPP JNI VM registration: %s\n",
           il2cpp_onload ? "original JNI_OnLoad returned" : "blocked/failed");
    printf("Original GameActivity registration preflight: %s (blockers 0x%x)\n",
           game_activity_registered ? "ready" : "blocked",
           (unsigned)game_activity.blockers);
    printf("Original GameActivity native entrypoints: %lu/%lu verified\n",
           (unsigned long)activity_topology.entrypoints_verified,
           (unsigned long)activity_topology.entrypoints_checked);
    printf("Original GameActivity 24 native methods: %s\n",
           native_activity_registered ? "original JNI_OnLoad returned" :
                                        "blocked/failed");
    printf("Native GameActivity event transport: %s (Java host separate)\n",
           native_activity_transport_ready ? "active" : "unavailable");
    printf("Original GameActivity.NativeCode lifecycle: %s\n", native_code_status);
    if (java_evidence_available)
        printf("Java Activity remaining: %s\n", java_evidence.first_blocker);
    printf("Original Unity AssetManager access: %s\n",
           original_assets_ready ? "verified" : "blocked/failed");
    printf("Pregraphics NVK handoff: %s (no VkInstance created)\n",
           native_graphics_entry ? "native driver/window verified" :
                                   "unavailable; inspect loader.log");
    return true;
}

int main(void) {
    /* The default libnx startup already attempts to mount the SD card. */
    Result rc = fsdevGetDeviceFileSystem("sdmc") ? 0 : fsdevMountSdmc();
    if (R_SUCCEEDED(rc)) {
        debugPrintf("\n[startup] SD mounted; initializing console\n");
    }
    consoleInit(NULL);
    atomic_store(&g_console_active, true);
    if (R_SUCCEEDED(rc)) {
        debugPrintf("[startup] console initialized\n");
        bs_sampler_register_current(0);
        watchdog_start();
    }
    /* Allow Horizon to expose independent left/right Joy-Cons on separate
     * Npad IDs for the game's two-handed input bridge. A paired JoyDual
     * controller continues to use one Npad ID and two six-axis handles. */
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    /* Initialize physical hand devices before any original game/XR code may
     * request action state. Their samples are consumed by the input bridge. */
    const int joycon_init_status = bs_joycon_init();
    if (joycon_init_status != 0)
        debugPrintf("[joycon] HID hand input initialization failed: %d\n",
                    joycon_init_status);
    printf("Beat Saber Quest / ARM64 Unity loader\n");
    printf("Experimental ELF mapping diagnostics\n\n");
    if (R_FAILED(rc)) {
        printf("Could not mount SD card (libnx 0x%08x).\n", rc);
        bs_console_update();
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & (HidNpadButton_B | HidNpadButton_Plus))
                break;
        }
        bs_joycon_shutdown();
        bs_host_console_release_for_graphics();
        return 1;
    }

    printf("Expected: " BS_GAME_ROOT "\n");
    printf("Quest game: 1.46.0, Unity 6000.3.19f1\n");
    printf("Logs: " BS_GAME_ROOT "loader.log\n\n");
    struct stat window_graphics_flag;
    unity_window_graphics =
        stat(BS_GAME_ROOT "unity-window-graphics", &window_graphics_flag) == 0 &&
        S_ISREG(window_graphics_flag.st_mode);
    const int transport_rc = bs_gameactivity_glue_prepare_native(
        &native_activity_transport);
    native_activity_transport_ready = transport_rc == 0;
    debugPrintf("[gameactivity:transport] native pipe/looper %s (rc=%d); "
                "Java Activity ready=0\n",
                native_activity_transport_ready ? "prepared" : "unavailable",
                transport_rc);
    show_meminfo();
    /* Verify the ORIGINAL Java bytecode from the staged Quest APK. The APK
     * contains an uncompressed classes.dex (not a standalone staged DEX).
     * A valid byte sequence is necessary for a future Java interpreter, but
     * does NOT mean Activity constructors were executed or that
     * UnityPlayer.currentActivity has been assigned. Keep this read-only
     * preflight independent of the original native relocation milestone. */
    BsUnityOriginalDexImage original_dex = {0};
    const bool original_dex_valid = bs_unity_apk_load_original_dex(
        BS_GAME_ROOT "beat-saber.apk", &original_dex) != 0;
    debugPrintf("[java:dex] original APK classes.dex %s (bytes=%lu; "
                "Java VM/Activity execution NOT established)\n",
                original_dex_valid ? "verified" : "unavailable or mismatched",
                (unsigned long)original_dex.size);
    if (original_dex_valid) {
        BsDexHost callbacks = {
            .validate_activity = reject_existing_activity,
        };
        BsDexVM *vm = bs_dex_vm_create(original_dex.data, original_dex.size,
                                       &callbacks);
        original_java_dex_parsed = vm != NULL;
        bs_dex_vm_destroy(vm);
    } else {
        debugPrintf("[java:vm] original APK absent/mismatched; "
                    "Java preinit blocked before bytecode execution\n");
    }
    bs_unity_apk_free_original_dex(&original_dex);
    bool ready = true;
    size_t total_bytes = 0;
    for (int i = 0; i < CORE_MODULE_COUNT; ++i) {
        char path[256];
        snprintf(path, sizeof(path), BS_GAME_ROOT "%s", module_names[i]);
        ElfScan scan;
        if (inspect_arm64_elf(path, &scan)) {
            printf("OK %-14s map=%lu MiB\n", module_names[i],
                   (unsigned long)(scan.mapping_size >> 20));
            total_bytes += scan.mapping_size;
        } else {
            printf("Missing/invalid %-14s %s\n", module_names[i], scan.error);
            ready = false;
        }
    }
    printf("\nCore ELF mapping: %lu MiB\n", (unsigned long)(total_bytes >> 20));
    printf("Original Unity code is NOT initialized.\n");
    printf("\nY: native Android bridge self-test\n");
    printf("Hold L + D-pad: %s\n", unity_window_graphics ?
           "move Unity graphics window" : "adjust simulated headset");
    printf("B / +: exit\n");
    debugPrintf("[startup] core preflight ready=%d; submitting first frame\n", ready);
    bs_console_update();
    if (ready) {
        debugPrintf("[startup] automatically staging Quest libraries\n");
        printf("\nStaging full original Unity/IL2CPP dependency set...\n");
        int staged = 0;
        /* Observe the applet after its real main-loop tick and between each
         * expensive ELF mapping operation. Native state remains telemetry
         * until an original Java Activity has actually been constructed. */
        while (staged < MODULE_COUNT && appletMainLoop()) {
            bs_pump_native_activity(true);
            if (!stage_library(staged)) break;
            ++staged;
        }
        if (staged == MODULE_COUNT) {
            if (!complete_link(MODULE_COUNT))
                printf("Link stage incomplete. Check loader.log\n");
        } else {
            printf("Stopped at %s. See loader.log\n", module_names[staged]);
        }
        bs_console_update();
    }
    BsHeadsetAngles headset = {0};
    bool applet_running;
    debugPrintf("[startup] host main loop tls=%p (lifecycle pump=%d)\n",
                armGetTls(), (int)original_java_started);
    while ((applet_running = appletMainLoop()) &&
           !atomic_load_explicit(&gameactivity_exit_requested,
                                 memory_order_acquire)) {
        if (bs_vulkan_graphics_boundary_seen()) {
            debugPrintf("[graphics:boundary] verified guest Vulkan entry; "
                        "stopping before graphics initialization\n");
            printf("\nOriginal Unity reached Vulkan graphics entry; stopped.\n");
            bs_console_update();
            break;
        }
        atomic_fetch_add(&watchdog_main_iterations, 1);
        atomic_store(&watchdog_main_stage, 1);
        bs_pump_native_activity(applet_running);
        if (original_startup.waiting_for_activity &&
            bs_jni_unityplayer_has_current_activity())
            bs_continue_original_startup(MODULE_COUNT);
        atomic_store(&watchdog_main_stage, 2);
        if (original_java_started)
            bs_pump_original_java_activity(applet_running);
        atomic_store(&watchdog_main_stage, 3);
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (padGetButtons(&pad) & HidNpadButton_L) {
            int horizontal = !!(down & HidNpadButton_Right) - !!(down & HidNpadButton_Left);
            int vertical = !!(down & HidNpadButton_Up) - !!(down & HidNpadButton_Down);
            if (unity_window_graphics) {
                if (horizontal || vertical)
                    bs_vulkan_move_unity_window(horizontal, -vertical);
            } else if (horizontal || vertical) {
                bs_headset_turn(&headset, horizontal, vertical);
                bs_openxr_set_head_angles(headset.yaw_degrees, headset.pitch_degrees);
                float orientation[4];
                bs_headset_orientation(&headset, orientation);
                printf("Headset yaw %d, pitch %d degrees\n",
                       headset.yaw_degrees, headset.pitch_degrees);
                debugPrintf("[headset] yaw=%d pitch=%d quaternion=(%.4f %.4f %.4f %.4f)\n",
                            headset.yaw_degrees, headset.pitch_degrees,
                            orientation[0], orientation[1], orientation[2], orientation[3]);
                bs_console_update();
            }
        }
        if (down & HidNpadButton_Y) debugPrintf("[input] Y: bridge self-test\n");
        /* Once the game runs, B/+/Y are game controls; quit with + and -. */
        if (original_java_started) {
            u64 held = padGetButtons(&pad);
            if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) break;
            svcSleepThread(16000000ULL);
            continue;
        }
        if (down & (HidNpadButton_B | HidNpadButton_Plus)) break;
        if (down & HidNpadButton_Y) bs_runtime_selftest();
        svcSleepThread(16000000ULL);
    }
    if (original_java_started) {
        bs_pump_original_java_activity(false);
        int java_shutdown = bs_gameactivity_java_driver_destroy(
            &original_java_activity);
        debugPrintf("[gameactivity:java] original NativeCode terminate=%d\n",
                    java_shutdown);
    }
    if (!original_java_activity.native_code && original_java_surface) {
        bs_jni_revoke_surface(original_java_surface);
        original_java_surface = NULL;
        original_java_window = NULL;
    }
    bs_pump_native_activity(false);
    /* Stop and join guest audio callbacks while the JNI and native looper
     * owners still exist; exit hook detaches the audio worker's guest TLS. */
    bs_android_audio_shutdown();
    if (activity_preinit.vm && !original_java_activity.native_code) {
        bs_jni_unityplayer_observe_current_activity(NULL);
        bs_jni_unityplayer_observe_current_context(NULL);
        release_activity_preinit();
    }
    if (host_activity_fallback_used) {
        bs_jni_unityplayer_observe_current_activity(NULL);
        bs_jni_unityplayer_observe_current_context(NULL);
        host_activity_fallback_used = false;
        unityplayer_natives_done = false;
        unityplayer_main_thread = NULL;
    }
    if (native_activity_transport_ready) {
        int close_rc = bs_gameactivity_glue_close_native(
            &native_activity_transport);
        debugPrintf("[gameactivity:transport] closed rc=%d\n", close_rc);
        native_activity_transport_ready = false;
    }
    if (native_activity_configuration) {
        bs_android_configuration_delete(native_activity_configuration);
        native_activity_configuration = NULL;
    }
    bs_joycon_shutdown();
    watchdog_shutdown();
    if (atomic_exchange(&g_console_active, false)) consoleExit(NULL);
    return 0;
}
