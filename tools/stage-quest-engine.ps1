param(
    [string]$SourceDirectory = '',
    [string]$Destination = '',
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

$portRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $portRoot 'SwitchPort' }
if (-not $Destination) { $Destination = Join-Path $portRoot 'import\quest-engine' }
$apkPath = Join-Path $SourceDirectory 'beat-saber.apk'
$obbPath = Join-Path $SourceDirectory 'main.3491.com.beatgames.beatsaber.obb'
if (-not (Test-Path -LiteralPath $apkPath -PathType Leaf)) { throw "Missing Quest APK: $apkPath" }
if (-not (Test-Path -LiteralPath $obbPath -PathType Leaf)) { throw "Missing Quest OBB: $obbPath" }

$entries = [Collections.Generic.List[object]]::new()
if (-not $VerifyOnly) { [void](New-Item -ItemType Directory -Path $Destination -Force) }
foreach ($source in @(@{Path=$apkPath; Kind='APK'}, @{Path=$obbPath; Kind='OBB'})) {
    $archive = [IO.Compression.ZipFile]::OpenRead($source.Path)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $include = if ($source.Kind -eq 'APK') {
                ($name -match '^lib/arm64-v8a/[^/]+\.so$') -or
                ($name -in @('assets/bin/Data/globalgamemanagers',
                             'assets/bin/Data/boot.config',
                             'assets/bin/Data/Managed/Metadata/global-metadata.dat',
                             'assets/bin/Data/UnitySubsystems/UnityOpenXR/UnitySubsystemsManifest.json'))
            } else {
                $name -in @('assets/aa/settings.json', 'assets/aa/catalog.json')
            }
            if (-not $include) { continue }
            $parts = $name.Split('/')
            if ($parts | Where-Object { $_ -in @('', '.', '..') }) { throw "Invalid archive entry: $name" }
            $output = Join-Path $Destination (Join-Path $source.Kind ($name -replace '/', '\'))
            if (-not $VerifyOnly) {
                $parent = Split-Path -Parent $output
                [void](New-Item -ItemType Directory -Path $parent -Force)
                $inputStream = $entry.Open()
                try {
                    $outStream = [IO.File]::Create($output)
                    try { $inputStream.CopyTo($outStream) }
                    finally { $outStream.Dispose() }
                } finally { $inputStream.Dispose() }
                $actualBytes = (Get-Item -LiteralPath $output).Length
                if ($actualBytes -ne $entry.Length) { throw "Unexpected extracted size: $name" }
            }
            $entries.Add([pscustomobject]@{Source=$source.Kind; Name=$name; Bytes=$entry.Length})
        }
    } finally { $archive.Dispose() }
}

if (-not ($entries | Where-Object { $_.Name -eq 'lib/arm64-v8a/libunity.so' })) { throw 'Quest APK has no ARM64 Unity engine library' }
if (-not ($entries | Where-Object { $_.Name -eq 'lib/arm64-v8a/libil2cpp.so' })) { throw 'Quest APK has no ARM64 IL2CPP library' }
if (-not ($entries | Where-Object { $_.Name -eq 'assets/aa/catalog.json' })) { throw 'Quest OBB lacks Android Addressables catalog' }

$lines = @('# Local private Quest native library and metadata inventory')
$lines += @($entries | Sort-Object Source,Name | ForEach-Object { '{0} | {1} | {2}' -f $_.Source,$_.Name,$_.Bytes })
if (-not $VerifyOnly) {
    [IO.File]::WriteAllLines((Join-Path $Destination 'inventory.txt'), [string[]]$lines)
    Write-Host "Staged $($entries.Count) native engine / metadata files to $Destination"
} else {
    Write-Host "Verified $($entries.Count) engine / metadata entries (no data extracted)"
}
$lines | Select-Object -First 30
