# Quest Beat Saber 1.46 ARM64 ABI audit

This Python standard-library analyzer examines staged ELF64 little-endian
AArch64 shared objects without loading or executing them. Its deterministic
text and JSON reports contain dynamic dependencies, imported and exported
symbols, GNU symbol versions, and dependency-closure resolution.

From the repository root in Windows PowerShell:

    py -3 tools/quest-analysis/analyze_elf.py
    py -3 tools/quest-analysis/analyze_elf.py --json-out tools/quest-analysis/example-report.json --text-out tools/quest-analysis/example-report.txt
    py -3 -m unittest discover -s tools/quest-analysis -p 'test_*.py' -v

Default input is import/quest-engine/APK/lib/arm64-v8a (created by
tools/stage-quest-engine.ps1). Pass
--input PATH to inspect a different staging directory, --format json to
print the complete JSON report, or --sample-limit N to select the number
of representative unresolved imports in text mode.

Both output formats contain symbol names and metadata, **never executable
binary payloads, source-game assets, absolute paths, or commercial audio**.

## Resolution statuses

| Status | Interpretation |
|---|---|
| resolved_staged | Compatible export found in the transitive DT_NEEDED closure; provider is the first match in dependency order |
| unresolved_missing_version_provider | GNU DT_VERNEED explicitly identifies a missing library for the import |
| unresolved_android_api_candidate | Symbol has a recognizable Android/JNI/graphics prefix; heuristic, not proven ownership |
| unresolved_unknown_provider | No reliable provider attribution available |
| exported_outside_dependency_closure | Symbol exists in another staged object but is absent from the recorded dependency graph |
| unresolved_optional_weak | An unresolved weak symbol; runtime loaders may permit absence |

Each missing DT_NEEDED is a demonstrated external loader requirement.
Unversioned imports cannot always be assigned to a specific system SONAME.
The tool checks only dynamic exports and follows the declared DT_NEEDED
transitive closure; actual dlopen/dlsym, JNI registration, symbol-visibility
scopes, relocations and OS services require separate verification.

## Results on the staged original Quest 1.46 engine

The example reports are not committed (they are git-ignored). Generate them
locally with the command above. Summary from the 15 staged native libraries
(2,422 imported and 18,864 exported dynamic symbols).
566 imports resolve to other staged objects.

Ten absent Android system SONAMEs occur in DT_NEEDED:

    libEGL.so, libGLESv3.so, libandroid.so, libc.so, libdl.so,
    liblog.so, libm.so, libmediandk.so, libstdc++.so, libz.so

libunity.so needs Android native APIs, EGL and media services; libOVRPlugin.so
needs Android/EGL/GLES; libil2cpp.so imports Android Bionic symbols. All
primary libraries are ELF64 AArch64, but Android's Bionic loader and APIs,
JNI/GameActivity, graphics, audio and Quest OpenXR services do not execute
on Horizon/libnx directly. The matching AArch64 CPU ISA is insufficient
for a working original Unity game NRO. The report isolates ABI work for
an eventual loader/compatibility implementation or a genuine engine rebuild.

## Parsing/format guarantees

- Rejects non-ELF64, wrong-endian or non-AArch64 binaries.
- Validates ELF program/section headers, symbol/string bounds and GNU hashes.
- Reads dynamic symbols from section headers or SysV/GNU hashes (sectionless).
- Supports DT_SONAME aliasing and GNU versioned symbol matches.
- Sorts library names, symbols, blockers and counters reproducibly.
- Reads source binaries; never mutates original APK/OBB, staged libraries,
  or the original-engine loader.
