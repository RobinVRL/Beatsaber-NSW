<#
  Launch the NRO in Eden and observe the original Unity game past graphics
  initialization. Reports presented frames (from [gles] eglSwapBuffers logs)
  and saves a screenshot of the Eden window before stopping it.
  Refuses to start when another Eden session is running.
#>
param(
    [string]$Nro = (Join-Path (Split-Path -Parent $PSScriptRoot) 'BeatSaberQuestLoaderLifecycle.nro'),
    [string]$Eden = $(if ($env:EDEN_EXE) { $env:EDEN_EXE } else { 'eden.exe' }),
    [string]$GuestLog = (Join-Path $env:APPDATA 'eden/sdmc/switch/beat-saber-quest/loader.log'),
    [string]$Screenshot = (Join-Path $env:TEMP 'eden_game.png'),
    [ValidateRange(5, 900)][int]$MaxSeconds = 180,
    [ValidateRange(5, 300)][int]$QuietSeconds = 45,
    [int]$StopAfterFrames = 0,
    # Also save <Screenshot>_<seconds>.png every N seconds (0 = off).
    [int]$ShotEverySeconds = 0
)
$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathRooted($Eden) -or -not (Test-Path -LiteralPath $Eden -PathType Leaf)) {
    throw "Eden not found: pass -Eden <path-to-eden.exe> or set `$env:EDEN_EXE"
}
if (-not (Test-Path -LiteralPath $Nro -PathType Leaf)) { throw "NRO missing: $Nro" }
$edenProcessName = [IO.Path]::GetFileNameWithoutExtension($Eden)
if (Get-Process -Name $edenProcessName -ErrorAction SilentlyContinue) {
    throw 'Eden is already running: refusing to interrupt a separate session'
}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class BsWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
$Nro = (Resolve-Path -LiteralPath $Nro).Path
$syncFlag = Join-Path (Split-Path -Parent $GuestLog) 'sync-log'
if (-not (Test-Path -LiteralPath $syncFlag)) { [void](New-Item -ItemType File -Path $syncFlag) }
$offset = if (Test-Path -LiteralPath $GuestLog) { (Get-Item -LiteralPath $GuestLog).Length } else { 0L }
$fresh = ''
$result = 'timeout'
$frames = 0
$lastGrowth = [DateTime]::UtcNow
$process = Start-Process -FilePath $Eden -WorkingDirectory (Split-Path $Eden) `
    -ArgumentList ('-g "' + $Nro + '"') -WindowStyle Normal -PassThru
function Save-Shot([string]$Path = $Screenshot) {
    $process.Refresh()
    $h = $process.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object BsWin+RECT
    if (-not [BsWin]::GetWindowRect($h, [ref]$r)) { return }
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # Capture only Eden's own window content (PW_RENDERFULLCONTENT), never
    # whatever else is on screen above it.
    $hdc = $g.GetHdc()
    $ok = [BsWin]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc)
    if (-not $ok) { $g.Dispose(); $bmp.Dispose(); return }
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "Screenshot: $Path"
}
try {
    $started = [DateTime]::UtcNow
    $nextShot = $ShotEverySeconds
    $deadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ($ShotEverySeconds -gt 0 -and ([DateTime]::UtcNow - $started).TotalSeconds -ge $nextShot) {
            Save-Shot ($Screenshot -replace '\.png$', "_$nextShot.png")
            $nextShot += $ShotEverySeconds
        }
        if (Test-Path -LiteralPath $GuestLog) {
            $length = (Get-Item -LiteralPath $GuestLog).Length
            if ($length -lt $offset) { $offset = 0L; $fresh = '' }
            if ($length -gt $offset) {
                $stream = [IO.FileStream]::new($GuestLog, 'Open', 'Read', 'ReadWrite')
                try {
                    [void]$stream.Seek($offset, 'Begin')
                    $buffer = [byte[]]::new([int]($length - $offset))
                    $read = $stream.Read($buffer, 0, $buffer.Length)
                    $fresh += [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
                    $offset += $read
                } finally { $stream.Dispose() }
                $lastGrowth = [DateTime]::UtcNow
            }
        }
        $m = [regex]::Matches($fresh, 'eglSwapBuffers frame=(\d+)')
        if ($m.Count) { $frames = [int64]$m[$m.Count - 1].Groups[1].Value }
        if ($frames -gt 0) { $result = 'rendering' }
        if ($StopAfterFrames -gt 0 -and $frames -ge $StopAfterFrames) { break }
        if ($fresh.Contains('JNI FatalError:')) { $result = 'jni-fatal'; break }
        if ($fresh.Contains('[bionic:exit]')) { $result = 'guest-exit'; break }
        if (([DateTime]::UtcNow - $lastGrowth).TotalSeconds -ge $QuietSeconds) {
            if ($result -ne 'rendering') { $result = 'stalled' }
            break
        }
        $process.Refresh()
        if ($process.HasExited) { $result = 'eden-exited'; break }
    }
    if (-not $process.HasExited) { Save-Shot }
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
Write-Host "Eden outcome: $result frames>=$frames"
($fresh -split "`r?`n" | Where-Object {
    ($_ -match '\[egl\]|\[gles\]|\[graphics|\[choreographer|\[looper|\[android:|\[bionic:|FATAL|ERROR') -and
    ($_ -notmatch 'OpenXR-Loader|\] \[XR\] ')
} | Select-Object -Last 40) | ForEach-Object { Write-Host $_ }
if ($result -eq 'rendering') { exit 0 }
exit 1
