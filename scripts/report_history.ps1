<#
.SYNOPSIS
  Trend report over tools\test-runs\history.jsonl: per scenario/variant pass
  rate, flake count, and drift of key measured metrics vs their rolling median.
  Catches degradation INSIDE the tolerance band that boolean gates cannot see.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\report_history.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\report_history.ps1 -Last 30 -DriftPct 25
#>
[CmdletBinding()]
param(
    # How many most-recent history entries per (scenario, variant) to consider.
    [int]$Last = 20,
    # Flag a numeric metric when the newest value is worse than the rolling
    # median of the prior window by more than this percentage.
    [double]$DriftPct = 30.0,
    [string]$HistoryFile = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
if ($HistoryFile -eq "") { $HistoryFile = Join-Path $repoRoot "tools\test-runs\history.jsonl" }
if (-not (Test-Path $HistoryFile)) { Write-Host "No history yet: $HistoryFile"; exit 0 }

# Metrics where a LARGER value is worse (drift = increase). Everything else
# numeric is treated as higher-is-better (drift = decrease).
$higherIsWorse = @("worstMedian", "latencyMs", "zeroFrac", "floatFrac", "marchFrac", "dxz", "recoverErrMs")

$entries = @()
foreach ($ln in Get-Content $HistoryFile) {
    if ($ln.Trim() -eq "") { continue }
    try { $entries += ($ln | ConvertFrom-Json) } catch {}
}
if ($entries.Count -eq 0) { Write-Host "History file is empty."; exit 0 }

# Group by scenario+variant.
$groups = $entries | Group-Object -Property { "$($_.scenario)|$($_.variant)" }

Write-Host "================= HISTORY REPORT ($($entries.Count) run(s) on file) ================="
$drifts = 0
foreach ($grp in ($groups | Sort-Object Name)) {
    $runs = @($grp.Group | Select-Object -Last $Last)
    $name = $grp.Name -replace '\|', ' ['
    $passN  = @($runs | Where-Object { $_.pass }).Count
    $flakyN = @($runs | Where-Object { $_.flaky }).Count
    Write-Host ""
    Write-Host ("  {0}]  runs={1} pass={2}/{1} flaky={3}" -f $name, $runs.Count, $passN, $flakyN)

    # Collect numeric metric series per gate across the window.
    $series = @{}   # "gate.metric" -> ordered list of doubles
    foreach ($run in $runs) {
        foreach ($g in @($run.gates)) {
            if ($null -eq $g.metrics) { continue }
            foreach ($p in $g.metrics.PSObject.Properties) {
                $val = 0.0
                if ([double]::TryParse("$($p.Value)", [ref]$val)) {
                    $key = "$($g.gate).$($p.Name)"
                    if (-not $series.ContainsKey($key)) { $series[$key] = New-Object System.Collections.ArrayList }
                    [void]$series[$key].Add($val)
                }
            }
        }
    }
    foreach ($key in ($series.Keys | Sort-Object)) {
        $vals = $series[$key]
        if ($vals.Count -lt 4) { continue }   # need history to trend
        $newest = $vals[$vals.Count - 1]
        $prior  = @($vals[0..($vals.Count - 2)] | Sort-Object)
        $median = $prior[[int]($prior.Count / 2)]
        $metricName = $key.Split('.')[-1]
        $worse = $false
        if ($median -ne 0) {
            $deltaPct = 100.0 * ($newest - $median) / [Math]::Abs($median)
            if ($higherIsWorse -contains $metricName) { $worse = ($deltaPct -gt $DriftPct) }
            else                                      { $worse = ($deltaPct -lt -$DriftPct) }
            if ($worse) {
                $drifts++
                Write-Host ("      DRIFT {0}: newest={1} vs rolling median={2} ({3}{4}%)" -f `
                    $key, [Math]::Round($newest, 3), [Math]::Round($median, 3),
                    $(if ($deltaPct -ge 0) { "+" } else { "" }), [Math]::Round($deltaPct, 1))
            }
        }
    }
    # Always show the headline metric trend for the primary-ish gates.
    foreach ($key in ($series.Keys | Sort-Object)) {
        $metricName = $key.Split('.')[-1]
        if ($metricName -in @("ratio", "worstMedian", "latencyMs", "downRatio")) {
            $vals = $series[$key]
            $recent = @($vals | Select-Object -Last 5 | ForEach-Object { [Math]::Round($_, 2) })
            Write-Host ("      {0}: {1}" -f $key, ($recent -join " -> "))
        }
    }
}
Write-Host ""
if ($drifts -gt 0) {
    Write-Host "DRIFT WARNINGS: $drifts (metric moved worse than rolling median by > $DriftPct%)"
    exit 2
}
Write-Host "No metric drift beyond $DriftPct% of rolling median."
exit 0
