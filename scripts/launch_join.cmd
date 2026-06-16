@echo off
REM Launch Kenshi as the co-op JOIN client. Connects to the host over UDP.
REM The plugin reads KENSHICOOP_MODE/IP/PORT from the environment at startup.
REM
REM Uses the SEPARATE join install (set up via scripts\setup_join_install.cmd) so
REM it has its own save/config/logs and can't collide with the HOST instance.
set "KENSHI_DIR=%USERPROFILE%\Kenshi-Join"

set KENSHICOOP_MODE=join
set KENSHICOOP_IP=127.0.0.1
set KENSHICOOP_PORT=27800

REM Optional: auto-load a save by name on launch (skips the menu). For co-op,
REM set the SAME name as launch_host.cmd, and run sync_save.cmd first so this
REM join install actually has that save on disk.
REM IMPORTANT: this must be an EXISTING save FOLDER name under
REM   %LOCALAPPDATA%\kenshi\save\<name>   (e.g. "c", not "current").
REM Auto-loading a save that doesn't exist will crash the game.
set "KENSHICOOP_SAVE="

if not exist "%KENSHI_DIR%\kenshi_x64.exe" (
    echo ERROR: join install not found at "%KENSHI_DIR%".
    echo Run scripts\setup_join_install.cmd first to create it.
    pause
    exit /b 1
)

cd /d "%KENSHI_DIR%"
echo Launching Kenshi as JOIN client -^> %KENSHICOOP_IP%:%KENSHICOOP_PORT% ...
echo   from: %KENSHI_DIR%
if defined KENSHICOOP_SAVE if not "%KENSHICOOP_SAVE%"=="" echo   auto-loading save: %KENSHICOOP_SAVE%
start "" "%KENSHI_DIR%\kenshi_x64.exe"
