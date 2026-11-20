"""Strict guest-facing Resources method tests with the original Quest ARSC."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"


def main():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        compiler = str(fallback) if fallback.is_file() else None
    if not compiler:
        raise RuntimeError("clang/gcc unavailable")
    assert APK.is_file(), APK
    with tempfile.TemporaryDirectory(prefix="bs-attach-runtime-") as folder:
        binary = Path(folder) / ("test.exe" if os.name == "nt" else "test")
        sources = [
            "source/unity_dex_horizon_java_attach_runtime.c",
            "source/unity_dex_quest_arsc_resolver.c",
            "source/unity_apk_stored_entry.c",
            "tests/test_unity_dex_horizon_java_attach_runtime.c",
        ]
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source"),
            *(str(ROOT / source) for source in sources),
            "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary), str(APK)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
