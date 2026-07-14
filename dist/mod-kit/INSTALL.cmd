@echo off
REM KenshiCoop mod installer. Double-click to install the co-op mod.
setlocal EnableDelayedExpansion
set "HERE=%~dp0"

REM Locate the Kenshi install (Steam default; edit KENSHI= if yours differs).
set "KENSHI=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
if not exist "%KENSHI%\kenshi_x64.exe" (
  echo Could not find Kenshi at:
  echo   %KENSHI%
  echo Edit this file and set KENSHI= to your Kenshi folder, then re-run.
  pause & exit /b 1
)

set "DST=%KENSHI%\mods\KenshiCoop"
if not exist "%DST%" mkdir "%DST%"
copy /Y "%HERE%mod\KenshiCoop.dll"  "%DST%\KenshiCoop.dll"  >nul
copy /Y "%HERE%mod\RE_Kenshi.json"  "%DST%\RE_Kenshi.json"  >nul
if exist "%HERE%mod\KenshiCoop.mod" copy /Y "%HERE%mod\KenshiCoop.mod" "%DST%\KenshiCoop.mod" >nul
REM Keep an existing config (so edits survive a re-run).
if not exist "%DST%\coop_config.json" copy /Y "%HERE%mod\coop_config.json" "%DST%\coop_config.json" >nul
echo Installed mod to %DST%
echo.
echo Next:
echo   1. Install RE_Kenshi if you don't have it: https://www.nexusmods.com/kenshi/mods/847
echo   2. Set Kenshi to WINDOWED mode (Options ^> Video ^> un-check Full Screen).
echo   3. Put the OTHER player's Steam ID in "steamPeer" in
echo      %DST%\coop_config.json  (see README.txt; use "Copy my Steam ID" in the
echo      F2 panel to share your own ID).
echo   4. Launch Kenshi, enable "KenshiCoop" in the Mods menu, and load a save.
echo   5. Press F2 to open the Co-op panel. Pick Host/Join + transport, then
echo      toggle Connection to ONLINE.
echo.
echo   NOTE: both players must load the SAME save to connect.
echo.
pause
