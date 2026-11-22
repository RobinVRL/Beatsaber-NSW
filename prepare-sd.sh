#!/usr/bin/env bash
# Linux port of prepare-sd.ps1: stage your locally owned Beat Saber Quest
# runtime (from SwitchPort/) plus the built loader for Switch homebrew.
#
# Usage: ./prepare-sd.sh --destination DIR/beat-saber-quest [--include-assets] [--dry-run] [--loader-only]
# Example: ./prepare-sd.sh --destination /media/$USER/SD/switch/beat-saber-quest --include-assets --dry-run
# The NRO never embeds game binaries or licensed media.
set -euo pipefail

destination=''
include_assets=0
dry_run=0
loader_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --destination)    destination=${2:?}; shift 2 ;;
        --include-assets) include_assets=1; shift ;;
        --dry-run)        dry_run=1; shift ;;
        --loader-only)    loader_only=1; shift ;;
        -h|--help)        sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
die() { echo "ERROR: $*" >&2; exit 1; }
[ -n "$destination" ] || die '--destination is required.'
command -v unzip >/dev/null || die "'unzip' is required (e.g. sudo apt install unzip)."
sha256() { sha256sum "$1" | cut -d' ' -f1; }

project=$(cd "$(dirname "$0")" && pwd)
source_dir="$project/SwitchPort"
apk="$source_dir/beat-saber.apk"
obb="$source_dir/main.3491.com.beatgames.beatsaber.obb"
nro="$project/BeatSaberQuestLoaderLifecycle.nro"
app_dir_name='beat-saber-quest'

for item in "$apk" "$obb" "$nro"; do
    [ -f "$item" ] || die "Required original/build file is missing: $item"
done
dest=$(realpath -m "$destination")
[ "$(basename "$dest")" = "$app_dir_name" ] ||
    die "Destination must end with '$app_dir_name' (Switch path: sdmc:/switch/$app_dir_name/)."
case "$dest/" in "$(realpath -m "$source_dir")"/*) die 'Do not stage into the original owned APK/OBB source folder.' ;; esac

loader_digest=$(sha256 "$nro")
header=(
    '# Private owner-supplied game files; never distribute'
    '# Quest version 1.46.0_34060; Unity 6000.3.19f1'
    "# Loader NRO SHA256: $loader_digest"
    '# Diagnostic: original ELF/TLS/JNI, GameActivity preflight, VI frame scheduling, MediaNDK metadata; original Unity gameplay requires missing services.'
)

if [ $loader_only -eq 1 ]; then
    [ $include_assets -eq 0 ] || die 'Loader-only preserves existing assets; omit --include-assets.'
    [ -d "$dest" ] || die 'Loader-only requires an existing, prepared private SD folder.'
    for module in libmain.so libc++_shared.so libunity.so libil2cpp.so libgame.so \
                  lib_burst_generated.so libopenxr_loader.so libUnityOpenXR.so libOVRPlugin.so \
                  libOVRMetricsTool.so libovrplatformloader.so libhaptics_sdk.so libenet.so \
                  libcrypto.so libssl.so; do
        [ -f "$dest/$module" ] || die "Loader-only blocked: original Quest library missing: $module"
    done
    [ -f "$dest/assets/bin/Data/boot.config" ] || die 'Loader-only blocked: original Unity boot.config is not staged.'
    [ -f "$dest/beat-saber.apk" ] && [ "$(sha256 "$dest/beat-saber.apk")" = "$(sha256 "$apk")" ] ||
        die 'Loader-only blocked: byte-identical original APK (classes.dex) is not staged.'
    manifest="$dest/local-files.txt"
    [ -f "$manifest" ] || die 'Loader-only blocked: original owner-only manifest is missing.'
    if [ $dry_run -eq 0 ]; then
        cp -f "$nro" "$dest/BeatSaberQuestLoader.nro"
        tmp=$(mktemp)
        { printf '%s\n' "${header[2]}" "${header[3]}"
          grep -v -e '^# Loader NRO SHA256:' -e '^# Diagnostic:' "$manifest" || true; } >"$tmp"
        mv -f "$tmp" "$manifest"
    fi
    echo "Loader-only $([ $dry_run -eq 1 ] && echo 'dry run' || echo updated): 15 private Quest libraries and original boot.config verified; no original media rewritten."
    echo "NRO SHA256: $loader_digest"
    exit 0
fi

# Refuse archives with entries that could escape the destination.
for archive in "$apk" "$obb"; do
    if unzip -Z1 "$archive" | grep -Eq '(^|/)\.\.(/|$)|^/|:'; then die "Unsafe archive entry in $archive"; fi
done

echo "Staging locally owned Quest Beat Saber to: $dest"
echo "Mode: $([ $dry_run -eq 1 ] && echo 'inspect only' || echo 'copy files')"

entries=$(mktemp)
trap 'rm -f "$entries"' EXIT
echo "APK | beat-saber.apk | $(stat -c %s "$apk")" >"$entries"
unzip -Zl "$apk" | awk -v assets=$include_assets '$1 ~ /^-/ {
    if ($NF ~ /^lib\/arm64-v8a\/[^\/]+\.so$/) { n = $NF; sub(/.*\//, "", n); print "APK | " n " | " $4 }
    if (assets && $NF ~ /^assets\//) print "APK | " $NF " | " $4 }' >>"$entries"
for lib in libunity.so libil2cpp.so libgame.so; do
    grep -q "^APK | $lib |" "$entries" || die 'APK is missing required ARM64 game runtime libraries.'
done
if [ $include_assets -eq 1 ]; then
    unzip -Zl "$obb" | awk '$1 ~ /^-/ && $NF ~ /^assets\// { print "OBB | " $NF " | " $4 }' >>"$entries"
fi

if [ $dry_run -eq 0 ]; then
    mkdir -p "$dest"
    # Preserve the original ZIP unchanged so the NRO reads the exact Quest classes.dex.
    cp -f "$apk" "$dest/beat-saber.apk"
    [ "$(sha256 "$dest/beat-saber.apk")" = "$(sha256 "$apk")" ] || die 'Original APK transfer failed its SHA256 identity check.'
    # libnx loader opens these as ./libunity.so rather than through dlopen().
    unzip -qq -o -j "$apk" 'lib/arm64-v8a/*.so' -d "$dest"
    if [ $include_assets -eq 1 ]; then
        unzip -qq -o "$apk" 'assets/*' -d "$dest"
        unzip -qq -o "$obb" 'assets/*' -d "$dest"
    fi
    cp -f "$nro" "$dest/BeatSaberQuestLoader.nro"
    { printf '%s\n' "${header[@]}"; cat "$entries"; } >"$dest/local-files.txt"
fi

awk -F' \\| ' -v assets=$include_assets '{ s += $3; n++ } END {
    printf "Prepared %d archive entries, %.2f GiB uncompressed, assets=%s\n", n, s / 1073741824, assets ? "True" : "False" }' "$entries"
if [ $dry_run -eq 1 ]; then
    echo 'Dry run finished: no game files copied.'
else
    echo 'Copy finished. Open Homebrew Menu in full-memory title mode, run BeatSaberQuestLoader.nro.'
fi
