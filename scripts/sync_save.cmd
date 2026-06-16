@echo off
REM Sync the HOST install's saves into the JOIN install so both clients can load
REM the *identical* save. This is required for handle-mapped NPC replication:
REM the same NPC only shares a `hand` across machines when both load the same
REM serialized save.
REM
REM Usage:
REM   scripts\sync_save.cmd ["C:\path\to\source Kenshi"] ["C:\path\to\Kenshi-Join"]
REM
REM Run this BEFORE launching, any time you've made/updated the save you want to
REM co-op on. Close both Kenshi instances first so save files aren't mid-write.
setlocal

set "SRC=%~1"
if "%SRC%"=="" set "SRC=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

set "DST=%~2"
if "%DST%"=="" set "DST=%USERPROFILE%\Kenshi-Join"

if not exist "%SRC%\save" (
    echo ERROR: no save folder at "%SRC%\save".
    exit /b 1
)
if not exist "%DST%\kenshi_x64.exe" (
    echo ERROR: join install not found at "%DST%". Run setup_join_install.cmd first.
    exit /b 1
)

echo Mirroring saves:
echo   from: %SRC%\save
echo   to:   %DST%\save
echo.

REM /MIR makes the destination an exact mirror (adds new, updates changed,
REM removes stale) so the join load menu matches the host exactly.
robocopy "%SRC%\save" "%DST%\save" /MIR /R:1 /W:1 /NFL /NDL /NP /NJH /NJS

set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 (
    echo ERROR: robocopy failed with code %RC%.
    exit /b %RC%
)

echo.
echo Saves synced. Load the SAME save name in both the host and join windows.
endlocal
