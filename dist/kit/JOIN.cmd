@echo off
rem Double-click entry point for joining a KenshiCoop session. Prompts for the
rem one thing the script can't know (how to reach the host) and hands off to
rem friend_join.ps1 - so players never have to open PowerShell or type a
rem command line. Ships inside the kit next to friend_join.ps1.
rem Input with a dot is treated as the host's IP (direct-UDP kits); anything
rem else as a Steam friend code / SteamID64 (Steam P2P kits).
setlocal
cd /d "%~dp0"
title KenshiCoop - JOIN
echo.
echo  =================================
echo    KenshiCoop  -  JOIN a session
echo  =================================
echo.
rem Show THIS player's friend code first (read from Steam) so both players
rem can swap codes straight off this screen - no profile-page digging.
powershell -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0kit_preflight.ps1'; Show-MySteamId"
echo  Now enter the HOST's code (they see theirs the same way; a 17-digit
echo  SteamID64 works too - on a direct-UDP kit, enter the host's public
echo  IP address instead).
echo.
set "HOSTID="
set /p HOSTID="  Host's code or IP (just press Enter if this kit came with it baked in): "
set "RESUME="
set /p RESUME="  Resume a previous session? (y/N): "
set "ARGS="
if not "%HOSTID%"=="" (
    echo(%HOSTID%| findstr /L "." >nul
    if not errorlevel 1 (set "ARGS=-HostIp %HOSTID%") else (set "ARGS=-HostSteamId %HOSTID%")
)
if /i "%RESUME%"=="y" set "ARGS=%ARGS% -Resume"
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0friend_join.ps1" %ARGS%
echo.
echo  Done. This window stays open so you can read the messages above.
pause
