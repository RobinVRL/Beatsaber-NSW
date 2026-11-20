"""Original APK + deterministic Horizon native attachment boundary regression."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT / "SwitchPort" / "beat-saber.apk"
SOURCES = (
    "unity_java_preinit.c",
    "unity_apk_dex_loader.c",
    "unity_apk_stored_entry.c",
    "unity_apk_manifest.c",
    "unity_dex_vm.c",
    "unity_dex_android_context_host.c",
    "unity_dex_android_surface_host.c",
    "unity_dex_android_text_input_host.c",
    "unity_dex_android_host.c",
    "unity_dex_horizon_activity_host.c",
    "unity_dex_horizon_java_runtime.c",
    "unity_dex_quest_package_host.c",
    "unity_dex_horizon_context_objects.c",
    "unity_dex_horizon_main_looper.c",
    "unity_dex_horizon_android_attachment.c",
)


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host compiler unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-horizon-attach-") as folder:
        temp = Path(folder)
        dirs = [temp / name for name in (
            "native", "private", "files", "obb", "external-files"
        )]
        for directory in dirs:
            directory.mkdir()
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        for filename in ("libmain.so", "libunity.so"):
            (dirs[0] / filename).write_bytes(elf)
        binary = temp / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             *(str(ROOT / "source" / src) for src in SOURCES),
             str(ROOT / "tests/test_unity_dex_horizon_android_attachment.c"),
             "-o", str(binary)],
            check=True, cwd=ROOT
        )
        subprocess.run(
            [str(binary), str(APK), *(str(item) for item in dirs)],
            check=True, cwd=ROOT
        )


if __name__ == "__main__":
    main()
