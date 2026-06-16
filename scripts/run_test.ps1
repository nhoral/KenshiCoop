<#
.SYNOPSIS
  Functional test runner for KenshiCoop: launch host + join, auto-load a save,
  run for a fixed time, self-exit, then collect per-client logs and screenshots
  for an agent to evaluate.

.DESCRIPTION
  Relies on the plugin's env-var driven behavior:
    KENSHICOOP_SAVE          - save to auto-load on the title screen
    KENSHICOOP_TEST_SECONDS  - self-exit this many seconds after gameplay starts
    KENSHICOOP_LOG           - dedicated, per-line-flushed log file
  Each client writes its own log; we time a screenshot of each window while both
  are still in-game, then wait for them to self-exit (with a hard-timeout kill as
  a safety net).

  Host runs from the Steam install; join runs from the separate Kenshi-Join
  install (scripts\setup_join_install.cmd). With -Sync, host saves are mirrored
  into the join install first (required so both can load the same save).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_test.ps1 -Save "MyCoopSave" -Seconds 60 -Sync
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Save,
    [int]$Seconds = 60,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [switch]$Sync,
    [switch]$NoKill,
    [int]$JoinDelaySec = 8,
    [int]$ShotLeadSec = 5,
    [int]$StartTimeoutSec = 90
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

# Fail fast on a bad save name. Auto-loading a non-existent save crashes the
# game (load() faults when the save folder is missing), so verify it exists in
# Kenshi's per-user save location before launching anything. Kenshi stores saves
# under %LOCALAPPDATA%\kenshi\save\<name> regardless of install dir.
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $Save))) {
    $avail = (Get-ChildItem $saveRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ", "
    throw "Save '$Save' not found in $saveRoot. Available saves: $avail"
}

$hostLog = Join-Path $OutDir "host.log"
$joinLog = Join-Path $OutDir "join.log"
$hostPng = Join-Path $OutDir "host.png"
$joinPng = Join-Path $OutDir "join.png"

Write-Host "== KenshiCoop test run =="
Write-Host "  save:     $Save"
Write-Host "  seconds:  $Seconds"
Write-Host "  out dir:  $OutDir"

# Clear any leftover Kenshi instances from a previous (possibly crashed) run.
# Stale instances break unattended loops: the loaded KenshiCoop.dll can't be
# redeployed (file lock), and Kenshi's single-instance lock / our new-process
# detection get confused. Pass -NoKill to skip (e.g. if you're playing).
if (-not $NoKill) {
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        Write-Host "  killing $($stale.Count) stale Kenshi process(es) before starting ..."
        $stale | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}

if ($Sync) {
    Write-Host "Syncing saves host -> join ..."
    & cmd /c "`"$scriptDir\sync_save.cmd`" `"$HostDir`" `"$JoinDir`""
    if ($LASTEXITCODE -ne 0) { throw "sync_save.cmd failed ($LASTEXITCODE)" }
}

function Set-CoopEnv {
    param([string]$Mode, [string]$Log)
    $env:KENSHICOOP_MODE         = $Mode
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_IP           = $Ip
    $env:KENSHICOOP_SAVE         = $Save
    $env:KENSHICOOP_TEST_SECONDS = "$Seconds"
    $env:KENSHICOOP_LOG          = $Log
}

# Wait until a regex appears in a (growing) file, or timeout. Returns $true/$false.
function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -SimpleMatch:$false -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Take-Shot {
    param([int]$ProcId, [string]$Out, [string]$Label)
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") -ProcessId $ProcId -Out $Out
        Write-Host "  captured $Label -> $Out"
    } catch {
        Write-Warning "screenshot ($Label) failed: $($_.Exception.Message)"
    }
}

# Launch a Kenshi instance and get it past Kenshi's Win32 launcher (the env vars
# set by Set-CoopEnv are inherited by the launched process). start_kenshi.ps1
# clicks the launcher's OK button and returns the REAL game process id - which
# differs from the launched kenshi_x64.exe (that's just a loader that relaunches
# the game as a separate "Kenshi_x64" process). Returns 0 on failure.
function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

Write-Host "Launching HOST (and clicking through the launcher) ..."
Set-CoopEnv -Mode "host" -Log $hostLog
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

Write-Host "Waiting $JoinDelaySec s before launching JOIN ..."
Start-Sleep -Seconds $JoinDelaySec

Write-Host "Launching JOIN (and clicking through the launcher) ..."
Set-CoopEnv -Mode "join" -Log $joinLog
$joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
if ($joinPid -eq 0) { Write-Warning "Join failed to get past the launcher; continuing with host only." }

Write-Host "Host game PID=$hostPid  Join game PID=$joinPid"

# Anchor the screenshot to when the HOST reaches gameplay (so both are still
# alive at capture time - host is the first to self-exit).
$started = Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec
if ($started) {
    Write-Host "Host reached gameplay; waiting before screenshot ..."
} else {
    Write-Warning "Did not see 'gameplay started' in host log within $StartTimeoutSec s; capturing on a best-effort basis."
}

function Test-Alive { param([int]$ProcId) return ($ProcId -ne 0 -and $null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

$lead = $Seconds - $ShotLeadSec
if ($lead -lt 0) { $lead = 0 }
Start-Sleep -Seconds $lead

if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
else { Write-Warning "Host already exited before screenshot." }
if (Test-Alive $joinPid) { Take-Shot -ProcId $joinPid -Out $joinPng -Label "join" }
else { Write-Warning "Join already exited before screenshot." }

# Wait for self-exit, with a hard-timeout kill so unattended runs never hang.
$killDeadline = (Get-Date).AddSeconds($ShotLeadSec + 60)
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
Write-Host "  host png: $hostPng"
Write-Host "  join png: $joinPng"

# Evaluate one client's log. A client "passed" if it reached gameplay, exited
# cleanly via the self-exit timer (i.e. did NOT crash mid-run), and logged no
# ERROR lines. Returns $true/$false and prints a one-line summary.
function Evaluate {
    param([string]$File, [string]$Label, [bool]$Required)
    if (-not (Test-Path $File)) {
        if ($Required) { Write-Host "  [$Label] FAIL - no log produced"; return $false }
        Write-Host "  [$Label] skipped (not launched)"; return $true
    }
    $text     = Get-Content $File -Raw
    $errs     = @(Select-String -Path $File -Pattern "\] ERROR:" -ErrorAction SilentlyContinue)
    $reached  = $text -match "gameplay started"
    $cleanEnd = $text -match "test duration elapsed; exiting"
    $ok = ($reached -and $cleanEnd -and $errs.Count -eq 0)
    $why = @()
    if (-not $reached)  { $why += "never reached gameplay" }
    if (-not $cleanEnd) { $why += "no clean self-exit (likely crashed)" }
    if ($errs.Count -gt 0) { $why += "$($errs.Count) error line(s)" }
    $verdict = if ($ok) { "PASS" } else { "FAIL - " + ($why -join "; ") }
    Write-Host "  [$Label] $verdict"
    foreach ($e in ($errs | Select-Object -First 5)) { Write-Host "      $($e.Line)" }
    return $ok
}

$hostOk = Evaluate -File $hostLog -Label "host" -Required $true
$joinOk = Evaluate -File $joinLog -Label "join" -Required ($joinPid -ne 0)

$pass = ($hostOk -and $joinOk)
Write-Host ""
Write-Host ("RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
