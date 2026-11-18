"""Host test of actual Quest binary AndroidManifest.xml parser."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        if fallback.exists():
            cc = str(fallback)
    if not cc:
        raise RuntimeError("Clang/gcc host compiler required")
    with tempfile.TemporaryDirectory(prefix="bs-apk-manifest-") as folder:
        binary = Path(folder) / ("manifest.exe" if os.name == "nt" else "manifest")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I",
            str(ROOT / "source"),
            str(ROOT / "source/unity_apk_stored_entry.c"),
            str(ROOT / "source/unity_apk_manifest.c"),
            str(ROOT / "tests/test_unity_apk_manifest.c"),
            "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary), str(APK)], check=True)


if __name__ == "__main__":
    main()
