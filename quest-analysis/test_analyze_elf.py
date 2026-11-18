"""Synthetic ELF fixtures and optional checks against staged Quest libraries.

Run: py -3 -m unittest discover -s quest-analysis -p 'test_*.py' -v
"""

import json
import os
import struct
import tempfile
import unittest
from pathlib import Path

from analyze_elf import ELF, ELFError, analyze, as_text


def elf_fixture(*, soname, needed=(), imports=(), exports=(), use_gnu=False,
                versioned_import=None):
    """Create a tiny valid ET_DYN with PT_DYNAMIC, no section headers.

    The fixture is only a test artifact, generated in a temporary directory.
    """
    image = bytearray(8192)
    image[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 64, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 5, 0, 0x1000, 0, len(image), len(image), 4096)
    struct.pack_into("<IIQQQQQQ", image, 120,
                     2, 6, 0x100, 0x1100, 0, 512, 512, 8)
    strtab = bytearray(b"\x00")
    str_offsets = {}

    def addstr(value):
        if value not in str_offsets:
            str_offsets[value] = len(strtab)
            strtab.extend(value.encode("ascii") + b"\x00")
        return str_offsets[value]

    soname_str = addstr(soname)
    needed_strs = [addstr(value) for value in needed]
    for name in (*imports, *exports):
        addstr(name)
    if versioned_import:
        addstr(versioned_import[1])
        addstr(versioned_import[2])
    image[0x500:0x500 + len(strtab)] = strtab
    symbols = [(0, 0, 0, 0)]
    for name in imports:
        symbols.append((addstr(name), 0x12, 0, 0))
    for name in exports:
        symbols.append((addstr(name), 0x12, 0, 1))
    for i, (offset, info, other, section) in enumerate(symbols):
        struct.pack_into("<IBBHQQ", image, 0x900 + i * 24,
                         offset, info, other, section, 0, 0)
    dynamic = [
        (14, soname_str),
        (5, 0x1500),
        (10, len(strtab)),
        (6, 0x1900),
        (11, 24),
    ]
    dynamic.extend((1, value) for value in needed_strs)
    if use_gnu:
        dynamic.append((0x6FFFFEF5, 0x2100))
        struct.pack_into("<IIIIQ", image, 0x1100, 1, 1, 1, 5, 0)
        struct.pack_into("<I", image, 0x1118, 1 if len(symbols) > 1 else 0)
        for i in range(1, len(symbols)):
            struct.pack_into("<I", image, 0x111c + (i - 1) * 4,
                             1 if i == len(symbols) - 1 else 0)
    else:
        dynamic.append((4, 0x2100))
        struct.pack_into("<II", image, 0x1100, 1, len(symbols))
    if versioned_import:
        # The first defined symbol (index 1) is a versioned undefined import.
        assert imports and imports[0] == versioned_import[0]
        dynamic.extend(((0x6FFFFFF0, 0x2200),
                        (0x6FFFFFFE, 0x2300),
                        (0x6FFFFFFF, 1)))
        for i in range(len(symbols)):
            struct.pack_into("<H", image, 0x1200 + i * 2, 2 if i == 1 else 1)
        struct.pack_into("<HHIII", image, 0x1300,
                         1, 1, addstr(versioned_import[1]), 16, 0)
        struct.pack_into("<IHHII", image, 0x1310,
                         0, 0, 2, addstr(versioned_import[2]), 0)
        # Extend the string table after adding version metadata.
        struct.pack_into("<Q", image, 0x100 + 2 * 16 + 8, len(strtab))
        image[0x500:0x500 + len(strtab)] = strtab
    for i, (tag, value) in enumerate(dynamic):
        struct.pack_into("<qQ", image, 0x100 + i * 16, tag, value)
    struct.pack_into("<qQ", image, 0x100 + len(dynamic) * 16, 0, 0)
    return bytes(image)


class ELFAnalysisTests(unittest.TestCase):
    def test_sysv_hash_and_transitive_soname_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "provider-file.so").write_bytes(elf_fixture(
                soname="libprovider.so", exports=("beatmap_load",)))
            (root / "libmiddle.so").write_bytes(elf_fixture(
                soname="libmiddle.so", needed=("libprovider.so",)))
            (root / "libconsumer.so").write_bytes(elf_fixture(
                soname="libconsumer.so",
                needed=("libmiddle.so", "libandroid.so"),
                imports=("beatmap_load", "AAssetManager_fromJava", "unmatched")))
            report = analyze(root)
            c = next(lib for lib in report["libraries"] if lib["file"] == "libconsumer.so")
            imports = {entry["name"]: entry for entry in c["imports"]}
            self.assertEqual(imports["beatmap_load"]["status"], "resolved_staged")
            self.assertEqual(imports["beatmap_load"]["provider"], "provider-file.so")
            self.assertEqual(imports["AAssetManager_fromJava"]["status"],
                             "unresolved_android_api_candidate")
            self.assertEqual(imports["unmatched"]["status"], "unresolved_unknown_provider")
            self.assertEqual(c["missing_needed"], ["libandroid.so"])
            self.assertEqual(report["summary"]["library_count"], 3)
            self.assertNotIn(str(root), json.dumps(report))

    def test_gnu_hash_sectionless(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "libgnu.so")
            path.write_bytes(elf_fixture(soname="libgnu.so", imports=("needs",),
                                         exports=("offers",), use_gnu=True))
            lib = ELF(path)
            self.assertEqual(lib.symbol_count, 3)
            self.assertEqual([entry["name"] for entry in lib.imports], ["needs"])
            self.assertEqual([entry["name"] for entry in lib.exports], ["offers"])

    def test_versioned_missing_provider(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "libversioned.so")
            path.write_bytes(elf_fixture(
                soname="libversioned.so", needed=("libc.so",),
                imports=("__cxa_atexit",), versioned_import=(
                    "__cxa_atexit", "libc.so", "LIBC")))
            report = analyze(Path(tmp))
            item = report["libraries"][0]["imports"][0]
            self.assertEqual(item["version"], "LIBC")
            self.assertEqual(item["version_library"], "libc.so")
            self.assertEqual(item["status"], "unresolved_missing_version_provider")
            self.assertEqual(item["missing_provider"], "libc.so")

    def test_not_executing_or_copying_binary_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "libbad.so")
            path.write_bytes(b"\x7fELF")
            with self.assertRaises(ELFError):
                analyze(Path(tmp))
            path.write_bytes(elf_fixture(
                soname="libbad.so", exports=("marker",)))
            report = analyze(Path(tmp))
            text = as_text(report)
            self.assertIn("libbad.so", text)
            self.assertIn("marker", json.dumps(report))
            self.assertNotIn("\x7fELF", text)
            self.assertNotIn(str(tmp), text)

    def test_real_staged_libraries_if_available(self):
        root = (Path(__file__).resolve().parent.parent /
                "import/quest-engine/APK/lib/arm64-v8a")
        if not (root / "libunity.so").is_file():
            self.skipTest("Quest staged libraries not present")
        report = analyze(root)
        self.assertEqual(report["summary"]["library_count"], 15)
        libs = {item["file"]: item for item in report["libraries"]}
        self.assertIn("libandroid.so", libs["libunity.so"]["needed"])
        self.assertIn("libEGL.so", libs["libunity.so"]["missing_needed"])
        self.assertIn("libc.so", libs["libil2cpp.so"]["missing_needed"])
        self.assertTrue(any(
            item["name"] == "OPENSSL_init_ssl" and
            item["status"] == "resolved_staged"
            for item in libs["libenet.so"]["imports"]
        ) or any(
            item["provider"] == "libcrypto.so"
            for item in libs["libssl.so"]["imports"]
            if item["status"] == "resolved_staged"
        ))
        self.assertIn("libmediandk.so", report["summary"]["missing_android_sonames"])


if __name__ == "__main__":
    unittest.main()
