"""Execute a bounded prefix of the ORIGINAL Quest DEX with the C interpreter.

No Android VM is available to these host tests. The first unavailable genuine
platform service must stop execution; a mocked Activity token is only a test
fixture for verifying bytecode dispatch and precise stop locations.
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
    if os.getenv("CC"):
        return os.environ["CC"]
    executable = shutil.which("clang") or shutil.which("gcc")
    if executable:
        return executable
    fallback = pathlib.Path.home() / "emsdk/upstream/bin/clang.exe"
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("C host compiler unavailable (set CC)")


def main() -> None:
    with zipfile.ZipFile(APK) as apk:
        dex = apk.read("classes.dex")
    with tempfile.TemporaryDirectory(prefix="bs-actual-dex-vm-") as folder:
        destination = pathlib.Path(folder)
        classes = destination / "classes.dex"
        classes.write_bytes(dex)
        binary = destination / ("test-dex-vm.exe" if os.name == "nt" else
                                "test-dex-vm")
        subprocess.run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "tests/test_unity_dex_vm.c"),
            "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary), str(classes)], check=True)


if __name__ == "__main__":
    main()
