#!/usr/bin/env python3
"""Read-only, standard-library ELF64 AArch64 ABI/dependency analysis.

Only metadata is emitted. Library contents are never copied to a report.
Supports SHT_DYNSYM, SysV DT_HASH, and sectionless GNU DT_GNU_HASH ELFs.
"""

from __future__ import annotations

import argparse
import collections
import json
import mmap
import struct
import sys
from pathlib import Path


PT_LOAD, PT_DYNAMIC = 1, 2
SHT_DYNSYM = 11
DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB = 1, 2, 4, 5, 6
DT_STRSZ, DT_SYMENT, DT_SONAME = 10, 11, 14
DT_GNU_HASH = 0x6FFFFEF5
DT_VERSYM, DT_VERDEF, DT_VERDEFNUM = 0x6FFFFFF0, 0x6FFFFFFC, 0x6FFFFFFD
DT_VERNEED, DT_VERNEEDNUM = 0x6FFFFFFE, 0x6FFFFFFF
ANDROID_SONAMES = {
    "libandroid.so", "libbinder_ndk.so", "libc.so", "libdl.so",
    "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
    "libjnigraphics.so", "liblog.so", "libm.so", "libmediandk.so",
    "libnativewindow.so", "libOpenMAXAL.so", "libOpenSLES.so",
    "libstdc++.so", "libsync.so", "libvulkan.so", "libz.so",
}
API_PREFIXES = (
    "__android_", "__system_property_", "AAsset", "AChoreographer",
    "AConfiguration", "AInput", "AKeyEvent", "ALooper", "AMedia",
    "AMotionEvent", "ANative", "ASensor", "ATrace", "android_", "egl",
    "gl", "vk", "JNI_", "JavaVM_", "OpenSLES", "slCreateEngine",
)
API_EXACT = {"__errno", "__errno_location", "__get_tls", "getauxval"}


class ELFError(ValueError):
    """Malformed or unsupported ELF input."""


class ELF:
    def __init__(self, path: Path):
        self.path = path
        self.name = path.name
        self.warnings: list[str] = []
        self.needed: list[str] = []
        self.soname = path.name
        self.imports: list[dict] = []
        self.exports: list[dict] = []
        with path.open("rb") as source:
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
                self._parse(data)

    @staticmethod
    def unpack(data: mmap.mmap, fmt: str, offset: int) -> tuple:
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ELFError(f"record {fmt} outside file at 0x{offset:x}")
        return struct.unpack_from(fmt, data, offset)

    @staticmethod
    def string(data: mmap.mmap, offset: int, size: int) -> str:
        if offset < 0 or offset >= len(data) or size < 0:
            raise ELFError(f"string offset outside file at 0x{offset:x}")
        end = data.find(b"\0", offset, min(len(data), offset + size))
        if end == -1:
            raise ELFError(f"unterminated string at 0x{offset:x}")
        return data[offset:end].decode("utf-8", "replace")

    def _parse(self, data: mmap.mmap) -> None:
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ELFError("not an ELF file")
        if data[4] != 2 or data[5] != 1 or data[6] != 1:
            raise ELFError("requires ELF64 little-endian version 1")
        (etype, machine, _version, _entry, phoff, shoff, _flags,
         _ehsize, phentsize, phnum, shentsize, shnum, _shstrndx) = self.unpack(
            data, "<HHIQQQIHHHHHH", 16
        )
        if machine != 183:
            raise ELFError(f"requires AArch64 e_machine=183; found {machine}")
        if etype not in (2, 3):
            raise ELFError(f"requires ET_EXEC or ET_DYN; found {etype}")
        if phentsize < 56 or phnum == 0 or phnum > 8192:
            raise ELFError("invalid ELF program-header table")
        if shnum and shentsize < 64:
            raise ELFError("invalid ELF section-header table")
        self.elf_type = "ET_DYN" if etype == 3 else "ET_EXEC"
        self.machine = machine
        self.size_bytes = len(data)
        self.loads = []
        dynamics = []
        for i in range(phnum):
            p = self.unpack(data, "<IIQQQQQQ", phoff + i * phentsize)
            ptype, _pflags, offset, vaddr, _paddr, filesz, _memsz, _align = p
            if offset + filesz > len(data):
                raise ELFError("program segment extends beyond file")
            if ptype == PT_LOAD:
                self.loads.append((vaddr, vaddr + filesz, offset))
            if ptype == PT_DYNAMIC:
                for at in range(offset, offset + filesz, 16):
                    tag, value = self.unpack(data, "<qQ", at)
                    if tag == 0:
                        break
                    dynamics.append((tag, value))
        if not dynamics:
            raise ELFError("no PT_DYNAMIC found")
        dt = collections.defaultdict(list)
        for tag, value in dynamics:
            dt[tag].append(value)
        self.dynamic = dt
        dynstr = self._vaddr(data, self._one(DT_STRTAB))
        strsz = self._one(DT_STRSZ)
        if strsz <= 0 or dynstr + strsz > len(data):
            raise ELFError("invalid DT_STRTAB/DT_STRSZ")

        def getstr(index: int) -> str:
            if index >= strsz:
                raise ELFError("dynamic string offset exceeds DT_STRSZ")
            return self.string(data, dynstr + index, strsz - index)

        self.needed = [getstr(x) for x in dt[DT_NEEDED]]
        if dt[DT_SONAME]:
            self.soname = getstr(dt[DT_SONAME][0])
        sections = []
        if shoff and shnum:
            for i in range(shnum):
                s = self.unpack(data, "<IIQQQQIIQQ", shoff + i * shentsize)
                if s[4] + s[5] > len(data) and s[1] != 8:
                    raise ELFError("section exceeds file bounds")
                sections.append(s)
        dynsyms = [s for s in sections if s[1] == SHT_DYNSYM]
        syment = self._one(DT_SYMENT, default=24)
        if syment < 24:
            raise ELFError("invalid DT_SYMENT")
        symoff = self._vaddr(data, self._one(DT_SYMTAB))
        if dynsyms:
            symbol_count = dynsyms[0][5] // (dynsyms[0][9] or syment)
            if symoff != dynsyms[0][4]:
                self.warnings.append("DT_SYMTAB differs from SHT_DYNSYM offset")
        elif dt[DT_HASH]:
            hoff = self._vaddr(data, dt[DT_HASH][0])
            _buckets, symbol_count = self.unpack(data, "<II", hoff)
        elif dt[DT_GNU_HASH]:
            symbol_count = self._gnu_hash_count(data, self._vaddr(data, dt[DT_GNU_HASH][0]))
        else:
            raise ELFError("cannot determine dynamic symbol count: no SHT_DYNSYM/DT_HASH/DT_GNU_HASH")
        if symbol_count > 2_000_000 or symoff + symbol_count * syment > len(data):
            raise ELFError("dynamic symbol table exceeds file or safety limit")
        self.symbol_count = symbol_count
        versions = self._versions(data, dt, symbol_count, getstr)
        for i in range(symbol_count):
            name_offset, info, other, shndx, _value, _size = self.unpack(
                data, "<IBBHQQ", symoff + i * syment
            )
            if name_offset == 0:
                continue
            name = getstr(name_offset)
            binding = info >> 4
            if binding not in (1, 2, 10):
                continue
            visibility = other & 3
            version = versions.get(i, {})
            symbol = {
                "name": name,
                "binding": {1: "GLOBAL", 2: "WEAK", 10: "GNU_UNIQUE"}[binding],
                "kind": {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 6: "TLS", 10: "GNU_IFUNC"}.get(info & 15, f"TYPE_{info & 15}"),
            }
            if version.get("name"):
                symbol["version"] = version["name"]
            if version.get("library"):
                symbol["version_library"] = version["library"]
            if shndx == 0:
                self.imports.append(symbol)
            elif visibility in (0, 3):
                if version.get("hidden"):
                    symbol["nondefault_version"] = True
                self.exports.append(symbol)
        self.imports.sort(key=self._symbol_order)
        self.exports.sort(key=self._symbol_order)

    @staticmethod
    def _symbol_order(sym: dict) -> tuple:
        return (sym["name"], sym.get("version", ""), sym["binding"], sym["kind"])

    def _one(self, tag: int, default: int | None = None) -> int:
        values = self.dynamic[tag]
        if values:
            return values[0]
        if default is not None:
            return default
        raise ELFError(f"missing required dynamic tag {tag}")

    def _vaddr(self, data: mmap.mmap, vaddr: int) -> int:
        for first, last, fileoff in self.loads:
            if first <= vaddr < last:
                offset = fileoff + vaddr - first
                if offset < len(data):
                    return offset
        raise ELFError(f"virtual address 0x{vaddr:x} not mapped to file PT_LOAD")

    def _gnu_hash_count(self, data: mmap.mmap, off: int) -> int:
        nbuckets, symoffset, bloom_size, _shift = self.unpack(data, "<IIII", off)
        if nbuckets > 2_000_000 or bloom_size > 2_000_000:
            raise ELFError("malformed GNU hash header")
        buckets = off + 16 + bloom_size * 8
        chains = buckets + nbuckets * 4
        highest = symoffset
        for i in range(nbuckets):
            bucket = self.unpack(data, "<I", buckets + i * 4)[0]
            if bucket == 0:
                continue
            if bucket < symoffset:
                raise ELFError("GNU hash bucket below symoffset")
            index = bucket
            while True:
                if index > 2_000_000:
                    raise ELFError("GNU hash chain exceeds safety limit")
                chain = self.unpack(data, "<I", chains + (index - symoffset) * 4)[0]
                highest = max(highest, index + 1)
                if chain & 1:
                    break
                index += 1
        return highest

    def _versions(self, data: mmap.mmap, dt: dict, count: int, getstr) -> dict:
        vernames: dict[int, tuple[str, str | None]] = {}
        if dt[DT_VERNEED] and dt[DT_VERNEEDNUM]:
            base = self._vaddr(data, dt[DT_VERNEED][0])
            cursor = base
            for _ in range(min(dt[DT_VERNEEDNUM][0], 100_000)):
                _v, aux_count, filename, auxoff, nxt = self.unpack(data, "<HHIII", cursor)
                library = getstr(filename)
                aux = cursor + auxoff
                for __ in range(min(aux_count, 100_000)):
                    _hash, _flags, other, name, anxt = self.unpack(data, "<IHHII", aux)
                    vernames[other & 0x7FFF] = (getstr(name), library)
                    if not anxt:
                        break
                    aux += anxt
                if not nxt:
                    break
                cursor += nxt
        if dt[DT_VERDEF] and dt[DT_VERDEFNUM]:
            cursor = self._vaddr(data, dt[DT_VERDEF][0])
            for _ in range(min(dt[DT_VERDEFNUM][0], 100_000)):
                _v, _flags, index, _count, _hash, auxoff, nxt = self.unpack(
                    data, "<HHHHIII", cursor
                )
                name_offset, _next = self.unpack(data, "<II", cursor + auxoff)
                vernames[index] = (getstr(name_offset), None)
                if not nxt:
                    break
                cursor += nxt
        if not dt[DT_VERSYM]:
            return {}
        start = self._vaddr(data, dt[DT_VERSYM][0])
        result = {}
        for i in range(count):
            value = self.unpack(data, "<H", start + i * 2)[0]
            index = value & 0x7FFF
            if index > 1:
                sym = {"hidden": bool(value & 0x8000)}
                if index in vernames:
                    sym["name"], sym["library"] = vernames[index]
                else:
                    self.warnings.append(f"unknown GNU symbol-version index {index}")
                result[i] = sym
        return result


def analyze(directory: Path) -> dict:
    if not directory.is_dir():
        raise ELFError(f"input directory does not exist: {directory}")
    paths = sorted(directory.glob("*.so"), key=lambda p: p.name)
    if not paths:
        raise ELFError(f"no .so libraries found in {directory}")
    libs: dict[str, ELF] = {}
    for path in paths:
        try:
            libs[path.name] = ELF(path)
        except (ELFError, OSError) as exc:
            raise ELFError(f"{path.name}: {exc}") from exc
    aliases = {}
    for library in libs.values():
        for alias in (library.name, library.soname):
            if alias in aliases and aliases[alias] != library.name:
                raise ELFError(f"duplicate SONAME/filename alias {alias}")
            aliases[alias] = library.name

    output_libraries = []
    all_missing = set()
    totals = collections.Counter()
    for library in libs.values():
        # DT_NEEDED load order, breadth-first. Library in another package
        # is not automatically visible merely because it is in the folder.
        loaded = []
        seen = {library.name}
        queue = list(library.needed)
        missing = set()
        while queue:
            item = queue.pop(0)
            resolved = aliases.get(item)
            if resolved is None:
                missing.add(item)
            elif resolved not in seen:
                seen.add(resolved)
                loaded.append(resolved)
                queue.extend(libs[resolved].needed)
        all_missing.update(missing)
        providers: dict[str, list[tuple[str, dict]]] = collections.defaultdict(list)
        all_providers: dict[str, list[str]] = collections.defaultdict(list)
        for provider in libs.values():
            for symbol in provider.exports:
                all_providers[symbol["name"]].append(provider.name)
        for name in loaded:
            for symbol in libs[name].exports:
                providers[symbol["name"]].append((name, symbol))
        resolved_imports = []
        status_counts = collections.Counter()
        for sym in library.imports:
            entry = dict(sym)
            candidates = []
            for provider_name, exp in providers[sym["name"]]:
                if sym.get("version") and exp.get("version") != sym["version"]:
                    continue
                if (sym.get("version_library") and
                        sym["version_library"] not in (provider_name, libs[provider_name].soname)):
                    continue
                if not sym.get("version") and exp.get("nondefault_version"):
                    continue
                candidates.append(provider_name)
            if candidates:
                entry["status"] = "resolved_staged"
                entry["provider"] = candidates[0]
            elif sym["binding"] == "WEAK":
                entry["status"] = "unresolved_optional_weak"
            elif sym.get("version_library") in missing:
                # DT_VERNEED explicitly identifies the absent SONAME. This
                # is stronger evidence than classifying by symbol prefix.
                entry["status"] = "unresolved_missing_version_provider"
                entry["missing_provider"] = sym["version_library"]
            elif any(n not in seen for n in all_providers.get(sym["name"], [])):
                entry["status"] = "exported_outside_dependency_closure"
                entry["other_staged_providers"] = sorted(set(all_providers[sym["name"]]) - seen)
            elif (sym["name"].startswith(API_PREFIXES) or
                  sym["name"] in API_EXACT):
                entry["status"] = "unresolved_android_api_candidate"
            else:
                entry["status"] = "unresolved_unknown_provider"
            status_counts[entry["status"]] += 1
            resolved_imports.append(entry)
        totals.update(status_counts)
        output_libraries.append({
            "file": library.name, "soname": library.soname,
            "size_bytes": library.size_bytes,
            "elf_class": 64, "endianness": "little",
            "machine": "AArch64", "elf_type": library.elf_type,
            "needed": library.needed,
            "staged_dependency_closure": loaded,
            "missing_needed": sorted(missing),
            "missing_android_sonames": sorted(missing & ANDROID_SONAMES),
            "missing_other_sonames": sorted(missing - ANDROID_SONAMES),
            "dynsym_count": library.symbol_count,
            "imports": resolved_imports, "exports": library.exports,
            "import_status_counts": dict(sorted(status_counts.items())),
            "warnings": sorted(set(library.warnings)),
        })
    return {
        "schema_version": 1,
        "scope": "ELF64 little-endian AArch64 dynamic symbols and DT_NEEDED closure",
        "limits": [
            "Unresolved symbols cannot be assigned to a particular missing library without version metadata or its exports.",
            "Symbol visibility, runtime dlsym, JNI, Android system calls and loader relocations require runtime analysis.",
            "AArch64 architecture matches Switch CPU ISA but Android Bionic ELF cannot directly execute as libnx NRO.",
        ],
        "summary": {
            "library_count": len(libs),
            "import_count": sum(len(x.imports) for x in libs.values()),
            "export_count": sum(len(x.exports) for x in libs.values()),
            "import_status_counts": dict(sorted(totals.items())),
            "missing_needed": sorted(all_missing),
            "missing_android_sonames": sorted(all_missing & ANDROID_SONAMES),
            "missing_other_sonames": sorted(all_missing - ANDROID_SONAMES),
        },
        "libraries": output_libraries,
    }


def as_text(report: dict, sample_limit: int = 20) -> str:
    summary = report["summary"]
    lines = [
        "Quest ARM64 ELF ABI analysis (deterministic, read-only)",
        f"Libraries: {summary['library_count']}; imported symbols: {summary['import_count']}; exported symbols: {summary['export_count']}",
        "Cross-library imports: " + json.dumps(summary["import_status_counts"], sort_keys=True),
        "External DT_NEEDED: " + (", ".join(summary["missing_needed"]) or "(none)"),
        "Android platform SONAME blockers: " + (", ".join(summary["missing_android_sonames"]) or "(none)"),
    ]
    if summary["missing_other_sonames"]:
        lines.append("Other missing SONAMEs: " + ", ".join(summary["missing_other_sonames"]))
    for lib in report["libraries"]:
        lines.extend((
            "",
            f"{lib['file']} ({lib['size_bytes']} B, SONAME={lib['soname']})",
            "  DT_NEEDED: " + (", ".join(lib["needed"]) or "(none)"),
            "  Missing: " + (", ".join(lib["missing_needed"]) or "(none)"),
            "  Imported/exported: " + f"{len(lib['imports'])}/{len(lib['exports'])}",
            "  Resolution: " + json.dumps(lib["import_status_counts"], sort_keys=True),
        ))
        unresolved = [x for x in lib["imports"] if x["status"] not in
                      ("resolved_staged", "unresolved_optional_weak")]
        rank = {"unresolved_android_api_candidate": 0,
                "unresolved_missing_version_provider": 1,
                "exported_outside_dependency_closure": 2,
                "unresolved_unknown_provider": 3}
        unresolved.sort(key=lambda x: (rank.get(x["status"], 99), x["name"]))
        for item in unresolved[:sample_limit]:
            ver = ("@" + item["version"]) if "version" in item else ""
            lines.append(f"    {item['status']}: {item['name']}{ver}")
        if len(unresolved) > sample_limit:
            lines.append(f"    ... {len(unresolved) - sample_limit} additional unresolved imports (see JSON)")
        for warning in lib["warnings"]:
            lines.append("  WARNING: " + warning)
    lines.append("")
    lines.append("Interpretation: Missing SONAMEs are proven DT_NEEDED requirements;")
    lines.append("symbol candidates are not exact provider assignments without external libraries.")
    lines.append("ELF64 AArch64 is insufficient for Android-to-Horizon/libnx binary compatibility.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    root = Path(__file__).resolve().parent.parent / "import/quest-engine/APK/lib/arm64-v8a"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=root, help="directory of staged ARM64 .so files")
    parser.add_argument("--format", choices=("text", "json"), default="text", help="stdout format")
    parser.add_argument("--json-out", type=Path, help="write deterministic complete JSON report")
    parser.add_argument("--text-out", type=Path, help="write deterministic summary report")
    parser.add_argument("--sample-limit", type=int, default=20, help="unresolved samples per library in text")
    args = parser.parse_args(argv)
    if args.sample_limit < 0:
        parser.error("--sample-limit must be non-negative")
    try:
        report = analyze(args.input)
        json_text = json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True) + "\n"
        text_report = as_text(report, sample_limit=args.sample_limit)
        if args.json_out:
            args.json_out.write_text(json_text, encoding="utf-8", newline="\n")
        if args.text_out:
            args.text_out.write_text(text_report, encoding="utf-8", newline="\n")
        sys.stdout.write(json_text if args.format == "json" else text_report)
    except (ELFError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
