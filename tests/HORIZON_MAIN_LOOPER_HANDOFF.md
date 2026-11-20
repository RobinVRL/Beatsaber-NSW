# Horizon main ALooper and original Java Handler queue boundary

The actual Quest DEX references android.os.Handler constructors, post,
postDelayed, removeCallbacks and android.os.Looper.getMainLooper. The
existing GameActivity native transport has a real ALooper (the
bs_looper_for_thread owner in android_window_bridge). That native looper
does not construct an Android Java Looper, Handler or MessageQueue.

source/unity_dex_horizon_main_looper.[ch] implements an isolated native
owner-thread work queue that can be bound to actual Java Handler objects
only after an independent Java runtime confirms them. Every posted
Runnable retains an explicit, strong VM/JNI reference through the
runtime's retain_runnable callback. Cancellation and completed execution
release the strong hold exactly once. Time uses monotonic Horizon ticks,
with overflow checking, stable due-time ordering and FIFO insertion
order for equal due times. Native bs_looper_wake signals the existing ALooper
after posting or cancellation; the queue itself never drives appletMainLoop.

## Native production integration

On the actual applet main thread, after native Activity base construction,
and only after the existing bs_looper_for_thread is prepared:

    BsDexHorizonMainLooperProduction native_owner = {
        .activity = &java_runtime,
        .horizon = &native_observations
    };
    BsDexHorizonMainLooperNative native_callbacks =
        bs_dex_horizon_main_looper_production_callbacks(&native_owner);
    BsDexHorizonMainLooper queue = {0};
    int native_observed = bs_dex_horizon_main_looper_observe_main_loop(
        &queue, &native_callbacks);
    uint64_t real_horizon_main_tid = 0;
    int owner_observed = bs_dex_horizon_main_looper_owner_thread_id(
        &queue, &real_horizon_main_tid);

Repeated native observation with the same owner/backend is idempotent and
does not acquire extra ALooper references. The queue holds one native
ALooper reference until bs_dex_horizon_main_looper_shutdown, which must
run on the actual owner thread BEFORE BsDexVM destruction.

## Java runtime requirements

Java task support is deliberately unavailable by default. After a real
Java main Looper and Handler constructor have executed, an interpreter
can provide BsDexHorizonMainLooperJava callbacks:

- verify_handler: inspect a live original DEX Handler receiver, its
  constructor and actual owning Java main Looper.
- retain_runnable: obtain strong Java/VM ownership of the precise Runnable
  reference, rather than a borrowed jobject or scalar weak pointer.
- release_runnable: release each successful retained Runnable exactly once.
- invoke_runnable: execute the ORIGINAL Runnable.run bytecode on the native
  owner thread, reporting success only after actual execution returns.

Then bs_dex_horizon_main_looper_bind_java_handler can enable the narrow
functional Handler subset. post and postDelayed enqueue only after exact
Java verification. removeCallbacks cancels every queued matching Runnable.
drain executes due callbacks only on the owner thread. next_timeout_ms
helps the outer native pump select a bounded bs_looper_poll_once timeout.
Off-thread posting is deliberately unsupported in this subset.

The existing BsDexVM has no public entry to invoke an arbitrary original
Runnable.run method or retain a Java Runnable. Consequently the production
Java callbacks are still missing. Native ALooper observation is true but
Handler construction and Android MessageQueue/Looper readiness are not.
Never publish a synthetic Java Looper or mark UnityPlayer.currentActivity.

The host regression uses explicit deterministic Java callback fixtures
only for queue semantics: sorted deadlines, equal-time FIFO, cancellation
and strong-ref release, failed wake rollback, 64-post capacity limit,
live-thread checks, failed original callback sealing, and shutdown. It
does not claim that original Runnable.run or Android Java Looper has been
executed in the Switch port.

## Original APK inventory and verified Handler constructor

`python tests/test_unity_dex_handler_inventory.py` reads the original
`SwitchPort/beat-saber.apk` DEX and confirms these exact external Java
method refs and signatures:

| Declaring class | Method and exact prototype |
| --- | --- |
| `android.os.Handler` | `<init>()V` |
| `android.os.Handler` | `post(Ljava/lang/Runnable;)Z` |
| `android.os.Handler` | `postDelayed(Ljava/lang/Runnable;J)Z` |
| `android.os.Handler` | `removeCallbacks(Ljava/lang/Runnable;)V` |
| `android.os.Handler` | `getLooper()Landroid/os/Looper;` |
| `android.os.Looper` | `getMainLooper()Landroid/os/Looper;` |
| `android.os.Looper` | `getThread()Ljava/lang/Thread;` |
| `java.lang.Runnable` | `run()V` |

The production `BsDexHorizonJavaHandlerHost` implements native-bound
`Handler.new-instance` and default `Handler.<init>()V` on the observed
Horizon main thread. `bs_dex_horizon_java_handler_original_constructor_ready`
is a separate read-only proof for the precise object. It requires an
unchanged native ALooper/owner, the exact host record, and a VM snapshot
whose `allocated` and `initialized` flags were committed by the original
DEX new-instance and invoke. A direct call to the host constructor alone
cannot pass it. This is a constructor check, not a Java scheduling check.

The native queue now rejects reentrant `drain`/`shutdown` during an executing
callback while allowing that same Runnable to `post` follow-up work. During
`removeCallbacks`, pending entries are compacted before their strong holds
are released, so release callbacks cannot see stale canceled tasks.
Reentrant queue mutation and shutdown during retain/release are rejected.
`bs_dex_horizon_main_looper_failure_diagnostic` reports the retained failed
Runnable reference, its invocation callback result and a bounded copied
exception descriptor (when one was reported), until native teardown. No
exception is manufactured for a callback that only reports unavailable.

## Current production dispatch blocker

The original guest interpreter in `unity_dex_vm.c` explicitly gates every
platform `Handler` call except the default constructor before a host callback
can run (`handler_method_supported`). It has no public method to run an
arbitrary DEX `Runnable.run()V` receiver, no external strong-root lifetime
interface for pending Runnable references, and does not expose a canonical
Android `MessageQueue` or Java Looper instance. Native event-loop identity
and the VM's limited `Looper.getMainLooper`/`getThread` identity tokens are
insufficient to claim these missing contracts.

For real guest dispatch, the VM owner must first add: (1) validated
interface/receiver and Java strong-root retention/release for precisely the
queued Runnable, (2) a resumable original guest virtual `run()V` invocation
on the owner thread with exception semantics and bytecode progress, (3) a
canonical Java Looper/MessageQueue contract tied to the native observed
owner, and (4) an explicit VM Handler method gate allowing verified host
post/cancel/getLooper operations once those capabilities exist. Until then,
`bs_dex_horizon_main_looper_bind_java_handler` is deliberately uncalled in
production and the existing original guest execution stops at the actual
missing Android behavior.

Validation on Windows (original Quest APK staged):

```
python tests/test_unity_dex_handler_inventory.py
python tests/test_unity_dex_horizon_main_looper.py
python tests/test_unity_dex_horizon_java_handler.py
C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc-16.1.0.exe -std=c11 -Wall -Wextra -Werror -D__SWITCH__ -fsyntax-only -Isource -IC:\devkitPro\libnx\include -IC:\devkitPro\portlibs\switch\include source/unity_dex_horizon_main_looper.c source/unity_dex_horizon_java_handler.c
```
