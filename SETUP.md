# SD card setup (Quest 1.46.0 loader, `main` branch)

This page covers the **1.46.0** loader on the `main` branch. The experimental
**1.40.8** port uses a different folder and file set; see `SETUP.md` on the
[`beat-saber-1.40.8`](../../tree/beat-saber-1.40.8) branch. The two ports can
sit on the same SD card side by side.

## What you need

- Beat Saber Quest **1.46.0_34060** base APK (any file name)
- `main.3491.com.beatgames.beatsaber.obb`
- A Switch with homebrew (Atmosphère + Homebrew Menu), a Switch 2 with
  kaiHEN, or the Eden emulator

See [docs/QUEST_FILES.md](docs/QUEST_FILES.md) for how to copy these from
your headset. Other versions will not work.

## Final SD card layout

Everything lives in one folder: `sdmc:/switch/beat-saber-quest/`.

```
SD card root
└── switch/
    └── beat-saber-quest/
        ├── Beatsaber.nro             Switch NRO (from setup.bat / releases)
        ├── BeatsaberNS2.nro          Switch 2 NRO (optional)
        │   or BeatSaberQuestLoader.nro if you staged a local build
        ├── beat-saber.apk            your APK, unchanged (classes.dex is read from it)
        ├── libmain.so                ┐
        ├── libunity.so               │
        ├── libil2cpp.so              │
        ├── libgame.so                │
        ├── libc++_shared.so          │
        ├── lib_burst_generated.so    │ 15 ARM64 libraries extracted from
        ├── libopenxr_loader.so       │ lib/arm64-v8a/ in the APK
        ├── libUnityOpenXR.so         │ (flat, next to the NRO)
        ├── libOVRPlugin.so           │
        ├── libOVRMetricsTool.so      │
        ├── libovrplatformloader.so   │
        ├── libhaptics_sdk.so         │
        ├── libenet.so                │
        ├── libcrypto.so              │
        ├── libssl.so                 ┘
        ├── assets/                   extracted from the APK and the OBB
        │   ├── aa/                   (catalog.json, asset bundles)
        │   └── bin/Data/             (boot.config, data.unity3d, ...)
        ├── local-files.txt           manifest written by the setup scripts
        │
        │   Created by the loader at runtime:
        ├── files/                    game save data
        ├── external-files/
        ├── loader.log
        └── crash.log / watchdog.log  (only after a crash or hang)
```

Notes:

- The OBB itself is **not** copied. Only its `assets/` are extracted.
- The `.so` files must sit directly in `beat-saber-quest/`, not in `lib/`.
- The folder must be named exactly `beat-saber-quest`. The scripts refuse
  any other name.

## Option A: setup.bat (players)

1. Put `setup.bat`, `setup.ps1`, your APK and your OBB into an empty folder.
2. Run `setup.bat` and choose Switch, Switch 2 or both.
3. Copy the generated `SD\switch` folder to the root of the SD card and merge
   it with the existing `switch` folder.

To write straight to the card, use `.\setup.ps1 -Destination E:\` (the
script adds `switch\beat-saber-quest` itself).

## Option B: prepare-sd.ps1 (developers)

Put the files in `SwitchPort/beat-saber.apk` and
`SwitchPort/main.3491.com.beatgames.beatsaber.obb`, build, then stage:

```powershell
& .\build.ps1 -Target BeatSaberQuestLoaderLifecycle
$dest = 'E:\switch\beat-saber-quest'   # path must end in beat-saber-quest
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets -DryRun
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets
& .\prepare-sd.ps1 -Destination $dest -LoaderOnly    # later rebuilds: NRO only
```

On Linux use `prepare-sd.sh` / `setup.sh` with the same options (see
[docs/BUILDING.md](docs/BUILDING.md)).

For Eden, use `<Eden sdmc>\switch\beat-saber-quest`
(usually `%APPDATA%\eden\sdmc\switch\beat-saber-quest`).

## Launching

Start the Homebrew Menu in **full-memory title-override mode** (hold R while
launching a game), not from the Album applet, and run the NRO. The first
start is slow because shaders are compiled. If it fails, read
`sdmc:/switch/beat-saber-quest/loader.log` from the last `SD mounted` line.
