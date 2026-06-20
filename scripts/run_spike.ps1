<#
.SYNOPSIS
  Run ONE investigative spike end to end: (optionally) build + deploy the plugin,
  launch host+join via run_test.ps1 with the generic "spike" scenario selected by
  KENSHICOOP_SPIKE=<id>, then collect the per-client logs + screenshots into
  resources/spikes/<id>/raw/ for the findings doc.

.DESCRIPTION
  The "spike" scenario (src/plugin/test/Scenario.cpp -> SpikeScenario) dispatches
  on the KENSHICOOP_SPIKE env var, which run_test.ps1 does NOT touch, so we set it
  in this parent shell and it is inherited by the launched game processes.

  Diagnostic by nature: the deliverable is the EVIDENCE in the collected logs
  (lines beginning "SPIKE <id> ..."), not a PASS/FAIL gate. A run that reaches
  gameplay and self-exits cleanly is a successful capture.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spike.ps1 -Id 8 -Save c

.EXAMPLE
  # Reuse the currently deployed DLL (batch several spikes off one build):
  powershell -ExecutionPolicy Bypass -File scripts\run_spike.ps1 -Id 9 -Save c -SkipBuild
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Id,
    [string]$Save = "c",
    [int]$Seconds = 45,
    [switch]$SkipBuild,
    [switch]$HostOnly,        # diagnostic probes that need no peer (faster)
    [int]$Port = 27800,
    [string]$Setup = "",
    # Free-form per-spike argument forwarded to the plugin via KENSHICOOP_SPIKE_ARG
    # (e.g. spike 9 "bake5" = host-only bake of a 10-body battle save).
    [string]$SpikeArg = "",
    [int]$NetSimDelayMs = 0,
    [int]$NetSimJitterMs = 0,
    [int]$NetSimLossPct = 0
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
$spikeDir  = Join-Path $repoRoot "resources\spikes\$Id"
$rawDir    = Join-Path $spikeDir "raw"
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

Write-Host "================================================================"
Write-Host "== SPIKE $Id  (save=$Save seconds=$Seconds skipBuild=$SkipBuild)"
Write-Host "================================================================"

# ---- 1. build + deploy (unless reusing a batch build) ------------------------
if (-not $SkipBuild) {
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

    Write-Host "=== build ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "SPIKE $Id BUILD-FAIL (exit $LASTEXITCODE)"; exit 2 }

    Write-Host "=== deploy ==="
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "SPIKE $Id DEPLOY-FAIL (exit $LASTEXITCODE)"; exit 3 }
} else {
    Write-Host "=== skip build (reusing deployed DLL) ==="
}

# ---- 2. run the spike --------------------------------------------------------
# KENSHICOOP_SPIKE rides through to the launched processes (run_test.ps1's
# Set-CoopEnv never overwrites it).
$env:KENSHICOOP_SPIKE = $Id
$env:KENSHICOOP_SPIKE_ARG = $SpikeArg

$outDir = ""
$runArgs = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $scriptDir "run_test.ps1"),
    "-Save", $Save, "-Scenario", "spike", "-Port", "$Port", "-Seconds", "$Seconds"
)
if ($Setup -ne "")  { $runArgs += @("-Setup", $Setup) }
if ($NetSimDelayMs)  { $runArgs += @("-NetSimDelayMs", "$NetSimDelayMs") }
if ($NetSimJitterMs) { $runArgs += @("-NetSimJitterMs", "$NetSimJitterMs") }
if ($NetSimLossPct)  { $runArgs += @("-NetSimLossPct", "$NetSimLossPct") }

$out = & powershell @runArgs 2>&1
$out | ForEach-Object {
    Write-Host $_
    if ("$_" -match "out dir:\s+(.+)$") { $outDir = $Matches[1].Trim() }
}
$env:KENSHICOOP_SPIKE = ""
$env:KENSHICOOP_SPIKE_ARG = ""

# ---- 3. collect evidence -----------------------------------------------------
if ($outDir -ne "" -and (Test-Path $outDir)) {
    Copy-Item -Path (Join-Path $outDir "*") -Destination $rawDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ""
    Write-Host "== collected -> $rawDir =="
}

# Surface this spike's own SPIKE lines so the transcript shows the evidence.
$hostLog = Join-Path $rawDir "host.log"
$joinLog = Join-Path $rawDir "join.log"
Write-Host ""
Write-Host "== SPIKE $Id evidence (host) =="
if (Test-Path $hostLog) {
    Select-String -Path $hostLog -Pattern "SPIKE $Id " -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("  " + ($_.Line -replace '^.*SPIKE', 'SPIKE').Trim()) }
}
if (-not $HostOnly) {
    Write-Host "== SPIKE $Id evidence (join) =="
    if (Test-Path $joinLog) {
        Select-String -Path $joinLog -Pattern "SPIKE $Id " -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("  " + ($_.Line -replace '^.*SPIKE', 'SPIKE').Trim()) }
    }
}

# A capture is "OK" if the host reached gameplay and logged the spike's start line.
$ok = (Test-Path $hostLog) -and `
      ((Get-Content $hostLog -Raw) -match "gameplay started") -and `
      (Select-String -Path $hostLog -Pattern "SPIKE $Id start" -Quiet)
Write-Host ""
Write-Host ("SPIKE $Id " + $(if ($ok) { "CAPTURE-OK" } else { "CAPTURE-INCOMPLETE" }))
if ($ok) { exit 0 } else { exit 1 }
