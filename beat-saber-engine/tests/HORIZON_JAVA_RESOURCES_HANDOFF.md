# Original Quest Resources / AssetManager / Configuration: backed subset

The pinned original Quest APK classes.dex imports
`Landroid/content/res/Resources;` methods `getAssets()`,
`getConfiguration()`, `getBoolean(int)`, `getString(int)`,
`getInteger(int)`, `getIdentifier(...)`, `getDisplayMetrics()`,
`getDrawable(...)`, `openRawResource(...)` and others. It imports
`Landroid/content/res/AssetManager;.open(String)` and `openFd(String)`;
it also imports `Configuration.getLocales()`, `getLayoutDirection()`,
`diff()`, `equals()`, `updateFrom()` and fields including
`orientation`, `densityDpi`, `screenWidthDp`,
`screenHeightDp`, `fontScale` and `uiMode`.

In the original `GameActivity.onCreate` (code 0x22EDAC), calls at
PC 0xDF/0xE3/0xE7 request `Activity.getAssets()`,
`Activity.getResources()` and `Resources.getConfiguration()`
before `initializeNativeCode`. Those framework operations require real
Java class instances, not a parsed resources.arsc CRC.

## Native subset implemented

`source/unity_dex_horizon_java_resources.[ch]` now provides a strict
production native stream backend. It starts from the pinned original
`BsDexQuestPackageHost`, live `BsDexHorizonContextObjects`, observed
`BsDexHorizonAndroidAttachment`, actual NDK AAssetManager and original
Horizon applet main-thread/NWindow lifetime. Production callbacks use
existing `bs_android_assetmanager_open/read/seek64/length64/close`.

`bs_dex_horizon_java_resources_open` opens actual files under the staged
original APK's `assets/` tree, rejects absolute/traversal/ambiguous
paths and holds one native AAsset until explicit close. Read and seek use
real native file offsets and return clean EOF separately from errors.
Each stream has a generation and serial, preventing stale stream handles
from acquiring a reused slot. At most 16 streams may remain open at once;
128 MiB is the maximum admitted asset length. Shutdown closes all
remaining owned streams, even after native readiness has been revoked.

The Configuration source reads *current measured* NWindow pixels and
the Horizon Settings system language, and preserves its observation
number. These are native observations. The provider does not invent
Android dp dimensions, density, fontScale, orientation, input device,
navigation mode, or UI mode. Framebuffer aspect ratio is insufficient
to establish Android `Configuration.orientation`.

The real Quest APK asset `assets/bin/Data/boot.config` has 768 decoded
bytes and CRC32 `0x396A5204`. The host test extracts those exact bytes
from the original Quest APK (ZIP method 8) into the staged-assets fixture,
then opens/reads/seeks/closes them through the native provider. It checks
the exact first line `gfx-enable-gfx-jobs=1`, EOF, stream slot exhaustion,
reused-slot invalidation, native manager revocation, package mutation and
cleanup.

## Production APIs

```c
BsDexHorizonJavaResources resources = {0};
BsDexHorizonJavaResourcesNative native =
    bs_dex_horizon_java_resources_production_native(&attachment);

/* Only after original Activity native base, package Context observations,
 * native ALooper and native Attachment graph are available. */
int native_ready = bs_dex_horizon_java_resources_init(
    &resources, &attachment, &native_context, &quest_package, &native);

BsDexHorizonObservedConfiguration observation = {0};
int configuration_read = bs_dex_horizon_java_resources_configuration(
    &resources, &observation);

BsDexHorizonAssetStream stream = {0};
uint64_t total_bytes = 0;
int opened = bs_dex_horizon_java_resources_open(
    &resources, "bin/Data/boot.config", &stream, &total_bytes);
if (opened) {
    char buffer[64];
    size_t count = 0;
    int read_ok = bs_dex_horizon_java_resources_read(
        stream, buffer, sizeof buffer, &count);
    (void)read_ok;
    bs_dex_horizon_java_resources_close(stream);
}

bs_dex_horizon_java_resources_shutdown(&resources);
```

Native asset operations above are real, independently verifiable I/O.
They do **not** represent the Java `AssetManager.open` method or its
`InputStream` class. Actual Java open/read/close method dispatch must
still be supplied by the original DEX VM with a verified Java stream
class instance and Java exception semantics.

## Java registry contract

`BsDexHorizonJavaResourceRegistry` allows a future executable guest
runtime to supply `build`, `verify_class`, `verify_link`, and
`destroy` callbacks. Successful `build` must execute real class
constructors/initialization and strongly retain `Resources`,
`AssetManager`, `Configuration` and their shared owner. A callback
returning arbitrary pointer-shaped tokens is insufficient.

The adapter checks the exact three Android DEX class descriptors,
retained strong owner, real getter links
`Resources.getAssets()/getConfiguration()`, native backing through
actual `AAssetManager_fromJava`, and truthful Configuration values.
Every Java getter rechecks native readiness, Java classes and links.
Failed or partial Java construction invokes `destroy` before returning
0, with no Java handle exposed. The host test uses an explicitly
simulated registry to exercise rollback and type gating. It does not
establish guest Java readiness on Switch.

The existing `jni_bridge.h` can create observed Configuration field/
locale objects, but it currently has no integrated public registry
constructor for an executable Java `Resources` and `AssetManager`
pair and no `BsDexVM` framework-method dispatcher for this provider.
The original `BsDexAndroidContextAttachment` also requires an
independently witnessed real `Activity.attach` and actual Java getters,
not native C objects. Consequently the Java getter functions return
0 in current production unless a conforming registry is connected.
`Resources.getString/getBoolean` also return 0: verification of the
258492-byte `resources.arsc` with CRC32 `0x0B0977D4` identifies
the table but does not implement locale/density/configuration-aware
resource resolution. Android Resources constructors, DisplayMetrics,
raw-resource InputStreams, drawables and themes remain unimplemented.

## Shutdown and next integration

The native provider must shut down while original Activity base, native
ALooper, VM and native Context still live; close all streams and release
any real Java registry owner first. Then release native main ALooper
**before** releasing the Activity base (ALooper shutdown itself requires
that native Activity's owner to remain live). In a future real attached
Java runtime, perform an independent Java Activity detach phase before
resource and ALooper shutdown, then destroy the VM and release native
Activity, attachment graph, native Context and package.

No source/main.c, DEX VM, JNI bridge, previous attachment modules or
original Quest binaries were modified by this work. Host tests confirm
native operations and callback-contract behavior only; no Eden run or
original Java onCreate/graphics completion is claimed.
