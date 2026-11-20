"""Verify independent, non-Intent Horizon startup observations with Quest APK."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from test_unity_dex_horizon_context_objects import ROOT, APK


def main() -> None:
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        compiler = str(fallback) if fallback.is_file() else None
    if not compiler:
        raise RuntimeError("clang/gcc unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-horizon-startup-") as folder:
        temp = Path(folder)
        paths = [temp / directory for directory in (
            "native", "private", "files", "obb", "external-files"
        )]
        for path in paths:
            path.mkdir()
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        for name in ("libmain.so", "libunity.so"):
            (paths[0] / name).write_bytes(elf)
        binary = temp / ("startup.exe" if os.name == "nt" else "startup")
        source = [
            "unity_apk_stored_entry.c",
            "unity_apk_manifest.c",
            "unity_java_preinit.c",
            "unity_apk_dex_loader.c",
            "unity_dex_quest_package_host.c",
            "unity_dex_horizon_context_objects.c",
        ]
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source"),
            *(str(ROOT / "source" / name) for name in source),
            str(ROOT / "tests/test_unity_dex_horizon_context_startup.c"),
            "-o", str(binary)
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary), str(APK), *(str(p) for p in paths)],
                       check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
