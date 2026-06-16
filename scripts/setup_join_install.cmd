@echo off
REM Create a fully independent second Kenshi install for the co-op JOIN client so
REM it can't share saves/config/logs with the HOST instance on the same machine.
REM
REM Usage:
REM   scripts\setup_join_install.cmd ["C:\path\to\source Kenshi"] ["C:\path\to\Kenshi-Join"]
REM
REM Defaults:
REM   source = Steam install
REM   dest   = %USERPROFILE%\Kenshi-Join   (user-writable; no elevation needed)
REM
REM Safe to re-run: it re-syncs from the source but PRESERVES the join copy's own
REM save/ and settings so your second client keeps its independent state.
setlocal EnableDelayedExpansion

set "SRC=%~1"
if "%SRC%"=="" set "SRC=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

set "DST=%~2"
if "%DST%"=="" set "DST=%USERPROFILE%\Kenshi-Join"

if not exist "%SRC%\kenshi_x64.exe" (
    echo ERROR: source Kenshi not found at "%SRC%".
    echo        Pass the real install path as the first argument.
    exit /b 1
)

echo === Creating JOIN install ===
echo   source: %SRC%
echo   dest:   %DST%
echo.
echo This copies the full game (several GB) the first time. Subsequent runs only
echo sync changed game files and leave the join client's save/settings alone.
echo.

REM /E copy subdirs incl. empty. /XO skip older (don't clobber newer join-side
REM files). Exclude the mutable, per-instance state so the join client keeps its
REM own world/config/logs independent of the host after the first copy.
robocopy "%SRC%" "%DST%" /E /XO /R:1 /W:1 /NFL /NDL /NP /NJH /NJS ^
    /XD "%SRC%\save" ^
    /XF settings.cfg controls.cfg kenshi.cfg ^
        kenshi.log kenshi_info.log Havok.log FileIOLog.txt RE_Kenshi_log.txt

set "RC=%ERRORLEVEL%"
REM robocopy: exit codes 0-7 are success (>=8 is a real failure).
if %RC% GEQ 8 (
    echo ERROR: robocopy failed with code %RC%.
    exit /b %RC%
)

REM First-time only: seed the join client's save from the source so it has a
REM world to load. After this it owns an independent copy.
if not exist "%DST%\save" (
    echo Seeding initial save into join install ...
    robocopy "%SRC%\save" "%DST%\save" /E /R:1 /W:1 /NFL /NDL /NP /NJH /NJS >nul
)

REM Ensure the join client reads/writes saves from its OWN local install folder
REM (User save location=1). Without it Kenshi looks in My Documents and won't see
REM the synced saves. settings.cfg is intentionally not copied from the host, so
REM add the key if the freshly-generated one lacks it.
if exist "%DST%\settings.cfg" (
    findstr /I /C:"User save location" "%DST%\settings.cfg" >nul 2>&1
    if errorlevel 1 (
        echo User save location=1>>"%DST%\settings.cfg"
        echo Added "User save location=1" to join settings.cfg
    )
)

echo.
echo JOIN install ready at: %DST%
echo Next:
echo   1) Rebuild/deploy the plugin:  scripts\deploy.cmd
echo      (deploy.cmd now also pushes KenshiCoop.dll into the join install)
echo   2) Launch HOST: scripts\launch_host.cmd
echo   3) Launch JOIN: scripts\launch_join.cmd
endlocal
