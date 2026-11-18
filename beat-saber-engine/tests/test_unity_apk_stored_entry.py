"""Validate bounded stored-entry reading against Quest and malformed ZIPs."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        if fallback.exists():
            cc = str(fallback)
    if not cc:
        raise RuntimeError("Clang/gcc host compiler required")
    with tempfile.TemporaryDirectory(prefix="bs-apk-entry-") as folder:
        temp = Path(folder)
        bad = temp / "bad_crc.zip"
        duplicate = temp / "duplicate.zip"
        binary = temp / ("test-zip.exe" if os.name == "nt" else "test-zip")
        with zipfile.ZipFile(bad, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("AndroidManifest.xml", b"HELLO")
        damaged = bytearray(bad.read_bytes())
        payload = damaged.find(b"HELLO")
        assert payload >= 0
        damaged[payload] ^= 0x41
        bad.write_bytes(damaged)
        with zipfile.ZipFile(duplicate, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr("AndroidManifest.xml", b"HELLO")
            archive.writestr("AndroidManifest.xml", b"WORLD")
        subprocess.run([
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I",
            str(ROOT / "source"),
            str(ROOT / "source/unity_apk_stored_entry.c"),
            str(ROOT / "tests/test_unity_apk_stored_entry.c"),
            "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary), str(APK), str(bad), str(duplicate)],
                       check=True)


if __name__ == "__main__":
    main()
