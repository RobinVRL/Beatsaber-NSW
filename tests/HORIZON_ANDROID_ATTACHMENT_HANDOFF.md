# Quest Activity.attach — native object graph and exact Java boundary

New `source/unity_dex_horizon_android_attachment.[ch]` owns a typed native
object graph for the verified Quest APK and Horizon applet. The graph is
available **after the original DEX has executed its native Activity base
constructor**, but does not mean the Android Java Activity is attached.
No bytecode, JNI bridge, original Quest binaries, or existing hosts were
modified for this implementation.

## Concrete native objects retained

The graph independently verifies and retains copies of:

- Exact original Quest package/application and ActivityInfo public values,
  source APK paths, version and launch filter. The filter records advertised
  ACTION_MAIN/LAUNCHER/VR acceptance; it is not a delivered Intent.
- All 15 application-scoped and one Activity-scoped manifest metadata entries
  as typed `BsApkManifestValue` values (boolean/integer/string/resource ID).
  Their ownership is independent of the source package model's memory.
- Real existing Files, OBB and external-files directories; the verified
  ARM64 native library directory. Every access checks the original native
  Context owner and re-stats the directory through its existing APIs.
- An actual current `NWindow` and independently probed native
  `AAssetManager*` that can read staged original `bin/Data/boot.config`.
  The native main ALooper is independently acquired/owned elsewhere.
- Real NWindow pixel dimensions and the observed Horizon Settings language,
  plus the pinned original `resources.arsc` 258492-byte/CRC identity.

Every native object is exposed as a `BsDexHorizonAttachmentRef` consisting
of its kind and owner generation. A changed applet thread, native window,
asset manager, directory, Activity generation, package manifest or native
ALooper invalidates readback. `BsDexHorizonAttachmentSnapshot` owns the
actual C metadata/path copies until release. These C objects are not JNI
objects, Java class instances, or `android.content.res.Resources`.

A delivered launch record is available only through
`bs_dex_horizon_context_read_launch`, which verifies an independent live
native launch source each time. libnx has no Android Intent source in the
current port, so production normally has **no** launch receipt.

## Production installation (after original DEX constructor)

```c
BsDexHorizonAttachmentNative native_attach =
    bs_dex_horizon_android_attachment_production_native(&native_services);

/* Requires a live verified original DEX Activity constructor, package,
 * native Context files/config and observed main ALooper. */
int ready = bs_dex_horizon_android_attachment_prepare(
    &attachment, &quest_package, &native_context,
    &java_runtime, &main_looper, &native_attach);

/* The Activity host must already have been initialized with
 * bs_dex_horizon_java_runtime_callbacks and bound to java_runtime.
 * Call after bs_dex_horizon_java_runtime_bind_verified_host, never before.
 * This swaps only in-memory callback pointers and preserves the real
 * native Activity base ctor, owner-thread and appletHook lifetime. */
int installed = ready &&
    bs_dex_horizon_android_attachment_install(&attachment, &horizon_host);
```

The current production `main.c` already performs this native installation
after its original constructor probe. Native attachment can therefore be
observed and inspected without claiming any Unity Java onCreate success.

## Required Java backend to actually attach

`bs_dex_horizon_android_attachment_set_java` accepts a complete external
`BsDexHorizonAttachmentJava` only when all of its callbacks and all
`BsDexQuestJavaWitness` functions are present. An actual implementation
must provide, **with independently executable and verified side effects**:

1. A live delivered Android launch Intent with the exact original
   ComponentName and categories, witnessed from the Activity's launch
   rather than inferred from the manifest filter.
2. The actual Java main Looper and Handler, associated with the verified
   native main owner. Native ALooper existence is insufficient.
3. Actual Java ContextImpl/Activity.attach execution and a Java Window
   linked to the live native NWindow; java.lang.String and java.io.File
   objects backed by verified package paths; live Java Intent,
   ComponentName, PackageManager, ActivityInfo, ApplicationInfo, and
   both typed metadata Bundles with exact source values.
4. A real Java Resources/Configuration implementation and Java
   AssetManager whose NDK backing is the observed original APK assets.
   Native pixel dimensions, system language and a resources.arsc CRC
   cannot satisfy this class/operation by themselves.
5. An independently checked Java `getActivityInfo(..., GET_META_DATA)`,
   `getIntent()`, `getApplicationInfo()`, `getResources()`,
   `getAssets()`, directory getters and attachment readback, plus
   actual Java object class assignability and retained lifetimes.
6. A real reversible Java detachment that releases all Java references,
   window registration and associated resources. The callback must return
   success only after side effects are fully undone.

The adapter refuses attach before every required native and Java witness is
present. When a Java backend returns a candidate, the adapter checks
both metadata scopes and all package owners via
`bs_dex_quest_package_stage_context`, verifies real Java classes and
independently rereads the attachment. It atomically publishes the
`BsDexAndroidContextAttachment` only after those checks succeed. A
failed verification detaches the real Java side effects before returning
0; a failed rollback retains the token for later retry, without publishing
readiness. Subsequent ActivityHost/context validation repeats class and
attachment verification.

### Teardown ordering

Stop the main event pump, then release guest VM references, detach the real
Java Activity through `bs_dex_horizon_activity_release`, and only then call
`bs_dex_horizon_android_attachment_release`. Shut down the main ALooper
and native Context data **after** Java detachment (since a future Java
detach callback may require them), then shut down the native Java Activity
base runtime. The current main.c cleanup presently closes its main ALooper
and native Context data earlier, which is sufficient for the present
native-only/unattached flow; it will need reordering before attaching actual
Java service objects. This module does not edit main.c.

## Verification and current stop

`python tests/test_unity_dex_horizon_android_attachment.py` runs the
actual pinned Quest APK DEX constructor and original manifest model, then
tests native data generation, source identity, path/config/window/asset
invalidation, missing launch, missing Handler/Looper, denied Java Resources
class, failed Java attachment rollback, independent Context readback,
teardown and stale-handle refusal.

The host test includes **explicit simulated Java callbacks** to exercise
the complete future contract. Those callbacks are fixtures; their
success does not establish any Android JVM or Java Activity.attach in
production. Production has no verified launch Intent source or complete
Java Resources/Handler/Looper implementation. Consequently production
attachment correctly returns 0 and original onCreate is still blocked;
no guest graphics readiness is claimed.

Even after a future real attach, executable Android/Java methods such as
`Context.getPackageManager`, `PackageManager.getActivityInfo`,
`Intent.getComponent/getStringExtra/putExtra`, `Bundle.getBoolean/getString`,
`Activity.findViewById`, `ClassLoader`, and actual
SurfaceView/ViewTreeObserver callbacks must still be provided by their
respective VM/Context/View hosts. Native typed metadata and callback
contracts cannot count as execution of those original framework methods.
