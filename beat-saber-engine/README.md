# beat-saber-engine

This folder builds the Switch NRO that loads the original Beat Saber Quest
1.46.0 Unity engine. For an overview, see the [root README](../README.md) and
[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

## Requirements

- devkitPro with devkitA64 and libnx.
- These portlibs:
  `dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-ffmpeg switch-dav1d switch-zlib switch-bzip2`.
- PowerShell 5.1+.

`build.ps1` sets `DEVKITPRO`/`DEVKITA64` to `C:/devkitPro` if they aren't
already set.

## Build

```powershell
& .\beat-saber-engine\build.ps1 -Target BeatSaberQuestLoaderLifecycle
& .\beat-saber-engine\build.ps1 -Target BeatSaberQuestLoaderLifecycle -Clean   # full rebuild
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

Put your files in `../SwitchPort/` (see
[docs/QUEST_FILES.md](../docs/QUEST_FILES.md)), then run:

```powershell
$dest = 'E:\switch\beat-saber-quest'          # or <Eden sdmc>\switch\beat-saber-quest
& .\beat-saber-engine\prepare-sd.ps1 -Destination $dest -IncludeAssets -DryRun
& .\beat-saber-engine\prepare-sd.ps1 -Destination $dest -IncludeAssets
& .\beat-saber-engine\prepare-sd.ps1 -Destination $dest -LoaderOnly   # later rebuilds
```

The NRO is copied as `BeatSaberQuestLoader.nro`. The APK and OBB are copied
next to it unchanged, and the loader reads them in place.

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
  resolve (needs `scripts/stage-quest-engine.ps1`).
- See [docs/DEBUGGING.md](../docs/DEBUGGING.md) for flag files and the Eden
  runner.
