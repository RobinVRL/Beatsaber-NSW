# Known issues

## Rendering

- A flat black, note-sized box sometimes passes near the centre of the track.
  It hasn't been identified yet. It isn't the obstacle core or the note body
  shader.
- Saber trails look like a solid sheet instead of a fading ribbon.
- There is a thin black stripe along the left edge of the image.
- Only the left eye is shown on screen. The right eye is rendered but not
  presented.
- Alpha8 emulation expands R8 textures to RGBA8. A 4096×4096 texture then
  takes 64 MiB, plus a 64 MiB temporary buffer during upload.

## Runtime

- GC is disabled (`GC_DONT_GC=1`), so memory use grows over long sessions.
- Zenject `AdbCommandReceiver` throws a `NullReferenceException` at start-up
  (harmless).
- The log reports "Could not set display frequency of 90". The display runs
  at the Switch refresh rate.
- `XR_ERROR_REFERENCE_SPACE_UNSUPPORTED` is returned for some reference
  spaces that the game probes.
- Repeated `/proc/self/exe` ENOENT and `[gles:tex]` messages make the log
  noisy.
- Start-up is slow in Eden, about 2.5 minutes to the first level, mostly
  shader compilation.

## Platform

- The loader must be launched in full-memory title mode. Applet mode doesn't
  have enough memory or code-mapping rights.
- Multiplayer, leaderboards, store/DLC purchases and other online features
  are disabled on purpose (offline only).
