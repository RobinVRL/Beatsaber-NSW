# Debugging and test tooling

## Logs

The loader writes `sdmc:/switch/beat-saber-quest/loader.log`. Each launch
appends to the file, so read from the last `SD mounted` line. On Eden the file
is at `%APPDATA%\eden\sdmc\switch\beat-saber-quest\loader.log`.

To flush the log synchronously after every line, create an empty `sync-log`
file next to it. This is slow, but no lines are lost if the loader crashes.

## Eden test runner

`beat-saber-engine/tests/run_eden_game.ps1` launches the NRO in Eden, watches
the guest log and takes screenshots of the Eden window:

```powershell
$env:EDEN_EXE = 'D:\Eden\eden.exe'
& .\beat-saber-engine\tests\run_eden_game.ps1 -MaxSeconds 165 -QuietSeconds 30 -ShotEverySeconds 5
```

Notes:

- Stage the SD folder first with `prepare-sd.ps1` (use Eden's `sdmc` folder as
  `-Destination`).
- Close Eden before you rebuild. A running Eden keeps the NRO locked, and the
  link step then fails.
- In Eden, the first gameplay level usually appears after about 2.5 minutes
  (shader compilation).
- Eden's *Ignore memory aborts* CPU option (`cpuopt_ignore_memory_aborts`)
  was enabled during development.

`run_eden_pregraphics.ps1` is the older start-up-only runner.

## Scripted input (`autoinput.txt`)

Use this to drive menus without a controller. Put one step per line:

```
<start_s> <end_s> TOKEN...
```

Times are seconds from the first input poll. The tokens are:

- Button names: `ZR ZL R L A B X Y PLUS MINUS UP DOWN LEFT RIGHT`.
- Aim tokens: `laim=yaw,pitch` / `raim=yaw,pitch` (radians).
- Head token: `head=yaw,pitch` (degrees).

## Diagnostic flag files

Create these files in `sdmc:/switch/beat-saber-quest/`. Most only need to
exist; their contents don't matter. Delete them afterwards, because several
of them change what is rendered.

| File | Effect |
|---|---|
| `dump_shaders` | Write every translated shader to `shaderdump/q<seq>.txt` |
| `trace_frame.txt` | Contains a frame number (default 1500). Logs every GL call in that frame |
| `trace_fog` | Log fog/bloom uniforms per draw |
| `trace_cut` | Log cutout/note draws |
| `no_light_tfb` | Disable the transform-feedback replacement for the light compute shader |
| `no_stencil` | Disable stencil use |
| `skip_opaque` | Skip opaque draws |
| `cube_white`, `note_magenta`, `note_split`, `note_pos`, `note_bloom`, `note_fogviz`, `obst_viz`, `id_viz` | Debug recolouring of notes, obstacles and draw IDs (**visibly changes the screen**) |
| `strict-java-activity` | Fail hard on any unimplemented Java Activity call |
| `sync-log` | Synchronous logging (see above) |

## Host-side tests

`beat-saber-engine/tests/` has C self-tests for individual bridges (JNI,
ELF loading, `madvise`, GameActivity and so on). The headers in the shim
folders let them build on a PC. It also has Python tests for the tools.
Tests named `test_original_*` and the `*_elf` tests inspect the real Quest
libraries. They need `scripts/stage-quest-engine.ps1` to have been run first.
Examples:

```powershell
python -m unittest discover -s beat-saber-engine/tests -p "test_*.py"
python -m unittest quest-analysis/test_analyze_elf.py

# Strict JNI self-test (needs any jni.h, e.g. from a JDK or Android NDK)
clang -std=c11 -Wall -Wextra -Werror -I<path-to-jni.h> -Ibeat-saber-engine/source `
  beat-saber-engine/tests/jni_gameactivity_strict_selftest.c beat-saber-engine/source/jni_bridge.c `
  -o jni_selftest.exe
.\jni_selftest.exe
```
