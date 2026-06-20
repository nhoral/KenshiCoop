<#
.SYNOPSIS
  Run a batch of spikes off a SINGLE build: build+deploy once, then run each id in
  the list with run_spike.ps1 -SkipBuild. Skip-on-error: a spike that fails to
  capture is logged and the loop continues to the next (the autonomous-run rule).

.DESCRIPTION
  Amortizes the (slow) build over many diagnostic probes that share one DLL. The
  per-spike SpikeScenario branch is selected at runtime by KENSHICOOP_SPIKE, so a
  single build can serve every probe currently compiled into the dispatcher.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spikes.ps1 -Ids 1,3,8,9 -Save c

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_spikes.ps1 -Ids 15,16 -Save c -Seconds 60
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Ids,
    [string]$Save = "c",
    [int]$Seconds = 45,
    [switch]$SkipBuild,
    [int]$Port = 27800
)

$ErrorActionPreference = "Continue"   # never abort the batch on a single failure
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---- build once --------------------------------------------------------------
if (-not $SkipBuild) {
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
    Write-Host "=== batch build ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "BATCH BUILD-FAIL (exit $LASTEXITCODE)"; exit 2 }
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "BATCH DEPLOY-FAIL (exit $LASTEXITCODE)"; exit 3 }
}

# ---- run each id, skip on error ----------------------------------------------
$results = @()
foreach ($id in $Ids) {
    Write-Host ""
    Write-Host "################# SPIKE $id #################"
    $code = $null
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $scriptDir "run_spike.ps1") `
            -Id $id -Save $Save -Seconds $Seconds -Port $Port -SkipBuild
        $code = $LASTEXITCODE
    } catch {
        Write-Host "SPIKE $id THREW: $($_.Exception.Message)"
        $code = 99
    }
    $results += [pscustomobject]@{ id = $id; exit = $code }
}

Write-Host ""
Write-Host "================= BATCH SUMMARY ================="
foreach ($r in $results) {
    $v = switch ($r.exit) { 0 { "CAPTURE-OK" } 1 { "CAPTURE-INCOMPLETE" } default { "ERROR($($r.exit))" } }
    Write-Host ("  spike {0,-3} {1}" -f $r.id, $v)
}
