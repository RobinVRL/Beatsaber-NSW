# Beat Saber for Nintendo Switch (Quest engine loader)

An experimental homebrew loader that runs the **original Meta Quest build of
Beat Saber** (1.46.0, Unity 6000.3.19f1, ARM64) on the Nintendo Switch. It
uses the libnx `.so`-loader technique. The Quest game's native Android
libraries are loaded unmodified, and a compatibility layer emulates the
Android, Bionic, JNI, EGL/GLES and OpenXR APIs that they expect.

> **This repository contains no game code or assets.** You need your own
> legally obtained copy of the Quest APK and OBB. See
> [docs/QUEST_FILES.md](docs/QUEST_FILES.md).

## Status

| Area | State |
|---|---|
| Boot: native constructors, JNI, IL2CPP, Unity GameActivity | Working |
| Rendering (GLES 3.2 via devkitPro Mesa/nouveau) | Working: menus, environments, notes, walls, sabers, bloom |
| Stereo/multiview (emulated with layered rendering) | Working, left eye shown on screen |
| Song playback and gameplay | Working in Eden (levels load, play, fail and exit) |
| Joy-Con gyro saber control | Working |
| Audio (FFmpeg-backed Android media bridge) | Working |
| Real Switch hardware | **Untested** |
| Switch 2 (kaiHEN JIT) | Planned, see [docs/SWITCH2.md](docs/SWITCH2.md) |

Remaining visual and functional issues are tracked in
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).

## Quick start

Requirements:

- Windows with PowerShell 5.1+ and Python 3.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64,
  libnx and these portlibs: `switch-mesa`, `switch-libdrm_nouveau`,
  `switch-ffmpeg`, `switch-dav1d`, `switch-zlib`, `switch-bzip2`.
- The Beat Saber Quest **1.46.0_34060** APK plus
  `main.3491.com.beatgames.beatsaber.obb`, copied from your own headset.
- A Switch running homebrew (launched in full-memory title mode), or the
  [Eden](https://eden-emulator.github.io/) emulator.

```powershell
# 1. Put your files in SwitchPort/ (see SwitchPort/README.md)
#    SwitchPort/beat-saber.apk
#    SwitchPort/main.3491.com.beatgames.beatsaber.obb

# 2. Build
& .\beat-saber-engine\build.ps1 -Target BeatSaberQuestLoaderLifecycle

# 3. Stage onto the SD card (or Eden's sdmc folder). The path must end in
#    switch\beat-saber-quest
$dest = 'D:\switch\beat-saber-quest'
& .\beat-saber-engine\prepare-sd.ps1 -Destination $dest -IncludeAssets -DryRun
& .\beat-saber-engine\prepare-sd.ps1 -Destination $dest -IncludeAssets
```

Then launch `BeatSaberQuestLoader.nro` from the Homebrew Menu. Use
**title-override (full-memory) mode**, not applet mode. On later rebuilds,
use `-LoaderOnly` instead of `-IncludeAssets` to copy only the loader.

See [beat-saber-engine/README.md](beat-saber-engine/README.md) for full build,
staging and troubleshooting details.

## Controls

Hold a Joy-Con in each hand. Each Joy-Con's gyroscope aims its saber.

| Action | Left Joy-Con | Right Joy-Con |
|---|---|---|
| Trigger / UI select | ZL | ZR |
| Grip | L | R |
| A / X | D-pad Down | A |
| B / Y | D-pad Left | B |
| Menu (pause) | − | + |
| Thumbstick | Left stick | Right stick |
| Recenter saber | Press left stick + L | Press right stick + R |
| Look around (head yaw/pitch) | Hold L + D-pad | |
| Quit the loader | | Hold + and − together |

Without motion sensors, the sticks aim the sabers instead.

## Repository layout

```
beat-saber-engine/   The NRO: loader, Android/Bionic/JNI/EGL/OpenXR bridges
  source/            C sources
  tests/             Host-side unit tests (C + Python) and Eden test runners
  tools/             Developer tools
  build.ps1          Build wrapper around the devkitPro Makefile
  prepare-sd.ps1     Stages loader + your Quest files into an SD layout
scripts/             Pull the APK from your headset; stage engine libraries
quest-analysis/      Read-only ELF/ABI analyzer for the Quest libraries
SwitchPort/          Put your own APK/OBB here (git-ignored)
docs/                Architecture, debugging, known issues, dev notes
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md): how the loader, bridges and renderer fit together
- [Getting the Quest files](docs/QUEST_FILES.md)
- [Debugging and test tooling](docs/DEBUGGING.md): logs, Eden runner, diagnostic flag files
- [Known issues](docs/KNOWN_ISSUES.md)
- [Switch 2 / kaiHEN plan](docs/SWITCH2.md)
- [Contributing](CONTRIBUTING.md)
- [Developer notes and audits](docs/dev-notes/): historical bring-up logs

## Legal

Beat Saber is a trademark of Beat Games / Meta. This project is not affiliated
with or endorsed by Beat Games, Meta, Nintendo or Unity Technologies.

Do **not** upload, share or link to APKs, OBBs, extracted assets, staged SD
folders or commercial music in issues or pull requests. Owning the game does
not grant redistribution rights.

The loader code is MIT-licensed (see [LICENSE](LICENSE)). It builds on the
open-source Switch `.so`-loader lineage by Andy Nguyen, fgsfds,
TheOfficialFloW, the PvZ Fusion NX port (pvz_fusion_en_nx) and others.
