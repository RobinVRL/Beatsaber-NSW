"""Original Quest DEX-backed bounded Handler/ALooper constructor regression."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"

def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host C compiler required")
    with zipfile.ZipFile(APK) as apk:
        dex = apk.read("classes.dex")
    with tempfile.TemporaryDirectory(prefix="bs-handler-") as folder:
        target = Path(folder)
        source = target / "classes.dex"
        source.write_bytes(dex)
        binary = target / ("handler.exe" if os.name == "nt" else "handler")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source"),
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "source/unity_dex_horizon_main_looper.c"),
            str(ROOT / "source/unity_dex_horizon_java_handler.c"),
            str(ROOT / "tests/test_unity_dex_horizon_java_handler.c"),
            "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary), str(source)], check=True)

if __name__ == "__main__":
    main()
