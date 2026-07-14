<#
.SYNOPSIS
  Validate the F2 in-game co-op panel from a session log.

.DESCRIPTION
  The F2 panel is driven by hand, so the auto-start SCENARIO harness cannot
  exercise it. This runner applies the panel_config oracle (Test-PanelConfig)
  to a real/manual session log and prints a PASS/FAIL/SKIP verdict. It checks:
    * every panel connect that resolved ranks from the ROLE default owns the
      correct tab ({0} as HOST, {1} as JOIN) - the "their character won't move"
      ownership-rank fix; and
    * each panel role/transport selection actually reached the connect path.

  Point it at a log you played through the panel (yours or a friend's). With no
  -Log it scans the default KenshiCoop_host.log / KenshiCoop_join.log next to
  each local install.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\check_panel_log.ps1 -Log "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiCoop_host.log"

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\check_panel_log.ps1
#>
[CmdletBinding()]
param(
    # One or more log files to validate. Empty -> the default install log paths.
    [string[]]$Log = @()
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

if ($Log.Count -eq 0) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\Kenshi\KenshiCoop_host.log"),
        (Join-Path ${env:ProgramFiles(x86)} "Steam\steamapps\common\Kenshi\KenshiCoop_join.log"),
        (Join-Path $env:USERPROFILE "Kenshi-Join\KenshiCoop_host.log"),
        (Join-Path $env:USERPROFILE "Kenshi-Join\KenshiCoop_join.log")
    )
    $Log = @($candidates | Where-Object { Test-Path $_ })
    if ($Log.Count -eq 0) {
        Write-Host "No logs given and no default KenshiCoop_*.log found. Pass -Log <path>."
        exit 2
    }
}

$anyFail = $false
$anyJudged = $false
foreach ($f in $Log) {
    if (-not (Test-Path $f)) { Write-Host "  MISSING $f"; continue }
    Reset-GateResults
    Write-Host ""
    Write-Host "== panel check: $f =="
    $status = Test-PanelConfig -File $f
    $g = (Get-GateResults)[0]
    $m = $g.metrics
    Write-Host ("  panel_config {0} - {1}" -f $g.status, $g.detail)
    Write-Host ("  metrics: connects={0} intents={1} roleSourced={2} problems={3}" -f `
                $m.connects, $m.intents, $m.roleSourced, $m.problems)
    if ($status -eq "FAIL") { $anyFail = $true; $anyJudged = $true }
    elseif ($status -eq "PASS") { $anyJudged = $true }
}

Write-Host ""
if ($anyFail) { Write-Host "panel check: FAIL"; exit 1 }
if (-not $anyJudged) { Write-Host "panel check: SKIP (no panel connects found in any log)"; exit 2 }
Write-Host "panel check: PASS"
exit 0
