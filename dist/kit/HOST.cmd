@echo off
rem Double-click entry point for hosting a KenshiCoop session. Prompts for the
rem one thing the script can't know (the other player's Steam code) and hands
rem off to friend_host.ps1 - so players never have to open PowerShell or type
rem a command line. Ships inside the kit next to friend_host.ps1.
setlocal
cd /d "%~dp0"
title KenshiCoop - HOST
echo.
echo  =================================
echo    KenshiCoop  -  HOST a session
echo  =================================
echo.
rem Show THIS player's friend code first (read from Steam) so both players
rem can swap codes straight off this screen - no profile-page digging.
powershell -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0kit_preflight.ps1'; Show-MySteamId"
echo  Now enter the OTHER player's code (they see theirs the same way;
echo  a 17-digit SteamID64 works too).
echo.
set "PEER="
set /p PEER="  Other player's code (just press Enter if this kit came with it baked in): "
set "RESUME="
set /p RESUME="  Resume a previous session? (y/N): "
set "ARGS="
if not "%PEER%"=="" set "ARGS=-PeerSteamId %PEER%"
if /i "%RESUME%"=="y" set "ARGS=%ARGS% -Resume"
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0friend_host.ps1" %ARGS%
echo.
echo  Done. This window stays open so you can read the messages above.
pause
