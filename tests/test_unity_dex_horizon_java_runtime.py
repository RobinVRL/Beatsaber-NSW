"""Host-test narrow libnx Activity base emulation against real pinned Quest DEX."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"
SOURCES = (
    "unity_java_preinit.c",
    "unity_apk_dex_loader.c",
    "unity_dex_vm.c",
    "unity_dex_android_context_host.c",
    "unity_dex_android_surface_host.c",
    "unity_dex_android_text_input_host.c",
    "unity_dex_android_host.c",
    "unity_dex_horizon_activity_host.c",
    "unity_dex_horizon_java_runtime.c",
)


def main():
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.exists() else None
    if not cc:
        raise SystemExit("No host C compiler available")
    assert APK.is_file(), str(APK)
    with tempfile.TemporaryDirectory(prefix="bs-native-activity-") as out:
        binary = Path(out) / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             *(str(ROOT / "source" / f) for f in SOURCES),
             str(ROOT / "tests" / "test_unity_dex_horizon_java_runtime.c"),
             "-o", str(binary)],
            cwd=ROOT, check=True,
        )
        subprocess.run([str(binary), str(APK)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
