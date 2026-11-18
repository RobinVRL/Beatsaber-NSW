/* Initial Bionic/Android C-symbol bridge for Unity 6 ABI analysis.
 * Trivial AAPCS64 C functions are mapped to libnx/newlib equivalents.
 * No placeholder success stubs for VR, JNI, entitlements, graphics or pthread.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <ctype.h>
#include <malloc.h>
#include <zlib.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "platform_bridge.h"
#include "so_util.h"
#include "util.h"
#include "bionic_threads.h"
#include "bionic_mmap.h"
#include "asset_fs_bridge.h"
#include "android_window_bridge.h"
#include "android_choreographer.h"
#include "android_audio_bridge.h"
#include "vulkan_bridge.h"
#include "egl_bridge.h"
#include "bionic_fortify.h"
#include "guest_tls.h"
#include "bionic_strncpy_chk2.h"
#include "bionic_misc_bridge.h"
#include "bionic_thread_atexit.h"
#include "android_media_format.h"
#include "offline_network_bridge.h"

void *bs_ovr_platform_symbol(const char *name); /* ovr_platform_offline.c */
#include "android_sensor_bridge.h"
#include "bionic_locale_bridge.h"
#include "bionic_multibyte_bridge.h"
#include "bionic_vectored_io.h"
#include "bionic_posix_bridge.h"
#include "android_socket_bridge.h"
#include "bionic_random_bridge.h"
#include "bionic_signal_bridge.h"
#include "bionic_errno.h"
#include "bionic_time_bridge.h"

int bs_bionic_posix_memalign(void **result, size_t alignment, size_t bytes);

/* Android's libdl is part of the runtime loader, not a normal static
 * library. Resolve actual mapped Quest ELF exports and the explicit C
 * compatibility table. Never report a missing plugin as successfully loaded.
 */
static char g_dl_error[200];
static int g_platform_loader_handle;
static int g_global_loader_handle;
static int g_vulkan_loader_handle;
static int g_aaudio_loader_handle;

static void bs_set_dl_error(const char *reason, const char *name) {
    snprintf(g_dl_error, sizeof(g_dl_error), "%s: %s",
             reason, name ? name : "(null)");
    debugPrintf("[dl] %s\n", g_dl_error);
}

static bool bs_is_platform_library(const char *name) {
    return name && (
        !strcmp(name, "libc.so") ||
        !strcmp(name, "libm.so") ||
        !strcmp(name, "libdl.so") ||
        !strcmp(name, "liblog.so") ||
        !strcmp(name, "libz.so") ||
        !strcmp(name, "libandroid.so") ||
        !strcmp(name, "libmediandk.so") ||
        !strcmp(name, "libEGL.so") ||
        !strcmp(name, "libGLESv2.so") ||
        !strcmp(name, "libGLESv3.so"));
}

static void *bs_android_dlopen(const char *name, int flags) {
    (void)flags;
    /* Some Android code calls dlopen(NULL) to search globally. */
    if (!name) return &g_global_loader_handle;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (strcmp(base, "libvulkan.so") == 0 &&
        bs_vulkan_symbol("vkGetInstanceProcAddr")) {
        static int reported;
        if (!reported++) debugPrintf("[graphics] guest dlopen(%s) -> NVK loader\n", name);
        return &g_vulkan_loader_handle;
    }
    /* Unity 6000.3.19f1 resolves AAudio at runtime with dlopen/dlsym.
     * Only return a handle when the real libnx PCM bridge exports exist.
     * OpenSL ES has a different object/vtable ABI and is not substituted.
     */
    if (!strcmp(base, "libaaudio.so") && bs_android_audio_exports_count)
        return &g_aaudio_loader_handle;
    so_module *module = so_find_module_by_name(name);
    if (module) return module;
    if (bs_is_platform_library(base)) return &g_platform_loader_handle;
    /* Unity's plugin manager opens XR plugins by bare name ("UnityOpenXR");
     * on Android the app's nativeLibraryDir resolves that to lib<name>.so. */
    if (!strchr(name, '/') && strncmp(base, "lib", 3) && !strstr(base, ".so")) {
        char decorated[256];
        int n = snprintf(decorated, sizeof decorated, "lib%s.so", base);
        if (n > 0 && (size_t)n < sizeof decorated &&
            (module = so_find_module_by_name(decorated)))
            return module;
    }
    /* Unity prefixes "lib" to plugin names that already carry it
     * ("libhaptics_sdk" -> liblibhaptics_sdk.so). */
    if (!strncmp(base, "liblib", 6) && (module = so_find_module_by_name(base + 3)))
        return module;
    bs_set_dl_error("shared library unavailable", name);
    return NULL;
}

/* Android's extended dlopen is equivalent to dlopen only for the default
 * namespace and no special extension flags. Mapping any other namespace or
 * fd-backed APK offset without actually honoring it would load wrong code.
 */
static void *bs_android_dlopen_ext(const char *name, int flags, const void *info) {
    if (info && *(const uint64_t *)info != 0) {
        bs_set_dl_error("unsupported android_dlextinfo flags", name);
        return NULL;
    }
    return bs_android_dlopen(name, flags);
}

static void *bs_android_dlsym(void *handle, const char *name) {
    if (!name || !*name) {
        bs_set_dl_error("invalid symbol", name);
        return NULL;
    }
    if (handle == &g_vulkan_loader_handle) {
        void *symbol = bs_vulkan_symbol(name);
        static int reported;
        if (reported < 24) {
            ++reported;
            debugPrintf("[graphics] guest dlsym(libvulkan, %s) -> %p\n", name, symbol);
        }
        if (!symbol) bs_set_dl_error("Vulkan symbol unavailable", name);
        return symbol;
    }
    if (handle == &g_aaudio_loader_handle) {
        void *symbol = bs_android_audio_find_export(name);
        if (!symbol) bs_set_dl_error("AAudio symbol unavailable", name);
        return symbol;
    }
    if (handle != NULL && handle != &g_platform_loader_handle &&
        handle != &g_global_loader_handle && handle != (void *)-1) {
        so_module *module = (so_module *)handle;
        /* Avoid interpreting an arbitrary handle as a module pointer. */
        if (so_is_loaded_module(module)) {
            /* No Horizon / Platform SDK runtime on Switch: the loader shim
             * would abort; answer ovr_* from the offline platform model. */
            if (strstr(module->name, "libovrplatformloader.so")) {
                void *ovr = bs_ovr_platform_symbol(name);
                if (ovr) return ovr;
            }
            uintptr_t address = so_find_module_export(module, name);
            if (address) return (void *)address;
            bs_set_dl_error("symbol not exported by module", name);
            return NULL;
        } else {
            bs_set_dl_error("unknown module handle", name);
            return NULL;
        }
    } else if (handle != &g_platform_loader_handle) {
        void *address = so_resolve_external(name);
        if (address) return address;
    }
    for (int i = 0; i < bs_platform_symbols_count; ++i) {
        if (!strcmp(bs_platform_symbols[i].symbol, name))
            return (void *)bs_platform_symbols[i].func;
    }
    /* Switch Mesa dynamically exposes hundreds of GL extension functions.
     * They must be looked up from the actual renderer, not stubbed as
     * successful symbol resolutions.
     */
    void *proc = bs_egl_symbol(name);
    if (proc) return proc;
    bs_set_dl_error("symbol unavailable", name);
    return NULL;
}

static int bs_android_dlclose(void *handle) {
    if (!handle) return -1;
    if (handle != &g_platform_loader_handle &&
        handle != &g_global_loader_handle &&
        handle != &g_vulkan_loader_handle &&
        handle != &g_aaudio_loader_handle &&
        !so_is_loaded_module((so_module *)handle)) {
        bs_set_dl_error("unknown module handle", NULL);
        return -1;
    }
    /* Modules have process lifetime; unloading is deferred until shutdown.
     * Do not remove their executable mappings while Unity might still use
     * exported function pointers.
     */
    return 0;
}

static char *bs_android_dlerror(void) {
    if (!g_dl_error[0]) return NULL;
    /* A single diagnostic thread uses the loader until thread-local
     * Android runtime state is implemented.
     */
    static char last_error[sizeof(g_dl_error)];
    strcpy(last_error, g_dl_error);
    g_dl_error[0] = 0;
    return last_error;
}

/* Android and newlib use the same four-pointer Dl_info representation.
 * Return only verified loaded-module identity; symbol details are optional
 * according to dladdr's contract.
 */
typedef struct BsAndroidDlInfo {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} BsAndroidDlInfo;
static int bs_android_dladdr(const void *address, BsAndroidDlInfo *info) {
    if (!info) return 0;
    so_module *module = so_find_module_by_addr(address);
    if (!module) return 0;
    info->dli_fname = module->name;
    info->dli_fbase = module->load_virtbase;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

/* Bionic C++ static initialization registers destructors per DSO handle.
 * Retain actual functions and execute them in reverse registration order
 * when __cxa_finalize is requested.
 */
#define BS_ATEXIT_CAP 8192
typedef struct BsAtexit {
    void (*destructor)(void *);
    void *object;
    void *dso;
} BsAtexit;
static BsAtexit g_atexit[BS_ATEXIT_CAP];
static size_t g_atexit_count;

static int bs_cxa_atexit(void (*destructor)(void *), void *object, void *dso) {
    if (!destructor || g_atexit_count >= BS_ATEXIT_CAP) return -1;
    g_atexit[g_atexit_count++] = (BsAtexit){destructor,object,dso};
    return 0;
}

static void bs_cxa_finalize(void *dso) {
    for (size_t i = g_atexit_count; i > 0; --i) {
        BsAtexit *entry = &g_atexit[i - 1];
        if (entry->destructor && (!dso || entry->dso == dso)) {
            void (*destructor)(void *) = entry->destructor;
            void *object = entry->object;
            entry->destructor = NULL;
            destructor(object);
        }
    }
}

/* Switch has no fork operation; register and retain the pthread_atfork
 * handlers even though no process fork can trigger them.
 */
#define BS_ATFORK_CAP 128
typedef struct BsAtfork {
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
} BsAtfork;
static BsAtfork g_atfork[BS_ATFORK_CAP];
static size_t g_atfork_count;
static int bs_register_atfork(void (*prepare)(void),
                             void (*parent)(void), void (*child)(void)) {
    if (g_atfork_count >= BS_ATFORK_CAP) return ENOMEM;
    g_atfork[g_atfork_count++] = (BsAtfork){prepare,parent,child};
    return 0;
}

static int bs_system_property_get(const char *name, char *value) {
    if (!name || !value) return 0;
    const struct {const char *name; const char *value;} known[] = {
        {"ro.build.version.sdk", "32"},
        {"ro.build.version.release", "12"},
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.product.model", "Nintendo Switch"},
        {"ro.product.manufacturer", "Nintendo"},
        {"ro.hardware", "nx"},
        {"ro.product.locale", "en-US"}
    };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); ++i) {
        if (!strcmp(name, known[i].name)) {
            size_t n = strlen(known[i].value);
            if (n > 91) return 0; /* Android PROP_VALUE_MAX: 92 */
            memcpy(value, known[i].value, n + 1);
            return (int)n;
        }
    }
    value[0] = 0; /* Unknown property: Android returns 0. */
    return 0;
}

static void bs_log_exit_caller(const char *what, int code, void *caller) {
    so_module *m = so_find_module_by_addr(caller);
    debugPrintf("[bionic:exit] %s(%d) from %s+0x%lx\n", what, code,
                m ? m->name : "host",
                (unsigned long)(m ? (uintptr_t)caller - (uintptr_t)m->load_virtbase
                                  : (uintptr_t)caller));
}
static void bs_guest_exit(int code) {
    bs_log_exit_caller("exit", code, __builtin_return_address(0));
    exit(code);
}
static void bs_guest__exit(int code) {
    bs_log_exit_caller("_exit", code, __builtin_return_address(0));
    _exit(code);
}
static void bs_guest_abort(void) {
    bs_log_exit_caller("abort", 0, __builtin_return_address(0));
    abort();
}
static void bs_assert2(const char *file, int line, const char *function,
                       const char *expression) {
    debugPrintf("[bionic:assert] %s:%d %s(): %s\n",
                file ? file : "?", line,
                function ? function : "?",
                expression ? expression : "?");
    abort();
}
static void bs_abort_message(const char *message) {
    debugPrintf("[bionic:abort] %s\n", message ? message : "(null)");
}
static size_t bs_ctype_mb_cur_max(void) { return MB_CUR_MAX; }
static char *bs_gnu_strerror_r(int code, char *buffer, size_t capacity) {
    if (!buffer || !capacity) return NULL;
    snprintf(buffer, capacity, "%s", strerror(bs_errno_from_bionic(code)));
    return buffer;
}
static char *bs_bionic_strerror(int code) {
    return strerror(bs_errno_from_bionic(code));
}
static void bs_fd_set_checked(int fd, void *set, size_t capacity) {
    if (fd < 0 || fd >= 1024 || !set || capacity < 128 ||
        (size_t)fd >= capacity * 8)
        bs_assert2("fd_set", 0, "__FD_SET_chk", "fd out of range");
    ((uint64_t *)set)[fd / 64] |= (uint64_t)1 << (fd & 63);
}
static void bs_fd_clr_checked(int fd, void *set, size_t capacity) {
    if (fd < 0 || fd >= 1024 || !set || capacity < 128 ||
        (size_t)fd >= capacity * 8)
        bs_assert2("fd_set", 0, "__FD_CLR_chk", "fd out of range");
    ((uint64_t *)set)[fd / 64] &= ~((uint64_t)1 << (fd & 63));
}
static int bs_fd_isset_checked(int fd, const void *set, size_t capacity) {
    if (fd < 0 || fd >= 1024 || !set || capacity < 128 ||
        (size_t)fd >= capacity * 8)
        bs_assert2("fd_set", 0, "__FD_ISSET_chk", "fd out of range");
    return (int)((((const uint64_t *)set)[fd / 64] >> (fd & 63)) & 1);
}
static int bs_sched_yield(void) {
    svcSleepThread(0);
    return 0;
}
static int bs_android_dup(int fd) {
    /* Original guest ALooper pipes use synthetic descriptors and cannot be
     * duplicated through newlib's native descriptor table. */
    if (fd < 0 || fd >= 0x4000) { errno = EBADF; return -1; }
    return dup(fd);
}
static void bs_sincosf(float x, float *out_s, float *out_c) {
    if (out_s) *out_s = sinf(x);
    if (out_c) *out_c = cosf(x);
}
static void bs_sincos(double x, double *out_s, double *out_c) {
    if (out_s) *out_s = sin(x);
    if (out_c) *out_c = cos(x);
}
static int bs_process_id(void) {
    u64 id = 0;
    return R_SUCCEEDED(svcGetProcessId(&id, CUR_PROCESS_HANDLE))
        ? (int)id : -1;
}
static int bs_android_getpagesize(void) { return 4096; }
static unsigned long bs_android_getauxval(unsigned long type) {
    /* Android's AT_PAGESZ, AT_CLKTCK and AT_SECURE have stable ELF auxv
     * indices across Linux architectures. Other CPU features must come from
     * genuine Horizon hardware capability detection; do not claim them.
     */
    switch (type) {
        case 6: return 4096;                 /* AT_PAGESZ */
        case 23: return 0;                   /* AT_SECURE */
        default: errno = ENOENT; return 0;
    }
}
typedef struct BsProp { const char *name, *value; } BsProp;
static const BsProp bs_properties[] = {
    {"ro.build.version.sdk","32"},
    {"ro.build.version.release","12"},
    {"ro.product.cpu.abi","arm64-v8a"},
    {"ro.product.model","Nintendo Switch"},
    {"ro.product.manufacturer","Nintendo"},
    {"ro.hardware","nx"},
    {"ro.product.locale","en-US"}
};
static const void *bs_system_property_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(bs_properties)/sizeof(bs_properties[0]); ++i)
        if (!strcmp(bs_properties[i].name, name)) return &bs_properties[i];
    return NULL;
}
static int bs_system_property_read(const void *property, char *name,
                                   char *value) {
    if (!property) return 0;
    const BsProp *prop = property;
    if (name) strcpy(name, prop->name);
    if (value) strcpy(value, prop->value);
    return (int)strlen(prop->value);
}

/* One log line per Android message: embedded newlines become " | ", and
 * the priority is kept for errors (prio >= ANDROID_LOG_WARN = 5). */
static void bs_android_log_emit_from(int prio, const char *tag, const char *msg, void *caller) {
    char line[768];
    size_t n = 0;
    for (const char *p = msg ? msg : ""; *p && n + 4 < sizeof line; ++p) {
        if (*p == '\n') {
            if (p[1]) { memcpy(line + n, " | ", 3); n += 3; }
        } else if (*p != '\r') {
            line[n++] = *p;
        }
    }
    line[n] = '\0';
    (void)caller;
    debugPrintf("[android:%s]%s %s\n", tag ? tag : "Unity",
                prio >= 6 ? " E" : prio == 5 ? " W" : "", line);
}
#define bs_android_log_emit(p, t, m) bs_android_log_emit_from(p, t, m, __builtin_return_address(0))

static int bs_guest_vsnprintf(char *out, size_t size, const char *fmt, va_list ap) {
    return vsnprintf(out, size, fmt, ap);
}
static off_t bs_guest_lseek(int fd, off_t off, int whence) {
    return lseek(fd, off, whence);
}
static int bs_guest_snprintf(char *out, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return n;
}
static int bs_android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    bs_android_log_emit(prio, tag, msg);
    return n;
}

static int bs_android_log_vprint(int prio, const char *tag,
                                const char *fmt, va_list args) {
    char msg[768];
    int n = vsnprintf(msg, sizeof(msg), fmt ? fmt : "", args);
    bs_android_log_emit(prio, tag, msg);
    return n;
}

static int bs_android_log_write(int prio, const char *tag, const char *msg) {
    (void)prio;
    bs_android_log_emit(prio, tag, msg);
    return 1;
}

static void bs_android_log_assert(const char *cond, const char *tag,
                                  const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);
    debugPrintf("[android:FATAL:%s] assertion: %s, %s\n",
                tag ? tag : "Unity", cond ? cond : "", msg);
    abort();
}

static int *bs_errno_pointer(void) {
    /* Bionic numbering; see bionic_errno.h. */
    return bs_guest_errno_pointer();
}

/* pthread-style functions return an error number directly. The guest
 * compares it with Linux values (ETIMEDOUT=110, ENOTSUP=95, EDEADLK=35), so
 * each newlib result is translated at the ABI boundary. Caller-sensitive
 * blocking entries (mutex lock, cond wait) translate internally instead so
 * their recorded guest return address stays accurate. */
#define BS_GUEST_RC1(fn, T1) \
    static int fn##_guest(T1 a) { return bs_errno_to_bionic(fn(a)); }
#define BS_GUEST_RC2(fn, T1, T2) \
    static int fn##_guest(T1 a, T2 b) { return bs_errno_to_bionic(fn(a, b)); }
#define BS_GUEST_RC3(fn, T1, T2, T3) \
    static int fn##_guest(T1 a, T2 b, T3 c) { \
        return bs_errno_to_bionic(fn(a, b, c)); }
#define BS_GUEST_RC4(fn, T1, T2, T3, T4) \
    static int fn##_guest(T1 a, T2 b, T3 c, T4 d) { \
        return bs_errno_to_bionic(fn(a, b, c, d)); }
BS_GUEST_RC2(bs_mutex_init, void **, const int *)
BS_GUEST_RC1(bs_mutex_destroy, void **)
BS_GUEST_RC2(bs_cond_init, void **, const int *)
BS_GUEST_RC1(bs_cond_destroy, void **)
BS_GUEST_RC1(bs_cond_signal, void **)
BS_GUEST_RC1(bs_cond_broadcast, void **)
BS_GUEST_RC2(bs_rwlock_init, void **, const void *)
BS_GUEST_RC1(bs_rwlock_rdlock, void **)
BS_GUEST_RC1(bs_rwlock_wrlock, void **)
BS_GUEST_RC1(bs_rwlock_unlock, void **)
BS_GUEST_RC1(bs_rwlock_destroy, void **)
BS_GUEST_RC2(bs_thread_getattr_np, pthread_t, void *)
typedef void *(*BsGuestThreadEntry)(void *);
typedef void (*BsGuestKeyDestructor)(void *);
BS_GUEST_RC4(bs_thread_create, pthread_t *, const void *,
             BsGuestThreadEntry, void *)
BS_GUEST_RC2(bs_pthread_setname_np, pthread_t, const char *)
BS_GUEST_RC2(bs_pthread_kill, pthread_t, int)
BS_GUEST_RC1(pthread_detach, pthread_t)
BS_GUEST_RC2(pthread_join, pthread_t, void **)
BS_GUEST_RC2(bs_key_create, unsigned *, BsGuestKeyDestructor)
BS_GUEST_RC1(bs_key_delete, unsigned)
BS_GUEST_RC2(bs_key_setspecific, unsigned, const void *)
BS_GUEST_RC3(bs_bionic_pthread_sigmask, int, const BsBionicSigset *,
             BsBionicSigset *)

static void bs_stack_check_fail(void) {
    debugPrintf("[android:FATAL] stack guard verification failed\n");
    abort();
}

/* The initial scope is functions with simple pointer/scalar C ABI. Never
 * bind bionic pthread, FILE internals or native Android graphics directly
 * to newlib: those interfaces have incompatible layouts and state.
 */
DynLibFunction bs_platform_symbols[] = {
    {"getrandom", (uintptr_t)&bs_bionic_getrandom},
    {"getentropy", (uintptr_t)&bs_bionic_getentropy},
    {"srand48", (uintptr_t)&bs_bionic_srand48},
    {"lrand48", (uintptr_t)&bs_bionic_lrand48},
    {"mmap", (uintptr_t)&bs_mmap},
    {"mmap64", (uintptr_t)&bs_mmap},
    {"munmap", (uintptr_t)&bs_munmap},
    {"mprotect", (uintptr_t)&bs_mprotect},
    {"madvise", (uintptr_t)&bs_madvise},
    {"ANativeWindow_fromSurface", (uintptr_t)&bs_window_from_surface},
    {"ASensorManager_getInstance", (uintptr_t)&bs_sensor_manager_get_instance},
    {"ASensorManager_getSensorList", (uintptr_t)&bs_sensor_manager_get_sensor_list},
    {"ASensorManager_getDefaultSensor", (uintptr_t)&bs_sensor_manager_get_default_sensor},
    {"ASensorManager_createEventQueue", (uintptr_t)&bs_sensor_manager_create_event_queue},
    {"ASensorManager_destroyEventQueue", (uintptr_t)&bs_sensor_manager_destroy_event_queue},
    {"ASensorEventQueue_enableSensor", (uintptr_t)&bs_sensor_event_queue_enable_sensor},
    {"ASensorEventQueue_disableSensor", (uintptr_t)&bs_sensor_event_queue_disable_sensor},
    {"ASensorEventQueue_setEventRate", (uintptr_t)&bs_sensor_event_queue_set_event_rate},
    {"ASensorEventQueue_hasEvents", (uintptr_t)&bs_sensor_event_queue_has_events},
    {"ASensorEventQueue_getEvents", (uintptr_t)&bs_sensor_event_queue_get_events},
    {"ASensor_getType", (uintptr_t)&bs_sensor_get_type},
    {"ASensor_getResolution", (uintptr_t)&bs_sensor_get_resolution},
    {"ASensor_getMinDelay", (uintptr_t)&bs_sensor_get_min_delay},
    {"ASensor_getName", (uintptr_t)&bs_sensor_get_name},
    {"ASensor_getVendor", (uintptr_t)&bs_sensor_get_vendor},
    {"ANativeWindow_acquire", (uintptr_t)&bs_window_acquire},
    {"ANativeWindow_release", (uintptr_t)&bs_window_release},
    {"ANativeWindow_getWidth", (uintptr_t)&bs_window_get_width},
    {"ANativeWindow_getHeight", (uintptr_t)&bs_window_get_height},
    {"ANativeWindow_getFormat", (uintptr_t)&bs_window_get_format},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&bs_window_set_geometry},
    {"ALooper_prepare", (uintptr_t)&bs_looper_prepare},
    {"ALooper_forThread", (uintptr_t)&bs_looper_for_thread},
    {"ALooper_acquire", (uintptr_t)&bs_looper_acquire},
    {"ALooper_release", (uintptr_t)&bs_looper_release},
    {"ALooper_pollOnce", (uintptr_t)&bs_looper_poll_once},
    {"ALooper_wake", (uintptr_t)&bs_looper_wake},
    {"ALooper_addFd", (uintptr_t)&bs_looper_add_fd},
    {"ALooper_removeFd", (uintptr_t)&bs_looper_remove_fd},
    {"AChoreographer_getInstance", (uintptr_t)&bs_choreographer_get_instance},
    {"AChoreographer_postFrameCallback", (uintptr_t)&bs_choreographer_post_frame},
    {"AChoreographer_postFrameCallback64", (uintptr_t)&bs_choreographer_post_frame64},
    /* Quest libunity's original MediaNDK OBJECT imports each require the
     * address of a const char *variable, not the underlying UTF-8 string.
     * AMediaFormat is a real typed map; codec/extractor calls return ENOSYS
     * or AMEDIA_ERROR_UNSUPPORTED until a decoder service is implemented. */
    {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&AMEDIAFORMAT_KEY_CHANNEL_COUNT},
    {"AMEDIAFORMAT_KEY_COLOR_FORMAT", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_FORMAT},
    {"AMEDIAFORMAT_KEY_COLOR_RANGE", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_RANGE},
    {"AMEDIAFORMAT_KEY_COLOR_STANDARD", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_STANDARD},
    {"AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&AMEDIAFORMAT_KEY_DURATION},
    {"AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&AMEDIAFORMAT_KEY_ENCODER_DELAY},
    {"AMEDIAFORMAT_KEY_FRAME_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_FRAME_RATE},
    {"AMEDIAFORMAT_KEY_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_HEIGHT},
    {"AMEDIAFORMAT_KEY_LANGUAGE", (uintptr_t)&AMEDIAFORMAT_KEY_LANGUAGE},
    {"AMEDIAFORMAT_KEY_MIME", (uintptr_t)&AMEDIAFORMAT_KEY_MIME},
    {"AMEDIAFORMAT_KEY_ROTATION", (uintptr_t)&AMEDIAFORMAT_KEY_ROTATION},
    {"AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_SAMPLE_RATE},
    {"AMEDIAFORMAT_KEY_SLICE_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_SLICE_HEIGHT},
    {"AMEDIAFORMAT_KEY_STRIDE", (uintptr_t)&AMEDIAFORMAT_KEY_STRIDE},
    {"AMEDIAFORMAT_KEY_WIDTH", (uintptr_t)&AMEDIAFORMAT_KEY_WIDTH},
    {"AMediaFormat_new", (uintptr_t)&AMediaFormat_new},
    {"AMediaFormat_delete", (uintptr_t)&AMediaFormat_delete},
    {"AMediaFormat_getFloat", (uintptr_t)&AMediaFormat_getFloat},
    {"AMediaFormat_getInt32", (uintptr_t)&AMediaFormat_getInt32},
    {"AMediaFormat_getInt64", (uintptr_t)&AMediaFormat_getInt64},
    {"AMediaFormat_getString", (uintptr_t)&AMediaFormat_getString},
    {"AMediaFormat_setInt32", (uintptr_t)&AMediaFormat_setInt32},
    {"AMediaExtractor_new", (uintptr_t)&AMediaExtractor_new},
    {"AMediaExtractor_delete", (uintptr_t)&AMediaExtractor_delete},
    {"AMediaExtractor_setDataSource", (uintptr_t)&AMediaExtractor_setDataSource},
    {"AMediaExtractor_setDataSourceFd", (uintptr_t)&AMediaExtractor_setDataSourceFd},
    {"AMediaExtractor_setDataSourceCustom", (uintptr_t)&AMediaExtractor_setDataSourceCustom},
    {"AMediaExtractor_getTrackCount", (uintptr_t)&AMediaExtractor_getTrackCount},
    {"AMediaExtractor_getTrackFormat", (uintptr_t)&AMediaExtractor_getTrackFormat},
    {"AMediaExtractor_selectTrack", (uintptr_t)&AMediaExtractor_selectTrack},
    {"AMediaExtractor_readSampleData", (uintptr_t)&AMediaExtractor_readSampleData},
    {"AMediaExtractor_getSampleTrackIndex", (uintptr_t)&AMediaExtractor_getSampleTrackIndex},
    {"AMediaExtractor_getSampleTime", (uintptr_t)&AMediaExtractor_getSampleTime},
    {"AMediaExtractor_advance", (uintptr_t)&AMediaExtractor_advance},
    {"AMediaExtractor_seekTo", (uintptr_t)&AMediaExtractor_seekTo},
    {"AMediaCodec_createDecoderByType", (uintptr_t)&AMediaCodec_createDecoderByType},
    {"AMediaCodec_delete", (uintptr_t)&AMediaCodec_delete},
    {"AMediaCodec_configure", (uintptr_t)&AMediaCodec_configure},
    {"AMediaCodec_start", (uintptr_t)&AMediaCodec_start},
    {"AMediaCodec_stop", (uintptr_t)&AMediaCodec_stop},
    {"AMediaCodec_flush", (uintptr_t)&AMediaCodec_flush},
    {"AMediaCodec_dequeueInputBuffer", (uintptr_t)&AMediaCodec_dequeueInputBuffer},
    {"AMediaCodec_getInputBuffer", (uintptr_t)&AMediaCodec_getInputBuffer},
    {"AMediaCodec_queueInputBuffer", (uintptr_t)&AMediaCodec_queueInputBuffer},
    {"AMediaCodec_dequeueOutputBuffer", (uintptr_t)&AMediaCodec_dequeueOutputBuffer},
    {"AMediaCodec_getOutputBuffer", (uintptr_t)&AMediaCodec_getOutputBuffer},
    {"AMediaCodec_getOutputFormat", (uintptr_t)&AMediaCodec_getOutputFormat},
    {"AMediaCodec_releaseOutputBuffer", (uintptr_t)&AMediaCodec_releaseOutputBuffer},
    {"AMediaCodec_setOutputSurface", (uintptr_t)&AMediaCodec_setOutputSurface},
    {"AMediaDataSource_new", (uintptr_t)&AMediaDataSource_new},
    {"AMediaDataSource_delete", (uintptr_t)&AMediaDataSource_delete},
    {"AMediaDataSource_setUserdata", (uintptr_t)&AMediaDataSource_setUserdata},
    {"AMediaDataSource_setReadAt", (uintptr_t)&AMediaDataSource_setReadAt},
    {"AMediaDataSource_setGetSize", (uintptr_t)&AMediaDataSource_setGetSize},
    {"AMediaDataSource_setClose", (uintptr_t)&AMediaDataSource_setClose},
    {"eglGetDisplay", (uintptr_t)&eglGetDisplay},
    {"eglInitialize", (uintptr_t)&eglInitialize},
    {"eglTerminate", (uintptr_t)&eglTerminate},
    {"eglBindAPI", (uintptr_t)&eglBindAPI},
    {"eglChooseConfig", (uintptr_t)&eglChooseConfig},
    {"eglCreateContext", (uintptr_t)&eglCreateContext},
    {"eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface},
    {"eglDestroySurface", (uintptr_t)&eglDestroySurface},
    {"eglDestroyContext", (uintptr_t)&eglDestroyContext},
    {"eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext},
    {"eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface},
    {"eglMakeCurrent", (uintptr_t)&eglMakeCurrent},
    {"eglSwapBuffers", (uintptr_t)&eglSwapBuffers},
    {"eglSwapInterval", (uintptr_t)&eglSwapInterval},
    {"eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib},
    {"eglGetConfigs", (uintptr_t)&eglGetConfigs},
    {"eglQueryContext", (uintptr_t)&eglQueryContext},
    {"eglQuerySurface", (uintptr_t)&eglQuerySurface},
    {"eglQueryString", (uintptr_t)&eglQueryString},
    {"eglSurfaceAttrib", (uintptr_t)&eglSurfaceAttrib},
    {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"eglGetError", (uintptr_t)&eglGetError},
    {"glGetError", (uintptr_t)&glGetError},
    {"__assert2", (uintptr_t)&bs_assert2},
    {"__ctype_get_mb_cur_max", (uintptr_t)&bs_ctype_mb_cur_max},
    {"__gnu_strerror_r", (uintptr_t)&bs_gnu_strerror_r},
    {"strerror_r", (uintptr_t)&bs_bionic_strerror_r},
    {"setlocale", (uintptr_t)&bs_bionic_setlocale},
    {"newlocale", (uintptr_t)&bs_bionic_newlocale},
    {"uselocale", (uintptr_t)&bs_bionic_uselocale},
    {"freelocale", (uintptr_t)&bs_bionic_freelocale},
    {"duplocale", (uintptr_t)&bs_bionic_duplocale},
    {"localeconv", (uintptr_t)&bs_bionic_localeconv},
    {"strcoll_l", (uintptr_t)&bs_bionic_strcoll_l},
    {"strxfrm_l", (uintptr_t)&bs_bionic_strxfrm_l},
    {"wcscoll_l", (uintptr_t)&bs_bionic_wcscoll_l},
    {"wcsxfrm_l", (uintptr_t)&bs_bionic_wcsxfrm_l},
    {"strftime_l", (uintptr_t)&bs_bionic_strftime_l},
    {"strtof_l", (uintptr_t)&bs_bionic_strtof_l},
    {"strtod_l", (uintptr_t)&bs_bionic_strtod_l},
    {"strtold_l", (uintptr_t)&bs_bionic_strtold_l},
    {"strtoll_l", (uintptr_t)&bs_bionic_strtoll_l},
    {"strtoull_l", (uintptr_t)&bs_bionic_strtoull_l},
    {"iswlower_l", (uintptr_t)&bs_bionic_iswlower_l},
    {"iswspace_l", (uintptr_t)&bs_bionic_iswspace_l},
    {"iswprint_l", (uintptr_t)&bs_bionic_iswprint_l},
    {"iswcntrl_l", (uintptr_t)&bs_bionic_iswcntrl_l},
    {"iswupper_l", (uintptr_t)&bs_bionic_iswupper_l},
    {"iswalpha_l", (uintptr_t)&bs_bionic_iswalpha_l},
    {"iswdigit_l", (uintptr_t)&bs_bionic_iswdigit_l},
    {"iswpunct_l", (uintptr_t)&bs_bionic_iswpunct_l},
    {"iswxdigit_l", (uintptr_t)&bs_bionic_iswxdigit_l},
    {"iswblank_l", (uintptr_t)&bs_bionic_iswblank_l},
    {"towupper_l", (uintptr_t)&bs_bionic_towupper_l},
    {"towlower_l", (uintptr_t)&bs_bionic_towlower_l},
    {"isxdigit_l", (uintptr_t)&bs_bionic_isxdigit_l},
    {"isdigit_l", (uintptr_t)&bs_bionic_isdigit_l},
    {"islower_l", (uintptr_t)&bs_bionic_islower_l},
    {"isupper_l", (uintptr_t)&bs_bionic_isupper_l},
    {"tolower_l", (uintptr_t)&bs_bionic_tolower_l},
    {"toupper_l", (uintptr_t)&bs_bionic_toupper_l},
    {"btowc", (uintptr_t)&bs_bionic_btowc},
    {"wctob", (uintptr_t)&bs_bionic_wctob},
    {"mbtowc", (uintptr_t)&bs_bionic_mbtowc},
    {"__FD_SET_chk", (uintptr_t)&bs_fd_set_checked},
    {"__FD_CLR_chk", (uintptr_t)&bs_fd_clr_checked},
    {"__FD_ISSET_chk", (uintptr_t)&bs_fd_isset_checked},
    {"__memcpy_chk", (uintptr_t)&bs_fortify_memcpy},
    {"__memmove_chk", (uintptr_t)&bs_fortify_memmove},
    {"__memset_chk", (uintptr_t)&bs_fortify_memset},
    {"__strlen_chk", (uintptr_t)&bs_fortify_strlen},
    {"__strchr_chk", (uintptr_t)&bs_fortify_strchr},
    {"__strncat_chk", (uintptr_t)&bs_fortify_strncat},
    {"__strncpy_chk2", (uintptr_t)&bs_fortify_strncpy_chk2},
    {"__vsnprintf_chk", (uintptr_t)&bs_fortify_vsnprintf},
    {"__write_chk", (uintptr_t)&bs_fortify_write},
    {"__pwrite_chk", (uintptr_t)&bs_fortify_pwrite},
    {"__fwrite_chk", (uintptr_t)&bs_fortify_fwrite},
    {"__open_2", (uintptr_t)&bs_fortify_open2},
    {"android_set_abort_message", (uintptr_t)&bs_abort_message},
    {"__system_property_find", (uintptr_t)&bs_system_property_find},
    {"__system_property_read", (uintptr_t)&bs_system_property_read},
    {"clock_gettime", (uintptr_t)&bs_bionic_clock_gettime},
    {"clock_getres", (uintptr_t)&bs_bionic_clock_getres},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"gmtime_r", (uintptr_t)&gmtime_r},
    {"time", (uintptr_t)&time},
    {"getpid", (uintptr_t)&bs_process_id},
    {"getpagesize", (uintptr_t)&bs_android_getpagesize},
    {"getauxval", (uintptr_t)&bs_android_getauxval},
    {"sched_yield", (uintptr_t)&bs_sched_yield},
    {"sincos", (uintptr_t)&bs_sincos},
    {"sincosf", (uintptr_t)&bs_sincosf},
    {"stdin", (uintptr_t)&bs_bionic_stdin},
    {"stdout", (uintptr_t)&bs_bionic_stdout},
    {"stderr", (uintptr_t)&bs_bionic_stderr},
    {"memchr", (uintptr_t)&memchr},
    {"strdup", (uintptr_t)&strdup},
    {"strnlen", (uintptr_t)&strnlen},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"strncat", (uintptr_t)&strncat},
    {"strspn", (uintptr_t)&strspn},
    {"strcspn", (uintptr_t)&strcspn},
    {"strtok_r", (uintptr_t)&strtok_r},
    {"strerror", (uintptr_t)&bs_bionic_strerror},
    {"sscanf", (uintptr_t)&sscanf},
    {"sprintf", (uintptr_t)&sprintf},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"printf", (uintptr_t)&printf},
    {"puts", (uintptr_t)&puts},
    {"vprintf", (uintptr_t)&vprintf},
    {"remove", (uintptr_t)&bs_android_remove},
    {"rename", (uintptr_t)&bs_android_rename},
    {"renameat", (uintptr_t)&bs_android_renameat},
    {"unlink", (uintptr_t)&bs_android_unlink},
    {"rmdir", (uintptr_t)&bs_android_rmdir},
    {"qsort", (uintptr_t)&qsort},
    {"bsearch", (uintptr_t)&bsearch},
    {"difftime", (uintptr_t)&difftime},
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"cbrtf", (uintptr_t)&cbrtf},
    {"asin", (uintptr_t)&asin},
    {"asinf", (uintptr_t)&asinf},
    {"atan", (uintptr_t)&atan},
    {"atanf", (uintptr_t)&atanf},
    {"atan2", (uintptr_t)&atan2},
    {"atan2f", (uintptr_t)&atan2f},
    {"exp", (uintptr_t)&exp},
    {"expf", (uintptr_t)&expf},
    {"fmod", (uintptr_t)&fmod},
    {"fmodf", (uintptr_t)&fmodf},
    {"modff", (uintptr_t)&modff},
    {"exp2", (uintptr_t)&exp2},
    {"exp2f", (uintptr_t)&exp2f},
    {"log2", (uintptr_t)&log2},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"tan", (uintptr_t)&tan},
    {"tanh", (uintptr_t)&tanh},
    {"tanhf", (uintptr_t)&tanhf},
    {"sinh", (uintptr_t)&sinh},
    {"cosh", (uintptr_t)&cosh},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},
    {"modf", (uintptr_t)&modf},
    {"hypot", (uintptr_t)&hypot},
    {"hypotf", (uintptr_t)&hypotf},
    {"remainderf", (uintptr_t)&remainderf},
    {"nearbyintf", (uintptr_t)&nearbyintf},
    {"nextafter", (uintptr_t)&nextafter},
    {"open", (uintptr_t)&bs_android_open},
    {"open64", (uintptr_t)&bs_android_open},
    {"openat", (uintptr_t)&bs_android_openat},
    {"read", (uintptr_t)&bs_pipe_read},
    {"readv", (uintptr_t)&bs_bionic_readv},
    {"write", (uintptr_t)&bs_pipe_write},
    {"writev", (uintptr_t)&bs_bionic_writev},
    {"close", (uintptr_t)&bs_pipe_close},
    /* Numeric address conversion is local and useful in offline mode. */
    {"inet_pton", (uintptr_t)&bs_android_inet_pton},
    {"inet_ntop", (uintptr_t)&bs_android_inet_ntop},
    {"pipe", (uintptr_t)&bs_pipe},
    {"fcntl", (uintptr_t)&bs_pipe_fcntl},
    {"lseek", (uintptr_t)&bs_guest_lseek},
    {"lseek64", (uintptr_t)&bs_guest_lseek},
    {"dup", (uintptr_t)&bs_android_dup},
    {"ftruncate", (uintptr_t)&bs_bionic_ftruncate},
    {"poll", (uintptr_t)&bs_bionic_poll},
    {"select", (uintptr_t)&bs_bionic_select},
    {"ioctl", (uintptr_t)&bs_pipe_ioctl},
    {"truncate", (uintptr_t)&bs_bionic_truncate},
    {"fsync", (uintptr_t)&bs_bionic_fsync},
    {"fdatasync", (uintptr_t)&bs_bionic_fdatasync},
    {"flock", (uintptr_t)&bs_bionic_flock},
    {"statvfs", (uintptr_t)&bs_bionic_statvfs},
    {"pathconf", (uintptr_t)&bs_bionic_pathconf},
    {"utime", (uintptr_t)&bs_bionic_utime},
    {"utimes", (uintptr_t)&bs_bionic_utimes},
    {"futimens", (uintptr_t)&bs_bionic_futimens},
    {"utimensat", (uintptr_t)&bs_bionic_utimensat},
    {"getuid", (uintptr_t)&bs_bionic_getuid},
    {"geteuid", (uintptr_t)&bs_bionic_geteuid},
    {"getgid", (uintptr_t)&bs_bionic_getgid},
    {"getegid", (uintptr_t)&bs_bionic_getegid},
    {"gethostname", (uintptr_t)&bs_bionic_gethostname},
    {"uname", (uintptr_t)&bs_bionic_uname},
    {"access", (uintptr_t)&bs_android_access},
    {"chmod", (uintptr_t)&bs_android_chmod},
    {"fchmod", (uintptr_t)&bs_android_fchmod},
    {"fchmodat", (uintptr_t)&bs_android_fchmodat},
    {"unlinkat", (uintptr_t)&bs_android_unlinkat},
    {"link", (uintptr_t)&bs_android_link},
    {"symlink", (uintptr_t)&bs_android_symlink},
    {"readlink", (uintptr_t)&bs_android_readlink},
    {"sendfile", (uintptr_t)&bs_android_sendfile},
    {"dup2", (uintptr_t)&bs_android_dup2},
    {"socket", (uintptr_t)&bs_offline_socket},
    {"socketpair", (uintptr_t)&bs_offline_socketpair},
    {"connect", (uintptr_t)&bs_offline_connect},
    {"bind", (uintptr_t)&bs_offline_bind},
    {"listen", (uintptr_t)&bs_offline_listen},
    {"accept", (uintptr_t)&bs_offline_accept},
    {"shutdown", (uintptr_t)&bs_offline_shutdown},
    {"getsockname", (uintptr_t)&bs_offline_getsockname},
    {"getpeername", (uintptr_t)&bs_offline_getpeername},
    {"getsockopt", (uintptr_t)&bs_offline_getsockopt},
    {"setsockopt", (uintptr_t)&bs_offline_setsockopt},
    {"send", (uintptr_t)&bs_offline_send},
    {"sendto", (uintptr_t)&bs_offline_sendto},
    {"recvfrom", (uintptr_t)&bs_offline_recvfrom},
    {"recv", (uintptr_t)&bs_offline_recv},
    {"sendmsg", (uintptr_t)&bs_offline_sendmsg},
    {"recvmsg", (uintptr_t)&bs_offline_recvmsg},
    {"getaddrinfo", (uintptr_t)&bs_offline_getaddrinfo},
    {"getnameinfo", (uintptr_t)&bs_offline_getnameinfo},
    {"freeaddrinfo", (uintptr_t)&bs_offline_freeaddrinfo},
    {"mkdir", (uintptr_t)&bs_android_mkdir},
    {"stat", (uintptr_t)&bs_android_stat},
    {"stat64", (uintptr_t)&bs_android_stat},
    {"fstat", (uintptr_t)&bs_android_fstat},
    {"fstat64", (uintptr_t)&bs_android_fstat},
    {"lstat", (uintptr_t)&bs_android_lstat},
    {"lstat64", (uintptr_t)&bs_android_lstat},
    {"opendir", (uintptr_t)&bs_android_opendir},
    {"readdir", (uintptr_t)&bs_android_readdir},
    {"closedir", (uintptr_t)&closedir},
    {"realpath", (uintptr_t)&bs_android_realpath},
    {"getcwd", (uintptr_t)&bs_android_getcwd},
    {"getenv", (uintptr_t)&bs_android_getenv},
    {"setenv", (uintptr_t)&setenv},
    {"unsetenv", (uintptr_t)&unsetenv},
    {"chdir", (uintptr_t)&bs_android_chdir},
    {"fopen", (uintptr_t)&bs_android_fopen},
    {"fread", (uintptr_t)&bs_android_fread},
    {"fwrite", (uintptr_t)&bs_android_fwrite},
    {"fscanf", (uintptr_t)&bs_android_fscanf},
    {"vfscanf", (uintptr_t)&bs_android_vfscanf},
    {"fgets", (uintptr_t)&bs_android_fgets},
    {"fdopen", (uintptr_t)&bs_android_fdopen},
    {"setbuf", (uintptr_t)&bs_android_setbuf},
    {"clearerr", (uintptr_t)&bs_android_clearerr},
    {"perror", (uintptr_t)&bs_android_perror},
    {"fclose", (uintptr_t)&bs_android_fclose},
    {"fflush", (uintptr_t)&bs_android_fflush},
    {"fprintf", (uintptr_t)&bs_android_fprintf},
    {"vfprintf", (uintptr_t)&bs_android_vfprintf},
    {"fputs", (uintptr_t)&bs_android_fputs},
    {"fputc", (uintptr_t)&bs_android_fputc},
    {"ferror", (uintptr_t)&bs_android_ferror},
    {"feof", (uintptr_t)&bs_android_feof},
    {"fileno", (uintptr_t)&bs_android_fileno},
    {"fseek", (uintptr_t)&bs_android_fseek},
    {"ftell", (uintptr_t)&bs_android_ftell},
    {"fseeko64", (uintptr_t)&bs_android_fseeko},
    {"ftello64", (uintptr_t)&bs_android_ftello},
    {"fgetc", (uintptr_t)&bs_android_getc},
    {"fseeko", (uintptr_t)&bs_android_fseeko},
    {"ftello", (uintptr_t)&bs_android_ftello},
    {"getc", (uintptr_t)&bs_android_getc},
    {"ungetc", (uintptr_t)&bs_android_ungetc},
    {"getwc", (uintptr_t)&bs_android_getwc},
    {"ungetwc", (uintptr_t)&bs_android_ungetwc},
    {"fputwc", (uintptr_t)&bs_android_fputwc},
    {"__sF", (uintptr_t)&bs_bionic_streams},
    {"AAssetManager_fromJava", (uintptr_t)&bs_android_assetmanager_from_java},
    {"AAssetManager_open", (uintptr_t)&bs_android_assetmanager_open},
    {"AAssetManager_openDir", (uintptr_t)&bs_android_assetmanager_open_dir},
    {"AAssetDir_getNextFileName", (uintptr_t)&bs_android_asset_dir_get_next_file_name},
    {"AAssetDir_rewind", (uintptr_t)&bs_android_asset_dir_rewind},
    {"AAssetDir_close", (uintptr_t)&bs_android_asset_dir_close},
    {"AAsset_close", (uintptr_t)&bs_android_asset_close},
    {"AAsset_read", (uintptr_t)&bs_android_asset_read},
    {"AAsset_seek64", (uintptr_t)&bs_android_asset_seek64},
    {"AAsset_getLength", (uintptr_t)&bs_android_asset_length},
    {"AAsset_getLength64", (uintptr_t)&bs_android_asset_length64},
    {"AAsset_getBuffer", (uintptr_t)&bs_android_asset_get_buffer},
    {"AConfiguration_new", (uintptr_t)&bs_android_configuration_new},
    {"AConfiguration_delete", (uintptr_t)&bs_android_configuration_delete},
    {"AConfiguration_fromAssetManager", (uintptr_t)&bs_android_configuration_from_assetmanager},
    {"AConfiguration_getLanguage", (uintptr_t)&bs_android_configuration_get_language},
    {"AConfiguration_getCountry", (uintptr_t)&bs_android_configuration_get_country},
    {"AConfiguration_getSdkVersion", (uintptr_t)&bs_android_configuration_get_sdk_version},
    {"pthread_mutex_init", (uintptr_t)&bs_mutex_init_guest},
    {"pthread_mutex_destroy", (uintptr_t)&bs_mutex_destroy_guest},
    {"pthread_mutex_lock", (uintptr_t)&bs_mutex_lock},
    {"pthread_mutex_trylock", (uintptr_t)&bs_mutex_trylock},
    {"pthread_mutex_unlock", (uintptr_t)&bs_mutex_unlock},
    {"pthread_mutexattr_init", (uintptr_t)&bs_mutexattr_init},
    {"pthread_mutexattr_destroy", (uintptr_t)&bs_mutexattr_destroy},
    {"pthread_mutexattr_settype", (uintptr_t)&bs_mutexattr_settype},
    {"pthread_cond_init", (uintptr_t)&bs_cond_init_guest},
    {"pthread_cond_destroy", (uintptr_t)&bs_cond_destroy_guest},
    {"pthread_cond_signal", (uintptr_t)&bs_cond_signal_guest},
    {"pthread_cond_broadcast", (uintptr_t)&bs_cond_broadcast_guest},
    {"pthread_cond_wait", (uintptr_t)&bs_cond_wait},
    {"pthread_cond_timedwait", (uintptr_t)&bs_cond_timedwait},
    {"pthread_condattr_init", (uintptr_t)&bs_condattr_init},
    {"pthread_condattr_destroy", (uintptr_t)&bs_condattr_destroy},
    {"pthread_condattr_setclock", (uintptr_t)&bs_condattr_setclock},
    {"pthread_rwlock_init", (uintptr_t)&bs_rwlock_init_guest},
    {"pthread_rwlock_rdlock", (uintptr_t)&bs_rwlock_rdlock_guest},
    {"pthread_rwlock_wrlock", (uintptr_t)&bs_rwlock_wrlock_guest},
    {"pthread_rwlock_unlock", (uintptr_t)&bs_rwlock_unlock_guest},
    {"pthread_rwlock_destroy", (uintptr_t)&bs_rwlock_destroy_guest},
    {"pthread_attr_init", (uintptr_t)&bs_thread_attr_init},
    {"pthread_attr_destroy", (uintptr_t)&bs_thread_attr_destroy},
    {"pthread_attr_setdetachstate", (uintptr_t)&bs_thread_attr_setdetachstate},
    {"pthread_attr_setstacksize", (uintptr_t)&bs_thread_attr_setstacksize},
    {"pthread_attr_getstack", (uintptr_t)&bs_thread_attr_getstack},
    {"pthread_getattr_np", (uintptr_t)&bs_thread_getattr_np_guest},
    {"pthread_create", (uintptr_t)&bs_thread_create_guest},
    {"pthread_once", (uintptr_t)&bs_pthread_once},
    {"pthread_setname_np", (uintptr_t)&bs_pthread_setname_np_guest},
    {"pthread_kill", (uintptr_t)&bs_pthread_kill_guest},
    {"pthread_sigmask", (uintptr_t)&bs_bionic_pthread_sigmask_guest},
    {"sigemptyset", (uintptr_t)&bs_bionic_sigemptyset},
    {"sigfillset", (uintptr_t)&bs_bionic_sigfillset},
    {"sigaddset", (uintptr_t)&bs_bionic_sigaddset},
    {"sigdelset", (uintptr_t)&bs_bionic_sigdelset},
    {"sigprocmask", (uintptr_t)&bs_bionic_sigprocmask},
    {"sigaction", (uintptr_t)&bs_bionic_sigaction},
    {"signal", (uintptr_t)&bs_bionic_signal},
    {"sigaltstack", (uintptr_t)&bs_bionic_sigaltstack},
    {"sigsuspend", (uintptr_t)&bs_bionic_sigsuspend},
    {"setjmp", (uintptr_t)&bs_bionic_setjmp},
    {"sigsetjmp", (uintptr_t)&bs_bionic_sigsetjmp},
    {"longjmp", (uintptr_t)&bs_bionic_longjmp},
    {"siglongjmp", (uintptr_t)&bs_bionic_longjmp},
    {"pthread_detach", (uintptr_t)&pthread_detach_guest},
    {"pthread_join", (uintptr_t)&pthread_join_guest},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_key_create", (uintptr_t)&bs_key_create_guest},
    {"pthread_key_delete", (uintptr_t)&bs_key_delete_guest},
    {"pthread_getspecific", (uintptr_t)&bs_key_getspecific},
    {"pthread_setspecific", (uintptr_t)&bs_key_setspecific_guest},
    {"__tls_get_addr", (uintptr_t)&bs_guest_tls_get_addr},
    {"sem_init", (uintptr_t)&bs_sem_init},
    {"sem_destroy", (uintptr_t)&bs_sem_destroy},
    {"sem_post", (uintptr_t)&bs_sem_post},
    {"sem_wait", (uintptr_t)&bs_sem_wait},
    {"sem_timedwait", (uintptr_t)&bs_sem_timedwait},
    {"sem_getvalue", (uintptr_t)&bs_sem_getvalue},
    {"syscall", (uintptr_t)&bs_linux_syscall},
    {"gettid", (uintptr_t)&bs_gettid},
    {"sysconf", (uintptr_t)&bs_sysconf},
    {"dlopen", (uintptr_t)&bs_android_dlopen},
    {"android_dlopen_ext", (uintptr_t)&bs_android_dlopen_ext},
    {"dlsym", (uintptr_t)&bs_android_dlsym},
    {"dladdr", (uintptr_t)&bs_android_dladdr},
    {"dlclose", (uintptr_t)&bs_android_dlclose},
    {"dlerror", (uintptr_t)&bs_android_dlerror},
    {"__cxa_atexit", (uintptr_t)&bs_cxa_atexit},
    {"__cxa_thread_atexit_impl", (uintptr_t)&bs_bionic_cxa_thread_atexit_impl},
    {"__cxa_finalize", (uintptr_t)&bs_cxa_finalize},
    {"__register_atfork", (uintptr_t)&bs_register_atfork},
    {"__system_property_get", (uintptr_t)&bs_system_property_get},
    {"__android_log_print", (uintptr_t)&bs_android_log_print},
    {"__android_log_vprint", (uintptr_t)&bs_android_log_vprint},
    {"__android_log_write", (uintptr_t)&bs_android_log_write},
    {"__android_log_assert", (uintptr_t)&bs_android_log_assert},
    {"__errno", (uintptr_t)&bs_errno_pointer},
    {"__stack_chk_fail", (uintptr_t)&bs_stack_check_fail},
    {"vasprintf", (uintptr_t)&bs_bionic_vasprintf},
    {"openlog", (uintptr_t)&bs_bionic_openlog},
    {"syslog", (uintptr_t)&bs_bionic_syslog},
    {"closelog", (uintptr_t)&bs_bionic_closelog},
    {"raise", (uintptr_t)&bs_bionic_raise},
    {"exit", (uintptr_t)&bs_guest_exit},
    {"_exit", (uintptr_t)&bs_guest__exit},
    {"environ", (uintptr_t)&environ},
    {"abort", (uintptr_t)&bs_guest_abort},
    {"atoi", (uintptr_t)&atoi},
    {"atol", (uintptr_t)&atol},
    {"atoll", (uintptr_t)&atoll},
    {"calloc", (uintptr_t)&calloc},
    {"free", (uintptr_t)&free},
    {"malloc", (uintptr_t)&malloc},
    {"realloc", (uintptr_t)&realloc},
    {"memalign", (uintptr_t)&memalign},
    {"posix_memalign", (uintptr_t)&bs_bionic_posix_memalign},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"memcmp", (uintptr_t)&memcmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"strncpy", (uintptr_t)&strncpy},
    {"strcmp", (uintptr_t)&strcmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strlen", (uintptr_t)&strlen},
    {"strchr", (uintptr_t)&strchr},
    {"strrchr", (uintptr_t)&strrchr},
    {"strstr", (uintptr_t)&strstr},
    {"strcasestr", (uintptr_t)&bs_bionic_strcasestr},
    {"basename", (uintptr_t)&bs_bionic_basename},
    {"dirname", (uintptr_t)&bs_bionic_dirname},
    {"isspace", (uintptr_t)&isspace},
    {"isatty", (uintptr_t)&isatty},
    {"strtol", (uintptr_t)&strtol},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtof", (uintptr_t)&strtof},
    {"strtod", (uintptr_t)&strtod},
    {"strtold", (uintptr_t)&strtold},
    {"strlcpy", (uintptr_t)&strlcpy},
    {"memrchr", (uintptr_t)&bs_bionic_memrchr},
    /* Bionic's 8-byte mbstate_t holds UTF-8 pending bytes, while newlib's
     * equally sized object uses an incompatible representation. */
    {"mbsrtowcs", (uintptr_t)&bs_bionic_mbsrtowcs},
    {"mbsnrtowcs", (uintptr_t)&bs_bionic_mbsnrtowcs},
    {"wcsrtombs", (uintptr_t)&bs_bionic_wcsrtombs},
    {"wcsnrtombs", (uintptr_t)&bs_bionic_wcsnrtombs},
    {"wcrtomb", (uintptr_t)&bs_bionic_wcrtomb},
    {"mbrtowc", (uintptr_t)&bs_bionic_mbrtowc},
    {"mbrtoc32", (uintptr_t)&bs_bionic_mbrtoc32},
    {"c32rtomb", (uintptr_t)&bs_bionic_c32rtomb},
    {"mbtowc", (uintptr_t)&bs_bionic_mbtowc_compat},
    {"mbrlen", (uintptr_t)&bs_bionic_mbrlen},
    {"mbsinit", (uintptr_t)&bs_bionic_mbsinit},
    {"mbstowcs", (uintptr_t)&bs_bionic_mbstowcs},
    {"mblen", (uintptr_t)&bs_bionic_mblen},
    {"wctomb", (uintptr_t)&bs_bionic_wctomb},
    {"btowc", (uintptr_t)&btowc},
    {"wctob", (uintptr_t)&wctob},
    {"wcstombs", (uintptr_t)&bs_bionic_wcstombs},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"wcslen", (uintptr_t)&wcslen},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstold", (uintptr_t)&wcstold},
    {"swprintf", (uintptr_t)&swprintf},
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"islower", (uintptr_t)&islower},
    {"isupper", (uintptr_t)&isupper},
    {"isdigit", (uintptr_t)&isdigit},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"div", (uintptr_t)&div},
    {"mktime", (uintptr_t)&mktime},
    {"gmtime", (uintptr_t)&gmtime},
    {"clock", (uintptr_t)&clock},
    {"srand", (uintptr_t)&srand},
    {"rand", (uintptr_t)&rand},
    {"snprintf", (uintptr_t)&bs_guest_snprintf},
    {"vsnprintf", (uintptr_t)&bs_guest_vsnprintf},
    {"fabs", (uintptr_t)&fabs},
    {"fabsf", (uintptr_t)&fabsf},
    {"sin", (uintptr_t)&sin},
    {"cos", (uintptr_t)&cos},
    {"sinf", (uintptr_t)&sinf},
    {"cosf", (uintptr_t)&cosf},
    {"tanf", (uintptr_t)&tanf},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"sqrt", (uintptr_t)&sqrt},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"log", (uintptr_t)&log},
    {"logf", (uintptr_t)&logf},
    {"log2f", (uintptr_t)&log2f},
    {"logb", (uintptr_t)&logb},
    {"scalbn", (uintptr_t)&scalbn},
    {"floor", (uintptr_t)&floor},
    {"floorf", (uintptr_t)&floorf},
    {"ceil", (uintptr_t)&ceil},
    {"ceilf", (uintptr_t)&ceilf},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"usleep", (uintptr_t)&usleep},
    {"localtime", (uintptr_t)&localtime},
    {"localtime_r", (uintptr_t)&localtime_r},
    {"strftime", (uintptr_t)&strftime},
    {"aligned_alloc", (uintptr_t)&aligned_alloc},
    {"inflate", (uintptr_t)&inflate},
    {"inflateEnd", (uintptr_t)&inflateEnd},
    {"inflateInit2_", (uintptr_t)&inflateInit2_},
    {"dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr}
};
/* Route every guest EGL/GLES import through the Android-translating EGL
 * bridge (the table rows above only name the imports). */
void bs_platform_bind_graphics(void) {
    for (size_t i = 0; i < sizeof bs_platform_symbols / sizeof bs_platform_symbols[0]; ++i) {
        const char *n = bs_platform_symbols[i].symbol;
        if (n && (!strncmp(n, "egl", 3) || !strncmp(n, "gl", 2))) {
            void *fn = bs_egl_symbol(n);
            if (fn) bs_platform_symbols[i].func = (uintptr_t)fn;
        }
    }
}

const int bs_platform_symbols_count =
    (int)(sizeof(bs_platform_symbols)/sizeof(bs_platform_symbols[0]));
