/* Executable, bounded android.content.res.Resources DEX method adapter.
 *
 * The original APK's validated resources.arsc supplies typed values. This
 * adapter runs only after a separately executed and independently verified
 * Activity.attach supplies an actual Java Resources receiver. The returned
 * String is owned by the DEX VM, never a fabricated JNI jobject.
 *
 * Native Context/AssetManager observations alone cannot enable this adapter.
 */
#ifndef BS_UNITY_DEX_HORIZON_JAVA_ATTACH_RUNTIME_H
#define BS_UNITY_DEX_HORIZON_JAVA_ATTACH_RUNTIME_H

#include "unity_dex_horizon_java_resources.h"
#include "unity_dex_vm.h"
#include <stddef.h>
#include <stdint.h>

typedef struct BsDexHorizonJavaAttachRuntime {
    BsDexHorizonAndroidAttachment *attachment; /* borrowed */
    BsDexHorizonJavaResources *resources;       /* borrowed */
    uint64_t attachment_generation;
    uint64_t resources_generation;
} BsDexHorizonJavaAttachRuntime;

/* Bind ONLY a real attached Activity with Java Resources whose registry
 * independently constructs and verifies Resources/AssetManager/Configuration.
 * No Java object or attachment is constructed by this function. */
int bs_dex_horizon_java_attach_runtime_bind(
    BsDexHorizonJavaAttachRuntime *runtime,
    BsDexHorizonAndroidAttachment *attachment,
    BsDexHorizonJavaResources *resources);

/* BsDexHost.invoke_platform-compatible dispatcher; forwards only the exact
 * original Java Resources.getBoolean(I)Z/getInteger(I)I/getString(I)String
 * methods. Requires a VM reference wrapping the actual observed Resources.
 * Unavailable IDs, missing qualifiers, wrong receivers and stale attachment
 * always return 0 without changing *result or announcing a Java exception.
 * The caller may continue to its other genuine Android/Java service hosts. */
int bs_dex_horizon_java_attach_runtime_invoke(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);

/* Alias with the explicit BsDexHost callback role; both signatures match. */
int bs_dex_horizon_java_attach_runtime_invoke_platform(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);

/* Revokes borrowed-owner identity. Must be called before owners are freed. */
void bs_dex_horizon_java_attach_runtime_clear(
    BsDexHorizonJavaAttachRuntime *runtime);

#endif
