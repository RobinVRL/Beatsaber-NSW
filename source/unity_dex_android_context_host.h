/* Exact Quest DEX Android Activity/Context/Intent method adapter.
 * This is a BACKED interface: unobserved Android services return unavailable.
 * It never constructs a fake Java Activity, grants JNI readiness, or publishes
 * UnityPlayer.currentActivity. */
#ifndef BS_UNITY_DEX_ANDROID_CONTEXT_HOST_H
#define BS_UNITY_DEX_ANDROID_CONTEXT_HOST_H

#include "unity_dex_vm.h"
#include <stddef.h>
#include <stdint.h>

/* Read-back from an actual Activity.attach and its Java getters. These are
 * opaque, independently verified Java objects, never strings or path tokens.
 * An Activity is constructed before attach; onCreate must follow attach. */
typedef struct BsDexAndroidContextAttachment {
    void *activity;
    void *intent;
    void *application_info;
    void *resources;
    void *assets;
    void *package_manager;
    void *metadata_bundle;
    int metadata_field_observed;
    void *files_dir;
    void *obb_dir;
    void *native_library_dir;
} BsDexAndroidContextAttachment;

typedef struct BsDexAndroidContextHost {
    void *user;
    void *activity;              /* actual verified GameActivity subclass */
    void *intent;                /* actual observed launch Intent */
    void *application_info;      /* actual observed ApplicationInfo */
    void *resources;             /* actual Resources */
    void *assets;                /* actual AssetManager */
    void *package_manager;       /* actual PackageManager */
    void *metadata_bundle;       /* observed ApplicationInfo.metaData */
    int metadata_field_observed;  /* permits a truly read-back null Bundle */
    void *files_dir;             /* observed java.io.File, not a raw string */
    void *obb_dir;               /* observed java.io.File */
    void *native_library_dir;    /* observed java.lang.String */
    /* Set by bind_attachment only after the backend independently attests
     * that the real Android Activity.attach was performed. */
    int attachment_evidenced;

    /* Must independently validate actual platform class identity/liveness.
     * A non-NULL field pointer, including the Activity, is never sufficient. */
    int (*verify)(void *user, const char *descriptor, void *object);
    /* Independently confirm a completed Activity.attach and the supplied
     * Java read-back. Must inspect the actual Activity/Context owner and
     * attachment state, not accept a matching pointer as its sole evidence. */
    int (*verify_attachment)(void *user,
                             const BsDexAndroidContextAttachment *observed);
    /* Actual Activity view hierarchy lookup. A returned NULL view is valid
     * only if the real lookup was performed (return 1). */
    int (*find_view)(void *user, void *activity, int32_t id, void **view);
    /* Actual Intent extras: return 1 after observing/mutating stored value,
     * 0 if unavailable, negative if a Java exception was raised. NULL extra
     * is a valid observed absence or value on success. */
    int (*get_extra)(void *user, void *intent, const char *key,
                     void **java_string, const char **exception);
    int (*put_extra)(void *user, void *intent, const char *key,
                     const char *value, const char **exception);
    /* Actual application metadata Bundle.getBoolean(name), with platform
     * semantics for absent key reflected in the supplied observed value. */
    int (*get_boolean)(void *user, void *bundle, const char *key,
                       int *out_value, const char **exception);
} BsDexAndroidContextHost;

/* Commit all observed attachment fields atomically, after verification.
 * A missing required Java object or a rejected backend attestation preserves
 * the previous host state. Does not itself perform Android Activity.attach. */
int bs_dex_android_context_bind_attachment(
    BsDexAndroidContextHost *host,
    const BsDexAndroidContextAttachment *observed);
/* Call only after releasing the live Activity through the platform backend. */
void bs_dex_android_context_clear_attachment(BsDexAndroidContextHost *host);

/* Returns 1 ONLY when an exact original DEX imported method has been
 * successfully performed against a live verified host object. Returns 0 for
 * unimplemented/unbacked and -1 for a witnessed Java exception. */
int bs_dex_android_context_invoke(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *method_name,
    const char *prototype, uint8_t invoke_opcode,
    const BsDexValue *args, size_t count,
    BsDexValue *result, const char **exception);

int bs_dex_android_context_field(
    void *user, BsDexVM *vm,
    const char *declaring_class, const char *field_name,
    const char *field_type, BsDexValue receiver, int is_write,
    BsDexValue *value, const char **exception);

#endif
