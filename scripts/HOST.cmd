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
echo.
echo  Which save do you want to play?
echo    [1] The bundled co-op starter save  - default
echo    [2] Your own save - picked in-game from Kenshi's Load menu
set "CHOICE=1"
set /p CHOICE="  Choose 1 or 2 - Enter = 1: "
rem First character only: tolerates stray trailing whitespace/CR.
set "CHOICE=%CHOICE:~0,1%"
set "ARGS="
if not "%PEER%"=="" set "ARGS=-PeerSteamId %PEER%"
if "%CHOICE%"=="2" (
    echo.
    echo  PLAYING YOUR OWN SAVE: you'll start on the bundled save so the two
    echo  games can connect. Once BOTH players are in-game, open Kenshi's
    echo  menu ^> Load and pick any save - the other player's game follows
    echo  automatically, and your save is streamed to them first if they
    echo  don't have it. This is also how you RESUME a previous co-op
    echo  session: just load the save you made last time. Only the HOST can
    echo  pick the save this way.
    echo.
    echo  TIP: your friend controls the SECOND squad tab. If your save only
    echo  has one squad, move some units into a new squad tab in-game to
    echo  give them a crew.
    echo.
)
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0friend_host.ps1" %ARGS%
echo.
echo  Done. This window stays open so you can read the messages above.
pause
