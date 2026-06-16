@echo off
REM Thin wrapper around run_test.ps1 so it runs like the other .cmd scripts.
REM
REM Usage:
REM   scripts\run_test.cmd "SaveName" [seconds]
REM
REM Example:
REM   scripts\run_test.cmd "MyCoopSave" 60
REM
REM For more options (ports, install dirs, -Sync, output dir) call the
REM PowerShell script directly:
REM   powershell -ExecutionPolicy Bypass -File scripts\run_test.ps1 -Save "MyCoopSave" -Seconds 60 -Sync

setlocal
set "SAVE=%~1"
if "%SAVE%"=="" (
    echo Usage: scripts\run_test.cmd "SaveName" [seconds]
    exit /b 1
)
set "SECS=%~2"
if "%SECS%"=="" set "SECS=60"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_test.ps1" -Save "%SAVE%" -Seconds %SECS% -Sync
endlocal
