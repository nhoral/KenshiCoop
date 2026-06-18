<#
.SYNOPSIS
  Regression suite for KenshiCoop: build + deploy once, then run the
  already-passing scenarios (locomotion + sit/stand NPC sync) and print a single
  PASS/FAIL summary with the key oracle ratios. Establishes a green baseline
  before new feature stages, and re-verifies that prior stages still pass.

.DESCRIPTION
  Wraps scripts\run_test.ps1 (which launches host + join, runs a compiled
  scenario, and emits oracle verdicts). This script only orchestrates - it adds
  no new oracle logic. The per-scenario verdict is run_test.ps1's own exit code;
  this script additionally surfaces the relevant oracle lines (CROSSCHECK,
  POSE-STATE, MARCH, smoothness, anim-truth) parsed from each run's logs so the
  baseline is captured in one place.

  Stages validated here:
    * Locomotion (Stage 3 walk-drive)         -> leader_move scenario
    * Sit/stand NPC sync (Stage 4/5 + I9-I11) -> npc_sync scenario

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Save sync

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Save sync -SkipBuild
#>
[CmdletBinding()]
param(
    # Shared save both clients load. 'sync' is the bar scene (sitters + standers),
    # the canonical sit/stand validation save.
    [string]$Save = "sync",
    [int]$Port = 27800,
    [double]$Tolerance = 3.0,
    # Reuse the currently deployed DLL (skip the build + deploy step).
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

# ---- 1. Build + deploy once (so every scenario runs the same fresh DLL) -------
# Invoked here via PowerShell -> cmd.exe /c, which runs the .cmd and exits
# cleanly (a bare `cmd /c` launched from a bash shell opens an interactive
# prompt and hangs - do not do that).
if (-not $SkipBuild) {
    Write-Host "=== kill stale Kenshi ==="
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

    Write-Host "=== build plugin ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (build failed, exit $LASTEXITCODE)"; exit 1 }

    Write-Host "=== deploy ==="
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (deploy failed, exit $LASTEXITCODE)"; exit 1 }
} else {
    Write-Host "=== skip build (reusing deployed DLL) ==="
}

# ---- 2. The regression matrix -------------------------------------------------
# Each entry runs run_test.ps1 with that scenario on $Save. run_test self-kills
# stale instances between runs, so they are safe to run sequentially.
$matrix = @(
    @{ scenario = "leader_move"; label = "locomotion (walk-drive)" },
    @{ scenario = "npc_sync";    label = "sit/stand NPC sync" }
)

# Oracle lines worth echoing into the summary, by scenario.
$oraclefilters = "RESULT:|CROSSCHECK|POSE-STATE|POSE \[|MARCH|smoothness|anim-truth"

$results = @()
foreach ($entry in $matrix) {
    $scenario = $entry.scenario
    Write-Host ""
    Write-Host "############################################################"
    Write-Host "# REGRESS: $scenario on '$Save'  ($($entry.label))"
    Write-Host "############################################################"

    # Run the scenario; capture all output so we can find the out dir + verdict.
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "run_test.ps1") `
                -Save $Save -Scenario $scenario -Port $Port -Tolerance $Tolerance 2>&1
    $exit = $LASTEXITCODE
    $out | ForEach-Object { Write-Host $_ }

    # Find the per-run output dir (run_test prints "out dir:  <path>").
    $outDir = ""
    foreach ($line in $out) {
        if ("$line" -match "out dir:\s+(.+)$") { $outDir = $Matches[1].Trim() }
    }

    # Pull the oracle lines from the run's own captured stdout for the summary.
    $oracleLines = @($out | Where-Object { "$_" -match $oraclefilters } | ForEach-Object { ("$_").Trim() })

    $results += @{
        scenario = $scenario
        label    = $entry.label
        pass     = ($exit -eq 0)
        outDir   = $outDir
        oracles  = $oracleLines
    }
}

# ---- 3. Single summary --------------------------------------------------------
Write-Host ""
Write-Host "================= REGRESSION SUMMARY ($Save) ================="
$allPass = $true
foreach ($r in $results) {
    $v = if ($r.pass) { "PASS" } else { "FAIL" }
    if (-not $r.pass) { $allPass = $false }
    Write-Host ""
    Write-Host ("  [{0}] {1}  ({2})" -f $v, $r.scenario, $r.label)
    if ($r.outDir -ne "") { Write-Host ("      logs: {0}" -f $r.outDir) }
    foreach ($o in $r.oracles) {
        if ($o -notmatch "^RESULT:") { Write-Host ("      {0}" -f $o) }
    }
}
Write-Host ""
Write-Host ("OVERALL: " + $(if ($allPass) { "PASS" } else { "FAIL" }))
if ($allPass) { exit 0 } else { exit 1 }
