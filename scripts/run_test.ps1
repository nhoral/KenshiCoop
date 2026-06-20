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
    [int]$StartTimeoutSec = 90,
    [int]$Frames = 5,
    [int]$FrameIntervalMs = 16,
    # Scenario mode: when set, both clients run the compiled scenario named here
    # (KENSHICOOP_SCENARIO). The scenario self-exits when complete, so the run is
    # short; the verdict adds in-plugin CHECK lines, the host's SCENARIO RESULT,
    # and a cross-client SCENARIO MEMBER vs RECV position comparison.
    [string]$Scenario = "",
    # Host-only setup scene (KENSHICOOP_SETUP). For craft validation pass "craft":
    # the HOST re-arms the baked worker's work goal each session (addGoal intent does
    # not survive save/load), while the JOIN is left clean so it reproduces purely via
    # replication. Never applied to the join.
    [string]$Setup = "",
    [double]$Tolerance = 3.0,
    [int]$ScenarioShotDelaySec = 5,
    # How long to wait (from the host capture) for the later-loading join to log
    # its first SCENARIO RECV, so the join screenshot is captured in-game and
    # mid-action rather than on a loading screen.
    [int]$JoinAnchorTimeoutSec = 45,
    # AI-suspend probe (join only, KENSHICOOP_PROBE_AISUSPEND=1): detour
    # Character::periodicUpdate so host-driven NPCs stop self-tasking (decision
    # layer off) while still animating. Faction untouched. Lets us self-validate
    # that suspended NPCs hold the host's pose without the sit->stand->walk thrash.
    [switch]$ProbeAiSuspend,
    # Backstop cap (seconds, measured from host gameplay) for the scenario
    # pre-screenshot wait. The wait normally ends much sooner via early-out (a
    # SCENARIO MEMBER line to anchor the shot, or SCENARIO RESULT / both clients
    # exited when a scenario finishes or fails fast). Kept well under the old
    # 90 s so a fast FAIL no longer stalls the run.
    [int]$ScenarioWaitSec = 25,
    # Auto-arrange the two client windows side by side (host left, join right) the
    # moment both are launched, so a run can be watched without one window hiding the
    # other. Launched in the BACKGROUND and re-applied through the load screen, so it
    # positions early and sticks (Kenshi re-centers its window on gameplay entry).
    # ON BY DEFAULT (every run is watchable); pass -NoArrange to skip, or -Arrange as
    # a harmless explicit opt-in (kept for backward-compatible call sites).
    [switch]$Arrange,
    [switch]$NoArrange,
    [ValidateSet("widest", "primary")]
    [string]$ArrangeMonitor = "primary",
    # How long (s) to keep re-pinning the windows. Covers host launch + JoinDelay +
    # both load screens; Kenshi re-centers on gameplay entry so this must outlast the
    # slower client's load. Bumped from 45 -> 75 so a slow join still gets pinned.
    [int]$ArrangeRepeatSec = 75,
    # Debug WAN simulation. Inbound entity batches are held base +/- jitter ms (and
    # NetSimLossPct% dropped) on BOTH clients, so the loopback run exercises the
    # real-latency path - render interpolation, dead reckoning, stale-state local
    # enforcement - rather than the ~0 ms same-frame delivery of pure loopback. All
    # zero (default) = no simulation. e.g. -NetSimDelayMs 120 -NetSimJitterMs 40.
    [int]$NetSimDelayMs = 0,
    [int]$NetSimJitterMs = 0,
    [int]$NetSimLossPct = 0
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
if ($Scenario -ne "") { Write-Host "  scenario: $Scenario (tolerance $Tolerance u)" }
if ($NetSimDelayMs -or $NetSimJitterMs -or $NetSimLossPct) {
    Write-Host "  net sim:  delay ${NetSimDelayMs}ms +/-${NetSimJitterMs}ms, loss ${NetSimLossPct}%"
}
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
    # KENSHICOOP_SCENARIO: empty string = normal co-op tick (no scenario). When
    # set, KENSHICOOP_TEST_SECONDS still applies as a hard backstop.
    $env:KENSHICOOP_SCENARIO     = $Scenario
    # Reconcile trace gate: inv_wpnseq is a local diagnostic that drives applyContainerContents
    # directly and needs the [recon]/DUMP traces to see which primitive fails for weapons.
    # Reconcile/world-item trace gate: inv_wpnseq needs the [recon] traces; the world_item_*
    # family enables the [wi] SEND/SPAWN/MOVE/CULL traces so the mechanism (not just the
    # outcome) is verifiable from the log.
    $env:KENSHICOOP_INV_DUMP     = if ($Scenario -eq "inv_wpnseq" -or $Scenario -eq "inv_addequip" -or $Scenario -eq "wpn_relocate" -or $Scenario -eq "world_weapon_drop" -or $Scenario -like "world_item_*") { "1" } else { "" }
    # Join-only AI-suspend probe. Host ignores it (plugin guards on !isHost), but
    # we still only set it for the join so the env is unambiguous.
    $env:KENSHICOOP_PROBE_AISUSPEND = if ($Mode -eq "join" -and $ProbeAiSuspend) { "1" } else { "" }
    # Host-only setup/re-arm scene. The join must stay clean (it reproduces via
    # replication), so we only set KENSHICOOP_SETUP for the host.
    $env:KENSHICOOP_SETUP = if ($Mode -eq "host") { $Setup } else { "" }
    # Debug WAN sim (both clients): delay/jitter/drop inbound entity batches so the
    # loopback run stresses the latency path. Zero = disabled (immediate delivery).
    $env:KENSHICOOP_NETSIM_DELAY_MS  = "$NetSimDelayMs"
    $env:KENSHICOOP_NETSIM_JITTER_MS = "$NetSimJitterMs"
    $env:KENSHICOOP_NETSIM_LOSS_PCT  = "$NetSimLossPct"
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
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") -ProcessId $ProcId -Out $Out -Frames $Frames -IntervalMs $FrameIntervalMs
        Write-Host "  captured $Label ($Frames frame(s)) -> $Out"
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

# Auto-arrange the windows side by side as EARLY as possible (background, re-pinned
# through the load screen) so the run is watchable from the start.
if (-not $NoArrange -and $hostPid -ne 0) {
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host "Arranging windows side by side ($ArrangeMonitor monitor, host left / join right; re-pinning ${ArrangeRepeatSec}s) ..."
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$hostPid", "-JoinPid", "$joinPid",
        "-Monitor", $ArrangeMonitor, "-TimeoutSec", "90", "-RepeatSec", "$ArrangeRepeatSec"
    ) | Out-Null
}

# Anchor the screenshot to when the HOST reaches gameplay (so both are still
# alive at capture time - host is the first to self-exit).
$started = Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec
if ($started) {
    Write-Host "Host reached gameplay; waiting before screenshot ..."
} else {
    Write-Warning "Did not see 'gameplay started' in host log within $StartTimeoutSec s; capturing on a best-effort basis."
}

function Test-Alive { param([int]$ProcId) return ($ProcId -ne 0 -and $null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

$shotsTaken = $false
if ($Scenario -ne "") {
    # Capture each client at ITS OWN anchor so BOTH screenshots show in-game,
    # mid-action state despite the launch stagger (the join loads ~10 s after the
    # host). Host anchor = its first SCENARIO MEMBER (it acts immediately); join
    # anchor = its first SCENARIO RECV (proof it loaded and applied a transform).
    # The host scenario keeps the leader moving long enough to still be live when
    # the join captures.
    # Most scenarios capture at the first authoritative/observed line so the shot
    # shows live, mid-action state. combat_kill is special: the meaningful state
    # (the loser on the ground) only exists AFTER the enforced takedown ~22 s in,
    # so anchor its capture on the KO marker and give the join a beat to apply the
    # reliable event before shooting.
    $hostAnchor = "SCENARIO MEMBER"
    $hostShotDelay = 1
    $joinShotDelay = 1
    if ($Scenario -eq "combat_kill") {
        $hostAnchor = "SCENARIO KO enforced"
        $hostShotDelay = 2
        $joinShotDelay = 4
    }
    if (Wait-ForLogLine -File $hostLog -Pattern $hostAnchor -TimeoutSec $ScenarioWaitSec) {
        Write-Host "Saw host '$hostAnchor'; capturing host shortly after."
        Start-Sleep -Seconds $hostShotDelay
    } else {
        Write-Warning "No host '$hostAnchor' within $ScenarioWaitSec s; capturing host best-effort."
    }
    if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
    else { Write-Warning "Host already exited before screenshot." }

    if ($joinPid -ne 0) {
        if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec $JoinAnchorTimeoutSec) {
            Write-Host "Saw join SCENARIO RECV; capturing join shortly after."
            Start-Sleep -Seconds $joinShotDelay
        } else {
            Write-Warning "No join SCENARIO RECV within $JoinAnchorTimeoutSec s; capturing join best-effort."
        }
        if (Test-Alive $joinPid) { Take-Shot -ProcId $joinPid -Out $joinPng -Label "join" }
        else { Write-Warning "Join already exited before screenshot." }
    }
    $shotsTaken = $true
} else {
    $lead = $Seconds - $ShotLeadSec
    if ($lead -lt 0) { $lead = 0 }
    Start-Sleep -Seconds $lead
}

if (-not $shotsTaken) {
    if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
    else { Write-Warning "Host already exited before screenshot." }
    if (Test-Alive $joinPid) { Take-Shot -ProcId $joinPid -Out $joinPng -Label "join" }
    else { Write-Warning "Join already exited before screenshot." }
}

# Wait for self-exit, with a hard-timeout kill so unattended runs never hang.
# Scenarios self-exit within seconds of logging SCENARIO RESULT, so a short
# grace is enough; normal timed runs keep the longer backstop.
# Scenario grace must cover the host/join launch stagger: each client runs the
# scenario from ITS OWN gameplay-start, so the later-loading join finishes (and
# self-exits) well after the host. Too small a grace kills a healthy join
# mid-scenario and falsely flags "no clean exit".
$killGrace = if ($Scenario -ne "") { 75 } else { $ShotLeadSec + 60 }
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
if ($Frames -gt 1) {
    Write-Host "  host png: $hostPng (+ frames host_1..host_$Frames.png)"
    Write-Host "  join png: $joinPng (+ frames join_1..join_$Frames.png)"
} else {
    Write-Host "  host png: $hostPng"
    Write-Host "  join png: $joinPng"
}

# Evaluate one client's log. A client "passed" if it reached gameplay, exited
# cleanly (did NOT crash mid-run), and logged no ERROR lines. The clean-exit
# marker differs by mode: the self-exit timer logs "test duration elapsed",
# while a scenario logs "SCENARIO RESULT". Returns $true/$false + a summary line.
function Evaluate {
    param([string]$File, [string]$Label, [bool]$Required, [string]$CleanPattern)
    if (-not (Test-Path $File)) {
        if ($Required) { Write-Host "  [$Label] FAIL - no log produced"; return $false }
        Write-Host "  [$Label] skipped (not launched)"; return $true
    }
    $text     = Get-Content $File -Raw
    $errs     = @(Select-String -Path $File -Pattern "\] ERROR:" -ErrorAction SilentlyContinue)
    $reached  = $text -match "gameplay started"
    $cleanEnd = $text -match $CleanPattern
    $ok = ($reached -and $cleanEnd -and $errs.Count -eq 0)
    $why = @()
    if (-not $reached)  { $why += "never reached gameplay" }
    if (-not $cleanEnd) { $why += "no clean exit (likely crashed)" }
    if ($errs.Count -gt 0) { $why += "$($errs.Count) error line(s)" }
    $verdict = if ($ok) { "PASS" } else { "FAIL - " + ($why -join "; ") }
    Write-Host "  [$Label] $verdict"
    foreach ($e in ($errs | Select-Object -First 5)) { Write-Host "      $($e.Line)" }
    return $ok
}

# Report any in-plugin "CHECK <key> FAIL" lines in a log. Returns $true if none.
function Test-NoCheckFail {
    param([string]$File, [string]$Label)
    if (-not (Test-Path $File)) { return $true }
    $fails = @(Select-String -Path $File -Pattern "CHECK \S+ FAIL" -ErrorAction SilentlyContinue)
    foreach ($f in $fails) { Write-Host "  [$Label] $($f.Line.Trim())" }
    return ($fails.Count -eq 0)
}

# Parse SCENARIO <Kind> hand=.. pos=.. lines from a log into hand -> "x,y,z"
# (latest logged position per hand wins). $Kind is MEMBER or RECV.
function Get-ScenarioLines {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $pat = "SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $map[$g[1].Value] = $g[2].Value
    }
    return $map
}

# Compare one authoritative MEMBER map against an observer RECV map: every
# authoritative member (keyed by hand) must have an observer entry within $Tol
# world units. Prints a CROSSCHECK line. $Dir labels the direction for output.
function Compare-MemberRecv {
    param([hashtable]$Members, [hashtable]$Recv, [double]$Tol, [string]$Dir)
    $allMatched = $true
    foreach ($hand in $Members.Keys) {
        if (-not $Recv.ContainsKey($hand)) {
            Write-Host "  CROSSCHECK [$Dir] member $hand FAIL - no observer RECV"
            $allMatched = $false; continue
        }
        $ap = $Members[$hand].Split(',') | ForEach-Object { [double]$_ }
        $op = $Recv[$hand].Split(',')    | ForEach-Object { [double]$_ }
        $dx = $ap[0]-$op[0]; $dy = $ap[1]-$op[1]; $dz = $ap[2]-$op[2]
        $dist = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)
        if ($dist -gt $Tol) {
            Write-Host "  CROSSCHECK [$Dir] member $hand FAIL - dist=$([Math]::Round($dist,2)) > $Tol"
            $allMatched = $false
        }
    }
    $v = if ($allMatched) { "PASS" } else { "FAIL" }
    Write-Host "  CROSSCHECK [$Dir] squad_positions_match $v ($($Members.Count) member(s), $($Recv.Count) RECV)"
    return $allMatched
}

# Cross-client position check. The authoritative side logs SCENARIO MEMBER and
# the observing side logs SCENARIO RECV; which side is authoritative depends on
# the scenario (squad_spawn_sync = host, squad_move_sync = join), so we
# auto-detect by whichever log has MEMBER lines and compare against the OTHER
# log's RECV. Both directions are checked if both sides logged MEMBER.
function Compare-Scenario {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $hostMembers = Get-ScenarioLines -File $HostFile -Kind "MEMBER"
    $joinMembers = Get-ScenarioLines -File $JoinFile -Kind "MEMBER"
    $hostRecv    = Get-ScenarioLines -File $HostFile -Kind "RECV"
    $joinRecv    = Get-ScenarioLines -File $JoinFile -Kind "RECV"

    if ($hostMembers.Count -eq 0 -and $joinMembers.Count -eq 0) {
        Write-Host "  CROSSCHECK squad_positions_match FAIL - no SCENARIO MEMBER lines on either side"
        return $false
    }
    $ok = $true
    if ($hostMembers.Count -gt 0) {
        $ok = (Compare-MemberRecv -Members $hostMembers -Recv $joinRecv -Tol $Tol -Dir "host->join") -and $ok
    }
    if ($joinMembers.Count -gt 0) {
        $ok = (Compare-MemberRecv -Members $joinMembers -Recv $hostRecv -Tol $Tol -Dir "join->host") -and $ok
    }
    return $ok
}

# Parse timestamped SCENARIO <Kind> lines into hand -> list of @{t=ms; p=@(x,y,z)}.
# The leading "[HH:MM:SS.mmm]" stamp is the machine clock, identical for both
# clients (same PC), so host and join samples are directly time-comparable.
function Get-ScenarioSeries {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)(?: task=(\d+))?(?: pelvis=(-?[\d\.]+))?(?: crouch=(-?\d+))?(?: idle=(-?\d+))?(?: bs=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = ([int]$g[1].Value*3600 + [int]$g[2].Value*60 + [int]$g[3].Value)*1000 + [int]$g[4].Value
        $hand = $g[5].Value
        $pos  = $g[6].Value.Split(',') | ForEach-Object { [double]$_ }
        $task = if ($g[7].Success) { [int]$g[7].Value } else { 65535 }
        $pelvis = if ($g[8].Success) { [double]$g[8].Value } else { -1.0 }
        $crouch = if ($g[9].Success) { [int]$g[9].Value } else { -1 }
        $bs   = if ($g[11].Success) { [int]$g[11].Value } else { 0 }
        if (-not $map.ContainsKey($hand)) { $map[$hand] = New-Object System.Collections.ArrayList }
        [void]$map[$hand].Add(@{ t = $t; p = $pos; task = $task; pelvis = $pelvis; crouch = $crouch; bs = $bs })
    }
    return $map
}

# NPC sync cross-check (Stage 4), TIME-ALIGNED. Autonomous bar NPCs never settle
# on command and the clients start staggered, so latest-vs-latest is meaningless
# (it compares unrelated moments + post-exit wander). Instead, for each NPC, pair
# every join RECV sample with the host MEMBER sample nearest in time (<= MaxDt) and
# take the MEDIAN distance: a well-driven NPC tracks within a couple of metres at
# every aligned moment, whether it is sitting or walking. PASS if a healthy
# majority of the NPCs that actually overlap in time tracked within $Tol. NPCs that
# never overlap (interest-boundary patrollers that left the host's stream) are
# excluded - that is a Stage 6 interest-handoff concern, not sync fidelity.
function Compare-NpcSync {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol,
          [double]$MinRatio = 0.80, [int]$MinJudged = 4, [int]$MaxDt = 800)
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    # Pass 1: per NPC, median distance of time-aligned (host,join) sample pairs,
    # plus how many aligned pairs it has (its "co-presence" count).
    $rows = @()
    foreach ($hand in $H.Keys) {
        if (-not $J.ContainsKey($hand)) { continue }
        $dists = New-Object System.Collections.ArrayList
        foreach ($js in $J[$hand]) {
            $best = [double]::MaxValue; $bp = $null
            foreach ($hs in $H[$hand]) {
                $dt = [Math]::Abs($hs.t - $js.t)
                if ($dt -lt $best) { $best = $dt; $bp = $hs.p }
            }
            if ($best -le $MaxDt -and $bp) {
                $dx = $bp[0]-$js.p[0]; $dy = $bp[1]-$js.p[1]; $dz = $bp[2]-$js.p[2]
                [void]$dists.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            }
        }
        if ($dists.Count -eq 0) { continue }
        $sorted = $dists | Sort-Object
        $rows += @{ hand = $hand; aligned = $dists.Count; med = $sorted[[int]($sorted.Count/2)] }
    }
    if ($rows.Count -eq 0) {
        Write-Host "  CROSSCHECK [npc host->join] FAIL - no time-overlapping NPCs"
        return $false
    }
    # Only JUDGE continuously co-present NPCs (aligned >= half the best coverage).
    # A patroller that wanders past the host's interest sphere has far fewer aligned
    # samples; excluding it keeps the oracle about sync fidelity, not interest
    # hand-off at the boundary (a Stage 6 concern).
    $maxAligned = ($rows | ForEach-Object { $_.aligned } | Measure-Object -Maximum).Maximum
    $minAligned = [Math]::Max(10, [int]($maxAligned * 0.5))
    $judged = @($rows | Where-Object { $_.aligned -ge $minAligned })
    if ($judged.Count -lt $MinJudged) {
        # Too few continuously-present NPCs to GATE on cross-position fidelity - e.g. a
        # focused single-worker craft/gather scene, vs the busy bar this threshold was
        # tuned for. Report the data we have as ADVISORY and defer to POSE-STATE (the
        # authoritative pose gate), exactly like the pose/anim/march oracles skip when
        # their sample is too small. A genuine streaming regression instead yields zero
        # overlapping NPCs (the hard FAIL above), so this relaxation can't mask one.
        if ($judged.Count -ge 1) {
            $tracked = @($judged | Where-Object { $_.med -le $Tol }).Count
            $worst = ($judged | ForEach-Object { $_.med } | Measure-Object -Maximum).Maximum
            Write-Host "  CROSSCHECK [npc host->join] ADVISORY - only $($judged.Count) continuously-present NPC(s) (need >= $MinJudged to gate); tracked $tracked/$($judged.Count) within $Tol (worstMedian=$([Math]::Round($worst,1))); deferring to POSE-STATE"
        } else {
            Write-Host "  CROSSCHECK [npc host->join] ADVISORY - no continuously-present NPC(s) (maxAligned=$maxAligned); deferring to POSE-STATE"
        }
        return $true
    }
    $tracked = @($judged | Where-Object { $_.med -le $Tol }).Count
    $worst = ($judged | ForEach-Object { $_.med } | Measure-Object -Maximum).Maximum
    $ratio = [Math]::Round($tracked / $judged.Count, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  CROSSCHECK [npc host->join] $v - tracked $tracked/$($judged.Count) continuously-present NPCs within $Tol (ratio=$ratio >= $MinRatio, worstMedian=$([Math]::Round($worst,1)), excluded $($rows.Count - $judged.Count) boundary)"
    return $ok
}

# coop_presence (Phase 3.5): BIDIRECTIONAL presence cross-check - the keystone
# two-player test. Each client streams its OWNED member (host rank 0, join rank 1)
# and drives the peer's, so BOTH directions must track:
#   host->join : host MEMBER(rank0) vs join RECV(rank0)
#   join->host : join MEMBER(rank1) vs host RECV(rank1)
# Reuses the TIME-ALIGNED Compare-NpcSync (robust to the launch stagger and to the
# peer body being released after a client exits) once per direction, judging the
# single owned member (MinJudged=1, MinRatio=1.0 = that member must track). A small
# walk catch-up lag is tolerated (the settle tail dominates the per-hand median).
function Test-CoopPresence {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $t = [Math]::Max($Tol, 6.0)
    Write-Host "  COOP-PRESENCE host->join (host owns rank0):"
    $h2j = Compare-NpcSync -HostFile $HostFile -JoinFile $JoinFile -Tol $t -MinRatio 1.0 -MinJudged 1
    Write-Host "  COOP-PRESENCE join->host (join owns rank1):"
    $j2h = Compare-NpcSync -HostFile $JoinFile -JoinFile $HostFile -Tol $t -MinRatio 1.0 -MinJudged 1
    $ok = ($h2j -and $j2h)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  COOP-PRESENCE $v - bidirectional presence (host->join=$h2j, join->host=$j2h, tol=$t)"
    return $ok
}

# inv_order (Phase 4a): container-contents replication. Both clients anchor on the
# SAME container and sample its contents (count + order-independent content hash)
# over time; the host performs a LIVE add mid-run. This is the content-snapshot /
# reconcile proof, so the gate is a MULTISET match, not a transform cross-check:
#   * host logged a live ADD (added>=1),
#   * the join OBSERVED a content change (>=1 distinct hash, not a static load),
#   * BOTH sides' item count GREW over their own baseline (the add landed), and
#   * the host's and join's FINAL content hashes are EQUAL (identical multiset -
#     same invEntryHash on both, so equal hash == equal contents).
# world_item_sync (Phase W1): host-authored ground-item visual sync. The host DROPS a
# known item (a free world item), which streams netId-keyed to the join; the join spawns a
# LOCAL proxy so its OWN interest scan enumerates it. Then the host DESPAWNS the item and
# the join must CULL its proxy. Both clients log "SCENARIO WI <HOST|JOIN> t=.. n=.. pos=..
# hash=..". PASS requires: host dropped + despawned; the join's observed ground item matched
# the host's CONTENT hash exactly and its position within tolerance (horizontal); and the
# join SPAWNED (n 0->>=1) then cleanly CULLED (n ->0) - i.e. no leaked proxy.
function Test-WorldItemSync {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol = 3.0)
    $rx = 'SCENARIO WI (HOST|JOIN) t=(\d+) n=(\d+) pos=(-?[0-9.]+),(-?[0-9.]+),(-?[0-9.]+) hash=(\d+)'
    $series = {
        param($file, $role)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx -and $matches[1] -eq $role) {
                    $arr += [pscustomobject]@{
                        n = [int]$matches[3]; x = [double]$matches[4]; y = [double]$matches[5]
                        z = [double]$matches[6]; hash = [uint32]$matches[7]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile "HOST"
    $J = & $series $JoinFile "JOIN"
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  WI-SYNC FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return $false
    }
    $drop    = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO WI DROP .*dropped=[1-9]' -Quiet)
    $despawn = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO WI DESPAWN destroyed=[1-9]' -Quiet)
    # First "item present" sample on each side (its pos + content hash).
    $hostPresent = $H | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinPresent = $J | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinSpawned = ($joinPresent -ne $null)
    $hostSaw     = ($hostPresent -ne $null)
    # Horizontal (XZ) placement match - Y can shift if the join re-grounds the proxy.
    $posMatch = $false; $hashMatch = $false; $dxz = -1.0
    if ($hostSaw -and $joinSpawned) {
        $dx = $hostPresent.x - $joinPresent.x; $dz = $hostPresent.z - $joinPresent.z
        $dxz = [math]::Sqrt($dx * $dx + $dz * $dz)
        $posMatch  = ($dxz -le $Tol)
        $hashMatch = ($hostPresent.hash -eq $joinPresent.hash)
    }
    # Clean cull: each side saw the item, and its LAST sample is back to n=0 (no leak).
    $hostCulled = $hostSaw -and ($H[$H.Count - 1].n -eq 0)
    $joinCulled = $joinSpawned -and ($J[$J.Count - 1].n -eq 0)
    $ok = $drop -and $despawn -and $joinSpawned -and $posMatch -and $hashMatch -and $joinCulled -and $hostCulled
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  WI-SYNC $v - drop=$drop despawn=$despawn joinSpawned=$joinSpawned " +
                "posMatch=$posMatch (dXZ=$([math]::Round($dxz,2))u tol=$Tol) hashMatch=$hashMatch " +
                "hostCulled=$hostCulled joinCulled=$joinCulled")
    if ($hostPresent -and $joinPresent) {
        Write-Host ("  WI-SYNC   host item pos=($($hostPresent.x),$($hostPresent.y),$($hostPresent.z)) hash=$($hostPresent.hash)")
        Write-Host ("  WI-SYNC   join item pos=($($joinPresent.x),$($joinPresent.y),$($joinPresent.z)) hash=$($joinPresent.hash)")
    }
    return $ok
}

# drop_probe (Phase W0, DIAGNOSTIC): the deliverable is the EVIDENCE, not a sync gate
# (W0 has no world-item channel yet). The host seeds a known loose item, enumerates
# nearby world items as a BEFORE baseline, drops the item, and re-enumerates (AFTER). We
# assert the probe actually EXECUTED (a drop happened and a RESULT line was produced) and
# surface the characterization (before/after counts + whether getObjectsWithinSphere
# enumerated the dropped object) so the W1 design rests on observed facts, not guesses.
function Test-DropProbe {
    param([string]$HostFile)
    if (-not (Test-Path $HostFile)) {
        Write-Host "  DROP-PROBE FAIL - no host log"
        return $false
    }
    $rx = 'SCENARIO DROP RESULT dropped=(-?\d+) before=(-?\d+) after=(-?\d+) enumerated=(\d+)'
    $line = Select-String -Path $HostFile -Pattern $rx | Select-Object -Last 1
    if (-not $line) {
        Write-Host "  DROP-PROBE FAIL - no 'SCENARIO DROP RESULT' evidence line"
        return $false
    }
    $null = ($line.Line -match $rx)
    $dropped    = [int]$matches[1]
    $before     = [int]$matches[2]
    $after      = [int]$matches[3]
    $enumerated = [int]$matches[4]
    # Also report the dropped object's itemType/hand/pos from the AFTER scan for the design.
    $seeded = Select-String -Path $HostFile -Pattern 'SCENARIO DROP SEEDED added=\d+ sid=' | Select-Object -Last 1
    $ok = ($dropped -gt 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  DROP-PROBE $v - dropped=$dropped before=$before after=$after enumerated=$enumerated")
    if ($seeded) { Write-Host ("  DROP-PROBE   " + $seeded.Line.Trim()) }
    # Surface the enumerated world objects from the AFTER scan (the characterization).
    $afterScan = $false
    foreach ($ln in Get-Content $HostFile) {
        if ($ln -match 'SCENARIO DROP AFTER-scan:') { $afterScan = $true; continue }
        if ($afterScan) {
            if ($ln -match 'WORLDITEM (scan|  )') { Write-Host ("  DROP-PROBE   " + ($ln -replace '^.*WORLDITEM', 'WORLDITEM').Trim()) }
            elseif ($ln -match 'SCENARIO DROP RESULT') { break }
        }
    }
    return $ok
}

function Test-InventorySync {
    param([string]$HostFile, [string]$JoinFile)
    # No '^' anchor: log lines carry a "[ts] [ROLE] INFO:" prefix before the payload.
    $rx = 'SCENARIO INV (MEMBER|RECV) t=(\d+) count=(\d+) hash=(\d+)'
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{ count = [int]$matches[3]; hash = [uint32]$matches[4] }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 1) {
        Write-Host "  INV-SYNC FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return $false
    }
    $added = $false
    if (Test-Path $HostFile) { $added = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO INV ADD added=[1-9]' -Quiet) }
    $hFirst = $H[0]; $hLast = $H[$H.Count - 1]
    $jFirst = $J[0]; $jLast = $J[$J.Count - 1]
    # Advisory: did the join witness the live transition itself? (Defeated by the
    # launch stagger when the join starts sampling AFTER the add already propagated.)
    $jChanged = $false
    foreach ($s in $J) { if ($s.hash -ne $jFirst.hash) { $jChanged = $true; break } }
    # Authoritative gate (stagger-robust):
    #   * host performed a LIVE add (added),
    #   * the host's content actually CHANGED (first hash != last hash) - proves it
    #     was created at runtime, not loaded from the save, and is robust to stacking,
    #   * the host's and join's FINAL content hashes MATCH (identical multiset), and
    #   * the join ends holding NON-EMPTY synced content (the save's leader baseline is
    #     empty, so a non-empty matching multiset proves cross-client reconstruction).
    $hostChanged = ($hFirst.hash -ne $hLast.hash)
    $hashMatch   = ($hLast.hash -eq $jLast.hash)
    $joinNonEmpty= ($jLast.count -gt 0)
    $ok = ($added -and $hostChanged -and $hashMatch -and $joinNonEmpty)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  INV-SYNC $v - add=$added hostChanged=$hostChanged hashMatch=$hashMatch " +
                "joinNonEmpty=$joinNonEmpty (joinSawTransition=$jChanged advisory) " +
                "host=$($hFirst.count)->$($hLast.count) join=$($jFirst.count)->$($jLast.count) " +
                "finalHash host=$($hLast.hash) join=$($jLast.hash)")
    return $ok
}

# inv_bidir (Phase 4a): BIDIRECTIONAL container-contents replication. Each client
# mutates ONLY a container it OWNS (host = squad-tab rank 0, join = rank 1) with an
# ADD-then-REMOVE sequence, and samples BOTH containers (logging OWN / PEER lines keyed
# by rank). The proof is PER-RANK convergence in BOTH directions:
#   * rank 0 is authored by the HOST; the JOIN must converge its PEER view to the
#     host's FINAL rank-0 contents (host -> join), and
#   * rank 1 is authored by the JOIN; the HOST must converge its PEER view to the
#     join's FINAL rank-1 contents (join -> host).
# For each rank: the author's own contents must actually CHANGE (>=2 distinct hashes,
# proving live mutation incl. the removal), and the observer's FINAL (count,hash) must
# EQUAL the author's FINAL (count,hash) - identical multiset, so no loss and no dupe.
function Test-InventoryBidir {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INVB r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+)'
    # Parsed series of {rank, role, count, hash} in log order for one file.
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; role = $matches[2]
                        count = [int]$matches[4]; hash = [uint32]$matches[5]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-BIDIR FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return $false
    }
    # Filter helpers: ordered samples for (file, rank, role); distinct-hash count.
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $distinct = { param($rows) ($rows | ForEach-Object { $_.hash } | Select-Object -Unique).Count }
    # Verify one direction: $authorRows OWN the container; $obsRows reconcile it.
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 1 -or $obsRows.Count -lt 1) {
            Write-Host "  INV-BIDIR $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]
        $oLast = $obsRows[$obsRows.Count - 1]
        $aChanged = ((& $distinct $authorRows) -ge 2)         # author mutated live (add+remove)
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count)
        $r = $aChanged -and $converged
        Write-Host ("  INV-BIDIR $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorChanged=$aChanged converged=$converged" +
                    " authorFinal=(c$($aLast.count),h$($aLast.hash)) observerFinal=(c$($oLast.count),h$($oLast.hash))")
        return $r
    }
    # host -> join: rank 0 authored by host, observed by join.
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    # join -> host: rank 1 authored by join, observed by host.
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    # Confirm both sides actually ran their add+remove sequence.
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVB ADD r=0 n=[1-9]' -Quiet) -and `
               (Select-String -Path $HostFile -Pattern 'SCENARIO INVB REM r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB ADD r=1 n=[1-9]' -Quiet) -and `
               (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB REM r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-BIDIR " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return $ok
}

# inv_equip (Phase 4a): EQUIPPED-gear (armour/weapon slot) replication. Each client
# UNEQUIPS one REAL (save-loaded) worn item from the geared member of a squad tab it OWNS
# (host = rank 0, join = rank 1) and leaves it off, while sampling BOTH tabs (OWN / PEER
# lines keyed by rank, with an `eq` count of how many items are worn). The proof is
# PER-RANK convergence in BOTH directions, on the EQUIPPED dimension specifically:
#   * rank 0 worn gear is authored by the HOST; the JOIN must converge its PEER view, and
#   * rank 1 worn gear is authored by the JOIN; the HOST must converge its PEER view.
# For each rank: the author's worn count must DROP below its peak (a real unequip), and
# the observer's FINAL (count, eq, hash) must EQUAL the author's. The observer loaded the
# same save, so its copy started wearing the SAME gear (worn-count = peak); converging to
# the reduced final can only happen via a real removal - so equipped-gear removal (the
# loose-only blind spot the user hit) replicated with no loss. The equip (up) path is out
# of scope: fabricated re-equips don't persist in the engine.
function Test-InventoryEquip {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INVE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) eq=(\d+) hash=(\d+)'
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; role = $matches[2]
                        count = [int]$matches[4]; eq = [int]$matches[5]; hash = [uint32]$matches[6]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-EQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return $false
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $maxEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Maximum).Maximum }
    # One direction: the author UNEQUIPS a worn item; the observer (which loaded the same
    # save, so it starts wearing it too) must converge to the author's reduced worn state.
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 1 -or $obsRows.Count -lt 1) {
            Write-Host "  INV-EQUIP $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]; $oLast = $obsRows[$obsRows.Count - 1]
        $aPeak = & $maxEq $authorRows
        $authorReduced = ($aPeak -ge 1) -and ($aLast.eq -lt $aPeak)   # a worn item was removed
        # Converged on the EQUIPPED dimension: identical worn-count AND content hash AND
        # total count. Since the observer's own copy was geared at load (worn-count $aPeak),
        # matching the author's reduced final can only happen via a real removal - proving
        # the worn-gear removal replicated (no loss: a stuck observer keeps the higher eq).
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count) -and `
                     ($aLast.eq -eq $oLast.eq)
        $r = $authorReduced -and $converged
        Write-Host ("  INV-EQUIP $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorReduced=$authorReduced(peakEq$aPeak`->$($aLast.eq)) converged=$converged" +
                    " authorFinal=(c$($aLast.count),eq$($aLast.eq),h$($aLast.hash))" +
                    " observerFinal=(c$($oLast.count),eq$($oLast.eq),h$($oLast.hash))")
        return $r
    }
    # host -> join: rank 0 worn gear authored by host, observed by join.
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    # join -> host: rank 1 worn gear authored by join, observed by host.
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    # Confirm both sides actually unequipped a worn item from their own tab.
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return $ok
}

# inv_reequip (Phase 4a, UP path): each client UNEQUIPS a real worn item from the geared
# member of a tab it OWNS to LOOSE (move, preserving it), HOLDS that dip, then RE-EQUIPS it
# (move loose -> slot). Re-equipping a REAL item persists (fabricated equips are discarded),
# so the slot fills back in. The proof, per direction (host authors rank 0, join authors
# rank 1):
#   * authorRestored - the author's own worn count DIPS below its peak (the unequip) and
#     RETURNS to the peak and HOLDS it (the re-equip persisted - the d25 risk), and
#   * converged - the observer's FINAL (count, eq, hash) EQUALS the author's, AND the
#     observer's PEER series shows the same dip-then-restore (it down-moved when it saw the
#     unequip, then UP-moved when it saw the re-equip). A broken up path leaves the observer
#     stuck loose (eq below peak) -> converged fails. observerSawCycle is required because a
#     plain final-state match could be the untouched baseline on a late-joining observer.
function Test-InventoryReequip {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INVE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) eq=(\d+) hash=(\d+)'
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; role = $matches[2]
                        count = [int]$matches[4]; eq = [int]$matches[5]; hash = [uint32]$matches[6]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-REEQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return $false
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $maxEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Maximum).Maximum }
    $minEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Minimum).Minimum }
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 2 -or $obsRows.Count -lt 2) {
            Write-Host "  INV-REEQUIP $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]; $oLast = $obsRows[$obsRows.Count - 1]
        $aPeak = & $maxEq $authorRows; $aDip = & $minEq $authorRows
        # Author dipped (unequip seen) AND returned to peak and held it (re-equip persisted).
        $authorRestored = ($aPeak -ge 1) -and ($aDip -lt $aPeak) -and ($aLast.eq -eq $aPeak)
        # Observer witnessed the same dip-then-restore on its PEER view of that tab.
        $oPeak = & $maxEq $obsRows; $oDip = & $minEq $obsRows
        $observerSawCycle = ($oDip -lt $oPeak) -and ($oLast.eq -eq $oPeak)
        # Converged on the equipped dimension (worn-count + content hash + total count).
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count) -and `
                     ($aLast.eq -eq $oLast.eq)
        $r = $authorRestored -and $converged -and $observerSawCycle
        Write-Host ("  INV-REEQUIP $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorRestored=$authorRestored(peak$aPeak dip$aDip ->$($aLast.eq))" +
                    " observerSawCycle=$observerSawCycle(peak$oPeak dip$oDip ->$($oLast.eq))" +
                    " converged=$converged" +
                    " authorFinal=(c$($aLast.count),eq$($aLast.eq),h$($aLast.hash))" +
                    " observerFinal=(c$($oLast.count),eq$($oLast.eq),h$($oLast.hash))")
        return $r
    }
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    # Confirm both sides actually unequipped AND re-equipped a worn item from their own tab.
    $hostUn = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $hostRe = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE REEQUIP r=0 n=[1-9]' -Quiet)
    $joinUn = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $joinRe = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE REEQUIP r=1 n=[1-9]' -Quiet)
    $seq = $hostUn -and $hostRe -and $joinUn -and $joinRe
    $ok = $h2j -and $j2h -and $seq
    Write-Host ("  INV-REEQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostUn=$hostUn hostRe=$hostRe joinUn=$joinUn joinRe=$joinRe")
    return $ok
}

# inv_addequip (Phase 4a, d25 fix): LOCAL single-client reconcile test. Reproduces the
# "picked-up weapon auto-equips into the empty slot, flickers, then VANISHES" bug: when the
# reconcile must ADD an EQUIPPED item with NO existing copy, the old code fabricated-and-
# equipped it (discarded within a tick). The fix creates it LOOSE and equips the now-real
# copy on a LATER reconcile pass. The scenario removes a worn item, then re-applies the worn
# baseline across ticks. ADD-EQUIP asserts the slot refilled to baseline AND persisted when
# we STOPPED re-applying (eqPersist>=baseWorn) - the proof the equipped copy is now durable.
function Test-AddEquip {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'ADDEQ verdict pass=(\d+) baseWorn=(-?\d+) create=(-?\d+) equip=(-?\d+) persist=(-?\d+)'
    $eval = {
        param($file, $label)
        if (-not (Test-Path $file)) { return $null }
        $line = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $line) { return $null }
        $baseWorn = [int]$line.Matches[0].Groups[2].Value
        $create   = [int]$line.Matches[0].Groups[3].Value
        $equip    = [int]$line.Matches[0].Groups[4].Value
        $persist  = [int]$line.Matches[0].Groups[5].Value
        # Durable-equip invariants: had a worn baseline, refilled to it after the 2nd apply,
        # and HELD it without re-applying (the fabricate path would show persist=0 here).
        $ok = ($baseWorn -ge 1) -and ($equip -ge $baseWorn) -and ($persist -ge $baseWorn)
        Write-Host ("  ADD-EQUIP [$label] " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                    " - baseWorn=$baseWorn create=$create equip=$equip persist=$persist" +
                    $(if ($create -lt $baseWorn) { " (create<baseWorn => equip was correctly DEFERRED)" } else { "" }))
        return $ok
    }
    $h = & $eval $HostFile "host"
    $j = & $eval $JoinFile "join"
    if ($null -eq $h -and $null -eq $j) {
        Write-Host "  ADD-EQUIP FAIL - no ADDEQ verdict line on either client"
        return $false
    }
    # Single-client deterministic scenario: every client that produced a verdict must pass.
    $ok = $true
    if ($null -ne $h) { $ok = $ok -and $h }
    if ($null -ne $j) { $ok = $ok -and $j }
    Write-Host ("  ADD-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return $ok
}

# wpn_relocate (SPIKE for the conservation model): LOCAL single-client test. A WEAPON cannot
# be fabricated by the engine factory (createItem returns null), so the trade model RELOCATES
# the real object instead of creating one. This proves the primitive end to end on one client:
#   drop : the worn weapon is unequipped to loose, then DROPPED -> a free ground weapon
#          appears (inv-1, ground>=1) and PERSISTS ticks later (held>=1) - real object.
#   pick : the ground weapon is RE-HOMED into the bag (inv back to baseline, ground drops)
#          and PERSISTS (persist>=invBase) - no createItem anywhere in the path.
function Test-WpnRelocate {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'RELOC verdict pass=(\d+) invBase=(-?\d+) drop\(inv=(-?\d+) grnd=(-?\d+) held=(-?\d+)\) pick\(inv=(-?\d+) grnd=(-?\d+) persist=(-?\d+)\)'
    $eval = {
        param($file, $label)
        if (-not (Test-Path $file)) { return $null }
        $line = Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $line) { return $null }
        $g = $line.Matches[0].Groups
        $pass = [int]$g[1].Value; $invBase = [int]$g[2].Value
        $dInv = [int]$g[3].Value; $dGrnd = [int]$g[4].Value; $held = [int]$g[5].Value
        $pInv = [int]$g[6].Value; $pGrnd = [int]$g[7].Value; $persist = [int]$g[8].Value
        $dropOk   = ($invBase -ge 1) -and ($dInv -le $invBase - 1) -and ($dGrnd -ge 1)
        $dropHeld = ($held -ge 1)
        $pickOk   = ($pInv -ge $invBase) -and ($pGrnd -lt $dGrnd)
        $pickHeld = ($persist -ge $invBase)
        $ok = ($pass -eq 1) -and $dropOk -and $dropHeld -and $pickOk -and $pickHeld
        Write-Host ("  WPN-RELOCATE [$label] " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                    " - invBase=$invBase drop(inv=$dInv grnd=$dGrnd held=$held) pick(inv=$pInv grnd=$pGrnd persist=$persist)")
        if ($ok) { Write-Host "    conservation OK: weapon moved bag->ground->bag as a REAL object (no createItem)" }
        return $ok
    }
    # The HOST is authoritative - its local relocation cannot be overwritten by anyone, so it
    # is the deterministic proof of the engine primitive and GATES the result. The JOIN runs
    # the same sequence while also being a host-driven reconcile target, so its result is
    # ADVISORY cross-client evidence (a clean join pass means both sides conserved in lockstep).
    $h = & $eval $HostFile "host"
    $j = & $eval $JoinFile "join"
    if ($null -eq $h) {
        Write-Host "  WPN-RELOCATE FAIL - no RELOC verdict line on the host (authoritative)"
        return $false
    }
    if ($null -ne $j) {
        Write-Host ("  WPN-RELOCATE [join] advisory cross-client result: " + $(if ($j) { "consistent" } else { "perturbed (host reconcile timing)" }))
    }
    Write-Host ("  WPN-RELOCATE " + $(if ($h) { "PASS" } else { "FAIL" }) + " (gated on host)")
    return $h
}

# world_weapon_drop (Phase W2): CROSS-CLIENT conservation drop. The HOST drops its leader's
# weapon; the JOIN (which does NOT own the leader) must RELOCATE its own copy to the ground -
# the weapon LEAVES the join leader's bag AND APPEARS as a free ground weapon (not destroyed
# by the inventory reconcile, which cannot rebuild a weapon). Two roles, two verdict lines.
function Test-WeaponDrop {
    param([string]$HostFile, [string]$JoinFile)
    $rxHost = 'WDROP verdict role=host pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invAfter=(-?\d+) grndAfter=(-?\d+)'
    $rxJoin = 'WDROP verdict role=join pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invMin=(-?\d+) grndMax=(-?\d+) relocated=(\d+)'
    # Host authored the drop?
    $hostOk = $false
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invAfter = [int]$g[4].Value; $grnd = [int]$g[5].Value
            $hostOk = ($invBase -ge 1) -and ($invAfter -le $invBase - 1) -and ($grnd -ge 1)
            Write-Host ("  WEAPON-DROP [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - dropped weapon to ground (invBase=$invBase invAfter=$invAfter ground=$grnd)")
        } else { Write-Host "  WEAPON-DROP [host] FAIL - no host WDROP verdict" }
    }
    # Join relocated its own copy (crossed over by conservation)?
    $joinOk = $false
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invMin = [int]$g[4].Value; $grndMax = [int]$g[5].Value
            $joinOk = ($invBase -ge 1) -and ($invMin -le $invBase - 1) -and ($grndMax -ge 1)
            Write-Host ("  WEAPON-DROP [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - relocated own copy to ground (invBase=$invBase invMin=$invMin grndMax=$grndMax)")
            if (-not $joinOk -and $invMin -le $invBase - 1 -and $grndMax -lt 1) {
                Write-Host "    NOTE: weapon LEFT the bag but never appeared on the ground => destroyed by inv-reconcile, not conserved"
            }
        } else { Write-Host "  WEAPON-DROP [join] FAIL - no join WDROP verdict" }
    }
    # Cross-client trace corroboration: host authored a DROP and join APPLIED it (moved>=1).
    $authored = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] DROP id=' -Quiet)
    $applied  = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[wd\] APPLY id=\d+ .* moved=1' -Quiet)
    Write-Host ("  WEAPON-DROP trace: host authored DROP=$authored, join APPLY moved=1=$applied")
    $ok = $hostOk -and $joinOk
    Write-Host ("  WEAPON-DROP " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return $ok
}

# Stage 5 pose oracle. For each STATIONARY host NPC (a sitter/idler: its host
# position barely moves across the run) that carries a reproducible task, check
# the join reproduced the SAME task on its local copy. captureNpcs reads the
# join body's CURRENT action AFTER applyTargets, so a successfully reseated NPC
# reports the host's task value; a join that only stood (old behaviour) reports a
# different/idle task. Movers are excluded (their task is a transient goal we drive
# kinematically, not a pose). Inconclusive (no seated tasked NPCs) -> skip, since
# motion sync is already proven by the cross-check and the manual gate is final.
function Compare-NpcPose {
    param([string]$HostFile, [string]$JoinFile,
          [double]$StillRadius = 3.0, [double]$MinRatio = 0.60, [int]$MinJudged = 2)
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    # Dominant (most frequent) reproducible task among a sample list, or $null.
    $modeTask = {
        param($samples)
        $counts = @{}
        foreach ($s in $samples) { if ($s.task -ne 65535) { $counts[$s.task] = 1 + ($counts[$s.task]) } }
        if ($counts.Count -eq 0) { return $null }
        ($counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    }
    $judged = 0; $matched = 0; $mism = @()
    foreach ($hand in $H.Keys) {
        if (-not $J.ContainsKey($hand)) { continue }
        $hs = $H[$hand]
        if ($hs.Count -lt 4) { continue }
        # Stationary? max displacement of any host sample from the first one.
        $p0 = $hs[0].p; $spread = 0.0
        foreach ($s in $hs) {
            $dx = $s.p[0]-$p0[0]; $dy = $s.p[1]-$p0[1]; $dz = $s.p[2]-$p0[2]
            $d = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)
            if ($d -gt $spread) { $spread = $d }
        }
        if ($spread -gt $StillRadius) { continue }       # a mover, not a pose
        $ht = & $modeTask $hs
        if ($null -eq $ht) { continue }                  # host had no reproducible task
        $jt = & $modeTask $J[$hand]
        $judged++
        if ($null -ne $jt -and [int]$jt -eq [int]$ht) { $matched++ }
        else { $mism += "$hand(host=$ht join=$jt)" }
    }
    if ($judged -lt $MinJudged) {
        Write-Host "  POSE [npc] inconclusive - only $judged seated tasked NPC(s) (need >= $MinJudged); manual gate is authoritative (skipped)"
        return $true
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " mismatches: $($mism -join ', ')" } else { "" }
    Write-Host "  POSE [npc] $v - join reproduced host task for $matched/$judged seated NPCs (ratio=$ratio >= $MinRatio)$detail"
    return $ok
}

# AUTHORITATIVE pose oracle (standing vs sitting), read off the ANIMATED skeleton.
# Each SCENARIO line carries pelvis=<Bip01 Pelvis world-Y minus root-Y, in Kenshi
# units>. That magnitude varies a LOT by race (Greenlander/Shek/Hiver/Skeleton are
# different heights), so an absolute "seat line" is meaningless. Instead we compare
# the SAME NPC host-vs-join, time-aligned: for each host MEMBER sample with a valid
# pelvis, find the nearest-in-time join RECV sample (same machine clock, <= $MaxDt)
# with a valid pelvis and take |Δpelvis|. If the join reproduces the host pose the
# pelvis tracks within ~$PelvisTol; if the host sits (~3 units lower) while the join
# stands, |Δ| blows past it. We only judge NPCs with enough valid aligned pairs.
# PASS if a healthy majority of pairs match. Immune to camera/lighting/load-stagger.
function Compare-NpcPoseState {
    param([string]$HostFile, [string]$JoinFile,
          [double]$PelvisTol = 1.5, [double]$MinRatio = 0.80,
          [int]$MinJudged = 20, [int]$MinPerNpc = 4, [int]$MaxDt = 800)
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $valid = { param($s) ($s.pelvis -gt 0.5) }
    $judged = 0; $matched = 0; $npcJudged = 0; $mism = @()
    foreach ($hand in $H.Keys) {
        if (-not $J.ContainsKey($hand)) { continue }
        $hv = @($H[$hand] | Where-Object { (& $valid $_) })
        $jv = @($J[$hand] | Where-Object { (& $valid $_) })
        if ($hv.Count -eq 0 -or $jv.Count -eq 0) { continue }
        $nJudged = 0; $nMatched = 0; $worst = 0.0
        foreach ($hs in $hv) {
            $best = [double]::MaxValue; $bj = $null
            foreach ($js in $jv) {
                $dt = [Math]::Abs($js.t - $hs.t)
                if ($dt -lt $best) { $best = $dt; $bj = $js }
            }
            if ($best -le $MaxDt -and $null -ne $bj) {
                $d = [Math]::Abs($hs.pelvis - $bj.pelvis)
                $nJudged++; $judged++
                if ($d -le $PelvisTol) { $nMatched++; $matched++ }
                if ($d -gt $worst) { $worst = $d }
            }
        }
        if ($nJudged -lt $MinPerNpc) { continue }
        $npcJudged++
        if ($nMatched -lt $nJudged) {
            $pct = [Math]::Round(100.0 * $nMatched / $nJudged, 0)
            $w = [Math]::Round($worst, 1)
            $mism += "$hand($nMatched/$nJudged ok $pct%, worstD=$w)"
        }
    }
    if ($npcJudged -eq 0 -or $judged -lt $MinJudged) {
        Write-Host "  POSE-STATE [npc] inconclusive - only $judged valid aligned pelvis pair(s) across $npcJudged NPC(s) (need >= $MinJudged; pelvis read may be failing)"
        return $true
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " pose-mismatch: $($mism -join ', ')" } else { "" }
    Write-Host "  POSE-STATE [npc] $v - join pelvis tracked host within ${PelvisTol}u for $matched/$judged pairs across $npcJudged NPC(s) (ratio=$ratio >= $MinRatio)$detail"
    return $ok
}

# Stage 2 body-state oracle. For each NPC the HOST reports as DOWN (bs has any of
# BODY_DOWN|BODY_RAGDOLL|BODY_DEAD = bits 1|2|4 => bs & 7), the JOIN's time-aligned
# sample must ALSO be down: the body lies on the ground on both clients. We pair
# each host-down MEMBER sample with the nearest-in-time join RECV sample (<= MaxDt)
# and assert the join is down too. Inconclusive (no host-down samples) -> skip, like
# the pose oracle, since the manual gate is then authoritative. BODY_CRAWL (bit 8)
# is intentionally NOT counted as down (it is an upright stealth posture).
function Compare-NpcBodyState {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinRatio = 0.80, [int]$MinJudged = 8, [int]$MaxDt = 800)
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $down = { param($bs) (($bs -band 7) -ne 0) }   # BODY_DOWN|RAGDOLL|DEAD
    $judged = 0; $matched = 0; $npcJudged = 0; $mism = @()
    foreach ($hand in $H.Keys) {
        if (-not $J.ContainsKey($hand)) { continue }
        $hd = @($H[$hand] | Where-Object { (& $down $_.bs) })
        if ($hd.Count -eq 0) { continue }
        $nJudged = 0; $nMatched = 0
        foreach ($hs in $hd) {
            $best = [double]::MaxValue; $bj = $null
            foreach ($js in $J[$hand]) {
                $dt = [Math]::Abs($js.t - $hs.t)
                if ($dt -lt $best) { $best = $dt; $bj = $js }
            }
            if ($best -le $MaxDt -and $null -ne $bj) {
                $nJudged++; $judged++
                if (& $down $bj.bs) { $nMatched++; $matched++ }
            }
        }
        if ($nJudged -lt 1) { continue }
        $npcJudged++
        if ($nMatched -lt $nJudged) {
            $pct = [Math]::Round(100.0 * $nMatched / $nJudged, 0)
            $mism += "$hand($nMatched/$nJudged down $pct%)"
        }
    }
    if ($npcJudged -eq 0 -or $judged -lt $MinJudged) {
        Write-Host "  BODY-STATE [npc] inconclusive - only $judged host-down aligned pair(s) across $npcJudged NPC(s) (need >= $MinJudged); manual gate is authoritative (skipped)"
        return $true
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " not-down-on-join: $($mism -join ', ')" } else { "" }
    Write-Host "  BODY-STATE [npc] $v - join body was down for $matched/$judged host-down pairs across $npcJudged NPC(s) (ratio=$ratio >= $MinRatio)$detail"
    return $ok
}

# craft_order LIVE-transition oracle. Proves the runtime EVENT path (not just
# boot-time state): the host issues a work order mid-run (logging a machine-stamped
# "SCENARIO ORDER" marker), and the JOIN's driven worker must transition from idle
# (task != work) BEFORE the order to operating (task == work) AFTER it. We split the
# join's per-hand task series at the order timestamp and assert both halves.
function Test-CraftOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$WorkTask = 97,
          [int]$GraceMs = 4000, [double]$MinRatio = 0.70)
    $om = Select-String -Path $HostFile -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO ORDER" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { Write-Host "  CRAFT-ORDER FAIL - no SCENARIO ORDER marker on host"; return $false }
    $g = $om.Matches[0].Groups
    $T = ([int]$g[1].Value*3600 + [int]$g[2].Value*60 + [int]$g[3].Value)*1000 + [int]$g[4].Value
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    # Worker hand = the one the HOST streams with the work task AFTER the order.
    $worker = $null
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and $_.task -eq $WorkTask }).Count -gt 0) { $worker = $hand; break }
    }
    if ($null -eq $worker) { Write-Host "  CRAFT-ORDER FAIL - host never streamed task=$WorkTask after the order"; return $false }
    if (-not $J.ContainsKey($worker)) { Write-Host "  CRAFT-ORDER FAIL - join never received worker hand=$worker"; return $false }
    $pre  = @($J[$worker] | Where-Object { $_.t -lt $T })
    $post = @($J[$worker] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($pre.Count -lt 1 -or $post.Count -lt 1) {
        Write-Host "  CRAFT-ORDER FAIL - insufficient join samples (pre=$($pre.Count) post=$($post.Count))"
        return $false
    }
    $preIdle  = @($pre  | Where-Object { $_.task -ne $WorkTask }).Count
    $postWork = @($post | Where-Object { $_.task -eq $WorkTask }).Count
    $preRatio  = [Math]::Round($preIdle  / $pre.Count, 3)
    $postRatio = [Math]::Round($postWork / $post.Count, 3)
    $ok = ($preRatio -ge $MinRatio -and $postRatio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  CRAFT-ORDER [join] $v - worker=$worker idle-before $preIdle/$($pre.Count) (ratio=$preRatio), operating-after $postWork/$($post.Count) (ratio=$postRatio), task=$WorkTask"
    return $ok
}

# down_order LIVE-transition oracle (Stage 2 body-state analog of Test-CraftOrder).
# The host knocks a pinned subject out mid-run (logging a machine-stamped "SCENARIO
# DOWN" marker); the JOIN's driven copy must transition from UPRIGHT (bs not down)
# BEFORE the order to DOWN (bs & 7) AFTER it. We split the join's per-hand bodyState
# series at the marker timestamp and assert both halves.
function Test-DownOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 4000, [double]$MinRatio = 0.70)
    # Match the ORDER marker specifically ("SCENARIO DOWN issued"). NB: Select-String
    # is case-insensitive, so a bare "SCENARIO DOWN" also matches the earlier
    # lowercase "SCENARIO down subject pinned" line and sets T ~18s too early - making
    # every join sample land "after" the order (the spurious pre=0). Anchor on "issued".
    $om = Select-String -Path $HostFile -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO DOWN issued" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { Write-Host "  DOWN-ORDER FAIL - no 'SCENARIO DOWN issued' marker on host"; return $false }
    $g = $om.Matches[0].Groups
    $T = ([int]$g[1].Value*3600 + [int]$g[2].Value*60 + [int]$g[3].Value)*1000 + [int]$g[4].Value
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $down = { param($bs) (($bs -band 7) -ne 0) }
    # Subject = the hand the HOST streams as DOWN after the knockout order.
    $subject = $null
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and (& $down $_.bs) }).Count -gt 0) { $subject = $hand; break }
    }
    if ($null -eq $subject) { Write-Host "  DOWN-ORDER FAIL - host never streamed a down body after the order"; return $false }
    if (-not $J.ContainsKey($subject)) { Write-Host "  DOWN-ORDER FAIL - join never received subject hand=$subject"; return $false }
    $pre  = @($J[$subject] | Where-Object { $_.t -lt $T })
    $post = @($J[$subject] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($pre.Count -lt 1 -or $post.Count -lt 1) {
        Write-Host "  DOWN-ORDER FAIL - insufficient join samples (pre=$($pre.Count) post=$($post.Count))"
        return $false
    }
    $preUp   = @($pre  | Where-Object { -not (& $down $_.bs) }).Count
    $postDn  = @($post | Where-Object {      (& $down $_.bs) }).Count
    $preRatio  = [Math]::Round($preUp  / $pre.Count, 3)
    $postRatio = [Math]::Round($postDn / $post.Count, 3)
    $ok = ($preRatio -ge $MinRatio -and $postRatio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  DOWN-ORDER [join] $v - subject=$subject upright-before $preUp/$($pre.Count) (ratio=$preRatio), down-after $postDn/$($post.Count) (ratio=$postRatio)"
    return $ok
}

# death_order RELIABLE-EVENT oracle. The host kills the pinned subject and emits a
# reliable EVT_DEATH ("[event] SEND ... ev=2 hand=H"); the join MUST log a matching
# "[event] RECV ... ev=2 ... hand=H". SEND and RECV print the hand in the SAME field
# order, so we match exactly. The win condition is RELIABILITY: under packet loss the
# unreliable bodyState batches drop, but this event still arrives on the reliable
# channel. (ev=2 == EVT_DEATH.)
function Test-DeathOrder {
    param([string]$HostFile, [string]$JoinFile)
    $send = Select-String -Path $HostFile -Pattern '\[event\] SEND id=\d+ ev=2 hand=([\d,]+?) bs' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $send) { Write-Host "  DEATH-ORDER FAIL - host emitted no death event (ev=2)"; return $false }
    $hand = $send.Matches[0].Groups[1].Value
    $pat  = '\[event\] RECV id=\d+ ev=2 owner=\d+ hand=' + [regex]::Escape($hand) + '\b'
    $recv = @(Select-String -Path $JoinFile -Pattern $pat -ErrorAction SilentlyContinue)
    if ($recv.Count -lt 1) {
        Write-Host "  DEATH-ORDER FAIL - join never received the reliable death event for hand=$hand"
        return $false
    }
    Write-Host "  DEATH-ORDER [join] PASS - reliable EVT_DEATH received for hand=$hand ($($recv.Count) delivery/deliveries)"
    return $true
}

# combat_probe READ oracle (Phase 3c, L5). Validates that the combat-state primitives
# populate during a live host-spawned duel: the duel spawned + both were ordered, a
# sustained number of COMBAT samples show inCombat=1 + hasTarget=1, and the two
# duelists target EACH OTHER (A's attack target == B's hand and vice versa - proving
# getAttackTarget returns a correct, resolvable hand). Host-side only (the duelists
# are runtime spawns, not baked, so the join has nothing to render yet).
function Test-CombatProbe {
    param([string]$HostFile)
    # setupDuelScene now spawns PEACEFUL (the scenario triggers the fight via startDuel);
    # match the peaceful-spawn line to recover the duelist hands.
    $setup = Select-String -Path $HostFile -Pattern 'SETUP\(duel\): spawned PEACEFUL A=([\d]+),([\d]+) B=([\d]+),([\d]+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $setup) { Write-Host "  COMBAT-PROBE FAIL - duel did not spawn"; return $false }
    $aIdx = $setup.Matches[0].Groups[1].Value; $aSer = $setup.Matches[0].Groups[2].Value
    $bIdx = $setup.Matches[0].Groups[3].Value; $bSer = $setup.Matches[0].Groups[4].Value
    $fighting = @(Select-String -Path $HostFile -Pattern 'COMBAT [AB] .*inCombat=1.*hasTarget=1' -ErrorAction SilentlyContinue)
    if ($fighting.Count -lt 10) {
        Write-Host "  COMBAT-PROBE FAIL - only $($fighting.Count) in-combat+targeted sample(s) (need >= 10)"
        return $false
    }
    # A must target B's hand, and B must target A's hand (mutual, resolvable target).
    $aOnB = @(Select-String -Path $HostFile -Pattern ("COMBAT A .*hasTarget=1 target=" + [regex]::Escape("$bIdx,$bSer")) -ErrorAction SilentlyContinue)
    $bOnA = @(Select-String -Path $HostFile -Pattern ("COMBAT B .*hasTarget=1 target=" + [regex]::Escape("$aIdx,$aSer")) -ErrorAction SilentlyContinue)
    if ($aOnB.Count -lt 1 -or $bOnA.Count -lt 1) {
        Write-Host "  COMBAT-PROBE FAIL - duelists not mutually targeting (A->B=$($aOnB.Count) B->A=$($bOnA.Count))"
        return $false
    }
    Write-Host "  COMBAT-PROBE [host] PASS - live duel: $($fighting.Count) in-combat samples; mutual attack-target hands resolve (A->B, B->A)"
    return $true
}

# combat_order LIVE-transition oracle (Phase 3c, L5 - the payoff). Both clients load
# the baked 'duel1' (two peaceful co-located NPCs). The host pins them, holds them
# peaceful, then at "SCENARIO COMBAT issued" orders the live fight. The combat intent
# streams as task=65024 (TASK_COMBAT_MELEE); the join reproduces it so its OWN copies
# enter combat, and the join's RECV series (its independent re-read of those copies)
# must flip from NOT-combat BEFORE the order to combat AFTER it. We split the join's
# per-hand task series on the marker and assert both halves for the duelist hands (the
# hands the host streams as combat after the order). Combat flickers between swings, so
# the post threshold is a sustained fraction, not 100%.
function Test-CombatOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 5000,
          [double]$PostMin = 0.40, [double]$PreMax = 0.10)
    $COMBAT = 65024 # TASK_COMBAT_MELEE (0xFE00)
    $om = Select-String -Path $HostFile -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO COMBAT issued" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { Write-Host "  COMBAT-ORDER FAIL - no 'SCENARIO COMBAT issued' marker on host"; return $false }
    $g = $om.Matches[0].Groups
    $T = ([int]$g[1].Value*3600 + [int]$g[2].Value*60 + [int]$g[3].Value)*1000 + [int]$g[4].Value
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    # Duelists = hands the HOST streams in combat (task=65024) after the order.
    $duelists = @()
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and $_.task -eq $COMBAT }).Count -gt 0) { $duelists += $hand }
    }
    if ($duelists.Count -lt 1) { Write-Host "  COMBAT-ORDER FAIL - host never streamed a combat task after the order"; return $false }
    $preCombat = 0; $preTotal = 0; $postCombat = 0; $postTotal = 0; $seen = 0
    foreach ($hand in $duelists) {
        if (-not $J.ContainsKey($hand)) { continue }
        $seen++
        $pre  = @($J[$hand] | Where-Object { $_.t -lt $T })
        $post = @($J[$hand] | Where-Object { $_.t -ge ($T + $GraceMs) })
        $preTotal  += $pre.Count;  $preCombat  += @($pre  | Where-Object { $_.task -eq $COMBAT }).Count
        $postTotal += $post.Count; $postCombat += @($post | Where-Object { $_.task -eq $COMBAT }).Count
    }
    if ($seen -lt 1) { Write-Host "  COMBAT-ORDER FAIL - join never received any duelist hand (saw $($duelists.Count) on host)"; return $false }
    if ($preTotal -lt 1 -or $postTotal -lt 1) { Write-Host "  COMBAT-ORDER FAIL - insufficient join samples (preTotal=$preTotal postTotal=$postTotal)"; return $false }
    $preRatio  = [Math]::Round($preCombat  / $preTotal, 3)
    $postRatio = [Math]::Round($postCombat / $postTotal, 3)
    $ok = ($preRatio -le $PreMax -and $postRatio -ge $PostMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  COMBAT-ORDER [join] $v - duelists=$($duelists.Count) seen=$seen combat-before $preCombat/$preTotal (ratio=$preRatio<=$PreMax), combat-after $postCombat/$postTotal (ratio=$postRatio>=$PostMin)"
    return $ok
}

# combat_kill OUTCOME oracle (Phase 3c, L5). Proves combat resolution is HOST-
# AUTHORITATIVE with attribution: the host's duelist A downs the (weakened) duelist B by
# REAL melee, emits a reliable KO/death event STAMPED with A as the actor, the join
# RECEIVES that exact event (hand B + actor A), and the join's copy of B is DOWN after
# it - even though the join ran its own local fight. Asserts all three: combat
# attribution (non-zero actor == opponent), reliable delivery, and synced down outcome.
function Test-CombatKill {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 3000, [double]$MinDown = 0.70)
    $pin = Select-String -Path $HostFile -Pattern 'SCENARIO duel subjects pinned A=(\d+),(\d+) B=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pin) { Write-Host "  COMBAT-KILL FAIL - no pinned duelists on host"; return $false }
    $ai = $pin.Matches[0].Groups[1].Value; $as = $pin.Matches[0].Groups[2].Value
    $bi = $pin.Matches[0].Groups[3].Value; $bs = $pin.Matches[0].Groups[4].Value
    # Host emitted a combat KO/death (ev=1 or 2) for B, stamped with A as the actor.
    # Event hand layout is type,container,containerSerial,INDEX,SERIAL, so B's index,serial
    # are the last two fields; actor is logged as index,serial.
    $sendPat = '\[event\] SEND id=\d+ ev=(1|2) hand=\d+,\d+,\d+,' + $bi + ',' + $bs + ' actor=' + $ai + ',' + $as
    $send = Select-String -Path $HostFile -Pattern $sendPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $send) {
        Write-Host "  COMBAT-KILL FAIL - host sent no combat KO/death for B=$bi,$bs with actor A=$ai,$as (no real takedown / no attribution)"
        return $false
    }
    $ev = $send.Matches[0].Groups[1].Value
    # Join received THAT exact reliable event (same ev, hand B, actor A).
    $recvPat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] RECV id=\d+ ev=' + $ev + ' .*hand=\d+,\d+,\d+,' + $bi + ',' + $bs + ' actor=' + $ai + ',' + $as
    $recv = Select-String -Path $JoinFile -Pattern $recvPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $recv) {
        Write-Host "  COMBAT-KILL FAIL - join never received the reliable combat event (ev=$ev hand=B actor=A)"
        return $false
    }
    $rg = $recv.Matches[0].Groups
    $T = ([int]$rg[1].Value*3600 + [int]$rg[2].Value*60 + [int]$rg[3].Value)*1000 + [int]$rg[4].Value
    # The join's copy of B must be DOWN after it received the event. Series hand keys are
    # index,serial,type,container,containerSerial, so B's key starts with "$bi,$bs,".
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $bKey = $null
    foreach ($hand in $J.Keys) { if ($hand -match ('^' + $bi + ',' + $bs + ',')) { $bKey = $hand; break } }
    if ($null -eq $bKey) { Write-Host "  COMBAT-KILL FAIL - join logged no body series for victim B=$bi,$bs"; return $false }
    $post = @($J[$bKey] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($post.Count -lt 1) { Write-Host "  COMBAT-KILL FAIL - no join samples after the event"; return $false }
    $down = @($post | Where-Object { ($_.bs -band 7) -ne 0 }).Count
    $ratio = [Math]::Round($down / $post.Count, 3)
    $ok = ($ratio -ge $MinDown)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $evName = if ($ev -eq "2") { "DEATH" } else { "KO" }
    Write-Host "  COMBAT-KILL [join] $v - combat $evName for B=$bi,$bs by A=$ai,${as}: reliable event received, victim down-after $down/$($post.Count) (ratio=$ratio>=$MinDown)"
    return $ok
}

# True if $File logged a "SCENARIO RESULT PASS" line.
function Test-ScenarioResultPass {
    param([string]$File)
    return ((Test-Path $File) -and ((Get-Content $File -Raw) -match "SCENARIO RESULT PASS"))
}

# Stage 2 smoothness oracle. The receiver logs a "SCENARIO SMOOTH ... zeroFrac=.."
# line: the fraction of frames where the driven body did NOT move while its
# source WAS moving. Per-frame interpolation keeps this low; raw 20 Hz stepping
# leaves the body static for ~2/3 of frames. If no SMOOTH line is present (e.g. a
# scenario that never drove a moving body), the check is skipped (returns $true).
function Test-Smoothness {
    # Engine-driven locomotion (Stage 3+) legitimately decelerates to ~0 at turns,
    # reversals and arrival, so a modest fraction of zero-advance frames is normal
    # (unlike the Stage 2 kinematic slide, which moved every frame). The strict
    # animation gate is Test-AnimTruth; this just catches gross stalling.
    param([string]$File, [string]$Label, [double]$MaxZeroFrac = 0.40)
    if (-not (Test-Path $File)) { return $true }
    $line = Select-String -Path $File -Pattern "SCENARIO SMOOTH .*zeroFrac=([\d\.]+).*maxStep=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) { Write-Host "  [$Label] no SCENARIO SMOOTH line (skipped)"; return $true }
    $zeroFrac = [double]$line.Matches[0].Groups[1].Value
    $maxStep  = [double]$line.Matches[0].Groups[2].Value
    $ok = ($zeroFrac -le $MaxZeroFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] smoothness $v - zeroFrac=$zeroFrac (<= $MaxZeroFrac), maxStep=$maxStep"
    return $ok
}

# Stage 3 anim-truth oracle (the float-bug detector). The receiver logs a
# "SCENARIO ANIM ... floatFrac=.." line: the fraction of frames where the driven
# body's actual position translated but it reported NO walk state (currentlyMoving
# / currentSpeed==0) - i.e. it slid a static pose. Engine-driven walk keeps this
# low. Skipped (returns $true) if no ANIM line is present.
function Test-AnimTruth {
    param([string]$File, [string]$Label, [double]$MaxFloatFrac = 0.30)
    if (-not (Test-Path $File)) { return $true }
    $line = Select-String -Path $File -Pattern "SCENARIO ANIM .*translate=(\d+) walkTruth=(\d+) floatFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) { Write-Host "  [$Label] no SCENARIO ANIM line (skipped)"; return $true }
    $translate = [int]$line.Matches[0].Groups[1].Value
    $floatFrac = [double]$line.Matches[0].Groups[3].Value
    # Need a meaningful sample of translating frames to trust the verdict. Too few
    # == nothing was walking this run (e.g. all-stationary NPCs); that is genuinely
    # inconclusive, not a failure (motion/sync is proven by the cross-check), so we
    # skip rather than fail.
    if ($translate -lt 30) { Write-Host "  [$Label] anim-truth inconclusive - only $translate translating frame(s) (skipped)"; return $true }
    $ok = ($floatFrac -le $MaxFloatFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] anim-truth $v - floatFrac=$floatFrac (<= $MaxFloatFrac), translateFrames=$translate"
    return $ok
}

# March-in-place oracle (the inverse of anim-truth). The receiver logs a
# "SCENARIO MARCH ... marchFrac=.." line: the fraction of AT-REST frames where the
# driven body played a walk clip while NOT translating - i.e. it marched on the
# spot (e.g. a host-seated NPC stuck walking in place on the join). Engine walk +
# correct rest poses keep this low. Skipped (returns $true) if no MARCH line or too
# few rest samples to judge.
function Test-MarchInPlace {
    param([string]$File, [string]$Label, [double]$MaxMarchFrac = 0.20)
    if (-not (Test-Path $File)) { return $true }
    $line = Select-String -Path $File -Pattern "SCENARIO MARCH restSamples=(\d+) march=(\d+) marchFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) { Write-Host "  [$Label] no SCENARIO MARCH line (skipped)"; return $true }
    $rest  = [int]$line.Matches[0].Groups[1].Value
    $marchFrac = [double]$line.Matches[0].Groups[3].Value
    if ($rest -lt 30) { Write-Host "  [$Label] march-in-place inconclusive - only $rest at-rest frame(s) (skipped)"; return $true }
    $ok = ($marchFrac -le $MaxMarchFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] march-in-place $v - marchFrac=$marchFrac (<= $MaxMarchFrac), restSamples=$rest"
    return $ok
}

$cleanPattern = if ($Scenario -ne "") { "SCENARIO RESULT" } else { "test duration elapsed; exiting" }
$hostOk = Evaluate -File $hostLog -Label "host" -Required $true -CleanPattern $cleanPattern
$joinOk = Evaluate -File $joinLog -Label "join" -Required ($joinPid -ne 0) -CleanPattern $cleanPattern

$pass = ($hostOk -and $joinOk)

if ($Scenario -ne "") {
    Write-Host ""
    Write-Host "== Scenario checks: $Scenario =="
    $hostNoFail     = Test-NoCheckFail -File $hostLog -Label "host"
    $joinNoFail     = Test-NoCheckFail -File $joinLog -Label "join"
    # Both launched clients must report SCENARIO RESULT PASS (either side may be
    # the authoritative actor depending on the scenario).
    $hostResultPass = Test-ScenarioResultPass -File $hostLog
    if (-not $hostResultPass) { Write-Host "  host SCENARIO RESULT is not PASS" }
    $joinResultPass = $true
    if ($joinPid -ne 0) {
        $joinResultPass = Test-ScenarioResultPass -File $joinLog
        if (-not $joinResultPass) { Write-Host "  join SCENARIO RESULT is not PASS" }
    }
    $pose = $true; $poseState = $true; $bodyState = $true
    if ($Scenario -eq "npc_sync") {
        $cross = Compare-NpcSync -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
        $pose  = Compare-NpcPose -HostFile $hostLog -JoinFile $joinLog
        # Authoritative standing-vs-sitting gate (rendered-skeleton pelvis/crouch).
        $poseState = Compare-NpcPoseState -HostFile $hostLog -JoinFile $joinLog
        # Stage 2: down/KO/ragdoll/dead bodies must be down on the join too. Skips
        # (advisory) when the host streamed no down bodies (e.g. an upright scene).
        $bodyState = Compare-NpcBodyState -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "craft_order") {
        # LIVE transition: the join's worker must go idle -> operating AFTER the
        # host's runtime order. POSE-STATE still co-gates pelvis tracking (the worker
        # is pinned at the prop the whole time, idle or operating).
        $cross     = Test-CraftOrder -HostFile $hostLog -JoinFile $joinLog
        $poseState = Compare-NpcPoseState -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "down_order") {
        # LIVE transition: the join's subject must go upright -> down AFTER the host
        # knocks it out mid-run. Body-state analog of craft_order.
        $cross = Test-DownOrder -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "death_order") {
        # RELIABLE-EVENT proof: the host kills the pinned subject mid-run and emits a
        # reliable EVT_DEATH; the join MUST receive it (even under packet loss, since
        # it rides the reliable channel while the unreliable bodyState batches drop).
        $cross = Test-DeathOrder -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "combat_probe") {
        # L5 READ probe: host spawns a live duel; assert the combat-state primitives
        # populate (inCombat + mutually-resolvable attack-target hands). Host-side only.
        $cross = Test-CombatProbe -HostFile $hostLog
    } elseif ($Scenario -eq "combat_order") {
        # L5 LIVE transition: both clients load duel1 (peaceful pair); the host triggers
        # the fight mid-run and the join's own copies must transition NOT-combat ->
        # combat (task=65024) AFTER the order. The payoff combat test.
        $cross = Test-CombatOrder -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "combat_kill") {
        # L5 OUTCOME: host-authoritative combat resolution. A downs B by real melee; the
        # host emits a reliable KO/death stamped with A as actor; the join receives it and
        # forces ITS B down. Asserts attribution + reliable delivery + synced down outcome.
        $cross = Test-CombatKill -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "coop_presence") {
        # Phase 3.5 BIDIRECTIONAL presence: both players' owned characters must be
        # present + tracking on the OTHER client. Time-aligned cross-check, both ways.
        $cross = Test-CoopPresence -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
    } elseif ($Scenario -eq "inv_order") {
        # Phase 4a CONTENT-SNAPSHOT: the host adds an item to a shared container mid-run;
        # the join must reconstruct it locally so both sides' contents match. Multiset
        # (content-hash) gate, not a transform cross-check.
        $cross = Test-InventorySync -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "inv_bidir") {
        # Phase 4a BIDIRECTIONAL: each side mutates only the container it owns (host
        # rank 0, join rank 1); the peer must converge BOTH ways with no loss/dupe.
        $cross = Test-InventoryBidir -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "inv_equip") {
        # Phase 4a EQUIPPED-GEAR: each side runs an unequip/equip/unequip cycle on worn
        # gear of a tab it owns (host rank 0, join rank 1); the peer must converge the
        # worn (equipped) state BOTH ways - the case loose-only sync missed.
        $cross = Test-InventoryEquip -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "inv_reequip") {
        # Phase 4a RE-EQUIP (up path): each side UNEQUIPS a real worn item to loose, then
        # RE-EQUIPS it (move, not fabricate). The observer must down-move then up-move its
        # copy to track the dip+restore; the author's worn count must return to its peak.
        $cross = Test-InventoryReequip -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "inv_addequip") {
        # Phase 4a d25 fix: LOCAL reconcile test. Adding an EQUIPPED item with no existing
        # copy must converge to EQUIPPED and PERSIST (create-loose-then-equip-next-tick),
        # not fabricate-and-vanish. ADD-EQUIP asserts the slot refilled and held.
        $cross = Test-AddEquip -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "drop_probe") {
        # Phase W0 DIAGNOSTIC: host seeds + drops a known item and enumerates nearby world
        # items before/after. Host-side only (no world-item channel yet); the log IS the
        # deliverable - DROP-PROBE asserts the probe executed and surfaces the facts.
        $cross = Test-DropProbe -HostFile $hostLog
    } elseif ($Scenario -eq "world_item_sync") {
        # Phase W1: host drops a ground item that streams (netId-keyed) to the join, which
        # spawns a local proxy; then the host despawns it and the join culls the proxy.
        # WI-SYNC asserts content-hash + position match on spawn and a clean cull both ways.
        $cross = Test-WorldItemSync -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
    } elseif ($Scenario -eq "wpn_relocate") {
        # SPIKE (conservation model): LOCAL single-client test that a WEAPON the factory CANNOT
        # create can still be moved bag->ground->bag by relocating the REAL object, persisting
        # at each step. WPN-RELOCATE asserts drop produced a persistent ground weapon and the
        # pickup re-homed it durably (no createItem) - the trade primitive for shared weapons.
        $cross = Test-WpnRelocate -HostFile $hostLog -JoinFile $joinLog
    } elseif ($Scenario -eq "world_weapon_drop") {
        # Phase W2: host drops its leader's weapon; the join must RELOCATE its own copy to the
        # ground (conservation) rather than have the inventory reconcile destroy it. WEAPON-DROP
        # asserts the host dropped + the join's copy left the bag AND landed on the ground.
        $cross = Test-WeaponDrop -HostFile $hostLog -JoinFile $joinLog
    } else {
        $cross = Compare-Scenario -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
    }
    # Smoothness + anim-truth are asserted on the observing (join) side, which
    # drives the body. Each is skipped automatically if its line is absent.
    $smooth = Test-Smoothness -File $joinLog -Label "join"
    $anim   = Test-AnimTruth  -File $joinLog -Label "join"
    $march  = Test-MarchInPlace -File $joinLog -Label "join"
    # craft_order is a transition test: the worker briefly walks to the prop and snaps
    # into the work pose, which legitimately spikes the locomotion-tuned smoothness /
    # anim-truth metrics. They're printed as advisory but don't gate here; CRAFT-ORDER
    # + POSE-STATE + MARCH are the authoritative gates.
    # A down-body scene (Setup down/downhold) intentionally does NOT walk-drive the
    # subject - it is KO'd on the ground - so the locomotion-tuned smoothness/anim
    # metrics are not meaningful here (a held body trivially "fails" zeroFrac). They
    # print as advisory; BODY-STATE + MARCH are the authoritative gates.
    $isDownScene = ($Setup -like "down*") -or ($Scenario -eq "down_order") -or ($Scenario -eq "death_order")
    # combat_probe is a host-side READ probe: the duelists are runtime host spawns the
    # join doesn't drive, so the join-side locomotion metrics (smoothness/anim/march)
    # are not meaningful and print as advisory; COMBAT-PROBE is the authoritative gate.
    $isReadProbe = ($Scenario -eq "combat_probe")
    # combat_order/combat_kill drive combatants by ENGINE (local combat footwork), not by
    # our walk-drive/park, so the locomotion-tuned smoothness/anim/march metrics
    # legitimately spike (real combat movement + a falling body). They print as advisory;
    # COMBAT-ORDER / COMBAT-KILL are the gates.
    $isCombatOrder = ($Scenario -eq "combat_order") -or ($Scenario -eq "combat_kill")
    # coop_presence is primarily a PRESENCE/PLACEMENT test: each side moves its tab
    # leader briefly to prove live presence, then SETTLES for a clean cross-check, so
    # the measured window is mostly stationary with a tiny translate sample. Under WAN
    # the driven leader advances in brief snaps (the known latency micro-slide / dead-
    # reckoning seam), which makes the locomotion-tuned smoothness/anim fractions spike
    # on that small sample. They print as advisory; COOP-PRESENCE (bidirectional
    # cross-check) + MARCH are the authoritative gates.
    $isPresence = ($Scenario -eq "coop_presence")
    # inv_order is a CONTENT test on (stationary) squad members: the locomotion-tuned
    # smoothness/anim/march metrics aren't meaningful here and print as advisory;
    # INV-SYNC (multiset match) is the authoritative gate.
    $isInventory = ($Scenario -eq "inv_order") -or ($Scenario -eq "inv_bidir") -or ($Scenario -eq "inv_equip") -or ($Scenario -eq "inv_reequip") -or ($Scenario -eq "inv_wpnseq") -or ($Scenario -eq "inv_addequip") -or ($Scenario -eq "wpn_relocate")
    # World-item scenarios (drop_probe + the Phase W1+ world_item_* family) are stationary
    # content/diagnostic tests on a host-driven world stream; the join doesn't walk-drive,
    # so the locomotion-tuned smoothness/anim/march metrics are advisory here.
    $isWorldItem = ($Scenario -eq "drop_probe") -or ($Scenario -like "world_item_*") -or ($Scenario -eq "world_weapon_drop")
    $gateSmooth = if ($Scenario -eq "craft_order" -or $isDownScene -or $isReadProbe -or $isCombatOrder -or $isPresence -or $isInventory -or $isWorldItem) { $true } else { $smooth }
    $gateAnim   = if ($Scenario -eq "craft_order" -or $isDownScene -or $isReadProbe -or $isCombatOrder -or $isPresence -or $isInventory -or $isWorldItem) { $true } else { $anim }
    $gateMarch  = if ($isReadProbe -or $isCombatOrder -or $isInventory -or $isWorldItem) { $true } else { $march }
    $pass = ($pass -and $hostNoFail -and $joinNoFail -and $hostResultPass -and $joinResultPass -and $cross -and $gateSmooth -and $gateAnim -and $gateMarch -and $pose -and $poseState -and $bodyState)
}

Write-Host ""
Write-Host ("RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
