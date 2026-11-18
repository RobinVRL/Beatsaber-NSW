# Switch 2 (kaiHEN) plan

Status: **planned, not started**.

[kaiHEN](https://github.com/ouncebrew/kaihen) runs homebrew on the Switch 2.
The loader currently relies on `svcMapProcessCodeMemory` (in `so_finalize`,
`source/so_util.c`) to make the Quest libraries executable. kaiHEN doesn't
offer that. It offers a JIT API in libounce instead.

## Steps

1. Install the ounce toolchain (`ounce-devkit` / `libounce`) from the
   `[ounce]` dkp-pacman repository at <https://pkg.ouncebrew.org>.
2. Port `so_finalize` to the libounce JIT API: `jitCreate` →
   copy/relocate through `jitGetRwAddr` → `jitTransitionToExecutable`. Link
   with `-lkaihen`.
3. Check that the GPU path (Mesa nouveau GLES) works on the Switch 2 GPU under
   kaiHEN, or fall back to NVK.
4. Check memory. The loaded engine, the IL2CPP heap and the uncompressed
   textures (see the Alpha8 note in [KNOWN_ISSUES.md](KNOWN_ISSUES.md)) need
   to fit in the memory available to homebrew.
5. Test in a Switch 2 emulator (such as Lichii), then on hardware.
