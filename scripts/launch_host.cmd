@echo off
REM Launch Kenshi as the co-op HOST (authoritative). Listens on UDP 27800.
REM The plugin reads KENSHICOOP_MODE/IP/PORT from the environment at startup.
set "KENSHI_DIR=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

set KENSHICOOP_MODE=host
set KENSHICOOP_PORT=27800

REM Optional: auto-load a save by name on launch (skips the menu). Leave blank to
REM pick a save manually. For co-op, set the SAME name here and in launch_join.cmd
REM (after running sync_save.cmd so both installs have that save).
REM IMPORTANT: this must be an EXISTING save FOLDER name under
REM   %LOCALAPPDATA%\kenshi\save\<name>   (e.g. "c", not "current").
REM Auto-loading a save that doesn't exist will crash the game.
set "KENSHICOOP_SAVE="

cd /d "%KENSHI_DIR%"
echo Launching Kenshi as HOST on port %KENSHICOOP_PORT% ...
if defined KENSHICOOP_SAVE if not "%KENSHICOOP_SAVE%"=="" echo   auto-loading save: %KENSHICOOP_SAVE%
start "" "%KENSHI_DIR%\kenshi_x64.exe"
