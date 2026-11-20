"""Compile original-DEX executable Java preinit adapter and run strict host test.

No fake Android service success is accepted. The expected result is an exact
missing original SurfaceView.<init> platform method after nine real opcodes.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"


def compiler() -> str:
    if os.environ.get("CC"):
        return os.environ["CC"]
    available = shutil.which("clang") or shutil.which("gcc")
    if available:
        return available
    fallback = pathlib.Path.home() / "emsdk/upstream/bin/clang.exe"
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("host C compiler unavailable (set CC)")


def main() -> None:
    with zipfile.ZipFile(APK) as archive:
        data = archive.read("classes.dex")
    with tempfile.TemporaryDirectory(prefix="bs-dex-exec-") as folder:
        destination = pathlib.Path(folder)
        dex = destination / "classes.dex"
        dex.write_bytes(data)
        binary = destination / ("test-executor.exe" if os.name == "nt" else
                                "test-executor")
        subprocess.run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "source/unity_dex_preinit_executor.c"),
            str(ROOT / "tests/test_unity_dex_preinit_executor.c"),
            "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary), str(dex)], check=True)


if __name__ == "__main__":
    main()
