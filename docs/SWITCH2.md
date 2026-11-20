# Switch 2 (kaiHEN)

Status: **builds; not yet run on hardware or in a Switch 2 emulator.**

[kaiHEN](https://github.com/ouncebrew/kaihen) runs homebrew on the Switch 2
as the game. The Switch 2 build uses
[libounce](https://github.com/ouncebrew/libounce) (libnx for Switch 2),
OunceVK (NVK + Zink: Vulkan, EGL and GLES) and libkaihen.

## Devkit

Install [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) into
devkitPro (`sh install.sh`, which adds the `[ounce]` pacman repository).
That provides `$DEVKITPRO/libounce` and `$DEVKITPRO/portlibs/switch2`.

To leave an existing devkitPro unchanged, you can unpack the three release
packages (`libounce`, `ouncevk`, `libkaihen`, from the GitHub releases of
each project) into a separate folder. Then point `DEVKITPRO` at an overlay
directory: junctions for `devkitA64`, `tools`, `msys2`, `libnx` and
`portlibs/switch` to the real devkitPro, and for `libounce` and
`portlibs/switch2` to the unpacked packages.

## Build

```powershell
.\build.ps1 -Switch2 -Target BeatSaberQuestLoaderLifecycle
# or: make SWITCH2=1 TARGET=BeatSaberQuestLoaderLifecycle
```

The output is `<Target>_switch2.nro`. Objects go to `build_switch2/`, so the
Switch 1 and Switch 2 builds don't overwrite each other.

`SWITCH2=1` includes `libounce/switch2_rules` (defines `__SWITCH2__`, puts
the Switch 2 portlibs first), builds for the Cortex-A78C and links
`-lkaihen`. The OunceVK `libEGL.a` pulls in its GL driver, and its empty
`libGLESv2.a`, `libglapi.a` and `libdrm_nouveau.a` let the Switch 1 library
list link unchanged.

## How the libraries are mapped

The game's policy doesn't allow the code-mapping syscalls
(`svcMapProcessCodeMemory`, `svcSetProcessMemoryPermission`). kaiHEN's RO
service makes them on the game's behalf: private `ldr:ro` command 65010
(`MapJitCode`), which `kaihen-jit.o` exposes as `__nx_jit_map_code`. RO maps
a heap range as code and makes all of it RX.

`ldrRoLoadNro` isn't used: RO allows only 8 NROs per client (hbloader's NRO
takes one, and the game loads 15 libraries). The libounce `Jit` API isn't
used either: its RW view is a separate staging copy, and the libraries
need real writable data next to their code.

So `so_finalize` (`source/so_util.c`) maps each library as two parts:

- **Text**: `[0, end of the last executable segment)`, mapped RX by RO.
  This includes the leading read-only segment and alignment gaps.
- **Data**: the rest (RELRO, `.data`, `.bss`), aliased RW with
  `svcMapMemory`. A game may use that syscall, but only with a destination
  in the stack region.

Both parts must be contiguous, so the libraries are placed in a reserved
arena (up to 384 MiB; the libraries need about 170 MiB) at the start of the
largest free block of the stack region. The kernel also accepts code
mappings there, because it lies outside the heap and alias regions (true on
39-bit address spaces). `so_finalize` refuses a library whose writable
segment would fall inside the text part.

All hooks are written to the staged copy before `so_finalize`, so the text
is never patched afterwards. `so_patch_code` returns an error on Switch 2.
hbloader releases leftover RO mappings when the NRO exits.

Without the kaiHEN `ldr:ro` override (for example, an emulator that runs
the NRO directly), the loader falls back to the Switch 1 syscall path.

## Other Switch 2 changes

- `bionic_mmap.c`: the overcommit window is clipped to the stack region
  (the kernel refuses `svcMapMemory` outside it; Eden doesn't check). It
  avoids the library arena. File I/O into library data pages is bounced
  through heap memory, as for window pages.
- `egl_bridge.c`: if an ES 3.2 context is refused, the loader retries ES 3.1
  (Zink may stop at 3.1). If the driver advertises `GL_OVR_multiview2`
  natively, the multiview calls pass straight through, and the
  layered-rendering emulation (shader rewrite, instance doubling, extension
  injection) is turned off.
- Audio: libounce's `audoutInitialize` already goes through `aud:u`, as a
  Switch 2 game requires.

## Unverified

- That RO's `MapJitCode` and the kernel accept destinations in the stack
  region on the Switch 2.
- The size of the stack region on the Switch 2. The overcommit window needs
  at least 512 MiB after the arena.
- OunceVK's GLES version and extensions. The layered multiview emulation
  needs `GL_NV_viewport_array2` and `GL_EXT_geometry_shader` if the driver
  has no native `GL_OVR_multiview2`.
- Joy-Con 2 six-axis input through libounce's HID.
- Memory: the engine, the IL2CPP heap and the uncompressed textures (see
  the Alpha8 note in [KNOWN_ISSUES.md](KNOWN_ISSUES.md)).

The loader log (`sdmc:/switch/beat-saber-quest/loader.log`) reports each of
these: `[link] module arena`, `[link] <lib>: text ... RX (RO)`,
`[mmap] stack region`, `[egl:preflight]` and `[gles:mv] native
GL_OVR_multiview2`.
