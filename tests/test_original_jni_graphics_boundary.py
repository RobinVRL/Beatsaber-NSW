"""Original Quest export and pregraphics boundary regression.

Matches the real owner-staged Android ARM64 libraries, not the Windows build.
No native code executes. This catches an incorrect JNI bootstrap order or an
accidental Vulkan driver call before the requested graphics stopping point.
"""
from pathlib import Path
import re

from test_original_gameactivity_elf import parse_elf

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "import/quest-engine/APK"
NATIVE = APK / "lib/arm64-v8a"

EXPECTED_ONLOAD_ORDER = (
    "libmain.so", "libil2cpp.so", "libgame.so", "libunity.so",
    "libOVRPlugin.so", "libovrplatformloader.so", "libUnityOpenXR.so",
)


def main():
    jni_sources = {}
    for shared in sorted(NATIVE.glob("*.so")):
        original = shared.read_bytes()
        _, exports, _ = parse_elf(original)
        if "JNI_OnLoad" in exports:
            jni_sources[shared.name] = exports["JNI_OnLoad"]
    assert set(jni_sources) == set(EXPECTED_ONLOAD_ORDER), jni_sources
    assert jni_sources["libunity.so"][1] == 196
    assert all(jni_sources[name][1] > 0 for name in EXPECTED_ONLOAD_ORDER)

    lifecycle = (ROOT / "source/unity_lifecycle.c").read_text()
    match = re.search(r"static const char \*const jni_order\[\] = \{(.*?)\};",
                      lifecycle, re.S)
    assert match, "missing original JNI bootstrap order"
    order = tuple(re.findall(r'"([^"]+)"', match.group(1)))
    assert order == EXPECTED_ONLOAD_ORDER, (
        f"JNI_OnLoad startup names differ from the supplied Quest ELF: {order}"
    )

    boot = (APK / "assets/bin/Data/boot.config").read_text()
    assert "xrsdk-pre-init-library=UnityOpenXR" in boot
    assert "xr-use-vulkan-offscreen-swapchain-no-main-display-buffer=1" in boot

    vulkan = (ROOT / "source/vulkan_bridge.c").read_text()
    assert "bs_vulkan_probe_pregraphics" in vulkan
    assert "VK_NN_VI_SURFACE_EXTENSION_NAME" in vulkan
    assert "VK_KHR_ANDROID_SURFACE_EXTENSION_NAME" in vulkan
    assert "bs_vulkan_graphics_boundary_seen" in vulkan
    # A probe must not accidentally create a Vulkan instance/surface or draw.
    probe = vulkan.split("bool bs_vulkan_probe_pregraphics(", 1)[1]
    assert "vkCreateInstance(" not in probe
    assert "vkCreateViSurfaceNN(" not in probe
    assert "vkQueueSubmit(" not in probe
    assert "vkCmdDraw(" not in probe
    # The guest boundary must be armed before original constructors execute.
    armed = lifecycle.index("bs_vulkan_arm_graphics_boundary(true);")
    constructors = lifecycle.index(
        "so_execute_init_array(module, bs_vulkan_graphics_boundary_seen);"
    )
    assert armed < constructors
    assert "caps->vulkan_preflight_ready" in lifecycle
    assert "caps->vulkan_surface_ready" not in lifecycle
    # A Vulkan-backed OpenXR session and tracked input are downstream of
    # the guarded Vulkan call. Original XR preinit may still need a runtime.
    assert "caps->openxr_runtime_ready" not in lifecycle
    assert "caps->tracked_input_ready" not in lifecycle
    assert lifecycle.index("bs_vulkan_graphics_boundary_seen()", constructors) < lifecycle.index(
        "bs_jni_call_onload_if_ready", constructors
    )
    assert "bs_vulkan_graphics_boundary_seen()" in (
        ROOT / "source/main.c").read_text()
    print("PASS: original JNI export set/order and graphics boundary matched "
          "Quest ARM64 ELF, boot.config and guarded NRO entry")


if __name__ == "__main__":
    main()
