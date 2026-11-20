"""Host regression for isolated fail-closed OpenXR runtime discovery.

    py -3 tests/test_openxr_runtime_discovery.py

No guest code, Android broker, or Vulkan/OpenXR driver is called.
"""

from __future__ import annotations

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from openxr_runtime_discovery import (  # noqa: E402
    DEFAULT_NEGOTIATION, discover, exported_dynamic_symbols, inspect_manifest)


ORIGINAL_LOADER = (ROOT / "import/quest-engine/APK/lib/arm64-v8a/"
                   "libopenxr_loader.so")


def make_arm64_elf(path: Path, symbol: str, *, defined: bool = True,
                   machine: int = 183) -> None:
    """Make an inert ELF64 fixture with only a dynamic symbol table."""
    strings = b"\0" + symbol.encode("ascii") + b"\0"
    blob = bytearray(0x400)
    blob[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", blob, 16,
                     3, machine, 1, 0, 0, 0x200, 0, 64, 56, 0, 64, 3, 0)
    blob[0x100:0x100 + len(strings)] = strings
    struct.pack_into("<IBBHQQ", blob, 0x150 + 24,
                     1, 0x12, 0, 1 if defined else 0,
                     0x1000 if defined else 0, 4)
    # SHT_STRTAB, then SHT_DYNSYM linked to that string table.
    struct.pack_into("<IIQQQQIIQQ", blob, 0x200 + 64,
                     0, 3, 0, 0, 0x100, len(strings), 0, 0, 1, 0)
    struct.pack_into("<IIQQQQIIQQ", blob, 0x200 + 128,
                     0, 11, 0, 0, 0x150, 48, 1, 0, 8, 24)
    path.write_bytes(blob)


def make_manifest(folder: Path, library_path: str, *,
                  version: str = "1.0.0", functions: dict | None = None) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    runtime = {"name": "inert_test_fixture", "library_path": library_path}
    if functions is not None:
        runtime["functions"] = functions
    manifest = folder / "active_runtime.aarch64.json"
    manifest.write_text(
        json.dumps({"file_format_version": version, "runtime": runtime}),
        encoding="utf-8")
    return manifest


class RuntimeDiscoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_no_manifest_is_never_ready(self) -> None:
        (self.root / "UnitySubsystemsManifest.json").write_text("{}", encoding="utf-8")
        result = discover([self.root])
        self.assertEqual(result["status"], "NO_RUNTIME_MANIFEST")
        self.assertEqual(result["findings"], [])
        self.assertFalse(result["pregraphics_runtime_ready"])
        self.assertFalse(result["loader_runtime_negotiated"])
        self.assertEqual(result["android_runtime_broker"],
                         "not_queryable_by_offline_file_scanner")

    def test_nonexistent_scan_root_is_reported_as_error(self) -> None:
        report = discover([self.root / "absent"])
        self.assertEqual(report["status"], "SCAN_ERROR")
        self.assertTrue(report["scan_errors"])
        self.assertFalse(report["pregraphics_runtime_ready"])

    def test_missing_or_malformed_manifest_fails_closed(self) -> None:
        self.assertEqual(discover([self.root])["status"], "NO_RUNTIME_MANIFEST")
        manifest = self.root / "active_runtime.json"
        manifest.write_text("{broken", encoding="utf-8")
        result = discover([self.root])
        self.assertEqual(result["status"], "MANIFEST_INVALID")
        self.assertEqual(result["findings"][0]["status"], "invalid")
        manifest.write_text('{"file_format_version":"1.0.0"}', encoding="utf-8")
        self.assertIn("runtime object", discover([self.root])["findings"][0]["detail"])

    def test_incorrect_version_and_missing_library_fail_closed(self) -> None:
        manifest = make_manifest(self.root, "./no-runtime.so", version="2.0.0")
        self.assertIn("file_format_version",
                      inspect_manifest(manifest, [self.root]).detail)
        make_manifest(self.root, "./no-runtime.so")
        self.assertIn("absent", inspect_manifest(manifest, [self.root]).detail)

    def test_wrong_architecture_and_undefined_symbol_fail_closed(self) -> None:
        library = self.root / "runtime.so"
        manifest = make_manifest(self.root, "./runtime.so")
        make_arm64_elf(library, DEFAULT_NEGOTIATION, machine=62)
        self.assertIn("AArch64", inspect_manifest(manifest, [self.root]).detail)
        make_arm64_elf(library, DEFAULT_NEGOTIATION, defined=False)
        self.assertIn("does not export",
                      inspect_manifest(manifest, [self.root]).detail)

    def test_valid_elf_export_only_produces_unnegotiated_candidate(self) -> None:
        library = self.root / "runtime.so"
        manifest = make_manifest(self.root, "./runtime.so")
        make_arm64_elf(library, DEFAULT_NEGOTIATION)
        self.assertIn(DEFAULT_NEGOTIATION, exported_dynamic_symbols(library))
        finding = inspect_manifest(manifest, [self.root])
        self.assertEqual(finding.status, "candidate_only")
        self.assertEqual(finding.negotiation_symbol, DEFAULT_NEGOTIATION)
        result = discover([self.root])
        self.assertEqual(result["status"], "CANDIDATE_NOT_NEGOTIATED")
        self.assertFalse(result["pregraphics_runtime_ready"])
        self.assertFalse(result["xr_instance_created"])
        self.assertFalse(result["xr_session_created"])
        self.assertFalse(result["xr_graphics_requirements_verified"])

    def test_remapped_negotiation_symbol_validates_real_export(self) -> None:
        library = self.root / "runtime.so"
        make_arm64_elf(library, "vendorNegotiate")
        manifest = make_manifest(
            self.root, "./runtime.so",
            functions={DEFAULT_NEGOTIATION: "vendorNegotiate"})
        self.assertEqual(inspect_manifest(manifest, [self.root]).status,
                         "candidate_only")
        make_manifest(self.root, "./runtime.so")
        self.assertEqual(inspect_manifest(manifest, [self.root]).status, "invalid")

    def test_path_escape_refused_even_with_valid_library(self) -> None:
        outside = self.root / "outside"
        outside.mkdir()
        make_arm64_elf(outside / "runtime.so", DEFAULT_NEGOTIATION)
        inside = self.root / "inside"
        manifest = make_manifest(inside, "../outside/runtime.so")
        finding = inspect_manifest(manifest, [inside])
        self.assertEqual(finding.status, "invalid")
        self.assertIn("escapes", finding.detail)

    def test_android_absolute_path_uses_only_explicit_mirror(self) -> None:
        mirror = self.root / "android"
        location = mirror / "vendor/lib64"
        location.mkdir(parents=True)
        make_arm64_elf(location / "runtime.so", DEFAULT_NEGOTIATION)
        scan = self.root / "scan"
        manifest = make_manifest(scan, "/vendor/lib64/runtime.so")
        self.assertEqual(inspect_manifest(manifest, [scan]).status, "invalid")
        self.assertEqual(inspect_manifest(manifest, [scan, mirror],
                                          android_root=mirror).status, "candidate_only")

    def test_original_quest_loader_is_not_an_openxr_runtime(self) -> None:
        self.assertTrue(ORIGINAL_LOADER.is_file())
        exports = exported_dynamic_symbols(ORIGINAL_LOADER)
        self.assertIn("xrGetInstanceProcAddr", exports)
        self.assertIn("xrCreateInstance", exports)
        self.assertNotIn(DEFAULT_NEGOTIATION, exports)
        # A manifest pointing at a copy of the REAL Quest loader is invalid
        # because it lacks the runtime negotiation entry point.
        runtime = self.root / "libopenxr_loader.so"
        runtime.write_bytes(ORIGINAL_LOADER.read_bytes())
        manifest = make_manifest(self.root, "./libopenxr_loader.so")
        finding = inspect_manifest(manifest, [self.root])
        self.assertEqual(finding.status, "invalid")
        self.assertIn("does not export", finding.detail)

    def test_command_line_reports_unready_with_nonzero_exit(self) -> None:
        cli = ROOT / "tools/openxr_runtime_discovery.py"
        completed = subprocess.run(
            [sys.executable, str(cli), "--root", str(self.root)],
            text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertFalse(report["pregraphics_runtime_ready"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
