# Horizon Context native dependency objects — integration handoff

New source/unity_dex_horizon_context_objects.[ch] owns a strictly native
snapshot of actual filesystem paths and Horizon display/language readings,
for the next original GameActivity.onCreate dependencies:

- getFilesDir, getObbDir and getExternalFilesDirs(null) can be backed by
  independently verified existing folders. Each typed native File owner
  supplies getAbsolutePath only while its directory still exists and the
  exact native Activity generation/main-thread owner remains live.
- ApplicationInfo.nativeLibraryDir is the actual original Quest staged
  ARM64 ELF directory verified by unity_dex_quest_package_host, rather
  than a guessed Java String.
- A native Configuration snapshot supplies real NWindow pixel dimensions
  and the Horizon Settings BCP-47 system language (setGetSystemLanguage).
  This does not infer Android dp units, screen density, orientation or
  other Java Resources.Configuration fields.
- An optional delivered Intent action/component record can be read only
  after a real external launch source verifies the ACTION_MAIN, exact
  package/component and LAUNCHER/VR categories on every access. libnx does
  not expose such an Android Intent source in the current loader, so the
  production verifier stays NULL and the Intent record remains unavailable.

The runtime uses the existing verified native Activity base object and
libnx applet main thread/NWindow observation. All APIs return unavailable
if that native owner dies, the configuration cannot be read, a required
directory disappears, or data validation fails.

## Production integration (main.c owner)

After the previously implemented package host and native Activity services
have been initialized and the original Quest native directories exist:

    BsDexHorizonContextProduction source = {
        .native_activity = &java_runtime,
        .horizon = &observed_native_services
    };
    BsDexHorizonContextNative hooks =
        bs_dex_horizon_context_production_callbacks(&source);
    BsDexHorizonContextObjects objects = {0};
    int ready = bs_dex_horizon_context_objects_init(
        &objects, &package_host, &hooks,
        GAME_HOME "/files", GAME_HOME "/obb",
        GAME_HOME "/external-files");

    /* When ready, native value accessors return truthful OS observations. */
    const BsDexHorizonContextFile *files =
        bs_dex_horizon_context_file(&objects, BS_HCTX_FILES_DIR);
    const char *native_path = NULL;
    int observed = files &&
        bs_dex_horizon_context_get_absolute_path(files, &native_path);

    /* Release after native file/config consumers have stopped. */
    bs_dex_horizon_context_objects_release(&objects);

Native object lifetimes and applet hook ownership remain outside this data
service. The runtime does not create, wrap or publish Java Context,
java.io.File, Intent, ApplicationInfo, Resources, AssetManager or Looper
jobjects. The separately implemented original DEX VM, Android Context
host and Activity.attach verifier must still construct/verify those Java
objects before the original Java method call can be declared executed.
bs_dex_horizon_context_java_attachment_available always returns zero.

## Verified limits

Host tests use the original pinned APK, physically existing temporary
directories and ELF-header fixtures. They verify native File path
revalidation after a directory is moved, dynamic screen/language updates,
main-thread/Activity invalidation, and strict rejection of an Intent without
a witnessed launch. The injected launcher callback is a host-test fixture;
there is currently no production native Android launch observation.

Both new modules compile with devkitA64 GCC for Horizon. The native fixture
tests do not establish guest Java onCreate completion, Java class metadata,
Horizon runtime execution, OpenXR or Vulkan graphics initialization.
