@echo off
REM Build dist\prototest.exe - the asserting unit layer for the wire protocol,
REM content hash and interpolation buffer (src\prototest). Uses the same v100
REM (VC++ 2010) x64 compiler as the plugin so the packed-struct layout under
REM test is EXACTLY the layout the shipped DLL compiles (that is the point:
REM prototest locks the wire contract of this toolchain).
REM
REM No game/KenshiLib/ENet dependencies - just the CRT.
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\prototest" mkdir "%REPO%\build\prototest"

echo === Building prototest.exe (Release^|x64, v100) ===
cl.exe /nologo /O2 /EHsc /W3 ^
    /Fo"%REPO%\build\prototest\\" ^
    /Fe"%REPO%\dist\prototest.exe" ^
    "%REPO%\src\prototest\main.cpp" ^
    "%REPO%\src\plugin\sync\Interp.cpp"
if errorlevel 1 (
    echo prototest build FAILED
    exit /b 1
)
echo prototest built: %REPO%\dist\prototest.exe
exit /b 0
