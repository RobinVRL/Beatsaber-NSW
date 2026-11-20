"""Compile and exercise the composed, fail-closed Android host on original DEX.

This is a host unit test: C fixture handles are NOT Android VM/UI services.
No original Quest file is modified, no guest native library is loaded.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_APK = ROOT / "SwitchPort" / "beat-saber.apk"
SOURCE = (
    "unity_java_preinit.c",
    "unity_apk_dex_loader.c",
    "unity_dex_vm.c",
    "unity_dex_android_context_host.c",
    "unity_dex_android_surface_host.c",
    "unity_dex_android_text_input_host.c",
    "unity_dex_android_host.c",
)


def main() -> None:
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        if fallback.exists():
            cc = str(fallback)
    if not cc:
        raise RuntimeError("Host C compiler unavailable; set CC.")
    assert ORIGINAL_APK.is_file(), "Original, locally-owned Quest APK absent"
    with tempfile.TemporaryDirectory(prefix="bs-android-host-test-") as temp:
        binary = Path(temp) / ("android-host.exe" if os.name == "nt" else "android-host")
        sources = [str(ROOT / "source" / item) for item in SOURCE]
        sources.append(str(ROOT / "tests/test_unity_dex_android_host.c"))
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"), *sources, "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary), str(ORIGINAL_APK)], check=True)


if __name__ == "__main__":
    main()
