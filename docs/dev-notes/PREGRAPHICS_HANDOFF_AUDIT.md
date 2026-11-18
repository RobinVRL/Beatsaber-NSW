# Original Quest Unity native startup to graphics handoff

This audit covers the owner-staged Beat Saber Quest 1.46.0 ARM64 binaries,
Unity 6000.3.19f1, rather than the unrelated Windows Mono executable.

## Original source-backed entrypoints

Fifteen original Android libraries are mapped and relocated, including 15
original AArch64 TLSDESC relocations. Actual original ELF dynamic symbols
include seven JNI_OnLoad entries in libmain, libil2cpp, libunity, libOVRPlugin,
libovrplatformloader, libUnityOpenXR and libgame. Unity's export is GNU
versioned as JNI_OnLoad@@libunity and is 196 bytes long. Tests compare this
exact set with the implemented JNI startup order.

The pinned original GameActivity call chain is JNI_OnLoad ->
GameActivity_register -> initializeNativeCode -> NativeCode ->
GameActivity_onCreate -> original worker/android_main -> Unity MainLoop.
Previous Eden logs confirm original libmain and libil2cpp JNI_OnLoad and
registration-only libgame JNI_OnLoad, but do not establish engine startup.

## Stop before graphics

The NVK pregraphics probe queries real Vulkan extension metadata, native
instance entrypoints and the current libnx NWindow. It checks the real
VK_KHR_surface and VK_NN_vi_surface extensions without creating a Vulkan
instance, surface, swapchain, graphics queue or draw command.

If all original platform readiness gates eventually pass, the loader arms
a one-shot guard before original constructors. The FIRST guest call to
vkCreateInstance or vkCreateAndroidSurfaceKHR is recorded and rejected
BEFORE entering the NVK graphics driver. The main loop then stops.
Function-pointer lookup and a successful extension query do not count as
reaching the graphics entrypoint.

The startup gate uses the read-only Vulkan preflight. Requiring an existing
surface would make this stop point unreachable. When a constructor or
JNI_OnLoad returns after hitting the guard, the loader skips the rest of the
bootstrap calls.

The original boot.config requires UnityOpenXR preinitialization, a Vulkan
offscreen swapchain and fragment-density-map support. The NVK probe does
not demonstrate those OpenXR facilities.

## Current blocking dependencies

- The JNI host registers all 24 original GameActivity native callbacks, and
  the latest Eden trace reports the Java host's own prerequisite checks as
  complete. Original NativeCode/android_main execution is still unobserved;
  Java host prerequisite checks do not by themselves prove guest lifecycle.
- The November 16 Eden run now reports **zero unresolved strong relocations**
  across all 15 mapped original libraries, down from 102 at the beginning
  of this pass. Backed CPU AImage objects resolve the last strong MediaNDK
  image import and tracked native directory descriptors allow fdopendir to
  transfer ownership to a real DIR*. This reports symbol binding, not proof
  of complete platform ABI semantics for every guest call path.
- Original MediaNDK key symbols, metadata, custom AMediaDataSource,
  FFmpeg-backed AMediaExtractor and CPU-buffer AMediaCodec decoding are
  implemented. The November 16 Eden trace reports a successful live G.711
  WAVE extract/decode/output-buffer self-test. Hardware-backed output
  surfaces and unsupported codecs still need their own runtime validation.
- The original libUnityOpenXR JNI_OnLoad needs the live
  UnityPlayer.currentActivity static field and JNI slots 144/145, not just
  GameActivity symbolic class handles. Slots 144/145 are implemented with
  strict original DEX-verified field signatures, but the static field has not
  been assigned by an executed Java constructor. The actual Quest DEX path is
  UnityPlayerGameActivity.onCreate -> GameActivity.onCreate -> overridden
  onCreateSurfaceView -> UnityPlayerForGameActivity constructor ->
  UnityPlayer.<init>, which writes currentContext unconditionally before its
  Activity instanceof check conditionally assigns currentActivity. Both
  original fields now have separate, strictly backed observed-value adapters;
  the currentActivity adapter rejects writes before its Context observation,
  and the actual Activity-path subclass UnityPlayerForGameActivity inherits
  their DEX-verified JNI field IDs. Neither adapter executes Java bytecode.
  GameActivity.initializeNativeCode is later in the original Java onCreate.
  The current native C driver does not execute that Java construction path,
  so it must NOT claim assignment merely from a bound GameActivity receiver.
  No usable vendor OpenXR runtime, active_runtime(.aarch64).json, or runtime
  negotiation entrypoint exists in the staged APK/SD set. These are separate
  blockers even after all native imports are linked.
- No original Unity Vulkan instance/surface or working Quest-compatible
  OpenXR session and six-degree-of-freedom tracking runtime exists.
- The original Unity launch retains strong-import, Bionic, GameActivity,
  MediaNDK and Vulkan entrypoint readiness gates. All imports and original
  native constructors now complete. The former second FindClass(NULL) in
  original libunity JNI_OnLoad has been fixed by adding owned-mapping
  MADV_NORMAL semantics to the Bionic VM bridge. The NEXT live failure is
  original libUnityOpenXR JNI_OnLoad reading the not-yet-assigned Java static
  currentActivity field. That lookup creates a pending
  UnsupportedOperationException, now explicitly rejected after JNI_OnLoad.
- Offline network policy remains in force.

### Original APK Java-bytecode staging (November 16 follow-up)

The previously reported absence of standalone extracted `classes.dex` files
does **not** mean the game's Java bytecode is missing. The unmodified original
`SwitchPort/beat-saber.apk` has a ZIP **stored** `classes.dex` entry (6,315,268
bytes, ZIP CRC32 `0x43f54de0`). The new bounded
`source/unity_apk_dex_loader.[ch]` reads that specific entry directly from the
APK, rejects encrypted/duplicate/mismatched entries and checks the entire DEX
checksum, header identity and original constructor opcode anchors. It does not
execute classes or change JNI state. A host test against the actual APK passes.

A SHA-256-identical COPY of that original APK was staged at
`sdmc:/switch/beat-saber-quest/beat-saber.apk` in Eden, without editing the
imported original APK or ELF binaries. The rebuilt NRO's **live** Eden log now
contains `[java:dex] original APK classes.dex verified (bytes=6315268; Java
VM/Activity execution NOT established)`. Native bootstrap still reaches and
fails at original `libUnityOpenXR.so` `GetStaticObjectField` for unassigned
`UnityPlayer.currentActivity`. Static bytecode verification is NOT a stand-in
for executing UnityPlayerGameActivity.onCreate and its nested Java constructors.

The offline `tools/openxr_runtime_discovery.py` utility additionally performs
fail-closed OpenXR manifest and ARM64 vendor-entrypoint checks. The November 16
scan of the project found **no** `active_runtime.json` or
`active_runtime.aarch64.json` vendor runtime; the report explicitly leaves
`pregraphics_runtime_ready=false` (expected command exit 2). The supplied
`libopenxr_loader.so` is a **loader**, not the missing vendor implementation:
it does not expose the runtime negotiation entrypoint. Eleven independent
discovery tests passed. A manifest or ELF export alone would not establish
successful runtime negotiation, actual XR display or tracking services.

### Executable DEX VM and Android service adapters (final November 16 pass)

The original APK's full `classes.dex` is now parsed by a bounded executable
Dalvik interpreter in `source/unity_dex_vm.[ch]`. It decodes original class,
method, field and prototype tables; executes genuine 16-bit DEX instructions
with method dispatch and stack frames; and stops at an exact original code
offset/PC when a platform method or opcode cannot be performed. It does not
substitute synthetic constructor completion. Its
`source/unity_dex_preinit_executor.[ch]` connector to the existing strict
`bs_unity_java_preinit_run` verifies the original currentActivity write and
reads actual VM-owned field state before accepting the checkpoint. It cannot
publish static fields to JNI or mark Unity ready by itself.

In host tests against the **original** DEX, interpreter-only execution reaches
the external `SurfaceView.<init>(Context)` at code offset `0x22e9d8`, PC `0x2`
after nine actual DEX opcodes through seven real method frames. Strictly
test-backed SurfaceView and EditorInfo services extend execution to 45 original
opcodes, then encounter missing `BaseInputConnection.<init>` at offset
`0x230330`, PC `0xA`. Those tests use C fixtures, **not** actual Android
framework objects or a running Switch Java VM.

The new Android Context/Intent/ApplicationInfo/Bundle host, SurfaceView/
SurfaceHolder host and composed dispatcher provide narrowly backed method
adapters. They verify actual host receiver class/identity, thread and JNI
surface binding before returning success, and reject missing or mismatched
services. The original concrete Quest `UnityPlayerGameActivity$GameActivitySurfaceView`
subclass is recognized; mismatched surfaces are revoked and rolled back.
Neither the original Activity nor the UnityPlayerForGameActivity Java object
has yet been constructed on Switch, and the necessary Android input, window,
Thread/Handler, Resources and OpenXR runtime services remain missing.

The real NRO now constructs and destroys the actual verified DEX interpreter
at startup, exercising original table parsing in the **live Eden guest**.
Its log confirms `[java:vm] original DEX tables parsed=1, real Java
Activity=0, executed instructions=0; preinit checkpoint BLOCKED`. The parser
readiness bit is a necessary additional GameActivity bootstrap gate, never a
replacement for observed Java construction. This latest Eden run again
reached original libUnityOpenXR JNI_OnLoad and rejected unassigned
`UnityPlayer.currentActivity`; the original Unity Vulkan creation boundary
remains unobserved. The NVK NRO build and all 24 Python-led original ELF/JNI,
DEX, Android bridge and OpenXR verifier tests passed on November 16.

### Observed SurfaceView binding

The SurfaceView adapter now requires an independently class-verified Java
view before completing the original DEX subclass's base constructor. It
binds that object to the original DEX reference only after verifying the
native window and JNI Surface round trip. Rejection revokes the surface and
detaches the view. A failed detach retains the backend ownership needed for
cleanup and prevents a constructor retry from attaching a second view.

The original-DEX host regression checks the binding and exercises a rejected
class with a failed detach. The NVK Lifecycle NRO rebuilt and a fresh Eden
run stopped in original libUnityOpenXR JNI_OnLoad at the unassigned
UnityPlayer.currentActivity. The Java host readiness output refers to the
bound native proxy. The guest still has no real DEX Activity instance or
production platform callback for constructing the Java view. The workspace
and Eden SD asset scan found no OpenXR runtime manifest. The graphics boundary
remains unobserved.

### Original Activity constructor in Eden

The NRO now starts the original no-argument UnityPlayerGameActivity DEX
constructor and its superclass class initializers. It waits until the
original libgame ELF constructors and verified JNI registration have
completed before running this probe. The exact System.loadLibrary("game")
request then resolves to that already initialized library.

In a fresh portable Eden session using the private SD, the original DEX
executed nine instructions and stopped at android.app.Activity.<init>
(code item 0x1a5550, PC 0x0). This platform constructor has no service
implementation in the Switch host. The guest subsequently reached the
original libUnityOpenXR JNI_OnLoad and stopped at currentActivity. The
Activity constructor did not complete, and neither onCreate nor the original
Vulkan entry ran. The original DEX host regression verifies
both the initial unresolved library call and continuation to the exact
platform Activity constructor.

### Resumable original library startup

The bootstrap now records completed ELF constructor and JNI_OnLoad phases.
The JNI startup order accounts for the earlier verified libgame registration,
and only the original UnityOpenXR hook waits for an observed, readable
UnityPlayer.currentActivity. The readiness query creates no JNI reference or
pending exception. The applet can resume on its attached guest thread when
the original Java observation arrives, without rerunning earlier hooks.
An actual failed JNI_OnLoad remains terminal.

Fresh Eden verification completed 15/15 ELF constructor phases with zero
unresolved relocations and 6/7 original JNI hooks. The guest deferred
libUnityOpenXR JNI_OnLoad with waiting_for_activity=1. The original DEX
still stops at the unimplemented platform Activity constructor, so there
is no live Activity observation to resume with. Host tests exercised the
resume path and the graphics stop guard.

### DEX Activity receiver handoff

The interpreter can carry the same DEX-owned Activity reference and fields
from the original constructor into onCreate. It checks that the constructor
has returned, that the receiver is bound to the exact supplied platform
object, and that the host verifies the Activity attachment. The preexisting
host-constructed Activity entry remains available.

In the original-DEX host regression, a fixture backs the Android base
Activity constructor. The interpreter advances from 9 to 35 original
instructions and stops at AtomicReference.<init> (code item 0x218994,
PC 0x3). The fixture also verifies that binding the base object alone cannot
start onCreate while the outer constructor is still blocked. The Switch
guest still needs a production Activity constructor and attachment service.

### VM-owned AtomicReference support

The interpreter now allocates and initializes VM-owned AtomicReference
objects. It implements both constructor signatures and the original DEX's
get, set and getAndSet calls with VM reference values. Existing host-backed
objects still use the host dispatcher. Allocation stops identify the actual
new-instance opcode separately from a constructor invoke.

With the same host-only Activity constructor fixture, the original DEX
advances from 35 to 43 instructions and stops at WeakHashMap allocation
(code item 0x12d174, PC 0x3). This test still cannot supply an Activity
constructor or attachment service to the Switch guest.

### Platform Activity constructor adapter

The live DEX probe now uses the composed Android dispatcher. The dispatcher
has a typed backend entry for the original Activity base constructor and
binds its result to the original UnityPlayerGameActivity DEX receiver only
after validating the concrete class. A rejected result is discarded and
the instruction can retry. Dispatching onCreate still requires the original
outer constructor to return and the host to verify attachment.

The host regression runs the original DEX through a rejected constructor
result and a successful retry with a fixture. The fixture reaches the
WeakHashMap allocation at 43 instructions. The Switch build has no production
constructor backend to register, so the seventh JNI hook remains gated.

### Verified DEX-to-JNI handoff

The original Activity constructor probe now runs after ELF constructors
and the six independent JNI hooks, immediately before UnityOpenXR. If the
platform supplies an Activity and the original constructor returns, the
same VM starts onCreate. The loader uses the shared original-bytecode
readback to verify the UnityPlayer checkpoint before publishing Context
and Activity to JNI. Successful publication retains the VM and its DEX
until teardown. An incomplete constructor or absent checkpoint leaves
the UnityOpenXR hook gated.

The NVK NRO rebuilt and the local original DEX host and graphics boundary
regressions passed. The build still lacks a production Activity backend.

### Java collection construction

The DEX VM supports construction of its own WeakHashMap and HashMap objects.
WeakHashMap allocates its initial table and HashMap keeps its lazy initial
capacity. Map operations that require Java key dispatch or weak-reference
queue processing remain unimplemented.

With the test-only base Activity constructor supplied, the original DEX
advances from 43 to 58 instructions and reaches ArrayList allocation in
code item 0x2183b0 at PC 0x11. The real Switch host still requires Activity
construction and attachment services before it can execute this prefix.

## Validation limits

### November 16 afternoon: real DEX instruction and Activity attachment progress

The Android Activity bridge now enforces original DEX constructor completion,
an independently verified platform `Activity.attach` and atomic Context/
Intent/Resources/AssetManager readback **before** executing the original
`onCreate`. It rejects superclass-only identity, missing UI thread and absent
attachment evidence. The NRO invokes this contract in its real preinit path,
but currently registers no production Activity constructor or attachment
backend; therefore the live Quest Java Activity remains blocked at
`Landroid/app/Activity;.<init>`, original DEX code `0x1a5550`, PC zero.

The interpreter added VM-owned ArrayList and Enum operations, original
`filled-new-array` opcode decoding, and host-managed weak Java references to
externally backed objects with explicit create/get/clear/release lifetime
callbacks. The external weak-reference host test invalidates a live Activity
and confirms a later `WeakReference.get()` returns null; it does not confer
Android Activity readiness. With test-only Activity and weak-reference host
fixtures, original Quest constructor execution passed **239 actual DEX
instructions** and stopped at `java.util.ArrayDeque` allocation, code item
`0x0ecd6c`, PC `0x3`. CopyOnWriteArraySet and CopyOnWriteArrayList are now
backed for the observed bounded operations, and unsupported Java equality
behavior remains explicitly rejected. At that earlier stage, real Switch
guest execution was still limited to nine original DEX instructions.

The opt-in `unity_dex_horizon_activity_host` adapter checks the exact seven
class_def superclass links in the pinned original DEX, maintains a native
Activity generation and validates attached NWindow and Context ownership.
Its tests cover malformed APK DEX, missing platform services, attachment
rollback and token invalidation. It does not provide a concrete production
Android Java/Looper/Intent/Resource or SurfaceView implementation, and the
loader does not yet configure those services.

The Eden smoke-test driver allows a 180-second staging window and recovers
from replaced/truncated guest logs. The NVK Lifecycle NRO was staged under
the isolated private SD tree. Earlier verification was delayed by an already
running Eden GUI instance; that obstacle was resolved for subsequent runs.

### Production Horizon applet host and guarded original Java continuation

The NRO constructor probe now collects live libnx main-thread, NWindow,
JNI Surface, AssetManager and original staged-directory observations. The
`unity_dex_horizon_java_runtime` adapter registers a real `appletHook` for
native focus, operation, exit and resume events and unregisters it on teardown.
The original seven-class Activity ancestry is checked from the pinned DEX,
and a native Activity lifetime is accepted only after observed applet event
registration. Java's complete `Activity.<init>` field graph, `Activity.attach`,
Intent, Resources, Handler/Looper, FragmentController, Android Window and Java
view hierarchy remain unimplemented. Executed original `onCreate` and the
UnityPlayer.currentActivity JNI field remain explicitly gated.

The original DEX VM now also executes bounded Java ArrayDeque, AtomicInteger,
ArrayList, Random and Bundle operations, with weak-reference lifetimes checked
through the native host. The default Random constructor reads fresh monotonic
nanoseconds from Horizon `armGetSystemTick`; missing observations stop the
original bytecode rather than inventing a seed.

A fresh **live Eden run** reached **328 original DEX instructions** after
the `Activity.<init>` bridge was repaired. Initial runs failed at the real
native callback subscription despite all 15 libraries and prerequisite
native services passing. A pinned devkitA64 libnx `appletHook` disassembly
showed its `AppletHookCookie` saves the **previous** hook-list head; the NRO
incorrectly required that cookie to already contain the just-registered
callback. The corrected cookie ownership/unregistration performs an actual
`appletHook` and `appletUnhook` without that false check. Eden then reported
the Activity's native constructor gate 8, real event cookie, two managed weak
references and 328 executed Quest DEX instructions. The exact next interpreter
stop is Dalvik `const-class` opcode `0x1c` at code item `0x12c5ec`, PC `0x7`.
The original Vulkan creation boundary remains unobserved and the seventh JNI
hook still waits for the original `UnityPlayer.currentActivity` write.

The next NRO, rebuilt after adding canonical DEX-backed class literals and
VM-owned `java.lang.Object` construction, passed a fresh Eden run through
**355 original Quest DEX instructions**. The first unsupported original call
is `java.util.concurrent.Executors.newFixedThreadPool` at code item
`0x12c814`, PC `0x10` (invoke-static opcode `0x71`). Its original APK method
signature is the **two-argument** overload with an integer pool size of four
and a DEX-created `DefaultTaskExecutor$1` ThreadFactory. Constructing the
pool metadata is independent of dispatching guest Runnable tasks; an actual
thread scheduler, task queue, dispatch, shutdown and exceptions remain
required before any task execution can be reported successful. The live guest
again completed 15/15 original constructors, 6/7 JNI hooks and zero strong
relocations; it did not reach graphics initialization.

### November 16: live native Looper and original DEX interface resolution

The next production NRO integrates a Horizon applet-backed main event queue,
using the actual native `bs_looper_for_thread()` already created by the
GameActivity transport. It takes an independent ALooper reference, verifies
the same live `svcGetThreadId` owner/current IDs, and releases the reference
before destroying the VM or native Activity. The VM implements only canonical
main `Looper.getMainLooper`, `Looper.getThread` and
`Thread.currentThread` identity tokens when that native observation succeeds.
It does not claim that Java Handler callbacks or arbitrary `Runnable.run`
methods are executable. An independent native queue provides tested delayed
work ordering, cancellation and reference retention, but Java posting remains
blocked pending a verified original Runnable invocation interface.

Eden executed **370 original DEX instructions** and stopped at the original
`Looper.getMainLooper` entry before this bridge. The bridged NRO reached
**400 instructions**, then discovered that the VM's `instance-of` handler
had only walked superclass chains. A fix now recursively resolves original
DEX `class_def` interfaces and superinterfaces, distinguishing complete
positive/negative checks from unknown external platform ancestry.

A subsequent **live Eden** run executed **416 original Quest DEX instructions**
and stopped at `Ljava/util/HashMap;.get`, original code item `0x12c8f8`, PC
`0x2`. That same run observed pinned original manifest/resources, ARM64 ELF
paths, real staged files/OBB directories, the live NWindow 1280x720 and
Horizon locale `en-US`. These data owners do not turn into a Java Context,
Intent, PackageManager, or Resources object without matching lifetime and
type-backed Java services. The original `UnityPlayer.currentActivity` field
and final OpenXR JNI hook remain gated, and the first original Vulkan
creation has not executed.

The devkitA64/NVK Lifecycle NRO built on November 16 with the FFmpeg
decoder linked. All 15 Python original-ELF/JNI/ABI tests passed after the
memory-advice change, as did the actual Bionic MADV_NORMAL ownership/page
preservation host regression and the focused UnityPlayer currentActivity,
currentContext and JNI_OnLoad pending-exception host tests. The test that
simulates a constructor reaching the Vulkan stop guard
still verifies that the next constructor and JNI callback are skipped.

The original libcrypto ARM constructor at VA 0xf8c60 reads OPENSSL_armcap
before attempting an Android/Linux SIGILL-based raw ARM system-register
feature probe. Eden aborts while translating that MRS instruction; the
loader now sets OPENSSL_armcap=0 before constructors, selecting the valid
software cryptographic routines rather than advertising CPU features or
patching the original binary. The subsequent guest trace confirmed that
the pinned OpenSSL constructor and all other original module constructors
completed normally.

A PowerShell-driven Eden Qt launch of the rebuilt NRO produced a fresh guest
log: all 15 libraries mapped, zero strong relocation failures, original
libmain and libil2cpp JNI_OnLoad ran, GameActivity registered 24 natives,
the FFmpeg MediaNDK self-test passed, and native Vulkan extension/window
preflight passed with 14 extensions. All original ELF constructors completed.

The original second JNI class name was a 50-byte heap-owned Unity string.
The guest's 40-byte object had a correct long tag `0x5f`, length/capacity
50/50, but a NULL heap pointer. Original allocator singleton/once state and
vtable dispatch were valid. Instrumenting the VM bridge revealed a genuine
536866816-byte PROT_NONE mmap reservation, followed by `madvise` with Linux
`MADV_NORMAL=0` and a 1MiB RW `mprotect`. The bridge formerly returned
EINVAL on `MADV_NORMAL` because only `MADV_DONTNEED=4` was recognized.
`MADV_NORMAL` now succeeds ONLY on verified owned mappings, as a non-mutating
reset to the bridge's sole normal policy; unknown addresses and advice still
fail and the DONTNEED page-discard path remains separate. No original guest
instructions or Quest ELF files were modified.

An actual Eden run after the change proved the long-string JNI failure gone:
the second class registered 20 methods and original `libunity.so JNI_OnLoad`
completed successfully with **88 native registrations**. Original
`libOVRPlugin.so` and `libovrplatformloader.so` JNI_OnLoad also completed.
Original `libUnityOpenXR.so JNI_OnLoad` reached
`GetStaticObjectField(UnityPlayer.currentActivity)`, which raised a pending
`UnsupportedOperationException` because no original Java constructor had
assigned that field. A new strict post-JNI_OnLoad pending-exception check
rejects this partial initialization instead of accepting its 0x00010006
return code. Actual original GameActivity NativeCode and the requested FIRST
Unity Vulkan graphics creation entry have NOT been observed. Emulator-host
Vulkan initialization and the read-only NVK preflight do not count as original
Unity graphics initialization.

### November 16 live continuation: 1,479 original DEX instructions

The completed 31,492,293-byte NVK Lifecycle NRO was exercised by the bounded
`tests/run_eden_pregraphics.ps1 -MaxSeconds 180` runner. The actual Eden guest
executed **1,479 original Quest DEX instructions**, advancing past the prior
live 416-instruction `HashMap.get` stop. Its next exact failure is the
`Ljava/util/WeakHashMap;.put` call inside
`Landroidx/arch/core/internal/SafeIterableMap;.iterator` at original code
item `0x12d10c`, PC `0x10`, opcode `0x6e`. The independent original-Dex host
fixture reached this same stop. True weak-key garbage-collection and
ReferenceQueue semantics are not yet present; a worker is auditing this
precisely rather than faking a `WeakHashMap.put` success.

A new type- and generation-checked native Activity attachment graph was
integrated into `source/main.c` after the original constructor's native
applet owner and Quest package/Context facts are observed. Its focused tests
pass, and the full subsequent NVK NRO build passed at **31,500,485 bytes**.
The subsequent Eden `loader.log` independently recorded:

`[java:attachment] original Quest typed native graph=1 Activity callback installed=1 Java attached=0`

The same live rerun remained at 1,479 original DEX instructions, confirming
no regression. These are backed **native data** with the original native
window, asset, path and thread owners; there is still no independently
delivered Android launch Intent, Java ContextImpl, Resources, Handler, or
verified Java Activity.attach. The adapter therefore correctly leaves Java
attached=0. The original startup still completes all 15 ELF constructors,
6/7 JNI hooks, zero unresolved strong relocations, and the read-only NVK
Vulkan preflight. Neither original Unity graphics creation nor OpenXR runtime
initialization has been reached.

### November 16 continuation: 1,525 guest DEX and native Quest assets

The VM implements bounded, non-owning `WeakHashMap` key identity through a
strictly non-collecting original-Dex startup interval (maximum 768 allocated
objects). It does not advertise GC, ReferenceQueue delivery, or fabricated
key collection. The original `SafeIterableMap` iterator insert now executes,
and a fresh NVK NRO build and Eden guest run independently advanced to
**1,525 original DEX instructions**. The next exact guest stop is
`Lkotlin/LazyKt;.lazy` from original `SavedStateHandlesProvider.<init>` at
code `0x21ad24`, PC `0x16`, opcode `0x71`. The focused original-Dex host
fixture reproduced this same stop and passed after updating its expectations.

`source/unity_dex_horizon_java_resources.[ch]` now provides native NDK
`AAssetManager`-backed file open/read/seek/close and a native-observed
configuration snapshot, while requiring a separate executable Java registry
for actual Resources/Configuration/AssetManager object getters. Live Eden
records `[java:resources] original Quest native assets=1 Java Resources=0
(unimplemented)`. This cannot substitute Java Resources or resource-ID
resolution. Native service integration, strict resource host tests, and the
full NVK NRO build passed. The initial cleanup-order experiment exposed a
native ALooper leak (`native ALooper owner cleanup incomplete`) because the
native owner had already been invalidated; the order was corrected to release
resources and native Looper while the actual Activity owner remains live,
then release the VM/Activity and attachment/package owners. The latest live
guest trace has no cleanup-incomplete message.

### November 16 continuation: actual Handler constructor and 1,952 DEX steps

The original `FragmentHostCallback.<init>` reached the first Android
`Handler` allocation at 1,902 verified guest DEX steps. A bounded native
Handler constructor adapter now validates the independently observed Horizon
main ALooper, exact queue identity and OS thread owner at allocation and
again at `Handler.<init>()V`. It owns at most 16 constructor-only records;
Android `Handler.post`, `Runnable.run`, Java Handler scheduling and Activity
attachment are NOT available. VM-side strict checks additionally require
same observed Looper across allocation/constructor. The dedicated original
Quest DEX-backed host regression checks stale owner, no native queue,
constructor identity and strict no-posting; it passes with clang `-Werror`.

The subsequent full NVK NRO build succeeded at **31,516,869 bytes**. A fresh
Eden guest run independently executed **1,952 actual original Quest DEX
instructions**, stopping at the imported
`Ljava/util/Collections;.synchronizedMap` call in the original
`FragmentManager.<init>` method (code `0x208384`, PC `0x2c`, opcode `0x71`).
Original native 15/15 ELF constructors, JNI 6/7, and native package/Context/
asset observations remain working; original guest Vulkan creation has not
been observed. The live resource service separately opened the original
`bin/Data/boot.config` asset at 768 bytes and read/closed 16 bytes. These
operations do not imply a real Android Java Resources or Handler dispatcher.

### Pinned original resources.arsc parser in the live NRO

The new bounded `source/unity_dex_quest_arsc_resolver.[ch]` parser authenticates
the original stored APK `resources.arsc` file (258,492 bytes, CRC32
`0x0b0977d4`), validates its resource table, package/type/key string pools,
type chunks and typed entries, and refuses ambiguous qualifiers when native
observations cannot choose a resource. Its host tests decode the actual Quest
app_name ID `0x7f0d0021` to `Beat Saber`, a real boolean and integer,
German/Japanese localized strings and orientation-dependent values, while
rejecting malformed bounds and unobserved density/locale.

The full NVK build passed at **31,525,061 bytes** and a fresh Eden guest run
independently logged: `[java:resources] pinned native ARSC=1 observed locale=1
typed app_name=1 value=Beat Saber`. That is a real native resource table
lookup with the OS-observed `en-US` language, not a Java Resources object,
Android Context or full resource/theme service. Original DEX still stops at
`Collections.synchronizedMap` after 1,952 guest instructions; original guest
graphics has not been reached.

### November 16: entire original Activity constructor returns in live Eden

After the VM's bounded synchronized-map semantics and strict host-backed native
Handler constructor path were integrated, `py -3 tests/test_unity_dex_vm.py`
passed. In that host fixture the original Quest Activity constructor returned
at 2,287 real DEX instructions; running the original onCreate afterward on the
same VM reached 2,296 instructions and stopped, correctly fail-closed, at
`Landroid/view/SurfaceView;.<init>` (code `0x22e9d8`, PC `0x2`, opcode `0x70`).
These latter onCreate instructions were **host fixture only**.

The full NVK-enabled `BeatSaberQuestLoaderLifecycle.nro` build passed at
**31,529,157 bytes**. A new controlled Eden guest run (runner exit 2,
`bootstrap-gated`) **independently verified 2,287 original Quest DEX steps
with `BS_DEX_STOP_RETURNED`**, zero code offset and no Java exception. Native
main-ALooper Handler constructors=1, but Java task scheduling=0. Original
Quest `bin/Data/boot.config` opened (768 bytes), read (16 bytes), and closed;
the authenticated native ARSC resolver returned `Beat Saber` and integer
duration `220`. The genuine native Horizon Activity owner, applet hook and
thread/queue observations remained valid. All 15 original ELF constructors
ran; 6/7 JNI hooks completed. The production Android Activity.attach check
returned `rc=0`, correctly blocking original onCreate and `currentActivity`;
libUnityOpenXR.so JNI_OnLoad remains waiting. A native package/attachment
graph is **not** a fabricated Java Context/Intent/Resources/Window object.
Original guest graphics initialization or Vulkan creation is still **not
reached**. Subsequent work must independently back Activity.attach before
claiming production onCreate progress, then implement a genuine child View/
SurfaceView provider and preserve the first original guest graphics guard.

### November 16: native child-view registry wired into production

The real NRO now initializes the bounded Horizon child-view registry and
connects its `BsDexAndroidSurfaceServices` callbacks to the production original
DEX dispatcher. It requires a verified attached Activity and independently
constructed Java child View and NWindow before allowing the original
`SurfaceView.<init>` to succeed. No production child provider is registered,
because an Android Java View hierarchy and `Activity.attach` have not run.
The cleanup path revokes SurfaceHost JNI tokens before resetting the child
registry and releasing the native Activity host.

The NVK-enabled Lifecycle NRO rebuilt successfully (31,537,349 bytes). A fresh
Eden run logged `[java:surface] native child registry=1 Java child provider=0
(unavailable)` and independently confirmed the original Activity constructor
returned at 2,287 instructions, followed by `Activity.attach rc=0` and the
unchanged 6/7 JNI startup wait. Original guest Vulkan creation remains
unobserved. The original DEX VM, Horizon SurfaceView child registration,
Activity attachment, original graphics boundary, Handler, and ARSC regression
tests passed. These native registry callbacks are now present in the genuine
NRO startup, but an attached Java Activity and child view still need executable
implementations before the original onCreate can advance.

### Follow-up: executable Resources adapter and original onCreate prefix

The production DEX Android host now has an optional `Resources` method
dispatcher, connected to the typed Horizon Java attachment adapter. The
adapter can execute `Resources.getBoolean(I)Z`, `getInteger(I)I`, and
`getString(I)Ljava/lang/String;` against the original authenticated ARSC data
only after an independently attached Java Resources receiver and its owner
generation have been verified. The real NRO additionally requires that binding
after a successful `Activity.attach` before entering the original onCreate.
This branch remains blocked in Eden because no attached Java Resources object
exists. The NVK-enabled Lifecycle NRO rebuilt at 31,537,349 bytes, and the
subsequent live Eden run again returned from the original Activity constructor
at 2,287 steps and stopped at `Activity.attach rc=0` with 6/7 JNI hooks.

In separate original-DEX host fixtures, exact test-only SurfaceView, EditorInfo,
BaseInputConnection, and Android Log operations permit 52 real original
`onCreate` bytecode steps. The next observed platform operation is
`Context.getSystemService("input_method")` at original code item `0x230330`,
PC `0x1a`, opcode `0x6e`. Production still has neither this Android input
service nor Activity.attach/child View evidence, so this host-only prefix is
not a claim of live Switch Java progress. The interpreter also refuses to
reenter or repeat an already dispatched onCreate while its original DEX frame
is active. Original-Dex VM, Android host, child-view, and graphics-stop
regressions passed after integration.

### November 16: guest attachment executor contract and Android text prefix

`source/unity_dex_android_attach_impl.[ch]` adds an executable attachment
adapter that can be registered only after a real guest Java framework executor
exists. It independently checks native NWindow and a delivered launch receipt,
executes the guest's actual `Activity.attach` callback, verifies live Java
`ContextImpl`/Activity/Window/Resources owners and getter results, and retains
outstanding partial-attach tokens for reliable teardown. The backend callback
contract and exact requirements are in
`tests/UNITY_DEX_ANDROID_ATTACH_IMPL_HANDOFF.md`; strict host and devkitA64
tests pass. No guest executor is registered in the Switch production sequence:
the platform still has no independently delivered Android launch Intent,
Java ContextImpl/Window/Resources object graph, or Java main Handler/Looper.

The VM can now allocate and run the default constructor of a real empty
`android.text.SpannableStringBuilder`, retaining the initialized mutable
empty sequence and its documented Java interface identity. Unsupported
editing/spans still fail at the original call. This lets original
`classes.dex` execute further in a separately labeled **host-only** fixture:
an explicit input-method service token models the observed
`Context.getSystemService("input_method")` return, and a narrow
`View.getContext()` fixture returns the *same* Context instance supplied to
the View constructor. With those declared test dependencies, the interpreter
executes **63 original onCreate instructions** and stops at the next
missing Android service: `Landroid/app/Activity;.getWindow`, code
`0x230330`, PC `0x31`, opcode `0x6e`. Without the test-only services
the existing production-faithful fixture still stops at 52 instructions
on `Context.getSystemService("input_method")`. None of this is live NRO
onCreate execution.

The full NVK-enabled `BeatSaberQuestLoaderLifecycle.nro` rebuilt with the
new adapter and original-Dalvik changes at **31,537,349 bytes**. The fresh
Eden guest run returned `bootstrap-gated` (runner exit code 2) and
independently confirmed **2,287 original DEX constructor instructions
returned**, **15/15** native ELF constructors and **6/7** JNI hooks.
The native SurfaceView registry remained available with no Java child
provider; `Activity.attach` returned `rc=0` and the original onCreate
was correctly blocked. Original Unity graphics initialization has
**not** been reached. `test_unity_dex_vm.py`,
`test_unity_dex_android_attach_impl.py`,
`test_unity_dex_android_framework_context.py`,
`test_unity_dex_horizon_android_attachment.py`,
`test_unity_dex_horizon_java_attach_runtime.py`, and
`test_original_jni_graphics_boundary.py` passed with the current code.

### November 16 evening: original graphics boundary reached

The Eden run now ends in `original-graphics-stop` (runner exit 0). Original
IL2CPP init completes. Unity logs SystemInfo (4 cores), applies Quest 2 /
Android 12L identity, loads UnityOpenXR (its OpenXR loader initializes; no
runtime is present), and handles APP_CMD_START, INIT_WINDOW and resume. Its
first graphics call is `eglGetDisplay` from libunity.so+0x972614. Unity never
dlopens `libvulkan.so` (one-shot `[graphics] guest dlopen/dlsym` logs stay
silent), so in this environment EGL/GLES is the true first graphics entry.
The Vulkan guard stays armed in case a later build selects Vulkan.

Changes that got startup here:

- **Signals:** `sigaction`/`signal` store bionic dispositions (`{handler,
  mask, flags, restorer}`) and return 0. Before this, Boehm GC aborted with
  "Cannot set SIG_SUSPEND handler". `bs_bionic_signal_handler()` exposes the
  stored handler for future explicit delivery.
- **GC:** `getenv("GC_DONT_GC")` returns `1`. Boehm stop-the-world uses
  `pthread_kill(SIG_SUSPEND)`, which Horizon can't deliver, so the heap grows
  instead of collecting. This is temporary.
- **CPU topology:** synthetic `/proc/cpuinfo` (4x Cortex-A57) and
  `/sys/devices/system/cpu` files (possible/present/online, capacity, cpufreq,
  topology, MIDR).
- **Paths:** package-private directories beside `files/` (cache,
  shared_prefs, Unity GUID dir) map into the game root.
- **dlopen:** bare plugin names (`UnityOpenXR`) resolve to loaded
  `lib<name>.so` modules, as Android's nativeLibraryDir lookup does.
- **EGL boundary:** every guest `egl*`, `glGetString`, `glGetIntegerv` and
  `glGetError` import maps to a refusal stub (EGL_BAD_DISPLAY,
  GL_INVALID_OPERATION). The stub logs `[graphics:boundary] verified guest EGL
  entry`. Before this, host Mesa EGL was passed straight through, ran on guest
  TLS/x18, and crashed inside nvc0 in `eglCreateContext`.
- **JNI:** the reference free list is FIFO, and `PopLocalFrame` creates the
  survivor before releasing frame refs, so stale handles never alias it.
  The `strdup` in jni_bridge.c was replaced so it builds with MSVC.
- **Tests and logging:** the strict self-test gets ENOENT filesystem stubs and
  pins strict JNI (`bs_jni_set_lenient(0)`). The runner accepts the EGL
  boundary. Per-call fs/mmap/cond_wait/GetEnv traces were removed (about 5 MB
  down to about 540 log lines).

Known remaining work before or alongside graphics:

- Cooperative GC thread suspension, so GC_DONT_GC can be removed.
- SharedPreferences, `Activity.runOnUiThread`, PackageManager versionCode
  and permissions, `splitPublicSourceDirs`, `ApplicationInfo.nativeLibraryDir`.
- AlertDialog (Unity's EGL failure dialog), and `raise(5)` after it.
- `android_app_set_activity_state` cmd 12 timeout.
- Display width/height (lenient zeros), and a second AssetManager jobject that
  `AAssetManager_fromJava` rejects.
- `/proc/<pid>/exe` is unmapped.
- A GLES (or GLES-on-Vulkan) renderer behind the EGL boundary.
