/* Original-library port integration tests for Nintendo Switch hardware.
 * Correct native ABI behavior is a prerequisite for actual Unity6 startup.
 * This tests real libnx threading/filesystem/NDK behavior independently of
 * unresolved Quest OpenXR and GameActivity services.
 */
#include <switch.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#include "asset_fs_bridge.h"
#include "bionic_threads.h"
#include "bionic_mmap.h"
#include "android_window_bridge.h"
#include "android_choreographer.h"
#include "android_sensor_bridge.h"
#include "gameactivity_native_glue.h"
#include "bionic_locale_bridge.h"
#include "offline_network_bridge.h"
#include "jni_bridge.h"
#include "so_util.h"
#include "guest_tls.h"
#include "util.h"
#include "runtime_selftest.h"

static int success_count;
static int failure_count;
static void record(const char *name, bool success) {
    printf("  [%s] %s\n", success ? "PASS" : "FAIL", name);
    debugPrintf("[selftest] %s %s\n", success ? "PASS" : "FAIL", name);
    if (success) success_count++;
    else failure_count++;
    consoleUpdate(NULL);
}

static bool test_assets(void) {
    int fd=bs_android_open("assets/bin/Data/globalgamemanagers", 0);
    if (fd < 0) return false;
    char buf[4096];
    ssize_t size = read(fd, buf, sizeof(buf));
    close(fd);
    if (size < 100) return false;
    const char version[] = "6000.3.19f1";
    const size_t version_len = sizeof(version) - 1;
    for (size_t offset = 0; offset <= (size_t)size - version_len; ++offset) {
        if (memcmp(buf + offset, version, version_len) == 0) return true;
    }
    return false;
}

static void log_jni(void *user, const char *message) {
    (void)user;
    debugPrintf("%s\n", message);
}

static bool test_assetmanager(void) {
    if (bs_jni_init(log_jni, NULL) != BS_JNI_OK) return false;
    void *env = bs_jni_env();
    void *activity = bs_jni_activity();
    if (!env || !activity) return false;

    /* Exercise the same JNI function table that native GameActivity sees. */
    enum { GET_OBJECT_CLASS = 31, GET_METHOD_ID = 33, CALL_OBJECT_METHOD_A = 36 };
    typedef void (*JniFunction)(void);
    const JniFunction *jni = *(const JniFunction **)env;
    void *(*object_class)(void *, void *) = (void *(*)(void *, void *))jni[GET_OBJECT_CLASS];
    void *(*get_method)(void *, void *, const char *, const char *) =
        (void *(*)(void *, void *, const char *, const char *))jni[GET_METHOD_ID];
    void *(*call_object)(void *, void *, void *, const void *) =
        (void *(*)(void *, void *, void *, const void *))jni[CALL_OBJECT_METHOD_A];
    void *method = get_method(env, object_class(env, activity), "getAssets",
                              "()Landroid/content/res/AssetManager;");
    if (!method || bs_android_assetmanager_from_java(env, activity)) return false;
    void *assets = call_object(env, activity, method, NULL);
    void *manager = bs_android_assetmanager_from_java(env, assets);
    if (!manager) return false;
    void *asset=bs_android_assetmanager_open(manager,
                                           "bin/Data/globalgamemanagers", 0);
    if (!asset) return false;
    int64_t length=bs_android_asset_length64(asset);
    char bytes[16]={0};
    int received=bs_android_asset_read(asset, bytes, sizeof bytes);
    bs_android_asset_close(asset);
    return length==383812 && received==sizeof bytes;
}

static bool test_mutex_sem(void) {
    void *mutex=NULL;
    if (bs_mutex_init(&mutex,NULL) != 0) return false;
    int locked=bs_mutex_lock(&mutex);
    int unlocked=bs_mutex_unlock(&mutex);
    int destroyed=bs_mutex_destroy(&mutex);
    void *semaphore=NULL;
    int created=bs_sem_init(&semaphore,0,0);
    int posted=created==0 ? bs_sem_post(&semaphore) : -1;
    int waited=created==0 ? bs_sem_wait(&semaphore) : -1;
    int deleted=created==0 ? bs_sem_destroy(&semaphore) : -1;
    return locked==0 && unlocked==0 && destroyed==0 &&
           created==0 && posted==0 && waited==0 && deleted==0;
}

static volatile int g_thread_completed;
static void *thread_body(void *arg) {
    volatile int *status=(volatile int *)arg;
    *status=1;
    return NULL;
}
static bool test_thread(void) {
    g_thread_completed=0;
    pthread_t thread;
    if (bs_thread_create(&thread,NULL,thread_body,
                         (void *)&g_thread_completed) != 0) return false;
    int joined=pthread_join(thread,NULL);
    return joined==0 && g_thread_completed==1;
}

static void *tls_thread_body(void *arg) {
    uint8_t *tls = bs_guest_tls_get_addr(&(BsGuestTlsIndex){1, 0});
    bool *success = arg;
    *success = tls != NULL && *tls == 0;
    if (tls) *tls = 0x56;
    return NULL;
}

static bool test_guest_tls_isolation(void) {
    if (!bs_guest_tls_is_ready() || bs_guest_tls_module_count() != 3 ||
        bs_guest_tls_relocation_count() != 15) return false;
    uint8_t *main_tls = bs_guest_tls_get_addr(&(BsGuestTlsIndex){1, 0});
    if (!main_tls) return false;
    const uint8_t original = *main_tls;
    *main_tls = 0x44;
    bool isolated = false;
    pthread_t thread;
    int rc = bs_thread_create(&thread, NULL, tls_thread_body, &isolated);
    if (!rc) rc = pthread_join(thread, NULL);
    bool result = !rc && isolated && *main_tls == 0x44;
    *main_tls = original;
    return result;
}

/* libOVRPlugin.so's original PT_TLS .tdata initializer is itself an Android
 * RELR target. Compare the live main-thread copy with the RELOCATED mapped
 * image, then verify the initialized pointer belongs to that real module.
 * This detects snapshots taken before so_relocate(). */
static bool test_guest_tls_relocated_initializers(void) {
    so_module *module = so_find_module_by_name("libOVRPlugin.so");
    uintptr_t module_id = bs_guest_tls_module_id(module);
    if (!module || !module_id || !module->load_virtbase) return false;
    for (int p = 0; p < module->phnum; ++p) {
        const Elf64_Phdr *tls = &module->phdr[p];
        if (tls->p_type != PT_TLS) continue;
        if (tls->p_filesz != sizeof(uintptr_t) ||
            tls->p_vaddr > module->load_size ||
            tls->p_filesz > module->load_size - tls->p_vaddr)
            return false;
        uint8_t *installed = bs_guest_tls_get_addr(
            &(BsGuestTlsIndex){module_id, 0});
        const uint8_t *relocated =
            (const uint8_t *)module->load_virtbase + tls->p_vaddr;
        if (!installed || memcmp(installed, relocated, tls->p_filesz))
            return false;
        uintptr_t pointer = 0;
        memcpy(&pointer, installed, sizeof(pointer));
        uintptr_t base = (uintptr_t)module->load_virtbase;
        return pointer >= base && pointer - base < module->load_size;
    }
    return false;
}

static bool test_bionic_tls_keys(void) {
    /* Unity creates more Bionic TLS slots than newlib can provide directly.
     * Forty distinct keys exercises the single-native-key multiplexer.
     */
    unsigned keys[40]={0};
    int created=0;
    bool good=true;
    for (int i=0; i<40; ++i) {
        if (bs_key_create(&keys[i],NULL) != 0) {
            good=false;
            break;
        }
        created++;
        if (bs_key_setspecific(keys[i], (void *)(uintptr_t)(i+1)) != 0 ||
            (uintptr_t)bs_key_getspecific(keys[i]) != (uintptr_t)(i+1))
            good=false;
    }
    for (int i=0; i<created; ++i)
        if (bs_key_delete(keys[i]) != 0) good=false;
    return good;
}

static bool test_bionic_memory_mapping(void) {
    const size_t bytes = 8192;
    void *address = bs_mmap(NULL, bytes, 3, 0x20 | 0x2, -1, 0);
    if (!address || address == (void *)-1) return false;
    bool good = ((uintptr_t)address % 4096 == 0);
    memset(address, 0xA5, bytes);
    good &= ((unsigned char *)address)[bytes-1] == 0xA5;
    good &= bs_madvise(address,bytes,4) == 0;
    good &= ((unsigned char *)address)[bytes-1] == 0;
    good &= bs_munmap(address,bytes) == 0;
    return good;
}

static bool test_looper(void) {
    void *looper=bs_looper_prepare(1);
    if (!looper) return false;
    bs_looper_wake(looper);
    bool good = bs_looper_poll_once(0,NULL,NULL,NULL) == -1;
    int pipe_fds[2];
    if (bs_pipe(pipe_fds) != 0) {
        bs_looper_release(looper);
        return false;
    }
    char command = 0;
    if (bs_pipe_fcntl(pipe_fds[0], 4, 0x800) != 0) {
        good = false;
    } else {
        good &= bs_pipe_read(pipe_fds[0], &command, 1) == -1 && errno == EAGAIN;
    }
    good &= bs_looper_add_fd(looper, pipe_fds[0], 7, 1, NULL, &command) == 1;
    const char sent = 42;
    good &= bs_pipe_write(pipe_fds[1], &sent, 1) == 1;
    int fd, events;
    void *data;
    good &= bs_looper_poll_once(0, &fd, &events, &data) == 7;
    good &= fd == pipe_fds[0] && events == 1 && data == &command;
    good &= bs_pipe_read(pipe_fds[0], &command, 1) == 1 && command == sent;
    good &= bs_pipe_close(pipe_fds[1]) == 0;
    good &= bs_looper_poll_once(0, &fd, &events, &data) == 7 && events == 8;
    good &= bs_pipe_read(pipe_fds[0], &command, 1) == 0;
    good &= bs_pipe_close(pipe_fds[0]) == 0;
    bs_looper_release(looper);
    return good;
}

typedef struct BsFrameTest {
    uint64_t owner;
    int legacy_calls;
    int64_t legacy_timestamp;
    int modern_calls;
    int64_t modern_timestamp;
    bool correct_thread;
} BsFrameTest;

static void bs_frame_test_legacy(long timestamp, void *userdata) {
    BsFrameTest *test = userdata;
    uint64_t current = 0;
    svcGetThreadId(&current, CUR_THREAD_HANDLE);
    test->correct_thread &= current == test->owner;
    test->legacy_timestamp = (int64_t)timestamp;
    test->legacy_calls++;
}

static void bs_frame_test_modern(int64_t timestamp, void *userdata) {
    BsFrameTest *test = userdata;
    uint64_t current = 0;
    svcGetThreadId(&current, CUR_THREAD_HANDLE);
    test->correct_thread &= current == test->owner;
    test->modern_timestamp = timestamp;
    test->modern_calls++;
}

/* A live VI display vsync is needed. This test deliberately fails if the
 * emulator/device lacks a working display frame event; it does not invent a
 * 60 Hz timer and report the Android NDK interface as working. */
static bool test_choreographer(void) {
    void *looper = bs_looper_prepare(1);
    if (!looper) return false;
    BsChoreographer *choreographer = bs_choreographer_get_instance();
    if (!choreographer) {
        bs_looper_release(looper);
        return false;
    }
    BsFrameTest test = {.correct_thread = true};
    svcGetThreadId(&test.owner, CUR_THREAD_HANDLE);
    bs_choreographer_post_frame(choreographer, bs_frame_test_legacy, &test);
    bs_choreographer_post_frame64(choreographer, bs_frame_test_modern, &test);
    for (unsigned i = 0; i < 30 &&
         (!test.legacy_calls || !test.modern_calls); ++i)
        bs_looper_poll_once(25, NULL, NULL, NULL);
    bs_looper_release(looper);
    return test.correct_thread && test.legacy_calls == 1 &&
           test.modern_calls == 1 && test.legacy_timestamp > 0 &&
           test.modern_timestamp == test.legacy_timestamp;
}

static bool test_so_metadata(void) {
    FILE *fp=bs_android_fopen("assets/bin/Data/boot.config","rb");
    if (!fp) return false;
    char buffer[2048];
    size_t n=fread(buffer,1,sizeof(buffer)-1,fp);
    fclose(fp);
    buffer[n]='\0';
    return strstr(buffer,"xrsdk-pre-init-library=UnityOpenXR") != NULL;
}

static bool test_offline_network(void) {
    errno = 0;
    const bool sockets_blocked =
        bs_offline_socket(2, 1, 0) == -1 && errno == ENETDOWN;
    struct addrinfo *addresses = (struct addrinfo *)(uintptr_t)1;
    const bool dns_blocked =
        bs_offline_getaddrinfo("example.invalid", NULL, NULL,
                               &addresses) == EAI_FAIL && addresses == NULL;
    return sockets_blocked && dns_blocked;
}

static bool test_physical_sensor_enumeration(void) {
    BsAndroidSensorManager *manager = bs_sensor_manager_get_instance();
    BsAndroidSensorList list = NULL;
    int count = bs_sensor_manager_get_sensor_list(manager, &list);
    if (count != 0 && count != 2) return false;
    if ((count == 0) != (list == NULL)) return false;
    if (count == 2) {
        if (bs_sensor_get_type(list[0]) != BS_ANDROID_SENSOR_ACCELEROMETER ||
            bs_sensor_get_type(list[1]) != BS_ANDROID_SENSOR_GYROSCOPE)
            return false;
    }
    return true;
}

/* Probe the actual 384-byte Quest android_app native command transport using
 * measured Horizon window/focus state, the ALooper and its virtual pipe.
 * This deliberately leaves Java Activity and all original game callbacks
 * untouched; native transport readiness alone cannot start Unity. */
static bool bs_test_dispatch_activity_native_commands(BsGameActivityGlue *glue) {
    BsGameActivityApp *app = &glue->app;
    bool valid = true;
    /* Each produced byte must be delivered by the actual ALooper and then
     * consumed by the pinned 24-byte poll-source callback protocol. */
    for (unsigned attempt = 0; attempt < 48 && glue->pendingCount; ++attempt) {
        int fd = -1, events = 0;
        void *userdata = NULL;
        int result = bs_looper_poll_once(0, &fd, &events, &userdata);
        if (result == BS_GA_LOOPER_ID_MAIN) {
            if (fd != app->msgread || !(events & BS_GA_EVENT_INPUT) ||
                userdata != &app->cmdPollSource) {
                valid = false;
                break;
            }
            BsGameActivityPollSource *source = userdata;
            source->process(app, source);
        } else if (result != -1 && result != -2 && result != -3) {
            valid = false;
            break;
        }
    }
    valid &= glue->pendingCount == 0 &&
             bs_gameactivity_glue_last_error(glue) == 0;
    return valid;
}

static bool test_gameactivity_native_transport(void) {
    BsGameActivityGlue *glue = calloc(1, sizeof(*glue));
    if (!glue) return false;
    if (bs_gameactivity_glue_prepare_native(glue) != 0) {
        free(glue);
        return false;
    }
    BsGameActivityApp *app = bs_gameactivity_glue_app(glue);
    bool valid = app == &glue->app && app->looper &&
                 app->running && app->msgread >= 0 && app->msgwrite >= 0 &&
                 !bs_gameactivity_glue_java_activity_ready(glue);
    /* The Y-key selftest only runs from the program's active
     * appletMainLoop. Observe REAL Horizon focus and emit the START/RESUME
     * commands through the native pipe without constructing Java Activity. */
    const int startup = bs_gameactivity_glue_observe_switch_lifecycle(glue, true);
    const int window = bs_gameactivity_glue_observe_switch_window(glue);
    const int focus = bs_gameactivity_glue_observe_switch_focus(glue);
    valid &= startup >= 0 && window >= 0 && focus >= 0;
    valid &= bs_test_dispatch_activity_native_commands(glue);
    /* Save/restore and teardown state follow the same original one-byte
     * GameActivity protocol. Only an observed stopped applet permits DESTROY. */
    valid &= bs_gameactivity_glue_observe_switch_lifecycle(glue, false) >= 0;
    valid &= bs_test_dispatch_activity_native_commands(glue);
    valid &= app->destroyRequested == 1;
    valid &= bs_gameactivity_glue_close_native(glue) == 0;
    valid &= app->destroyed == 1;
    free(glue);
    return valid;
}

bool bs_runtime_probe_bootstrap_abi(void) {
    const bool synchronization = test_mutex_sem();
    const bool tls_keys = test_bionic_tls_keys();
    const bool memory = test_bionic_memory_mapping();
    const bool thread_tls = test_guest_tls_isolation();
    const bool relocated_tls = test_guest_tls_relocated_initializers();
    const bool stack_canary = bs_guest_tls_is_thread_attached() &&
        ((const uintptr_t *)armGetTlsRw())[5] != 0;
    debugPrintf("[startup:abi] sync=%d tls_keys=%d mmap=%d thread_tls=%d "
                "tls_initializer=%d canary=%d\n",
                synchronization, tls_keys, memory, thread_tls, relocated_tls,
                stack_canary);
    return synchronization && tls_keys && memory && thread_tls &&
        relocated_tls && stack_canary;
}

int bs_runtime_selftest(void) {
    success_count=0;
    failure_count=0;
    printf("\nTesting native Quest Android bridge...\n");
    consoleUpdate(NULL);
    record("Real Unity 6000.3.19f1 asset", test_assets());
    record("GameActivity JNI getAssets and NDK asset read", test_assetmanager());
    record("Original XR boot configuration", test_so_metadata());
    record("Bionic mutex and semaphores", test_mutex_sem());
    record("Bionic 40-key TLS multiplex", test_bionic_tls_keys());
    record("Android anonymous mmap/munmap", test_bionic_memory_mapping());
    record("Bionic thread creation / TLS", test_thread());
    record("Original Quest static TLS descriptors / thread isolation",
           test_guest_tls_isolation());
    record("Original OVRPlugin relocated PT_TLS initializer",
           test_guest_tls_relocated_initializers());
    record("Main-thread Bionic stack canary initialized",
           bs_guest_tls_is_thread_attached() &&
           ((uintptr_t *)armGetTlsRw())[5] != 0);
    record("GameActivity pipe and Android looper", test_looper());
    record("Original GameActivity native command transport / observed window",
           test_gameactivity_native_transport());
    record("Actual VI vsync / Android Choreographer on Looper thread",
           test_choreographer());
    record("Bionic locale category / native locale ABI",
           bs_bionic_locale_abi_selftest() != 0);
    record("Offline socket/DNS failure contracts", test_offline_network());
    record("Actual Nintendo sensor availability and NDK metadata",
           test_physical_sensor_enumeration());
    record("AArch64 Linux gettid", bs_linux_syscall(178) > 0);
    printf("Selftest: %d passed, %d failed.\n",
           success_count, failure_count);
    printf("No Unity engine code was executed.\n");
    consoleUpdate(NULL);
    debugPrintf("[selftest] passed=%d failed=%d\n",success_count,failure_count);
    return failure_count;
}
