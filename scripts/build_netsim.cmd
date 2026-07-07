@echo off
REM Build dist\netsim.exe - the WAN-conditions UDP relay proxy (src\netsim).
REM Applies delay/jitter/loss to ALL datagrams in BOTH directions BELOW ENet,
REM so WAN-variant test runs exercise real retransmission behaviour.
REM Uses the same v100 (VC++ 2010) x64 toolchain as the plugin.
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
if not exist "%REPO%\build\netsim" mkdir "%REPO%\build\netsim"

echo === Building netsim.exe (Release^|x64, v100) ===
cl.exe /nologo /O2 /EHsc /W3 ^
    /Fo"%REPO%\build\netsim\\" ^
    /Fe"%REPO%\dist\netsim.exe" ^
    "%REPO%\src\netsim\main.cpp" ^
    ws2_32.lib
if errorlevel 1 (
    echo netsim build FAILED
    exit /b 1
)
echo netsim built: %REPO%\dist\netsim.exe
exit /b 0
