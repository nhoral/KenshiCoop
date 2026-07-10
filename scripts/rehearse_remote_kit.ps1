<#
.SYNOPSIS
  DRESS REHEARSAL for the remote session kit, entirely on this machine: install
  the kit into the local Kenshi-Join install exactly as the friend would, run a
  host + kit-join session THROUGH the WAN relay proxy, then judge the collected
  logs with analyze_run.ps1 - so the eventual internet session changes only the
  network, not the workflow.

.DESCRIPTION
  Flow:
    1. (build +) assemble the kit          - scripts\make_remote_kit.ps1
    2. deploy the same build to the host   - scripts\deploy.cmd
    3. start dist\netsim.exe ('bad' WAN profile by default)
    4. launch the HOST (host_session-equivalent env, but unattended)
    5. run the kit's friend_join.ps1 against 127.0.0.1:<proxy port>,
       pointed at the local Kenshi-Join install
    6. judge host.log + the friend results join.log via Invoke-RunAnalysis

  PASS means: kit install works, kit connects through a degraded link, the
  scenario passes, and the results bundle contains everything analyze_run needs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_remote_kit.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_remote_kit.ps1 -SkipBuild -Wan regional
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$Wan = "bad",
    [switch]$SkipBuild,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest
if ($Save -eq "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $Save = $manifest.Scenarios[$Scenario].Save
}
if ($Save -eq "") { throw "No save for scenario '$Scenario' (pass -Save)." }

# ---- 1. Kit ---------------------------------------------------------------------
Write-Host "=== 1. assemble kit ==="
$kitArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", (Join-Path $scriptDir "make_remote_kit.ps1"),
             "-Role", "join",   # this rehearsal drives the kit's friend_join.ps1
             "-Save", $Save, "-Scenario", $Scenario, "-Port", "$Port")
if ($SkipBuild) { $kitArgs += "-SkipBuild" }
& powershell @kitArgs
if ($LASTEXITCODE -ne 0) { throw "kit assembly failed" }
$kitDir = Join-Path $repoRoot "dist\remote-kit"

# ---- 2. Host deploy (same build as the kit) ---------------------------------------
Write-Host ""
Write-Host "=== 2. deploy to host install ==="
$stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
& cmd.exe /c "`"$scriptDir\deploy.cmd`""
if ($LASTEXITCODE -ne 0) { throw "deploy failed" }

# ---- 3. WAN proxy ------------------------------------------------------------------
Write-Host ""
Write-Host "=== 3. WAN relay proxy ($Wan) ==="
$netsimExe = Join-Path $repoRoot "dist\netsim.exe"
if (-not (Test-Path $netsimExe)) {
    & cmd.exe /c "`"$scriptDir\build_netsim.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "netsim build failed" }
}
$wp = $manifest.WanProfiles[$Wan]
$proxyPort = $Port + 1
$stampDir = Join-Path $repoRoot ("tools\test-runs\rehearsal_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Force -Path $stampDir | Out-Null
$wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $stampDir "netsim.log") `
    -ArgumentList @("$proxyPort", "127.0.0.1", "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
Start-Sleep -Milliseconds 500
if ($wanProc.HasExited) { throw "netsim exited immediately" }
Write-Host "  proxy up: 127.0.0.1:$proxyPort -> 127.0.0.1:$Port ($($wp.DelayMs)ms +/-$($wp.JitterMs)ms, $($wp.LossPct)% loss)"

$hostLog = Join-Path $stampDir "host.log"
try {
    # ---- 4. Host (unattended) --------------------------------------------------------
    Write-Host ""
    Write-Host "=== 4. launch host ==="
    $env:KENSHICOOP_MODE         = "host"
    $env:KENSHICOOP_IP           = "0.0.0.0"
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_SAVE         = $Save
    $env:KENSHICOOP_LOG          = $hostLog
    $env:KENSHICOOP_SCENARIO     = $Scenario
    $env:KENSHICOOP_SETUP        = $manifest.Scenarios[$Scenario].Setup
    $env:KENSHICOOP_TEST_SECONDS = "600"
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
    $env:KENSHICOOP_ARM_TIMEOUT_MS = "240000"
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath (Join-Path $HostDir "kenshi_x64.exe") -WorkDir $HostDir -TimeoutSec 120 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $hostGamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $hostGamePid = [int]$Matches[1] }
    if ($hostGamePid -eq 0) { throw "Host failed to launch." }

    Start-Sleep -Seconds 8

    # ---- 5. Kit join, exactly as the friend runs it -----------------------------------
    Write-Host ""
    Write-Host "=== 5. run the kit's friend_join.ps1 (as the friend would) ==="
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kitDir "friend_join.ps1") `
        -HostIp "127.0.0.1" -Port $proxyPort -KenshiDir $JoinDir
    $friendExit = $LASTEXITCODE

    # Host should self-exit with the scenario; backstop-kill if not.
    try { Wait-Process -Id $hostGamePid -Timeout 120 -ErrorAction Stop }
    catch { try { Stop-Process -Id $hostGamePid -Force -ErrorAction SilentlyContinue } catch {} }

    # ---- 6. Judge the collected logs ------------------------------------------------------
    Write-Host ""
    Write-Host "=== 6. judge collected logs (analyze_run path) ==="
    $resultsZip = Get-ChildItem (Join-Path $kitDir "KenshiCoop-results-*.zip") -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime | Select-Object -Last 1
    if ($null -eq $resultsZip) { throw "friend_join produced no results zip (exit=$friendExit)" }
    Write-Host "  friend results: $($resultsZip.FullName)"
    Expand-Archive -Path $resultsZip.FullName -DestinationPath $stampDir -Force

    $joinLog = Join-Path $stampDir "join.log"
    if (-not (Test-Path $joinLog)) { throw "results zip contained no join.log" }

    $verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
                  -Tolerance $manifest.Scenarios[$Scenario].Tolerance -JoinExpected $true `
                  -RunInfo @{ rehearsal = $true; wan = $Wan; kit = $kitDir } `
                  -OutJson (Join-Path $stampDir "verdict.json")

    Write-Host ""
    Write-Host ("REHEARSAL RESULT: " + $(if ($verdict.pass) { "PASS - kit is ready to send" } else { "FAIL" }))
    Write-Host ("  artifacts: $stampDir")
    if ($verdict.pass) { exit 0 } else { exit 1 }
} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { Stop-Process -Id $wanProc.Id -Force -ErrorAction SilentlyContinue } catch {}
    }
}
