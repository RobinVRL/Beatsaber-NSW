# Architecture

The loader is a single libnx homebrew NRO (`BeatSaberQuestLoader.nro`). At run
time it opens the user's own Quest APK and OBB from the SD card, maps the
original ARM64 Android shared objects (`libunity.so`, `libil2cpp.so`,
`libmain.so` and their dependencies) into memory, resolves their imports
against a compatibility layer, and then drives Unity through the same
GameActivity lifecycle that Android would.

```
            +-----------------------------------------------+
            | Quest libraries (unmodified, from your APK)   |
            |  libunity.so  libil2cpp.so  libmain.so  ...   |
            +----------------------+------------------------+
                                   | imports (Bionic, JNI, EGL/GLES,
                                   |          OpenXR, Android NDK)
            +----------------------v------------------------+
            | Compatibility layer (this repository)         |
            |  so_util / elf_*        ELF loader, relocation |
            |  platform_bridge        import symbol table    |
            |  bionic_*               libc/pthread/mmap/...  |
            |  jni_bridge, unity_dex_* minimal Java VM + DEX |
            |  gameactivity_*         Android GameActivity   |
            |  egl_bridge             EGL/GLES hooks         |
            |  openxr_runtime         in-process OpenXR      |
            |  android_*              audio, media, sensors  |
            +----------------------+------------------------+
                                   |
            +----------------------v------------------------+
            | libnx + devkitPro portlibs                    |
            |  Mesa (nouveau, GLES 3.2) / EGL, FFmpeg, zlib |
            +-----------------------------------------------+
```

## Loading

- **`so_util.c`, `elf_dependency_order.c`, `elf_symbol_versions.c`**: parse
  each ELF, load segments in dependency order, apply relocations and run
  constructors. Code pages are made executable with `svcMapProcessCodeMemory`
  (`so_finalize`). This needs a full-memory title launch, not applet mode.
- **`platform_bridge.c`**: the import table. Every symbol the Quest libraries
  import maps to a native libnx/newlib function or to a bridge function here.
  `import-report.ps1` lists any imports that are still unresolved.
- **`guest_tls.c`**: Bionic-style TLS slots (the stack guard, `errno` and so
  on) for guest threads.

## Bionic emulation (`bionic_*.c`)

These files provide Android libc semantics on top of newlib/libnx: pthreads,
`mmap`/`madvise`, signals, epoll/eventfd, time, locale and fortify checks.

The Boehm GC that IL2CPP uses stops the world with `pthread_kill(SIGPWR)`.
`bionic_threads.c` and `thread_sampler.c` emulate that by suspending threads
with `svcSetThreadActivity`. GC is disabled by default (`GC_DONT_GC=1`).

## Java / JNI (`jni_bridge.c`, `unity_dex_*`, `gameactivity_*`)

Unity on Android needs a JavaVM. The loader provides a small JNI
implementation and a minimal DEX interpreter (`unity_dex_vm.c`). Together they
run the parts of the APK's Java code that Unity calls during start-up. That
covers the Activity, Context, resources (`unity_dex_quest_arsc_resolver.c`),
the main Looper, SurfaceView and system services. Anything Horizon-specific
is answered by `unity_dex_horizon_*` hosts.

The Oculus/Meta platform SDK is stubbed offline (`ovr_platform_offline.c`),
and all network access is refused (`offline_network_bridge.c`).

## Rendering (`egl_bridge.c`)

Unity renders with GLES 3.2 through the devkitPro Switch Mesa
(nouveau) EGL. The bridge hooks the GL entry points so that it can work around
driver and emulator gaps:

- **Multiview**: `GL_OVR_multiview` is emulated with layered rendering.
  Shaders are rewritten to select `gl_Layer` per instance.
- **Alpha8**: R8 textures swizzled to `(0,0,0,R)` are stored as RGBA8 so that
  alpha-only textures (note arrows, glows) sample correctly.
- **Light compute shader**: replaced by an equivalent transform-feedback
  vertex shader.
- **Integer divide/shift**: `idx_div_to_shift` rewrites patterns that Eden
  mis-executes (missing IMAD).
- Optional diagnostics (shader dumps, frame traces, debug colours). These are
  controlled by flag files, see [DEBUGGING.md](DEBUGGING.md).

`vulkan_bridge.c` is an experimental NVK (Mesa Vulkan) path. It is only built
with `BS_ENABLE_NVK=1` and a prebuilt `nvk-sdk/` (not included).

## XR (`openxr_runtime.c`, `joycon_*`, `headset_pose.c`)

Beat Saber talks to OpenXR. The loader includes an in-process runtime that
identifies itself as "Oculus":

- Two 1280x720 swapchain eyes. The left eye is presented to the Switch screen.
- A fixed head height of 1.70 m and a 35° vertical half-FOV. L + D-pad turns
  the head.
- Controller poses come from each Joy-Con's gyroscope (recentred on demand),
  and buttons map to Touch controller actions (`joycon_xr_actions.c`).
- `autoinput.txt` scripted input for unattended tests.

## Audio and media (`android_audio_bridge.c`, `android_media_*.c`)

AAudio/OpenSL-style output goes to libnx `audout`. The `AMediaExtractor` and
`AMediaCodec` NDK APIs are implemented with FFmpeg so that songs (Ogg/Vorbis)
and videos decode on the console.

## Assets (`asset_fs_bridge.c`, `unity_apk_*`)

`AAssetManager` and Unity's file access read directly from the APK (stored ZIP
entries) and the OBB on the SD card. Nothing is extracted into the NRO.
