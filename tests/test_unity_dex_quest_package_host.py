"""Host regression for the original Quest Android package/Intent data model."""
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
        raise RuntimeError("Host clang/gcc compiler unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-quest-package-") as folder:
        temp = Path(folder)
        binary = temp / ("test.exe" if os.name == "nt" else "test")
        staged_libs = temp / "native"
        staged_libs.mkdir()
        staged_data = temp / "private"
        staged_data.mkdir()
        # Header-only fixtures confirm that the production path gate rejects
        # arbitrary files. ELF identity and imports are pinned elsewhere by
        # the Quest original library loader, not by the package service.
        elf64_arm64 = bytearray(64)
        elf64_arm64[0:6] = b"\x7fELF\x02\x01"
        elf64_arm64[18:20] = b"\xb7\x00"
        for filename in ("libmain.so", "libunity.so"):
            (staged_libs / filename).write_bytes(elf64_arm64)

        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             str(ROOT / "source/unity_apk_stored_entry.c"),
             str(ROOT / "source/unity_apk_manifest.c"),
             str(ROOT / "source/unity_java_preinit.c"),
             str(ROOT / "source/unity_apk_dex_loader.c"),
             str(ROOT / "source/unity_dex_quest_package_host.c"),
             str(ROOT / "tests/test_unity_dex_quest_package_host.c"),
             "-o", str(binary)],
            check=True, cwd=ROOT,
        )
        subprocess.run(
            [str(binary), str(APK), str(staged_libs), str(staged_data)],
            check=True, cwd=ROOT,
        )


if __name__ == "__main__":
    main()
