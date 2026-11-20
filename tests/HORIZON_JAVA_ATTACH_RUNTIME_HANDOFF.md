# Original Quest Java Resources method dispatch: attachment-gated runtime

`source/unity_dex_horizon_java_attach_runtime.[ch]` supplies a narrow
`BsDexHost.invoke_platform`-compatible callback for exactly:

- `Landroid/content/res/Resources;.getBoolean(I)Z`
- `Landroid/content/res/Resources;.getInteger(I)I`
- `Landroid/content/res/Resources;.getString(I)Ljava/lang/String;`

It executes the already-authenticated **original Quest** `resources.arsc`
typed resolution through `BsDexHorizonJavaResources`, with the independently
observed native configuration and fail-closed qualifier selection. The returned
`getString` value is a VM-owned Java String value allocated by
`bs_dex_new_observed_string_utf8`, not an external JNI `jstring`.

## Integration

After an actual Java backend has executed `Activity.attach`, installed an
independently witnessed launch Intent, bound the Java Resources registry,
and successfully verified Java attachment:

```c
BsDexHorizonJavaAttachRuntime runtime = {0};
if (bs_dex_horizon_java_attach_runtime_bind(
        &runtime, &attachment, &native_resources) == 1) {
    /* Route original Resources invocations through this exact callback. */
    host.invoke_platform = bs_dex_horizon_java_attach_runtime_invoke_platform;
    host.user = &runtime;
}
```

The example shows the callback signature; a real combined platform host must
preserve its existing `host.user` and forward Resources requests to this
callback with `&runtime` while retaining the existing Activity/Surface/Handler
dispatch. `bs_dex_horizon_java_attach_runtime_clear` revokes the borrowed
identity before attachment/resource owners are freed.

`bind` requires `bs_dex_horizon_android_attachment_java_attached()`, a live
native Resource service, the independently verified Java Resources registry,
the exact same Resources and AssetManager handles in the Activity readback,
and live class verification. Every method call rechecks these facts, both
owner generations, and the exact receiver bound to the VM. Unsupported method
names, signatures or invocation opcodes, wrong receivers, missing resource
IDs, incompatible types and insufficient Android configuration are unavailable
(`0`, without changing `*result`). This adapter never sets Java attachment,
launch Intent, Activity state or JNI UnityPlayer.currentActivity.

## Production boundary

Current production has a verified native Activity base, real `NWindow`, native
AssetManager and authentic ARSC, but **no independently delivered Android
Intent, Java ContextImpl/Window, complete Resources registry, verified Java
main Looper/Handler dispatch, or executable Activity.attach**. The current
`bs_dex_horizon_android_attachment_java_attached()` therefore correctly returns
zero. This adapter is executable once those services exist, but deliberately
cannot advance production onCreate until the external attachment requirements
are completed. It adds no fake Java objects or synthetic readiness.

## Validation

`py -3 tests/test_unity_dex_horizon_java_attach_runtime.py` builds with host
clang `-std=c11 -Wall -Wextra -Werror` and tests real original APK ARSC ID
`0x7f0d0021` (`Beat Saber`), boolean `0x7f040001`, integer `0x7f090000`
(`220`), wrong resource type, unobserved orientation/density, invalid opcode,
wrong prototype, mismatching receiver, stale generation, missing Java
Resources verification, missing class proof, and absent Java attachment.
All checks passed. Fake test-side attachment tokens only exercise the strict
gate; they do not assert production Java framework completion.
