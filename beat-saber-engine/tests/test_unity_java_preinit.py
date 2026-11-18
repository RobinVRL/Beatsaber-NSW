"""Compile the isolated Java preinit gate and verify it against the original APK.

No Java bytecode is executed: host callbacks are intentionally negative test
doubles. The genuine Java backend and constructor still need implementation.
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
APK = ROOT.parent / "SwitchPort" / "beat-saber.apk"


def compiler() -> str:
    preferred = os.environ.get("CC")
    if preferred:
        return preferred
    path = shutil.which("clang") or shutil.which("gcc")
    if path:
        return path
    fallback = pathlib.Path.home() / "emsdk/upstream/bin/clang.exe"
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError("Host clang/gcc unavailable; set CC to a host C compiler")


def main() -> None:
    with zipfile.ZipFile(APK) as archive:
        data = archive.read("classes.dex")
    assert len(data) == 6_315_268, "original Quest DEX size changed"
    assert data[0x238c0c:0x238c10] == bytes.fromhex("6907973f"), (
        "original UnityPlayer.currentActivity sput-object changed")
    assert data[0x238bf8:0x238bfc] == bytes.fromhex("6907983f"), (
        "original UnityPlayer.currentContext sput-object changed")
    assert data[0x2379ce:0x2379d4] == bytes.fromhex("7054c9644120"), (
        "UnityPlayerForGameActivity constructor dispatch changed")
    with tempfile.TemporaryDirectory(prefix="unity-java-preinit-") as tmp:
        temp = pathlib.Path(tmp)
        dex = temp / "classes.dex"
        dex.write_bytes(data)
        exe = temp / ("unity_java_preinit.exe" if os.name == "nt" else
                      "unity_java_preinit")
        subprocess.run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(ROOT / "source/unity_java_preinit.c"),
            str(ROOT / "tests/test_unity_java_preinit.c"),
            "-o", str(exe)
        ], check=True)
        subprocess.run([str(exe), str(dex)], check=True)
    print("PASS: original Quest classes.dex original-PC and host preinit gate")


if __name__ == "__main__":
    main()
