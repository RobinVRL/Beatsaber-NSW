"""Compile strict Context bridge and exercise original Quest DEX-backed owners."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import zipfile
import zlib

from test_unity_dex_horizon_java_resources import ROOT, APK, SOURCES


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host clang/gcc compiler unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-framework-context-") as folder:
        temp = Path(folder)
        paths = [temp / name for name in (
            "native", "private", "files", "obb", "external", "assets"
        )]
        for path in paths:
            path.mkdir()
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        for name in ("libmain.so", "libunity.so"):
            (paths[0] / name).write_bytes(elf)
        with zipfile.ZipFile(APK) as apk:
            asset = apk.read("assets/bin/Data/boot.config")
            assert len(asset) == 768 and zlib.crc32(asset) == 0x396A5204
        boot = paths[5] / "bin" / "Data" / "boot.config"
        boot.parent.mkdir(parents=True)
        boot.write_bytes(asset)
        binary = temp / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             *(str(ROOT / "source" / src) for src in SOURCES),
             str(ROOT / "source/unity_dex_android_framework_context.c"),
             str(ROOT / "tests/test_unity_dex_android_framework_context.c"),
             "-o", str(binary)], check=True, cwd=ROOT,
        )
        subprocess.run(
            [str(binary), str(APK), *(str(path) for path in paths)],
            check=True, cwd=ROOT,
        )


if __name__ == "__main__":
    main()
