# Guest Activity.attach adapter: exact scope and integration contract

New, standalone files:

- `source/unity_dex_android_attach_impl.[ch]`
- `tests/test_unity_dex_android_attach_impl.[cpy]`

The bridge provides `bs_dex_android_attach_impl_init()`,
`bs_dex_android_attach_impl_callbacks()` and
`bs_dex_android_attach_impl_clear()`. The callback table is shaped exactly
as `BsDexHorizonAttachmentJava`, for
`bs_dex_horizon_android_attachment_set_java()`. It preserves the existing
original DEX Activity constructor -> Android Activity.attach -> original
`onCreate` order, without affecting `main.c` or the existing Android host.

The guest service MUST provide **real executable Android Java** operations:

1. `BsDexFrameworkJavaExecutor` with a complete
   `BsDexQuestJavaWitness`. Every class inspection and `invoke_noarg`
   must call into real live Java object state, including
   `ContextWrapper.getBaseContext`, `Activity.getWindow/getIntent`,
   `Context.getResources/getAssets/getFilesDir/getObbDir/getClassLoader`,
   `Handler.getLooper`, `Resources.getAssets/getConfiguration` and
   `Intent.getComponent`.
2. `BsDexAndroidAttachGuest.verify_main_java_loop`: independently prove
   actual owner-thread Java `Looper` and `Handler`, not merely ALooper.
3. `BsDexAndroidAttachGuest.attach`: construct/retain the real
   `ContextImpl`, `Window`, `Intent`, Java `AssetManager`,
   `Resources`, `Configuration`, package graph and required `File`
   objects; actually invoke the Android `Activity.attach` side effects.
   Expose the real `BsDexFrameworkJavaAttachment` and its strong object
   owners, plus an opaque outstanding attachment token. Bind Java Resources
   through the existing `bs_dex_horizon_java_resources_bind_java` contract.
4. `BsDexAndroidAttachGuest.verify_attached`: reread independent live
   `Activity`/`ContextImpl`/`Window` fields on **every** request.
5. `BsDexAndroidAttachGuest.detach`: actually unregister/undo guest
   attachment side effects and release the strong Java owners. On a teardown
   failure, preserve them for retry.

With a genuine executor implemented, create a
`BsDexAndroidFrameworkContext` using the already bound native attachment and
resources; call `bs_dex_android_attach_impl_init(&adapter, &framework,
&java_executor, &guest)`; retrieve callbacks with
`bs_dex_android_attach_impl_callbacks(&adapter, &callbacks)`; then register
them using `bs_dex_horizon_android_attachment_set_java(&attachment,
&callbacks)`. Keep the framework, native owners, guest executor and adapter
alive through verified detach, and call `bs_dex_android_attach_impl_clear`
only afterward.

The bridge calls the native source's real observation and witnessed launch
functions again before the guest call. It rejects mismatched activity,
native NWindow, original attachment generation, delivered action/component,
launcher/VR category and source token. After the guest call, it executes the
entire existing Java Context getter verifier, independently reobserves native
resources/window/launch and runs the guest's own attach verifier.
Verification is repeated whenever the outer Activity host asks. On a failure
with an outstanding token it returns the token to the outer
`wrapper_attach`, which calls this adapter's `detach`. Even when native
sources become unavailable, the token can still be used for cleanup.

**Current production blocker:** Horizon currently observes a real NWindow,
native AssetManager, staged APK, paths and native Looper, but
`bs_dex_horizon_context_production_callbacks()` leaves
`verify_launch_receipt = NULL`: no independent delivery of an Android launch
`Intent` exists. `main.c` has no executable Java ContextImpl/Window/
Intent/Resources registry, no actual Activity.attach executor, and no
verified Java Handler/Looper attached to the native queue. A manifest
intent-filter and NWindow cannot establish those objects. This bridge
therefore does **not** claim guest attachment, onCreate readiness or
`UnityPlayer.currentActivity`; it stays unregistered until a real guest
implementation is available.

Verified on the pinned Quest APK and original `classes.dex`:

- `py tests/test_unity_dex_android_attach_impl.py`: strict C11 clang
  `-Wall -Wextra -Werror`, successful verified token, reread, native revocation,
  partial failure/Java exception, cleanup retry.
- `py tests/test_unity_dex_android_framework_context.py`: original APK,
  typed Context/assets/receipt and fully checked Java graph fixtures.
- `py tests/test_unity_dex_horizon_android_attachment.py`: original APK,
  owner generation, independent receipt, native/Java attach gates, detach.
- `py tests/test_unity_dex_horizon_java_attach_runtime.py`: original ARSC,
  Java Resources call gate.
- `py tests/test_unity_dex_vm.py`: original DEX baseline:
  native-host constructor reaches 1,902 steps and stops at
  `Landroid/os/Handler;.new-instance` code `0x204e50` PC `0x0`
  opcode `0x22`; separately supplied test Handler advances 2,296 steps and
  stops at `Landroid/view/SurfaceView;.<init>` code `0x22e9d8`
  PC `0x2` opcode `0x70`. The 52-instruction original onCreate host fixture
  stops at `Landroid/content/Context;.getSystemService` code `0x230330`
  PC `0x1a` opcode `0x6e`. Its explicitly test-only
  `input_method` fixture, together with the VM-owned empty
  `SpannableStringBuilder()` constructor and a separate test-only
  `View.getContext()` owner readback, reaches 63 steps, stopping at
  `Landroid/app/Activity;.getWindow` code `0x230330` PC `0x31`
  opcode `0x6e`. The test preserves the original Context object identity.

All test fixtures are host-only and grant no production service readiness.
