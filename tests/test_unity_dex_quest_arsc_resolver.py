"""Read exact stored original Quest ARSC and run C parser failure regressions."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"

def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host clang/gcc compiler unavailable")
    assert APK.exists()
    with zipfile.ZipFile(APK) as archive:
        dex = archive.read("classes.dex")
        # Confirm selected cases are actual resource IDs in the ORIGINAL
        # compiled guest DEX, rather than examples from a hand-built table.
        for resource_id in (
            0x7F0D0021, 0x7F0D0005, 0x7F040001,
            0x7F040000, 0x7F090000,
        ):
            assert struct.pack("<I", resource_id) in dex
    with tempfile.TemporaryDirectory(prefix="bs-quest-arsc-") as folder:
        output = Path(folder) / ("arsc.exe" if os.name == "nt" else "arsc")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             str(ROOT / "source/unity_apk_stored_entry.c"),
             str(ROOT / "source/unity_dex_quest_arsc_resolver.c"),
             str(ROOT / "tests/test_unity_dex_quest_arsc_resolver.c"),
             "-o", str(output)], check=True, cwd=ROOT)
        subprocess.run([str(output), str(APK)], check=True, cwd=ROOT)

if __name__ == "__main__":
    main()
