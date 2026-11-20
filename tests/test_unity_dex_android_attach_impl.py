"""Build the guest Android attach bridge with strict clang diagnostics.

The full APK-backed owner/resource/DEX tests are separate; this test focuses
on the bridge's failure paths and token lifecycle with injected Java fixtures.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("clang/gcc unavailable")
    with tempfile.TemporaryDirectory(prefix="bs-guest-attach-") as folder:
        binary = Path(folder) / ("test.exe" if os.name == "nt" else "test")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source"),
            str(ROOT / "source/unity_dex_android_attach_impl.c"),
            str(ROOT / "tests/test_unity_dex_android_attach_impl.c"),
            "-o", str(binary),
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
