# Original Quest ContextImpl / Activity.attach: verified native services

`source/unity_dex_android_framework_context.[ch]` provides an independently
rechecked native Android Context data layer for the original Quest Beat Saber
1.46.0 APK. It composes existing original-APK manifest/package, typed metadata,
native directory, AAssetManager, NWindow, observed Horizon system language and
native ALooper services. This component does not construct Java objects or
execute `Activity.attach` and does not grant Java/onCreate/graphics readiness.

The original `UnityPlayerGameActivity` constructor was observed returning after
2287 real DEX instructions in live Eden; production `Activity.attach` still
returns 0. Original `GameActivity.onCreate` code item `0x22EDAC` requires, in
order, `getPackageManager` (PC `0x18`), `getIntent` (`0x1C`),
`Intent.getComponent` (`0x20`), `PackageManager.getActivityInfo` (`0x26`),
activity-scoped metadata Bundle (`0x32`), `getClassLoader` (`0x62`),
`getFilesDir` (`0xCB`), `getObbDir` (`0xD3`), `getAssets` (`0xDF`),
`getResources` (`0xE3`), `Resources.getConfiguration` (`0xE7`) and
`initializeNativeCode` (`0xEC`). The original subclass first constructs its
`SurfaceView`/UnityPlayer route. The DEX PC addresses are 16-bit instruction
units from the original APK and do not imply execution has reached those PCs.

## Supported operations

- `bs_dex_android_framework_context_init` establishes a borrowed, generation
  checked relationship between the native attachment and real asset manager
  owners. It requires the verified original Activity base, original native
  files and live main ALooper, all witnessed by the existing services.
- `bs_dex_android_framework_context_observe` rereads the typed original Quest
  `ApplicationInfo`/`ActivityInfo` C representations, native file/OBB/external
  paths, current NWindow/NDK AssetManager/native ALooper, owner thread and
  actual framebuffer pixels/Horizon Settings language. It rechecks native
  identity and rejects a revoked owner, replaced asset manager or mutated
  source metadata. These values are **not** JNI `jobject` or Android Java
  `Configuration` fields. Unavailable density/dp/orientation remain unknown.
- `bs_dex_android_framework_context_metadata` returns an exact typed manifest
  entry from its specified application versus Activity scope, without coercion.
- `bs_dex_android_framework_context_open_asset` forwards to the real staged
  `AAssetManager` service and its bounded stream/path checks. The regression
  opens original `assets/bin/Data/boot.config` (768 bytes, CRC32 `0x396A5204`)
  from the genuine Quest APK and confirms exact leading bytes.
- `bs_dex_android_framework_context_launch` requires the existing independently
  witnessed delivered-launch receipt, and asks its native source to reverify it
  on every read. The Quest manifest's advertised `ACTION_MAIN` intent filter
  cannot supply this receipt. Current libnx production launch callbacks lack
  an Android Intent source, so this method returns 0 by default.

## Java execution witness contract

`BsDexFrameworkJavaAttachment` identifies candidate real guest Java objects for
the original Activity, `ContextImpl`, `Window`, `Handler`, `Looper`,
`ClassLoader`, file objects, typed Android package metadata and Intent.
`bs_dex_android_framework_context_verify_java` checks that every required
callback exists, a native delivered launch is independently witnessed, and
the original `BsDexQuestJavaWitness` independently verifies full package/
activity metadata, Java classes, `Intent` component and actual PackageManager
results. It queries the executable resource registry's `Resources`,
`AssetManager` and `Configuration` objects, verifies the NDK asset-manager link
and native-observed configuration, validates Java file paths, checks the actual
delivered Intent receipt, and demands an independent live Activity attachment
readback associating `ContextImpl`, Java `Window`, Handler/Looper and native
window/thread ownership.

The Java executor must **execute** and inspect these real Java getter calls:
`Activity.getIntent/getWindow`, `ContextWrapper.getBaseContext`,
`Context.getClassLoader/getMainLooper`, `Handler.getLooper`,
`Context.getPackageManager/getApplicationInfo/getFilesDir/getObbDir/
getAssets/getResources`, `Resources.getAssets/getConfiguration`, and
`Intent.getComponent`. All returned Java references must match the independently
witnessed retained Java owners. Every rejected callback clears the candidate
output. A successful readback is only evidence that the external Java executor
already completed the side effects; the Context bridge itself does not execute
`Activity.attach`, publish `UnityPlayer.currentActivity`, or advance the DEX.

The current production loader has no executable Java `ContextImpl`/`Window`,
delivered Intent source, JVM object/verifier for all getter methods, or proven
main Java Looper/Handler correspondence. The observed original DEX Handler
constructor count does not itself establish the attached Java Looper identity.
To integrate, prime must implement a guest Java framework constructor/service
registry that retains these objects; provide the independent native launch
source, real `Activity.attach` and teardown callbacks; bind verified Java
Resources/Configuration and Handler/Looper; and wire the resulting witness to
`BsDexHorizonAttachmentJava.execute_attach`, `verify_attachment` and
`verify_type`. The existing original Java VM remains blocked until all of
those checks pass. An adapter that merely fills these C fields is insufficient.

## Validation

Run `py -3 tests/test_unity_dex_android_framework_context.py`. Its fixture
loads the pinned original `classes.dex`, runs actual original constructor
instructions through the Activity native base, constructs the original native
package/files/window/ALooper/asset owners, then verifies package identity,
scoped metadata, real asset reads and path guards, revocation and generation
checks, missing/wrong/verified launch receipts, and Java getter failures.
The optional host Java witness is explicitly a test double; it proves the
contract accepts a fully witnessed graph and rejects stale attachment,
incorrect Intent, missing getter dispatch and file-path mismatches, but is
not evidence of a functioning Java framework on Horizon.

The new production C file also compiles with strict host Clang
`-std=c11 -Wall -Wextra -Werror -pedantic -fsyntax-only` and devkitA64
`aarch64-none-elf-gcc -std=c11 -Wall -Wextra -Werror -c` without changes to
existing source files or project build scripts. Prime can add this source to
the NRO build when an executable Java attach backend is available.
