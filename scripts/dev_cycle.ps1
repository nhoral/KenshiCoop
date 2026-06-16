<#
.SYNOPSIS
  One-shot autonomous iteration cycle for KenshiCoop:
  kill stale Kenshi -> build the plugin -> deploy to both installs ->
  run the host+join functional test -> exit with PASS/FAIL.

.DESCRIPTION
  Intended to be the single command a Cursor session calls each iteration. It
  guarantees a clean slate (no leftover instance holding the DLL lock), rebuilds
  and redeploys the plugin, then runs scripts/run_test.ps1. The exit code is the
  test verdict (0 = PASS, non-zero = FAIL/build error), so an agent can branch on
  it without parsing output. Per-client logs + screenshots land under
  tools/test-runs/<stamp>/ (path is printed by run_test.ps1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\dev_cycle.ps1 -Save "c" -Seconds 60 -Sync
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Save,
    [int]$Seconds = 60,
    [switch]$Sync,
    [switch]$SkipBuild,
    [int]$Port = 27800
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Invoke-Step {
    param([string]$Name, [scriptblock]$Action)
    Write-Host ""
    Write-Host "=== $Name ==="
    & $Action
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL ($Name failed, exit $LASTEXITCODE)"; exit $LASTEXITCODE }
}

# 1. Clean slate so build/deploy aren't blocked by a loaded DLL / running game.
Write-Host "=== kill stale Kenshi ==="
$stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
Write-Host "  killed $($stale.Count) process(es)"

# 2/3. Build + deploy (skippable when only the test changed).
if (-not $SkipBuild) {
    Invoke-Step "build plugin" { & cmd /c "`"$scriptDir\build_plugin.cmd`"" }
    Invoke-Step "deploy"       { & cmd /c "`"$scriptDir\deploy.cmd`"" }
}

# 4. Run the functional test; its exit code is the verdict.
Write-Host ""
Write-Host "=== run test ==="
$testArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $scriptDir "run_test.ps1"),
              "-Save", $Save, "-Seconds", "$Seconds", "-Port", "$Port")
if ($Sync) { $testArgs += "-Sync" }
& powershell @testArgs
exit $LASTEXITCODE
