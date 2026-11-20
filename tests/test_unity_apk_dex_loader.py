"""Host regression: extract EXACT original ZIP-contained Quest classes.dex."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_APK = ROOT / "SwitchPort" / "beat-saber.apk"


def main() -> None:
    cc = os.getenv("CC") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        fallback = Path.home() / "emsdk/upstream/bin/clang.exe"
        if fallback.exists():
            cc = str(fallback)
    if not cc:
        raise RuntimeError("Host C compiler unavailable; set CC.")
    assert ORIGINAL_APK.is_file(), "Original, locally-owned Quest APK absent"
    with tempfile.TemporaryDirectory(prefix="bs-quest-apk-test-") as temp:
        binary = Path(temp) / ("apk-dex.exe" if os.name == "nt" else "apk-dex")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "source"),
             str(ROOT / "source/unity_java_preinit.c"),
             str(ROOT / "source/unity_apk_dex_loader.c"),
             str(ROOT / "tests/test_unity_apk_dex_loader.c"),
             "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary), str(ORIGINAL_APK)], check=True)


if __name__ == "__main__":
    main()
