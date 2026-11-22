#!/usr/bin/env bash
# One-step setup (Linux/macOS port of setup.ps1): turns your own Beat Saber
# Quest APK + OBB into a ready SD card folder for the Switch / Switch 2 loader.
#
# Put this script in a folder together with:
#   - your Beat Saber Quest 1.46.0_34060 APK   (any *.apk name)
#   - main.3491.com.beatgames.beatsaber.obb   (any *.obb name)
# then run ./setup.sh. It validates the files, extracts the ARM64 libraries and
# Unity data into <destination>/switch/beat-saber-quest/, and downloads the
# loader NRO from the latest GitHub release (or uses --nro).
# No game files ever leave your PC; nothing is uploaded.
#
# Usage: ./setup.sh [--console switch|switch2|both] [--destination DIR]
#                   [--nro FILE] [--skip-download] [--repository OWNER/REPO]
# Requires: bash, unzip, curl (only for downloading), sha256sum or shasum.
set -euo pipefail

console=ask
destination=''
nro_path=''
skip_download=0
repository='RobinVRL/Beatsaber-NSW'

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --console)       console=$(printf '%s' "${2:?}" | tr '[:upper:]' '[:lower:]'); shift 2 ;;
        --destination)   destination=${2:?}; shift 2 ;;
        --nro)           nro_path=${2:?}; shift 2 ;;
        --skip-download) skip_download=1; shift ;;
        --repository)    repository=${2:?}; shift 2 ;;
        -h|--help)       usage 0 ;;
        *) echo "Unknown option: $1" >&2; usage 1 ;;
    esac
done

here=$(cd "$(dirname "$0")" && pwd)
app_dir_name='beat-saber-quest'
expected_version_code='3491'
required_libs=(libmain.so libunity.so libil2cpp.so libgame.so libc++_shared.so)

if [ -t 1 ]; then cyan=$'\e[36m'; red=$'\e[31m'; green=$'\e[32m'; yellow=$'\e[33m'; reset=$'\e[0m'
else cyan=''; red=''; green=''; yellow=''; reset=''; fi
step() { printf '\n%s==> %s%s\n' "$cyan" "$1" "$reset"; }
fail() { printf '\n%sERROR: %s%s\n' "$red" "$1" "$reset" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "'$1' is required. Install it with your package manager (e.g. sudo apt install $1)."; }
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
file_size() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }

need unzip

# ---- 1. locate inputs -------------------------------------------------------
step "Looking for your APK and OBB in $here"
shopt -s nullglob nocaseglob
apks=("$here"/*.apk)
obbs=("$here"/*.obb)
shopt -u nocaseglob
[ ${#apks[@]} -eq 0 ] && fail "No .apk found next to setup.sh. Copy your Beat Saber Quest APK into: $here"
[ ${#obbs[@]} -eq 0 ] && fail "No .obb found next to setup.sh. Copy main.$expected_version_code.com.beatgames.beatsaber.obb into: $here"
[ ${#apks[@]} -gt 1 ] && fail "More than one .apk found. Keep only the Beat Saber one."
if [ ${#obbs[@]} -gt 1 ]; then
    match=("$here"/main."$expected_version_code".*.obb)
    [ ${#match[@]} -eq 1 ] || fail "More than one .obb found. Keep only main.$expected_version_code.com.beatgames.beatsaber.obb."
    obbs=("${match[@]}")
fi
apk=${apks[0]}
obb=${obbs[0]}
echo "  APK: $(basename "$apk") ($(( $(file_size "$apk") / 1048576 )) MB)"
echo "  OBB: $(basename "$obb") ($(( $(file_size "$obb") / 1048576 )) MB)"

# ---- 2. validate ------------------------------------------------------------
step 'Checking the files'
apk_names=$(unzip -Z1 "$apk" 2>/dev/null) || fail "APK is not a valid archive (corrupt or incomplete copy?): $apk"
for lib in "${required_libs[@]}"; do
    grep -qxF "lib/arm64-v8a/$lib" <<<"$apk_names" ||
        fail "APK is missing lib/arm64-v8a/$lib. This is not the Quest (ARM64) Beat Saber APK."
done
grep -qxF 'assets/bin/Data/boot.config' <<<"$apk_names" || fail 'APK has no Unity boot.config. Wrong APK?'
grep -qxF 'classes.dex' <<<"$apk_names" || fail 'APK has no classes.dex. Wrong or stripped APK?'
obb_names=$(unzip -Z1 "$obb" 2>/dev/null) || fail "OBB is not a valid archive (corrupt or incomplete copy?): $obb"
grep -qxF 'assets/aa/catalog.json' <<<"$obb_names" || fail 'OBB has no assets/aa/catalog.json. This is not the Beat Saber main OBB.'
case "$(basename "$obb")" in
    main."$expected_version_code".*) ;;
    *) echo "  ${yellow}WARNING: OBB name does not contain version code $expected_version_code."
       echo "           The loader targets Beat Saber 1.46.0_34060; other versions will likely not work.$reset" ;;
esac
# Reject archive entries that could escape the destination.
if grep -E '(^|/)\.\.(/|$)|^/' <<<"$apk_names"$'\n'"$obb_names" | grep -q .; then
    fail 'Archive contains unsafe entry paths.'
fi
echo "  ${green}APK and OBB look like Beat Saber Quest.$reset"

# ---- 3. choose console / destination ---------------------------------------
if [ "$console" = ask ] && [ -z "$nro_path" ] && [ $skip_download -eq 0 ]; then
    printf '\nWhich console is this for?\n  1) Nintendo Switch\n  2) Nintendo Switch 2\n  3) Both\n'
    read -r -p 'Enter 1, 2 or 3: ' choice
    case "$choice" in
        1) console=switch ;; 2) console=switch2 ;; 3) console=both ;;
        *) fail 'Please enter 1, 2 or 3.' ;;
    esac
fi
case "$console" in ask|switch|switch2|both) ;; *) fail "--console must be switch, switch2 or both." ;; esac
default_destination=0
if [ -z "$destination" ]; then destination="$here/SD"; default_destination=1; fi
mkdir -p "$destination"
destination=$(cd "$destination" && pwd)
target="$destination/switch/$app_dir_name"
[ "$target" = "$here" ] && fail 'Destination must not be the folder holding your APK/OBB.'

# Free-space check against the uncompressed size we will write.
sum_sizes() { unzip -Zl "$1" | awk '$1 ~ /^-/ && ($NF ~ /^assets\// || $NF ~ /^lib\/arm64-v8a\/[^\/]+\.so$/) { s += $4 } END { print s + 0 }'; }
needed=$(( $(file_size "$apk") + $(sum_sizes "$apk") + $(sum_sizes "$obb") ))
free_kb=$(df -Pk "$destination" | awk 'NR == 2 { print $4 }')
if [ -n "$free_kb" ] && [ $(( free_kb * 1024 )) -lt $(( needed + 104857600 )) ]; then
    fail "$(awk -v n="$needed" -v f="$free_kb" 'BEGIN { printf "Not enough free space: need %.1f GB, have %.1f GB.", n / 1073741824, f / 1048576 }')"
fi
printf '\nOutput: %s  (~%s GB)\n' "$target" "$(awk -v n="$needed" 'BEGIN { printf "%.1f", n / 1073741824 }')"

# ---- 4. extract -------------------------------------------------------------
manifest=$(mktemp)
trap 'rm -f "$manifest"' EXIT
{
    echo '# Private owner-supplied game files; never distribute'
    echo '# Quest version 1.46.0_34060; Unity 6000.3.19f1'
} >"$manifest"

step 'Copying the original APK (the loader reads classes.dex from it)'
mkdir -p "$target"
cp -f "$apk" "$target/beat-saber.apk"
[ "$(sha256 "$target/beat-saber.apk")" = "$(sha256 "$apk")" ] || fail 'APK copy failed verification.'
echo "APK | beat-saber.apk | $(file_size "$apk")" >>"$manifest"

step 'Extracting APK contents'
# The loader opens ./libunity.so etc., so libraries go flat into the target.
unzip -qq -o -j "$apk" 'lib/arm64-v8a/*.so' -d "$target"
unzip -qq -o "$apk" 'assets/*' -d "$target"
unzip -Zl "$apk" | awk '$1 ~ /^-/ {
    if ($NF ~ /^lib\/arm64-v8a\/[^\/]+\.so$/) { n = $NF; sub(/.*\//, "", n); print "APK | " n " | " $4 }
    else if ($NF ~ /^assets\//) print "APK | " $NF " | " $4 }' >>"$manifest"
echo "  $(grep -c '^APK' "$manifest") files"

step 'Extracting OBB contents'
unzip -qq -o "$obb" 'assets/*' -d "$target"
unzip -Zl "$obb" | awk '$1 ~ /^-/ && $NF ~ /^assets\// { print "OBB | " $NF " | " $4 }' >>"$manifest"
echo "  $(grep -c '^OBB' "$manifest") files"

# ---- 5. loader NRO ----------------------------------------------------------
case "$console" in
    switch)  nro_names=(Beatsaber.nro) ;;
    switch2) nro_names=(BeatsaberNS2.nro) ;;
    both)    nro_names=(Beatsaber.nro BeatsaberNS2.nro) ;;
    *)       nro_names=() ;;
esac
installed=()
if [ -n "$nro_path" ]; then
    step 'Copying your local NRO'
    [ -f "$nro_path" ] || fail "NRO not found: $nro_path"
    cp -f "$nro_path" "$target/"
    installed+=("$(basename "$nro_path")")
elif [ $skip_download -eq 0 ]; then
    need curl
    step "Downloading the loader from github.com/$repository"
    # /releases (not /releases/latest) so pre-releases are included.
    releases=$(curl -fsSL -H 'User-Agent: beatsaber-nsw-setup' -H 'Accept: application/vnd.github+json' \
        "https://api.github.com/repos/$repository/releases?per_page=10") ||
        fail 'Could not reach GitHub. Re-run with --nro <file.nro> to use a downloaded NRO.'
    # Skip releases of the separate 1.40.8 port (v1408-*).
    tag=$(grep -o '"tag_name": *"[^"]*"' <<<"$releases" | sed 's/.*"\([^"]*\)"$/\1/' | grep -v '^v1408-' | head -n1)
    [ -n "$tag" ] || fail 'No published release found.'
    echo "  Release: $tag"
    for n in "${nro_names[@]}"; do
        url="https://github.com/$repository/releases/download/$tag/$n"
        echo "  $n..."
        curl -fL --progress-bar -H 'User-Agent: beatsaber-nsw-setup' -o "$target/$n" "$url" ||
            { rm -f "$target/$n"; fail "Release $tag has no $n (or the download failed)."; }
        installed+=("$n")
    done
fi
{
    head -n 2 "$manifest"
    for n in ${installed[@]+"${installed[@]}"}; do echo "# Loader $n SHA256: $(sha256 "$target/$n")"; done
    tail -n +3 "$manifest"
} >"$target/local-files.txt"

# ---- done -------------------------------------------------------------------
printf '\n%sSetup complete.%s\nGame folder: %s\n' "$green" "$reset" "$target"
if [ $default_destination -eq 1 ]; then
    printf "\nNext: copy the 'switch' folder from\n  %s\nto the root of your SD card (merge with the existing switch folder).\n" "$destination"
fi
if [ ${#installed[@]} -eq 0 ]; then
    printf '\nNo NRO was installed. Put Beatsaber.nro (Switch) or BeatsaberNS2.nro (Switch 2) into %s\n' "$target"
    launch='the NRO'
else
    launch=$(printf '%s / ' "${installed[@]}"); launch=${launch% / }
fi
echo
echo 'On the console, open the Homebrew Menu in full-memory (title override) mode,'
echo "not applet mode, and launch $launch from switch/$app_dir_name."
echo 'Never share the generated folder: it contains your copy of the game.'
