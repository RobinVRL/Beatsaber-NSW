"""Read actual original Quest Unity asset and test strict Resources provider."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"
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
    "unity_dex_horizon_java_resources.c",
    "unity_dex_quest_arsc_resolver.c",
)


def main():
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        cc = str(fallback) if fallback.is_file() else None
    if not cc:
        raise RuntimeError("Host clang/gcc compiler unavailable")
    assert APK.is_file()
    with tempfile.TemporaryDirectory(prefix="bs-java-resources-") as folder:
        temp = Path(folder)
        native = temp / "native"
        private = temp / "private"
        files = temp / "files"
        obb = temp / "obb"
        external = temp / "external-files"
        root_assets = temp / "assets"
        for directory in (native, private, files, obb, external, root_assets):
            directory.mkdir()
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = b"\xb7\x00"
        for filename in ("libmain.so", "libunity.so"):
            (native / filename).write_bytes(elf)
        with zipfile.ZipFile(APK) as apk:
            entry = apk.getinfo("assets/bin/Data/boot.config")
            payload = apk.read(entry)
            assert len(payload) == 768
            assert zlib.crc32(payload) == 0x396A5204
        outfile = root_assets / "bin" / "Data" / "boot.config"
        outfile.parent.mkdir(parents=True)
        outfile.write_bytes(payload)
        binary = temp / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             *(str(ROOT / "source" / x) for x in SOURCES),
             str(ROOT / "tests/test_unity_dex_horizon_java_resources.c"),
             "-o", str(binary)],
            cwd=ROOT, check=True,
        )
        subprocess.run(
            [str(binary), str(APK), str(native), str(private),
             str(files), str(obb), str(external), str(root_assets)],
            cwd=ROOT, check=True,
        )


if __name__ == "__main__":
    main()
