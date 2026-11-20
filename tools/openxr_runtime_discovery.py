"""Offline, fail-closed OpenXR runtime discovery for the Quest-to-Switch port.

This is a packaging/preflight utility, not a guest OpenXR implementation.
It never loads a library, calls negotiation, queries an Android ContentProvider,
or claims an OpenXR instance or session is available.

Usage:
    py -3 tools/openxr_runtime_discovery.py
    py -3 tools/openxr_runtime_discovery.py --root PATH [--root PATH ...]
    py -3 tools/openxr_runtime_discovery.py --root PATH --android-root PATH

The default scan is limited to the owning /1.40.8 project. Non-zero exit codes
are intentional until a real runtime is negotiated by the running guest.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path, PurePosixPath
import struct
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[3]
RUNTIME_FILENAMES = frozenset(("active_runtime.json", "active_runtime.aarch64.json"))
DEFAULT_NEGOTIATION = "xrNegotiateLoaderRuntimeInterface"
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_LIBRARY_BYTES = 128 * 1024 * 1024


@dataclass(frozen=True)
class Finding:
    manifest: str
    status: str
    detail: str
    library: str | None = None
    negotiation_symbol: str | None = None


def exported_dynamic_symbols(library: Path) -> set[str]:
    """Read ELF64 ARM64 dynamic exports without executing or mapping the ELF.

    A name merely present in .rodata or in an undefined dynamic import does
    not satisfy dlsym. A stripped ELF without a parseable SHT_DYNSYM fails
    conservatively. The staged Quest libraries retain their section headers.
    """
    if not library.is_file():
        raise ValueError("runtime shared library does not exist")
    if library.stat().st_size > MAX_LIBRARY_BYTES:
        raise ValueError("runtime shared library exceeds offline audit size limit")
    blob = library.read_bytes()
    if len(blob) < 64 or blob[:6] != b"\x7fELF\x02\x01":
        raise ValueError("not a little-endian ELF64 shared library")
    e_type, machine = struct.unpack_from("<HH", blob, 16)
    if e_type != 3 or machine != 183:
        raise ValueError("expected ET_DYN AArch64 guest runtime, not a host/other ELF")
    section_off = struct.unpack_from("<Q", blob, 40)[0]
    shentsize, count = struct.unpack_from("<HH", blob, 58)
    if not section_off or shentsize != 64 or not count:
        raise ValueError("ELF dynamic-symbol section table unavailable")
    if section_off > len(blob) or count > (len(blob) - section_off) // shentsize:
        raise ValueError("ELF section header table extends past file")
    sections = [struct.unpack_from("<IIQQQQIIQQ", blob, section_off + i * 64)
                for i in range(count)]
    exports: set[str] = set()
    found_dynsym = False
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM (not SHT_SYMTAB)
            continue
        found_dynsym = True
        off, size, link, entsize = section[4], section[5], section[6], section[9]
        if link >= count or entsize != 24 or size % 24 or off > len(blob) or size > len(blob) - off:
            raise ValueError("invalid ELF dynamic symbol section")
        strings = sections[link]
        if strings[1] != 3:  # SHT_STRTAB
            raise ValueError("ELF dynamic symbol string table is invalid")
        str_off, str_size = strings[4], strings[5]
        if str_off > len(blob) or str_size > len(blob) - str_off:
            raise ValueError("ELF dynamic string table exceeds file")
        strtab = blob[str_off:str_off + str_size]
        for i in range(size // 24):
            name_off, info, other, shndx, value, _ = struct.unpack_from(
                "<IBBHQQ", blob, off + i * 24)
            if not shndx or not value or (info >> 4) not in (1, 2):
                continue  # STN_UNDEF, local, or non-exported
            if (info & 15) not in (0, 2) or (other & 3) not in (0, 3):
                continue  # Not a function/not visible to dlsym
            if name_off >= len(strtab):
                raise ValueError("ELF dynamic symbol name is out of range")
            end = strtab.find(b"\0", name_off)
            if end < 0:
                raise ValueError("unterminated ELF dynamic symbol name")
            try:
                exports.add(strtab[name_off:end].decode("ascii"))
            except UnicodeDecodeError as exc:
                raise ValueError("non-ASCII ELF export name") from exc
    if not found_dynsym:
        raise ValueError("ELF has no dynamic export symbol section")
    return exports


def _contained(path: Path, roots: tuple[Path, ...]) -> bool:
    resolved = path.resolve()
    return any(resolved == root or root in resolved.parents for root in roots)


def _library_for_manifest(manifest: Path, name: str,
                          allowed_roots: tuple[Path, ...],
                          library_dirs: tuple[Path, ...],
                          android_root: Path | None) -> Path:
    """Resolve local library evidence; never search the host DLL loader path."""
    guest_path = PurePosixPath(name.replace("\\", "/"))
    if guest_path.is_absolute() and android_root is not None:
        candidate = android_root / str(guest_path).lstrip("/")
    elif guest_path.is_absolute() or Path(name).is_absolute():
        candidate = Path(name)
    elif "/" in name or "\\" in name:
        candidate = manifest.parent / name
    else:
        # Bare sonames normally use the Android dynamic loader search path.
        # Only explicit audit directories and the manifest's directory can
        # establish a local file without executing an Android linker.
        candidates = [manifest.parent / name]
        candidates.extend(directory / name for directory in library_dirs)
        candidate = next((path for path in candidates if path.is_file()), candidates[0])
    if not _contained(candidate, allowed_roots):
        raise ValueError("runtime library path escapes inspected roots")
    return candidate.resolve()


def inspect_manifest(manifest: Path, roots: Iterable[Path],
                     library_dirs: Iterable[Path] = (),
                     android_root: Path | None = None) -> Finding:
    allowed_roots = tuple(root.resolve() for root in roots)
    allowed_library_dirs = tuple(directory.resolve() for directory in library_dirs)
    canonical = manifest.resolve()
    manifest_name = str(manifest)

    def invalid(detail: str, library: str | None = None) -> Finding:
        return Finding(manifest_name, "invalid", detail, library)

    if not _contained(canonical, allowed_roots):
        return invalid("manifest path escapes inspected roots")
    try:
        if manifest.stat().st_size > MAX_MANIFEST_BYTES:
            return invalid("manifest exceeds offline audit size limit")
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return invalid(f"cannot read runtime manifest JSON: {exc}")
    if not isinstance(data, dict) or data.get("file_format_version") != "1.0.0":
        return invalid("expected OpenXR runtime manifest file_format_version 1.0.0")
    runtime = data.get("runtime")
    if not isinstance(runtime, dict):
        return invalid("runtime object is missing")
    library_name = runtime.get("library_path")
    if not isinstance(library_name, str) or not library_name.strip():
        return invalid("runtime.library_path is missing")
    if "\0" in library_name or not library_name.endswith(".so"):
        return invalid("runtime library_path must name an Android .so")
    functions = runtime.get("functions", {})
    if not isinstance(functions, dict):
        return invalid("runtime.functions must be a mapping")
    symbol = functions.get(DEFAULT_NEGOTIATION, DEFAULT_NEGOTIATION)
    if not isinstance(symbol, str) or not symbol or "\0" in symbol:
        return invalid("invalid runtime negotiation function remapping")
    try:
        library = _library_for_manifest(
            manifest, library_name, allowed_roots, allowed_library_dirs, android_root)
    except (OSError, ValueError) as exc:
        return invalid(str(exc))
    if not library.is_file():
        return invalid("referenced runtime library is absent", str(library))
    try:
        exports = exported_dynamic_symbols(library)
    except (OSError, ValueError, struct.error) as exc:
        return invalid(f"runtime library not validated: {exc}", str(library))
    if symbol not in exports:
        return invalid("AArch64 library does not export the required runtime negotiation function",
                       str(library))
    return Finding(
        manifest_name, "candidate_only",
        "ARM64 runtime negotiation export exists; loader negotiation, instance, "
        "session, device tracking and graphics support remain unverified",
        str(library), symbol)


def discover(roots: Iterable[Path], library_dirs: Iterable[Path] = (),
             android_root: Path | None = None) -> dict:
    allowed = tuple(root.resolve() for root in roots)
    permitted_lib_dirs = tuple(directory.resolve() for directory in library_dirs)
    if android_root is not None:
        allowed += (android_root.resolve(),)
    manifests: set[Path] = set()
    errors = []
    for root in allowed:
        if not root.is_dir():
            errors.append(f"scan root absent or not a directory: {root}")
            continue
        for candidate in root.rglob("active_runtime*.json"):
            if candidate.name in RUNTIME_FILENAMES:
                manifests.add(candidate)
    findings = [inspect_manifest(path, allowed, permitted_lib_dirs, android_root)
                for path in sorted(manifests)]
    return {
        "status": ("SCAN_ERROR" if errors else
                   "CANDIDATE_NOT_NEGOTIATED"
                   if any(item.status == "candidate_only" for item in findings)
                   else "MANIFEST_INVALID" if findings else "NO_RUNTIME_MANIFEST"),
        "roots_scanned": list(map(str, allowed)),
        "scan_errors": errors,
        "findings": [asdict(item) for item in findings],
        "android_runtime_broker": "not_queryable_by_offline_file_scanner",
        "loader_runtime_negotiated": False,
        "xr_instance_created": False,
        "xr_session_created": False,
        "xr_graphics_requirements_verified": False,
        "pregraphics_runtime_ready": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", action="append", type=Path,
                        help="Project tree to inspect; repeat for multiple trees")
    parser.add_argument("--library-dir", action="append", type=Path,
                        help="Explicit Android shared-library search directory")
    parser.add_argument("--android-root", type=Path,
                        help="Mirror of Android / for manifest paths like /vendor/lib64/...")
    args = parser.parse_args()
    roots = args.root if args.root is not None else [PROJECT_ROOT]
    report = discover(roots, args.library_dir or (), args.android_root)
    print(json.dumps(report, indent=2))
    return 2  # Offline inspection never proves a negotiated/initialized runtime.


if __name__ == "__main__":
    raise SystemExit(main())
