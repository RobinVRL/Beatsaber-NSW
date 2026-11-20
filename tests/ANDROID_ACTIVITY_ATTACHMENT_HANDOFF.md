# Original Quest Activity constructor/attachment handoff

The interpreter enters original `UnityPlayerGameActivity.<init>()V` through
`bs_dex_vm_start_activity_constructor()`. Its imported
`android.app.Activity.<init>()V` dispatches through
`BsDexAndroidHost.construct_activity_base`. It must construct a live Java
identity for the original subclass and its Android base state, with class
metadata verified through `observed_instance_of`; constructing an unrelated
`GameActivity` JNI proxy is insufficient. The VM binds the resulting object
to the original DEX receiver and continues executing the actual remaining
original constructor bytecode.

Android constructs its Activity before attaching a base Context. The platform
adapter must therefore do the following in order:

1. Supply a concrete `construct_activity_base` and `discard_activity` with
   independently verified subclass identity, on the actual UI thread. The
   constructor must only run the platform superclass constructor. The DEX VM
   itself executes the subclass's original bytecode.
2. After the constructor returns (`BS_DEX_STOP_RETURNED`), perform the
   equivalent of `Activity.attach`: associate the Activity with the real
   event/UI thread, application/launch Context, observed Intent, resources,
   AssetManager, directories and window. In the Horizon implementation this
   requires an actual native service or an emulated Java object with backed
   lifecycle, storage and window operations. This work belongs in the
   backend's `attach_and_observe_activity` callback.
3. The callback returns `BsDexAndroidContextAttachment` containing the exact
   live Java objects observed after attachment. Implement
   `BsDexAndroidContextHost.verify_attachment` using an independent readback
   of the Activity attachment state, Context owner and UI thread. The
   `verify` callback checks actual Java type identity for every returned
   object. A null metadata Bundle is allowed only when its field was read;
   optional directories and PackageManager remain unavailable until observed.
4. Call `bs_dex_android_host_complete_attachment(&preinit.platform, vm,
   &exception)`. This checks constructor return and UI thread, invokes the
   actual attach service, verifies Context ownership, then commits the
   observed snapshot atomically. Only after success call
   `bs_dex_vm_start_activity_oncreate(vm, activity, bs_dex_null())`.
5. Retain the attached Java objects through original DEX `onCreate` and the
   JNI GameActivity.NativeCode lifecycle. Destroy the DEX VM before discarding
   Activity and attached views. Afterwards call
   `bs_dex_android_context_clear_attachment` to revoke readback state.

The current NRO startup (`source/main.c:181-216`) installs only the already
initialized game library callback. It does not provide construction,
attachment, `Context` services, or a UI-backed `SurfaceView` service. The
host unit tests exercise callbacks with simulated Java objects; they are not
proof of production Android or Horizon execution.

Missing production services: Android class registry and superclass
construction semantics; Activity.attach and application/launch Context with
live Intent; resource, AssetManager, ApplicationInfo, files/OBB directory and
optional PackageManager mappings; UI event thread with Activity Window/view
hierarchy; SurfaceView's own verified NWindow and JNI Surface identity;
SurfaceHolder callback dispatch and view lifetime; and later observed
Configuration/native GameActivity callbacks. `bs_jni_gameactivity_native_proxy`
currently supplies only selected JNI method handlers; it is not a constructed
`UnityPlayerGameActivity` and cannot serve as a substitute.

An unavailable service must leave the original VM at its exact DEX stop,
without fabricating constructor completion, class assignability, attachment,
or `UnityPlayer.currentActivity`. The backend never publishes those Unity
static fields; the original interpreted field writes must be observed first.

## Native weak references

The original constructor calls `java.lang.ref.WeakReference.<init>(Object)`
with an externally hosted Activity referent. The DEX VM cannot infer native
object liveness from the VM's scalar reference identity. All four optional
`BsDexAndroidHost.weak_reference_create/get/clear/release` callbacks must be
installed together; the composed host rejects incomplete sets. The native
weak handle must never retain a JNI global reference or other strong owner.

`create` returns an independent, non-NULL weak token. `get` queries the
backend's live-object lifetime on every call and returns `bs_dex_null()`
after invalidation/collection, otherwise the exact original VM reference.
`clear` invalidates the weak referent in the backend and `release` reclaims
the token exactly once at VM destruction, even after `clear`. Backend host
lifetime must extend through VM destruction. The VM checks returned referent
identity and refuses a substitute VM reference.

The host tests simulate independent invalidation and observe the real
original constructor crossing this callback, followed by teardown release.
They do not establish Android garbage-collector or JNI weak-global behavior
on Switch. A Horizon Activity registry can instead back weak refs through
its authoritative native object generation and destruction lifecycle.
