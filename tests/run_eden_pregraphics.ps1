<#
  Launch the current NRO using Eden's Qt executable (PowerShell only), and
  report the FRESH guest trace. This is a bounded smoke test, not proof of
  native-Switch execution. A host Vulkan renderer/preflight is never counted
  as Unity crossing the original graphics boundary.
#>
param(
    [string]$Nro = (Join-Path (Split-Path -Parent $PSScriptRoot) 'BeatSaberQuestLoaderLifecycle.nro'),
    [string]$Eden = $(if ($env:EDEN_EXE) { $env:EDEN_EXE } else { 'eden.exe' }),
    [string]$GuestLog = (Join-Path $env:APPDATA 'eden/sdmc/switch/beat-saber-quest/loader.log'),
    [ValidateRange(2, 300)][int]$MaxSeconds = 180,
    [ValidateRange(2, 300)][int]$QuietSeconds = 30
)
$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathRooted($Eden) -or -not (Test-Path -LiteralPath $Eden -PathType Leaf)) {
    throw "Eden not found: pass -Eden <path-to-eden.exe> or set `$env:EDEN_EXE"
}
if (-not (Test-Path -LiteralPath $Nro -PathType Leaf)) { throw "NRO missing: $Nro" }
if (-not (Test-Path -LiteralPath $Eden -PathType Leaf)) { throw "Eden missing: $Eden" }
$edenProcessName = [IO.Path]::GetFileNameWithoutExtension($Eden)
if (Get-Process -Name $edenProcessName -ErrorAction SilentlyContinue) {
    throw 'Eden is already running: refusing to interrupt a separate session'
}
$Nro = (Resolve-Path -LiteralPath $Nro).Path
$GuestLog = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($GuestLog)
# Eden commits guest writes to the host only on flush/close; the loader
# fsyncs each log line when this flag exists so a forced stop keeps the tail.
$syncFlag = Join-Path (Split-Path -Parent $GuestLog) 'sync-log'
if (-not (Test-Path -LiteralPath $syncFlag)) { [void](New-Item -ItemType File -Path $syncFlag) }
$initialLength = if (Test-Path -LiteralPath $GuestLog) {
    (Get-Item -LiteralPath $GuestLog).Length
} else { 0L }
$fresh = ''
$result = 'timeout'
$lastGrowth = [DateTime]::UtcNow
# Qt's VI/window lifecycle needs the emulator frontend to have an ordinary
# visible window. Hiding it can leave the guest stalled before graphics setup.
$process = Start-Process -FilePath $Eden -WorkingDirectory (Split-Path $Eden) `
    -ArgumentList ('-g "' + $Nro + '"') -WindowStyle Normal -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $GuestLog) {
            $length = (Get-Item -LiteralPath $GuestLog).Length
            # Eden may replace/truncate its log at guest launch. A saved
            # offset into the previous (larger) run would otherwise hide the
            # entire new startup until it happened to grow past that offset.
            if ($length -lt $initialLength) {
                $initialLength = 0L
                $fresh = ''
            }
            if ($length -gt $initialLength) {
                $stream = [System.IO.FileStream]::new(
                    $GuestLog, [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                try {
                    [void]$stream.Seek($initialLength, [System.IO.SeekOrigin]::Begin)
                    $buffer = [byte[]]::new([int]($length - $initialLength))
                    $read = $stream.Read($buffer, 0, $buffer.Length)
                    $fresh += [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
                    $initialLength += $read
                } finally { $stream.Dispose() }
                $lastGrowth = [DateTime]::UtcNow
                if ($fresh.Contains('[graphics:boundary] ORIGINAL guest reached') -or
                    $fresh.Contains('[graphics:boundary] verified guest Vulkan entry') -or
                    $fresh.Contains('[graphics:boundary] verified guest EGL entry')) {
                    $result = 'original-graphics-stop'
                    break
                }
                if ($fresh.Contains('unsupported JNI operation')) {
                    $result = 'jni-unsupported'
                    break
                }
                if ($fresh.Contains('JNI FatalError:')) {
                    $result = 'jni-fatal'
                    break
                }
                if ($fresh.Contains('[startup] original libunity/libgame bootstrap gated on missing services')) {
                    $result = 'bootstrap-gated'
                    break
                }
            }
        }
        # Once original NativeCode has started, its android_main worker keeps
        # running; keep observing it instead of stopping at the handoff. A
        # long quiet period after that point is reported as a stall.
        if ($fresh -match 'COMPLETE modules=\d+ unresolved=\d+') {
            $result = 'completed-diagnostics'
            if (([DateTime]::UtcNow - $lastGrowth).TotalSeconds -ge $QuietSeconds) {
                $result = 'native-stalled'
                break
            }
        }
        $process.Refresh()
        if ($process.HasExited) {
            $result = 'eden-exited'
            break
        }
    }
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

Write-Host "Eden outcome: $result"
$dexStops = [regex]::Matches(
    $fresh,
    '\[java:vm\] original Activity constructor stop=(\d+) steps=(\d+) code=0x([0-9a-fA-F]+) pc=0x([0-9a-fA-F]+) opcode=0x([0-9a-fA-F]+)')
if ($dexStops.Count -gt 0) {
    $lastStop = $dexStops[$dexStops.Count - 1]
    Write-Host ("Eden original DEX: steps={0} stop={1} code=0x{2} pc=0x{3} opcode=0x{4}" -f `
        $lastStop.Groups[2].Value, $lastStop.Groups[1].Value, `
        $lastStop.Groups[3].Value, $lastStop.Groups[4].Value, `
        $lastStop.Groups[5].Value)
}
($fresh -split "`r?`n" | Where-Object {
    $_ -match 'unresolved|\[java:(vm|horizon|horizon-hook|activity-base|attachment|handler|resources|surface|looper|package)\]|\[unity\]|\[java:unityplayer\]|\[android:|\[jni\] ERROR|\[graphics:|COMPLETE modules=|bootstrap gated|\[startup\] (automatically staging|original libraries:)'
} | Select-Object -Last 35) | ForEach-Object { Write-Host $_ }

if ($result -eq 'original-graphics-stop') { exit 0 }
if ($result -in 'completed-diagnostics', 'bootstrap-gated', 'native-stalled') { exit 2 }
if ($result -in 'jni-fatal', 'jni-unsupported') { exit 3 }
if ($result -eq 'eden-exited') { exit 4 }
exit 5
