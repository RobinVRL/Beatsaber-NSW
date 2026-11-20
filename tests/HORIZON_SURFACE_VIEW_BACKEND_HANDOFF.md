# Original Quest SurfaceView: native Horizon registration boundary

The pinned Quest 1.46 `classes.dex` reaches the `SurfaceView` **base** constructor
through original `UnityPlayerGameActivity$GameActivitySurfaceView` construction:

| Fact | Observed original guest state |
| --- | --- |
| Concrete object | `Lcom/unity3d/player/UnityPlayerGameActivity$GameActivitySurfaceView;` |
| Framework target | `Landroid/view/SurfaceView;-><init>(Landroid/content/Context;)V` |
| DEX execution site | Code item `0x22E9D8`, PC `0x2`, after nine real `onCreate` DEX instructions |
| Previous Activity constructor | Original Quest Activity constructor returned in live Eden after 2,287 DEX steps; `Activity.attach` is still unavailable |

`source/unity_dex_android_surface_host.[ch]` already checks the original
receiver's concrete DEX class, the live Context identity, the UI owner thread,
child-specific NWindow dimensions, and an exact
`bs_jni_surface_from_nwindow` → `bs_jni_backed_surface_nwindow` roundtrip.
It also requires an actual Java View object for an original DEX-owned
subclass and rejects unsupported holder callbacks, formats and Z-order changes.
These checks are not waived by this backend.

## Implemented native subset

`source/unity_dex_horizon_surface_view_backend.[ch]` supplies a bounded,
reversible registration registry through **only new files**. Its production
root proofs call `bs_dex_horizon_activity_on_ui_thread`,
`bs_dex_horizon_activity_context_matches`,
`bs_dex_horizon_services_on_applet_thread`, and
`bs_dex_horizon_services_valid_nwindow`, and require the attached Activity's
Window to be precisely the witnessed native applet root. These proofs will
remain unavailable until real `Activity.attach` has run.

An optional external `BsDexHorizonSurfaceViewChildOps` provider must actually
create a child in the live UI hierarchy, then independently attest its
attachment, UI ownership, Java identity, and teardown. The registry rejects
the default/parent NWindow, a reused sibling window, duplicate object identity,
non-UI calls, stale parent/activity state, and unverified Java objects. It
retains failed removals for retry, including when removal succeeded but the
independent detach verification failed. A separate native-only
`bs_dex_horizon_surface_view_register_native()` and
`bs_dex_horizon_surface_view_validate_native()` permit testing an externally
registered child without claiming a Java constructor has run.

`bs_dex_horizon_surface_view_services()` adapts this backend to the existing
`BsDexAndroidSurfaceServices` and wires the existing JNI/NWindow functions.
The `construct_view` service is more restrictive than the native-only entry:
it requires the child provider to produce an independently witnessed real
Java View, followed by the SurfaceHost's separate DEX class binding and
JNI Surface roundtrip. Holder callback registration must reach an actual
event dispatcher and retain the callback for eventual delivery. Holder format
and Z-order operations must actually change the child compositor state;
without those provider functions, their service callbacks return 0.

## Concrete libnx capability and blocker

The installed devkitPro libnx `switch/display/native_window.h` exposes
`nwindowGetDefault()` (the applet-owned default Window),
`nwindowCreateFromLayer()`, `nwindowGetDimensions()` and `nwindowClose()`.
`switch/services/vi.h` exposes `viCreateLayer`, `viSetLayerZ`,
`viSetLayerPosition` and `viCloseLayer`. A separately created VI layer can
therefore potentially own a *different* presentation surface, subject to
runtime permissions and successful service calls. That is **not** the same as
the original Android `Activity`'s `FrameLayout` containing an actual Java
`SurfaceView` child. No current Unity port code implements the Java View tree,
native child attachment to that tree, Java View class/lifetime, holder
callback dispatch, or the Android SurfaceView surface lifecycle.

Consequently, the production initializer with `child_ops=NULL` exposes no
successful child constructor. Returning the already observed
`nwindowGetDefault()` (or an unrelated independent VI layer) would misidentify
the parent window as the original View's child, so the backend refuses it.
It also does not claim that a VI layer creation alone satisfies Android
`setZOrderOnTop`, `SurfaceHolder.setFormat`, or callback delivery.

## Prime integration API and lifetime

```c
BsDexHorizonSurfaceViewBackend view_backend;
bs_dex_horizon_surface_view_backend_init(
    &view_backend, &activity_host, &observed_horizon, NULL);

BsDexAndroidSurfaceServices view_services =
    bs_dex_horizon_surface_view_services(&view_backend);
bs_dex_android_surface_host_init(&surface_host, &view_services);
/* Bind surface_host to the composed original-Dex host as before. */

/* After the original DEX VM is destroyed: */
int surface_removed = bs_dex_android_surface_host_reset(&surface_host);
int backend_removed = bs_dex_horizon_surface_view_backend_reset(&view_backend);
```

The Activity and Horizon observation records, and the provider callbacks
and their `user` data, must outlive the registry. The registry must outlive
the SurfaceHost; the SurfaceHost and original Activity owner outlive the DEX
VM. A failed reset must be retried while the actual native owner remains
usable. `bs_dex_horizon_surface_view_backend_init_with_hooks()` is reserved
for isolated host testing or another independently backed UI runtime; it
does not assert Switch platform readiness.

## Validation

* Host Clang `-std=c11 -Wall -Wextra -Werror`: compile and run
  `py -3 tests/test_unity_dex_horizon_surface_view_backend.py`, loading the
  real Quest APK `classes.dex`. The test compiles the existing original DEX
  VM and SurfaceHost with the new `.c` and test fixture.
  Covers missing provider/Java view, correct native child registration,
  Window/Context identity, parent and sibling alias rejection, callbacks,
  format and Z-order callbacks, stale owner, rollback and removal retry. With
  a simulated fully backed child, original DEX execution reaches code item
  `0x22EA48` PC 0 after 11 instructions. An intentionally mismatched JNI
  Surface roundtrip stops at `0x22E9D8` PC 2 after 9 instructions and revokes
  its token and removes its child. These host tests are not an Eden run.
* devkitA64 `aarch64-none-elf-gcc -std=c11 -Wall -Wextra -Werror -D__SWITCH__`
  compiled the new production `.c` using the real libnx headers.
* `py -3 tests/test_unity_dex_android_surface_host.py` passed against the
  **original Quest APK DEX**, including blocked original code item
  `0x22E9D8` PC 2 without a complete backed surface service.

No Eden run, original binaries, existing VM/JNI/platform/main files, or
existing SurfaceHost files were modified in this task.
