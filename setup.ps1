<#
.SYNOPSIS
 One-step setup: turns your own Beat Saber Quest APK + OBB into a ready SD card
 folder for the Switch / Switch 2 loader.
.DESCRIPTION
 Put this script in a folder together with:
   - your Beat Saber Quest 1.46.0_34060 APK   (any *.apk name)
   - main.3491.com.beatgames.beatsaber.obb   (any *.obb name)
 then run it (or double-click setup.bat). It:
   1. finds and validates the APK and OBB,
   2. extracts the original ARM64 libraries and Unity data into
      <Destination>\switch\beat-saber-quest\ (the layout the loader expects),
   3. downloads the loader NRO from the latest GitHub release (or uses -NroPath).
 Copy the resulting "switch" folder to the root of your SD card, or pass
 -Destination with the SD card's drive letter to write there directly.
 No game files ever leave your PC; nothing is uploaded.
.PARAMETER Console
 Switch (Beatsaber.nro), Switch2 (BeatsaberNS2.nro), or Both. Default: ask.
.PARAMETER Destination
 Folder that receives "switch\beat-saber-quest". Default: an "SD" folder next
 to this script. Use e.g. "E:\" to write straight to an SD card.
.PARAMETER NroPath
 Use a locally built NRO instead of downloading one.
.PARAMETER SkipDownload
 Stage only the game files; copy an NRO yourself later.
.EXAMPLE
 .\setup.ps1
.EXAMPLE
 .\setup.ps1 -Console Switch2 -Destination E:\
#>
[CmdletBinding()]
param(
    [ValidateSet('Ask', 'Switch', 'Switch2', 'Both')]
    [string]$Console = 'Ask',
    [string]$Destination = '',
    [string]$NroPath = '',
    [switch]$SkipDownload,
    [string]$Repository = 'RobinVRL/Beatsaber-NSW'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression          # ZipArchive (needed explicitly on PS 5.1)
Add-Type -AssemblyName System.IO.Compression.FileSystem

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$appDirName = 'beat-saber-quest'
$expectedVersionCode = '3491'
$requiredLibs = @('libmain.so', 'libunity.so', 'libil2cpp.so', 'libgame.so', 'libc++_shared.so')

function Step([string]$msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Fail([string]$msg) {
    Write-Host "`nERROR: $msg" -ForegroundColor Red
    throw $msg
}

# ---- 1. locate inputs -------------------------------------------------------
Step "Looking for your APK and OBB in $here"
$apks = @(Get-ChildItem -LiteralPath $here -File -Filter '*.apk')
$obbs = @(Get-ChildItem -LiteralPath $here -File -Filter '*.obb')
if ($apks.Count -eq 0) { Fail "No .apk found next to setup.ps1. Copy your Beat Saber Quest APK into: $here" }
if ($obbs.Count -eq 0) { Fail "No .obb found next to setup.ps1. Copy main.$expectedVersionCode.com.beatgames.beatsaber.obb into: $here" }
if ($apks.Count -gt 1) { Fail "More than one .apk found ($($apks.Name -join ', ')). Keep only the Beat Saber one." }
if ($obbs.Count -gt 1) {
    $match = @($obbs | Where-Object { $_.Name -like "main.$expectedVersionCode.*" })
    if ($match.Count -ne 1) { Fail "More than one .obb found ($($obbs.Name -join ', ')). Keep only main.$expectedVersionCode.com.beatgames.beatsaber.obb." }
    $obbs = $match
}
$apk = $apks[0].FullName
$obb = $obbs[0].FullName
Write-Host "  APK: $($apks[0].Name) ($([math]::Round($apks[0].Length / 1MB)) MB)"
Write-Host "  OBB: $($obbs[0].Name) ($([math]::Round($obbs[0].Length / 1MB)) MB)"

# ---- 2. validate ------------------------------------------------------------
Step 'Checking the files'
# Shared read so antivirus scans or an open file manager don't block us.
function Open-Zip([string]$path, [string]$kind) {
    $fs = $null
    for ($try = 1; $try -le 5 -and -not $fs; $try++) {
        try { $fs = New-Object IO.FileStream($path, 'Open', 'Read', 'ReadWrite, Delete') }
        catch [System.IO.IOException] {
            if ($try -eq 5) { Fail "$kind is locked by another program (close emulators, ADB transfers or file managers using it): $path" }
            Start-Sleep -Seconds 2
        }
    }
    try { return New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Read, $false) }
    catch { $fs.Dispose(); Fail "$kind is not a valid archive (corrupt or incomplete copy?): $path" }
}
$apkZip = Open-Zip $apk 'APK'
try {
    $apkNames = @($apkZip.Entries | ForEach-Object { $_.FullName })
    foreach ($lib in $requiredLibs) {
        if ($apkNames -notcontains "lib/arm64-v8a/$lib") {
            Fail "APK is missing lib/arm64-v8a/$lib. This is not the Quest (ARM64) Beat Saber APK."
        }
    }
    if ($apkNames -notcontains 'assets/bin/Data/boot.config') { Fail 'APK has no Unity boot.config. Wrong APK?' }
    if ($apkNames -notcontains 'classes.dex') { Fail 'APK has no classes.dex. Wrong or stripped APK?' }
} finally { $apkZip.Dispose() }

$obbZip = Open-Zip $obb 'OBB'
try {
    if (-not ($obbZip.Entries | Where-Object { $_.FullName -eq 'assets/aa/catalog.json' })) {
        Fail 'OBB has no assets/aa/catalog.json. This is not the Beat Saber main OBB.'
    }
} finally { $obbZip.Dispose() }
if ($obbs[0].Name -notlike "main.$expectedVersionCode.*") {
    Write-Host "  WARNING: OBB name does not contain version code $expectedVersionCode." -ForegroundColor Yellow
    Write-Host '           The loader targets Beat Saber 1.46.0_34060; other versions will likely not work.' -ForegroundColor Yellow
}
Write-Host '  APK and OBB look like Beat Saber Quest.' -ForegroundColor Green

# ---- 3. choose console / destination ---------------------------------------
if ($Console -eq 'Ask' -and -not $NroPath -and -not $SkipDownload) {
    Write-Host "`nWhich console is this for?"
    Write-Host '  1) Nintendo Switch'
    Write-Host '  2) Nintendo Switch 2'
    Write-Host '  3) Both'
    switch (Read-Host 'Enter 1, 2 or 3') {
        '1' { $Console = 'Switch' }
        '2' { $Console = 'Switch2' }
        '3' { $Console = 'Both' }
        default { Fail 'Please enter 1, 2 or 3.' }
    }
}
if (-not $Destination) { $Destination = Join-Path $here 'SD' }
$target = [IO.Path]::GetFullPath((Join-Path (Join-Path $Destination 'switch') $appDirName))
if ($target -eq [IO.Path]::GetFullPath($here).TrimEnd('\')) {
    Fail 'Destination must not be the folder holding your APK/OBB.'
}

# Free-space check against the uncompressed size we will write.
$needed = (Get-Item -LiteralPath $apk).Length
foreach ($p in @($apk, $obb)) {
    $z = Open-Zip $p 'Archive'
    try { $needed += ($z.Entries | Where-Object { $_.FullName.StartsWith('assets/') -or $_.FullName -like 'lib/arm64-v8a/*.so' } |
                      Measure-Object -Property Length -Sum).Sum }
    finally { $z.Dispose() }
}
$root = [IO.Path]::GetPathRoot($target)
try {
    $free = (New-Object IO.DriveInfo $root).AvailableFreeSpace
    if ($free -lt $needed + 100MB) {
        Fail ("Not enough free space on {0}: need {1:N1} GB, have {2:N1} GB." -f $root, ($needed / 1GB), ($free / 1GB))
    }
} catch [System.ArgumentException] { }  # network paths: skip the check
Write-Host ("`nOutput: {0}  (~{1:N1} GB)" -f $target, ($needed / 1GB))

# ---- 4. extract -------------------------------------------------------------
function Save-Entry([IO.Compression.ZipArchiveEntry]$entry, [string]$relative) {
    $parts = $relative.Split('/')
    if ($parts | Where-Object { $_ -in @('', '.', '..') -or $_.Contains(':') }) { Fail "Unsafe archive entry: $relative" }
    $out = [IO.Path]::GetFullPath((Join-Path $target ($relative -replace '/', '\')))
    if (-not $out.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) { Fail "Archive entry escapes destination: $relative" }
    [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out))
    $in = $entry.Open()
    try {
        $fs = [IO.File]::Create($out)
        try { $in.CopyTo($fs) } finally { $fs.Dispose() }
    } finally { $in.Dispose() }
    if ((Get-Item -LiteralPath $out).Length -ne $entry.Length) { Fail "Size mismatch while writing $relative" }
}

$manifest = [Collections.Generic.List[string]]::new()
$manifest.Add('# Private owner-supplied game files; never distribute')
$manifest.Add('# Quest version 1.46.0_34060; Unity 6000.3.19f1')

Step 'Copying the original APK (the loader reads classes.dex from it)'
[void](New-Item -ItemType Directory -Force -Path $target)
$stagedApk = Join-Path $target 'beat-saber.apk'
Copy-Item -LiteralPath $apk -Destination $stagedApk -Force
if ((Get-FileHash -LiteralPath $stagedApk).Hash -ne (Get-FileHash -LiteralPath $apk).Hash) { Fail 'APK copy failed verification.' }
$manifest.Add(('APK | beat-saber.apk | {0}' -f (Get-Item -LiteralPath $apk).Length))

foreach ($src in @(@{ Path = $apk; Kind = 'APK' }, @{ Path = $obb; Kind = 'OBB' })) {
    Step "Extracting $($src.Kind) contents"
    $zip = Open-Zip $src.Path $src.Kind
    try {
        $work = @($zip.Entries | Where-Object {
            -not $_.FullName.EndsWith('/') -and
            ($_.FullName.StartsWith('assets/') -or ($src.Kind -eq 'APK' -and $_.FullName -match '^lib/arm64-v8a/[^/]+\.so$'))
        })
        $i = 0
        foreach ($e in $work) {
            $i++
            if ($i % 50 -eq 0 -or $i -eq $work.Count) {
                Write-Progress -Activity "Extracting $($src.Kind)" -Status "$i / $($work.Count)" -PercentComplete (100 * $i / $work.Count)
            }
            if ($e.FullName -match '^lib/arm64-v8a/([^/]+\.so)$') {
                Save-Entry $e $matches[1]          # loader opens ./libunity.so etc.
                $manifest.Add(('APK | {0} | {1}' -f $matches[1], $e.Length))
            } else {
                Save-Entry $e $e.FullName
                $manifest.Add(('{0} | {1} | {2}' -f $src.Kind, $e.FullName, $e.Length))
            }
        }
        Write-Progress -Activity "Extracting $($src.Kind)" -Completed
        Write-Host "  $($work.Count) files"
    } finally { $zip.Dispose() }
}

# ---- 5. loader NRO ----------------------------------------------------------
$nroNames = switch ($Console) {
    'Switch'  { @('Beatsaber.nro') }
    'Switch2' { @('BeatsaberNS2.nro') }
    'Both'    { @('Beatsaber.nro', 'BeatsaberNS2.nro') }
    default   { @() }
}
$installed = @()
if ($NroPath) {
    Step 'Copying your local NRO'
    if (-not (Test-Path -LiteralPath $NroPath -PathType Leaf)) { Fail "NRO not found: $NroPath" }
    $name = Split-Path -Leaf $NroPath
    Copy-Item -LiteralPath $NroPath -Destination (Join-Path $target $name) -Force
    $installed += $name
} elseif (-not $SkipDownload) {
    Step "Downloading the loader from github.com/$Repository"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $headers = @{ 'User-Agent' = 'beatsaber-nsw-setup'; 'Accept' = 'application/vnd.github+json' }
    try {
        # /releases (not /releases/latest) so pre-releases are included.
        $releases = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases?per_page=10"
    } catch { Fail "Could not reach GitHub ($($_.Exception.Message)). Re-run with -NroPath <file.nro> to use a downloaded NRO." }
    $release = @($releases | Where-Object { -not $_.draft })[0]
    if (-not $release) { Fail 'No published release found.' }
    Write-Host "  Release: $($release.name) ($($release.tag_name))"
    foreach ($n in $nroNames) {
        $asset = @($release.assets | Where-Object { $_.name -eq $n })[0]
        if (-not $asset) { Fail "Release $($release.tag_name) has no $n." }
        $out = Join-Path $target $n
        Write-Host ("  {0} ({1:N1} MB)..." -f $n, ($asset.size / 1MB))
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'  # PS5 progress bar is very slow
        try { Invoke-WebRequest -Headers $headers -Uri $asset.browser_download_url -OutFile $out -UseBasicParsing }
        finally { $ProgressPreference = $old }
        if ((Get-Item -LiteralPath $out).Length -ne $asset.size) { Fail "Download of $n was incomplete." }
        $installed += $n
    }
}
foreach ($n in $installed) {
    $manifest.Insert(2, ('# Loader {0} SHA256: {1}' -f $n, (Get-FileHash -LiteralPath (Join-Path $target $n)).Hash.ToLowerInvariant()))
}
[IO.File]::WriteAllLines((Join-Path $target 'local-files.txt'), [string[]]$manifest)

# ---- done -------------------------------------------------------------------
Write-Host "`nSetup complete." -ForegroundColor Green
Write-Host "Game folder: $target"
if ($Destination -eq (Join-Path $here 'SD')) {
    Write-Host "`nNext: copy the 'switch' folder from"
    Write-Host "  $(Join-Path $here 'SD')"
    Write-Host 'to the root of your SD card (merge with the existing switch folder).'
}
if ($installed.Count -eq 0) {
    Write-Host "`nNo NRO was installed. Put Beatsaber.nro (Switch) or BeatsaberNS2.nro (Switch 2) into $target"
}
Write-Host "`nOn the console, open the Homebrew Menu in full-memory (title override) mode,"
Write-Host "not applet mode, and launch $(if ($installed.Count) { $installed -join ' / ' } else { 'the NRO' }) from switch/$appDirName."
Write-Host 'Never share the generated folder: it contains your copy of the game.'
