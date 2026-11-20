"""Compile host-facing Horizon Activity class/attach adapter against original DEX.

Fixture state models native side effects; a passing test does not certify that
libnx Activity/Window/Intent/AppInfo resources are wired in main.c.
"""
from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"
SOURCES = (
    "unity_java_preinit.c",
    "unity_apk_dex_loader.c",
    "unity_dex_vm.c",
    "unity_dex_android_context_host.c",
    "unity_dex_android_surface_host.c",
    "unity_dex_android_text_input_host.c",
    "unity_dex_android_host.c",
    "unity_dex_horizon_activity_host.c",
)


def main() -> None:
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host C compiler unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-horizon-activity-") as temp:
        exe = Path(temp) / ("host.exe" if os.name == "nt" else "host")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             *(str(ROOT / "source" / item) for item in SOURCES),
             str(ROOT / "tests/test_unity_dex_horizon_activity_host.c"),
             "-o", str(exe)],
            check=True, cwd=ROOT,
        )
        subprocess.run([str(exe), str(APK)], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
