# Building

How to build the Switch NRO that loads the original Beat Saber Quest 1.46.0
Unity engine. Run all commands from the repository root. For an overview, see
the [root README](../README.md) and [ARCHITECTURE.md](ARCHITECTURE.md).

## Requirements

- devkitPro with devkitA64 and libnx.
- These portlibs:
  `dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-ffmpeg switch-dav1d switch-zlib switch-bzip2`.
- PowerShell 5.1+.

`build.ps1` sets `DEVKITPRO`/`DEVKITA64` to `C:/devkitPro` if they aren't
already set.

## Build

```powershell
& .\build.ps1 -Target BeatSaberQuestLoaderLifecycle
& .\build.ps1 -Target BeatSaberQuestLoaderLifecycle -Clean   # full rebuild
```

This produces `BeatSaberQuestLoaderLifecycle.nro`, which `prepare-sd.ps1`
stages. The link uses `-lGLESv2 -lEGL -lglapi -ldrm_nouveau`, FFmpeg
(`avformat avcodec swscale swresample avutil dav1d bz2`), `-lz -lnx -lm`.

### Optional: NVK (Vulkan)

The experimental Vulkan path needs a prebuilt Mesa NVK for Horizon (see
[nxvk](https://github.com/PalindromicBreadLoaf/nxvk)). Put it in `nvk-sdk/`
or point `NVK_SDK` at it, and build with `BS_ENABLE_NVK=1`. The default GLES
renderer doesn't need it.

## Stage the SD card

Put your files in `SwitchPort/` (see [QUEST_FILES.md](QUEST_FILES.md)), then
run:

```powershell
$dest = 'E:\switch\beat-saber-quest'          # or <Eden sdmc>\switch\beat-saber-quest
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets -DryRun
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets
& .\prepare-sd.ps1 -Destination $dest -LoaderOnly   # later rebuilds
```

The NRO is copied as `BeatSaberQuestLoader.nro`. The APK and OBB are copied
next to it unchanged, and the loader reads them in place.

## Linux

`build.sh`, `prepare-sd.sh` and `setup.sh` do the same jobs as the
PowerShell scripts. You need `bash`, `make`, `unzip` and `curl`. `DEVKITPRO`
defaults to `/opt/devkitpro`.

```bash
chmod +x *.sh
./build.sh --target BeatSaberQuestLoaderLifecycle [--clean] [--switch2]
./prepare-sd.sh --destination /media/$USER/SD/switch/beat-saber-quest --include-assets --dry-run
./prepare-sd.sh --destination /media/$USER/SD/switch/beat-saber-quest --include-assets
./prepare-sd.sh --destination /media/$USER/SD/switch/beat-saber-quest --loader-only
./setup.sh [--console switch|switch2|both] [--destination DIR] [--nro FILE] [--skip-download]
```

## Run

Open the Homebrew Menu in **full-memory title mode** (hold R while launching
a game), not from the album applet, and start `BeatSaberQuestLoader.nro`.
Start-up takes a while the first time because shaders are compiled.

Hold **+ and −** together to quit.

## Troubleshooting

- Read `sdmc:/switch/beat-saber-quest/loader.log` from the last
  `SD mounted` line.
- If the build fails at the link step, a running emulator probably has the
  NRO open. Close it.
- `import-report.ps1` lists Quest imports that `platform_bridge.c` doesn't
  resolve (needs `tools/stage-quest-engine.ps1`).
- See [DEBUGGING.md](DEBUGGING.md) for flag files and the Eden
  runner.
