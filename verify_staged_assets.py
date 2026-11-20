#!/usr/bin/env python3
"""Validate the owner's private Switch Beat Saber stage against APK/OBB ZIP CRCs.

Only file sizes, CRC32 results, and the homebrew header are examined. Game
media, original binaries, and content are not written into reports or logs.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import zipfile
import zlib


def check_stage(project: pathlib.Path, staged: pathlib.Path) -> int:
    source = project / "SwitchPort"
    inputs = (
        (source / "beat-saber.apk", "apk"),
        (source / "main.3491.com.beatgames.beatsaber.obb", "obb"),
    )
    bad: list[str] = []
    checked = 0
    total_bytes = 0

    for archive_path, kind in inputs:
        if not archive_path.is_file():
            print(f"Missing original {kind} file: {archive_path}")
            return 2
        with zipfile.ZipFile(archive_path) as archive:
            for info in archive.infolist():
                name = info.filename
                if kind == "apk" and re.fullmatch(r"lib/arm64-v8a/[^/]+\.so", name):
                    relative = pathlib.Path(name.rsplit("/", 1)[-1])
                elif name.startswith("assets/") and not name.endswith("/"):
                    relative = pathlib.Path(*name.split("/"))
                else:
                    continue
                candidate = staged / relative
                if not candidate.is_file():
                    bad.append(f"missing {kind}:{name}")
                    continue
                expected_size = info.file_size
                if candidate.stat().st_size != expected_size:
                    bad.append(f"size mismatch {kind}:{name}")
                    continue
                checksum = 0
                with candidate.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        checksum = zlib.crc32(chunk, checksum)
                if (checksum & 0xFFFFFFFF) != info.CRC:
                    bad.append(f"CRC32 mismatch {kind}:{name}")
                else:
                    checked += 1
                    total_bytes += expected_size

    nro = staged / "BeatSaberQuestLoader.nro"
    expected_nro = project / "BeatSaberQuestLoader.nro"
    if not nro.is_file() or not expected_nro.is_file():
        bad.append("missing compiled or staged NRO")
    else:
        with nro.open("rb") as stream:
            stream.seek(16)
            magic = stream.read(4)
        if magic != b"NRO0":
            bad.append("staged homebrew has invalid NRO0 magic")
        if nro.read_bytes() != expected_nro.read_bytes():
            bad.append("staged NRO differs from the current build")

    print(f"Checked {checked} owned game files ({total_bytes:,} bytes) against original ZIP CRC32.")
    if bad:
        print(f"FAILED: {len(bad)} mismatch(es).")
        for line in bad[:25]:
            print(" -", line)
        return 1
    print("PASS: original game assets and ARM64 libraries match supplied APK/OBB; NRO0 header valid.")
    print("Runtime functionality, audio and VR behavior are not tested by this file integrity check.")
    return 0


def main() -> int:
    project = pathlib.Path(__file__).resolve().parent
    default = project / "private-sd" / "switch" / "beat-saber-quest"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, default=default)
    args = parser.parse_args()
    if not args.stage.is_dir():
        print(f"Staged directory missing: {args.stage}")
        return 2
    return check_stage(project, args.stage)


if __name__ == "__main__":
    sys.exit(main())
