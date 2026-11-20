# Getting the Quest files

The loader needs exactly the build it was developed against:

| File | Expected name in `SwitchPort/` |
|---|---|
| Beat Saber Quest 1.46.0_34060 base APK | `beat-saber.apk` |
| Main expansion file (OBB) | `main.3491.com.beatgames.beatsaber.obb` |

Buy Beat Saber on the [Meta Horizon Store](https://www.meta.com/experiences/beat-saber/2448060205267927/)
and install it on a Meta Quest headset. A Steam/PC copy does **not** include
the Android APK.

## Pulling from your headset with ADB

1. Enable developer mode for the headset in the Meta Horizon phone app. See
   Meta's guide:
   <https://developers.meta.com/vr/documentation/android-apps/enable-developer-mode/>.
2. Install the Android SDK
   [platform-tools](https://developer.android.com/tools/releases/platform-tools)
   so that `adb` is on your `PATH`.
3. Connect the headset over a USB **data** cable and accept the USB debugging
   prompt inside the headset.
4. From the repository root, run:

   ```powershell
   # Show the installed version and package paths only (copies nothing)
   & .\tools\pull-owned-quest-apk.ps1 -InspectOnly

   # Copy the APK(s) and OBB into import/quest-beat-saber/
   & .\tools\pull-owned-quest-apk.ps1 -IncludeObb
   ```

   Use `-Serial <id>` if more than one device is connected.

5. Copy the files into `SwitchPort/` under the expected names:

   ```powershell
   Copy-Item import\quest-beat-saber\base.apk SwitchPort\beat-saber.apk
   Copy-Item import\quest-beat-saber\obb\com.beatgames.beatsaber\main.3491.com.beatgames.beatsaber.obb SwitchPort\
   ```

Check that `-InspectOnly` reports `versionName=1.46.0_34060`. Other versions
have different native libraries and will not work without changes to the
loader.

## Without a headset (unofficial)

The third-party Chrome/Edge extension
[DownQuest](https://github.com/ptrpaws/DownQuest) can download builds of apps
you own directly from Meta after you sign in on Meta's website, including
older versions where your account can still access them. It is not an official
Meta service, needs browser/account permissions, and may stop working. Pick
the **1.46.0_34060** build and its OBB, then copy them into `SwitchPort/` as
above. Never share Meta passwords, cookies or tokens.

## Optional: staging the native libraries for analysis

`import-report.ps1` and `tools/quest-analysis/` look at the extracted `.so` files.
To extract them, run:

```powershell
& .\tools\stage-quest-engine.ps1          # -> import/quest-engine/
& .\import-report.ps1     # unresolved-import summary
```

Building and running the loader doesn't need this step.

## Keep your files private

`SwitchPort/`, `import/` and any staged SD folders are git-ignored. Do not
commit, upload or share them.
