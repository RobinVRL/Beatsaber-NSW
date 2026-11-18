"""Exercise the fail-closed original-DEX Android Context/Intent host adapter.

Host fixture values are observed-object stand-ins strictly for unit tests;
passing them does not certify Android Activity construction in Eden/Switch.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"


def compiler() -> str:
    if os.getenv("CC"):
        return os.environ["CC"]
    available = shutil.which("clang") or shutil.which("gcc")
    if available:
        return available
    fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("C host compiler unavailable (set CC)")


def main() -> None:
    with zipfile.ZipFile(APK) as original:
        assert original.getinfo("classes.dex").file_size == 6315268
    with tempfile.TemporaryDirectory(prefix="bs-dex-context-") as name:
        temporary = Path(name)
        binary = temporary / ("test-context.exe" if os.name == "nt" else
                              "test-context")
        command = [
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Isource",
            str(ROOT / "source/unity_apk_dex_loader.c"),
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "source/unity_dex_android_context_host.c"),
            str(ROOT / "tests/test_unity_dex_android_context_host.c"),
            "-o", str(binary),
        ]
        subprocess.run(command, check=True, cwd=ROOT)
        # The native ZIP reader owns the APK path; the real DEX VM in its
        # fixture is initialized from exactly this owner's original archive.
        subprocess.run([str(binary), str(APK)], check=True, cwd=ROOT)
    print("PASS: DEX Android Context/Intent live-host gating and observed values")


if __name__ == "__main__":
    main()
