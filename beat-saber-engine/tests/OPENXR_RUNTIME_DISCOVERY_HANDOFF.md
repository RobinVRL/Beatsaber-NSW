# OpenXR runtime discovery handoff (pregraphics only)

## Verified evidence, November 16, 2026

This audit uses the **original** Quest 1.46.0 AArch64
libopenxr_loader.so (480,224 bytes) and libUnityOpenXR.so (1,446,608 bytes).
Their SHA-256 identities, JNI_OnLoad and XRSDKPreInit ARM64 call sites and
exact exported symbol offsets are checked independently in
test_original_openxr_post_activity_elf.py. The staged APK and SD copies
match. No guest OpenXR runtime was executed for this handoff; Eden remains
owned by the prime agent.

Original libUnityOpenXR.so JNI_OnLoad at 0xC0B78 loads
libopenxr_loader.so, looks up optional JNI_OnLoad (not exported by the
staged loader), then reads the actual UnityPlayer.currentActivity Java static
field and makes a global reference. That JNI static field has not yet been
assigned by an executed original Java constructor. JNI_OnLoad must succeed
before separately invoking XRSDKPreInit.

Original XRSDKPreInit at 0xC0088 loads the OpenXR dispatch functions.
The 0x90F10 indirect call requests xrInitializeLoaderKHR through
xrGetInstanceProcAddr(XR_NULL_HANDLE, ...) and 0x9102C requests xrCreateInstance.
These are **statically verified proc-address lookups**, not proof that an
OpenXR instance has been created or that a display/session is available.

The original loader exports xrGetInstanceProcAddr (0x274F0) and
xrCreateInstance (0x26110), but **does not export**
xrNegotiateLoaderRuntimeInterface. That is correct for an OpenXR loader:
this last symbol is exported by a separate, actual runtime. A string
mention of xrNegotiateLoaderRuntimeInterface in the loader is not an export.

The Windows-side full /1.40.8 project scan found no active_runtime.json,
active_runtime.aarch64.json, vendor runtime .so, identifiable Android runtime
broker, or Monado build. The UnitySubsystemsManifest.json is a Unity plugin
catalog, not an OpenXR active-runtime manifest. The only OpenXR-related
managed MockRuntime.dll files belong to the unrelated Windows Beat Saber
installation and are neither Android ARM64 vendor runtime libraries nor
evidence of XR hardware.

The NVK SDK contains Mesa/nxvk libvulkan.a and exports
vkGetInstanceProcAddr and vk_icd* entry points. Vulkan graphics-driver
entry points cannot provide the separate OpenXR loader/runtime interface.
Plugin extension strings include XR_KHR_android_create_instance,
XR_KHR_vulkan_enable, XR_KHR_vulkan_enable2, XR_FB_foveation, and
XR_META_vulkan_swapchain_create_info. These establish future compatibility
targets, but their mere presence does not establish which extensions are
mandatory for the original plugin in the observed execution path.

## Runtime discovery and actual broker boundary

According to Khronos OpenXR loader design documentation, Android loaders
query Android ContentProviders under
org.khronos.openxr.system_runtime_broker and
org.khronos.openxr.runtime_broker. Those providers expose runtime
package_name, native_lib_dir, so_filename, and optional function remapping
via content resolver/Cursor APIs. If the broker cannot supply a runtime,
Android can fall back to active_runtime.aarch64.json or active_runtime.json
under the API-versioned openxr/1 folders in product, odm, oem, vendor and
system image configuration directories. These system files are not expected
to reside in an application's APK. Their absence from the staged project
proves only the local lack of runtime assets; an actual Android device may
have a separately installed/system-provided broker and runtime.

The shipped Switch NRO's synthetic JNI/Android host must back any broker
query with real ContentProvider data and functioning ContentResolver,
Cursor, Activity/Context, ApplicationInfo and AssetManager semantics.
Returning a fabricated provider row, instance, or success code will
break the runtime negotiation contract. Alternatively, an actual runtime
manifest could select a real Switch-ported ARM64 runtime library if the
guest VFS/library loader supports its location and native dependencies.
The original Android loader will still have its Android loader-init
requirements. A local filesystem manifest alone is insufficient.

An authentic Switch OpenXR route needs:

1. A real OpenXR runtime implementation built for Switch AArch64, exporting
   xrNegotiateLoaderRuntimeInterface (or its manifest-declared mapped name),
   with a functioning proc-address dispatch table and instance lifecycle.
2. Backed Android loader initialization and broker/manifest discovery plus
   an executable bridge to the chosen runtime's exported negotiation ABI.
3. A real display/compositor and tracked input/reference-space implementation
   satisfying the selected OpenXR runtime's actual capability claims; XR
   session creation and graphics requirements must return true runtime data.
4. Real Vulkan interop with NVK, including whatever Vulkan and OpenXR
   extensions the original Unity plugin actually requests. The existing
   one-shot original vkCreateInstance/vkCreateAndroidSurfaceKHR guard must
   stay armed so that the first original graphics creation call stops before
   reaching the graphics driver.

No such OpenXR runtime, native XR display service or tracked
session implementation has been verified in this project. It is therefore
not possible to claim that original XRSDKPreInit can successfully pass
through to the first guarded Vulkan call today.

## Isolated usable verifier

Run from beat-saber-engine:

    py -3 tools/openxr_runtime_discovery.py
    py -3 tests/test_openxr_runtime_discovery.py
    py -3 tests/test_original_openxr_post_activity_elf.py

The verifier scans the project for the two named active-runtime manifests,
validates the Khronos 1.0.0 manifest format, resolves referenced .so files
only inside explicit scan roots, verifies ELF64 AArch64 ET_DYN, and requires
the actual exported (possibly renamed) loader/runtime negotiation symbol
in SHT_DYNSYM. It never calls the library or probes Android hardware.
Paths are read only; arbitrary system libraries are never loaded. An
unusual stripped ELF lacking a readable dynamic section table is conservatively
rejected by this audit tool until a stronger binary parser is implemented.

Runtime scan result for the actual /1.40.8 workspace:

    status: NO_RUNTIME_MANIFEST
    scan_errors: []
    pregraphics_runtime_ready: false
    loader_runtime_negotiated: false

The tool intentionally exits 2 even if it discovers a valid library export:
its strongest possible status is CANDIDATE_NOT_NEGOTIATED. A real
loader/runtime handshake with actual return codes and runtime capabilities
is necessary before integration can mark runtime ready. The independent
eleven-case host test includes a synthetic inert ARM64 ELF fixture to exercise
positive export parsing; it never claims that fixture implements a runtime.
It additionally rejects the actual original Quest libopenxr_loader.so as
a vendor runtime despite its OpenXR API exports.

Reference: Khronos OpenXR loader/runtime format and Android broker:
https://registry.khronos.org/OpenXR/specs/1.1/loader.html

The original binary call graph is documented separately in
OPENXR_POST_ACTIVITY_PREGRAPHICS_AUDIT.md.
