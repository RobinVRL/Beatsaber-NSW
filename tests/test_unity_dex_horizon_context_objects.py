"""Native-only Horizon Context dependencies using real original Quest APK."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"


def main() -> None:
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host C compiler unavailable")
    assert APK.exists()
    with tempfile.TemporaryDirectory(prefix="bs-native-context-") as folder:
        temp = Path(folder)
        native = temp / "native"
        private = temp / "private"
        files = temp / "files"
        obb = temp / "obb"
        external = temp / "external-files"
        for path in (native, private, files, obb, external):
            path.mkdir()
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        for name in ("libmain.so", "libunity.so"):
            (native / name).write_bytes(elf)
        output = temp / ("context.exe" if os.name == "nt" else "context")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I",
            str(ROOT / "source"),
            *(str(ROOT / "source" / item) for item in (
                "unity_apk_stored_entry.c",
                "unity_apk_manifest.c",
                "unity_java_preinit.c",
                "unity_apk_dex_loader.c",
                "unity_dex_quest_package_host.c",
                "unity_dex_horizon_context_objects.c",
            )),
            str(ROOT / "tests/test_unity_dex_horizon_context_objects.c"),
            "-o", str(output),
        ], check=True, cwd=ROOT)
        subprocess.run([
            str(output), str(APK), str(native), str(private),
            str(files), str(obb), str(external),
        ], check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
