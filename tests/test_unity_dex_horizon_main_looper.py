"""Host regression of the guarded Horizon main-thread Handler work queue."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host C compiler required")
    with tempfile.TemporaryDirectory(prefix="bs-native-looper-") as folder:
        exe = Path(folder) / ("looper.exe" if os.name == "nt" else "looper")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source"),
            str(ROOT / "source/unity_dex_horizon_main_looper.c"),
            str(ROOT / "tests/test_unity_dex_horizon_main_looper.c"),
            "-o", str(exe)
        ], check=True, cwd=ROOT)
        subprocess.run([str(exe)], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
