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
    [double]$Tolerance = 3.0,
    [int]$ScenarioShotDelaySec = 5,
    # How long to wait (from the host capture) for the later-loading join to log
    # its first SCENARIO RECV, so the join screenshot is captured in-game and
    # mid-action rather than on a loading screen.
    [int]$JoinAnchorTimeoutSec = 45,
    # Backstop cap (seconds, measured from host gameplay) for the scenario
    # pre-screenshot wait. The wait normally ends much sooner via early-out (a
    # SCENARIO MEMBER line to anchor the shot, or SCENARIO RESULT / both clients
    # exited when a scenario finishes or fails fast). Kept well under the old
    # 90 s so a fast FAIL no longer stalls the run.
    [int]$ScenarioWaitSec = 25
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
    if (Wait-ForLogLine -File $hostLog -Pattern "SCENARIO MEMBER" -TimeoutSec $ScenarioWaitSec) {
        Write-Host "Saw host SCENARIO MEMBER; capturing host shortly after."
        Start-Sleep -Seconds 1
    } else {
        Write-Warning "No host SCENARIO MEMBER within $ScenarioWaitSec s; capturing host best-effort."
    }
    if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
    else { Write-Warning "Host already exited before screenshot." }

    if ($joinPid -ne 0) {
        if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec $JoinAnchorTimeoutSec) {
            Write-Host "Saw join SCENARIO RECV; capturing join shortly after."
            Start-Sleep -Seconds 1
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
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = ([int]$g[1].Value*3600 + [int]$g[2].Value*60 + [int]$g[3].Value)*1000 + [int]$g[4].Value
        $hand = $g[5].Value
        $pos  = $g[6].Value.Split(',') | ForEach-Object { [double]$_ }
        if (-not $map.ContainsKey($hand)) { $map[$hand] = New-Object System.Collections.ArrayList }
        [void]$map[$hand].Add(@{ t = $t; p = $pos })
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
        Write-Host "  CROSSCHECK [npc host->join] FAIL - only $($judged.Count) continuously-present NPC(s) (need >= $MinJudged; maxAligned=$maxAligned)"
        return $false
    }
    $tracked = @($judged | Where-Object { $_.med -le $Tol }).Count
    $worst = ($judged | ForEach-Object { $_.med } | Measure-Object -Maximum).Maximum
    $ratio = [Math]::Round($tracked / $judged.Count, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  CROSSCHECK [npc host->join] $v - tracked $tracked/$($judged.Count) continuously-present NPCs within $Tol (ratio=$ratio >= $MinRatio, worstMedian=$([Math]::Round($worst,1)), excluded $($rows.Count - $judged.Count) boundary)"
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
    if ($Scenario -eq "npc_sync") {
        $cross = Compare-NpcSync -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
    } else {
        $cross = Compare-Scenario -HostFile $hostLog -JoinFile $joinLog -Tol $Tolerance
    }
    # Smoothness + anim-truth are asserted on the observing (join) side, which
    # drives the body. Each is skipped automatically if its line is absent.
    $smooth = Test-Smoothness -File $joinLog -Label "join"
    $anim   = Test-AnimTruth  -File $joinLog -Label "join"
    $pass = ($pass -and $hostNoFail -and $joinNoFail -and $hostResultPass -and $joinResultPass -and $cross -and $smooth -and $anim)
}

Write-Host ""
Write-Host ("RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
