param(
    [string]$Destination = '',
    [string]$Serial = '',
    [switch]$IncludeObb,
    [switch]$InspectOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$package = 'com.beatgames.beatsaber'
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Destination) { $Destination = Join-Path $projectRoot 'import\quest-beat-saber' }

$adbCommand = Get-Command adb.exe -ErrorAction SilentlyContinue
if (-not $adbCommand) {
    $candidate = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $adb = $candidate }
    else { throw 'Android Platform Tools (adb.exe) were not found. Install Android SDK Platform-Tools.' }
} else { $adb = $adbCommand.Source }

$deviceLines = @(& $adb devices)
if ($LASTEXITCODE -ne 0) { throw 'ADB could not list connected devices.' }
$connected = @($deviceLines | Where-Object { $_ -match '^\S+\s+device\s*$' } | ForEach-Object { ($_ -split '\s+')[0] })
if (-not $Serial) {
    if ($connected.Count -eq 0) { throw 'No authorized Quest is connected. Enable developer mode, attach a USB data cable and accept the USB debugging prompt on the headset.' }
    if ($connected.Count -gt 1) { throw 'Multiple Android devices connected. Run again with -Serial <device-serial>.' }
    $Serial = $connected[0]
} elseif ($Serial -notin $connected) {
    throw "ADB device '$Serial' is not authorized or connected."
}

$adbArgs = @('-s', $Serial)
$pathsOutput = @(& $adb @adbArgs shell pm path $package)
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the installed Beat Saber package.' }
$paths = @($pathsOutput | Where-Object { $_ -match '^package:/' } | ForEach-Object { ($_ -replace '^package:', '').Trim() })
if ($paths.Count -eq 0) {
    throw "No official Beat Saber package ($package) appears installed on the connected Quest. Install it from the Meta Horizon Store first."
}

$versions = @(& $adb @adbArgs shell dumpsys package $package |
    Select-String -Pattern 'versionCode=|versionName=' | Select-Object -First 4)
Write-Host "Connected Android device: $Serial"
Write-Host "Installed package: $package"
$versions | ForEach-Object { Write-Host ($_.ToString().Trim()) }
Write-Host "APK component(s): $($paths.Count)"
$paths | ForEach-Object { Write-Host "  $_" }
if ($InspectOnly) { return }

[void](New-Item -ItemType Directory -Path $Destination -Force)
$destinationFull = (Resolve-Path -LiteralPath $Destination).Path
$files = @()
foreach ($remote in $paths) {
    $filename = [IO.Path]::GetFileName($remote)
    if (-not $filename.EndsWith('.apk', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected package path (not .apk): $remote"
    }
    $output = Join-Path $destinationFull $filename
    Write-Host "Copying $filename..."
    & $adb @adbArgs pull $remote $output
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "Failed to copy $remote. Verify the headset permits adb pull for the installed APK."
    }
    $files += $output
}

if ($IncludeObb) {
    $remoteObb = "/sdcard/Android/obb/$package"
    $obbContents = @(& $adb @adbArgs shell ls $remoteObb)
    if ($LASTEXITCODE -eq 0 -and ($obbContents -notmatch 'No such file or directory')) {
        $obbDir = Join-Path $destinationFull 'obb'
        [void](New-Item -ItemType Directory -Path $obbDir -Force)
        Write-Host 'Copying external OBB assets...'
        & $adb @adbArgs pull $remoteObb $obbDir
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Could not copy optional OBB assets.' }
    } else { Write-Host 'No accessible OBB directory found.' }
}

$manifest = @(
    "Package: $package"
    'Origin: locally installed copy on connected Meta Quest'
    'APK components:'
)
foreach ($file in $files) {
    $item = Get-Item -LiteralPath $file
    $sha = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    $manifest += "$($item.Name) | $($item.Length) bytes | SHA256 $sha"
}
$manifestPath = Join-Path $destinationFull 'manifest.txt'
[IO.File]::WriteAllLines($manifestPath, [string[]]$manifest)
Write-Host "APK backup copied to: $destinationFull"
Write-Host 'All APK components and their SHA256 hashes are recorded in manifest.txt.'
Write-Host 'Keep game APKs private. The port only needs local access to inspect engine libraries.'
