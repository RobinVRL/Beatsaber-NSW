/* Quest 1.40.8 startup (Unity 2022.3.33f1, UnityPlayerActivity).
 *
 * Included from main.c (needs its statics). Replaces the 1.46 GameActivity
 * chain. Android order being reproduced without Java bytecode:
 *   UnityPlayerActivity.onCreate -> new UnityPlayer(activity)
 *     -> System.loadLibrary("main") / NativeLoader.load -> libunity JNI_OnLoad
 *     -> initJni(context)
 *   surfaceCreated  -> nativeRecreateGfxState(0, surface)
 *   onResume        -> nativeResume(); onWindowFocusChanged -> nativeFocusChanged(true)
 *   UnityMain loop  -> nativeRender() every frame
 * libmain.so here is MBF's LibMainLoader (Scotland2). Its NativeLoader.load
 * path is only taken when the "load-mods" flag file exists on the SD card;
 * otherwise libunity's JNI_OnLoad is entered directly (unmodded run). */

static bool u1408_started;
static bool u1408_surface_bound;
static bool u1408_resumed;
static unsigned long u1408_frames;

static const BSJNINativeRegistration *u1408_native(const char *name) {
    const BSJNINativeRegistration *hit = NULL;
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *m = bs_jni_registration_at(i);
        if (!m || strcmp(m->method_name, name)) continue;
        /* Prefer the base UnityPlayer class when several classes share it. */
        if (!hit || !strcmp(m->class_name, "com/unity3d/player/UnityPlayer"))
            hit = m;
    }
    return hit;
}

static void u1408_dump_natives(void) {
    for (size_t i = 0; i < bs_jni_registration_count(); ++i) {
        const BSJNINativeRegistration *m = bs_jni_registration_at(i);
        if (m) debugPrintf("[1408:natives] %s.%s%s = %#lx\n", m->class_name,
                           m->method_name, m->signature,
                           (unsigned long)m->function_address);
    }
}

static bool u1408_onload(const char *library, bool required) {
    so_module *module = NULL;
    for (int i = 0; i < MODULE_COUNT; ++i)
        if (!strcmp(module_names[i], library)) module = &modules[i];
    uintptr_t entry = module ? so_find_module_export(module, "JNI_OnLoad") : 0;
    if (!entry) {
        debugPrintf("[1408] %s has no JNI_OnLoad\n", library);
        return !required;
    }
    BSJNIReadiness ready = {.platform_services_ready = 1};
    debugPrintf("[1408] entering original JNI_OnLoad: %s\n", library);
    int32_t version = bs_jni_call_onload_if_ready(library, (BSJNIOnLoadFn)entry,
                                                  &ready);
    debugPrintf("[1408] %s JNI_OnLoad -> 0x%08x (%s)\n", library,
                (unsigned)version, version > 0 ? "ok" : bs_jni_last_error());
    if (bs_vulkan_graphics_boundary_seen()) return false;
    return version > 0 || !required;
}

/* Call a registered UnityPlayer native with no extra args, (Z) or (I...). */
static bool u1408_call_void(const char *name) {
    const BSJNINativeRegistration *m = u1408_native(name);
    if (!m) { debugPrintf("[1408] native %s not registered\n", name); return false; }
    debugPrintf("[1408] -> %s%s\n", name, m->signature);
    ((void (*)(void *, void *))m->function_address)(
        bs_jni_env(), bs_jni_unityplayer_host_object());
    return !jni_take_exception(bs_jni_env(), name);
}

static bool u1408_call_bool(const char *name, bool value) {
    const BSJNINativeRegistration *m = u1408_native(name);
    if (!m) { debugPrintf("[1408] native %s not registered\n", name); return false; }
    debugPrintf("[1408] -> %s%s(%d)\n", name, m->signature, (int)value);
    ((void (*)(void *, void *, uint8_t))m->function_address)(
        bs_jni_env(), bs_jni_unityplayer_host_object(), value);
    return !jni_take_exception(bs_jni_env(), name);
}

static bool u1408_init_jni(void) {
    const BSJNINativeRegistration *m = u1408_native("initJni");
    if (!m) { debugPrintf("[1408] initJni not registered\n"); return false; }
    void *env = bs_jni_env(), *player = bs_jni_unityplayer_host_object();
    void *context = gameactivity_native_proxy;
    debugPrintf("[1408] -> initJni%s player=%p context=%p\n", m->signature,
                player, context);
    if (!strcmp(m->signature, "(Landroid/content/Context;)V")) {
        ((void (*)(void *, void *, void *))m->function_address)(env, player, context);
    } else {
        /* Variants with trailing int/String args: pass 0 / "". */
        uintptr_t new_string = jni_env_slot(env, 167);
        void *empty = new_string ?
            ((void *(*)(void *, const char *))new_string)(env, "") : NULL;
        if (strstr(m->signature, "ILjava/lang/String;"))
            ((void (*)(void *, void *, void *, int32_t, void *))
                m->function_address)(env, player, context, 0, empty);
        else
            ((void (*)(void *, void *, void *, void *))
                m->function_address)(env, player, context, empty);
    }
    return !jni_take_exception(env, "initJni");
}

static bool u1408_bind_surface(void) {
    if (u1408_surface_bound) return true;
    const BSJNINativeRegistration *m = u1408_native("nativeRecreateGfxState");
    if (!m || !original_java_surface) {
        debugPrintf("[1408] nativeRecreateGfxState=%p surface=%p\n",
                    (void *)m, original_java_surface);
        return false;
    }
    debugPrintf("[1408] -> nativeRecreateGfxState%s(0, %p)\n", m->signature,
                original_java_surface);
    ((void (*)(void *, void *, int32_t, void *))m->function_address)(
        bs_jni_env(), bs_jni_unityplayer_host_object(), 0, original_java_surface);
    if (jni_take_exception(bs_jni_env(), "nativeRecreateGfxState")) return false;
    u1408_surface_bound = true;
    if (u1408_native("nativeSendSurfaceChangedEvent"))
        u1408_call_void("nativeSendSurfaceChangedEvent");
    return true;
}

/* OculusUnity.initOculus(): the Java side attaches the activity SurfaceView
 * and its callback calls the native surfaceCreated(surface), which releases
 * OculusDisplayProvider::GfxThread_Start. Do that synchronously here. */
static int u1408_host_init_oculus(void *user, void *env, void *receiver,
                                  const BSJNIValue *args, size_t argc,
                                  BSJNIValue *result) {
    (void)user; (void)receiver; (void)args; (void)argc; (void)result;
    for (int i = 0; i < MODULE_COUNT; ++i) {
        if (strcmp(module_names[i], "libOculusXRPlugin.so")) continue;
        uintptr_t f = so_find_module_export(&modules[i],
            "Java_com_unity_oculus_OculusUnity_surfaceCreated");
        debugPrintf("[1408] initOculus -> surfaceCreated(%p) fn=%#lx\n",
                    original_java_surface, (unsigned long)f);
        if (f) ((void (*)(void *, void *, void *))f)(env, NULL,
                                                      original_java_surface);
    }
    return 0;
}

/* Diagnostic: compare guest memory speed in the newlib heap against the
 * mmap overcommit window (code region). */
static void u1408_membench(void) {
    extern void *bs_mmap(void *, size_t, int, int, int, int64_t);
    extern int bs_munmap(void *, size_t);
    const size_t n = 8u << 20;
    void *bufs[2] = { malloc(n), bs_mmap(NULL, n, 3, 0x22, -1, 0) };
    const char *names[2] = { "heap", "window" };
    for (int b = 0; b < 2; ++b) {
        volatile uint8_t *p = bufs[b];
        if (!p || p == (void *)-1) continue;
        u64 t0 = armGetSystemTick();
        uint32_t x = 1, sum = 0;
        for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)i;
        for (size_t i = 0; i < 4000000; ++i) {
            x = x * 1664525u + 1013904223u;
            sum += p[x & (n - 1)];
            p[(x >> 7) & (n - 1)] = (uint8_t)sum;
        }
        u64 ns = armTicksToNs(armGetSystemTick() - t0);
        debugPrintf("[membench] %s %p: %lu ms (sum %u)\n", names[b], (void *)p,
                    (unsigned long)(ns / 1000000), sum);
    }
    free(bufs[0]);
    if (bufs[1] && bufs[1] != (void *)-1) bs_munmap(bufs[1], n);
}

static bool bs1408_start_unity(int count) {
    u1408_membench();
    if (bs_jni_register_host_method("com/unity/oculus/OculusUnity",
            "initOculus", "()V", 0, u1408_host_init_oculus, NULL) != BS_JNI_OK)
        debugPrintf("[1408] initOculus host method not registered\n");
    bs_host_console_release_for_graphics();
    { extern void bs_egl_precreate_xr_window(void); bs_egl_precreate_xr_window(); }
    const bool assets = bs_probe_original_asset_manager();
    const bool media = bs_android_media_decode_selftest();
    debugPrintf("[1408] assets=%d media-decoder=%d egl=%d\n", assets, media,
                g_egl_host_ready);

    /* Activity/Context host: the bridge's bound Activity proxy plus a real
     * NWindow-backed Surface and observed Configuration. */
    NWindow *window = nwindowGetDefault();
    original_java_surface = bs_jni_surface_from_nwindow(window);
    if (original_java_surface) original_java_window = window;
    gameactivity_java_configuration = bs_gameactivity_observe_configuration(window);
    void *asset_manager = bs_jni_asset_manager();
    int bound = bs_jni_gameactivity_bind_host(gameactivity_native_proxy,
        asset_manager, gameactivity_java_configuration, original_java_surface);
    bs_gameactivity_prepare_directory(BS_GAME_ROOT "files/");
    bs_gameactivity_prepare_directory(BS_GAME_ROOT "obb/");
    bs_gameactivity_prepare_directory(BS_GAME_ROOT "external-files/");
    int ctx = bs_jni_unityplayer_observe_current_context(gameactivity_native_proxy);
    int act = bs_jni_unityplayer_observe_current_activity(gameactivity_native_proxy);
    debugPrintf("[1408] host bind=%d currentContext=%d currentActivity=%d "
                "surface=%p config=%p\n", bound, ctx, act,
                original_java_surface, gameactivity_java_configuration);

    if (!original_constructor_order_valid) return false;
    setenv("OPENSSL_armcap", "0", 1);
    if (unity_window_graphics) bs_vulkan_enable_unity_window();
    else bs_vulkan_arm_graphics_boundary(true);

    for (int i = 0; i < count; ++i) {
        so_module *module = &modules[original_constructor_order[i]];
        debugPrintf("[1408] constructors: %s\n", module->name);
        so_execute_init_array(module, bs_vulkan_graphics_boundary_seen);
        if (bs_vulkan_graphics_boundary_seen()) return false;
    }

    struct stat flag;
    const bool load_mods = stat(BS_GAME_ROOT "load-mods", &flag) == 0;
    debugPrintf("[1408] mod loading %s (%s)\n", load_mods ? "enabled" : "disabled",
                BS_GAME_ROOT "load-mods");
    if (load_mods) {
        /* Mods' mimalloc reads environ directly: ask for small arenas so
         * its 1-2 GiB reservations don't exhaust the Switch 1 window. */
        setenv("MIMALLOC_ARENA_RESERVE", "64MiB", 1);
        setenv("MIMALLOC_SHOW_ERRORS", "1", 1);
        /* Reserve PROT_NONE and commit via mprotect (Eden can't fault in
         * untouched pages), so only used memory gets backed. */
        setenv("MIMALLOC_ARENA_EAGER_COMMIT", "0", 1);
        setenv("MIMALLOC_EAGER_COMMIT", "0", 1);
        setenv("MIMALLOC_PURGE_DECOMMITS", "1", 1);
        /* Scotland2, driven the way LibMainLoader does it (its Java-heavy
         * search/copy of libsl2 is skipped): preload -> load -> libunity
         * JNI_OnLoad -> accept_unity_handle. sl2 then hooks il2cpp_init. */
        static const char sl2_src[] =
            "/sdcard/ModData/com.beatgames.beatsaber/Modloader/libsl2.so";
        extern const char *bs_android_translate_path(const char *, char *, size_t);
        char native[768];
        so_module *sl2 = NULL;
        if (bs_android_translate_path(sl2_src, native, sizeof native))
            sl2 = bs_runtime_dlopen(native, sl2_src);
        typedef void (*preload_fn)(void *, const char *, const char *, const char *,
                                   const char *, const char *);
        typedef void (*load_fn)(void *, const char *);
        typedef void (*accept_fn)(void *, void *);
        preload_fn preload = sl2 ? (preload_fn)so_find_module_export(sl2, "modloader_preload") : NULL;
        load_fn load = sl2 ? (load_fn)so_find_module_export(sl2, "modloader_load") : NULL;
        accept_fn accept = sl2 ? (accept_fn)so_find_module_export(sl2,
            "modloader_accept_unity_handle") : NULL;
        debugPrintf("[sl2] module=%p preload=%p load=%p accept=%p\n", (void *)sl2,
                    (void *)preload, (void *)load, (void *)accept);
        if (preload && load && accept) {
            debugPrintf("[sl2] -> modloader_preload\n");
            preload(bs_jni_env(), "com.beatgames.beatsaber", sl2_src, sl2_src,
                    "/data/data/com.beatgames.beatsaber/files",
                    "/storage/emulated/0/Android/data/com.beatgames.beatsaber/files");
            debugPrintf("[sl2] -> modloader_load\n");
            load(bs_jni_env(), "/data/app/com.beatgames.beatsaber/lib/arm64");
            debugPrintf("[sl2] libs and early mods opened\n");
        }
        if (!u1408_onload("libunity.so", true)) return false;
        if (preload && load && accept) {
            so_module *unity = so_find_module_by_name("libunity.so");
            debugPrintf("[sl2] -> modloader_accept_unity_handle(%p)\n", (void *)unity);
            accept(bs_jni_env(), unity);
            debugPrintf("[sl2] il2cpp_init hook installed\n");
        }
    } else {
        if (!u1408_onload("libunity.so", true)) return false;
    }
    static const char *const plugins[] = {
        "libil2cpp.so", "libOVRPlugin.so", "libovrplatformloader.so",
        "libOculusXRPlugin.so"
    };
    for (size_t i = 0; i < sizeof plugins / sizeof plugins[0]; ++i)
        if (!u1408_onload(plugins[i], false)) return false;
    u1408_dump_natives();

    if (!u1408_init_jni()) return false;
    if (bs_vulkan_graphics_boundary_seen()) return false;
    u1408_bind_surface();
    if (u1408_native("nativeResume")) u1408_call_void("nativeResume");
    if (u1408_native("nativeFocusChanged")) u1408_call_bool("nativeFocusChanged", true);
    u1408_resumed = true;
    u1408_started = true;
    original_java_started = true;
    debugPrintf("[1408] UnityPlayer started; entering nativeRender loop\n");
    return true;
}

/* One UnityMain loop iteration. */
static bool bs1408_pump(bool running) {
    if (!u1408_started) return true;
    if (!running) {
        if (u1408_resumed && u1408_native("nativePause")) {
            const BSJNINativeRegistration *m = u1408_native("nativePause");
            ((uint8_t (*)(void *, void *))m->function_address)(
                bs_jni_env(), bs_jni_unityplayer_host_object());
        }
        u1408_resumed = false;
        return false;
    }
    if (!u1408_surface_bound) u1408_bind_surface();
    const BSJNINativeRegistration *render = u1408_native("nativeRender");
    if (!render) return false;
    uint8_t keep = ((uint8_t (*)(void *, void *))render->function_address)(
        bs_jni_env(), bs_jni_unityplayer_host_object());
    static int last_keep = -1;
    if (++u1408_frames <= 3 || u1408_frames % 30 == 0 || keep != last_keep) {
        struct mallinfo mi = mallinfo();
        extern size_t bs_guest_alloc_bytes[4];
        debugPrintf("[1408] nativeRender #%lu -> %d heap inuse=%zuMiB cum unity=%zu il2cpp=%zu other=%zu host=%zu MiB\n",
                    u1408_frames, keep, (size_t)mi.uordblks >> 20,
                    bs_guest_alloc_bytes[0] >> 20, bs_guest_alloc_bytes[1] >> 20,
                    bs_guest_alloc_bytes[2] >> 20, bs_guest_alloc_bytes[3] >> 20);
    }
    last_keep = keep;
    jni_take_exception(bs_jni_env(), "nativeRender");
    /* false = Unity wants the Activity finished; Horizon apps exit via HOME. */
    return true;
}
