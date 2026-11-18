param(
    [switch]$Clean,
    [ValidatePattern('^[A-Za-z][A-Za-z0-9_-]*$')]
    [string]$Target = 'BeatSaberQuestLoader',
    # Build for Switch 2 (kaiHEN). DEVKITPRO must contain libounce and
    # portlibs/switch2 (the ounce-devkit packages); see docs/SWITCH2.md.
    [switch]$Switch2
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $env:DEVKITPRO) { $env:DEVKITPRO = 'C:/devkitPro' }
if (-not $env:DEVKITA64) { $env:DEVKITA64 = "$env:DEVKITPRO/devkitA64" }
$localNvk = Join-Path $project 'nvk-sdk'
if (-not $env:NVK_SDK -and (Test-Path -LiteralPath (Join-Path $localNvk 'lib/libvulkan.a'))) {
    # make recursively changes from the project directory into build/.
    # Keeping the SDK outside build/ preserves it across -Clean builds.
    $env:NVK_SDK = '../nvk-sdk'
}
if ($env:NVK_SDK) {
    $sdkDirectory = if ($env:NVK_SDK -eq '../nvk-sdk') { $localNvk } else { $env:NVK_SDK }
    foreach ($required in @('lib/libvulkan.a', 'include/vulkan', 'Vulkan-Headers-1.4.305/include/vk_video')) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdkDirectory $required))) {
            throw "NVK_SDK is missing $required under $sdkDirectory"
        }
    }
    Write-Host "Using NVK Vulkan SDK: $sdkDirectory"
}
$env:PATH = "$env:DEVKITPRO/devkitA64/bin;$env:DEVKITPRO/msys2/usr/bin;$env:DEVKITPRO/tools/bin;" + $env:PATH
$make = Join-Path $env:DEVKITPRO 'msys2/usr/bin/make.exe'
if (-not (Test-Path -LiteralPath $make)) { throw "Missing devkitPro make.exe: $make" }
$platformArgs = @()
$suffix = ''
if ($Switch2) {
    foreach ($required in @('libounce/switch2_rules', 'portlibs/switch2/lib/libouncevk-gl.a', 'portlibs/switch2/lib/kaihen-jit.o')) {
        if (-not (Test-Path -LiteralPath (Join-Path $env:DEVKITPRO $required))) {
            throw "DEVKITPRO ($env:DEVKITPRO) is missing $required; install ounce-devkit"
        }
    }
    $platformArgs = @('SWITCH2=1')
    $suffix = '_switch2'
}
if ($Clean) {
    & $make -C $project clean "TARGET=$Target" @platformArgs
    if ($LASTEXITCODE -ne 0) { throw "Clean failed: $LASTEXITCODE" }
}
& $make -C $project -j4 "TARGET=$Target" @platformArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
$nro = Join-Path $project "$Target$suffix.nro"
if (-not (Test-Path -LiteralPath $nro)) { throw 'The NRO build output is missing.' }
$info = Get-Item -LiteralPath $nro
Write-Host "Built $($info.FullName) ($($info.Length) bytes)"
