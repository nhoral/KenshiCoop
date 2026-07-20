<#
.SYNOPSIS
  Missing-save bootstrap proof (protocol 31/32): force the JOIN to STREAM the
  host's world instead of loading a local copy, then load it - the real
  "seamless join, no pre-shared save" path players hit on a second machine.

.DESCRIPTION
  The normal harness (run_test.ps1) auto-loads the SAME save on both clients, and
  on one machine both installs share %LOCALAPPDATA%\kenshi\save, so a join always
  finds a fingerprint MATCH and loads from disk - the folder-transfer half never
  runs. This runner exercises it end-to-end on a single machine:

    * HOST  loads $Save and goes ONLINE (auto). On the join's connect it bakes its
      live world and announces it with a LOAD_GO ([boot] baking / GO->join).
    * JOIN  stays at the MAIN MENU (it goes ONLINE via auto-start and takes the
      menu bootstrap branch, which returns before any config auto-load) with
      KENSHICOOP_FORCE_STREAM=1 so it NACKs the LOAD_GO even though the shared
      folder would MATCH - forcing the host to stream the folder (SaveXfer over
      CH_BULK). The join stages + CRC-verifies + commits it, then loads. (The
      join is handed the save NAME only so the title hook installs under the
      test's self-exit timer; it never auto-loads it - see the launch comment.)

  Both clients self-exit (KENSHICOOP_TEST_SECONDS), the per-client logs are
  written straight into the out dir, and the run is graded by the connect_stream
  oracle (Test-ConnectStream): it REQUIRES the transfer edges (NACK -> XFER-BEGIN
  -> XFER-COMMIT badCrc=0 -> XFER-ACK ok=1 -> transfer-committed load -> gameplay),
  so a run that quietly fell back to a MATCH load FAILS.

  On loopback the join's commit rewrites the shared folder byte-identically (same
  as resume_test.ps1); a real second machine is where the bytes land on a
  separate disk. Either way the stage/verify/commit/load path is fully run.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\stream_test.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\stream_test.ps1 -Save sync -SkipBuild
#>
[CmdletBinding()]
param(
    # Save the HOST loads (and therefore bakes + streams to the join). Must exist
    # in %LOCALAPPDATA%\kenshi\save. The join receives THIS save by name.
    [string]$Save = "sync",
    [int]$Seconds = 90,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [int]$JoinDelaySec = 8,
    [int]$StartTimeoutSec = 90,
    [switch]$SkipBuild,
    [switch]$SkipDeploy,
    [switch]$NoKill
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\stream_$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

# The host must have a real save to load + bake (auto-loading a missing save
# crashes the game). The join deliberately starts with NO save.
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $Save))) {
    $avail = (Get-ChildItem $saveRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ", "
    throw "Host save '$Save' not found in $saveRoot. Available saves: $avail"
}

$hostLog = Join-Path $OutDir "host.log"
$joinLog = Join-Path $OutDir "join.log"

Write-Host "== KenshiCoop STREAM (missing-save bootstrap) test =="
Write-Host "  host save:  $Save (baked + streamed to the join)"
Write-Host "  join:       stays at menu online, FORCE_STREAM=1 (NACKs to pull the folder)"
Write-Host "  seconds:    $Seconds (self-exit)"
Write-Host "  out dir:    $OutDir"

# Clean slate so build/deploy aren't blocked by a loaded DLL / running game.
if (-not $NoKill) {
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        Write-Host "  killing $($stale.Count) stale Kenshi process(es) ..."
        $stale | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}

# Build + deploy the FORCE_STREAM plugin into BOTH installs (unless skipped).
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build plugin ==="
    & cmd /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd failed ($LASTEXITCODE)" }
}
if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== deploy ==="
    & cmd /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "deploy.cmd failed ($LASTEXITCODE)" }
}

function Set-CoopEnv {
    param([string]$Mode, [string]$SaveName, [string]$Log, [string]$ForceStream)
    $env:KENSHICOOP_MODE          = $Mode
    # Same-machine loopback: force direct UDP (Steam P2P can't self-connect).
    $env:KENSHICOOP_TRANSPORT     = "udp"
    $env:KENSHICOOP_STEAM_PEER    = "0"
    $env:KENSHICOOP_PORT          = "$Port"
    $env:KENSHICOOP_IP            = $Ip
    $env:KENSHICOOP_SAVE          = $SaveName   # host: real save; join: "" (menu)
    # TEST_SECONDS>0 both self-exits AND forces the load-time auto-start, so the
    # join goes ONLINE from the menu with no manual F2 (see Plugin.cpp autoStart).
    $env:KENSHICOOP_TEST_SECONDS  = "$Seconds"
    $env:KENSHICOOP_SCENARIO      = ""          # normal co-op tick, no scenario
    $env:KENSHICOOP_LOG           = $Log
    # The whole point: join NACKs a matching save so the transfer really runs.
    $env:KENSHICOOP_FORCE_STREAM  = $ForceStream
    # Keep the coordinated save/load channels ON (defaults) - leave them unset.
}

function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

function Test-Alive { param([int]$ProcId) return ($ProcId -ne 0 -and $null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

# ---- Launch HOST (loads $Save, hosts, bakes on connect) -------------------------
Write-Host ""
Write-Host "Launching HOST (save $Save) ..."
Set-CoopEnv -Mode "host" -SaveName $Save -Log $hostLog -ForceStream ""
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

Write-Host "Waiting for HOST to reach gameplay (timeout ${StartTimeoutSec}s) ..."
if (Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec) {
    Write-Host "Host in gameplay; launching JOIN after ${JoinDelaySec}s ..."
} else {
    Write-Warning "Host not in gameplay after ${StartTimeoutSec}s; launching JOIN anyway."
}
Start-Sleep -Seconds $JoinDelaySec

# ---- Launch JOIN (stays at menu, online, FORCE_STREAM) --------------------------
# The join is given the save NAME (not left empty) for ONE harness reason: the
# title hook - which pumps the menu bootstrap (drain GO -> NACK -> receive) - is
# only installed when a save is configured OR the session is interactive
# (testSeconds==0). This runner needs testSeconds>0 for auto-start + self-exit, so
# an empty save would skip the hook entirely. The join does NOT auto-load it: once
# online it takes the menu bootstrap branch (titleUpdate_hook) and returns before
# the config auto-load, and FORCE_STREAM=1 makes it NACK its on-disk copy so the
# real host->join folder transfer runs. Real players join interactively, so their
# hook installs the normal way - this only bridges the automated harness.
Write-Host "Launching JOIN (stays at menu, online, FORCE_STREAM=1) ..."
Set-CoopEnv -Mode "join" -SaveName $Save -Log $joinLog -ForceStream "1"
$joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
if ($joinPid -eq 0) { Write-Warning "Join failed to get past the launcher; host is up alone." }

Write-Host "Host game PID=$hostPid  Join game PID=$joinPid"

# Progress breadcrumbs (best-effort; the oracle is the real verdict).
if (Wait-ForLogLine -File $joinLog -Pattern "\[save\] XFER-RECV" -TimeoutSec 60) {
    Write-Host "  join is RECEIVING the streamed save ..."
    if (Wait-ForLogLine -File $joinLog -Pattern "\[save\] XFER-COMMIT" -TimeoutSec 60) {
        Write-Host "  join COMMITTED the streamed save."
    }
} else {
    Write-Warning "  did not see [save] XFER-RECV on the join within 60s (transfer may not have engaged)."
}

# ---- Wait for self-exit, with a hard-timeout kill backstop ----------------------
$killGrace   = $Seconds + $JoinDelaySec + 120
$killDeadline = (Get-Date).AddSeconds($killGrace)
foreach ($p in @($hostPid, $joinPid)) {
    if ($p -eq 0) { continue }
    $remain = [int]([Math]::Max(1, ($killDeadline - (Get-Date)).TotalSeconds))
    try { Wait-Process -Id $p -Timeout $remain -ErrorAction Stop }
    catch {
        if (Test-Alive $p) {
            Write-Warning "PID $p did not self-exit; killing."
            try { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
}

Write-Host ""
Write-Host "== Results =="
Write-Host "  host log: $hostLog"
Write-Host "  join log: $joinLog"

# ---- Grade with the connect_stream oracle (transfer REQUIRED) -------------------
Write-Host ""
Write-Host "== connect_stream oracle =="
Reset-GateResults
$status = Invoke-OneOracle -Id "connect_stream" -HostLog $hostLog -JoinLog $joinLog -Tolerance 0 -ExpectedSkewMs $null

Write-Host ""
Write-Host ("STREAM-TEST RESULT: " + $(if ($status -eq "PASS") { "PASS" } else { $status }))
if ($status -eq "PASS") { exit 0 } else { exit 1 }
