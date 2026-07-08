<#
.SYNOPSIS
  Bake a fixture save: launch a single HOST instance, auto-load a base save,
  run a setup scene (KENSHICOOP_SETUP), then auto-write the fixture save
  (KENSHICOOP_BAKESAVE) - no manual save-menu round-trip.

  Example (bed+cage occupancy fixture):
    powershell -ExecutionPolicy Bypass -File scripts\bake_scene.ps1 `
        -Setup bedcage -BaseSave squad1 -BakeSave bedcage1
#>
param(
    [Parameter(Mandatory=$true)][string]$Setup,
    [string]$BaseSave = "squad1",
    [Parameter(Mandatory=$true)][string]$BakeSave,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [int]$Seconds = 120,
    [int]$StartTimeoutSec = 240,
    [switch]$SkipDeploy
)
$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo      = Split-Path -Parent $scriptDir

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Kenshi not found at $HostDir" }

$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $BaseSave))) {
    throw "Base save '$BaseSave' not found in $saveRoot"
}

if (-not $SkipDeploy) {
    Write-Host "Deploying freshly built plugin ..."
    & cmd /c "`"$scriptDir\deploy.cmd`" `"$HostDir`""
    if ($LASTEXITCODE -ne 0) { throw "deploy.cmd failed ($LASTEXITCODE)" }
}

$log = Join-Path $repo "tools\_bake_$Setup.log"
Remove-Item $log -ErrorAction SilentlyContinue

$env:KENSHICOOP_MODE         = "host"
$env:KENSHICOOP_SAVE         = $BaseSave
$env:KENSHICOOP_SETUP        = $Setup
$env:KENSHICOOP_BAKESAVE     = $BakeSave
$env:KENSHICOOP_TEST_SECONDS = "$Seconds"
$env:KENSHICOOP_LOG          = $log
$env:KENSHICOOP_SCENARIO     = ""
$env:KENSHICOOP_ARM_TIMEOUT_MS = "1"

Write-Host "Launching HOST for bake (setup=$Setup base=$BaseSave -> bake=$BakeSave) ..."
$out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $hostExe -WorkDir $HostDir -TimeoutSec $StartTimeoutSec 6>&1
$out | ForEach-Object { Write-Host "    $_" }
$line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
if (-not ($line -and ("$line" -match "GAMEPID=(\d+)"))) { throw "Host failed to get past the launcher." }
$gamePid = [int]$Matches[1]
Write-Host "Game PID=$gamePid; waiting for auto-bake (timeout $($Seconds + 120)s) ..."

$deadline = (Get-Date).AddSeconds($Seconds + 120)
$baked = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path $log) {
        $hit = Select-String -Path $log -Pattern "auto-bake save" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hit) { Write-Host "  $($hit.Line)"; $baked = $hit.Line -match "ISSUED"; break }
    }
    if (-not (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) {
        Write-Warning "Game exited before auto-bake line appeared."
        break
    }
    Start-Sleep -Seconds 2
}

# Give the save write time to finish, then stop the game.
Start-Sleep -Seconds 20
if (Get-Process -Id $gamePid -ErrorAction SilentlyContinue) {
    Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue
    Write-Host "Game process stopped."
}

$bakedDir = Join-Path $saveRoot $BakeSave
if (Test-Path (Join-Path $bakedDir "quick.save")) {
    Write-Host "OK: baked save present at $bakedDir"
    exit 0
} else {
    Write-Warning "Baked save NOT found at $bakedDir (baked=$baked). Check $log"
    Get-Content $log -Tail 40 -ErrorAction SilentlyContinue
    exit 1
}
