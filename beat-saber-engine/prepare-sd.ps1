<#
.SYNOPSIS
 Stage the user's locally owned Beat Saber Quest runtime for Switch homebrew.
.DESCRIPTION
 Copies the latest lifecycle-verified homebrew loader, locally owned Quest APK
 (for its original classes.dex), and original Android ARM64 libraries to an
 explicit destination directory. With -IncludeAssets, extracts APK/OBB Unity
 data into the same sdmc:/switch/beat-saber-quest/ relative layout.
 It never embeds game binaries or licensed media inside the NRO.
 .EXAMPLE
 .\prepare-sd.ps1 -Destination 'D:\switch\beat-saber-quest' -DryRun -IncludeAssets
#>
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [switch]$IncludeAssets,
    [switch]$DryRun,
    [switch]$LoaderOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = Split-Path -Parent $project
$source = Join-Path $port 'SwitchPort'
$apk = Join-Path $source 'beat-saber.apk'
$obb = Join-Path $source 'main.3491.com.beatgames.beatsaber.obb'
$nro = Join-Path $project 'BeatSaberQuestLoaderLifecycle.nro'
$appDirName = 'beat-saber-quest'

foreach ($item in @($apk,$obb,$nro)) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) {
        throw "Required original/build file is missing: $item"
    }
}

$destinationFull = [IO.Path]::GetFullPath($Destination)
if ([IO.Path]::GetFileName($destinationFull.TrimEnd('\','/')) -ne $appDirName) {
    throw "Destination must end with '$appDirName' (Switch path: sdmc:/switch/$appDirName/)."
}
if ($destinationFull.StartsWith([IO.Path]::GetFullPath($source),
                                [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Do not stage into the original owned APK/OBB source folder.'
}
$expectedApkVersion = '3491'
if (-not ([IO.Path]::GetFileName($obb) -like "main.$expectedApkVersion.*.obb")) {
    throw "OBB does not match expected version code $expectedApkVersion"
}

$loaderDigest = (Get-FileHash -LiteralPath $nro -Algorithm SHA256).Hash.ToLowerInvariant()
if ($LoaderOnly) {
    if ($IncludeAssets) { throw 'LoaderOnly preserves existing assets; omit -IncludeAssets.' }
    if (-not (Test-Path -LiteralPath $destinationFull -PathType Container)) {
        throw 'LoaderOnly requires an existing, prepared private SD folder.'
    }
    $expectedModules = @(
        'libmain.so', 'libc++_shared.so', 'libunity.so', 'libil2cpp.so',
        'libgame.so', 'lib_burst_generated.so', 'libopenxr_loader.so',
        'libUnityOpenXR.so', 'libOVRPlugin.so', 'libOVRMetricsTool.so',
        'libovrplatformloader.so', 'libhaptics_sdk.so', 'libenet.so',
        'libcrypto.so', 'libssl.so'
    )
    foreach ($module in $expectedModules) {
        if (-not (Test-Path -LiteralPath (Join-Path $destinationFull $module) -PathType Leaf)) {
            throw "LoaderOnly blocked: original Quest library missing: $module"
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $destinationFull 'assets/bin/Data/boot.config') -PathType Leaf)) {
        throw 'LoaderOnly blocked: original Unity boot.config is not staged.'
    }
    $stagedApk = Join-Path $destinationFull 'beat-saber.apk'
    if (-not (Test-Path -LiteralPath $stagedApk -PathType Leaf) -or
        (Get-FileHash -LiteralPath $stagedApk -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash) {
        throw 'LoaderOnly blocked: byte-identical original APK (classes.dex) is not staged.'
    }
    $manifestFile = Join-Path $destinationFull 'local-files.txt'
    if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) {
        throw 'LoaderOnly blocked: original owner-only manifest is missing.'
    }
    if (-not $DryRun) {
        Copy-Item -LiteralPath $nro -Destination (Join-Path $destinationFull 'BeatSaberQuestLoader.nro') -Force
        $originalEntries = @([IO.File]::ReadAllLines($manifestFile) | Where-Object {
            $_ -notlike '# Loader NRO SHA256:*' -and $_ -notlike '# Diagnostic:*'
        })
        $revised = @(
            "# Loader NRO SHA256: $loaderDigest",
            '# Diagnostic: original ELF/TLS/JNI, GameActivity preflight, VI frame scheduling, MediaNDK metadata; original Unity gameplay requires missing services.'
        ) + $originalEntries
        [IO.File]::WriteAllLines($manifestFile, [string[]]$revised)
    }
    Write-Host "LoaderOnly $(if ($DryRun) { 'dry run' } else { 'updated' }): 15 private Quest libraries and original boot.config verified; no original media rewritten."
    Write-Host "NRO SHA256: $loaderDigest"
    return
}

function Copy-SourceEntry {
    param([IO.Compression.ZipArchiveEntry]$Entry, [string]$TargetDir)
    if ($Entry.FullName.EndsWith('/')) { return }
    $name = $Entry.FullName.Replace('/','\')
    $segments = $name.Split('\')
    if ($segments | Where-Object { $_ -in @('','.','..') -or $_.Contains(':') }) {
        throw "Unsafe archive entry: $name"
    }
    $result = [IO.Path]::GetFullPath((Join-Path $TargetDir $name))
    $rootWithSep = $TargetDir.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $result.StartsWith($rootWithSep, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Archive entry escapes destination: $name"
    }
    if (-not $DryRun) {
        $parent = Split-Path -Parent $result
        [void](New-Item -ItemType Directory -Force -Path $parent)
        $inputStream = $Entry.Open()
        try {
            $outputStream = [IO.File]::Create($result)
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose() }
        } finally { $inputStream.Dispose() }
        if ((Get-Item -LiteralPath $result).Length -ne $Entry.Length) {
            throw "Staging size mismatch: $result"
        }
    }
}

Write-Host "Staging locally owned Quest Beat Saber to: $destinationFull"
Write-Host "Mode: $(if ($DryRun) { 'inspect only' } else { 'copy files' })"
$files = [Collections.Generic.List[object]]::new()
# Preserve the original ZIP container unchanged so the running NRO can read
# the exact Quest classes.dex, not reconstructed/synthetic Java bytecode.
$files.Add([pscustomobject]@{
    Origin='APK';Name='beat-saber.apk';Bytes=(Get-Item -LiteralPath $apk).Length
})
if (-not $DryRun) {
    [void](New-Item -ItemType Directory -Force -Path $destinationFull)
    $stagedApk = Join-Path $destinationFull 'beat-saber.apk'
    Copy-Item -LiteralPath $apk -Destination $stagedApk -Force
    if ((Get-FileHash -LiteralPath $stagedApk -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash) {
        throw 'Original APK transfer failed its SHA256 identity check.'
    }
}
$apkArchive = [IO.Compression.ZipFile]::OpenRead($apk)
try {
    foreach ($entry in $apkArchive.Entries) {
        $name = $entry.FullName
        if ($name -match '^lib/arm64-v8a/([^/]+\.so)$') {
            $basename = $matches[1]
            $files.Add([pscustomobject]@{Origin='APK';Name=$basename;Bytes=$entry.Length})
            # libnx loader opens these as ./libunity.so rather than through dlopen().
            if (-not $DryRun) {
                [void](New-Item -ItemType Directory -Force -Path $destinationFull)
                $stream = $entry.Open()
                try {
                    $out = [IO.File]::Create((Join-Path $destinationFull $basename))
                    try { $stream.CopyTo($out) } finally { $out.Dispose() }
                } finally { $stream.Dispose() }
            }
        }
        if ($IncludeAssets -and $name.StartsWith('assets/',[StringComparison]::Ordinal)) {
            Copy-SourceEntry -Entry $entry -TargetDir $destinationFull
            $files.Add([pscustomobject]@{Origin='APK';Name=$name;Bytes=$entry.Length})
        }
    }
} finally { $apkArchive.Dispose() }

if (-not ($files | Where-Object { $_.Name -eq 'libunity.so' }) -or
    -not ($files | Where-Object { $_.Name -eq 'libil2cpp.so' }) -or
    -not ($files | Where-Object { $_.Name -eq 'libgame.so' })) {
    throw 'APK is missing required ARM64 game runtime libraries.'
}

if ($IncludeAssets) {
    $obbArchive = [IO.Compression.ZipFile]::OpenRead($obb)
    try {
        foreach ($entry in $obbArchive.Entries) {
            if (-not $entry.FullName.StartsWith('assets/',[StringComparison]::Ordinal)) { continue }
            Copy-SourceEntry -Entry $entry -TargetDir $destinationFull
            $files.Add([pscustomobject]@{Origin='OBB';Name=$entry.FullName;Bytes=$entry.Length})
        }
    } finally { $obbArchive.Dispose() }
}

if (-not $DryRun) {
    Copy-Item -LiteralPath $nro -Destination (Join-Path $destinationFull 'BeatSaberQuestLoader.nro') -Force
    $manifest = @(
        '# Private owner-supplied game files; never distribute'
        '# Quest version 1.46.0_34060; Unity 6000.3.19f1'
        "# Loader NRO SHA256: $loaderDigest"
        '# Diagnostic: original ELF/TLS/JNI, GameActivity preflight, VI frame scheduling, MediaNDK metadata; original Unity gameplay requires missing services.'
    )
    $manifest += @($files | ForEach-Object {
        '{0} | {1} | {2}' -f $_.Origin,$_.Name,$_.Bytes
    })
    [IO.File]::WriteAllLines((Join-Path $destinationFull 'local-files.txt'), [string[]]$manifest)
}
$aggregate = ($files | Measure-Object -Property Bytes -Sum).Sum
Write-Host ('Prepared {0} archive entries, {1:N2} GiB uncompressed, assets={2}' -f
            $files.Count,([double]$aggregate/1GB),[bool]$IncludeAssets)
if ($DryRun) {
    Write-Host 'Dry run finished: no game files copied.'
} else {
    Write-Host 'Copy finished. Open Homebrew Menu in full-memory title mode, run BeatSaberQuestLoader.nro.'
    Write-Host 'Original ELF/TLS, guarded libmain/IL2CPP JNI, GameActivity preflight and Android bridge diagnostics are available; gameplay remains unavailable.'
}
