@echo off
rem Double-click launcher for setup.ps1 (bypasses the PowerShell script policy for this run only).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
