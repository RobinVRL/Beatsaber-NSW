# Horizon startup provenance and genuine Java Window attachment

Scope: only `unity_dex_horizon_context_objects.[ch]`,
`unity_dex_android_attach_impl.[ch]` and new tests/docs. No changes to
`main.c`, `unity_dex_vm`, `unity_dex_android_context_host`, or Makefile.

## Actual libnx launch capability

Inspected the installed libnx API declarations in
`C:/devkitPro/libnx/include/switch/runtime/env.h` and
`switch/services/applet.h`. Native applets expose:

- `appletGetAppletType()` and `appletGetAppletResourceUserId()`: native
  process/applet owner identity, not an Android package or activity.
- `envHasArgv()`, `envGetArgv()`, `envGetLoaderInfo()`,
  `envGetLoaderInfoSize()`, `envIsNso()`: homebrew loader state. The
  `EntryType_Argv` environment configuration describes arbitrary homebrew
  arguments. The native `main(void)` does not receive Android Activity launch
  extras or a trusted `Intent`.
- `appletPopLaunchParameter(..., AppletLaunchParameterKind_UserChannel)`:
  app-specific IPC storage for a launched **Horizon** application, destructively
  popped. No Android Intent parser/schema, launcher component, or Android
  action/category is specified. The preselected-user parameter likewise
  describes a Horizon account, not an Android Activity.
- `appletGetCurrentApplicationId()`: only documented for a LibraryApplet
  and reports a Horizon ApplicationId, not Quest package metadata.

The new
`bs_dex_horizon_context_observe_startup(owner, &BsDexHorizonStartupObservation)`
reads only these *independently available native facts* on the existing
verified applet owner thread. Production installs an `observe_startup` source
callback, but leaves `verify_launch_receipt` NULL and never calls
`bs_dex_horizon_context_bind_launch`. This API is read-only and returns zero
and clears the result if the owner or source is unavailable. The observed
`loader_has_argv` field is only a boolean, **not** validated Android launch
args; loader-info availability does not prove any content.

Result: **no genuine delivered Quest-like `ACTION_MAIN` Intent can be proven
from the available Horizon applet/argv APIs.** The manifest `ACTION_MAIN`
filter is only the original APK's *accepted* Android launch type, not a
delivered event; it cannot stand in for Activity.getIntent or Activity.attach.
A future launcher must supply an independently auditable delivered event
containing exact Android action/package/component/categories and a durable
live verification source. That is distinct from platform launch ownership.

## Guest Activity.getWindow

The existing `BsDexFrameworkJavaAttachment.window` is the guest-owned
**Java** `Landroid/view/Window;` object. The independent framework verifier
already dispatches `Activity.getWindow()` and compares its resulting object
to `graph.window`. The new
`bs_dex_android_attach_impl_observe_java_window(adapter, activity, &out)`
provides access to that *verified, borrowed Java object* without changing
`BsDexAndroidContextAttachment` or `BsDexAndroidContextHost`.

It:
1. Rejects missing/partial/detached guest attachment and wrong Activity owner.
2. Rechecks the native NWindow, Java main Looper/Handler, exact delivered
   `source_token`, launch action/component/categories and native generation.
   The initial delivered receipt is copied into the adapter after successful
   attach; later verification rejects launch-source identity substitution.
3. Revalidates the complete guest attachment graph and Java getters,
   independently verifies `Landroid/view/Window;`, and **invokes the real
   guest `Activity.getWindow()` again**. Returns its pointer only when it is
   the exact `graph.window` owner; output is NULL on every failure.

Integration proposal for the separately owned ContextHost work: extend
`BsDexAndroidContextAttachment` and `BsDexAndroidContextHost` with a
`void *window` Java Window field, validate its class with
`host.verify(user, "Landroid/view/Window;", window)`, and add exact
`Activity.getWindow:()Landroid/view/Window;` dispatch using the observed
object only while `verify_attachment` confirms a live guest attach.
For a stronger live getter, add a backend callback that invokes this new
accessor on each dispatch. **Do not** pass native `NWindow *` as that object;
the C field `BsDexAndroidAttachImpl.window` still means *native NWindow*,
whereas `graph.window`/this accessor means Java Window.

This does not bypass the independently verified launch receipt or create Java
Window/ContextImpl/Resources. A real guest executor remains required before
original onCreate can proceed beyond `Activity.getWindow` at code 0x230330
PC 0x31. Test-only context fixtures are not production substitutes.

## Verification

- `py tests/test_unity_dex_horizon_context_startup.py`: original pinned Quest
  APK, strict clang `-std=c11 -Wall -Wextra -Werror`, real directory/ELF
  fixtures; no startup source, unavailable source, re-observation/changed
  native values, malformed booleans, owner invalidation and absence of
  Android Intent are checked.
- `py tests/test_unity_dex_android_attach_impl.py`: strict clang, live
  source-token substitution denial, mismatched Java `getWindow` return,
  correct Java Window vs native NWindow separation, post-detach denial,
  existing partial-attach exception/rollback checks.
- `py tests/test_unity_dex_horizon_context_objects.py`: original Quest
  native Context regression passes.
- `aarch64-none-elf-gcc -D__SWITCH__ -std=c11 -Wall -Wextra -Werror
  -fsyntax-only -Isource -IC:/devkitPro/libnx/include
  source/unity_dex_horizon_context_objects.c`: passed.
- Same devkit command for `unity_dex_android_attach_impl.c`: passed.

No actual Switch console run, Java Activity.attach, or Quest onCreate success
is claimed by these host/device compile tests.
