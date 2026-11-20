"""Exercise original Quest DEX through the new Horizon SurfaceView registry.

Native applet, child and JNI operations in this host test are explicitly fake.
The production service still refuses the Java SurfaceView constructor until
an independently attached Android View and child NWindow backend exists.
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
    with tempfile.TemporaryDirectory(prefix="bs-horizon-surfaceview-") as tmp:
        folder = pathlib.Path(tmp)
        dex_file = folder / "classes.dex"
        dex_file.write_bytes(dex)
        binary = folder / "horizon-surfaceview-test.exe"
        subprocess.run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "source/unity_dex_vm.c"),
            str(ROOT / "source/unity_dex_android_surface_host.c"),
            str(ROOT / "source/unity_dex_horizon_surface_view_backend.c"),
            str(ROOT / "tests/test_unity_dex_horizon_surface_view_backend.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), str(dex_file)], check=True)


if __name__ == "__main__":
    main()
