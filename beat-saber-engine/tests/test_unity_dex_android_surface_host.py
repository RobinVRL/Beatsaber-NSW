"""Compile the independent SurfaceView host against the original Quest DEX.

No JNI bridge replacements, Unity VM modifications, Eden or Switch graphics
are linked. A deliberately fake NWindow only simulates externally observed
host service completions for the focused contract tests.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import zipfile

from test_unity_dex_vm import APK, ROOT, compiler


def main() -> None:
    with zipfile.ZipFile(APK) as original:
        dex = original.read("classes.dex")
    with tempfile.TemporaryDirectory(prefix="bs-quest-surface-host-") as tmp:
        dest = pathlib.Path(tmp)
        dex_path = dest / "classes.dex"
        dex_path.write_bytes(dex)
        executable = dest / "test-surface-host.exe"
        subprocess.run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "source/unity_dex_android_surface_host.c"),
            str(ROOT / "tests/test_unity_dex_android_surface_host.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), str(dex_path)], check=True)


if __name__ == "__main__":
    main()
