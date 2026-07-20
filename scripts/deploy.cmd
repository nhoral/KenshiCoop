@echo off
REM Deploy KenshiCoop into a Kenshi install's mods folder.
REM Usage:  scripts\deploy.cmd ["C:\path\to\Kenshi"]
REM Defaults to the Steam install path if no argument is given.
setlocal EnableDelayedExpansion

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "KENSHI=%~1"
if "%KENSHI%"=="" set "KENSHI=C:\Program Files (x86)\Steam\steamapps\common\Kenshi"

REM Build config to deploy (Phase 1 build separation). Default = Harness (the
REM test build with the scenario runner). Pass "Release" as the 2nd argument to
REM deploy the shipped player DLL instead.
REM   Usage:  scripts\deploy.cmd ["C:\path\to\Kenshi"] [Harness|Release|Debug]
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Harness"

set "DLL=%REPO%\src\plugin\x64\%CONFIG%\KenshiCoop.dll"
set "JSON=%REPO%\dist\mods\KenshiCoop\RE_Kenshi.json"
set "DST=%KENSHI%\mods\KenshiCoop"

if not exist "%DLL%" (
    echo ERROR: %DLL% not found. Build first: scripts\build_plugin.cmd
    exit /b 1
)
if not exist "%KENSHI%\kenshi_x64.exe" (
    echo ERROR: Kenshi not found at "%KENSHI%". Pass the path as the first argument.
    exit /b 1
)

if not exist "%DST%" mkdir "%DST%"

copy /Y "%DLL%"  "%DST%\KenshiCoop.dll"   >nul
if errorlevel 1 (
    echo ERROR: could not copy KenshiCoop.dll to "%DST%".
    echo        The file is locked - a Kenshi instance is probably still running.
    echo        Close all Kenshi processes and retry.
    exit /b 1
)
echo Copied KenshiCoop.dll
copy /Y "%JSON%" "%DST%\RE_Kenshi.json"   >nul
if errorlevel 1 (
    echo ERROR: could not copy RE_Kenshi.json to "%DST%" ^(locked?^).
    exit /b 1
)
echo Copied RE_Kenshi.json

REM A .mod file is required for Kenshi to list the mod so it can be enabled.
REM It is a binary FCS file and cannot be generated here. If missing, copy any
REM existing .mod as a placeholder, otherwise make an empty one in the FCS.
if not exist "%DST%\KenshiCoop.mod" (
    set "TEMPLATE="
    for /f "delims=" %%m in ('dir /b /s "%KENSHI%\mods\*.mod" 2^>nul') do if not defined TEMPLATE set "TEMPLATE=%%m"
    if defined TEMPLATE (
        copy /Y "!TEMPLATE!" "%DST%\KenshiCoop.mod" >nul
        echo Created KenshiCoop.mod from template
    ) else (
        echo NOTE: no .mod found to copy. Create an empty KenshiCoop.mod via the FCS
        echo       and place it in "%DST%".
    )
)

echo.
echo Deployed to: %DST%
dir /b "%DST%"

REM Also deploy into the separate JOIN install if it exists, so both clients run
REM the same freshly-built plugin. (Created by scripts\setup_join_install.cmd.)
set "JOINDIR=%USERPROFILE%\Kenshi-Join"
if not "%KENSHI%"=="%JOINDIR%" if exist "%JOINDIR%\kenshi_x64.exe" (
    set "JDST=%JOINDIR%\mods\KenshiCoop"
    if not exist "!JDST!" mkdir "!JDST!"
    copy /Y "%DLL%"  "!JDST!\KenshiCoop.dll" >nul
    if errorlevel 1 (
        echo ERROR: could not copy KenshiCoop.dll to join install "!JDST!".
        echo        The file is locked - a Kenshi-Join instance is probably still running.
        exit /b 1
    )
    echo Copied KenshiCoop.dll  -^> join install
    copy /Y "%JSON%" "!JDST!\RE_Kenshi.json" >nul && echo Copied RE_Kenshi.json  -^> join install
    if not exist "!JDST!\KenshiCoop.mod" if exist "%DST%\KenshiCoop.mod" (
        copy /Y "%DST%\KenshiCoop.mod" "!JDST!\KenshiCoop.mod" >nul
        echo Copied KenshiCoop.mod   -^> join install
    )
)

echo.
echo Next: launch Kenshi, enable "KenshiCoop" in the Mods tab, then check
echo RE_Kenshi_log.txt for "KenshiCoop loaded!".
endlocal
