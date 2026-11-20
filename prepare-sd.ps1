<#
.SYNOPSIS
 Stage the user's own Beat Saber Quest 1.40.8 (optionally MBF-modded) files
 for the separate 1.40.8 Switch loader (sdmc:/switch/beat-saber-1408/).
.EXAMPLE
 .\prepare-sd.ps1 -Destination "$env:APPDATA\eden\sdmc\switch\beat-saber-1408" -IncludeAssets -IncludeMods
#>
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Source = '',
    [switch]$IncludeAssets,
    [switch]$IncludeMods,
    [switch]$LoaderOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = Split-Path -Parent $project
if (-not $Source) { $Source = Join-Path $port 'port-1.40.8-modded\quest-files' }
$apk = Join-Path $Source 'beat-saber.apk'
$obb = Join-Path $Source 'obb\main.1716.com.beatgames.beatsaber.obb'
$modData = Join-Path $Source 'ModData'
$nro = Join-Path $project 'BeatSaber1408.nro'
$appDirName = 'beat-saber-1408'

foreach ($item in @($apk, $obb, $nro)) {
    if (-not (Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing: $item" }
}
$dest = [IO.Path]::GetFullPath($Destination)
if ([IO.Path]::GetFileName($dest.TrimEnd('\','/')) -ne $appDirName) {
    throw "Destination must end with '$appDirName' (Switch path: sdmc:/switch/$appDirName/)."
}
[void](New-Item -ItemType Directory -Force -Path $dest)
Copy-Item -LiteralPath $nro -Destination (Join-Path $dest 'BeatSaber1408.nro') -Force
if ($LoaderOnly) { Write-Host "Loader updated in $dest"; return }

function Save-Entry([IO.Compression.ZipArchiveEntry]$Entry, [string]$Target) {
    $full = [IO.Path]::GetFullPath($Target)
    if (-not $full.StartsWith($dest, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe entry: $($Entry.FullName)" }
    [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $full))
    $in = $Entry.Open()
    try { $out = [IO.File]::Create($full); try { $in.CopyTo($out) } finally { $out.Dispose() } }
    finally { $in.Dispose() }
}

Copy-Item -LiteralPath $apk -Destination (Join-Path $dest 'beat-saber.apk') -Force
$count = 0
$zip = [IO.Compression.ZipFile]::OpenRead($apk)
try {
    foreach ($e in $zip.Entries) {
        if ($e.FullName -match '^lib/arm64-v8a/([^/]+\.so)$') {
            Save-Entry $e (Join-Path $dest $matches[1]); $count++
        } elseif ($IncludeAssets -and $e.FullName.StartsWith('assets/') -and -not $e.FullName.EndsWith('/')) {
            Save-Entry $e (Join-Path $dest ($e.FullName -replace '/', '\')); $count++
        }
    }
} finally { $zip.Dispose() }

$obbDir = Join-Path $dest 'obb'
[void](New-Item -ItemType Directory -Force -Path $obbDir)
Copy-Item -LiteralPath $obb -Destination $obbDir -Force
if ($IncludeAssets) {
    $zip = [IO.Compression.ZipFile]::OpenRead($obb)
    try {
        foreach ($e in $zip.Entries) {
            if ($e.FullName.StartsWith('assets/') -and -not $e.FullName.EndsWith('/')) {
                Save-Entry $e (Join-Path $dest ($e.FullName -replace '/', '\')); $count++
            }
        }
    } finally { $zip.Dispose() }
}
foreach ($d in 'files', 'external-files') { [void](New-Item -ItemType Directory -Force -Path (Join-Path $dest $d)) }
if ($IncludeMods -and (Test-Path -LiteralPath $modData)) {
    $modDest = Join-Path $dest 'ModData'
    [void](New-Item -ItemType Directory -Force -Path $modDest)
    Copy-Item -LiteralPath (Join-Path $modData 'Modloader') -Destination $modDest -Recurse -Force
    Write-Host 'Staged Scotland2 ModData/Modloader'
}
Write-Host "Staged $count entries to $dest"
