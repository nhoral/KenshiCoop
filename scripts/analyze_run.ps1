<#
.SYNOPSIS
  Offline oracle runner: judge ANY host/join log pair with the exact same
  oracles + verdict rule that gate live loopback runs.

.DESCRIPTION
  This is the post-session judge for logs collected from a remote (human) test:
  the friend sends back join.log, you keep host.log, and this script produces
  the same gate summary + verdict.json a live run would have. Clock alignment
  comes from the CLOCKSYNC lines the plugin logs (wire time-sync), so the two
  machines' wall clocks do not need to agree.

  Also useful for re-judging historical tools\test-runs\<stamp>\ folders after
  an oracle change (regression-test the oracles themselves).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -RunDir tools\test-runs\20260705_120000 -Scenario coop_presence

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -HostLog c:\collected\host.log -JoinLog c:\collected\join.log -Scenario coop_presence
#>
[CmdletBinding()]
param(
    # Either a run dir containing host.log/join.log ...
    [string]$RunDir = "",
    # ... or explicit log paths.
    [string]$HostLog = "",
    [string]$JoinLog = "",
    [Parameter(Mandatory = $true)][string]$Scenario,
    [double]$Tolerance = 0,
    # Expected injected clock skew (ms) if the run used -FakeClockSkewMs.
    [string]$ExpectedSkewMs = "",
    # Set when the run went through the WAN relay (or a real internet link):
    # applies the manifest's deliberate WAN-regime gate adjustments.
    [switch]$WanActive,
    # Where to write verdict.json (defaults beside the host log).
    [string]$OutJson = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

if ($RunDir -ne "") {
    if ($HostLog -eq "") { $HostLog = Join-Path $RunDir "host.log" }
    if ($JoinLog -eq "") { $JoinLog = Join-Path $RunDir "join.log" }
}
if ($HostLog -eq "" -or -not (Test-Path $HostLog)) { throw "Host log not found: '$HostLog' (use -RunDir or -HostLog)" }
if ($JoinLog -ne "" -and -not (Test-Path $JoinLog)) { Write-Warning "Join log not found: $JoinLog (judging host-only)" }

$manifest = Get-ScenarioManifest
if ($Tolerance -eq 0) {
    $Tolerance = 3.0
    if ($manifest.Scenarios.ContainsKey($Scenario)) { $Tolerance = $manifest.Scenarios[$Scenario].Tolerance }
}
if ($OutJson -eq "") { $OutJson = Join-Path (Split-Path -Parent $HostLog) "verdict.json" }

Write-Host "== analyze_run: $Scenario =="
Write-Host "  host log: $HostLog"
Write-Host "  join log: $JoinLog"
$off = Get-LogClockOffsetMs -File $JoinLog
Write-Host "  join clock offset applied: ${off}ms (from CLOCKSYNC; 0 = none logged)"
Write-Host ""

$expSkew = if ($ExpectedSkewMs -ne "") { [int]$ExpectedSkewMs } else { $null }
$verdict = Invoke-RunAnalysis -HostLog $HostLog -JoinLog $JoinLog -Scenario $Scenario `
              -Tolerance $Tolerance -JoinExpected ($JoinLog -ne "") `
              -RunInfo @{ offline = $true; hostLog = $HostLog; joinLog = $JoinLog } `
              -ExpectedSkewMs $expSkew -WanActive ([bool]$WanActive) -OutJson $OutJson

Write-Host ""
Write-Host ("RESULT: " + $(if ($verdict.pass) { "PASS" } else { "FAIL" }))
if ($verdict.pass) { exit 0 } else { exit 1 }
