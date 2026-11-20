"""Read-only original OpenXR JNI_OnLoad and XRSDKPreInit call-graph regression.

Validates the supplied Quest ARM64 libraries without executing original code
or inferring that an XR runtime is available. Run:
    py -3 tests/test_original_openxr_post_activity_elf.py
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import struct

from test_original_gameactivity_elf import parse_elf

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "import/quest-engine/APK"
STAGED = ROOT / "private-sd/switch/beat-saber-quest"
PLUGIN = APK / "lib/arm64-v8a/libUnityOpenXR.so"
LOADER = APK / "lib/arm64-v8a/libopenxr_loader.so"

PLUGIN_SHA256 = "4d62c5142fd1fa4e14a52b23ea25b2f9d95cc5a9db27da7cff0e858559f8198b"
LOADER_SHA256 = "1ac1ac7f39b8652c9fab8f4e4fb210ae6fdec982ca14cbb83bcc9316bd58320d"


def word(blob: bytes, locate, va: int) -> int:
    return struct.unpack_from("<I", blob, locate(va, 4))[0]


def branch(blob: bytes, locate, site: int, target: int) -> None:
    opcode = word(blob, locate, site)
    assert opcode & 0xFC000000 == 0x94000000, (
        f"0x{site:x} is not a direct AArch64 BL")
    relative = opcode & 0x3FFFFFF
    if relative & (1 << 25):
        relative -= 1 << 26
    assert site + relative * 4 == target, (
        f"0x{site:x} branches to 0x{site + relative * 4:x}, "
        f"not expected 0x{target:x}")


def string_at(blob: bytes, locate, va: int, literal: bytes) -> None:
    assert blob[locate(va, len(literal) + 1):
                locate(va, len(literal) + 1) + len(literal) + 1] == (
                    literal + b"\0"), f"0x{va:x}: source string changed"


def main() -> None:
    plugin = PLUGIN.read_bytes()
    loader = LOADER.read_bytes()
    assert len(plugin) == 1446608 and len(loader) == 480224
    assert hashlib.sha256(plugin).hexdigest() == PLUGIN_SHA256
    assert hashlib.sha256(loader).hexdigest() == LOADER_SHA256
    assert plugin == (STAGED / PLUGIN.name).read_bytes()
    assert loader == (STAGED / LOADER.name).read_bytes()

    pva, pexp, _ = parse_elf(plugin)
    lva, lexp, _ = parse_elf(loader)
    assert pexp["JNI_OnLoad"][:2] == (0xC0B78, 604)
    assert pexp["XRSDKPreInit"][:2] == (0xC0088, 116)
    assert lexp["xrGetInstanceProcAddr"][0] == 0x274F0
    assert lexp["xrCreateInstance"][0] == 0x26110
    assert "JNI_OnLoad" not in lexp, (
        "The loader now exports JNI_OnLoad: revisit the optional dlsym path")
    assert "xrNegotiateLoaderRuntimeInterface" not in lexp

    for va, literal in (
        (0x61F28, b"libopenxr_loader.so"),
        (0x5B4B5, b"JNI_OnLoad"),
        (0x5C464, b"com/unity3d/player/UnityPlayer"),
        (0x582D2, b"currentActivity"),
        (0x5E694, b"Landroid/app/Activity;"),
        (0x56F72, b"SetXRTrace"),
        (0x56F7D, b"xrGetInstanceProcAddr"),
        (0x5B247, b"xrInitializeLoaderKHR"),
        (0x58C12, b"xrEnumerateApiLayerProperties"),
        (0x5BAB7, b"xrEnumerateInstanceExtensionProperties"),
        (0x56930, b"xrCreateInstance"),
    ):
        string_at(plugin, pva, va, literal)

    # JavaVM.GetEnv first; if detached, AttachCurrentThread is attempted.
    assert word(plugin, pva, 0xC0BA8) == 0xF9401908  # JavaVM slot 6
    assert word(plugin, pva, 0xC0BB4) == 0xD63F0100
    assert word(plugin, pva, 0xC0BD8) == 0xF9401108  # AttachCurrentThread 4
    assert word(plugin, pva, 0xC0BDC) == 0xD63F0100

    # The optional nested loader JNI_OnLoad call only runs if its dlsym
    # returns an actual export. The staged loader has no such export.
    branch(plugin, pva, 0xC0C40, 0x156FD0)  # dlopen
    branch(plugin, pva, 0xC0C50, 0x156FE0)  # dlsym("JNI_OnLoad")
    assert word(plugin, pva, 0xC0C54) == 0xB4000700  # CBZ => 0xC0D34
    assert word(plugin, pva, 0xC0C64) == 0xD63F0100  # optional BLR

    # All direct post-dlsym JNI dispatches in this 604-byte JNI_OnLoad.
    # JNI table offsets are 8-byte entries; no Activity instance method
    # invocation or xrCreateInstance occurs after GetStaticObjectField.
    for site, call_site, table_offset, label in (
        (0xC0D44, 0xC0D48, 48, "FindClass / slot 6"),
        (0xC0D6C, 0xC0D74, 1152, "GetStaticFieldID / slot 144"),
        (0xC0D8C, 0xC0D90, 1160, "GetStaticObjectField / slot 145"),
        (0xC0DA0, 0xC0DA4, 168, "NewGlobalRef / slot 21"),
    ):
        insn = word(plugin, pva, site)
        assert insn & 0xFFC00000 == 0xF9400000, (
            f"{label}: expected 64-bit LDR")
        assert ((insn >> 10) & 0xFFF) * 8 == table_offset, label
        # GetStaticFieldID dispatch uses x9; the other entries use x8.
        # The BLR must call the same register loaded from that table slot.
        assert word(plugin, pva, call_site) == (
            0xD63F0000 | ((insn & 31) << 5)), label
    assert word(plugin, pva, 0xC0DAC) == 0xF901CD00  # store global ref
    assert word(plugin, pva, 0xC0DBC) == 0x528000C0  # JNI 1.6 low 16 bits
    assert word(plugin, pva, 0xC0DC0) == 0x72A00020  # JNI 1.6 high 16 bits

    # XRSDKPreInit is separate and conditional. It loads the XR library,
    # resolves xrGetInstanceProcAddr, initializes a dispatch table, then
    # attempts native session/display preparation.
    for site, target in (
        (0xC008C, 0xC00FC),
        (0xC0098, 0x8D858),
        (0xC00A4, 0x8DC28),
        (0xC00AC, 0xD74B8),
        (0xC00BC, 0x9CAE0),
        (0x8D878, 0xF0E80),
        (0x8D88C, 0xF14B4),  # optional SetXRTrace dlsym
        (0x8D8B0, 0xF14B4),  # xrGetInstanceProcAddr dlsym
    ):
        branch(plugin, pva, site, target)

    # Stage-one dispatch requests xrInitializeLoaderKHR with a NULL XrInstance.
    # The query is real; executing the resulting function is NOT established.
    assert word(plugin, pva, 0x90F04) == 0x91091C21  # name at 0x5B247
    assert word(plugin, pva, 0x90F0C) == 0xAA1F03E0  # x0 = NULL instance
    assert word(plugin, pva, 0x90F10) == 0xD63F0100  # call proc address
    assert word(plugin, pva, 0x90F14) == 0x37F81600  # negative result branch
    for site in (0x90F4C, 0x90FBC, 0x9102C):
        assert word(plugin, pva, site) == 0xD63F0100
    assert word(loader, lva, 0x274F0) == 0x14000004  # loader entry trampoline

    # The loader contains real Android runtime discovery, not a bundled XR
    # runtime. Require the real strings and reject mistakenly treating the
    # Unity subsystem plugin catalog as an active-runtime JSON manifest.
    for required in (
        b"org.khronos.openxr.system_runtime_broker",
        b"org.khronos.openxr.runtime_broker",
        b"/active_runtime.json",
        b"/active_runtime.aarch64.json",
        b"getApplicationContext()Landroid/content/Context;",
        b"getContentResolver",
        b"getAssets()Landroid/content/res/AssetManager;",
        b"android/database/Cursor",
        b"getColumnIndex",
        b"xrNegotiateLoaderRuntimeInterface",
        b"xrInitializeLoaderKHR",
    ):
        assert required in loader, f"OpenXR loader dependency missing: {required}"
    for graphics_dependency in (
        b"XR_KHR_vulkan_enable",
        b"XR_KHR_vulkan_enable2",
        b"xrGetVulkanInstanceExtensionsKHR",
        b"xrGetVulkanGraphicsRequirementsKHR",
        b"xrGetVulkanGraphicsRequirements2KHR",
        b"xrCreateVulkanInstanceKHR",
        b"xrCreateVulkanDeviceKHR",
    ):
        assert graphics_dependency in plugin, (
            f"original Vulkan interop prerequisite missing: {graphics_dependency}")
    for root in (APK, STAGED):
        assert not tuple(root.rglob("active_runtime*.json")), (
            f"usable Android XR runtime manifest unexpectedly staged under {root}")
    assert (APK / "assets/bin/Data/UnitySubsystems/UnityOpenXR/"
            "UnitySubsystemsManifest.json").exists()
    print("PASS: original UnityOpenXR JNI_OnLoad static Activity/global-ref "
          "chain, separate XRSDK loader proc lookups, Android broker and "
          "absent staged runtime manifest; no guest execution")


if __name__ == "__main__":
    main()
