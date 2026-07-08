@echo off
REM Build dist\tunneltest.exe - proves the ENet protocol survives the
REM socket-hook tunnel (patch 0002) under Steam P2P's constraints (1200-byte
REM datagram cap + loss) in one process, no game / no Steam (src\tunneltest).
REM Compiles the SAME vendored+patched ENet sources as the plugin, with the
REM same v100 (VC++ 2010) x64 toolchain, so the seam under test is the seam
REM that ships.
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat;%REPO%\third_party\enet\enet\include"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\tunneltest" mkdir "%REPO%\build\tunneltest"

set "ENET=%REPO%\third_party\enet\enet"

echo === Building tunneltest.exe (Release^|x64, v100) ===
cl.exe /nologo /O2 /EHsc /W3 /DWIN32 ^
    /Fo"%REPO%\build\tunneltest\\" ^
    /Fe"%REPO%\dist\tunneltest.exe" ^
    "%REPO%\src\tunneltest\main.cpp" ^
    "%ENET%\callbacks.c" "%ENET%\compress.c" "%ENET%\host.c" "%ENET%\list.c" ^
    "%ENET%\packet.c" "%ENET%\peer.c" "%ENET%\protocol.c" "%ENET%\win32.c" ^
    ws2_32.lib winmm.lib
if errorlevel 1 (
    echo tunneltest build FAILED
    exit /b 1
)
echo tunneltest built: %REPO%\dist\tunneltest.exe
exit /b 0
