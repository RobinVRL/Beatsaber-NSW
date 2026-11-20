# Contributing

Contributions are welcome, especially fixes for anything in
[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) and testing on real hardware.

## Ground rules

- **No game content.** Never commit or attach APKs, OBBs, `.so` files from the
  game, extracted assets, shader dumps of game shaders, SD-card folders or
  music. Logs are fine, but check them for personal paths first.
- Keep the loader offline. Don't add network access.
- Keep the Quest libraries unmodified. Fix behaviour in the bridges, not by
  patching game binaries.

## Workflow

1. Build with `build.ps1 -Target BeatSaberQuestLoaderLifecycle`
   with no new warnings.
2. Run the host tests (see [docs/DEBUGGING.md](docs/DEBUGGING.md)).
3. If your change affects runtime behaviour, run it in Eden or on hardware.
   Include the relevant `loader.log` excerpt and, for visual changes,
   before/after screenshots.
4. Keep diagnostics behind flag files. They must be off by default.

## Style

- C11 in the existing style: 4-space indent, `bs_` prefix for exported bridge
  functions, `static` for everything else.
- Files use LF line endings.
- Keep one bridge per subsystem (`bionic_*`, `android_*`, `unity_dex_*` and
  so on), and register new imports in `platform_bridge.c`.
