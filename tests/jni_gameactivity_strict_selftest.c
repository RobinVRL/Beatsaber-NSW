/* JNI 1.6 regression test for the exact original Quest GameActivity table.
 *
 * Run on Windows with the AOSP/Android NDK JNI header:
 * clang -std=c11 -Wall -Wextra -Werror \
 *   -I<path-to-jni.h> \
 *   -Isource tests/jni_gameactivity_strict_selftest.c \
 *   source/jni_bridge.c -o tests/jni_gameactivity_strict_selftest.exe
 *
 * Include the independent 77-member/24-method pinned original test so its
 * real ELF-derived data can also exercise re-registration and failure cases.
 * No Quest ELF function or Java/Dex bytecode executes in this test.
 */
#define main bs_original_gameactivity_metadata_main
#include "jni_gameactivity_metadata_selftest.c"
#undef main

/* jni_bridge.c's File/AssetManager methods call the Switch filesystem bridge
 * (asset_fs_bridge.c), which is not linked into this host test. No mapped
 * Android filesystem exists here, so every operation fails as ENOENT. */
#include <errno.h>
#include <stdio.h>
FILE *bs_android_fopen(const char *path, const char *mode) {
    (void)path; (void)mode; errno = ENOENT; return NULL;
}
int bs_android_stat(const char *path, void *out) {
    (void)path; (void)out; errno = ENOENT; return -1;
}
int bs_android_mkdir(const char *path, unsigned mode) {
    (void)path; (void)mode; errno = ENOENT; return -1;
}
int bs_android_unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd; (void)path; (void)flags; errno = ENOENT; return -1;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1
#include <windows.h>
#endif

static jint jni_call_static_int_v(JNIEnv *env, jclass clazz,
                                   jmethodID method, ...) {
    va_list arguments;
    va_start(arguments, method);
    const jint value = (*env)->CallStaticIntMethodV(env, clazz, method, arguments);
    va_end(arguments);
    return value;
}

static jint fake_onload_proof(void *vm, void *reserved) {
    assert(vm && !reserved);
    return BS_JNI_VERSION_1_6;
}

static void test_native_registration_transactions(JNIEnv *env) {
    jclass game = (*env)->FindClass(env, GAME);
    assert(game);
    assert(bs_jni_gameactivity_natives_captured() == 1);
    const size_t original_count = bs_jni_registration_count();
    assert(original_count == 24);

    JNINativeMethod original[24];
    for (size_t i = 0; i < 24; ++i) {
        original[i].name = (char *)original_natives[i].name;
        original[i].signature = (char *)original_natives[i].signature;
        original[i].fnPtr = (void *)(uintptr_t)original_natives[i].elf_vaddr;
    }

    /* Re-registering the SAME original declarations is idempotent, not a
     * second 24-entry append or a premature capacity failure. */
    assert((*env)->RegisterNatives(env, game, original, 24) == JNI_OK);
    assert(bs_jni_registration_count() == original_count);
    assert(bs_jni_gameactivity_natives_captured() == 1);

    JNINativeMethod altered[24];
    memcpy(altered, original, sizeof altered);
    altered[9].fnPtr = (void *)(uintptr_t)0xdead1234;
    assert((*env)->RegisterNatives(env, game, altered, 24) == JNI_ERR);
    assert(bs_jni_registration_count() == original_count);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);

    /* A partial GameActivity table must never be mistaken for a successful
     * Java GameActivity registration. */
    assert((*env)->RegisterNatives(env, game, original, 23) == JNI_ERR);
    assert(bs_jni_registration_count() == original_count);

    memcpy(altered, original, sizeof altered);
    altered[12].signature = "(J)V";
    assert((*env)->RegisterNatives(env, game, altered, 24) == JNI_ERR);
    assert(bs_jni_registration_count() == original_count);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    /* Real JNI calls cannot continue class lookups while an exception pends. */
    assert((*env)->FindClass(env, GAME) == NULL);
    (*env)->ExceptionClear(env);

    memcpy(altered, original, sizeof altered);
    altered[11] = altered[10]; /* duplicate method tuple in same batch */
    assert((*env)->RegisterNatives(env, game, altered, 24) == JNI_ERR);
    assert(bs_jni_registration_count() == original_count);

    jclass native_loader = (*env)->FindClass(
        env, "com/unity3d/player/NativeLoader");
    assert(native_loader);
    JNINativeMethod native = {
        "nativeBootstrap", "()V", (void *)(uintptr_t)0x12345678
    };
    /* This other symbolic native class is used by original libmain JNI_OnLoad;
     * GameActivity's exact table restrictions do not affect it. */
    assert((*env)->RegisterNatives(env, native_loader, &native, 1) == JNI_OK);
    assert(bs_jni_registration_count() == original_count + 1);
    assert(bs_jni_gameactivity_natives_captured() == 1);
}

static void test_reference_ownership(JNIEnv *env) {
    /* Popping without a pushed frame is a JNI error. */
    assert((*env)->PopLocalFrame(env, NULL) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    assert((*env)->EnsureLocalCapacity(env, 4) == JNI_OK);
    assert((*env)->EnsureLocalCapacity(env, -1) == JNI_ERR);

    jclass clazz = (*env)->FindClass(env, TYPE);
    assert(clazz);
    /* Local frames release frame-local aliases; PopLocalFrame keeps result. */
    assert((*env)->PushLocalFrame(env, 4) == JNI_OK);
    jobject scoped = (*env)->NewLocalRef(env, clazz);
    jobject dropped = (*env)->NewLocalRef(env, clazz);
    assert(scoped && dropped);
    jobject survivor = (*env)->PopLocalFrame(env, scoped);
    assert(survivor && survivor != scoped);
    assert((*env)->IsSameObject(env, survivor, clazz) == JNI_TRUE);
    assert((*env)->GetObjectRefType(env, dropped) == JNIInvalidRefType);
    (*env)->DeleteLocalRef(env, survivor);
    assert((*env)->GetObjectRefType(env, clazz) == JNILocalRefType);
    jobject global = (*env)->NewGlobalRef(env, clazz);
    assert(global && global != clazz);
    assert((*env)->GetObjectRefType(env, global) == JNIGlobalRefType);
    assert((*env)->IsSameObject(env, clazz, global) == JNI_TRUE);
    assert((*env)->GetObjectClass(env, global) ==
           (*env)->FindClass(env, "java/lang/Class"));

    jobject local = (*env)->NewLocalRef(env, global);
    assert(local && local != global && local != clazz);
    assert((*env)->GetObjectRefType(env, local) == JNILocalRefType);
    assert((*env)->IsSameObject(env, global, local) == JNI_TRUE);

    jweak weak = (*env)->NewWeakGlobalRef(env, clazz);
    assert(weak);
    assert((*env)->GetObjectRefType(env, weak) == JNIWeakGlobalRefType);
    assert((*env)->IsSameObject(env, weak, clazz) == JNI_TRUE);

    assert((*env)->IsInstanceOf(env, NULL, clazz) == JNI_TRUE);
    assert((*env)->IsInstanceOf(env, bs_jni_asset_manager(), clazz) ==
           JNI_FALSE);

    (*env)->DeleteLocalRef(env, local);
    assert((*env)->GetObjectRefType(env, local) == JNIInvalidRefType);
    (*env)->DeleteGlobalRef(env, global);
    assert((*env)->GetObjectRefType(env, global) == JNIInvalidRefType);
    (*env)->DeleteWeakGlobalRef(env, weak);
    assert((*env)->GetObjectRefType(env, weak) == JNIInvalidRefType);

    jobject fake = (jobject)(uintptr_t)0x424242;
    assert((*env)->NewGlobalRef(env, fake) == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    assert((*env)->GetObjectRefType(env, fake) == JNIInvalidRefType);

    jstring a = (*env)->NewStringUTF(env, "same bytes");
    jstring b = (*env)->NewStringUTF(env, "same bytes");
    assert(a && b && a != b); /* Java strings are objects, not interned bytes. */
    assert((*env)->IsSameObject(env, a, b) == JNI_FALSE);
    assert(strcmp((*env)->GetStringUTFChars(env, a, NULL), "same bytes") == 0);

    assert((*env)->FindClass(env, "not.a/valid/ClassName") == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    assert(strstr(bs_jni_last_error(), "NoClassDefFoundError") != NULL);
    (*env)->ExceptionClear(env);
    assert((*env)->FindClass(env, "/leadingSlash") == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    assert((*env)->FindClass(env, "double//slash") == NULL);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
}

static void test_exact_static_masks(JNIEnv *env) {
    const struct { const char *name; jint value; } masks[] = {
        {"statusBars", 1}, {"navigationBars", 2}, {"captionBar", 4},
        {"ime", 8}, {"systemGestures", 16},
        {"mandatorySystemGestures", 32}, {"tappableElement", 64},
        {"displayCutout", 128}, {"systemBars", 7}
    };
    jclass type = (*env)->FindClass(env, TYPE);
    jclass unrelated = (*env)->FindClass(env, CONFIG);
    assert(type && unrelated);
    for (size_t i = 0; i < sizeof masks/sizeof masks[0]; ++i) {
        jmethodID method = (*env)->GetStaticMethodID(env, type,
                                                     masks[i].name, "()I");
        assert(method && (*env)->ExceptionCheck(env) == JNI_FALSE);
        assert((*env)->CallStaticIntMethod(env, type, method) == masks[i].value);
        assert(jni_call_static_int_v(env, type, method) == masks[i].value);
        assert((*env)->CallStaticIntMethodA(env, type, method, NULL) ==
               masks[i].value);
    }

    jmethodID ime = (*env)->GetStaticMethodID(env, type, "ime", "()I");
    assert((*env)->CallStaticIntMethod(env, unrelated, ime) == 0);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionDescribe(env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
}

static void test_real_observed_fields(JNIEnv *env) {
    BSJNIObservedField measured[] = {
        {"screenWidthDp", "I", 730, 0},
        {"fontScale", "F", 0, 1.125f}
    };
    jobject config = bs_jni_object_from_observed_fields(CONFIG,
                          measured, sizeof measured / sizeof measured[0]);
    assert(config);
    jclass config_class = (*env)->FindClass(env, CONFIG);
    assert((*env)->GetObjectClass(env, config) == config_class);
    jfieldID width = (*env)->GetFieldID(env, config_class,
                                       "screenWidthDp", "I");
    jfieldID font = (*env)->GetFieldID(env, config_class,
                                      "fontScale", "F");
    assert(width && font);
    assert((*env)->GetIntField(env, config, width) == 730);
    assert((*env)->GetFloatField(env, config, font) == 1.125f);
    jfieldID height = (*env)->GetFieldID(env, config_class,
                                        "screenHeightDp", "I");
    assert(height);
    assert((*env)->GetIntField(env, config, height) == 0);
    assert((*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);

    BSJNIObservedField duplicate[] = {
        {"screenWidthDp", "I", 730, 0},
        {"screenWidthDp", "I", 730, 0}
    };
    assert(bs_jni_object_from_observed_fields(CONFIG, duplicate, 2) == NULL);
}

#if defined(_WIN32)
typedef struct BsJniThreadCheck {
    JavaVM *vm;
    const void *main_thread_env;
    int passed;
} BsJniThreadCheck;

static DWORD WINAPI bs_jni_worker_thread(LPVOID opaque) {
    BsJniThreadCheck *check = (BsJniThreadCheck *)opaque;
    JNIEnv *worker = NULL;
    assert((*check->vm)->GetEnv(check->vm, (void **)&worker,
                                 JNI_VERSION_1_6) == JNI_EDETACHED);
    assert(worker == NULL);
    assert(bs_jni_env() == NULL);
    assert((*check->vm)->AttachCurrentThread(check->vm, &worker, NULL) ==
           JNI_OK);
    assert(worker && *worker && (const void *)worker != check->main_thread_env);
    assert(worker == (JNIEnv *)bs_jni_env());
    assert((*worker)->ExceptionCheck(worker) == JNI_FALSE);
    jclass game = (*worker)->FindClass(worker, GAME);
    assert(game);
    assert((*worker)->GetStaticMethodID(worker, game,
                                       "finish", "()V") == NULL);
    assert((*worker)->ExceptionCheck(worker) == JNI_TRUE);
    (*worker)->ExceptionClear(worker);
    assert((*worker)->ExceptionCheck(worker) == JNI_FALSE);
    assert((*check->vm)->DetachCurrentThread(check->vm) == JNI_OK);
    assert(bs_jni_env() == NULL);
    check->passed = 1;
    return 0;
}

static void test_jni_env_thread_identity(JNIEnv *env) {
    JavaVM *vm = (JavaVM *)bs_jni_vm();
    BsJniThreadCheck context = {vm, env, 0};
    HANDLE worker = CreateThread(NULL, 0, bs_jni_worker_thread,
                                 &context, 0, NULL);
    assert(worker != NULL);
    assert(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0);
    assert(CloseHandle(worker));
    assert(context.passed);
    assert(bs_jni_env() == env);
    assert((*env)->ExceptionCheck(env) == JNI_FALSE);
}
#else
static void test_jni_env_thread_identity(JNIEnv *env) {
    (void)env;
}
#endif

int main(void) {
    assert(bs_original_gameactivity_metadata_main() == 0);
    JNIEnv *env = (JNIEnv *)bs_jni_env();
    assert(env && *env);

    test_native_registration_transactions(env);
    test_reference_ownership(env);
    test_exact_static_masks(env);
    test_real_observed_fields(env);
    test_jni_env_thread_identity(env);

    BSJNIReadiness declared_ready = {0, 0, 1};
    assert(bs_jni_call_onload_if_ready(
        "sdmc:/switch/beat-saber-quest/libgame.so",
        fake_onload_proof, &declared_ready) == JNI_ERR);
    assert(strstr(bs_jni_last_error(), "GameActivity Java lifecycle") != NULL);
    assert(bs_jni_call_onload_if_ready(
        "isolated-jni-bootstrap-test", fake_onload_proof,
        &declared_ready) == JNI_VERSION_1_6);

    puts("PASS: exact GameActivity native ABI transactions, JNI ref identity, "
         "observed fields, AndroidX masks and fail-closed Java lifecycle");
    return 0;
}
