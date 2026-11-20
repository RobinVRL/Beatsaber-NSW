# Quest UnityOpenXR: Activity-to-pregraphics audit

This is a read-only ARM64 ELF investigation. Offsets are guest ELF virtual
addresses, not host addresses. No guest execution or original-library edits
were performed. The original SHA-256 identities and exact ARM64 instruction
patterns are pinned in test_original_openxr_post_activity_elf.py.

## End of libUnityOpenXR JNI_OnLoad

The original exported JNI_OnLoad starts at 0xC0B78 (604 bytes).

| VA | Call or effect |
| --- | --- |
| 0xC0BB4 | JavaVM.GetEnv (VM vtable slot 6, JNI 1.6) |
| 0xC0BDC | Conditional JavaVM.AttachCurrentThread (slot 4) |
| 0xC0C40 | dlopen(libopenxr_loader.so, RTLD_LAZY) |
| 0xC0C50 | dlsym(handle, JNI_OnLoad) |
| 0xC0C64 | Conditional call of nested loader JNI_OnLoad, only if exported |
| 0xC0D48 | JNIEnv.FindClass(com/unity3d/player/UnityPlayer), slot 6 |
| 0xC0D74 | GetStaticFieldID(currentActivity, Landroid/app/Activity;), slot 144 |
| 0xC0D90 | GetStaticObjectField(currentActivity), slot 145 |
| 0xC0DA4 | NewGlobalRef(activity), slot 21 |
| 0xC0DAC | Store returned Activity global ref at image-relative 0x16E398 |
| 0xC0DBC-0xC0DC8 | Return JNI_VERSION_1_6 |

The staged libopenxr_loader.so DOES NOT export JNI_OnLoad in its dynamic
symbol table. Therefore the optional call at 0xC0C64 is not executed when
that exact loader is loaded. Its genuine dlopen and dlsym results still
matter. After GetStaticObjectField, the only additional JNI call in the
plugin JNI_OnLoad is NewGlobalRef: no Activity member method or XR runtime
entry point is called directly in that remainder.

## Separate XRSDKPreInit and OpenXR function table

Original boot.config requests xrsdk-pre-init-library=UnityOpenXR. Export
XRSDKPreInit starts at 0xC0088 (116 bytes). Its successful conditional path:

1. 0xC008C calls plugin-state helper 0xC00FC.
2. 0xC0098 calls loader discovery helper 0x8D858. That helper calls
   0xF0E80, which uses dladdr and dlopen; 0x8D88C optionally dlsym
   SetXRTrace and 0x8D8B0 dlsym xrGetInstanceProcAddr.
3. 0xC00A4 calls stage-one table initializer 0x8DC28. The indirect
   call at 0x90F10 requests xrInitializeLoaderKHR using
   xrGetInstanceProcAddr(XR_NULL_HANDLE, name, &function_pointer).
   Calls at 0x90F4C, 0x90FBC and 0x9102C similarly request
   xrEnumerateApiLayerProperties, xrEnumerateInstanceExtensionProperties
   and xrCreateInstance, respectively. The routine also probes numerous
   other entry points conditionally.
4. 0xC00AC calls session-init helper 0xD74B8; 0xC00BC enters display
   pre-init helper 0x9CAE0 only after the preceding checks succeed.

These are STATICALLY reachable pointer lookups. They do not show that
xrInitializeLoaderKHR was invoked, xrCreateInstance succeeded, or any XR
runtime or Vulkan command actually executed. Live progress reported for this
task stops at unbacked UnityPlayer.currentActivity in original JNI_OnLoad.

## Android broker and real XR runtime prerequisites

The original libopenxr_loader.so exports xrGetInstanceProcAddr at 0x274F0
(branch to body 0x27500), and xrCreateInstance at 0x26110. The loader
itself is NOT an XR runtime. Its embedded Android runtime discovery names
show requirements for real:

- Android Context.getApplicationContext, getContentResolver, getAssets and
  getApplicationInfo behavior;
- android/net/Uri and Uri.Builder;
- ContentResolver.query overloads returning backed android/database/Cursor
  objects, supporting getColumnIndex/getString and cursor iteration/close;
- org.khronos.openxr.system_runtime_broker and
  org.khronos.openxr.runtime_broker content-provider authorities;
- a valid active_runtime.json or active_runtime.aarch64.json and a real
  runtime exporting xrNegotiateLoaderRuntimeInterface.

Neither the staged APK tree nor the staged Switch SD tree contains an
active_runtime*.json manifest. UnitySubsystemsManifest.json describes
UnityOpenXR display and input plugins; it is not a vendor runtime manifest.
An Activity global reference is necessary but insufficient for Android
loader initialization or runtime discovery.

Selected original libopenxr_loader.so ELF-relative string addresses:
0x905E = system_runtime_broker, 0x9087 = runtime_broker,
0x92F2 = /active_runtime.json, 0x9307 = /active_runtime.aarch64.json,
0x9DE7 = xrNegotiateLoaderRuntimeInterface, 0xA570 = getApplicationContext,
0x8C6B = getContentResolver, 0xA757 = getAssets,
0x8B26 = android/database/Cursor, 0x82C1 = getColumnIndex,
0x97ED = getString. These are exact source-string locations, not proof
that the corresponding Java methods have already executed.

The Unity plugin also embeds XR_KHR_vulkan_enable and
XR_KHR_vulkan_enable2 and graphics interop names such as
xrGetVulkanInstanceExtensionsKHR, xrGetVulkanGraphicsRequirementsKHR,
xrGetVulkanGraphicsRequirements2KHR, xrCreateVulkanInstanceKHR and
xrCreateVulkanDeviceKHR. Their strings show future graphics dependencies.
They are not proof of execution prior to the guarded Vulkan creation call.

Next, back UnityPlayer.currentActivity with a real observed Activity and
preserve NewGlobalRef ownership. Only after that, trace original XRSDKPreInit
and real OpenXR return codes from xrGetInstanceProcAddr and
xrInitializeLoaderKHR. Do not fake runtime manifests, XR functions, broker
results, session readiness or Vulkan creation.
