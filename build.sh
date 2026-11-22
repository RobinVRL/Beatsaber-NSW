#!/usr/bin/env bash
# Linux port of build.ps1.
# Usage: ./build.sh [--clean] [--target NAME] [--switch2] [--icon FILE.jpg]
#   --switch2  Build for Switch 2 (kaiHEN). DEVKITPRO must contain libounce and
#              portlibs/switch2 (the ounce-devkit packages); see docs/SWITCH2.md.
#   --icon     Optional 256x256 JPEG for the NRO icon (default: assets/icon.jpg).
set -euo pipefail

clean=0
target='BeatSaberQuestLoader'
switch2=0
icon=''
while [ $# -gt 0 ]; do
    case "$1" in
        --clean)   clean=1; shift ;;
        --target)  target=${2:?}; shift 2 ;;
        --switch2) switch2=1; shift ;;
        --icon)    icon=${2:?}; shift 2 ;;
        -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
[[ $target =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]] || { echo "Invalid target: $target" >&2; exit 1; }
die() { echo "ERROR: $*" >&2; exit 1; }

project=$(cd "$(dirname "$0")" && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITA64=${DEVKITA64:-$DEVKITPRO/devkitA64}
[ -d "$DEVKITA64" ] || die "devkitA64 not found at $DEVKITA64. Install devkitPro: https://devkitpro.org/wiki/devkitPro_pacman"

local_nvk="$project/nvk-sdk"
if [ -z "${NVK_SDK:-}" ] && [ -f "$local_nvk/lib/libvulkan.a" ]; then
    # make recursively changes from the project directory into build/.
    # Keeping the SDK outside build/ preserves it across --clean builds.
    export NVK_SDK='../nvk-sdk'
fi
if [ -n "${NVK_SDK:-}" ]; then
    if [ "$NVK_SDK" = '../nvk-sdk' ]; then sdk_dir=$local_nvk; else sdk_dir=$NVK_SDK; fi
    for required in lib/libvulkan.a include/vulkan Vulkan-Headers-1.4.305/include/vk_video; do
        [ -e "$sdk_dir/$required" ] || die "NVK_SDK is missing $required under $sdk_dir"
    done
    echo "Using NVK Vulkan SDK: $sdk_dir"
fi

export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
command -v make >/dev/null || die "'make' is required (e.g. sudo apt install make)."

platform_args=()
suffix=''
if [ $switch2 -eq 1 ]; then
    for required in libounce/switch2_rules portlibs/switch2/lib/libouncevk-gl.a portlibs/switch2/lib/kaihen-jit.o; do
        [ -e "$DEVKITPRO/$required" ] || die "DEVKITPRO ($DEVKITPRO) is missing $required; install ounce-devkit"
    done
    platform_args+=(SWITCH2=1)
    suffix='_switch2'
fi
if [ -n "$icon" ]; then
    [ -f "$icon" ] || die "Missing NRO icon: $icon"
    platform_args+=("NRO_ICON=$(cd "$(dirname "$icon")" && pwd)/$(basename "$icon")")
elif [ ! -f "$project/assets/icon.jpg" ]; then
    die 'Missing assets/icon.jpg (official Beat Saber NRO icon).'
fi

if [ $clean -eq 1 ]; then
    make -C "$project" clean "TARGET=$target" ${platform_args[@]+"${platform_args[@]}"}
fi
make -C "$project" -j"$(nproc 2>/dev/null || echo 4)" "TARGET=$target" ${platform_args[@]+"${platform_args[@]}"}

nro="$project/$target$suffix.nro"
[ -f "$nro" ] || die 'The NRO build output is missing.'
echo "Built $nro ($(stat -c %s "$nro") bytes)"
