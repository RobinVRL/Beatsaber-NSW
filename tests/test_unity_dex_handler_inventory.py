"""Audit the exact Quest 1.46.0 DEX method refs needed by Handler dispatch.

The inventory proves which Java calls occur in the source APK. It never
claims that Android platform methods or Runnable.run are executable in NRO.
"""
from __future__ import annotations

import struct
import zipfile

from test_original_unity_java_dex import APK, Dex, u32


def prototype(dex: Dex, proto_index: int) -> str:
    off = u32(dex.data, 0x4C) + proto_index * 12
    result_index, args_offset = struct.unpack_from("<II", dex.data, off + 4)
    args = ""
    if args_offset:
        count = u32(dex.data, args_offset)
        indices = struct.unpack_from(f"<{count}H", dex.data, args_offset + 4)
        args = "".join(dex.types[index] for index in indices)
    return f"({args}){dex.types[result_index]}"


EXPECTED = {
    ("Landroid/os/Handler;", "<init>", "()V"),
    ("Landroid/os/Handler;", "post", "(Ljava/lang/Runnable;)Z"),
    ("Landroid/os/Handler;", "postDelayed", "(Ljava/lang/Runnable;J)Z"),
    ("Landroid/os/Handler;", "removeCallbacks", "(Ljava/lang/Runnable;)V"),
    ("Landroid/os/Handler;", "getLooper", "()Landroid/os/Looper;"),
    ("Landroid/os/Looper;", "getMainLooper", "()Landroid/os/Looper;"),
    ("Landroid/os/Looper;", "getThread", "()Ljava/lang/Thread;"),
    ("Ljava/lang/Runnable;", "run", "()V"),
}


def main() -> None:
    with zipfile.ZipFile(APK) as archive:
        dex_files = [name for name in archive.namelist()
                     if name.startswith("classes") and name.endswith(".dex")]
        inventory = set()
        for name in dex_files:
            dex = Dex(archive.read(name))
            inventory.update((cls, method, prototype(dex, proto))
                             for cls, proto, method in dex.methods)
    absent = EXPECTED - inventory
    assert not absent, f"Missing original Quest DEX method refs: {sorted(absent)}"
    for declaring, method, signature in sorted(EXPECTED):
        print(f"{declaring}->{method}{signature}")
    print("PASS: exact original Quest DEX Handler/Looper/Runnable method refs")


if __name__ == "__main__":
    main()
