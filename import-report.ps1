<#
 .SYNOPSIS
 Analyze the imported symbols needed to execute the original Android Unity engine.
 .DESCRIPTION
 Read-only nm(1) inspection of the owner's staged ARM64 shared objects,
 compared with the existing compiled platform_bridge.c table. Outputs a
 deterministic summary; -Save writes a local JSON report (no game binaries).
#>
param([switch]$Save)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = $project
$libs = Join-Path $port 'import\quest-engine\APK\lib\arm64-v8a'
$devkit = if ($env:DEVKITPRO) { $env:DEVKITPRO } else { 'C:/devkitPro' }
$nm = Join-Path $devkit 'devkitA64\bin\aarch64-none-elf-nm.exe'
if (-not (Test-Path -LiteralPath $nm -PathType Leaf)) { throw "Missing devkitA64 nm.exe: $nm" }
if (-not (Test-Path -LiteralPath $libs -PathType Container)) { throw "Stage the original Quest engine libraries first: $libs" }

$bridgeCode = Get-Content -LiteralPath (Join-Path $project 'source\platform_bridge.c')
$bridge = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($line in $bridgeCode) {
    if ($line -match '^\s*\{"([^"]+)",\s*\(uintptr_t\)') { [void]$bridge.Add($matches[1]) }
}

function SymbolsFromNm {
    param([string]$Path, [string]$Option)
    $argsForNm = if ($Option -eq 'exports') { @('-D','--defined-only',$Path) }
                 else { @('-D','--undefined-only',$Path) }
    $nmOutput = @(& $nm @argsForNm)
    if ($LASTEXITCODE -ne 0) { throw "nm failed for $Path ($LASTEXITCODE)" }
    $sym = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($line in $nmOutput) {
        if ($line -match '^\s*(?:[0-9a-fA-F]+\s+)?[A-Za-z]\s+(\S+)') {
            $name = ($matches[1] -split '@')[0]
            if ($name) { [void]$sym.Add($name) }
        }
    }
    return $sym
}

$libFiles = @(Get-ChildItem -LiteralPath $libs -Filter '*.so' -File | Sort-Object Name)
if ($libFiles.Count -eq 0) { throw 'No .so libraries were staged.' }
$exports = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$defs = @{}
foreach ($file in $libFiles) {
    $defines = SymbolsFromNm -Path $file.FullName -Option 'exports'
    $defs[$file.Name] = $defines
    foreach ($symbol in $defines) { [void]$exports.Add($symbol) }
}

$report = [Collections.Generic.List[object]]::new()
$missingTotal = 0
foreach ($file in $libFiles) {
    $imports = SymbolsFromNm -Path $file.FullName -Option 'imports'
    $fromGame = [Collections.Generic.List[string]]::new()
    $fromBridge = [Collections.Generic.List[string]]::new()
    $unresolved = [Collections.Generic.List[string]]::new()
    foreach ($symbol in $imports) {
        if ($bridge.Contains($symbol)) { $fromBridge.Add($symbol) }
        elseif ($exports.Contains($symbol)) { $fromGame.Add($symbol) }
        else { $unresolved.Add($symbol) }
    }
    $sortedMissing = @($unresolved | Sort-Object)
    $missingTotal += $unresolved.Count
    $report.Add([pscustomobject]@{
        Library = $file.Name
        Imports = $imports.Count
        BridgeBindings = $fromBridge.Count
        GameExports = $fromGame.Count
        NeedsPlatform = $unresolved.Count
        MissingNames = $sortedMissing
    })
}

$summary = [pscustomobject]@{
    OriginalPackage = 'Beat Saber Quest 1.46.0_34060 (Unity 6000.3.19f1)'
    LibraryCount = $libFiles.Count
    BridgeSymbolCount = $bridge.Count
    TotalUnresolvedUniquePerLibrary = $missingTotal
    Note = 'Offline symbol-name coverage only; versioned/weak imports and ABI-level compatibility still require testing.'
    Libraries = @($report)
}
$report | Select-Object Library, Imports, BridgeBindings, GameExports, NeedsPlatform | Format-Table -AutoSize
Write-Host "Remaining unresolved symbol names across modules (not deduplicated globally): $missingTotal"
if ($Save) {
    $jsonFile = Join-Path $project 'abi-report.json'
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonFile -Encoding UTF8
    Write-Host "Wrote $jsonFile"
}
