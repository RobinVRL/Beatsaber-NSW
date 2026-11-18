# Beat Saber Quest Android engine -> Switch homebrew loader

## Local input and version

The user supplied `SwitchPort/beat-saber.apk` (66,583,730 bytes), the
`SwitchPort/main.3491.com.beatgames.beatsaber.obb` expansion archive
(1,597,460,439 bytes), and 34 additional named UnityFS content packages.
These are **private owner-provided game files**; keep them local, out of Git
commits and public release packages.

The APK is package `com.beatgames.beatsaber`, Android version
`1.46.0_34060` (`versionCode=3491`). This is **not** the earlier Windows
`1.40.8_7379` build, and its asset/runtime ABI must be handled separately.
The separate 34 loose files are from mixed UnityFS versions (27 from
`2021.3.16f1`, six from `2022.3.33f1`, and one from `6000.0.40f1`).
**None** match the `6000.3.19f1` core OBB's Android bundles. Treat these
loose content files as **unverified external content** until their
provenance and compatibility are established.

Crucially, `assets/bin/Data/globalgamemanagers` identifies the Quest Unity
engine as **6000.3.19f1 (Unity 6)**. The APK uses **IL2CPP**, with
`assets/bin/Data/Managed/Metadata/global-metadata.dat` (21,260,344 bytes).
The Windows Beat Saber build uses Unity 2022.3.33f1 and Mono and is not a
binary-compatible source of the Quest engine.

The Android manifest launches `com.unity3d.player.UnityPlayerGameActivity`
with Oculus VR category and requires VR head tracking. Its OpenXR subsystem
manifest is **UnityOpenXR 1.17.1**. The `boot.config` contains
`xrsdk-pre-init-library=UnityOpenXR` and Vulkan XR-specific options,
including offscreen swapchain and fragment density map settings.

## Inspected ARM64 shared libraries

The following are ELF64 AArch64 dynamic libraries extracted from the owner's
APK into `import/quest-engine/APK/lib/arm64-v8a` with
`scripts/stage-quest-engine.ps1`:

| Library | Original uncompressed bytes | Purpose |
| --- | ---: | --- |
| `libunity.so` | 24,496,192 | Unity native runtime |
| `libil2cpp.so` | 130,481,456 | Compiled Unity/Beat Saber scripting runtime |
| `libmain.so` | 6,696 | Android Unity bootstrap |
| `libgame.so` | 152,576 | Android GameActivity native glue |
| `libUnityOpenXR.so` | 1,446,608 | OpenXR Unity plugin |
| `libopenxr_loader.so` | 480,224 | Android OpenXR loader |
| `libOVRPlugin.so` | 5,291,104 | Meta/Oculus XR integration |
| `lib_burst_generated.so` | 347,128 | Unity Burst native code |

The other dependencies include `libandroid.so`, `libmediandk.so`,
`libEGL.so`, `libGLESv3.so`, Android `liblog.so`, `libm.so`,
`libdl.so`, and `libc.so`/Bionic. Some libraries were included in the
APK, others are platform-provided and must be implemented or bridged on
Switch.

`readelf -Ws` confirms that `libmain.so` and `libunity.so` export
`JNI_OnLoad`, `libil2cpp.so` exports `il2cpp_init`, and
`libgame.so` exports `android_main` and
`Java_com_google_androidgamesdk_GameActivity_initializeNativeCode`.
The last two mean this Quest build includes **GameActivity startup logic**
that a loader designed for classic `UnityPlayer` may not satisfy.

The OBB itself is a valid ZIP archive, with Android-targeted UnityFS assets
under `assets/aa/Android`, audio/beatmap bundles under
`assets/BeatmapLevelsData`, and an Android Addressables catalog.
The owner has provided meaningful original game content. No public bundle
distribution is needed.

## Active loader source

See [beat-saber-engine](beat-saber-engine/) for the original-engine loader and
[the current startup audit](beat-saber-engine/PREGRAPHICS_HANDOFF_AUDIT.md).

## Genuine engine integration milestones

1. Create Beat Saber-specific `GameActivity`/JNI/NDK bootstrap and port the
   so-loader imports for actual Unity **6000.3.19f1**, with explicit validation
   against the supplied libraries. No hardcoded Unity 2022 patch offsets.
2. Verify the Unity 6 engine's entrypoints and memory layout under libnx,
   resolve all required Bionic/JNI/platform symbols, and reach engine
   initialization on hardware while preserving useful crash logs.
3. Provide a verified GLES/Vulkan graphics path for the actual Unity 6
   rendering configuration and Android assets. The PvZ wrapper's EGL/GLES3
   setup does not establish Vulkan/OpenXR compatibility by itself.
4. Recreate GameActivity lifecycle, Android input/audio/filesystem services,
   load the supplied Android Addressables/beatmap assets, and establish
   original game scene/gameplay startup.
5. Implement a real or explicitly adapted XR view/controller input layer;
   ordinary Joy-Cons have no equivalent of Quest 6DoF head/hand tracking.
   VR gameplay and comfort cannot be assumed even after engine startup.

Passing ELF/static archive checks **does not** prove the Unity engine can run
in libnx or on Nintendo Switch hardware. A buildable loader and a functioning
original game are different milestones.

## Native loader milestone implemented

`switch-port/beat-saber-engine/` now contains a **separate** homebrew target
`BeatSaberQuestLoader.nro`, built from a bounded AArch64 ELF verifier and
an MIT-licensed `so_util` loader adapted from the reference. On the console,
it checks that the owner's `libmain.so`, `libunity.so`, `libil2cpp.so`
and `libgame.so` are available and offers **A** to map their actual PT_LOAD
segments, apply relative relocations and report unresolved import
relocations in `loader.log`. It does not invoke Unity's initialization or
render functions. Runtime execution remains **untested** on hardware.

`beat-saber-engine/prepare-sd.ps1` creates the private local
`sdmc:/switch/beat-saber-quest/` layout from the supplied APK and OBB,
including the engine and matching Android assets. It supports a read-only
`-DryRun`. All commercial game files remain external to the homebrew
binary and must not be redistributed.

This stage establishes a testable native ELF loading foundation. Remaining
requirements include Android/Bionic imports, GameActivity/JNI, runtime TLS,
graphics, OpenXR, tracked input and original gameplay boot.

The separate `beat-saber-engine/import-report.ps1` compares the 15 staged
ELF shared libraries with their exports and the initial Switch C ABI bridge,
and records the result in `beat-saber-engine/abi-report.json`. The present
analysis finds **1,568 unresolved symbol names counted per library**. This
includes unresolved Android system, GameActivity, graphics, Meta, C++
runtime and other native interfaces; some imports are optional/weak.
No attempt has been made to substitute permissive placeholders for required
engine/XR functions or bypass entitlement checks.

## Reproducible local checks

From PowerShell at the installed `1.40.8` directory:

```powershell
& .\switch-port\scripts\stage-quest-engine.ps1 -VerifyOnly
& .\switch-port\scripts\stage-quest-engine.ps1
& 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-readelf.exe' -d '.\switch-port\import\quest-engine\APK\lib\arm64-v8a\libunity.so'
```

The staging script extracts **21 selected engine libraries/metadata files**
for inspection and preserves the original APK/OBB untouched. It does not
unpack the ~1.6 GB OBB asset payload or produce a playable `.nro`.
