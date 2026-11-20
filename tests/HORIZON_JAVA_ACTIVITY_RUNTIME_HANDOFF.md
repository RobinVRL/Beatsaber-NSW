# Original Quest Activity base constructor: Horizon adapter

The pinned Quest DEX inheritance chain is verified by
bs_dex_horizon_activity_init(): UnityPlayerGameActivity -> GameActivity ->
AppCompatActivity -> FragmentActivity -> androidx.activity.ComponentActivity
-> androidx.core.app.ComponentActivity -> android.app.Activity. The original
DEX interpreter executes the Quest GameActivity and AndroidX constructors.
The terminal android.app.Activity constructor is framework code absent from
the APK.

The implemented minimum is a live, concrete-class native Activity instance,
stable Horizon main-thread ownership, and an actual libnx applet event
subscription for the duration of that instance. On Switch, the backend
uses bs_dex_horizon_services_native_ready() to verify the original staged
assets, SD directories, main thread and NWindow; then appletHook registers
focus, operation/performance, exit and resume observations. Events arrive via
the existing appletMainLoop(), with no synthetic event dispatch. appletUnhook
releases the callback and its heap-owned cookie on Activity destruction.

These are native Activity base-lifetime services for the portion of the
original DEX before Activity.attach. They do not implement a complete Android
framework Activity constructor, ContextImpl, Intent, Resources, Handler,
Looper, FragmentController or Java Window. Any later operation requiring
these Java services remains blocked. Native applet events do not imply Java
onResume/onPause, GameActivity NativeCode or Unity graphics readiness.

## Integration for the main.c owner

After staged assets and the JNI bridge are live on the applet main thread:

    BsDexHorizonServices observed = {0};
    BsDexHorizonJavaRuntime runtime = {0};
    BsDexHorizonActivityHost host = {0};
    if (!bs_dex_horizon_services_observe(&observed) ||
        !bs_dex_horizon_java_runtime_init(&runtime, &observed))
        return;
    BsDexHorizonActivityServices native =
        bs_dex_horizon_java_runtime_callbacks(&runtime);
    if (!bs_dex_horizon_activity_init(&host, dex.data, dex.size, &native) ||
        !bs_dex_horizon_java_runtime_bind_verified_host(&runtime, &host))
        return;

Then bs_dex_horizon_activity_bind() supplies the constructor, DEX-verified
class identity and weak-reference callbacks to the combined host. Configure
the existing independent surface host and create the original DEX VM.
Teardown in order: DEX VM, native views/surfaces, Activity host, runtime.
After bs_dex_horizon_activity_release(&host), call
bs_dex_horizon_java_runtime_shutdown(&runtime); this must succeed before
freeing runtime memory. A failed unhook must retain the runtime until retry.
The applet's existing main loop must continue to dispatch its real events.

## First observed next blocker

The focused host test interprets real Quest DEX instructions with this
constructor and host-managed external weak references. It executes 253 real
DEX instructions, then stops with BS_DEX_STOP_HOST_MISSING at code item
0xED804 PC 0x3, Ljava/util/Random;.new-instance. This is a missing Java
utility-class allocation path. It establishes host-side interpreter progress,
not actual Switch or emulator runtime execution. Android Activity.attach,
Context/Intent/Resources/Looper and eventual view/graphics initialization
are still blocked by unavailable Java services.
