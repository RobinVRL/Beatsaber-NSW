# SD card setup (Quest 1.40.8 port, `beat-saber-1.40.8` branch)

This page covers the experimental **1.40.8** port. The main **1.46.0**
loader uses a different folder and file set; see `SETUP.md` on the
[`main`](../../tree/main) branch. Both ports can sit on the same SD card side
by side (`switch/beat-saber-quest/` and `switch/beat-saber-1408/`).

> [!WARNING]
> **Online multiplayer is experimental and still buggy.** It works in Eden
> (official servers or BeatTogether through MultiplayerCore, staged with
> `-IncludeMods`) but is untested on real hardware. Expect disconnects,
> desyncs or crashes.

## What you need

- Beat Saber Quest **1.40.8_7379** APK, optionally MBF-patched (Scotland2)
- `main.1716.com.beatgames.beatsaber.obb`
- Optional: the headset's `/sdcard/ModData/` folder for Scotland2 mods
- A Switch with homebrew, a Switch 2 with kaiHEN, or the Eden emulator

By default `prepare-sd.ps1` reads these from
`../port-1.40.8-modded/quest-files/` (override with `-Source`):

```
quest-files/
├── beat-saber.apk
├── obb/main.1716.com.beatgames.beatsaber.obb
└── ModData/Modloader/       (libsl2.so, libs/, early_mods/, mods/)
```

## Final SD card layout

Everything lives in one folder: `sdmc:/switch/beat-saber-1408/`.

```
SD card root
└── switch/
    └── beat-saber-1408/
        ├── BeatSaber1408.nro         Switch NRO
        │   (BeatSaber1408_switch2.nro for Switch 2, copy it manually)
        ├── beat-saber.apk            your APK, unchanged
        ├── libmain.so                ┐ all ARM64 libraries from
        ├── libunity.so               │ lib/arm64-v8a/ in the APK, flat
        ├── libil2cpp.so              │ next to the NRO (libOculusXRPlugin.so,
        ├── ...                       ┘ libunityopus.so, libcrypto.so, ...)
        ├── obb/
        │   └── main.1716.com.beatgames.beatsaber.obb   (copied as-is)
        ├── assets/                   extracted from the APK and the OBB
        ├── files/                    game save data (created empty)
        ├── external-files/           (created empty)
        ├── ModData/                  only for modded setups (-IncludeMods)
        │   ├── Modloader/            Scotland2: libsl2.so, libs/, early_mods/, mods/
        │   └── com.beatgames.beatsaber/   mod configs and songs
        │                             (the game's /sdcard/ModData/...)
        │
        │   Created by the loader at runtime:
        ├── loader.log
        └── crash.log / watchdog.log  (only after a crash or hang)
```

Differences from the 1.46.0 layout:

- Folder is `beat-saber-1408`, not `beat-saber-quest`.
- The OBB **is** copied, into `obb/`, in addition to its extracted `assets/`.
- `files/` and `external-files/` are created at staging time.
- `ModData/` maps the Quest's `/sdcard/ModData/`, so Scotland2 mods and
  custom songs go there with the same structure as on the headset.
- No `local-files.txt` manifest and no `setup.bat`; use `prepare-sd.ps1`.

## Staging

```powershell
& .\build.ps1                          # produces BeatSaber1408.nro
$dest = 'E:\switch\beat-saber-1408'    # path must end in beat-saber-1408
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets               # vanilla
& .\prepare-sd.ps1 -Destination $dest -IncludeAssets -IncludeMods  # with Scotland2 mods
& .\prepare-sd.ps1 -Destination $dest -LoaderOnly                  # later rebuilds: NRO only
```

For Eden, use `%APPDATA%\eden\sdmc\switch\beat-saber-1408`.

For Switch 2, also copy `BeatSaber1408_switch2.nro` into the same folder.

## Launching

Start the Homebrew Menu in **full-memory title-override mode** (hold R while
launching a game), not from the Album applet, and run `BeatSaber1408.nro`.
If it fails, read `sdmc:/switch/beat-saber-1408/loader.log`.
