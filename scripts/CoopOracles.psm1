# CoopOracles.psm1 - the KenshiCoop validation oracle library.
#
# Every oracle that judges a host/join log pair lives here (ported from the old
# inline run_test.ps1 implementations) so the SAME verdict code runs in three
# contexts:
#   * live loopback runs  (run_test.ps1 calls Invoke-RunAnalysis at the end)
#   * regression tiers    (regress.ps1 consumes the per-run verdict.json)
#   * remote sessions     (analyze_run.ps1 judges logs collected from a friend)
#
# Three-state verdicts: each oracle returns 'PASS' | 'FAIL' | 'SKIP' and records
# a gate entry (name, status, metrics, detail) via Add-GateResult. 'SKIP' means
# "could not judge" (missing instrumentation / too small a sample) and is
# surfaced - never silently converted to a pass. A scenario's PRIMARY gate
# (declared in scenarios.psd1) failing OR skipping fails the whole run: a green
# run must PROVE its mechanism was judged (the no-signal guard).
#
# Clock alignment: Get-ScenarioSeries and every marker-timestamp parse apply the
# per-log CLOCKSYNC offset (logged by the plugin's wire time-sync), so
# time-aligned oracles work when host and join run on machines whose wall
# clocks disagree. Logs without CLOCKSYNC lines get offset 0 (the legacy
# same-machine behaviour).

# ---- Gate result infrastructure ---------------------------------------------

$script:Gates = New-Object System.Collections.ArrayList
$script:ClockOffsetCache = @{}

function Reset-GateResults {
    $script:Gates = New-Object System.Collections.ArrayList
    $script:ClockOffsetCache = @{}
}

# Record one gate verdict. Status: PASS | FAIL | SKIP. Metrics is a hashtable of
# measured values (persisted into verdict.json for trending).
function Add-GateResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet("PASS", "FAIL", "SKIP")][string]$Status,
        [hashtable]$Metrics = @{},
        [string]$Detail = ""
    )
    [void]$script:Gates.Add([pscustomobject]@{
        gate    = $Name
        status  = $Status
        metrics = $Metrics
        detail  = $Detail
    })
    return $Status
}

function Get-GateResults { return @($script:Gates) }

# Merge two direction/sub-check statuses: FAIL dominates, then SKIP, then PASS.
function Merge-Status {
    param([string[]]$Statuses)
    if ($Statuses -contains "FAIL") { return "FAIL" }
    if ($Statuses -contains "SKIP") { return "SKIP" }
    return "PASS"
}

# ---- Clock-offset helpers -----------------------------------------------------

# Per-log wall-clock offset (ms) relative to the HOST clock, estimated by the
# plugin's wire time-sync and logged as "CLOCKSYNC offset=<ms> rtt=<ms>". We take
# the minimum-RTT sample (the classic NTP filter: least queueing noise). A log
# with no CLOCKSYNC lines (host, or a pre-time-sync DLL) gets offset 0.
function Get-LogClockOffsetMs {
    param([string]$File)
    if (-not (Test-Path $File)) { return 0 }
    if ($script:ClockOffsetCache.ContainsKey($File)) { return $script:ClockOffsetCache[$File] }
    $best = $null; $bestRtt = [int]::MaxValue
    foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
        $off = [int]$m.Matches[0].Groups[1].Value
        $rtt = [int]$m.Matches[0].Groups[2].Value
        if ($rtt -le $bestRtt) { $bestRtt = $rtt; $best = $off }
    }
    $result = if ($null -ne $best) { $best } else { 0 }
    $script:ClockOffsetCache[$File] = $result
    return $result
}

# Count of CLOCKSYNC samples + the min-RTT sample, for the clock_sync gate.
function Get-ClockSyncStats {
    param([string]$File)
    $n = 0; $bestOff = $null; $bestRtt = [int]::MaxValue
    if (Test-Path $File) {
        foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
            $n++
            $off = [int]$m.Matches[0].Groups[1].Value
            $rtt = [int]$m.Matches[0].Groups[2].Value
            if ($rtt -le $bestRtt) { $bestRtt = $rtt; $bestOff = $off }
        }
    }
    return [pscustomobject]@{ samples = $n; offsetMs = $bestOff; rttMs = $(if ($n -gt 0) { $bestRtt } else { $null }) }
}

# Parse a "[HH:MM:SS.mmm]" stamp match-group set into ms-since-midnight, applying
# the log's clock offset so all returned times are in the HOST clock frame.
function Convert-StampToMs {
    param($Groups, [int]$OffsetMs)
    $t = ([int]$Groups[1].Value * 3600 + [int]$Groups[2].Value * 60 + [int]$Groups[3].Value) * 1000 + [int]$Groups[4].Value
    return $t + $OffsetMs
}

# ---- Log parsing helpers ------------------------------------------------------

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

# Parse timestamped SCENARIO <Kind> lines into hand -> list of @{t=ms; p=@(x,y,z)}.
# Timestamps are converted into the HOST clock frame via the log's CLOCKSYNC
# offset (0 when absent), so host and join series are directly time-comparable
# even across machines with disagreeing wall clocks.
function Get-ScenarioSeries {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)(?: task=(\d+))?(?: pelvis=(-?[\d\.]+))?(?: crouch=(-?\d+))?(?: idle=(-?\d+))?(?: bs=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
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

# Find the first timestamped marker line matching $Pattern in $File and return its
# ms-since-midnight in the HOST clock frame, or $null.
function Get-MarkerTimeMs {
    param([string]$File, [string]$Pattern)
    $om = Select-String -Path $File -Pattern ("\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*" + $Pattern) -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { return $null }
    $off = Get-LogClockOffsetMs -File $File
    return (Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs $off)
}

# ---- Log health / scenario-result gates ---------------------------------------

# Evaluate one client's log health. PASS if it reached gameplay, exited cleanly
# (did NOT crash mid-run), and logged no ERROR lines. The clean-exit marker
# differs by mode: the self-exit timer logs "test duration elapsed", while a
# scenario logs "SCENARIO RESULT".
function Test-LogHealth {
    param([string]$File, [string]$Label, [bool]$Required, [string]$CleanPattern)
    $gate = "health_$Label"
    if (-not (Test-Path $File)) {
        if ($Required) {
            Write-Host "  [$Label] FAIL - no log produced"
            return (Add-GateResult -Name $gate -Status FAIL -Detail "no log produced")
        }
        Write-Host "  [$Label] skipped (not launched)"
        return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched")
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
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ errors = $errs.Count } -Detail ($why -join "; "))
}

# Report any in-plugin "CHECK <key> FAIL" lines across both logs.
function Test-NoCheckFail {
    param([string]$HostFile, [string]$JoinFile)
    $count = 0
    foreach ($entry in @(@{ f = $HostFile; l = "host" }, @{ f = $JoinFile; l = "join" })) {
        if ($entry.f -eq "" -or -not (Test-Path $entry.f)) { continue }
        $fails = @(Select-String -Path $entry.f -Pattern "CHECK \S+ FAIL" -ErrorAction SilentlyContinue)
        foreach ($f in $fails) { Write-Host "  [$($entry.l)] $($f.Line.Trim())" }
        $count += $fails.Count
    }
    return (Add-GateResult -Name "check_fail" -Status $(if ($count -eq 0) { "PASS" } else { "FAIL" }) `
                -Metrics @{ failLines = $count })
}

# The in-plugin scenario verdict ("SCENARIO RESULT PASS") for one client.
function Test-ScenarioResultPass {
    param([string]$File, [string]$Label, [bool]$Required)
    $gate = "result_$Label"
    if ($File -eq "" -or -not (Test-Path $File)) {
        if (-not $Required) { return (Add-GateResult -Name $gate -Status SKIP -Detail "not launched") }
        Write-Host "  $Label SCENARIO RESULT missing (no log)"
        return (Add-GateResult -Name $gate -Status FAIL -Detail "no log")
    }
    $ok = ((Get-Content $File -Raw) -match "SCENARIO RESULT PASS")
    if (-not $ok) { Write-Host "  $Label SCENARIO RESULT is not PASS" }
    return (Add-GateResult -Name $gate -Status $(if ($ok) { "PASS" } else { "FAIL" }))
}

# ---- Clock-sync gate ------------------------------------------------------------

# Validates the wire time-sync itself. SKIP when the join log carries no CLOCKSYNC
# lines (pre-time-sync DLL). When -ExpectedSkewMs is provided (a run with injected
# fake clock skew), the estimated offset must recover the injected skew: the join's
# wall clock was shifted +S, so its estimated host-relative offset must be ~ -S.
function Test-ClockSync {
    param([string]$HostFile, [string]$JoinFile, $ExpectedSkewMs = $null, [int]$MinSamples = 3)
    $s = Get-ClockSyncStats -File $JoinFile
    if ($s.samples -eq 0) {
        Write-Host "  CLOCK-SYNC SKIP - no CLOCKSYNC lines in the join log (plugin without time-sync?)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Detail "no CLOCKSYNC lines")
    }
    $m = @{ samples = $s.samples; offsetMs = $s.offsetMs; rttMs = $s.rttMs }
    if ($s.samples -lt $MinSamples) {
        Write-Host "  CLOCK-SYNC SKIP - only $($s.samples) CLOCKSYNC sample(s) (need >= $MinSamples)"
        return (Add-GateResult -Name "clock_sync" -Status SKIP -Metrics $m -Detail "too few samples")
    }
    if ($null -ne $ExpectedSkewMs -and "$ExpectedSkewMs" -ne "") {
        $exp = [int]$ExpectedSkewMs
        $tol = [Math]::Max(100, 2 * $s.rttMs)
        $err = [Math]::Abs($s.offsetMs + $exp)
        $m.expectedSkewMs = $exp; $m.recoverErrMs = $err; $m.tolMs = $tol
        $ok = ($err -le $tol)
        $v = if ($ok) { "PASS" } else { "FAIL" }
        Write-Host "  CLOCK-SYNC $v - injected skew ${exp}ms, estimated offset $($s.offsetMs)ms (|err|=${err}ms <= ${tol}ms, rtt=$($s.rttMs)ms, n=$($s.samples))"
        return (Add-GateResult -Name "clock_sync" -Status $v -Metrics $m)
    }
    Write-Host "  CLOCK-SYNC PASS - offset=$($s.offsetMs)ms rtt=$($s.rttMs)ms n=$($s.samples)"
    return (Add-GateResult -Name "clock_sync" -Status PASS -Metrics $m)
}

# ---- Cross-check oracles --------------------------------------------------------

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

# Generic cross-client position check (squad_spawn_sync / squad_move_sync style).
function Test-Crosscheck {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $hostMembers = Get-ScenarioLines -File $HostFile -Kind "MEMBER"
    $joinMembers = Get-ScenarioLines -File $JoinFile -Kind "MEMBER"
    $hostRecv    = Get-ScenarioLines -File $HostFile -Kind "RECV"
    $joinRecv    = Get-ScenarioLines -File $JoinFile -Kind "RECV"

    if ($hostMembers.Count -eq 0 -and $joinMembers.Count -eq 0) {
        Write-Host "  CROSSCHECK squad_positions_match FAIL - no SCENARIO MEMBER lines on either side"
        return (Add-GateResult -Name "crosscheck" -Status FAIL -Detail "no MEMBER lines on either side")
    }
    $ok = $true
    if ($hostMembers.Count -gt 0) {
        $ok = (Compare-MemberRecv -Members $hostMembers -Recv $joinRecv -Tol $Tol -Dir "host->join") -and $ok
    }
    if ($joinMembers.Count -gt 0) {
        $ok = (Compare-MemberRecv -Members $joinMembers -Recv $hostRecv -Tol $Tol -Dir "join->host") -and $ok
    }
    return (Add-GateResult -Name "crosscheck" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostMembers = $hostMembers.Count; joinMembers = $joinMembers.Count })
}

# NPC sync cross-check, TIME-ALIGNED. For each NPC, pair every join RECV sample
# with the host MEMBER sample nearest in time (<= MaxDt) and take the MEDIAN
# distance. PASS if a healthy majority of the continuously-co-present NPCs
# tracked within $Tol. Returns a rich object; the caller records the gate (this
# is reused by npc_sync and per-direction by coop_presence).
function Measure-NpcSync {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol,
          [double]$MinRatio = 0.80, [int]$MinJudged = 4, [int]$MaxDt = 800,
          [string]$Dir = "npc host->join")
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
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
        Write-Host "  CROSSCHECK [$Dir] FAIL - no time-overlapping NPCs"
        return [pscustomobject]@{ status = "FAIL"; metrics = @{ overlapping = 0 }; detail = "no time-overlapping NPCs" }
    }
    # Only JUDGE continuously co-present NPCs (aligned >= half the best coverage);
    # boundary patrollers are an interest-handoff concern, not sync fidelity.
    $maxAligned = ($rows | ForEach-Object { $_.aligned } | Measure-Object -Maximum).Maximum
    $minAligned = [Math]::Max(10, [int]($maxAligned * 0.5))
    $judged = @($rows | Where-Object { $_.aligned -ge $minAligned })
    if ($judged.Count -lt $MinJudged) {
        # Too few continuously-present NPCs to judge. This used to silently pass
        # ("defer to POSE-STATE"); it is now an explicit SKIP so a scenario whose
        # PRIMARY gate this is fails loudly instead of green-by-default.
        if ($judged.Count -ge 1) {
            $tracked = @($judged | Where-Object { $_.med -le $Tol }).Count
            $worst = ($judged | ForEach-Object { $_.med } | Measure-Object -Maximum).Maximum
            Write-Host "  CROSSCHECK [$Dir] SKIP - only $($judged.Count) continuously-present NPC(s) (need >= $MinJudged to judge); tracked $tracked/$($judged.Count) within $Tol (worstMedian=$([Math]::Round($worst,1)))"
            return [pscustomobject]@{ status = "SKIP"; metrics = @{ judged = $judged.Count; tracked = $tracked; worstMedian = [Math]::Round($worst, 2) }; detail = "too few co-present NPCs" }
        }
        Write-Host "  CROSSCHECK [$Dir] SKIP - no continuously-present NPC(s) (maxAligned=$maxAligned)"
        return [pscustomobject]@{ status = "SKIP"; metrics = @{ judged = 0; maxAligned = $maxAligned }; detail = "no continuously-present NPCs" }
    }
    $tracked = @($judged | Where-Object { $_.med -le $Tol }).Count
    $worst = ($judged | ForEach-Object { $_.med } | Measure-Object -Maximum).Maximum
    $ratio = [Math]::Round($tracked / $judged.Count, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  CROSSCHECK [$Dir] $v - tracked $tracked/$($judged.Count) continuously-present NPCs within $Tol (ratio=$ratio >= $MinRatio, worstMedian=$([Math]::Round($worst,1)), excluded $($rows.Count - $judged.Count) boundary)"
    return [pscustomobject]@{ status = $v
        metrics = @{ judged = $judged.Count; tracked = $tracked; ratio = $ratio;
                     worstMedian = [Math]::Round($worst, 2); excluded = ($rows.Count - $judged.Count) } }
}

function Test-NpcTrack {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $r = Measure-NpcSync -HostFile $HostFile -JoinFile $JoinFile -Tol $Tol
    return (Add-GateResult -Name "npc_track" -Status $r.status -Metrics $r.metrics -Detail $r.detail)
}

# coop_presence: BIDIRECTIONAL presence cross-check - the keystone two-player test.
#   host->join : host MEMBER(rank0) vs join RECV(rank0)
#   join->host : join MEMBER(rank1) vs host RECV(rank1)
function Test-CoopPresence {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $t = [Math]::Max($Tol, 6.0)
    Write-Host "  COOP-PRESENCE host->join (host owns rank0):"
    $h2j = Measure-NpcSync -HostFile $HostFile -JoinFile $JoinFile -Tol $t -MinRatio 1.0 -MinJudged 1 -Dir "presence host->join"
    Write-Host "  COOP-PRESENCE join->host (join owns rank1):"
    $j2h = Measure-NpcSync -HostFile $JoinFile -JoinFile $HostFile -Tol $t -MinRatio 1.0 -MinJudged 1 -Dir "presence join->host"
    $status = Merge-Status @($h2j.status, $j2h.status)
    Write-Host "  COOP-PRESENCE $status - bidirectional presence (host->join=$($h2j.status), join->host=$($j2h.status), tol=$t)"
    $metrics = @{ tol = $t }
    foreach ($k in $h2j.metrics.Keys) { $metrics["h2j_$k"] = $h2j.metrics[$k] }
    foreach ($k in $j2h.metrics.Keys) { $metrics["j2h_$k"] = $j2h.metrics[$k] }
    return (Add-GateResult -Name "coop_presence" -Status $status -Metrics $metrics)
}

# ---- Pose / body-state oracles ---------------------------------------------------

# Stage 5 pose oracle. For each STATIONARY host NPC with a reproducible task,
# check the join reproduced the SAME task. Inconclusive -> SKIP.
function Test-NpcPose {
    param([string]$HostFile, [string]$JoinFile,
          [double]$StillRadius = 3.0, [double]$MinRatio = 0.60, [int]$MinJudged = 2)
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
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
        Write-Host "  POSE [npc] SKIP - only $judged seated tasked NPC(s) (need >= $MinJudged)"
        return (Add-GateResult -Name "pose" -Status SKIP -Metrics @{ judged = $judged } -Detail "too few seated tasked NPCs")
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " mismatches: $($mism -join ', ')" } else { "" }
    Write-Host "  POSE [npc] $v - join reproduced host task for $matched/$judged seated NPCs (ratio=$ratio >= $MinRatio)$detail"
    return (Add-GateResult -Name "pose" -Status $v -Metrics @{ judged = $judged; matched = $matched; ratio = $ratio } -Detail $detail.Trim())
}

# AUTHORITATIVE pose oracle (standing vs sitting) read off the ANIMATED skeleton:
# same-NPC host-vs-join time-aligned |pelvis delta|. Inconclusive -> SKIP.
function Test-NpcPoseState {
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
        Write-Host "  POSE-STATE [npc] SKIP - only $judged valid aligned pelvis pair(s) across $npcJudged NPC(s) (need >= $MinJudged; pelvis read may be failing)"
        return (Add-GateResult -Name "pose_state" -Status SKIP -Metrics @{ pairs = $judged; npcs = $npcJudged } -Detail "too few pelvis pairs")
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " pose-mismatch: $($mism -join ', ')" } else { "" }
    Write-Host "  POSE-STATE [npc] $v - join pelvis tracked host within ${PelvisTol}u for $matched/$judged pairs across $npcJudged NPC(s) (ratio=$ratio >= $MinRatio)$detail"
    return (Add-GateResult -Name "pose_state" -Status $v -Metrics @{ pairs = $judged; matched = $matched; ratio = $ratio; npcs = $npcJudged } -Detail $detail.Trim())
}

# bed_pose oracle (protocol 19 phase 1, conscious bed use). Anchors on the
# host's "SCENARIO BED ORDER issued" marker (which carries the subject hand and
# the ACCEPTED bed TaskType ids, so nothing is hardcoded), then gates:
#   1. host committed: the host's post-order MEMBER series for L0 holds a bed
#      task for >= MinCommit samples (the order actually landed + streamed),
#   2. join committed: the join's RECV series for the same hand holds the same
#      accepted task for >= MinCommit samples (the driven copy took the pose),
#   3. co-located: median time-aligned host<->join gap over the join's bed-task
#      samples <= PosTol (same bed, not a different fixture).
# Pelvis agreement over the same aligned pairs is reported as a metric (the
# generic pose_state oracle owns skeleton-level gating).
function Test-BedPose {
    param([string]$HostFile, [string]$JoinFile,
          [double]$PosTol = 3.0, [int]$MinCommit = 6, [int]$MaxDt = 800)
    $m = Select-String -Path $HostFile -Pattern "SCENARIO BED ORDER issued hand=(\d+),(\d+) task=(\d+) accept=(\d+),(\d+) ok=1" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $m) {
        Write-Host "  BED-POSE FAIL - host never issued the bed order (ok=1 marker missing)"
        return (Add-GateResult -Name "bed_pose" -Status FAIL -Detail "no BED ORDER ok=1 marker")
    }
    $g = $m.Matches[0].Groups
    $keyPrefix = "$($g[1].Value),$($g[2].Value),"
    $accept = @([int]$g[4].Value, [int]$g[5].Value)
    $t0 = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO BED ORDER issued"

    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $hKey = $H.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
    $jKey = $J.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
    if ($null -eq $hKey -or $null -eq $jKey) {
        Write-Host "  BED-POSE FAIL - subject series missing (host=$([bool]$hKey) join=$([bool]$jKey))"
        return (Add-GateResult -Name "bed_pose" -Status FAIL -Detail "subject series missing")
    }
    $hPost = @($H[$hKey] | Where-Object { $_.t -ge $t0 })
    $jPost = @($J[$jKey] | Where-Object { $_.t -ge $t0 })
    $hBed  = @($hPost | Where-Object { $accept -contains $_.task })
    $jBed  = @($jPost | Where-Object { $accept -contains $_.task })

    # Co-location + pelvis agreement over the join's bed-task samples.
    $gaps = New-Object System.Collections.ArrayList
    $pelvisPairs = 0; $pelvisOk = 0
    foreach ($js in $jBed) {
        $best = [double]::MaxValue; $bh = $null
        foreach ($hs in $hPost) {
            $dt = [Math]::Abs($hs.t - $js.t)
            if ($dt -lt $best) { $best = $dt; $bh = $hs }
        }
        if ($best -le $MaxDt -and $null -ne $bh) {
            $dx = $js.p[0]-$bh.p[0]; $dy = $js.p[1]-$bh.p[1]; $dz = $js.p[2]-$bh.p[2]
            [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            if ($js.pelvis -gt 0.5 -and $bh.pelvis -gt 0.5) {
                $pelvisPairs++
                if ([Math]::Abs($js.pelvis - $bh.pelvis) -le 1.5) { $pelvisOk++ }
            }
        }
    }
    $medGap = -1.0
    if ($gaps.Count -gt 0) {
        $sorted = @($gaps | Sort-Object)
        $medGap = [Math]::Round($sorted[[int][Math]::Floor($sorted.Count / 2)], 2)
    }

    $hostCommit = ($hBed.Count -ge $MinCommit)
    $joinCommit = ($jBed.Count -ge $MinCommit)
    $colocated  = ($gaps.Count -gt 0 -and $medGap -le $PosTol)
    $ok = ($hostCommit -and $joinCommit -and $colocated)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $why = @()
    if (-not $hostCommit) { $why += "host bed-task samples $($hBed.Count) < $MinCommit (order never committed on host)" }
    if (-not $joinCommit) { $why += "join bed-task samples $($jBed.Count) < $MinCommit (driven copy never took the pose)" }
    if (-not $colocated)  { $why += "median gap $medGap > $PosTol (or no aligned pairs)" }
    $detail = $why -join "; "
    Write-Host "  BED-POSE $v - hostBed=$($hBed.Count) joinBed=$($jBed.Count) medGap=$medGap pelvisOk=$pelvisOk/$pelvisPairs $detail"
    return (Add-GateResult -Name "bed_pose" -Status $v `
                -Metrics @{ hostBed = $hBed.Count; joinBed = $jBed.Count; medGap = $medGap;
                            pelvisPairs = $pelvisPairs; pelvisOk = $pelvisOk } -Detail $detail)
}

# Stage 2 body-state oracle: every host-DOWN sample must be down on the join's
# time-aligned sample too. Inconclusive (no host-down samples) -> SKIP.
function Test-NpcBodyState {
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
        Write-Host "  BODY-STATE [npc] SKIP - only $judged host-down aligned pair(s) across $npcJudged NPC(s) (need >= $MinJudged)"
        return (Add-GateResult -Name "body_state" -Status SKIP -Metrics @{ pairs = $judged; npcs = $npcJudged } -Detail "no/too few host-down samples")
    }
    $ratio = [Math]::Round($matched / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " not-down-on-join: $($mism -join ', ')" } else { "" }
    Write-Host "  BODY-STATE [npc] $v - join body was down for $matched/$judged host-down pairs across $npcJudged NPC(s) (ratio=$ratio >= $MinRatio)$detail"
    return (Add-GateResult -Name "body_state" -Status $v -Metrics @{ pairs = $judged; matched = $matched; ratio = $ratio } -Detail $detail.Trim())
}

# ---- Live-transition oracles -----------------------------------------------------

# craft_order LIVE-transition: the join's driven worker must go idle -> operating
# after the host's runtime "SCENARIO ORDER" marker.
function Test-CraftOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$WorkTask = 97,
          [int]$GraceMs = 4000, [double]$MinRatio = 0.70)
    $T = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO ORDER"
    if ($null -eq $T) { Write-Host "  CRAFT-ORDER FAIL - no SCENARIO ORDER marker on host"; return (Add-GateResult -Name "craft_order" -Status FAIL -Detail "no ORDER marker") }
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $worker = $null
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and $_.task -eq $WorkTask }).Count -gt 0) { $worker = $hand; break }
    }
    if ($null -eq $worker) { Write-Host "  CRAFT-ORDER FAIL - host never streamed task=$WorkTask after the order"; return (Add-GateResult -Name "craft_order" -Status FAIL -Detail "host never streamed work task") }
    if (-not $J.ContainsKey($worker)) { Write-Host "  CRAFT-ORDER FAIL - join never received worker hand=$worker"; return (Add-GateResult -Name "craft_order" -Status FAIL -Detail "join never received worker") }
    $pre  = @($J[$worker] | Where-Object { $_.t -lt $T })
    $post = @($J[$worker] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($pre.Count -lt 1 -or $post.Count -lt 1) {
        Write-Host "  CRAFT-ORDER FAIL - insufficient join samples (pre=$($pre.Count) post=$($post.Count))"
        return (Add-GateResult -Name "craft_order" -Status FAIL -Metrics @{ pre = $pre.Count; post = $post.Count } -Detail "insufficient join samples")
    }
    $preIdle  = @($pre  | Where-Object { $_.task -ne $WorkTask }).Count
    $postWork = @($post | Where-Object { $_.task -eq $WorkTask }).Count
    $preRatio  = [Math]::Round($preIdle  / $pre.Count, 3)
    $postRatio = [Math]::Round($postWork / $post.Count, 3)
    $ok = ($preRatio -ge $MinRatio -and $postRatio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  CRAFT-ORDER [join] $v - worker=$worker idle-before $preIdle/$($pre.Count) (ratio=$preRatio), operating-after $postWork/$($post.Count) (ratio=$postRatio), task=$WorkTask"
    return (Add-GateResult -Name "craft_order" -Status $v -Metrics @{ preRatio = $preRatio; postRatio = $postRatio })
}

# down_order LIVE-transition: join subject upright BEFORE the host's
# "SCENARIO DOWN issued" marker, down (bs & 7) AFTER it.
function Test-DownOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 4000, [double]$MinRatio = 0.70)
    # Anchor on "issued" - a bare "SCENARIO DOWN" also matches the earlier lowercase
    # "SCENARIO down subject pinned" line (Select-String is case-insensitive).
    $T = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO DOWN issued"
    if ($null -eq $T) { Write-Host "  DOWN-ORDER FAIL - no 'SCENARIO DOWN issued' marker on host"; return (Add-GateResult -Name "down_order" -Status FAIL -Detail "no DOWN marker") }
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $down = { param($bs) (($bs -band 7) -ne 0) }
    $subject = $null
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and (& $down $_.bs) }).Count -gt 0) { $subject = $hand; break }
    }
    if ($null -eq $subject) { Write-Host "  DOWN-ORDER FAIL - host never streamed a down body after the order"; return (Add-GateResult -Name "down_order" -Status FAIL -Detail "host never streamed down body") }
    if (-not $J.ContainsKey($subject)) { Write-Host "  DOWN-ORDER FAIL - join never received subject hand=$subject"; return (Add-GateResult -Name "down_order" -Status FAIL -Detail "join never received subject") }
    $pre  = @($J[$subject] | Where-Object { $_.t -lt $T })
    $post = @($J[$subject] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($pre.Count -lt 1 -or $post.Count -lt 1) {
        Write-Host "  DOWN-ORDER FAIL - insufficient join samples (pre=$($pre.Count) post=$($post.Count))"
        return (Add-GateResult -Name "down_order" -Status FAIL -Metrics @{ pre = $pre.Count; post = $post.Count } -Detail "insufficient join samples")
    }
    $preUp   = @($pre  | Where-Object { -not (& $down $_.bs) }).Count
    $postDn  = @($post | Where-Object {      (& $down $_.bs) }).Count
    $preRatio  = [Math]::Round($preUp  / $pre.Count, 3)
    $postRatio = [Math]::Round($postDn / $post.Count, 3)
    $ok = ($preRatio -ge $MinRatio -and $postRatio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  DOWN-ORDER [join] $v - subject=$subject upright-before $preUp/$($pre.Count) (ratio=$preRatio), down-after $postDn/$($post.Count) (ratio=$postRatio)"
    return (Add-GateResult -Name "down_order" -Status $v -Metrics @{ preRatio = $preRatio; postRatio = $postRatio })
}

# death_order RELIABLE-EVENT: the join must receive the host's EVT_DEATH (ev=2)
# for the same hand. Also measures the reliable-event delivery latency (SEND ->
# RECV, clock-offset-corrected) - the metric a real WAN run makes meaningful.
function Test-DeathOrder {
    param([string]$HostFile, [string]$JoinFile)
    # NB: the SEND line format is "... ev=2 hand=<h> actor=<a> bs <x>-><y>". The
    # original oracle predated the actor field and anchored 'hand=([\d,]+?) bs',
    # which stopped matching when combat attribution added ' actor=..' - a silent
    # oracle rot found by the re-validation audit. Greedy digits/commas is enough.
    $send = Select-String -Path $HostFile -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] SEND id=\d+ ev=2 hand=([\d,]+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $send) { Write-Host "  DEATH-ORDER FAIL - host emitted no death event (ev=2)"; return (Add-GateResult -Name "death_order" -Status FAIL -Detail "no EVT_DEATH sent") }
    $hand = $send.Matches[0].Groups[5].Value
    $sendT = Convert-StampToMs -Groups $send.Matches[0].Groups -OffsetMs (Get-LogClockOffsetMs -File $HostFile)
    $pat  = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] RECV id=\d+ ev=2 owner=\d+ hand=' + [regex]::Escape($hand) + '\b'
    $recv = @(Select-String -Path $JoinFile -Pattern $pat -ErrorAction SilentlyContinue)
    if ($recv.Count -lt 1) {
        Write-Host "  DEATH-ORDER FAIL - join never received the reliable death event for hand=$hand"
        return (Add-GateResult -Name "death_order" -Status FAIL -Detail "EVT_DEATH not received")
    }
    $recvT = Convert-StampToMs -Groups $recv[0].Matches[0].Groups -OffsetMs (Get-LogClockOffsetMs -File $JoinFile)
    $latMs = $recvT - $sendT
    Write-Host "  DEATH-ORDER [join] PASS - reliable EVT_DEATH received for hand=$hand ($($recv.Count) delivery/deliveries, latency=${latMs}ms)"
    return (Add-GateResult -Name "death_order" -Status PASS -Metrics @{ deliveries = $recv.Count; latencyMs = $latMs })
}

# combat_probe READ oracle (host-side): duel spawned, sustained in-combat samples,
# and the duelists mutually target each other's resolvable hands.
function Test-CombatProbe {
    param([string]$HostFile)
    $setup = Select-String -Path $HostFile -Pattern 'SETUP\(duel\): spawned PEACEFUL A=([\d]+),([\d]+) B=([\d]+),([\d]+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $setup) { Write-Host "  COMBAT-PROBE FAIL - duel did not spawn"; return (Add-GateResult -Name "combat_probe" -Status FAIL -Detail "duel did not spawn") }
    $aIdx = $setup.Matches[0].Groups[1].Value; $aSer = $setup.Matches[0].Groups[2].Value
    $bIdx = $setup.Matches[0].Groups[3].Value; $bSer = $setup.Matches[0].Groups[4].Value
    $fighting = @(Select-String -Path $HostFile -Pattern 'COMBAT [AB] .*inCombat=1.*hasTarget=1' -ErrorAction SilentlyContinue)
    if ($fighting.Count -lt 10) {
        Write-Host "  COMBAT-PROBE FAIL - only $($fighting.Count) in-combat+targeted sample(s) (need >= 10)"
        return (Add-GateResult -Name "combat_probe" -Status FAIL -Metrics @{ samples = $fighting.Count } -Detail "too few in-combat samples")
    }
    $aOnB = @(Select-String -Path $HostFile -Pattern ("COMBAT A .*hasTarget=1 target=" + [regex]::Escape("$bIdx,$bSer")) -ErrorAction SilentlyContinue)
    $bOnA = @(Select-String -Path $HostFile -Pattern ("COMBAT B .*hasTarget=1 target=" + [regex]::Escape("$aIdx,$aSer")) -ErrorAction SilentlyContinue)
    if ($aOnB.Count -lt 1 -or $bOnA.Count -lt 1) {
        Write-Host "  COMBAT-PROBE FAIL - duelists not mutually targeting (A->B=$($aOnB.Count) B->A=$($bOnA.Count))"
        return (Add-GateResult -Name "combat_probe" -Status FAIL -Metrics @{ aOnB = $aOnB.Count; bOnA = $bOnA.Count } -Detail "not mutually targeting")
    }
    Write-Host "  COMBAT-PROBE [host] PASS - live duel: $($fighting.Count) in-combat samples; mutual attack-target hands resolve (A->B, B->A)"
    return (Add-GateResult -Name "combat_probe" -Status PASS -Metrics @{ samples = $fighting.Count })
}

# combat_order LIVE-transition: the join's own copies flip NOT-combat -> combat
# (task=65024) after the host's "SCENARIO COMBAT issued" marker.
function Test-CombatOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 5000,
          [double]$PostMin = 0.40, [double]$PreMax = 0.10)
    $COMBAT = 65024 # TASK_COMBAT_MELEE (0xFE00)
    $T = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO COMBAT issued"
    if ($null -eq $T) { Write-Host "  COMBAT-ORDER FAIL - no 'SCENARIO COMBAT issued' marker on host"; return (Add-GateResult -Name "combat_order" -Status FAIL -Detail "no COMBAT marker") }
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $duelists = @()
    foreach ($hand in $H.Keys) {
        if (@($H[$hand] | Where-Object { $_.t -ge $T -and $_.task -eq $COMBAT }).Count -gt 0) { $duelists += $hand }
    }
    if ($duelists.Count -lt 1) { Write-Host "  COMBAT-ORDER FAIL - host never streamed a combat task after the order"; return (Add-GateResult -Name "combat_order" -Status FAIL -Detail "no combat task streamed") }
    $preCombat = 0; $preTotal = 0; $postCombat = 0; $postTotal = 0; $seen = 0
    foreach ($hand in $duelists) {
        if (-not $J.ContainsKey($hand)) { continue }
        $seen++
        $pre  = @($J[$hand] | Where-Object { $_.t -lt $T })
        $post = @($J[$hand] | Where-Object { $_.t -ge ($T + $GraceMs) })
        $preTotal  += $pre.Count;  $preCombat  += @($pre  | Where-Object { $_.task -eq $COMBAT }).Count
        $postTotal += $post.Count; $postCombat += @($post | Where-Object { $_.task -eq $COMBAT }).Count
    }
    if ($seen -lt 1) { Write-Host "  COMBAT-ORDER FAIL - join never received any duelist hand (saw $($duelists.Count) on host)"; return (Add-GateResult -Name "combat_order" -Status FAIL -Detail "join received no duelists") }
    if ($preTotal -lt 1 -or $postTotal -lt 1) { Write-Host "  COMBAT-ORDER FAIL - insufficient join samples (preTotal=$preTotal postTotal=$postTotal)"; return (Add-GateResult -Name "combat_order" -Status FAIL -Detail "insufficient join samples") }
    $preRatio  = [Math]::Round($preCombat  / $preTotal, 3)
    $postRatio = [Math]::Round($postCombat / $postTotal, 3)
    $ok = ($preRatio -le $PreMax -and $postRatio -ge $PostMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  COMBAT-ORDER [join] $v - duelists=$($duelists.Count) seen=$seen combat-before $preCombat/$preTotal (ratio=$preRatio<=$PreMax), combat-after $postCombat/$postTotal (ratio=$postRatio>=$PostMin)"
    return (Add-GateResult -Name "combat_order" -Status $v -Metrics @{ preRatio = $preRatio; postRatio = $postRatio; duelists = $duelists.Count })
}

# combat_kill OUTCOME oracle: host-authoritative resolution + attribution +
# reliable delivery + synced down outcome. Also measures event latency.
function Test-CombatKill {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 3000, [double]$MinDown = 0.70)
    $pin = Select-String -Path $HostFile -Pattern 'SCENARIO duel subjects pinned A=(\d+),(\d+) B=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pin) { Write-Host "  COMBAT-KILL FAIL - no pinned duelists on host"; return (Add-GateResult -Name "combat_kill" -Status FAIL -Detail "no pinned duelists") }
    $ai = $pin.Matches[0].Groups[1].Value; $as = $pin.Matches[0].Groups[2].Value
    $bi = $pin.Matches[0].Groups[3].Value; $bs = $pin.Matches[0].Groups[4].Value
    $sendPat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] SEND id=\d+ ev=(1|2) hand=\d+,\d+,\d+,' + $bi + ',' + $bs + ' actor=' + $ai + ',' + $as
    $send = Select-String -Path $HostFile -Pattern $sendPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $send) {
        Write-Host "  COMBAT-KILL FAIL - host sent no combat KO/death for B=$bi,$bs with actor A=$ai,$as (no real takedown / no attribution)"
        return (Add-GateResult -Name "combat_kill" -Status FAIL -Detail "no attributed KO/death sent")
    }
    $ev = $send.Matches[0].Groups[5].Value
    $sendT = Convert-StampToMs -Groups $send.Matches[0].Groups -OffsetMs (Get-LogClockOffsetMs -File $HostFile)
    $recvPat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] RECV id=\d+ ev=' + $ev + ' .*hand=\d+,\d+,\d+,' + $bi + ',' + $bs + ' actor=' + $ai + ',' + $as
    $recv = Select-String -Path $JoinFile -Pattern $recvPat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $recv) {
        Write-Host "  COMBAT-KILL FAIL - join never received the reliable combat event (ev=$ev hand=B actor=A)"
        return (Add-GateResult -Name "combat_kill" -Status FAIL -Detail "attributed event not received")
    }
    $rg = $recv.Matches[0].Groups
    $T = Convert-StampToMs -Groups $rg -OffsetMs (Get-LogClockOffsetMs -File $JoinFile)
    $latMs = $T - $sendT
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $bKey = $null
    foreach ($hand in $J.Keys) { if ($hand -match ('^' + $bi + ',' + $bs + ',')) { $bKey = $hand; break } }
    if ($null -eq $bKey) { Write-Host "  COMBAT-KILL FAIL - join logged no body series for victim B=$bi,$bs"; return (Add-GateResult -Name "combat_kill" -Status FAIL -Detail "no victim series on join") }
    $post = @($J[$bKey] | Where-Object { $_.t -ge ($T + $GraceMs) })
    if ($post.Count -lt 1) { Write-Host "  COMBAT-KILL FAIL - no join samples after the event"; return (Add-GateResult -Name "combat_kill" -Status FAIL -Detail "no post-event samples") }
    $down = @($post | Where-Object { ($_.bs -band 7) -ne 0 }).Count
    $ratio = [Math]::Round($down / $post.Count, 3)
    $ok = ($ratio -ge $MinDown)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $evName = if ($ev -eq "2") { "DEATH" } else { "KO" }
    Write-Host "  COMBAT-KILL [join] $v - combat $evName for B=$bi,$bs by A=$ai,${as}: reliable event received (latency=${latMs}ms), victim down-after $down/$($post.Count) (ratio=$ratio>=$MinDown)"
    return (Add-GateResult -Name "combat_kill" -Status $v -Metrics @{ downRatio = $ratio; latencyMs = $latMs; eventType = $evName })
}

# damage_guard: the join's cosmetic fights must apply NO local damage. From the
# host's pinned victim (B), compare each side's LOCAL blood series ("SCENARIO
# VITALS hand=i,s ... blood=..") - the HOST's victim must LOSE blood (a real
# fight happened). PROTOCOL 16 REWRITE: combat-scoped NPC vitals now STREAM
# host->join, so the join's series legitimately TRACKS the host's drop - the
# old "join stays flat" gate would fail on the feature working. The guard is
# now proven by (a) the intercept counters (local swings happened and were
# stopped) and (b) NO EXCESS local damage: the join's lowest blood must not
# undershoot the host's lowest by more than MaxExcessDrop (a local leak lands
# ON TOP of the streamed truth and drags the copy below it).
function Test-DamageGuard {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinHostDrop = 5.0, [double]$MaxExcessDrop = 8.0)
    $pin = Select-String -Path $HostFile -Pattern 'SCENARIO duel subjects pinned A=(\d+),(\d+) B=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pin) {
        Write-Host "  DAMAGE-GUARD SKIP - no pinned duelists on host"
        return (Add-GateResult -Name "damage_guard" -Status SKIP -Detail "no pinned duelists")
    }
    $bi = $pin.Matches[0].Groups[3].Value; $bs = $pin.Matches[0].Groups[4].Value
    $series = {
        param($file)
        $vals = @()
        foreach ($m in (Select-String -Path $file -Pattern ("SCENARIO VITALS hand=" + $bi + "," + $bs + " t=\d+ blood=(-?[\d\.]+)") -ErrorAction SilentlyContinue)) {
            $vals += [double]$m.Matches[0].Groups[1].Value
        }
        return ,$vals
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  DAMAGE-GUARD SKIP - insufficient vitals samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "damage_guard" -Status SKIP -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient vitals samples")
    }
    $hostDrop = ($H | Measure-Object -Maximum).Maximum - ($H | Measure-Object -Minimum).Minimum
    $joinDrop = ($J | Measure-Object -Maximum).Maximum - ($J | Measure-Object -Minimum).Minimum
    $hostMin  = ($H | Measure-Object -Minimum).Minimum
    $joinMin  = ($J | Measure-Object -Minimum).Minimum
    $excess   = $hostMin - $joinMin  # join undershooting the host's floor = local leak
    $m = @{ hostDrop = [Math]::Round($hostDrop, 1); joinDrop = [Math]::Round($joinDrop, 1)
            hostMin = [Math]::Round($hostMin, 1); joinMin = [Math]::Round($joinMin, 1)
            excessDrop = [Math]::Round($excess, 1)
            hostSamples = $H.Count; joinSamples = $J.Count }
    # The JOIN-BELOW-HOST gate: streamed vitals can only pull the copy DOWN TO
    # the host's truth; any blood below that floor was local damage the guard
    # should have intercepted.
    if ($excess -gt $MaxExcessDrop) {
        Write-Host ("  DAMAGE-GUARD FAIL - join's driven victim B=$bi,$bs fell $([Math]::Round($excess,1)) blood BELOW the host's floor (> $MaxExcessDrop; cosmetic fight leaked damage)")
        return (Add-GateResult -Name "damage_guard" -Status FAIL -Metrics $m -Detail "join copy took local damage beyond the streamed truth")
    }
    # Engagement signal: the join's guard must have actually intercepted swings
    # (proves the local cosmetic fight generated hits AND the hook stopped them).
    $dg = Select-String -Path $JoinFile -Pattern 'SCENARIO DMGGUARD guarded=(\d+) passed=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $dg) {
        $m.guardedHits = [int]$dg.Matches[0].Groups[1].Value
        $m.passedHits  = [int]$dg.Matches[0].Groups[2].Value
    }
    if ($hostDrop -lt $MinHostDrop) {
        # The scripted takedown is a KO (knockDown), which may draw no blood on the
        # host - the join-side no-excess plus the hook's intercept count still prove
        # the guard: local swings happened and none landed.
        if ($null -ne $dg -and $m.guardedHits -gt 0) {
            Write-Host ("  DAMAGE-GUARD PASS - join guard intercepted $($m.guardedHits) local swing(s), no excess join damage (excess=$([Math]::Round($excess,1))); host fight drew $([Math]::Round($hostDrop,1)) blood (KO takedown)")
            return (Add-GateResult -Name "damage_guard" -Status PASS -Metrics $m)
        }
        Write-Host ("  DAMAGE-GUARD SKIP - host fight drew only $([Math]::Round($hostDrop,1)) blood and the join guard intercepted no swings; nothing to judge")
        return (Add-GateResult -Name "damage_guard" -Status SKIP -Metrics $m -Detail "no blood drawn + no swings intercepted")
    }
    Write-Host ("  DAMAGE-GUARD PASS - victim B=$bi,$bs host blood drop=$([Math]::Round($hostDrop,1)) (real fight) " +
                "join floor excess=$([Math]::Round($excess,1)) (<= $MaxExcessDrop; join tracks the streamed truth, drop=$([Math]::Round($joinDrop,1)))")
    return (Add-GateResult -Name "damage_guard" -Status PASS -Metrics $m)
}

# Parse the EXTENDED "SCENARIO VITALS hand=i,s t=.. blood=.. bleed=.. fl=a,b,c,d
# bd=a,b,c,d unc=.. dead=.." series for one hand (i,s) into a time-ordered list of
# @{t; blood; fleshMin; bandSum; unc; dead; pfl; pst; ls}. Lines in the SHORT
# (combat_kill) format still parse (medical fields default). Protocol-16 logs
# append " pfl=<minFleshAllParts> pst=<minStunAllParts> ls=a,b,c,d" (LimbStates,
# 255=unknown); older logs leave those null. Timestamps are clock-offset
# corrected into the host frame like Get-ScenarioSeries.
function Get-VitalsSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO VITALS hand=' + [regex]::Escape($HandIS) +
           ' t=\d+ blood=(-?[\d\.]+)' +
           '(?: bleed=(-?[\d\.]+) fl=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+)' +
           ' bd=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) unc=(\d) dead=(\d))?' +
           '(?: pfl=(-?[\d\.]+) pst=(-?[\d\.]+) ls=(\d+),(\d+),(\d+),(\d+))?'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        # PSCustomObject, NOT a hashtable: Measure-Object -Property only sees real
        # properties (a hashtable 'blood' KEY measures as null -> silent nonsense).
        $e = [pscustomobject]@{ t = $t; blood = [double]$g[5].Value; fleshMin = $null; bandSum = $null; unc = 0; dead = 0; pfl = $null; pst = $null; ls = $null }
        if ($g[6].Success) {
            $fl = @(); for ($i = 7; $i -le 10; $i++) { $v = [double]$g[$i].Value; if ($v -ge 0) { $fl += $v } }
            $bd = 0.0;  for ($i = 11; $i -le 14; $i++) { $v = [double]$g[$i].Value; if ($v -ge 0) { $bd += $v } }
            if ($fl.Count -gt 0) { $e.fleshMin = ($fl | Measure-Object -Minimum).Minimum }
            $e.bandSum = $bd
            $e.unc  = [int]$g[15].Value
            $e.dead = [int]$g[16].Value
        }
        if ($g[17].Success) {
            $pv = [double]$g[17].Value; if ($pv -ge 0) { $e.pfl = $pv }
            $sv = [double]$g[18].Value; if ($sv -ge 0) { $e.pst = $sv }
            $e.ls = @([int]$g[19].Value, [int]$g[20].Value, [int]$g[21].Value, [int]$g[22].Value)
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# npc_vitals (protocol 16, Phase B): combat-scoped world-NPC vitals must MATCH
# across sides - the combat_kill victim's join-side blood must CONVERGE to the
# host's truth, not just reach the KO outcome. Time-aligns the two VITALS
# series (CLOCKSYNC-corrected) and gates the median |host - join| blood gap
# over the tail (last TailMs of overlap) <= Tol.
function Test-NpcVitals {
    param([string]$HostFile, [string]$JoinFile,
          [double]$Tol = 15.0, [int]$TailMs = 10000, [int]$PairWinMs = 1500)
    $pin = Select-String -Path $HostFile -Pattern 'SCENARIO duel subjects pinned A=(\d+),(\d+) B=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pin) {
        Write-Host "  NPC-VITALS SKIP - no pinned duelists on host"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Detail "no pinned duelists")
    }
    $bi = $pin.Matches[0].Groups[3].Value; $bs = $pin.Matches[0].Groups[4].Value
    $handIS = "$bi,$bs"
    $Hv = Get-VitalsSeries -File $HostFile -HandIS $handIS
    $Jv = Get-VitalsSeries -File $JoinFile -HandIS $handIS
    if ($Hv.Count -lt 3 -or $Jv.Count -lt 3) {
        Write-Host "  NPC-VITALS SKIP - insufficient vitals series (host=$($Hv.Count) join=$($Jv.Count))"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Metrics @{ host = $Hv.Count; join = $Jv.Count } -Detail "insufficient series")
    }
    $endT = [Math]::Min(($Hv | Measure-Object -Property t -Maximum).Maximum,
                        ($Jv | Measure-Object -Property t -Maximum).Maximum)
    $tail = $endT - $TailMs
    $gaps = @()
    foreach ($js in @($Jv | Where-Object { $_.t -ge $tail -and $_.t -le $endT })) {
        $near = $null; $best = $PairWinMs + 1
        foreach ($hs in $Hv) {
            $dt = [Math]::Abs($hs.t - $js.t)
            if ($dt -lt $best) { $best = $dt; $near = $hs }
        }
        if ($null -ne $near) { $gaps += [Math]::Abs($near.blood - $js.blood) }
    }
    if ($gaps.Count -lt 3) {
        Write-Host "  NPC-VITALS SKIP - too few aligned tail pairs ($($gaps.Count))"
        return (Add-GateResult -Name "npc_vitals" -Status SKIP -Metrics @{ pairs = $gaps.Count } -Detail "too few aligned pairs")
    }
    $sorted = @($gaps | Sort-Object)
    $median = $sorted[[int][Math]::Floor($sorted.Count / 2)]
    $m = @{ pairs = $gaps.Count; medianGap = [Math]::Round($median, 1)
            maxGap = [Math]::Round(($gaps | Measure-Object -Maximum).Maximum, 1) }
    if ($median -gt $Tol) {
        Write-Host "  NPC-VITALS FAIL - victim B=$handIS tail blood gap median=$($m.medianGap) > $Tol (NPC vitals not converging)"
        return (Add-GateResult -Name "npc_vitals" -Status FAIL -Metrics $m -Detail "victim vitals diverge")
    }
    Write-Host "  NPC-VITALS PASS - victim B=$handIS tail blood gap median=$($m.medianGap) (<= $Tol) over $($m.pairs) pairs"
    return (Add-GateResult -Name "npc_vitals" -Status PASS -Metrics $m)
}

# limb_loss (protocol 16, Phase C): each side amputates its OWN tab leader's
# limb (A: host member limb 0, B: join member limb 1). Gates per direction:
#   stump-crossed - the COPY side's ls[limb] leaves ORIGINAL (STUMP/REPLACED)
#                   within MaxLatencyMs of the owner's cut (event or self-heal)
#   stump-sticky  - once stump, the copy never reverts to ORIGINAL
#   ground-item   - each side sees >= 1 severed ground item near the subject
#                   after the cut, and no side ever sees more than the two
#                   legitimate items (a dedupe failure shows 3+)
function Test-LimbLoss {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [int]$GraceMs = 4000, [int]$MaxItems = 2)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; copyLog = $JoinFile },
        @{ tag = "B"; ownerLog = $JoinFile; copyLog = $HostFile }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO LIMB ' + $t + ' amputate issued hand=(\d+),(\d+) limb=(\d) ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no amputate marker (ok=1)"; continue }
        $hi = $mk.Matches[0].Groups[1].Value; $hs = $mk.Matches[0].Groups[2].Value
        $limb = [int]$mk.Matches[0].Groups[3].Value
        $handIS = "$hi,$hs"
        $Tc = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO LIMB ' + $t + ' amputate issued')
        # Owner ground truth: the amputation landed (ls[limb] != ORIGINAL).
        $O = Get-VitalsSeries -File $d.ownerLog -HandIS $handIS
        $ownerCut = @($O | Where-Object { $_.t -ge $Tc -and $null -ne $_.ls -and $_.ls[$limb] -ge 1 -and $_.ls[$limb] -le 3 }).Count -gt 0
        if (-not $ownerCut) { $bad += "${t}: stump never visible on the owner (scaffold failed?)"; continue }
        # 1. Stump crossed: the copy's LimbState leaves ORIGINAL within budget.
        $C = Get-VitalsSeries -File $d.copyLog -HandIS $handIS
        $cutSamples = @($C | Where-Object { $_.t -ge $Tc -and $null -ne $_.ls -and $_.ls[$limb] -ge 1 -and $_.ls[$limb] -le 3 })
        if ($cutSamples.Count -lt 1) {
            $bad += "${t}: stump never reached the copy (limb states don't cross)"
            continue
        }
        $lat = [int]($cutSamples[0].t - $Tc)
        $m["${t}StumpLatMs"] = $lat
        if ($lat -gt $MaxLatencyMs) { $bad += "${t}: stump latency ${lat}ms > $MaxLatencyMs" }
        # 2. Sticky: after the first stump sample the copy never reverts.
        $revert = @($C | Where-Object { $_.t -gt $cutSamples[0].t -and $null -ne $_.ls -and $_.ls[$limb] -eq 0 }).Count
        if ($revert -gt 0) { $bad += "${t}: copy reverted to ORIGINAL $revert time(s) after the stump" }
        # 3. Ground item: both sides converge on >= 1 severed item near the
        # subject after the cut (host-authoritative world-item channel).
        foreach ($side in @(@{ n = "owner"; f = $d.ownerLog }, @{ n = "copy"; f = $d.copyLog })) {
            $items = @()
            foreach ($mi in (Select-String -Path $side.f -Pattern ('SCENARIO LIMBITEMS hand=' + $hi + ',' + $hs + ' t=(\d+) n=(\d+)') -ErrorAction SilentlyContinue)) {
                $items += [pscustomobject]@{ t = [int]$mi.Matches[0].Groups[1].Value; n = [int]$mi.Matches[0].Groups[2].Value }
            }
            # LIMBITEMS t= is scenario-elapsed; the cut marker time is wall-clock,
            # so anchor on the owner's cut ELAPSED time via the issued line's t.
            $post = @($items | Where-Object { $_.n -ge 1 })
            $maxN = 0
            if ($items.Count -gt 0) { $maxN = ($items | Measure-Object -Property n -Maximum).Maximum }
            $m["${t}$($side.n)MaxItems"] = $maxN
            if ($post.Count -lt 1) { $bad += "${t}: no severed ground item ever visible on the $($side.n)" }
            if ($maxN -gt $MaxItems) { $bad += "${t}: $($side.n) saw $maxN severed items (> $MaxItems; dedupe failed)" }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  LIMB-LOSS FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "limb_loss" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  LIMB-LOSS PASS - A: stump crossed in $($m.AStumpLatMs)ms (items owner=$($m.AownerMaxItems) copy=$($m.AcopyMaxItems)); " +
                "B: stump crossed in $($m.BStumpLatMs)ms (items owner=$($m.BownerMaxItems) copy=$($m.BcopyMaxItems))")
    return (Add-GateResult -Name "limb_loss" -Status PASS -Metrics $m)
}

# Parse the "SCENARIO STATS hand=i,s t=.. str=.. stealth=.. dex=.. athl=..
# tough=.. xp=.." series (protocol-17 stats sync) for one hand into a
# time-ordered list of @{t; str; stealth; dex; athl; tough; xp}. Timestamps are
# clock-offset corrected into the host frame like Get-ScenarioSeries.
function Get-StatsSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO STATS hand=' + [regex]::Escape($HandIS) +
           ' t=\d+ str=(-?[\d\.]+) stealth=(-?[\d\.]+) dex=(-?[\d\.]+) athl=(-?[\d\.]+)' +
           ' tough=(-?[\d\.]+) xp=(-?[\d\.]+)'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $e = [pscustomobject]@{
            t = $t
            str     = [double]$g[5].Value
            stealth = [double]$g[6].Value
            dex     = [double]$g[7].Value
            athl    = [double]$g[8].Value
            tough   = [double]$g[9].Value
            xp      = [double]$g[10].Value
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# stats_sync (protocol 17): each side raises stats on its OWN tab leader via
# the raise-only scaffold (A: host raises str+stealth, B: join raises
# dex+athl). Gates per direction:
#   raised-crossed - the COPY side's raised stats reach the raise target within
#                    MaxLatencyMs of the owner's write (change-gated stream)
#   raised-sticky  - once crossed, the copy never falls back below the target
#   no-drift       - the control stat (toughness, touched by NEITHER side)
#                    never deviates more than DriftTol from its first sample
#                    on the copy (the stream must not corrupt untouched stats)
function Test-StatsSync {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$RaiseTo = 60.0,
          [double]$Tol = 0.5, [double]$DriftTol = 2.0)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; copyLog = $JoinFile; raised = @("str", "stealth") },
        @{ tag = "B"; ownerLog = $JoinFile; copyLog = $HostFile; raised = @("dex", "athl") }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO STATRAISE ' + $t + ' hand=(\d+),(\d+) stat\d+=[\d\.]+ ok=1 stat\d+=[\d\.]+ ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no raise marker (ok=1,1)"; continue }
        $hi = $mk.Matches[0].Groups[1].Value; $hs = $mk.Matches[0].Groups[2].Value
        $handIS = "$hi,$hs"
        $Tc = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO STATRAISE ' + $t + ' ')
        # Owner ground truth: the raise landed locally.
        $O = Get-StatsSeries -File $d.ownerLog -HandIS $handIS
        $C = Get-StatsSeries -File $d.copyLog  -HandIS $handIS
        if ($C.Count -lt 3) { $bad += "${t}: copy stats series too short ($($C.Count))"; continue }
        foreach ($stat in $d.raised) {
            $oOk = @($O | Where-Object { $_.t -ge $Tc -and $_.$stat -ge ($RaiseTo - $Tol) }).Count -gt 0
            if (-not $oOk) { $bad += "${t}: $stat never reached $RaiseTo on the owner (scaffold failed?)"; continue }
            # 1. Crossed: the copy's value reaches the target within budget.
            $cross = @($C | Where-Object { $_.t -ge $Tc -and $_.$stat -ge ($RaiseTo - $Tol) })
            if ($cross.Count -lt 1) { $bad += "${t}: $stat never crossed to the copy"; continue }
            $lat = [int]($cross[0].t - $Tc)
            $m["${t}${stat}LatMs"] = $lat
            if ($lat -gt $MaxLatencyMs) { $bad += "${t}: $stat latency ${lat}ms > $MaxLatencyMs" }
            # 2. Sticky: after crossing, the copy never falls back below.
            $revert = @($C | Where-Object { $_.t -gt $cross[0].t -and $_.$stat -lt ($RaiseTo - $Tol - 0.5) }).Count
            if ($revert -gt 0) { $bad += "${t}: $stat reverted on the copy $revert time(s) after crossing" }
        }
        # 3. No-drift: the untouched control stat stays put on the copy.
        $base = [double]$C[0].tough
        $drift = ($C | ForEach-Object { [Math]::Abs($_.tough - $base) } | Measure-Object -Maximum).Maximum
        $m["${t}ToughDrift"] = [Math]::Round($drift, 2)
        if ($drift -gt $DriftTol) { $bad += "${t}: control stat (toughness) drifted $([Math]::Round($drift,2)) on the copy (> $DriftTol)" }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  STATS-SYNC FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "stats_sync" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  STATS-SYNC PASS - A: str crossed in $($m.AstrLatMs)ms, stealth in $($m.AstealthLatMs)ms (control drift $($m.AToughDrift)); " +
                "B: dex crossed in $($m.BdexLatMs)ms, athl in $($m.BathlLatMs)ms (control drift $($m.BToughDrift))")
    return (Add-GateResult -Name "stats_sync" -Status PASS -Metrics $m)
}

# Parse a "SCENARIO CARRY" series for one hand from a log: the body's LOCAL
# carry relationship (carrying + carried hand, beingCarried), position and
# bodyState, at ~2 Hz. t = stamp-derived common-clock ms (CLOCKSYNC-corrected);
# rawT = the scenario-relative t= field (identical for every body logged in the
# same tick - used to PAIR carrier/carried samples from the same log).
function Get-CarrySeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO CARRY hand=' + [regex]::Escape($HandIS) +
           ' t=(\d+) carrying=(\d) carried=(\d+),(\d+) beingCarried=(\d)' +
           ' pos=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) bs=(\d+)'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $e = [pscustomobject]@{
            t = $t
            rawT         = [long]$g[5].Value
            carrying     = ([int]$g[6].Value -ne 0)
            carried      = ($g[7].Value + ',' + $g[8].Value)
            beingCarried = ([int]$g[9].Value -ne 0)
            x = [double]$g[10].Value; y = [double]$g[11].Value; z = [double]$g[12].Value
            bs = [int]$g[13].Value
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# Parse the 2 Hz "SCENARIO FURN hand=i,s t=ms in=<0|1|2> furn=i,s pos=x,y,z bs=n"
# series (protocol 19 furniture occupancy) for one subject hand, timestamps
# CLOCKSYNC-corrected like every other cross-log series.
function Get-FurnSeries {
    param([string]$File, [string]$HandIS)
    $list = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return ,$list }
    $off = Get-LogClockOffsetMs -File $File
    $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO FURN hand=' + [regex]::Escape($HandIS) +
           ' t=(\d+) in=(-?\d+) furn=(\d+),(\d+)' +
           ' pos=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) bs=(\d+)'
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $e = [pscustomobject]@{
            t    = $t
            rawT = [long]$g[5].Value
            in   = [int]$g[6].Value
            furn = ($g[7].Value + ',' + $g[8].Value)
            x = [double]$g[9].Value; y = [double]$g[10].Value; z = [double]$g[11].Value
            bs = [int]$g[12].Value
        }
        [void]$list.Add($e)
    }
    return ,$list
}

# bed_put / cage_put (protocol 19, unconscious furniture placement): two
# owner-side windows against the same baked fixture - A (host puts its KO'd M2
# in, later takes it out), B (the join does the same with its L1). Gates per
# window:
#   enter-crossed - the PEER's local copy reads in=<kind> within MaxLatencyMs
#                   of the author's put marker (the reliable ENTER edge or the
#                   self-heal landed engine-native on the peer)
#   held-in-place - while occupied on the peer, the copy sits at the author's
#                   copy position (same-tick median gap <= PosTol; the fixture
#                   owns the transform on both machines)
#   exit-crossed  - the peer's copy leaves the furniture (in=0) within budget
#                   of the author's out marker
function Test-FurnPut {
    param([string]$HostFile, [string]$JoinFile, [int]$Kind = 1,
          [int]$MaxLatencyMs = 12000, [double]$PosTol = 3.0)
    $gate = if ($Kind -eq 2) { "cage_put" } else { "bed_put" }
    $tagU = $gate.ToUpper()
    # Subject hands from the latch lines (each side latches both subjects; take
    # each window's subject from its AUTHOR's log).
    $roles = @{}
    foreach ($r in @(@{ who = "M2"; log = $HostFile }, @{ who = "L1"; log = $JoinFile })) {
        $mk = Select-String -Path $r.log -Pattern ('SCENARIO FURN ' + $r.who + ' hand=(\d+),(\d+)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) {
            Write-Host "  $tagU FAIL - $($r.who) never latched"
            return (Add-GateResult -Name $gate -Status FAIL -Detail "no $($r.who) latch")
        }
        $roles[$r.who] = ($mk.Matches[0].Groups[1].Value + ',' + $mk.Matches[0].Groups[2].Value)
    }
    $dirs = @(
        @{ tag = "A"; authorLog = $HostFile; peerLog = $JoinFile; subject = $roles.M2 },
        @{ tag = "B"; authorLog = $JoinFile; peerLog = $HostFile; subject = $roles.L1 }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        # Author-side markers (ok=1 - the local engine call must have landed).
        $pk = Select-String -Path $d.authorLog -Pattern ('SCENARIO FURNACT ' + $t + ' put hand=[\d,]+ kind=' + $Kind + ' ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        $ok2 = Select-String -Path $d.authorLog -Pattern ('SCENARIO FURNACT ' + $t + ' out hand=[\d,]+ kind=' + $Kind + ' ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $pk) { $bad += "${t}: author put never landed (no ok=1 marker)"; continue }
        if ($null -eq $ok2) { $bad += "${t}: author out never landed (no ok=1 marker)"; continue }
        $Tp = Get-MarkerTimeMs -File $d.authorLog -Pattern ('SCENARIO FURNACT ' + $t + ' put ')
        $Td = Get-MarkerTimeMs -File $d.authorLog -Pattern ('SCENARIO FURNACT ' + $t + ' out ')
        # Author ground truth: its own local subject really entered the furniture.
        $Aall = Get-FurnSeries -File $d.authorLog -HandIS $d.subject
        $aIn = @($Aall | Where-Object { $_.t -ge $Tp -and $_.t -le ($Td + 1500) -and $_.in -eq $Kind })
        if ($aIn.Count -lt 1) { $bad += "${t}: subject never read in=$Kind on the AUTHOR (engine put failed?)"; continue }
        # 1. Enter crossed: the peer's local copy reads the occupancy.
        $Pall = Get-FurnSeries -File $d.peerLog -HandIS $d.subject
        if ($Pall.Count -lt 3) { $bad += "${t}: peer furn series too short ($($Pall.Count))"; continue }
        $pIn = @($Pall | Where-Object { $_.t -ge $Tp -and $_.in -eq $Kind })
        if ($pIn.Count -lt 1) { $bad += "${t}: enter never crossed to the peer"; continue }
        $enterLat = [int]($pIn[0].t - $Tp)
        $m["${t}EnterLatMs"] = $enterLat
        if ($enterLat -gt $MaxLatencyMs) { $bad += "${t}: enter latency ${enterLat}ms > $MaxLatencyMs" }
        # 2. Held in place: same-tick author<->peer distance while occupied (the
        # fixture owns the transform on both machines, so the copies must agree).
        $gaps = New-Object System.Collections.ArrayList
        foreach ($s in @($pIn | Where-Object { $_.t -le ($Td + 1500) })) {
            $as = @($aIn | Where-Object { $_.rawT -eq $s.rawT })
            if ($as.Count -lt 1) { continue }
            $dx = $s.x - $as[0].x; $dy = $s.y - $as[0].y; $dz = $s.z - $as[0].z
            [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
        }
        if ($gaps.Count -ge 2) {
            $sorted = @($gaps | Sort-Object)
            $med = [double]$sorted[[int][Math]::Floor($sorted.Count / 2)]
            $m["${t}FurnGapMed"] = [Math]::Round($med, 2)
            if ($med -gt $PosTol) { $bad += "${t}: occupied copy sits off the fixture (median gap $([Math]::Round($med,2)) > $PosTol)" }
        } else {
            $m["${t}FurnGapMed"] = -1
        }
        # 3. Exit crossed: the peer's copy leaves the furniture after the out.
        $pOut = @($Pall | Where-Object { $_.t -ge $Td -and $_.in -eq 0 })
        if ($pOut.Count -lt 1) { $bad += "${t}: exit never crossed to the peer"; continue }
        $exitLat = [int]($pOut[0].t - $Td)
        $m["${t}ExitLatMs"] = $exitLat
        if ($exitLat -gt $MaxLatencyMs) { $bad += "${t}: exit latency ${exitLat}ms > $MaxLatencyMs" }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  $tagU FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name $gate -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  $tagU PASS - A: enter $($m.AEnterLatMs)ms exit $($m.AExitLatMs)ms gap $($m.AFurnGapMed); " +
                "B: enter $($m.BEnterLatMs)ms exit $($m.BExitLatMs)ms gap $($m.BFurnGapMed)")
    return (Add-GateResult -Name $gate -Status PASS -Metrics $m)
}

# cage_peer_sync (protocol 36, third-party placement): the HOST places the
# join's KO'd leader (a peer-owned DRIVEN body on the host) into the baked
# cage - the guard-jails-the-join-PC reproduction. Gates:
#   author     - the host authored the third-party edge ([furn] SEND PEER-ENTER
#                for L1's hand); the local put itself landed (FURNACT ok=1).
#   apply      - the JOIN applied the enter to its OWN body ([furn] RECV ENTER
#                occ=L1 ok=1) - the relaxed own-hand path.
#   occupancy  - the join's local L1 reads in=2 within MaxLatencyMs of the
#                host put, and stays in through the hold window.
#   no-eject   - the host NEVER self-heal-ejected the body ([furn] HEAL EXIT
#                for L1 absent) and its own series shows no in=0 dwell inside
#                the hold window - the 2026-07-09 "kept taking it out" bug.
#   exit-clean - after the join's owner-side out, both sides' final series
#                read in=0.
function Test-CagePeer {
    param([string]$HostFile, [string]$JoinFile, [int]$MaxLatencyMs = 12000)
    $mk = Select-String -Path $JoinFile -Pattern 'SCENARIO FURN L1 hand=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mk) {
        Write-Host "  CAGE-PEER FAIL - L1 never latched on the join"
        return (Add-GateResult -Name "cage_peer" -Status FAIL -Detail "no L1 latch")
    }
    $subj = ($mk.Matches[0].Groups[1].Value + ',' + $mk.Matches[0].Groups[2].Value)
    $bad = @(); $m = @{}

    # Author-side markers: the host's put landed, the join's out landed.
    $pk = Select-String -Path $HostFile -Pattern 'SCENARIO FURNACT host put hand=[\d,]+ kind=2 ok=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pk) { $bad += "host put never landed (no ok=1 marker)" }
    $ok2 = Select-String -Path $JoinFile -Pattern 'SCENARIO FURNACT join out hand=[\d,]+ kind=2 ok=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $ok2) { $bad += "join out never landed (no ok=1 marker)" }

    # 1. Third-party authorship on the host.
    $authored = (Select-String -Path $HostFile -Pattern ('\[furn\] SEND PEER-ENTER id=\d+ occ=' + [regex]::Escape($subj)) -Quiet)
    if (-not $authored) { $bad += "host never authored SEND PEER-ENTER for L1" }
    # 2. Own-body apply on the join.
    $applied = (Select-String -Path $JoinFile -Pattern ('\[furn\] RECV ENTER id=\d+ occ=' + [regex]::Escape($subj) + ' .*ok=1') -Quiet)
    if (-not $applied) { $bad += "join never applied the enter to its own body" }

    if ($bad.Count -eq 0) {
        $Tp = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO FURNACT host put '
        $Td = Get-MarkerTimeMs -File $JoinFile -Pattern 'SCENARIO FURNACT join out '
        $J = Get-FurnSeries -File $JoinFile -HandIS $subj
        $H = Get-FurnSeries -File $HostFile -HandIS $subj
        # 3. Occupancy on the JOIN (its own body entered the cage).
        $jIn = @($J | Where-Object { $_.t -ge $Tp -and $_.t -le $Td -and $_.in -eq 2 })
        if ($jIn.Count -lt 1) {
            $bad += "join's own body never read in=2"
        } else {
            $enterLat = [int]($jIn[0].t - $Tp)
            $m["EnterLatMs"] = $enterLat
            if ($enterLat -gt $MaxLatencyMs) { $bad += "enter latency ${enterLat}ms > $MaxLatencyMs" }
        }
        # 4. No eject on the HOST: no self-heal exit for L1, and once its copy
        # entered, no in=0 dwell inside the hold window (2 consecutive samples
        # = ~1 s, well under the old 3 s eject-repark cycle's visibility).
        $healExit = (Select-String -Path $HostFile -Pattern ('\[furn\] HEAL EXIT occ=' + [regex]::Escape($subj)) -Quiet)
        if ($healExit) { $bad += "host self-heal EJECTED the body (HEAL EXIT fired)" }
        $hIn = @($H | Where-Object { $_.t -ge $Tp -and $_.t -le $Td -and $_.in -eq 2 })
        if ($hIn.Count -ge 1) {
            $zeros = 0; $maxZeros = 0
            foreach ($s in @($H | Where-Object { $_.t -ge $jIn[0].t -and $_.t -le ($Td - 1000) })) {
                if ($s.in -eq 0) { $zeros++; if ($zeros -gt $maxZeros) { $maxZeros = $zeros } }
                else { $zeros = 0 }
            }
            $m["HostZeroDwell"] = $maxZeros
            if ($maxZeros -ge 4) { $bad += "host copy left the cage mid-hold ($maxZeros consecutive in=0 samples)" }
        } else {
            $bad += "host copy never read in=2 (put never took?)"
        }
        # 5. Exit clean: both final samples read in=0.
        if ($J.Count -ge 1 -and $J[$J.Count-1].in -ne 0) { $bad += "join's final sample still occupied (in=$($J[$J.Count-1].in))" }
        if ($H.Count -ge 1 -and $H[$H.Count-1].in -ne 0) { $bad += "host's final sample still occupied (in=$($H[$H.Count-1].in))" }
    }

    $v = if ($bad.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $bad -join "; "
    Write-Host "  CAGE-PEER $v - authored=$authored applied=$applied enterLat=$($m.EnterLatMs)ms hostZeroDwell=$($m.HostZeroDwell) $detail"
    return (Add-GateResult -Name "cage_peer" -Status $v `
                -Metrics @{ authored = $authored; applied = $applied
                            enterLatMs = $m.EnterLatMs; hostZeroDwell = $m.HostZeroDwell } -Detail $detail)
}

# sneak_probe (protocol 20 phase 0, host-side spike): the host forced
# stealthMode on a DRIVEN copy near NPCs. Gates: the engine call landed
# (SNEAKACT on ok=1), the mode read back 1 on the copy, and the copy's
# whoSeesMeSneaking accumulated at least one seer entry while the mode was on
# (detection fires against driven copies). Log-only spike - no cross-client
# assertion yet.
function Test-SneakProbe {
    param([string]$HostFile)
    $on = Select-String -Path $HostFile -Pattern 'SCENARIO SNEAKACT on hand=[\d,]+ ok=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $on) {
        Write-Host "  SNEAK-PROBE FAIL - stealth-on never landed (no SNEAKACT on ok=1)"
        return (Add-GateResult -Name "sneak_probe" -Status FAIL -Detail "no SNEAKACT on ok=1")
    }
    $lines = @(Select-String -Path $HostFile -Pattern 'SCENARIO SNEAKPROBE hand=[\d,]+ t=(\d+) mode=(\d) unseen=(\d+) map=(\d+)' -ErrorAction SilentlyContinue)
    $modeOn = 0; $withSeers = 0; $maxMap = 0
    foreach ($l in $lines) {
        $g = $l.Matches[0].Groups
        if ([int]$g[2].Value -eq 1) {
            $modeOn++
            $m = [int]$g[4].Value
            if ($m -gt 0) { $withSeers++ }
            if ($m -gt $maxMap) { $maxMap = $m }
        }
    }
    $ok = ($modeOn -ge 6 -and $withSeers -ge 2)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $why = @()
    if ($modeOn -lt 6) { $why += "driven copy read mode=1 in only $modeOn sample(s) (setStealthMode didn't stick?)" }
    if ($withSeers -lt 2) { $why += "only $withSeers sample(s) with detection entries (NPCs never noticed the driven sneaker)" }
    $detail = $why -join "; "
    Write-Host "  SNEAK-PROBE $v - modeOn=$modeOn withSeers=$withSeers maxMap=$maxMap $detail"
    return (Add-GateResult -Name "sneak_probe" -Status $v `
                -Metrics @{ modeOn = $modeOn; withSeers = $withSeers; maxMap = $maxMap } -Detail $detail)
}

# Parse "SCENARIO SNEAK hand=i,s t=ms mode=d bs=n" lines into per-hand series
# (mirrors Get-FurnSeries). Returns hashtable hand -> list of @{T; Mode; Bs}.
function Get-SneakSeries {
    param([string]$File)
    $series = @{}
    $rx = 'SCENARIO SNEAK hand=(\d+),(\d+) t=(\d+) mode=(-?\d+) bs=(\d+)'
    foreach ($l in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $l.Matches[0].Groups
        $hand = "$($g[1].Value),$($g[2].Value)"
        if (-not $series.ContainsKey($hand)) { $series[$hand] = New-Object System.Collections.ArrayList }
        [void]$series[$hand].Add(@{ T = [long]$g[3].Value; Mode = [int]$g[4].Value; Bs = [int]$g[5].Value })
    }
    return $series
}

# sneak_pose (protocol 20): stealth posture crossing, both directions. For each
# window (A: host L0 10-35 s, B: join L1 45-70 s) assert the PEER's driven copy
# read mode=1 while the owner sneaked (>= MinOn samples, within budget of the
# owner's SNEAKACT edge) and mode=0 again after the off edge.
function Test-SneakPose {
    param([string]$HostFile, [string]$JoinFile, [double]$BudgetS = 6.0)
    $whys = @(); $metrics = @{}
    # Window A: owner = host (L0), peer = join. Window B: owner = join (L1), peer = host.
    $legs = @(
        @{ Name = "A"; OwnerFile = $HostFile; PeerFile = $JoinFile; OnAct = 'A-on'; OffAct = 'A-off' },
        @{ Name = "B"; OwnerFile = $JoinFile; PeerFile = $HostFile; OnAct = 'B-on'; OffAct = 'B-off' }
    )
    foreach ($leg in $legs) {
        $n = $leg.Name
        # The owner's SNEAKACT lines carry the subject hand + define the window edges.
        $onLine = Select-String -Path $leg.OwnerFile -Pattern ('SCENARIO SNEAKACT ' + $leg.OnAct + ' hand=(\d+),(\d+) ok=(\d)') -ErrorAction SilentlyContinue | Select-Object -First 1
        $offLine = Select-String -Path $leg.OwnerFile -Pattern ('SCENARIO SNEAKACT ' + $leg.OffAct + ' hand=[\d,]+ ok=(\d)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $onLine -or $onLine.Matches[0].Groups[3].Value -ne '1') {
            $whys += "$($n): owner never toggled stealth on (no $($leg.OnAct) ok=1)"; continue
        }
        if ($null -eq $offLine -or $offLine.Matches[0].Groups[1].Value -ne '1') {
            $whys += "$($n): owner never toggled stealth off (no $($leg.OffAct) ok=1)"; continue
        }
        $hand = "$($onLine.Matches[0].Groups[1].Value),$($onLine.Matches[0].Groups[2].Value)"
        # Owner-side series brackets the window in the owner's scenario clock;
        # both clocks arm off the same peer-ready moment (aligned to ~1 s).
        $ownSeries = (Get-SneakSeries -File $leg.OwnerFile)[$hand]
        $peerSeries = (Get-SneakSeries -File $leg.PeerFile)[$hand]
        if ($null -eq $ownSeries -or $null -eq $peerSeries) {
            $whys += "$($n): missing SNEAK series for $hand (own=$($null -ne $ownSeries) peer=$($null -ne $peerSeries))"; continue
        }
        $tOn  = ($ownSeries | Where-Object { $_.Mode -eq 1 } | Select-Object -First 1)
        $tOffCandidates = @($ownSeries | Where-Object { $_.Mode -eq 1 })
        if ($null -eq $tOn -or $tOffCandidates.Count -eq 0) {
            $whys += "$($n): owner's own copy never read mode=1 (setStealthMode failed on the owner)"; continue
        }
        $tOnMs  = $tOn.T
        $tOffMs = ($tOffCandidates | Select-Object -Last 1).T
        # Peer: mode=1 samples inside [on+budget, off] and mode=0 after off+budget.
        $budgetMs = [long]($BudgetS * 1000)
        $peerOn = @($peerSeries | Where-Object { $_.Mode -eq 1 -and $_.T -ge $tOnMs -and $_.T -le ($tOffMs + $budgetMs) })
        $firstPeerOn = $peerOn | Select-Object -First 1
        $onLatency = if ($null -ne $firstPeerOn) { ($firstPeerOn.T - $tOnMs) / 1000.0 } else { -1 }
        $peerAfterOff = @($peerSeries | Where-Object { $_.T -ge ($tOffMs + $budgetMs) })
        $stillOn = @($peerAfterOff | Where-Object { $_.Mode -eq 1 })
        $metrics["$($n)_peerOnSamples"] = $peerOn.Count
        $metrics["$($n)_onLatencyS"]   = [math]::Round($onLatency, 2)
        if ($peerOn.Count -lt 4) { $whys += "$($n): peer's driven copy read mode=1 in only $($peerOn.Count) samples (posture never crossed)" }
        elseif ($onLatency -gt $BudgetS) { $whys += "$($n): peer flipped mode=1 only after $onLatency s (> $BudgetS s budget)" }
        if ($peerAfterOff.Count -ge 2 -and $stillOn.Count -gt 0) { $whys += "$($n): peer still read mode=1 in $($stillOn.Count) samples after the off edge (+budget)" }
    }
    $v = if ($whys.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $whys -join "; "
    Write-Host "  SNEAK-POSE $v - $(($metrics.Keys | Sort-Object | ForEach-Object { "$($_)=$($metrics[$_])" }) -join ' ') $detail"
    return (Add-GateResult -Name "sneak_pose" -Status $v -Metrics $metrics -Detail $detail)
}

# sneak_detect (protocol 20): detection-indicator feedback. The JOIN's leader
# sneaks near the bar NPCs; the HOST's world detects its driven copy and
# streams the map back. Gates: the host authored DETECT SEND snapshots, the
# join applied entries between its local pair (DETECT RECV applied>=1), the
# join's local map accumulated entries while sneaking, and it cleared after
# the sneak ended.
function Test-SneakDetect {
    param([string]$HostFile, [string]$JoinFile, [double]$BudgetS = 8.0)
    $whys = @(); $metrics = @{}
    $rxDetect = 'SCENARIO DETECT hand=(\d+),(\d+) t=(\d+) mode=(\d) unseen=(\d+) map=(\d+)'
    $joinLines = @(Select-String -Path $JoinFile -Pattern $rxDetect -ErrorAction SilentlyContinue)
    $hostLines = @(Select-String -Path $HostFile -Pattern $rxDetect -ErrorAction SilentlyContinue)
    if ($joinLines.Count -eq 0) { $whys += "no join DETECT series" }
    if ($hostLines.Count -eq 0) { $whys += "no host DETECT series (driven copy unreadable)" }
    # Host authored snapshots + join applied them (the channel itself).
    $sends = @(Select-String -Path $HostFile -Pattern '\[sneak\] DETECT SEND hand=[\d,]+ seers=(\d+)' -ErrorAction SilentlyContinue | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 0 })
    $recvs = @(Select-String -Path $JoinFile -Pattern '\[sneak\] DETECT RECV hand=[\d,]+ seers=\d+ applied=(\d+)' -ErrorAction SilentlyContinue | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 0 })
    $metrics.hostSends   = $sends.Count
    $metrics.joinApplied = $recvs.Count
    if ($sends.Count -lt 2) { $whys += "host authored only $($sends.Count) non-empty DETECT SEND snapshots (its NPCs never detected the driven sneaker)" }
    if ($recvs.Count -lt 2) { $whys += "join applied entries from only $($recvs.Count) snapshots (feedback channel dead)" }
    if ($joinLines.Count -gt 0) {
        $series = $joinLines | ForEach-Object { $g = $_.Matches[0].Groups; @{ T = [long]$g[3].Value; Mode = [int]$g[4].Value; Map = [int]$g[6].Value } }
        $sneakOn = @($series | Where-Object { $_.Mode -eq 1 })
        $withMap = @($sneakOn | Where-Object { $_.Map -gt 0 })
        $metrics.joinSneakSamples = $sneakOn.Count
        $metrics.joinMapSamples   = $withMap.Count
        if ($sneakOn.Count -lt 4) { $whys += "join's own leader read mode=1 in only $($sneakOn.Count) samples" }
        if ($withMap.Count -lt 2) { $whys += "join's local detection map stayed empty while sneaking ($($withMap.Count) samples with entries)" }
        # After the sneak ends the map must drain (engine ages entries out).
        $offT = ($sneakOn | Select-Object -Last 1).T
        $tail = @($series | Where-Object { $_.T -gt ($offT + [long]($BudgetS * 1000)) })
        if ($tail.Count -ge 2) {
            $lastMaps = @($tail | Select-Object -Last 3 | ForEach-Object { $_.Map })
            if (($lastMaps | Where-Object { $_ -gt 0 }).Count -eq $lastMaps.Count) {
                $whys += "join's detection map never cleared after the sneak ended (last maps: $($lastMaps -join ','))"
            }
        }
    }
    $v = if ($whys.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $whys -join "; "
    Write-Host "  SNEAK-DETECT $v - $(($metrics.Keys | Sort-Object | ForEach-Object { "$($_)=$($metrics[$_])" }) -join ' ') $detail"
    return (Add-GateResult -Name "sneak_detect" -Status $v -Metrics $metrics -Detail $detail)
}

# carry_order (protocol 18): three carry legs - A own-tab (host L0 carries the
# host's downed M2), B cross-tab (join L1 carries the host-owned M2), C
# cross-tab the other direction (host L0 carries the join's downed L1). Gates
# per window:
#   pickup-crossed - the PEER's local pair enters the carried state (the
#                    carried copy's beingCarried flips 1) within MaxLatencyMs
#                    of the author's pickup marker
#   tracks-carrier - while carried on the peer, the carried copy stays glued
#                    to its carrier (median same-tick distance <= MaxCarryGap;
#                    the pre-fix artifact dragged the body meters behind)
#   drop-crossed   - the peer's copy leaves the carried state within budget of
#                    the author's drop marker, and (windows A/B, where no
#                    revive follows) the dropped body still reads DOWN
function Test-CarryOrder {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$MaxCarryGap = 5.0)
    # Subject hands from the latch lines (host log carries all three).
    $roles = @{}
    foreach ($who in @("L0", "M2", "L1")) {
        $mk = Select-String -Path $HostFile -Pattern ('SCENARIO CARRY ' + $who + ' hand=(\d+),(\d+)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) {
            Write-Host "  CARRY-ORDER FAIL - host never latched $who"
            return (Add-GateResult -Name "carry_order" -Status FAIL -Detail "no $who latch")
        }
        $roles[$who] = ($mk.Matches[0].Groups[1].Value + ',' + $mk.Matches[0].Groups[2].Value)
    }
    $dirs = @(
        @{ tag = "A"; authorLog = $HostFile; peerLog = $JoinFile
           carrier = $roles.L0; carried = $roles.M2; checkDown = $true },
        @{ tag = "B"; authorLog = $JoinFile; peerLog = $HostFile
           carrier = $roles.L1; carried = $roles.M2; checkDown = $true },
        @{ tag = "C"; authorLog = $HostFile; peerLog = $JoinFile
           carrier = $roles.L0; carried = $roles.L1; checkDown = $false }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        # Author-side markers (ok=1 - the local engine call must have landed).
        $pk = Select-String -Path $d.authorLog -Pattern ('SCENARIO CARRYACT ' + $t + ' pickup hand=[\d,]+ ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        $dk = Select-String -Path $d.authorLog -Pattern ('SCENARIO CARRYACT ' + $t + ' drop hand=[\d,]+ ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $pk) { $bad += "${t}: author never picked up (no ok=1 marker)"; continue }
        if ($null -eq $dk) { $bad += "${t}: author never dropped (no ok=1 marker)"; continue }
        $Tp = Get-MarkerTimeMs -File $d.authorLog -Pattern ('SCENARIO CARRYACT ' + $t + ' pickup ')
        $Td = Get-MarkerTimeMs -File $d.authorLog -Pattern ('SCENARIO CARRYACT ' + $t + ' drop ')
        # Author ground truth: its own local pair really entered the carried state.
        $Aall = Get-CarrySeries -File $d.authorLog -HandIS $d.carried
        $aIn = @($Aall | Where-Object { $_.t -ge $Tp -and $_.t -le ($Td + 1500) -and $_.beingCarried })
        if ($aIn.Count -lt 1) { $bad += "${t}: carried body never read beingCarried on the AUTHOR (engine pickup failed?)"; continue }
        # 1. Pickup crossed: the peer's local copy enters the carried state.
        $Pall = Get-CarrySeries -File $d.peerLog -HandIS $d.carried
        if ($Pall.Count -lt 3) { $bad += "${t}: peer carry series too short ($($Pall.Count))"; continue }
        $pIn = @($Pall | Where-Object { $_.t -ge $Tp -and $_.beingCarried })
        if ($pIn.Count -lt 1) { $bad += "${t}: pickup never crossed to the peer"; continue }
        $pickLat = [int]($pIn[0].t - $Tp)
        $m["${t}PickLatMs"] = $pickLat
        if ($pickLat -gt $MaxLatencyMs) { $bad += "${t}: pickup latency ${pickLat}ms > $MaxLatencyMs" }
        # 2. Tracks carrier: same-tick (rawT-paired) carrier<->carried distance
        # on the PEER while carried. The local shoulder attach owns the carried
        # transform, so a healthy copy rides within ~1-2u; the pre-fix down-
        # enforcement dragged it meters behind the carrier.
        $Call = Get-CarrySeries -File $d.peerLog -HandIS $d.carrier
        $gaps = New-Object System.Collections.ArrayList
        foreach ($s in @($pIn | Where-Object { $_.t -le ($Td + 1500) })) {
            $cs = @($Call | Where-Object { $_.rawT -eq $s.rawT })
            if ($cs.Count -lt 1) { continue }
            $dx = $s.x - $cs[0].x; $dy = $s.y - $cs[0].y; $dz = $s.z - $cs[0].z
            [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
        }
        if ($gaps.Count -ge 2) {
            $sorted = @($gaps | Sort-Object)
            $med = [double]$sorted[[int][Math]::Floor($sorted.Count / 2)]
            $m["${t}CarryGapMed"] = [Math]::Round($med, 2)
            if ($med -gt $MaxCarryGap) { $bad += "${t}: carried copy trails its carrier (median gap $([Math]::Round($med,2)) > $MaxCarryGap)" }
        } else {
            $m["${t}CarryGapMed"] = -1
        }
        # 3. Drop crossed: the peer's copy leaves the carried state after the
        # author's drop; where no revive follows (A/B) it must still read DOWN
        # (bs & 7: DOWN|RAGDOLL|DEAD) - dropped, not teleport-stood.
        $pOut = @($Pall | Where-Object { $_.t -ge $Td -and (-not $_.beingCarried) })
        if ($pOut.Count -lt 1) { $bad += "${t}: drop never crossed to the peer"; continue }
        $dropLat = [int]($pOut[0].t - $Td)
        $m["${t}DropLatMs"] = $dropLat
        if ($dropLat -gt $MaxLatencyMs) { $bad += "${t}: drop latency ${dropLat}ms > $MaxLatencyMs" }
        if ($d.checkDown -and (($pOut[0].bs -band 7) -eq 0)) {
            $bad += "${t}: dropped body not DOWN on the peer (bs=$($pOut[0].bs))"
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  CARRY-ORDER FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "carry_order" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  CARRY-ORDER PASS - A: pick $($m.APickLatMs)ms drop $($m.ADropLatMs)ms gap $($m.ACarryGapMed); " +
                "B: pick $($m.BPickLatMs)ms drop $($m.BDropLatMs)ms gap $($m.BCarryGapMed); " +
                "C: pick $($m.CPickLatMs)ms drop $($m.CDropLatMs)ms gap $($m.CCarryGapMed)")
    return (Add-GateResult -Name "carry_order" -Status PASS -Metrics $m)
}

# npc_carry (protocol 18, world-NPC carrier extension): the HOST directs a world
# NPC to carry its downed M2; the JOIN must execute the pickup on its LOCAL NPC
# copy. Single window, same three gates as a carry_order direction:
#   pickup-crossed - the join's M2 copy reads beingCarried within budget, AND
#                    the join DETECTED a local carrier (its own NPC latch line -
#                    the join only latches an NPC it finds carrying M2 locally)
#   tracks-carrier - median join-side carrier<->carried distance stays small
#   drop-crossed   - the join's M2 copy leaves the carried state within budget
#                    of the host's drop and still reads DOWN
function Test-NpcCarry {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$MaxCarryGap = 5.0)
    $mk = Select-String -Path $HostFile -Pattern 'SCENARIO CARRY M2 hand=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mk) {
        Write-Host "  NPC-CARRY FAIL - host never latched M2"
        return (Add-GateResult -Name "npc_carry" -Status FAIL -Detail "no M2 latch")
    }
    $m2 = ($mk.Matches[0].Groups[1].Value + ',' + $mk.Matches[0].Groups[2].Value)
    $nk = Select-String -Path $HostFile -Pattern 'SCENARIO CARRY NPC hand=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $nk) {
        Write-Host "  NPC-CARRY FAIL - host never latched a world-NPC carrier"
        return (Add-GateResult -Name "npc_carry" -Status FAIL -Detail "no host NPC latch")
    }
    $m = @{}; $bad = @()
    # Author-side markers (ok=1 - the local engine call must have landed).
    $pk = Select-String -Path $HostFile -Pattern 'SCENARIO CARRYACT N pickup hand=[\d,]+ ok=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    $dk = Select-String -Path $HostFile -Pattern 'SCENARIO CARRYACT N drop hand=[\d,]+ ok=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pk) { $bad += "host NPC pickup never landed (no ok=1 marker)" }
    if ($null -eq $dk) { $bad += "host NPC drop never landed (no ok=1 marker)" }
    if ($bad.Count -eq 0) {
        $Tp = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO CARRYACT N pickup '
        $Td = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO CARRYACT N drop '
        # Author ground truth: the host's local pair really entered the carried state.
        $Aall = Get-CarrySeries -File $HostFile -HandIS $m2
        $aIn = @($Aall | Where-Object { $_.t -ge $Tp -and $_.t -le ($Td + 1500) -and $_.beingCarried })
        if ($aIn.Count -lt 1) { $bad += "M2 never read beingCarried on the HOST (engine pickup failed?)" }
        # 1. Pickup crossed: the join's M2 copy enters the carried state, and the
        # join detected the carrier locally (its own NPC latch line).
        $Pall = Get-CarrySeries -File $JoinFile -HandIS $m2
        if ($bad.Count -eq 0 -and $Pall.Count -lt 3) { $bad += "join M2 carry series too short ($($Pall.Count))" }
        if ($bad.Count -eq 0) {
            $pIn = @($Pall | Where-Object { $_.t -ge $Tp -and $_.beingCarried })
            if ($pIn.Count -lt 1) { $bad += "pickup never crossed to the join" }
            else {
                $pickLat = [int]($pIn[0].t - $Tp)
                $m.PickLatMs = $pickLat
                if ($pickLat -gt $MaxLatencyMs) { $bad += "pickup latency ${pickLat}ms > $MaxLatencyMs" }
                # 2. Tracks carrier: the join's OWN carrier latch (its local hand may
                # legitimately differ from the host key), same-tick paired distances.
                $jk = Select-String -Path $JoinFile -Pattern 'SCENARIO CARRY NPC hand=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($null -eq $jk) { $bad += "join never detected a local NPC carrying M2" }
                else {
                    $jnpc = ($jk.Matches[0].Groups[1].Value + ',' + $jk.Matches[0].Groups[2].Value)
                    $m.JoinCarrier = $jnpc
                    $Call = Get-CarrySeries -File $JoinFile -HandIS $jnpc
                    $gaps = New-Object System.Collections.ArrayList
                    foreach ($s in @($pIn | Where-Object { $_.t -le ($Td + 1500) })) {
                        $cs = @($Call | Where-Object { $_.rawT -eq $s.rawT })
                        if ($cs.Count -lt 1) { continue }
                        $dx = $s.x - $cs[0].x; $dy = $s.y - $cs[0].y; $dz = $s.z - $cs[0].z
                        [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
                    }
                    if ($gaps.Count -ge 2) {
                        $sorted = @($gaps | Sort-Object)
                        $med = [double]$sorted[[int][Math]::Floor($sorted.Count / 2)]
                        $m.CarryGapMed = [Math]::Round($med, 2)
                        if ($med -gt $MaxCarryGap) { $bad += "carried copy trails its carrier (median gap $([Math]::Round($med,2)) > $MaxCarryGap)" }
                    } else {
                        $m.CarryGapMed = -1
                    }
                }
                # 3. Drop crossed: leaves the carried state after the host's drop,
                # still DOWN (bs & 7: DOWN|RAGDOLL|DEAD) - dropped, not stood.
                $pOut = @($Pall | Where-Object { $_.t -ge $Td -and (-not $_.beingCarried) })
                if ($pOut.Count -lt 1) { $bad += "drop never crossed to the join" }
                else {
                    $dropLat = [int]($pOut[0].t - $Td)
                    $m.DropLatMs = $dropLat
                    if ($dropLat -gt $MaxLatencyMs) { $bad += "drop latency ${dropLat}ms > $MaxLatencyMs" }
                    if (($pOut[0].bs -band 7) -eq 0) { $bad += "dropped body not DOWN on the join (bs=$($pOut[0].bs))" }
                }
            }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  NPC-CARRY FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "npc_carry" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  NPC-CARRY PASS - pick $($m.PickLatMs)ms drop $($m.DropLatMs)ms gap $($m.CarryGapMed) carrier=$($m.JoinCarrier)")
    return (Add-GateResult -Name "npc_carry" -Status PASS -Metrics $m)
}

# player_combat BIDIRECTIONAL (armed NPC strikers vs the two tab leaders): the
# striker's combat intent must be APPLIED by the join's replicator against the
# right victim, and each window's victim must lose blood on ITS OWNER (medical
# resolves on the victim's owner: join for window A, host for window B).
# Also measures the victim's rendered-copy blood drop on the OTHER side: the
# divergence the host-side damage guard + vitals sync (phase 2) close
# (recorded, not gated in phase 1).
function Test-PlayerCombat {
    # MinIntent=1: ONE applied order is the HEALTHY case - the join's replicator
    # deliberately re-issues only while the local fight is broken (run 011229:
    # a green window applied exactly 1 order and drew blood). The blood gate
    # below independently proves a real fight followed the order.
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinIntent = 1, [double]$MinDrop = 3.0, [double]$MaxEndGap = 12.0)
    # Both windows are issued host-side (world-NPC strikers are host-owned):
    #   A: NPC-A melees the JOIN's leader  - victim owned by the JOIN
    #   B: NPC-B melees the HOST's leader  - victim owned by the HOST
    $mA = Select-String -Path $HostFile -Pattern 'SCENARIO PCOMBAT A issued atk=(\d+),(\d+) vic=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    $mB = Select-String -Path $HostFile -Pattern 'SCENARIO PCOMBAT B issued atk=(\d+),(\d+) vic=(\d+),(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mA) { Write-Host "  PLAYER-COMBAT FAIL - host never issued window A (no striker/victim)"; return (Add-GateResult -Name "player_combat" -Status FAIL -Detail "no A order") }
    if ($null -eq $mB) { Write-Host "  PLAYER-COMBAT FAIL - host never issued window B (no striker/victim)"; return (Add-GateResult -Name "player_combat" -Status FAIL -Detail "no B order") }
    $TA = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO PCOMBAT A issued'
    $TB = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO PCOMBAT B issued'

    # peerLog = where the striker's RECV series proves intent crossed (always the
    # join: strikers are host-owned). ownerLog = the victim's owner (authoritative
    # damage). copyLog = the other side's rendered copy (divergence metric only in
    # phase 1; becomes a convergence gate after phase-2 vitals sync).
    $dirs = @(
        @{ tag = "A"; T = $TA; atk = ($mA.Matches[0].Groups[1].Value + ',' + $mA.Matches[0].Groups[2].Value)
           vic = ($mA.Matches[0].Groups[3].Value + ',' + $mA.Matches[0].Groups[4].Value)
           peerLog = $JoinFile; ownerLog = $JoinFile; copyLog = $HostFile },
        @{ tag = "B"; T = $TB; atk = ($mB.Matches[0].Groups[1].Value + ',' + $mB.Matches[0].Groups[2].Value)
           vic = ($mB.Matches[0].Groups[3].Value + ',' + $mB.Matches[0].Groups[4].Value)
           peerLog = $JoinFile; ownerLog = $HostFile; copyLog = $JoinFile }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        # 1. Intent crossed: the JOIN's replicator APPLIED a striker's combat
        # order against the right victim ("[combat] order hand=... tgt=<vic>").
        # This is the direct wire-level proof; the driven copy's sampled local
        # task is unreliable (the local engine reports transient fight sub-tasks,
        # measured task=87/65535 mid-fight in the first sync run). ANY striker
        # hand counts: the scenario replaces a striker the bar brawl KOs.
        $ip  = '\[combat\] order hand=\d+,\d+ tgt=' + $d.vic + ' '
        $int = @(Select-String -Path $d.peerLog -Pattern $ip -ErrorAction SilentlyContinue).Count
        # 2. Authoritative damage: the victim's blood drops on its OWNER's side.
        # NB: assign-then-filter - piping Get-VitalsSeries directly would pass its
        # comma-wrapped ArrayList through Where-Object as ONE opaque item.
        $ownerDrop = $null
        $Vall = Get-VitalsSeries -File $d.ownerLog -HandIS $d.vic
        $V = @($Vall | Where-Object { $_.t -ge $d.T })
        if ($V.Count -ge 2) {
            $ownerDrop = [double]($V | Measure-Object -Property blood -Maximum).Maximum -
                         [double]($V | Measure-Object -Property blood -Minimum).Minimum
        }
        # 3. Convergence gate (GATING since phase 2 - vitals sync + host-side
        # damage guard): the victim's rendered copy on the OTHER side must TRACK
        # the owner's blood (the stream writes it). Compare at the end of the
        # OVERLAP window, not last-vs-last: the two scenarios end ~6 s apart
        # (peer-ready arming skew + different durations) and the owner keeps
        # bleeding after the copy's log stops - run 014713 measured a fake 38.3
        # "gap" while the aligned samples agreed to 0.0. Vitals t is already on
        # the common clock (Get-VitalsSeries applies the CLOCKSYNC offset).
        $copyDrop = $null; $endGap = $null
        $Call = Get-VitalsSeries -File $d.copyLog -HandIS $d.vic
        $C = @($Call | Where-Object { $_.t -ge $d.T })
        if ($C.Count -ge 2) {
            $copyDrop = [double]($C | Measure-Object -Property blood -Maximum).Maximum -
                        [double]($C | Measure-Object -Property blood -Minimum).Minimum
        }
        if ($V.Count -ge 1 -and $C.Count -ge 1) {
            $tEnd = [Math]::Min([double]$V[-1].t, [double]$C[-1].t)
            $vAt = @($V | Where-Object { $_.t -le ($tEnd + 750) })
            $cAt = @($C | Where-Object { $_.t -le ($tEnd + 750) })
            if ($vAt.Count -ge 1 -and $cAt.Count -ge 1) {
                $endGap = [Math]::Abs([double]$vAt[-1].blood - [double]$cAt[-1].blood)
            }
        }
        $m["${t}Intent"]    = $int
        $m["${t}OwnerDrop"] = if ($null -ne $ownerDrop) { [Math]::Round($ownerDrop,1) } else { -1 }
        $m["${t}CopyDrop"]  = if ($null -ne $copyDrop)  { [Math]::Round($copyDrop,1) }  else { -1 }
        $m["${t}EndGap"]    = if ($null -ne $endGap)    { [Math]::Round($endGap,1) }    else { -1 }
        if ($int -lt $MinIntent) { $bad += "${t}: join applied $int combat order(s) for striker $($d.atk) -> $($d.vic) (need >= $MinIntent)" }
        if ($null -eq $ownerDrop -or $ownerDrop -lt $MinDrop) { $bad += "${t}: victim $($d.vic) owner-side blood drop=$($m["${t}OwnerDrop"]) (need >= $MinDrop - no authoritative damage landed)" }
        if ($null -eq $endGap -or $endGap -gt $MaxEndGap) { $bad += "${t}: victim $($d.vic) copy/owner final blood gap=$($m["${t}EndGap"]) (need <= $MaxEndGap - vitals sync not converging)" }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  PLAYER-COMBAT FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "player_combat" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  PLAYER-COMBAT PASS - A: $($m.AIntent) applied orders, victim owner-drop $($m.AOwnerDrop) (copy $($m.ACopyDrop), end-gap $($m.AEndGap)); " +
                "B: $($m.BIntent) applied orders, victim owner-drop $($m.BOwnerDrop) (copy $($m.BCopyDrop), end-gap $($m.BEndGap))")
    return (Add-GateResult -Name "player_combat" -Status PASS -Metrics $m)
}

# player_ko BIDIRECTIONAL: each side KOs then revives its OWN squad member; the
# KO and revive must cross as reliable events (EVT_KNOCKOUT=1 / EVT_REVIVE=3,
# SEND on the owner, RECV on the peer) and the peer's driven copy must lie down
# between the edges and stand after the revive.
function Test-PlayerKo {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 5000, [double]$MinRatio = 0.60)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; peerLog = $JoinFile; peerKind = "RECV" },
        @{ tag = "B"; ownerLog = $JoinFile; peerLog = $HostFile; peerKind = "RECV" }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $mk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO PKO ' + $t + ' down issued hand=(\d+),(\d+)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $mk) { $bad += "${t}: no down marker"; continue }
        $hi = $mk.Matches[0].Groups[1].Value; $hs = $mk.Matches[0].Groups[2].Value
        $Tdown = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO PKO ' + $t + ' down issued')
        $Trev  = Get-MarkerTimeMs -File $d.ownerLog -Pattern ('SCENARIO PKO ' + $t + ' revive issued')
        if ($null -eq $Trev) { $bad += "${t}: no revive marker"; continue }
        # Reliable edges: KO (ev=1) + revive (ev=3) sent by the owner, received by the peer.
        $ev = {
            param($evId, $sinceMs)
            $sp = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] SEND id=\d+ ev=' + $evId + ' hand=\d+,\d+,\d+,' + $hi + ',' + $hs
            $rp = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[event\] RECV id=\d+ ev=' + $evId + ' .*hand=\d+,\d+,\d+,' + $hi + ',' + $hs
            $so = Get-LogClockOffsetMs -File $d.ownerLog; $ro = Get-LogClockOffsetMs -File $d.peerLog
            $send = $null
            foreach ($x in @(Select-String -Path $d.ownerLog -Pattern $sp -ErrorAction SilentlyContinue)) {
                $ts = Convert-StampToMs -Groups $x.Matches[0].Groups -OffsetMs $so
                if ($ts -ge $sinceMs) { $send = $ts; break }
            }
            if ($null -eq $send) { return $null }
            foreach ($x in @(Select-String -Path $d.peerLog -Pattern $rp -ErrorAction SilentlyContinue)) {
                $tr = Convert-StampToMs -Groups $x.Matches[0].Groups -OffsetMs $ro
                if ($tr -ge $send - 1000) { return ($tr - $send) }
            }
            return -1
        }
        $koLat  = & $ev 1 ($Tdown - 2000)
        $revLat = & $ev 3 ($Trev - 2000)
        if ($null -eq $koLat)  { $bad += "${t}: owner sent no EVT_KNOCKOUT for hand=$hi,$hs" }
        elseif ($koLat -lt 0)  { $bad += "${t}: peer never received the KO event" }
        if ($null -eq $revLat) { $bad += "${t}: owner sent no EVT_REVIVE for hand=$hi,$hs" }
        elseif ($revLat -lt 0) { $bad += "${t}: peer never received the revive event" }
        # Peer pose: driven copy down between the edges, upright after the revive.
        $S = Get-ScenarioSeries -File $d.peerLog -Kind $d.peerKind
        $key = $null
        foreach ($hand in $S.Keys) { if ($hand -match ('^' + $hi + ',' + $hs + ',')) { $key = $hand; break } }
        if ($null -eq $key) { $bad += "${t}: peer logged no series for hand=$hi,$hs"; continue }
        $downWin = @($S[$key] | Where-Object { $_.t -ge ($Tdown + $GraceMs) -and $_.t -lt $Trev })
        $upWin   = @($S[$key] | Where-Object { $_.t -ge ($Trev + $GraceMs) })
        if ($downWin.Count -lt 1 -or $upWin.Count -lt 1) { $bad += "${t}: insufficient peer samples (down=$($downWin.Count) up=$($upWin.Count))"; continue }
        $downR = [Math]::Round((@($downWin | Where-Object { ($_.bs -band 7) -ne 0 }).Count) / $downWin.Count, 3)
        $upR   = [Math]::Round((@($upWin   | Where-Object { ($_.bs -band 7) -eq 0 }).Count) / $upWin.Count, 3)
        $m["${t}KoLatMs"] = $koLat; $m["${t}RevLatMs"] = $revLat
        $m["${t}DownRatio"] = $downR; $m["${t}UpRatio"] = $upR
        if ($downR -lt $MinRatio) { $bad += "${t}: peer down-ratio $downR < $MinRatio between the edges" }
        if ($upR   -lt $MinRatio) { $bad += "${t}: peer upright-ratio $upR < $MinRatio after the revive" }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  PLAYER-KO FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "player_ko" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  PLAYER-KO PASS - A(host victim): KO lat=$($m.AKoLatMs)ms revive lat=$($m.ARevLatMs)ms down=$($m.ADownRatio) up=$($m.AUpRatio); " +
                "B(join victim): KO lat=$($m.BKoLatMs)ms revive lat=$($m.BRevLatMs)ms down=$($m.BDownRatio) up=$($m.BUpRatio)")
    return (Add-GateResult -Name "player_ko" -Status PASS -Metrics $m)
}

# medic_order BIDIRECTIONAL: the owner wounds its own member (limb flesh + blood),
# the OTHER player bandages the driven copy it renders. Judged per direction:
#   wound-crossed    - the HEALER's copy shows the wound (fleshMin dropped)
#   heal-happened    - the healer's bandage found damaged limbs (n>0)
#   treatment-crossed- the OWNER's body shows the bandaging afterwards
# All three require the medical replication features (vitals sync + treatment
# forwarding); without them this documents spikes 21-23's truth (nothing crosses).
function Test-MedicOrder {
    param([string]$HostFile, [string]$JoinFile, [int]$GraceMs = 5000,
          [double]$WoundFlesh = 60.0, [double]$MinBandRise = 50.0)
    $dirs = @(
        @{ tag = "A"; ownerLog = $HostFile; healerLog = $JoinFile },
        @{ tag = "B"; ownerLog = $JoinFile; healerLog = $HostFile }
    )
    $m = @{}; $bad = @()
    foreach ($d in $dirs) {
        $t = $d.tag
        $wk = Select-String -Path $d.ownerLog -Pattern ('SCENARIO MEDIC ' + $t + ' wound issued hand=(\d+),(\d+) ok=1') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $wk) { $bad += "${t}: no wound marker"; continue }
        $hi = $wk.Matches[0].Groups[1].Value; $hs = $wk.Matches[0].Groups[2].Value
        $handIS = "$hi,$hs"
        $Tw = Get-MarkerTimeMs -File $d.ownerLog  -Pattern ('SCENARIO MEDIC ' + $t + ' wound issued')
        $hk = Select-String -Path $d.healerLog -Pattern ('SCENARIO MEDIC ' + $t + ' heal issued hand=\d+,\d+ n=(\d+)') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $hk) { $bad += "${t}: no heal marker on the healer"; continue }
        $healN = [int]$hk.Matches[0].Groups[1].Value
        $Th = Get-MarkerTimeMs -File $d.healerLog -Pattern ('SCENARIO MEDIC ' + $t + ' heal issued')

        # Owner ground truth: the wound landed on the authoritative body.
        $O = Get-VitalsSeries -File $d.ownerLog -HandIS $handIS
        $ownerWounded = @($O | Where-Object { $_.t -ge $Tw -and $null -ne $_.fleshMin -and $_.fleshMin -le $WoundFlesh }).Count -gt 0
        if (-not $ownerWounded) { $bad += "${t}: wound never visible on the owner (scaffold failed?)"; continue }
        # 1. Wound crossed: the HEALER's driven copy shows the wound before the heal.
        $H = Get-VitalsSeries -File $d.healerLog -HandIS $handIS
        $crossWound = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.fleshMin -and $_.fleshMin -le $WoundFlesh }).Count -gt 0
        # 2. Heal happened: the bandage pass found damaged limbs on the driven copy.
        $healed = ($healN -gt 0)
        # 3. Treatment crossed: the OWNER's bandaging rises after the heal.
        $preBand  = @($O | Where-Object { $_.t -lt $Th -and $null -ne $_.bandSum })
        $postBand = @($O | Where-Object { $_.t -ge ($Th + $GraceMs) -and $null -ne $_.bandSum })
        $bandRise = $null
        if ($preBand.Count -gt 0 -and $postBand.Count -gt 0) {
            $bandRise = [double]($postBand | Measure-Object -Property bandSum -Maximum).Maximum -
                        [double]($preBand  | Measure-Object -Property bandSum -Maximum).Maximum
        }
        $treated = ($null -ne $bandRise -and $bandRise -ge $MinBandRise)
        $m["${t}WoundCrossed"] = $crossWound; $m["${t}HealN"] = $healN
        $m["${t}BandRise"] = if ($null -ne $bandRise) { [Math]::Round($bandRise,1) } else { -1 }
        if (-not $crossWound) { $bad += "${t}: wound never reached the healer's copy (vitals don't cross)" }
        if (-not $healed)     { $bad += "${t}: healer found nothing to bandage (n=0 - its copy is pristine)" }
        if (-not $treated)    { $bad += "${t}: treatment never reached the owner (bandRise=$($m["${t}BandRise"]))" }
        # 4. FULL-ANATOMY crossing (protocol 16): the wound scaffold now lowers
        # head/chest/stomach AND the stun track; the healer's copy must show
        # both (pfl/pst = min flesh/stun across ALL parts). Gated only when the
        # extended fields are present (older logs skip).
        $ownerP = @($O | Where-Object { $_.t -ge $Tw -and $null -ne $_.pfl })
        if ($ownerP.Count -gt 0) {
            $crossPart = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.pfl -and $_.pfl -le $WoundFlesh }).Count -gt 0
            $crossStun = @($H | Where-Object { $_.t -ge ($Tw + $GraceMs) -and $null -ne $_.pst -and $_.pst -le ($WoundFlesh + 20) }).Count -gt 0
            $m["${t}PartCrossed"] = $crossPart; $m["${t}StunCrossed"] = $crossStun
            if (-not $crossPart) { $bad += "${t}: full-anatomy wound (head/chest) never reached the healer's copy" }
            if (-not $crossStun) { $bad += "${t}: stun damage never reached the healer's copy" }
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host ("  MEDIC-ORDER FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "medic_order" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  MEDIC-ORDER PASS - A: wound crossed, bandaged n=$($m.AHealN), owner bandRise=$($m.ABandRise); " +
                "B: wound crossed, bandaged n=$($m.BHealN), owner bandRise=$($m.BBandRise)")
    return (Add-GateResult -Name "medic_order" -Status PASS -Metrics $m)
}

# speed_sync (consensus game speed): both clients must render the SAME effective
# speed, arbitrated as min(requests) - a raise needs BOTH players, a lowering
# needs either, combat caps at 1x. Parses each side's "SCENARIO SPEED t=..
# mult=.. paused=.." series (~2 Hz, CLOCKSYNC-corrected into the host clock
# frame; effective = 0 when paused) plus the HOST's arbitration markers
# ("[speed] SET mult=X paused=N combat=N (my=..." - logged only on CHANGE).
# NB: loop variables deliberately avoid $h/$j/$s - PowerShell variable names
# are case-INSENSITIVE, so `foreach ($h in $H)` silently DESTROYS $H (first
# run of this oracle: every nearest-sample lookup after the first came up
# empty because $H had become one sample).
# Gates:
#   transitions   - the host arbitrated >= MinTransitions speed CHANGES (the
#                   scenario drives 1x seed -> 3x -> 1x -> 3x -> combat 1x),
#                   including >= 1 combat demotion (combat=1 at mult=1)
#   denied raise  - the HOST's lone 3x click must NOT produce a 3x SET before
#                   the JOIN's first 3x click (min semantics: both must raise)
#   follow        - after each transition the JOIN's series reaches the new
#                   multiplier within MaxFollowMs (2 Hz sampling + reliable
#                   delivery; WAN 'bad' adds ~120 ms + retransmits)
#   match         - of the join samples OUTSIDE +/-TransitionWinMs of every
#                   transition (and inside the host series' time range), the
#                   fraction whose multiplier equals the nearest-in-time host
#                   sample (within 600 ms) is >= MinMatch
#   combat window - for [Tc+2s, Tc+8s] after the combat demotion, BOTH series
#                   sit at 1x for >= MinMatch of their samples
function Test-SpeedSync {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinMatch = 0.80, [int]$MaxFollowMs = 2500,
          [int]$TransitionWinMs = 2000, [int]$MinTransitions = 4)
    $series = {
        param($file)
        $off = Get-LogClockOffsetMs -File $file
        $list = New-Object System.Collections.ArrayList
        $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SPEED t=\d+ mult=([\d\.]+) paused=(\d)'
        foreach ($mi in (Select-String -Path $file -Pattern $pat -ErrorAction SilentlyContinue)) {
            $gg = $mi.Matches[0].Groups
            $tt = Convert-StampToMs -Groups $gg -OffsetMs $off
            $mu = [double]$gg[5].Value
            if ([int]$gg[6].Value -eq 1) { $mu = 0.0 }
            [void]$list.Add([pscustomobject]@{ t = $tt; mult = $mu })
        }
        return ,$list
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 10 -or $J.Count -lt 10) {
        Write-Host "  SPEED-SYNC FAIL - insufficient SPEED series (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "speed_sync" -Status FAIL -Metrics @{ hostSamples = $H.Count; joinSamples = $J.Count } -Detail "insufficient SPEED series")
    }
    # Host arbitration markers (change-only): time + effective + combat bit.
    $sets = New-Object System.Collections.ArrayList
    $hoff = Get-LogClockOffsetMs -File $HostFile
    $sp = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[speed\] SET mult=([\d\.]+) paused=(\d) combat=(\d) \(my='
    foreach ($mi in (Select-String -Path $HostFile -Pattern $sp -ErrorAction SilentlyContinue)) {
        $gg = $mi.Matches[0].Groups
        $mu = [double]$gg[5].Value
        if ([int]$gg[6].Value -eq 1) { $mu = 0.0 }
        [void]$sets.Add([pscustomobject]@{
            t = (Convert-StampToMs -Groups $gg -OffsetMs $hoff)
            mult = $mu; combat = [int]$gg[7].Value })
    }
    $m = @{ transitions = $sets.Count; hostSamples = $H.Count; joinSamples = $J.Count }
    $bad = @()
    if ($sets.Count -lt $MinTransitions) {
        $bad += "only $($sets.Count) arbitrated speed change(s) (need >= $MinTransitions - scenario clicks not detected?)"
    }
    $combatSet = @($sets | Where-Object { $_.combat -eq 1 -and $_.mult -le 1.01 -and $_.mult -ge 0.99 }) | Select-Object -First 1
    if ($null -eq $combatSet) { $bad += "no combat demotion (no SET with combat=1 mult=1)" }

    # Denied raise: min semantics say the host's lone 3x click (T+10) must not
    # change the effective speed; the first 3x SET must come at/after the
    # JOIN's first 3x click.
    $tHostClick = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO SPEEDSYNC host click mult=3'
    $tJoinClick = Get-MarkerTimeMs -File $JoinFile -Pattern 'SCENARIO SPEEDSYNC join click mult=3'
    $first3 = @($sets | Where-Object { [Math]::Abs([double]$_.mult - 3.0) -le 0.01 }) | Select-Object -First 1
    if ($null -ne $tHostClick -and $null -ne $first3) {
        if ($null -eq $tJoinClick -or [double]$first3.t -lt ([double]$tJoinClick - 1000)) {
            $bad += "host's lone 3x click propagated (3x SET at t=$([long]$first3.t) before the join's 3x click - min arbitration broken)"
        } else {
            $m.deniedRaise = $true
        }
    }

    # Same-value vote (the stuck-vote fix): the join clicks 1x while the
    # effective is ALREADY 1x (its own stale vote is 3x). Engine state doesn't
    # change, so only hook-captured intent can see it - the host must log a
    # REQ RECV mult=1.00 shortly after, and the host's later re-raise click
    # must NOT produce a 3x SET before the join's own second 3x click.
    $tSame = Get-MarkerTimeMs -File $JoinFile -Pattern 'SCENARIO SPEEDSYNC join click mult=1\.0 ok=1 tag=samevalue'
    if ($null -ne $tSame) {
        $gotVote = $false
        $rp = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[speed\] REQ RECV owner=\d+ mult=1\.00'
        foreach ($mi in (Select-String -Path $HostFile -Pattern $rp -ErrorAction SilentlyContinue)) {
            $tt = Convert-StampToMs -Groups $mi.Matches[0].Groups -OffsetMs $hoff
            if ($tt -ge ($tSame - 500) -and $tt -le ($tSame + 4000)) { $gotVote = $true; break }
        }
        if (-not $gotVote) { $bad += "same-value 1x click never arrived as a vote (no host REQ RECV mult=1.00 within 4s - stuck-vote bug back)" }
        else { $m.sameValueVote = $true }
        $tReraise = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO SPEEDSYNC host click mult=3\.0 ok=1 tag=reraise'
        $tRaise2  = Get-MarkerTimeMs -File $JoinFile -Pattern 'SCENARIO SPEEDSYNC join click mult=3\.0 ok=1 tag=raise2'
        if ($null -ne $tReraise) {
            $limit = if ($null -ne $tRaise2) { [double]$tRaise2 - 1000 } else { [double]::MaxValue }
            $leak = @($sets | Where-Object { [Math]::Abs([double]$_.mult - 3.0) -le 0.01 -and
                                             [double]$_.t -ge [double]$tReraise -and [double]$_.t -lt $limit }) | Select-Object -First 1
            if ($null -ne $leak) { $bad += "host re-raise propagated to 3x at t=$([long]$leak.t) despite the join's lowered vote (min arbitration ignored the same-value vote)" }
            else { $m.reraiseDenied = $true }
        }
    }

    # Follow latency: after each transition the join must render the new mult.
    # Transitions OUTSIDE the join's logging window skip: before it started
    # (the pre-arm 1x seed) and too close to / after its end (the host's tail
    # runs longer; a post-combat re-raise landing after the join's last sample
    # is unjudgeable - 2026-07-08 flake).
    $joinT0 = [double]$J[0].t
    $joinT1 = [double]$J[$J.Count - 1].t
    $lats = @()
    for ($i = 0; $i -lt $sets.Count; $i++) {
        $st = $sets[$i]
        if ([double]$st.t -lt $joinT0) { continue }
        if ([double]$st.t -gt ($joinT1 - $MaxFollowMs)) { continue }
        $tNext = if ($i + 1 -lt $sets.Count) { [double]$sets[$i + 1].t } else { [double]::MaxValue }
        $win = @($J | Where-Object { [double]$_.t -ge [double]$st.t -and [double]$_.t -lt $tNext })
        $hit = @($win | Where-Object { [Math]::Abs([double]$_.mult - [double]$st.mult) -le 0.01 }) | Select-Object -First 1
        if ($null -eq $hit) {
            $bad += "join never reached mult=$($st.mult) after the transition at t=$([long]$st.t)"
        } else {
            $lat = [double]$hit.t - [double]$st.t
            $lats += $lat
            if ($lat -gt $MaxFollowMs) { $bad += "transition to mult=$($st.mult): join follow latency $([Math]::Round($lat))ms > ${MaxFollowMs}ms" }
        }
    }
    if ($lats.Count -gt 0) { $m.maxFollowMs = [Math]::Round(($lats | Measure-Object -Maximum).Maximum) }

    # Steady-state match fraction (join vs nearest host sample within 600 ms),
    # outside the transition windows.
    $consider = 0; $match = 0
    $hMin = [double]$H[0].t; $hMax = [double]$H[$H.Count - 1].t
    foreach ($js in $J) {
        $tj = [double]$js.t
        if ($tj -lt $hMin -or $tj -gt $hMax) { continue }
        $inTrans = $false
        foreach ($st in $sets) { if ([Math]::Abs($tj - [double]$st.t) -le $TransitionWinMs) { $inTrans = $true; break } }
        if ($inTrans) { continue }
        $near = $null; $nd = 600.0
        foreach ($hs in $H) {
            $dd = [Math]::Abs([double]$hs.t - $tj)
            if ($dd -le $nd) { $nd = $dd; $near = $hs }
        }
        if ($null -eq $near) { continue }
        $consider++
        if ([Math]::Abs([double]$near.mult - [double]$js.mult) -le 0.01) { $match++ }
    }
    $frac = if ($consider -gt 0) { [Math]::Round($match / $consider, 3) } else { 0 }
    $m.matchFrac = $frac; $m.matchSamples = $consider
    if ($consider -lt 10)        { $bad += "only $consider aligned steady-state samples (need >= 10)" }
    elseif ($frac -lt $MinMatch) { $bad += "join matched the host multiplier for $frac of aligned samples (need >= $MinMatch)" }

    # Combat window: both sides at 1x shortly after the demotion.
    if ($null -ne $combatSet) {
        $c0 = [double]$combatSet.t + 2000; $c1 = [double]$combatSet.t + 8000
        foreach ($side in @(@{ n = "host"; S = $H }, @{ n = "join"; S = $J })) {
            $w = @($side.S | Where-Object { [double]$_.t -ge $c0 -and [double]$_.t -le $c1 })
            if ($w.Count -lt 3) { $bad += "$($side.n): only $($w.Count) sample(s) in the combat window"; continue }
            $at1 = @($w | Where-Object { [Math]::Abs([double]$_.mult - 1.0) -le 0.01 }).Count
            $r = [Math]::Round($at1 / $w.Count, 3)
            $m[($side.n + "Combat1x")] = $r
            if ($r -lt $MinMatch) { $bad += "$($side.n) at 1x for only $r of the combat window (need >= $MinMatch)" }
        }
    }

    if ($bad.Count -gt 0) {
        Write-Host ("  SPEED-SYNC FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "speed_sync" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  SPEED-SYNC PASS - $($sets.Count) transitions (max follow $($m.maxFollowMs)ms, lone raise denied), " +
                "match $frac over $consider samples, combat window 1x host=$($m.hostCombat1x) join=$($m.joinCombat1x)")
    return (Add-GateResult -Name "speed_sync" -Status PASS -Metrics $m)
}

# speed_probe (vote-decoupling phase-0 spike, HOST log only): proves the three
# claims the decoupled speed design rests on (speedSync forced off in the run):
#   quiet drives  - after the quiet 3x write the sim multiplier sits at 3.0 and
#                   STICKS (nothing re-syncs it from the buttons) until loud2
#   quiet is quiet- the buttons= series is UNCHANGED across every quiet act
#                   (3x / 1x / pause / resume), while the loud 2x click MOVES it
#   intent capture- the loud click produces an INTENT line (~2.0) within 2 s;
#                   no quiet act produces one (reentrancy guard holds)
function Test-SpeedProbe {
    param([string]$HostFile)
    $S = New-Object System.Collections.ArrayList  # periodic samples
    $pat = 'SCENARIO SPEEDPROBE t=(\d+) mult=([\d\.]+) paused=(\d) nbtn=(-?\d+) buttons=(\S+)'
    foreach ($mi in (Select-String -Path $HostFile -Pattern $pat -ErrorAction SilentlyContinue)) {
        $gg = $mi.Matches[0].Groups
        [void]$S.Add([pscustomobject]@{
            t = [long]$gg[1].Value; mult = [double]$gg[2].Value
            paused = [int]$gg[3].Value; nbtn = [int]$gg[4].Value
            btn = $gg[5].Value })
    }
    $acts = @{}
    foreach ($mi in (Select-String -Path $HostFile -Pattern 'SCENARIO SPEEDPROBE act=(\w+) t=(\d+) ok=(\d)' -ErrorAction SilentlyContinue)) {
        $gg = $mi.Matches[0].Groups
        $acts[$gg[1].Value] = [pscustomobject]@{ t = [long]$gg[2].Value; ok = [int]$gg[3].Value }
    }
    $I = New-Object System.Collections.ArrayList  # captured intent
    foreach ($mi in (Select-String -Path $HostFile -Pattern 'SCENARIO SPEEDPROBE INTENT t=(\d+) mult=([\d\.]+) paused=(\d)' -ErrorAction SilentlyContinue)) {
        $gg = $mi.Matches[0].Groups
        [void]$I.Add([pscustomobject]@{
            t = [long]$gg[1].Value; mult = [double]$gg[2].Value; paused = [int]$gg[3].Value })
    }
    $m = @{ samples = $S.Count; intents = $I.Count; acts = $acts.Count }
    $bad = @()
    foreach ($a in @('quiet3', 'loud2', 'quiet1', 'quietpause', 'quietresume')) {
        if (-not $acts.ContainsKey($a))   { $bad += "act $a missing" }
        elseif ($acts[$a].ok -ne 1)       { $bad += "act $a FAILED (writer returned 0)" }
    }
    if ($S.Count -lt 20) { $bad += "only $($S.Count) probe samples" }
    if ($bad.Count -gt 0) {
        Write-Host ("  SPEED-PROBE FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "speed_probe" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    # Button probe must actually be reading buttons (nbtn >= 1 on the samples).
    $withBtn = @($S | Where-Object { $_.nbtn -ge 1 })
    $m.buttonSamples = $withBtn.Count
    if ($withBtn.Count -lt [Math]::Floor($S.Count * 0.5)) {
        $bad += "button probe unavailable (nbtn>=1 on only $($withBtn.Count)/$($S.Count) samples)"
    }
    $tq3 = $acts['quiet3'].t;     $tl2 = $acts['loud2'].t
    $tq1 = $acts['quiet1'].t;     $tp  = $acts['quietpause'].t
    $tr  = $acts['quietresume'].t
    $lastBtnBefore = {
        param($t)
        $pre = @($withBtn | Where-Object { $_.t -lt $t }) | Select-Object -Last 1
        if ($null -ne $pre) { $pre.btn } else { $null }
    }
    # Claim 1: quiet 3x drives the sim and sticks until the loud click.
    $w3 = @($S | Where-Object { $_.t -ge ($tq3 + 1500) -and $_.t -le ($tl2 - 500) })
    $at3 = @($w3 | Where-Object { [Math]::Abs($_.mult - 3.0) -le 0.05 -and $_.paused -eq 0 }).Count
    $m.quiet3Samples = $w3.Count; $m.quiet3At3 = $at3
    if ($w3.Count -lt 5)            { $bad += "only $($w3.Count) samples in the quiet3 window" }
    elseif ($at3 -lt $w3.Count)     { $bad += "quiet 3x did not stick: $at3/$($w3.Count) samples at 3.0 (re-synced from buttons?)" }
    # Claim 2a: buttons unchanged across quiet3.
    $base = & $lastBtnBefore $tq3
    $b3 = @($withBtn | Where-Object { $_.t -ge ($tq3 + 500) -and $_.t -le ($tl2 - 500) })
    $moved3 = @($b3 | Where-Object { $_.btn -ne $base }).Count
    $m.quiet3BtnMoved = $moved3
    if ($null -eq $base)   { $bad += "no button baseline before quiet3" }
    elseif ($moved3 -gt 0) { $bad += "quiet 3x MOVED the buttons ($moved3 samples differ from '$base')" }
    # Claim 2b: the loud 2x click moves the buttons.
    $bl = @($withBtn | Where-Object { $_.t -ge ($tl2 + 500) -and $_.t -le ($tq1 - 500) })
    $movedLoud = @($bl | Where-Object { $_.btn -ne $base }).Count
    $m.loud2BtnMoved = $movedLoud
    if ($bl.Count -ge 2 -and $movedLoud -eq 0) { $bad += "loud 2x click did NOT move the buttons (probe can't see clicks)" }
    # Loud window sanity: mult reaches ~2.
    $at2 = @($S | Where-Object { $_.t -ge ($tl2 + 1000) -and $_.t -le ($tq1 - 500) -and [Math]::Abs($_.mult - 2.0) -le 0.05 }).Count
    if ($at2 -lt 2) { $bad += "loud 2x did not drive the sim to 2.0" }
    # Quiet pause/resume: paused flag flips, buttons stay where the click left them.
    $preP = & $lastBtnBefore $tp
    $wp = @($S | Where-Object { $_.t -ge ($tp + 1500) -and $_.t -le ($tr - 500) })
    $pOk = @($wp | Where-Object { $_.paused -eq 1 }).Count
    $pBtnMoved = @($wp | Where-Object { $_.nbtn -ge 1 -and $_.btn -ne $preP }).Count
    $m.pauseSamples = $wp.Count; $m.pausedOk = $pOk
    if ($wp.Count -lt 3)          { $bad += "only $($wp.Count) samples in the quiet-pause window" }
    elseif ($pOk -lt $wp.Count)   { $bad += "quiet pause did not hold ($pOk/$($wp.Count) paused)" }
    if ($pBtnMoved -gt 0)         { $bad += "quiet pause MOVED the buttons" }
    $wr = @($S | Where-Object { $_.t -ge ($tr + 1500) })
    $rOk = @($wr | Where-Object { $_.paused -eq 0 }).Count
    if ($wr.Count -ge 2 -and $rOk -lt $wr.Count) { $bad += "quiet resume did not unpause ($rOk/$($wr.Count))" }
    # Claim 3: intent capture - loud click seen, quiet writes silent.
    $loudIntent = @($I | Where-Object { $_.t -ge ($tl2 - 200) -and $_.t -le ($tl2 + 2000) -and [Math]::Abs($_.mult - 2.0) -le 0.05 })
    if ($loudIntent.Count -lt 1) { $bad += "loud 2x click produced NO captured intent (hooks not seeing clicks)" }
    $quietIntents = @()
    foreach ($tq in @($tq3, $tq1, $tp, $tr)) {
        $quietIntents += @($I | Where-Object { $_.t -ge ($tq - 200) -and $_.t -le ($tq + 1500) })
    }
    $m.quietIntentLeaks = $quietIntents.Count
    if ($quietIntents.Count -gt 0) { $bad += "$($quietIntents.Count) INTENT line(s) during quiet writes (reentrancy guard leaking)" }
    if ($bad.Count -gt 0) {
        Write-Host ("  SPEED-PROBE FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "speed_probe" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  SPEED-PROBE PASS - quiet 3x stuck ($at3/$($w3.Count) samples), buttons still ('$base') across quiet acts, " +
                "loud click moved buttons + captured as intent, pause window $pOk/$($wp.Count)")
    return (Add-GateResult -Name "speed_probe" -Status PASS -Metrics $m)
}

# combat_crowd (waiting-attacker stance conformance): the host orders ~5 bar NPCs
# onto its own leader; the attack-slot system keeps most of them QUEUED. Gates:
#   * both stances streamed (host MEMBER task series shows active 0xFE00=65024
#     AND waiting 0xFE01=65025 samples) - else the run proves nothing;
#   * the join's [combat] order re-issue count per crowd hand stays low (the
#     1.5 s clearGoals reset loop is dead - pre-fix: ~40 per hand per minute);
#   * the join's [combat] snap teleport count stays near zero after engagement
#     settles (the teleporting-crowd artifact is gone);
#   * each crowd copy TRACKS its host body (median time-aligned distance <= Tol
#     for >= MinTrackRatio of the crowd).
function Test-CombatCrowd {
    param([string]$HostFile, [string]$JoinFile,
          [double]$Tol = 20.0, [int]$MaxOrdersPerHand = 18,
          [int]$MaxWaitSnaps = 6, [int]$MaxSnaps = 30,
          [double]$MinTrackRatio = 0.6,
          [int]$MaxDt = 800, [int]$SettleMs = 8000)
    # Crowd hands + window anchor from the host log.
    $crowd = @()
    foreach ($cm in (Select-String -Path $HostFile -Pattern 'SCENARIO CROWD striker=(\d+),(\d+)' -ErrorAction SilentlyContinue)) {
        $crowd += ($cm.Matches[0].Groups[1].Value + "," + $cm.Matches[0].Groups[2].Value)
    }
    if ($crowd.Count -lt 3) {
        Write-Host "  COMBAT-CROWD FAIL - only $($crowd.Count) striker(s) picked (need >= 3)"
        return (Add-GateResult -Name "combat_crowd" -Status FAIL -Metrics @{ strikers = $crowd.Count } -Detail "crowd pick failed")
    }
    $tIssued = Get-MarkerTimeMs -File $HostFile -Pattern 'SCENARIO CROWD issued'
    if ($null -eq $tIssued) {
        Write-Host "  COMBAT-CROWD FAIL - host never issued the crowd order"
        return (Add-GateResult -Name "combat_crowd" -Status FAIL -Detail "no issued marker")
    }
    $HS = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $JS = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $m = @{ strikers = $crowd.Count }
    $bad = @()

    # Helper: is a series key (i,s,type,ctnr,ctnrSerial) one of the crowd hands?
    $crowdKeys = @{}
    foreach ($ck in ($HS.Keys + $JS.Keys | Select-Object -Unique)) {
        foreach ($cr in $crowd) {
            if ($ck.StartsWith($cr + ",")) { $crowdKeys[$ck] = $cr; break }
        }
    }

    # Stance coverage: the host must have STREAMED both stances for crowd hands
    # inside the window (task rides the MEMBER lines).
    $nActive = 0; $nWaiting = 0
    foreach ($ck in $crowdKeys.Keys) {
        if (-not $HS.ContainsKey($ck)) { continue }
        foreach ($hsamp in $HS[$ck]) {
            if ([double]$hsamp.t -lt [double]$tIssued) { continue }
            if ($hsamp.task -eq 65024) { $nActive++ }
            elseif ($hsamp.task -eq 65025) { $nWaiting++ }
        }
    }
    $m.activeSamples = $nActive; $m.waitingSamples = $nWaiting
    if ($nActive -lt 3)  { $bad += "host streamed only $nActive ACTIVE combat sample(s) (fight never ran?)" }
    if ($nWaiting -lt 3) { $bad += "host streamed only $nWaiting WAITING sample(s) (no queued attackers - crowd too small or stance read broken)" }

    # Join re-issue counts per crowd hand ([combat] order hand=i,s ...). The
    # pre-fix loop re-ordered every 1.5 s (~40/min); post-fix a copy arms once
    # and re-issues only on retarget/disengage with backoff.
    $orders = @{}
    foreach ($om in (Select-String -Path $JoinFile -Pattern '\[combat\] order hand=(\d+),(\d+)' -ErrorAction SilentlyContinue)) {
        $oh = $om.Matches[0].Groups[1].Value + "," + $om.Matches[0].Groups[2].Value
        if ($crowd -contains $oh) { $orders[$oh] = 1 + $(if ($orders.ContainsKey($oh)) { $orders[$oh] } else { 0 }) }
    }
    $maxOrders = 0
    foreach ($ov in $orders.Values) { if ($ov -gt $maxOrders) { $maxOrders = $ov } }
    $m.maxOrdersPerHand = $maxOrders
    if ($maxOrders -gt $MaxOrdersPerHand) {
        $bad += "a crowd copy was re-ordered $maxOrders times (> $MaxOrdersPerHand - the re-issue loop is back)"
    }

    # Join snap teleports for crowd hands after the settle grace. The WAIT-stance
    # count is the artifact gate (a queued host body holds its ring spot, so a
    # snap there means the join copy wandered - the exact pre-fix behaviour); the
    # total is a looser sanity cap (an ACTIVE brawl legitimately churns, and a
    # host body that sprint-chases 40-110 u NEEDS the teleport).
    $joff = Get-LogClockOffsetMs -File $JoinFile
    $snaps = 0; $waitSnaps = 0
    foreach ($sm in (Select-String -Path $JoinFile -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[combat\] snap hand=(\d+),(\d+) drift=[\d\.]+ wait=(\d)' -ErrorAction SilentlyContinue)) {
        $sg = $sm.Matches[0].Groups
        $sh = $sg[5].Value + "," + $sg[6].Value
        if ($crowd -notcontains $sh) { continue }
        $st = Convert-StampToMs -Groups $sg -OffsetMs $joff
        if ([double]$st -lt ([double]$tIssued + $SettleMs)) { continue }
        $snaps++
        if ($sg[7].Value -eq "1") { $waitSnaps++ }
    }
    $m.snaps = $snaps; $m.waitSnaps = $waitSnaps
    if ($waitSnaps -gt $MaxWaitSnaps) {
        $bad += "$waitSnaps WAIT-stance snap teleport(s) after settle (> $MaxWaitSnaps - waiting copies still bouncing)"
    }
    if ($snaps -gt $MaxSnaps) {
        $bad += "$snaps total combat snap teleport(s) after settle (> $MaxSnaps)"
    }

    # Per-hand tracking: median time-aligned host-vs-join distance in the window.
    # DOWN samples (bs&7) are excluded on either side: where a KO'd body comes to
    # rest is the Stage-2 down-placement problem, not combat stance tracking (a
    # host body that sprints away and falls 70 u off is judged by neither).
    # Hands are matched on the index,serial PREFIX only: the join detaches its
    # driven combat copies from town AI, which changes the local CONTAINER fields
    # of its logged hand mid-run (the same body appears under two 5-field keys).
    $judged = 0; $tracked = 0; $worst = 0.0; $mism = @()
    foreach ($cr in $crowd) {
        $hk = @($HS.Keys | Where-Object { $_.StartsWith($cr + ",") }) | Select-Object -First 1
        if ($null -eq $hk) { continue }
        $hPost = @($HS[$hk] | Where-Object {
            [double]$_.t -ge ([double]$tIssued + $SettleMs) -and (($_.bs -band 7) -eq 0) })
        if ($hPost.Count -lt 8) { continue }
        $jUp = New-Object System.Collections.ArrayList
        foreach ($jk in $JS.Keys) {
            if (-not $jk.StartsWith($cr + ",")) { continue }
            foreach ($jsamp in $JS[$jk]) {
                if (($jsamp.bs -band 7) -eq 0) { [void]$jUp.Add($jsamp) }
            }
        }
        $dists = New-Object System.Collections.ArrayList
        foreach ($hsamp in $hPost) {
            $best = [double]::MaxValue; $bj = $null
            foreach ($jsamp in $jUp) {
                $dt = [Math]::Abs([double]$jsamp.t - [double]$hsamp.t)
                if ($dt -lt $best) { $best = $dt; $bj = $jsamp }
            }
            if ($best -le $MaxDt -and $null -ne $bj) {
                $dx = $hsamp.p[0] - $bj.p[0]; $dy = $hsamp.p[1] - $bj.p[1]; $dz = $hsamp.p[2] - $bj.p[2]
                [void]$dists.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            }
        }
        if ($dists.Count -lt 4) { continue }
        $sorted = $dists | Sort-Object
        $med = $sorted[[int]($sorted.Count / 2)]
        $judged++
        if ($med -le $Tol) { $tracked++ } else { $mism += "$cr(med=$([Math]::Round($med,1)))" }
        if ($med -gt $worst) { $worst = $med }
    }
    $m.judged = $judged; $m.tracked = $tracked
    $m.worstMedian = [Math]::Round($worst, 2)
    if ($judged -lt 3) {
        $bad += "only $judged crowd hand(s) present in both series (need >= 3)"
    } else {
        $ratio = [Math]::Round($tracked / $judged, 3)
        $m.trackRatio = $ratio
        if ($ratio -lt $MinTrackRatio) {
            $bad += "only $tracked/$judged crowd copies tracked within ${Tol}u (ratio $ratio < $MinTrackRatio)$(if ($mism.Count) { ': ' + ($mism -join ', ') })"
        }
    }

    if ($bad.Count -gt 0) {
        Write-Host ("  COMBAT-CROWD FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "combat_crowd" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  COMBAT-CROWD PASS - $($crowd.Count) strikers (active/waiting samples $nActive/$nWaiting), " +
                "maxOrders $maxOrders, snaps $snaps (wait $waitSnaps), tracked $tracked/$judged (worstMedian $($m.worstMedian)u)")
    return (Add-GateResult -Name "combat_crowd" -Status PASS -Metrics $m)
}

# split_interest (step 5, dual-interest conformance): after the HOST relocates its
# whole tab away from the bar ("SCENARIO SPLIT moved=.."), NPCs anchored at the bar
# (near the logged "SCENARIO SPLIT bar=x,y,z" position) must KEEP streaming and
# tracking on the join - the second interest sphere (centered on the join's tab
# leader, who stayed) is what makes that possible. Pre-dual-interest this fails:
# the bar leaves the host's only sphere and its NPCs freeze/vanish on the join.
function Test-SplitInterest {
    param([string]$HostFile, [string]$JoinFile,
          [double]$AnchorR = 150.0, [double]$Tol = 6.0,
          [double]$MinRatio = 0.8, [int]$MinJudged = 2,
          [int]$GraceMs = 6000, [int]$MaxDt = 800)
    $bar = Select-String -Path $HostFile -Pattern 'SCENARIO SPLIT bar=(-?[\d\.]+),(-?[\d\.]+),(-?[\d\.]+) have=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $bar) {
        Write-Host "  SPLIT-INTEREST FAIL - no bar anchor on host (2-tab save required)"
        return (Add-GateResult -Name "split_interest" -Status FAIL -Detail "no bar anchor")
    }
    $bx = [double]$bar.Matches[0].Groups[1].Value
    $bz = [double]$bar.Matches[0].Groups[3].Value
    $T = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO SPLIT moved="
    if ($null -eq $T) {
        Write-Host "  SPLIT-INTEREST FAIL - host never moved its tab away"
        return (Add-GateResult -Name "split_interest" -Status FAIL -Detail "no moved marker")
    }
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    # Bar-anchored NPCs: hands whose HOST positions stay near the bar anchor.
    $judged = 0; $tracked = 0; $worst = 0.0; $mism = @()
    foreach ($hand in $H.Keys) {
        if (-not $J.ContainsKey($hand)) { continue }
        # Post-move host samples for this hand, restricted to near-the-bar.
        $hPost = @($H[$hand] | Where-Object {
            $_.t -ge ($T + $GraceMs) -and
            [Math]::Sqrt(($_.p[0]-$bx)*($_.p[0]-$bx) + ($_.p[2]-$bz)*($_.p[2]-$bz)) -le $AnchorR })
        if ($hPost.Count -lt 8) { continue }  # not a continuously-streamed bar NPC
        $dists = New-Object System.Collections.ArrayList
        foreach ($hs in $hPost) {
            $best = [double]::MaxValue; $bj = $null
            foreach ($js in $J[$hand]) {
                $dt = [Math]::Abs($js.t - $hs.t)
                if ($dt -lt $best) { $best = $dt; $bj = $js }
            }
            if ($best -le $MaxDt -and $null -ne $bj) {
                $dx = $hs.p[0]-$bj.p[0]; $dy = $hs.p[1]-$bj.p[1]; $dz = $hs.p[2]-$bj.p[2]
                [void]$dists.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            }
        }
        if ($dists.Count -lt 4) { continue }  # join stopped receiving it post-move
        $sorted = $dists | Sort-Object
        $med = $sorted[[int]($sorted.Count/2)]
        $judged++
        if ($med -le $Tol) { $tracked++ } else { $mism += "$hand(med=$([Math]::Round($med,1)))" }
        if ($med -gt $worst) { $worst = $med }
    }
    if ($judged -lt $MinJudged) {
        Write-Host "  SPLIT-INTEREST FAIL - only $judged bar-anchored NPC(s) streamed+received post-split (need >= $MinJudged; the remote sphere is not covering the bar)"
        return (Add-GateResult -Name "split_interest" -Status FAIL -Metrics @{ judged = $judged } -Detail "bar NPCs not streamed post-split")
    }
    $ratio = [Math]::Round($tracked / $judged, 3)
    $ok = ($ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($mism.Count -gt 0) { " mismatches: $($mism -join ', ')" } else { "" }
    # Churn metric (hysteresis health): suppress/restore totals from the join.
    $churn = Select-String -Path $JoinFile -Pattern 'SCENARIO AUTH suppresses=(\d+) restores=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    $sup = 0; $res = 0
    if ($null -ne $churn) { $sup = [int]$churn.Matches[0].Groups[1].Value; $res = [int]$churn.Matches[0].Groups[2].Value }
    Write-Host "  SPLIT-INTEREST $v - $tracked/$judged bar-anchored NPCs tracked within ${Tol}u AFTER the host moved away (ratio=$ratio >= $MinRatio, worstMedian=$([Math]::Round($worst,1)), churn=$sup/$res)$detail"
    return (Add-GateResult -Name "split_interest" -Status $v -Metrics @{
        judged = $judged; tracked = $tracked; ratio = $ratio
        worstMedian = [Math]::Round($worst, 2); suppresses = $sup; restores = $res })
}

# ---- Inventory / world-item oracles ------------------------------------------------

# inv_order (Phase 4a): content-snapshot replication; multiset (hash) gate.
function Test-InventorySync {
    param([string]$HostFile, [string]$JoinFile)
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
        return (Add-GateResult -Name "inv_sync" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $added = $false
    if (Test-Path $HostFile) { $added = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO INV ADD added=[1-9]' -Quiet) }
    $hFirst = $H[0]; $hLast = $H[$H.Count - 1]
    $jFirst = $J[0]; $jLast = $J[$J.Count - 1]
    $jChanged = $false
    foreach ($s in $J) { if ($s.hash -ne $jFirst.hash) { $jChanged = $true; break } }
    $hostChanged = ($hFirst.hash -ne $hLast.hash)
    $hashMatch   = ($hLast.hash -eq $jLast.hash)
    $joinNonEmpty= ($jLast.count -gt 0)
    $ok = ($added -and $hostChanged -and $hashMatch -and $joinNonEmpty)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  INV-SYNC $v - add=$added hostChanged=$hostChanged hashMatch=$hashMatch " +
                "joinNonEmpty=$joinNonEmpty (joinSawTransition=$jChanged advisory) " +
                "host=$($hFirst.count)->$($hLast.count) join=$($jFirst.count)->$($jLast.count) " +
                "finalHash host=$($hLast.hash) join=$($jLast.hash)")
    return (Add-GateResult -Name "inv_sync" -Status $v -Metrics @{
        added = $added; hostChanged = $hostChanged; hashMatch = $hashMatch
        joinNonEmpty = $joinNonEmpty; hostFinal = $hLast.count; joinFinal = $jLast.count })
}

# inv_bidir: per-rank convergence in BOTH directions (host authors r0, join r1).
function Test-InventoryBidir {
    param([string]$HostFile, [string]$JoinFile)
    $rx = 'SCENARIO INVB r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+)'
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
        return (Add-GateResult -Name "inv_bidir" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $distinct = { param($rows) ($rows | ForEach-Object { $_.hash } | Select-Object -Unique).Count }
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
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVB ADD r=0 n=[1-9]' -Quiet) -and `
               (Select-String -Path $HostFile -Pattern 'SCENARIO INVB REM r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB ADD r=1 n=[1-9]' -Quiet) -and `
               (Select-String -Path $JoinFile -Pattern 'SCENARIO INVB REM r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-BIDIR " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return (Add-GateResult -Name "inv_bidir" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; hostSeq = $hostSeq; joinSeq = $joinSeq })
}

# Shared engine for inv_equip / inv_reequip (both parse SCENARIO INVE lines).
function Get-InveSeries {
    param([string]$File)
    $rx = 'SCENARIO INVE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) eq=(\d+) hash=(\d+)'
    $arr = @()
    if (Test-Path $File) {
        foreach ($ln in Get-Content $File) {
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

# inv_equip: equipped-gear removal replicates per rank, both directions.
function Test-InventoryEquip {
    param([string]$HostFile, [string]$JoinFile)
    $H = Get-InveSeries -File $HostFile
    $J = Get-InveSeries -File $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-EQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_equip" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $pick = { param($S, $rank, $role) @($S | Where-Object { $_.rank -eq $rank -and $_.role -eq $role }) }
    $maxEq = { param($rows) ($rows | ForEach-Object { $_.eq } | Measure-Object -Maximum).Maximum }
    $checkDir = {
        param($name, $authorRows, $obsRows)
        if ($authorRows.Count -lt 1 -or $obsRows.Count -lt 1) {
            Write-Host "  INV-EQUIP $name FAIL - missing samples (author=$($authorRows.Count) observer=$($obsRows.Count))"
            return $false
        }
        $aLast = $authorRows[$authorRows.Count - 1]; $oLast = $obsRows[$obsRows.Count - 1]
        $aPeak = & $maxEq $authorRows
        $authorReduced = ($aPeak -ge 1) -and ($aLast.eq -lt $aPeak)   # a worn item was removed
        $converged = ($aLast.hash -eq $oLast.hash) -and ($aLast.count -eq $oLast.count) -and `
                     ($aLast.eq -eq $oLast.eq)
        $r = $authorReduced -and $converged
        Write-Host ("  INV-EQUIP $name " + $(if ($r) { "PASS" } else { "FAIL" }) +
                    " - authorReduced=$authorReduced(peakEq$aPeak`->$($aLast.eq)) converged=$converged" +
                    " authorFinal=(c$($aLast.count),eq$($aLast.eq),h$($aLast.hash))" +
                    " observerFinal=(c$($oLast.count),eq$($oLast.eq),h$($oLast.hash))")
        return $r
    }
    $h2j = & $checkDir "host->join(r0)" (& $pick $H 0 "OWN") (& $pick $J 0 "PEER")
    $j2h = & $checkDir "join->host(r1)" (& $pick $J 1 "OWN") (& $pick $H 1 "PEER")
    $hostSeq = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $joinSeq = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $ok = $h2j -and $j2h -and $hostSeq -and $joinSeq
    Write-Host ("  INV-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostSeq=$hostSeq joinSeq=$joinSeq")
    return (Add-GateResult -Name "inv_equip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; hostSeq = $hostSeq; joinSeq = $joinSeq })
}

# inv_reequip (up path): unequip to loose, hold, re-equip; observer must witness
# the dip-then-restore and converge.
function Test-InventoryReequip {
    param([string]$HostFile, [string]$JoinFile)
    $H = Get-InveSeries -File $HostFile
    $J = Get-InveSeries -File $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  INV-REEQUIP FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "inv_reequip" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
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
        $authorRestored = ($aPeak -ge 1) -and ($aDip -lt $aPeak) -and ($aLast.eq -eq $aPeak)
        $oPeak = & $maxEq $obsRows; $oDip = & $minEq $obsRows
        $observerSawCycle = ($oDip -lt $oPeak) -and ($oLast.eq -eq $oPeak)
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
    $hostUn = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE UNEQUIP r=0 n=[1-9]' -Quiet)
    $hostRe = (Select-String -Path $HostFile -Pattern 'SCENARIO INVE REEQUIP r=0 n=[1-9]' -Quiet)
    $joinUn = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE UNEQUIP r=1 n=[1-9]' -Quiet)
    $joinRe = (Select-String -Path $JoinFile -Pattern 'SCENARIO INVE REEQUIP r=1 n=[1-9]' -Quiet)
    $seq = $hostUn -and $hostRe -and $joinUn -and $joinRe
    $ok = $h2j -and $j2h -and $seq
    Write-Host ("  INV-REEQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }) +
                " - host->join=$h2j join->host=$j2h hostUn=$hostUn hostRe=$hostRe joinUn=$joinUn joinRe=$joinRe")
    return (Add-GateResult -Name "inv_reequip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ h2j = $h2j; j2h = $j2h; seq = $seq })
}

# inv_addequip (d25 fix): LOCAL reconcile - a fabricated equip must become a durable
# create-loose-then-equip. Every client that produced a verdict must pass.
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
        return (Add-GateResult -Name "add_equip" -Status FAIL -Detail "no ADDEQ verdict on either client")
    }
    $ok = $true
    if ($null -ne $h) { $ok = $ok -and $h }
    if ($null -ne $j) { $ok = $ok -and $j }
    Write-Host ("  ADD-EQUIP " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "add_equip" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ host = $h; join = $j })
}

# trade_probe (protocol-36 BASELINE, evidence not a sync-quality gate): the host
# performed three real cross-owner drags (TAKE / GIVE / WTAKE, violating the
# single-writer inventory model the way a player's direct drag does) while both
# clients sampled both squad containers. PASS = the probe EXECUTED; the value is
# the printed conservation report (dupe / loss / weapon-vanish signatures).
function Test-TradeProbe {
    param([string]$HostFile, [string]$JoinFile)
    $rx = "SCENARIO TRDP r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+) probe=(-?\d+) wpn=(-?\d+)"
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; t = [int]$matches[3]
                        count = [int]$matches[4]; probe = [int]$matches[6]; wpn = [int]$matches[7]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  TRADE-PROBE FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "trade_probe" -Status FAIL `
                    -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    # The three cross-owner drags (host log). WTAKE n=-1 = no weapon was found to move.
    $moves = @{}
    foreach ($m in @("TAKE", "GIVE", "WTAKE")) {
        $l = Select-String -Path $HostFile -Pattern "SCENARIO TRDP $m n=(-?\d+) sid='([^']*)'" |
             Select-Object -Last 1
        if ($l -and $l.Line -match "n=(-?\d+) sid='([^']*)'") {
            $moves[$m] = @{ n = [int]$matches[1]; sid = $matches[2] }
        }
    }
    $seedH = Select-String -Path $HostFile -Pattern "SCENARIO TRDP SEED r=0 n=[1-9]" -Quiet
    $seedJ = Select-String -Path $JoinFile -Pattern "SCENARIO TRDP SEED r=1 n=[1-9]" -Quiet

    # Per client: first/final probe + weapon totals across both containers, and
    # per-rank finals (where the moved items LANDED). A cross-owner move conserves
    # the per-client total; the seeds add +5 globally (host +2, join +3, same sid).
    $summar = {
        param($S)
        $first = @{}; $final = @{}
        foreach ($r in 0, 1) {
            $rows = @($S | Where-Object { $_.rank -eq $r })
            if ($rows.Count -gt 0) {
                $first[$r] = $rows[0]
                $final[$r] = $rows[$rows.Count - 1]
            }
        }
        if (-not ($first.ContainsKey(0) -and $first.ContainsKey(1))) { return $null }
        [pscustomobject]@{
            probeFirst = $first[0].probe + $first[1].probe
            probeFinal = $final[0].probe + $final[1].probe
            probeR0 = $final[0].probe; probeR1 = $final[1].probe
            wpnFirst = $first[0].wpn + $first[1].wpn
            wpnFinal = $final[0].wpn + $final[1].wpn
            wpnR0 = $final[0].wpn; wpnR1 = $final[1].wpn
        }
    }
    $hs = & $summar $H
    $js = & $summar $J
    if (-not $hs -or -not $js) {
        Write-Host "  TRADE-PROBE FAIL - a client never sampled both containers"
        return (Add-GateResult -Name "trade_probe" -Status FAIL -Detail "missing rank series")
    }
    # PER-MOVE signatures (final totals alone can lie: a TAKE-dupe and a GIVE-wipe
    # cancel arithmetically). The drags fire at fixed scenario times (TAKE 16 s /
    # GIVE 26 s / WTAKE 36 s on the host clock; both scenario clocks arm at
    # peer-ready, so join timestamps align within ~a second) - read each rank's
    # value just before each drag and compare with the settled value before the
    # NEXT drag / at the end.
    $valAt = {
        param($S, $rank, $tMax)   # last sample of `rank` strictly before tMax (0 = final)
        $rows = @($S | Where-Object { $_.rank -eq $rank -and ($tMax -le 0 -or $_.t -lt $tMax) })
        if ($rows.Count -eq 0) { return $null }
        return $rows[$rows.Count - 1]
    }
    $TAKE_MS = 16000; $GIVE_MS = 26000; $WPN_MS = 36000
    # probe-item value per (client, rank) at each boundary
    $hR0pre = (& $valAt $H 0 $TAKE_MS).probe; $hR1pre = (& $valAt $H 1 $TAKE_MS).probe
    $jR1pre = (& $valAt $J 1 $TAKE_MS).probe
    $hR0mid = (& $valAt $H 0 $GIVE_MS).probe; $hR1mid = (& $valAt $H 1 $GIVE_MS).probe
    $jR1mid = (& $valAt $J 1 $GIVE_MS).probe
    $hR0end = $hs.probeR0; $hR1end = $hs.probeR1
    $jR0end = $js.probeR0; $jR1end = $js.probeR1
    # TAKE (r1 -> r0 by the host): did it land, did the REMOVAL ever reach the owner,
    # did the owner's snapshot re-add it on the host (the dupe)?
    $takeLanded     = ($hR0mid - $hR0pre) -ge 1
    $takePropagated = ($jR1mid - $jR1pre) -le -1
    $takeReAdded    = $takeLanded -and (-not $takePropagated) -and ($hR1mid -ge $hR1pre)
    $takeSig = if ($takeReAdded) { "DUPE(re-added by owner snapshot; removal never propagated)" }
               elseif ($takeLanded -and $takePropagated) { "CLEAN" }
               elseif (-not $takeLanded) { "NO-OP(item never landed)" }
               else { "PARTIAL" }
    # GIVE (r0 -> r1 by the host): it left r0; did it ever ARRIVE on the owner, or
    # did the owner's reconcile wipe it?
    $giveSent    = ($hR0end - $hR0mid) -le -1
    $giveArrived = ($jR1end - $jR1mid) -ge 1
    $giveSig = if ($giveSent -and -not $giveArrived) { "WIPED(owner reconcile destroyed the given item)" }
               elseif ($giveSent -and $giveArrived) { "CLEAN" }
               else { "NO-OP(item never left)" }
    # WTAKE (weapon r1 -> r0): weapons cannot be fabricated on a peer, so the failure
    # mode is cross-client STATE DIVERGENCE (each client renders different bags).
    $wpnDiverged = ($hs.wpnR0 -ne $js.wpnR0) -or ($hs.wpnR1 -ne $js.wpnR1)
    $wpnSig = if ($wpnDiverged) { "DIVERGED(host r0=$($hs.wpnR0)/r1=$($hs.wpnR1) vs join r0=$($js.wpnR0)/r1=$($js.wpnR1))" }
              else { "CLEAN" }
    $mv = { param($m) if ($moves.ContainsKey($m)) { "n=$($moves[$m].n) sid='$($moves[$m].sid)'" } else { "(missing)" } }
    Write-Host "  TRADE-PROBE moves: TAKE $(& $mv 'TAKE')  GIVE $(& $mv 'GIVE')  WTAKE $(& $mv 'WTAKE')  seeds host=$seedH join=$seedJ"
    Write-Host "  TRADE-PROBE probe totals: host $($hs.probeFirst)->$($hs.probeFinal) (r0=$hR0end r1=$hR1end)  join $($js.probeFirst)->$($js.probeFinal) (r0=$jR0end r1=$jR1end)"
    Write-Host "  TRADE-PROBE wpn   totals: host $($hs.wpnFirst)->$($hs.wpnFinal) (r0=$($hs.wpnR0) r1=$($hs.wpnR1))  join $($js.wpnFirst)->$($js.wpnFinal) (r0=$($js.wpnR0) r1=$($js.wpnR1))"
    Write-Host "  TRADE-PROBE TAKE : landed=$takeLanded removalPropagated=$takePropagated reAdded=$takeReAdded => $takeSig"
    Write-Host "  TRADE-PROBE GIVE : sent=$giveSent arrived=$giveArrived => $giveSig"
    Write-Host "  TRADE-PROBE WTAKE: => $wpnSig"
    Write-Host "  FINDING: trade_probe baseline - TAKE:$takeSig GIVE:$giveSig WEAPON:$wpnSig"
    # PASS = executed (all three drags fired, TAKE/GIVE actually moved something,
    # both clients sampled + seeded). The signatures above are the evidence.
    $executed = $moves.ContainsKey("TAKE") -and $moves["TAKE"].n -ge 1 -and `
                $moves.ContainsKey("GIVE") -and $moves["GIVE"].n -ge 1 -and `
                $moves.ContainsKey("WTAKE") -and $seedH -and $seedJ
    $v = if ($executed) { "PASS" } else { "FAIL" }
    Write-Host "  TRADE-PROBE $v - executed=$executed"
    return (Add-GateResult -Name "trade_probe" -Status $v -Metrics @{
        hostProbe = "$($hs.probeFirst)->$($hs.probeFinal)"; joinProbe = "$($js.probeFirst)->$($js.probeFinal)"
        hostWpn = "$($hs.wpnFirst)->$($hs.wpnFinal)"; joinWpn = "$($js.wpnFirst)->$($js.wpnFinal)"
        takeSig = $takeSig; giveSig = $giveSig; wpnSig = $wpnSig })
}

# trade_peer (protocol 37 VALIDATION): the trade_probe drags rerun with the
# transfer-intent channel live. GATES that every baselined failure signature is
# closed: TAKE lands AND its removal reaches the owner (no dupe), GIVE arrives on
# the owner (no wipe), the traded WEAPON is conserved per client AND both clients
# agree on the final per-rank state (no divergence/vanish), and the channel
# actually carried it ([xfer] SEND on the dragger, [xfer] APPLY on the peer).
function Test-TradePeer {
    param([string]$HostFile, [string]$JoinFile)
    $rx = "SCENARIO TRDE r=(\d+) (OWN|PEER) t=(\d+) count=(\d+) hash=(\d+) probe=(-?\d+) wpn=(-?\d+)"
    $series = {
        param($file)
        $arr = @()
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match $rx) {
                    $arr += [pscustomobject]@{
                        rank = [int]$matches[1]; t = [int]$matches[3]
                        count = [int]$matches[4]; probe = [int]$matches[6]; wpn = [int]$matches[7]
                    }
                }
            }
        }
        return ,$arr
    }
    $H = & $series $HostFile
    $J = & $series $JoinFile
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  TRADE-PEER FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name "trade_peer" -Status FAIL `
                    -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    # The three cross-owner drags must have MOVED something locally on the host.
    $moves = @{}
    foreach ($m in @("TAKE", "GIVE", "WTAKE")) {
        $l = Select-String -Path $HostFile -Pattern "SCENARIO TRDE $m n=(-?\d+) sid='([^']*)'" |
             Select-Object -Last 1
        if ($l -and $l.Line -match "n=(-?\d+) sid='([^']*)'") {
            $moves[$m] = @{ n = [int]$matches[1]; sid = $matches[2] }
        }
    }
    $seedH = Select-String -Path $HostFile -Pattern "SCENARIO TRDE SEED r=0 n=[1-9]" -Quiet
    $seedJ = Select-String -Path $JoinFile -Pattern "SCENARIO TRDE SEED r=1 n=[1-9]" -Quiet
    # The channel evidence: the dragger authored intents, the peer applied them.
    $sends   = @(Select-String -Path $HostFile -Pattern "\[xfer\] SEND id=").Count
    $applies = @(Select-String -Path $JoinFile -Pattern "\[xfer\] APPLY id=").Count

    $summar = {
        param($S)
        $first = @{}; $final = @{}
        foreach ($r in 0, 1) {
            $rows = @($S | Where-Object { $_.rank -eq $r })
            if ($rows.Count -gt 0) {
                $first[$r] = $rows[0]
                $final[$r] = $rows[$rows.Count - 1]
            }
        }
        if (-not ($first.ContainsKey(0) -and $first.ContainsKey(1))) { return $null }
        [pscustomobject]@{
            probeFirst = $first[0].probe + $first[1].probe
            probeFinal = $final[0].probe + $final[1].probe
            probeR0 = $final[0].probe; probeR1 = $final[1].probe
            wpnFirst = $first[0].wpn + $first[1].wpn
            wpnFinal = $final[0].wpn + $final[1].wpn
            wpnR0 = $final[0].wpn; wpnR1 = $final[1].wpn
        }
    }
    $hs = & $summar $H
    $js = & $summar $J
    if (-not $hs -or -not $js) {
        Write-Host "  TRADE-PEER FAIL - a client never sampled both containers"
        return (Add-GateResult -Name "trade_peer" -Status FAIL -Detail "missing rank series")
    }
    # Same per-move boundaries as the baseline oracle (drag times are shared).
    $valAt = {
        param($S, $rank, $tMax)   # last sample of `rank` strictly before tMax (0 = final)
        $rows = @($S | Where-Object { $_.rank -eq $rank -and ($tMax -le 0 -or $_.t -lt $tMax) })
        if ($rows.Count -eq 0) { return $null }
        return $rows[$rows.Count - 1]
    }
    $TAKE_MS = 16000; $GIVE_MS = 26000
    $hR0pre = (& $valAt $H 0 $TAKE_MS).probe; $hR1pre = (& $valAt $H 1 $TAKE_MS).probe
    $jR1pre = (& $valAt $J 1 $TAKE_MS).probe
    $hR0mid = (& $valAt $H 0 $GIVE_MS).probe; $hR1mid = (& $valAt $H 1 $GIVE_MS).probe
    $jR1mid = (& $valAt $J 1 $GIVE_MS).probe
    $hR0end = $hs.probeR0; $hR1end = $hs.probeR1
    $jR0end = $js.probeR0; $jR1end = $js.probeR1
    # TAKE (r1 -> r0): landed on the dragger AND the removal reached the owner AND
    # no re-add on the dragger's r1 (the dupe window).
    $takeLanded     = ($hR0mid - $hR0pre) -ge 1
    $takePropagated = ($jR1mid - $jR1pre) -le -1
    $takeNoDupe     = ($hR1mid - $hR1pre) -le -1   # dragger's r1 stayed down (no re-add)
    $takeOk  = $takeLanded -and $takePropagated -and $takeNoDupe
    $takeSig = if ($takeOk) { "CLEAN" }
               elseif (-not $takeLanded) { "NO-OP(item never landed)" }
               elseif (-not $takePropagated) { "DUPE(removal never reached the owner)" }
               else { "DUPE(re-added on the dragger)" }
    # GIVE (r0 -> r1): left the dragger's r0 AND arrived on the owner (no wipe).
    $giveSent    = ($hR0end - $hR0mid) -le -1
    $giveArrived = ($jR1end - $jR1mid) -ge 1
    $giveKeptH   = ($hR1end - $hR1mid) -ge 1       # the dragger still renders it in r1
    $giveOk  = $giveSent -and $giveArrived -and $giveKeptH
    $giveSig = if ($giveOk) { "CLEAN" }
               elseif (-not $giveSent) { "NO-OP(item never left)" }
               elseif (-not $giveArrived) { "WIPED(never arrived on the owner)" }
               else { "WIPED(reconciled away on the dragger)" }
    # WTAKE: conservation per client + cross-client agreement + it actually moved.
    $wpnConsH  = $hs.wpnFinal -eq $hs.wpnFirst
    $wpnConsJ  = $js.wpnFinal -eq $js.wpnFirst
    $wpnAgree  = ($hs.wpnR0 -eq $js.wpnR0) -and ($hs.wpnR1 -eq $js.wpnR1)
    $wpnMoved  = $hs.wpnR0 -ge 1
    $wpnOk  = $wpnConsH -and $wpnConsJ -and $wpnAgree -and $wpnMoved
    $wpnSig = if ($wpnOk) { "CLEAN" }
              elseif (-not ($wpnConsH -and $wpnConsJ)) { "VANISH(host $($hs.wpnFirst)->$($hs.wpnFinal) join $($js.wpnFirst)->$($js.wpnFinal))" }
              elseif (-not $wpnAgree) { "DIVERGED(host r0=$($hs.wpnR0)/r1=$($hs.wpnR1) vs join r0=$($js.wpnR0)/r1=$($js.wpnR1))" }
              else { "NO-MOVE(weapon never landed in r0)" }
    # Probe-item conservation: cross-owner moves conserve; the seeds add +5 globally.
    $consH = $hs.probeFinal -eq ($hs.probeFirst + 5)
    $consJ = $js.probeFinal -eq ($js.probeFirst + 5)
    $probeAgree = ($hR0end -eq $jR0end) -and ($hR1end -eq $jR1end)
    $mv = { param($m) if ($moves.ContainsKey($m)) { "n=$($moves[$m].n) sid='$($moves[$m].sid)'" } else { "(missing)" } }
    Write-Host "  TRADE-PEER moves: TAKE $(& $mv 'TAKE')  GIVE $(& $mv 'GIVE')  WTAKE $(& $mv 'WTAKE')  seeds host=$seedH join=$seedJ  xfer sent=$sends applied=$applies"
    Write-Host "  TRADE-PEER probe totals: host $($hs.probeFirst)->$($hs.probeFinal) (r0=$hR0end r1=$hR1end)  join $($js.probeFirst)->$($js.probeFinal) (r0=$jR0end r1=$jR1end)  conserve host=$consH join=$consJ agree=$probeAgree"
    Write-Host "  TRADE-PEER wpn   totals: host $($hs.wpnFirst)->$($hs.wpnFinal) (r0=$($hs.wpnR0) r1=$($hs.wpnR1))  join $($js.wpnFirst)->$($js.wpnFinal) (r0=$($js.wpnR0) r1=$($js.wpnR1))"
    Write-Host "  TRADE-PEER TAKE : landed=$takeLanded removalPropagated=$takePropagated noDupe=$takeNoDupe => $takeSig"
    Write-Host "  TRADE-PEER GIVE : sent=$giveSent arrived=$giveArrived keptOnDragger=$giveKeptH => $giveSig"
    Write-Host "  TRADE-PEER WTAKE: conserveH=$wpnConsH conserveJ=$wpnConsJ agree=$wpnAgree moved=$wpnMoved => $wpnSig"
    $executed = $moves.ContainsKey("TAKE") -and $moves["TAKE"].n -ge 1 -and `
                $moves.ContainsKey("GIVE") -and $moves["GIVE"].n -ge 1 -and `
                $moves.ContainsKey("WTAKE") -and $moves["WTAKE"].n -ge 1 -and `
                $seedH -and $seedJ
    $channel = ($sends -ge 3) -and ($applies -ge 3)
    $ok = $executed -and $channel -and $takeOk -and $giveOk -and $wpnOk -and `
          $consH -and $consJ -and $probeAgree
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  TRADE-PEER $v - executed=$executed channel=$channel take=$takeOk give=$giveOk wpn=$wpnOk conserve=$($consH -and $consJ) agree=$probeAgree"
    return (Add-GateResult -Name "trade_peer" -Status $v -Metrics @{
        hostProbe = "$($hs.probeFirst)->$($hs.probeFinal)"; joinProbe = "$($js.probeFirst)->$($js.probeFinal)"
        hostWpn = "$($hs.wpnFirst)->$($hs.wpnFinal)"; joinWpn = "$($js.wpnFirst)->$($js.wpnFinal)"
        sends = $sends; applies = $applies
        takeSig = $takeSig; giveSig = $giveSig; wpnSig = $wpnSig })
}

# drop_probe (W0 diagnostic): assert the probe EXECUTED and surface the evidence.
function Test-DropProbe {
    param([string]$HostFile)
    if (-not (Test-Path $HostFile)) {
        Write-Host "  DROP-PROBE FAIL - no host log"
        return (Add-GateResult -Name "drop_probe" -Status FAIL -Detail "no host log")
    }
    $rx = 'SCENARIO DROP RESULT dropped=(-?\d+) before=(-?\d+) after=(-?\d+) enumerated=(\d+)'
    $line = Select-String -Path $HostFile -Pattern $rx | Select-Object -Last 1
    if (-not $line) {
        Write-Host "  DROP-PROBE FAIL - no 'SCENARIO DROP RESULT' evidence line"
        return (Add-GateResult -Name "drop_probe" -Status FAIL -Detail "no RESULT line")
    }
    $null = ($line.Line -match $rx)
    $dropped    = [int]$matches[1]
    $before     = [int]$matches[2]
    $after      = [int]$matches[3]
    $enumerated = [int]$matches[4]
    $seeded = Select-String -Path $HostFile -Pattern 'SCENARIO DROP SEEDED added=\d+ sid=' | Select-Object -Last 1
    $ok = ($dropped -gt 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  DROP-PROBE $v - dropped=$dropped before=$before after=$after enumerated=$enumerated")
    if ($seeded) { Write-Host ("  DROP-PROBE   " + $seeded.Line.Trim()) }
    $afterScan = $false
    foreach ($ln in Get-Content $HostFile) {
        if ($ln -match 'SCENARIO DROP AFTER-scan:') { $afterScan = $true; continue }
        if ($afterScan) {
            if ($ln -match 'WORLDITEM (scan|  )') { Write-Host ("  DROP-PROBE   " + ($ln -replace '^.*WORLDITEM', 'WORLDITEM').Trim()) }
            elseif ($ln -match 'SCENARIO DROP RESULT') { break }
        }
    }
    return (Add-GateResult -Name "drop_probe" -Status $v -Metrics @{ dropped = $dropped; before = $before; after = $after; enumerated = $enumerated })
}

# world_item_sync (W1): drop streams to the join (proxy spawn), despawn culls it.
function Test-WorldItemSync {
    # -JoinAuthor: the world_item_join variant (W1 bidir) - the JOIN drops/despawns
    # and the HOST must spawn/cull the proxy. Same series logic, roles swapped;
    # $GateName labels the verdict (wi_sync / wi_join).
    param([string]$HostFile, [string]$JoinFile, [double]$Tol = 3.0,
          [switch]$JoinAuthor, [string]$GateName = "wi_sync")
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
    $label = if ($JoinAuthor) { "WI-JOIN" } else { "WI-SYNC" }
    if ($H.Count -lt 2 -or $J.Count -lt 2) {
        Write-Host "  $label FAIL - insufficient samples (host=$($H.Count) join=$($J.Count))"
        return (Add-GateResult -Name $GateName -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $authorFile = if ($JoinAuthor) { $JoinFile } else { $HostFile }
    $drop    = [bool](Select-String -Path $authorFile -Pattern 'SCENARIO WI DROP .*dropped=[1-9]' -Quiet)
    $despawn = [bool](Select-String -Path $authorFile -Pattern 'SCENARIO WI DESPAWN destroyed=[1-9]' -Quiet)
    $hostPresent = $H | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinPresent = $J | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    # The OBSERVER side seeing the item proves the proxy spawned (the author's own
    # sighting is just its real local item).
    $observerSpawned = if ($JoinAuthor) { $hostPresent -ne $null } else { $joinPresent -ne $null }
    $authorSaw       = if ($JoinAuthor) { $joinPresent -ne $null } else { $hostPresent -ne $null }
    $posMatch = $false; $hashMatch = $false; $dxz = -1.0
    if ($hostPresent -and $joinPresent) {
        $dx = $hostPresent.x - $joinPresent.x; $dz = $hostPresent.z - $joinPresent.z
        $dxz = [math]::Sqrt($dx * $dx + $dz * $dz)
        $posMatch  = ($dxz -le $Tol)
        $hashMatch = ($hostPresent.hash -eq $joinPresent.hash)
    }
    $hostCulled = ($hostPresent -ne $null) -and ($H[$H.Count - 1].n -eq 0)
    $joinCulled = ($joinPresent -ne $null) -and ($J[$J.Count - 1].n -eq 0)
    $ok = $drop -and $despawn -and $authorSaw -and $observerSpawned -and $posMatch -and $hashMatch -and $joinCulled -and $hostCulled
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("  $label $v - drop=$drop despawn=$despawn observerSpawned=$observerSpawned " +
                "posMatch=$posMatch (dXZ=$([math]::Round($dxz,2))u tol=$Tol) hashMatch=$hashMatch " +
                "hostCulled=$hostCulled joinCulled=$joinCulled")
    if ($hostPresent -and $joinPresent) {
        Write-Host ("  $label   host item pos=($($hostPresent.x),$($hostPresent.y),$($hostPresent.z)) hash=$($hostPresent.hash)")
        Write-Host ("  $label   join item pos=($($joinPresent.x),$($joinPresent.y),$($joinPresent.z)) hash=$($joinPresent.hash)")
    }
    return (Add-GateResult -Name $GateName -Status $v -Metrics @{
        drop = $drop; despawn = $despawn; observerSpawned = $observerSpawned
        posMatch = $posMatch; dxz = [math]::Round($dxz, 2); hashMatch = $hashMatch
        hostCulled = $hostCulled; joinCulled = $joinCulled })
}

# wpn_relocate (conservation spike): LOCAL single-client bag->ground->bag move of a
# real weapon. Gated on the host; the join result is advisory cross-client evidence.
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
    $h = & $eval $HostFile "host"
    $j = & $eval $JoinFile "join"
    if ($null -eq $h) {
        Write-Host "  WPN-RELOCATE FAIL - no RELOC verdict line on the host (authoritative)"
        return (Add-GateResult -Name "wpn_relocate" -Status FAIL -Detail "no host RELOC verdict")
    }
    if ($null -ne $j) {
        Write-Host ("  WPN-RELOCATE [join] advisory cross-client result: " + $(if ($j) { "consistent" } else { "perturbed (host reconcile timing)" }))
    }
    Write-Host ("  WPN-RELOCATE " + $(if ($h) { "PASS" } else { "FAIL" }) + " (gated on host)")
    return (Add-GateResult -Name "wpn_relocate" -Status $(if ($h) { "PASS" } else { "FAIL" }) `
                -Metrics @{ host = $h; joinAdvisory = $j })
}

# world_weapon_drop / world_armor_drop (W2): host drops gear; join relocates its own
# copy to the ground. Both scenario variants emit the same WDROP log contract, so one
# oracle serves both - $GateName picks the verdict label (weapon_drop / armor_drop).
function Test-WeaponDrop {
    param([string]$HostFile, [string]$JoinFile, [string]$GateName = "weapon_drop")
    $tag = $GateName.ToUpper().Replace("_", "-")   # WEAPON-DROP / ARMOR-DROP
    $rxHost = 'WDROP verdict role=host pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invAfter=(-?\d+) grndAfter=(-?\d+)'
    $rxJoin = 'WDROP verdict role=join pass=(\d+) sid=''([^'']*)'' invBase=(-?\d+) invMin=(-?\d+) grndMax=(-?\d+) relocated=(\d+)'
    $hostOk = $false
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invAfter = [int]$g[4].Value; $grnd = [int]$g[5].Value
            $hostOk = ($invBase -ge 1) -and ($invAfter -le $invBase - 1) -and ($grnd -ge 1)
            Write-Host ("  $tag [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - dropped gear to ground (invBase=$invBase invAfter=$invAfter ground=$grnd)")
        } else { Write-Host "  $tag [host] FAIL - no host WDROP verdict" }
    }
    $joinOk = $false
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $invBase = [int]$g[3].Value; $invMin = [int]$g[4].Value; $grndMax = [int]$g[5].Value
            $joinOk = ($invBase -ge 1) -and ($invMin -le $invBase - 1) -and ($grndMax -ge 1)
            Write-Host ("  $tag [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - relocated own copy to ground (invBase=$invBase invMin=$invMin grndMax=$grndMax)")
            if (-not $joinOk -and $invMin -le $invBase - 1 -and $grndMax -lt 1) {
                Write-Host "    NOTE: weapon LEFT the bag but never appeared on the ground => destroyed by inv-reconcile, not conserved"
            }
        } else { Write-Host "  $tag [join] FAIL - no join WDROP verdict" }
    }
    $authored = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[wd\] DROP id=' -Quiet)
    $applied  = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[wd\] APPLY id=\d+ .* moved=1' -Quiet)
    Write-Host ("  $tag trace: host authored DROP=$authored, join APPLY moved=1=$applied")
    $ok = $hostOk -and $joinOk
    Write-Host ("  $tag " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name $GateName -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; authored = $authored; applied = $applied })
}

# weapon_loot: the host's owned leader acquires a NOVEL weapon (a sid in no shared-save
# inventory); the join must fabricate exactly one copy onto its driven leader via the
# inventory snapshot channel + the spike-451 weapon CREATE. Gates: both verdicts pass
# (arrived, persisted, max count never exceeded 1 - the zero-dupe requirement), the
# sids MATCH across clients, and the quality buckets agree within tolerance.
function Test-WeaponLoot {
    param([string]$HostFile, [string]$JoinFile)
    $rxHost = 'WLOOT verdict role=host pass=(\d+) sid=''([^'']*)'' added=(-?\d+) final=(-?\d+) max=(-?\d+) qual=(-?\d+)'
    $rxJoin = 'WLOOT verdict role=join pass=(\d+) sid=''([^'']*)'' final=(-?\d+) max=(-?\d+) qual=(-?\d+)'
    $hostOk = $false; $hostSid = ""; $hostQual = -1
    if (Test-Path $HostFile) {
        $hl = Select-String -Path $HostFile -Pattern $rxHost -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $hl) {
            $g = $hl.Matches[0].Groups
            $hostSid = $g[2].Value; $hostQual = [int]$g[6].Value
            $added = [int]$g[3].Value; $final = [int]$g[4].Value; $max = [int]$g[5].Value
            $hostOk = ($added -eq 1) -and ($final -eq 1) -and ($max -eq 1)
            Write-Host ("  WEAPON-LOOT [host] " + $(if ($hostOk) { "PASS" } else { "FAIL" }) +
                        " - acquired novel weapon sid='$hostSid' (added=$added final=$final max=$max qual=$hostQual)")
        } else { Write-Host "  WEAPON-LOOT [host] FAIL - no host WLOOT verdict" }
    }
    $joinOk = $false; $joinSid = ""; $joinQual = -1
    if (Test-Path $JoinFile) {
        $jl = Select-String -Path $JoinFile -Pattern $rxJoin -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $jl) {
            $g = $jl.Matches[0].Groups
            $joinSid = $g[2].Value; $joinQual = [int]$g[5].Value
            $final = [int]$g[3].Value; $max = [int]$g[4].Value
            $joinOk = ($final -eq 1) -and ($max -eq 1)
            Write-Host ("  WEAPON-LOOT [join] " + $(if ($joinOk) { "PASS" } else { "FAIL" }) +
                        " - peer copy fabricated sid='$joinSid' (final=$final max=$max qual=$joinQual)")
            if (-not $joinOk -and $final -eq 0) {
                Write-Host "    NOTE: weapon never appeared on the join => acquisition did not propagate (snapshot or CREATE failed)"
            }
            if (-not $joinOk -and $max -gt 1) {
                Write-Host "    NOTE: transient count $max > 1 => fabrication raced the conservation channel into a dupe"
            }
        } else { Write-Host "  WEAPON-LOOT [join] FAIL - no join WLOOT verdict" }
    }
    $sidMatch = ($hostSid -ne "") -and ($hostSid -eq $joinSid)
    if (-not $sidMatch) { Write-Host "  WEAPON-LOOT sid MISMATCH host='$hostSid' join='$joinSid'" }
    $qualOk = ($hostQual -ge 0) -and ($joinQual -ge 0) -and ([Math]::Abs($hostQual - $joinQual) -le 5)
    Write-Host ("  WEAPON-LOOT quality host=$hostQual join=$joinQual match=" + $(if ($qualOk) { "yes" } else { "NO" }))
    $ok = $hostOk -and $joinOk -and $sidMatch -and $qualOk
    Write-Host ("  WEAPON-LOOT " + $(if ($ok) { "PASS" } else { "FAIL" }))
    return (Add-GateResult -Name "weapon_loot" -Status $(if ($ok) { "PASS" } else { "FAIL" }) `
                -Metrics @{ hostOk = $hostOk; joinOk = $joinOk; sid = $hostSid;
                            sidMatch = $sidMatch; hostQual = $hostQual; joinQual = $joinQual })
}

# ---- Runtime-spawn sync oracles (protocol 21) ---------------------------------------

# Parse "SCENARIO SPAWN leg=<leg> hand=t,c,cs,i,s" lines from $File into a
# hashtable leg -> list of hand strings (readObjectHand order, matching the
# replicator's "[spawn] ..." log keys).
function Get-SpawnHands {
    param([string]$File)
    $legs = @{}
    if (-not (Test-Path $File)) { return $legs }
    foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO SPAWN leg=(\w+) hand=([\d,]+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        if (-not $legs.ContainsKey($g[1].Value)) { $legs[$g[1].Value] = New-Object System.Collections.ArrayList }
        [void]$legs[$g[1].Value].Add($g[2].Value)
    }
    return $legs
}

# Convert a t,c,cs,i,s hand string (readObjectHand / [spawn] log order) into the
# i,s,t,c,cs order the SCENARIO MEMBER/RECV/PROXY series lines use.
function Convert-SpawnHandToSeriesKey {
    param([string]$Hand)
    $p = $Hand.Split(',')
    if ($p.Count -ne 5) { return $null }
    return "$($p[3]),$($p[4]),$($p[0]),$($p[1]),$($p[2])"
}

# How many of the join-local spawned hands (t,c,cs,i,s strings) drew an
# "[authority] suppress NPC hand=i,s" line in the join log?
function Measure-JoinSpawnSuppression {
    param([string]$JoinFile, $JoinHands)
    $supp = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[authority\] suppress NPC hand=(\d+,\d+)' -ErrorAction SilentlyContinue)) {
        $supp[$m.Matches[0].Groups[1].Value] = $true
    }
    $n = 0
    foreach ($h in $JoinHands) {
        $p = $h.Split(',')
        if ($p.Count -eq 5 -and $supp.ContainsKey("$($p[3]),$($p[4])")) { $n++ }
    }
    return $n
}

# spawn_probe (protocol 21 phase 0, spawnSync FORCED OFF): baseline the two
# runtime-spawn failure modes with evidence.
#   * mechanism proof (gates): the host spawned runtime squads in BOTH legs,
#     the join spawned its local squad, and the JOIN logged
#     "[spawn] unresolved hand=..." for at least one host near-spawn hand -
#     the host-only-enemies desync, observed rather than assumed.
#   * finding (metrics only, never gates): whether enforceHostAuthority
#     suppressed the join-local spawns ([authority] suppress) - measuring
#     that gap is the probe's PURPOSE, so both outcomes are a valid result.
function Test-SpawnProbe {
    param([string]$HostFile, [string]$JoinFile)
    $hostLegs = Get-SpawnHands -File $HostFile
    $joinLegs = Get-SpawnHands -File $JoinFile
    $near = if ($hostLegs.ContainsKey('near')) { @($hostLegs['near']) } else { @() }
    $far  = if ($hostLegs.ContainsKey('far'))  { @($hostLegs['far'])  } else { @() }
    $jsp  = if ($joinLegs.ContainsKey('join')) { @($joinLegs['join']) } else { @() }
    $why = @()
    if ($near.Count -eq 0) { $why += "host never spawned the near leg" }
    if ($far.Count -eq 0)  { $why += "host never spawned the far leg" }
    if ($jsp.Count -eq 0)  { $why += "join never spawned its local squad" }

    # Failure mode 1 evidence: join couldn't resolve the host's runtime hands.
    $unres = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[spawn\] unresolved hand=([\d,]+)' -ErrorAction SilentlyContinue)) {
        $unres[$m.Matches[0].Groups[1].Value] = $true
    }
    $nearUnres = @($near | Where-Object { $unres.ContainsKey($_) }).Count
    $farUnres  = @($far  | Where-Object { $unres.ContainsKey($_) }).Count
    if ($near.Count -gt 0 -and $nearUnres -eq 0) {
        $why += "join logged no [spawn] unresolved line for any near-spawn hand (hand collision? stream gap?)"
    }

    # Failure mode 2 finding: did suppression catch the join-local spawns?
    $jSupp = Measure-JoinSpawnSuppression -JoinFile $JoinFile -JoinHands $jsp

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SPAWN-PROBE $v - near=$($near.Count) far=$($far.Count) joinLocal=$($jsp.Count) nearUnresolved=$nearUnres farUnresolved=$farUnres joinSuppressed=$jSupp/$($jsp.Count) $detail"
    if ($jsp.Count -gt 0 -and $jSupp -lt $jsp.Count) {
        Write-Host "    FINDING: $($jsp.Count - $jSupp) join-local runtime spawn(s) were NEVER suppressed - the phase-2 suppression gap, now measured"
    }
    return (Add-GateResult -Name "spawn_probe" -Status $v `
                -Metrics @{ near = $near.Count; far = $far.Count; joinLocal = $jsp.Count
                            nearUnresolved = $nearUnres; farUnresolved = $farUnres
                            joinSuppressed = $jSupp } -Detail $detail)
}

# shop_probe (protocol 22 phase 0): money + vendor-trading EVIDENCE gate.
# Gates only that the probe produced its evidence:
#   1. the host enumerated at least one vendor (SCENARIO VENDOR),
#   2. both sides logged a readable per-tab WALLET series (money >= 0),
#   3. both sides logged their scripted SHOPBUY attempt (any res - the result
#      is a finding, not a gate).
# Everything else - did the host's purchase cross (wallet/vendor stock deltas
# on the join), did the join-side buy against a driven vendor copy work at all
# - is MEASURED and reported as FINDINGs; those findings gate the 1b/1c design,
# not this oracle.
function Get-WalletSeries {
    param([string]$File)
    # rank -> ordered list of @{ t; money }
    $out = @{}
    if (-not (Test-Path $File)) { return $out }
    foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO WALLET rank=(\d+) money=(-?\d+) t=(\d+)' -ErrorAction SilentlyContinue)) {
        $r = [int]$m.Matches[0].Groups[1].Value
        if (-not $out.ContainsKey($r)) { $out[$r] = New-Object System.Collections.ArrayList }
        [void]$out[$r].Add(@{ t = [long]$m.Matches[0].Groups[3].Value
                              money = [int]$m.Matches[0].Groups[2].Value })
    }
    return $out
}
function Test-ShopProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1. Vendor enumeration (host side is the authority for "a vendor exists").
    # Vendors are keyed by the trader CHARACTER's save-stable hand (thand): the
    # ShopTrader wrapper's own serial is runtime-minted and never matches across
    # clients (run 103018 finding).
    $vendorRegex = 'SCENARIO VENDOR hand=[\d,]+ money=(-?\d+) stock=(-?\d+) qty=(-?\d+) src=(-?\d+) thand=([\d,]+)'
    $hostVendorLines = @(Select-String -Path $HostFile -Pattern $vendorRegex -ErrorAction SilentlyContinue)
    $joinVendorLines = @(Select-String -Path $JoinFile -Pattern $vendorRegex -ErrorAction SilentlyContinue)
    if ($hostVendorLines.Count -eq 0) { $why += "host enumerated no vendor (save has none in range?)" }

    # 2. Wallet series readable on both sides.
    $hw = Get-WalletSeries -File $HostFile
    $jw = Get-WalletSeries -File $JoinFile
    $hostWalletOk = $false
    foreach ($r in $hw.Keys) { foreach ($s in $hw[$r]) { if ($s.money -ge 0) { $hostWalletOk = $true } } }
    $joinWalletOk = $false
    foreach ($r in $jw.Keys) { foreach ($s in $jw[$r]) { if ($s.money -ge 0) { $joinWalletOk = $true } } }
    if (-not $hostWalletOk) { $why += "host wallet series unreadable (accessors unresolved?)" }
    if (-not $joinWalletOk) { $why += "join wallet series unreadable" }

    # 3. Both scripted buy attempts logged.
    $hostBuy = Select-String -Path $HostFile -Pattern 'SCENARIO SHOPBUY who=host res=(-?\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinBuy = Select-String -Path $JoinFile -Pattern 'SCENARIO SHOPBUY who=join res=(-?\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    $hostBuyRes = if ($hostBuy) { [int]$hostBuy.Matches[0].Groups[1].Value } else { -9 }
    $joinBuyRes = if ($joinBuy) { [int]$joinBuy.Matches[0].Groups[1].Value } else { -9 }
    if ($null -eq $hostBuy) { $why += "host never logged its SHOPBUY attempt" }
    if ($null -eq $joinBuy) { $why += "join never logged its SHOPBUY attempt" }

    # 4. Both scripted wallet writes logged (the 1b apply-primitive check + the
    #    decisive crossing lever: side-distinct sentinels, host 5000 / join 7000).
    $setRegex = 'SCENARIO WALLETSET who=(host|join) rank=(\d+) target=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+)'
    $hostSet = Select-String -Path $HostFile -Pattern $setRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinSet = Select-String -Path $JoinFile -Pattern $setRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostSet) { $why += "host never logged its WALLETSET" }
    if ($null -eq $joinSet) { $why += "join never logged its WALLETSET" }
    $hostSetOk = $false; $joinSetOk = $false
    if ($hostSet) { $hostSetOk = ($hostSet.Matches[0].Groups[4].Value -eq '1') -and ($hostSet.Matches[0].Groups[6].Value -eq $hostSet.Matches[0].Groups[3].Value) }
    if ($joinSet) { $joinSetOk = ($joinSet.Matches[0].Groups[4].Value -eq '1') -and ($joinSet.Matches[0].Groups[6].Value -eq $joinSet.Matches[0].Groups[3].Value) }

    # FINDING A: final wallet divergence per rank (does anything cross today?).
    $rankDiv = @{}
    foreach ($r in $hw.Keys) {
        if (-not $jw.ContainsKey($r)) { continue }
        $hEnd = $hw[$r][$hw[$r].Count - 1].money
        $jEnd = $jw[$r][$jw[$r].Count - 1].money
        if ($hEnd -ge 0 -and $jEnd -ge 0) { $rankDiv[$r] = ($hEnd - $jEnd) }
    }
    # FINDING B: vendor money/stock end-state per trader hand, host vs join.
    $vend = @{}
    foreach ($pair in @(@('host', $hostVendorLines), @('join', $joinVendorLines))) {
        $side = $pair[0]
        foreach ($m in $pair[1]) {
            $hand = $m.Matches[0].Groups[5].Value
            if ($hand -eq '0,0,0,0,0') { continue } # trader hand unread
            if (-not $vend.ContainsKey($hand)) { $vend[$hand] = @{} }
            $vend[$hand][$side] = @{ money = [int]$m.Matches[0].Groups[1].Value
                                     stock = [int]$m.Matches[0].Groups[2].Value }
        }
    }
    $vendorDiverged = 0; $vendorShared = 0
    foreach ($hand in $vend.Keys) {
        if ($vend[$hand].ContainsKey('host') -and $vend[$hand].ContainsKey('join')) {
            $vendorShared++
            $h = $vend[$hand]['host']; $j = $vend[$hand]['join']
            if (($h.money -ne $j.money) -or ($h.stock -ne $j.stock)) { $vendorDiverged++ }
        }
    }

    # FINDING C: did either sentinel CROSS? (host sets its rank to 5000, join
    # sets its rank to 7000; the peer's final money for that rank tells us.)
    $crossToJoin = $null; $crossToHost = $null
    if ($hostSet -and $hostSetOk) {
        $hr = [int]$hostSet.Matches[0].Groups[2].Value
        $ht = [int]$hostSet.Matches[0].Groups[3].Value
        if ($jw.ContainsKey($hr)) { $crossToJoin = ($jw[$hr][$jw[$hr].Count - 1].money -eq $ht) }
    }
    if ($joinSet -and $joinSetOk) {
        $jr = [int]$joinSet.Matches[0].Groups[2].Value
        $jt = [int]$joinSet.Matches[0].Groups[3].Value
        if ($hw.ContainsKey($jr)) { $crossToHost = ($hw[$jr][$hw[$jr].Count - 1].money -eq $jt) }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SHOP-PROBE $v - hostVendors=$($hostVendorLines.Count) joinVendors=$($joinVendorLines.Count) hostBuyRes=$hostBuyRes joinBuyRes=$joinBuyRes hostSetOk=$hostSetOk joinSetOk=$joinSetOk $detail"
    foreach ($r in ($rankDiv.Keys | Sort-Object)) {
        $tag = if ($rankDiv[$r] -eq 0) { "converged" } else { "DIVERGED by $($rankDiv[$r])" }
        Write-Host "    FINDING: tab rank $r final wallet host-join = $tag"
    }
    if ($null -ne $crossToJoin) {
        Write-Host ("    FINDING: host wallet sentinel " + $(if ($crossToJoin) { "CROSSED to the join (something syncs money today!)" } else { "did NOT cross to the join (the 1b gap, as predicted)" }))
    }
    if ($null -ne $crossToHost) {
        Write-Host ("    FINDING: join wallet sentinel " + $(if ($crossToHost) { "CROSSED to the host" } else { "did NOT cross to the host" }))
    }
    if ($vendorShared -gt 0) {
        Write-Host "    FINDING: $vendorDiverged/$vendorShared cross-visible vendor(s) ended with diverged money/stock"
    }
    if ($joinBuyRes -eq 1) { Write-Host "    FINDING: join-side buyItem against its local vendor copy SUCCEEDED (engine allows it)" }
    elseif ($joinBuyRes -eq 0) { Write-Host "    FINDING: join-side buyItem refused/no-stock (res=0)" }
    elseif ($joinBuy) { Write-Host "    FINDING: join-side buy attempt failed hard (res=$joinBuyRes) - 1c must redesign around host-forwarded purchase" }
    $rankDivMetric = @{}
    foreach ($r in $rankDiv.Keys) { $rankDivMetric["rank$r"] = $rankDiv[$r] }
    return (Add-GateResult -Name "shop_probe" -Status $v `
                -Metrics @{ hostVendors = $hostVendorLines.Count; joinVendors = $joinVendorLines.Count
                            hostBuyRes = $hostBuyRes; joinBuyRes = $joinBuyRes
                            hostSetOk = $hostSetOk; joinSetOk = $joinSetOk
                            crossToJoin = $crossToJoin; crossToHost = $crossToHost
                            vendorShared = $vendorShared; vendorDiverged = $vendorDiverged
                            walletDiv = $rankDivMetric } -Detail $detail)
}

# money_sync (protocol 22, moneySync ON): the wallet-channel gate. Same script
# as shop_probe minus the vendor legs: each side writes a side-distinct wallet
# sentinel into the tab it OWNS (host rank0=5000, join rank1=7000) and the
# channel must carry it across - the gate is CONVERGENCE:
#   1. both WALLETSET writes succeeded (apply primitive works);
#   2. the peer's WALLET series ends at the sender's sentinel for that rank
#      (a "[money] RECV" landed and stuck);
#   3. every rank readable on both sides ends converged (no drift elsewhere).
function Test-MoneySync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    $hw = Get-WalletSeries -File $HostFile
    $jw = Get-WalletSeries -File $JoinFile
    if ($hw.Keys.Count -eq 0) { $why += "host logged no WALLET series" }
    if ($jw.Keys.Count -eq 0) { $why += "join logged no WALLET series" }

    $setRegex = 'SCENARIO WALLETSET who=(host|join) rank=(\d+) target=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+)'
    $hostSet = Select-String -Path $HostFile -Pattern $setRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinSet = Select-String -Path $JoinFile -Pattern $setRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostSet) { $why += "host never logged its WALLETSET" }
    if ($null -eq $joinSet) { $why += "join never logged its WALLETSET" }
    foreach ($pair in @(@('host', $hostSet), @('join', $joinSet))) {
        $side = $pair[0]; $s = $pair[1]
        if ($s -and (($s.Matches[0].Groups[4].Value -ne '1') -or
                     ($s.Matches[0].Groups[6].Value -ne $s.Matches[0].Groups[3].Value))) {
            $why += "$side WALLETSET write failed (ok/after mismatch)"
        }
    }

    # Crossing: the peer's final money at the sender's rank equals the sentinel.
    $crossed = 0; $expected = 0
    foreach ($leg in @(@($hostSet, $jw, 'host->join'), @($joinSet, $hw, 'join->host'))) {
        $s = $leg[0]; $peer = $leg[1]; $tag = $leg[2]
        if ($null -eq $s) { continue }
        $rank = [int]$s.Matches[0].Groups[2].Value
        $tgt  = [int]$s.Matches[0].Groups[3].Value
        $expected++
        if (-not $peer.ContainsKey($rank)) { $why += "$tag rank $rank absent from peer WALLET series"; continue }
        $end = $peer[$rank][$peer[$rank].Count - 1].money
        if ($end -eq $tgt) { $crossed++ }
        else { $why += "$tag sentinel did not cross (peer rank $rank ended $end, want $tgt)" }
    }

    # No drift on any co-visible rank.
    $diverged = @()
    foreach ($r in $hw.Keys) {
        if (-not $jw.ContainsKey($r)) { continue }
        $hEnd = $hw[$r][$hw[$r].Count - 1].money
        $jEnd = $jw[$r][$jw[$r].Count - 1].money
        if ($hEnd -ge 0 -and $jEnd -ge 0 -and $hEnd -ne $jEnd) { $diverged += "rank$r($hEnd/$jEnd)" }
    }
    if ($diverged.Count -gt 0) { $why += ("final wallets diverged: " + ($diverged -join ", ")) }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  MONEY-SYNC $v - crossed=$crossed/$expected $detail"
    return (Add-GateResult -Name "money_sync" -Status $v `
                -Metrics @{ crossed = $crossed; expected = $expected
                            diverged = $diverged.Count } -Detail $detail)
}

# recruit_probe (protocol 23 phase 0): mid-session recruitment evidence.
# Gates only that the script RAN (both legs logged a RECRUIT line + the TABS
# census series exists on both sides); the design-shaping findings are:
#   * per side/leg: did recruit() succeed, did the hand CONTAINER change, did
#     index/serial survive?
#   * did the recruit mint a NEW tab (container census grew) - and did the
#     PRE-EXISTING containers keep their sorted order (rank stability)?
#   * peer visibility: does the recruiter's new hand show up in the peer's
#     [spawn] unresolved/REQ/proxy stream (the accidental-proxy hazard)?
function Test-RecruitProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $rRegex = 'SCENARIO RECRUIT who=(host|join) leg=(baked|runtime) res=(-?\d+) before=([\d,]+) after=([\d,]+)'
    $legs = @{}
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $side = $pair[0]
        foreach ($m in @(Select-String -Path $pair[1] -Pattern $rRegex -ErrorAction SilentlyContinue)) {
            $legs["$side/$($m.Matches[0].Groups[2].Value)"] = @{
                res    = [int]$m.Matches[0].Groups[3].Value
                before = $m.Matches[0].Groups[4].Value
                after  = $m.Matches[0].Groups[5].Value
            }
        }
    }
    foreach ($k in @('host/baked', 'host/runtime', 'join/baked', 'join/runtime')) {
        if (-not $legs.ContainsKey($k)) { $why += "$k never logged its RECRUIT" }
    }

    # TABS census series (both sides).
    function Get-TabsSeries([string]$File) {
        $out = New-Object System.Collections.ArrayList
        foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO TABS n=(\d+) squad=(\d+) list=(\S*) t=(\d+)' -ErrorAction SilentlyContinue)) {
            [void]$out.Add(@{ n = [int]$m.Matches[0].Groups[1].Value
                              squad = [int]$m.Matches[0].Groups[2].Value
                              list = $m.Matches[0].Groups[3].Value
                              t = [long]$m.Matches[0].Groups[4].Value })
        }
        return $out
    }
    $hTabs = Get-TabsSeries $HostFile
    $jTabs = Get-TabsSeries $JoinFile
    if ($hTabs.Count -eq 0) { $why += "host logged no TABS census" }
    if ($jTabs.Count -eq 0) { $why += "join logged no TABS census" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RECRUIT-PROBE $v - $detail"

    # FINDINGS per leg.
    foreach ($k in ($legs.Keys | Sort-Object)) {
        $l = $legs[$k]
        $b = $l.before -split ','; $a = $l.after -split ','
        $contChanged = ($b[1] -ne $a[1]) -or ($b[2] -ne $a[2])
        $idxSurvived = ($b[3] -eq $a[3]) -and ($b[4] -eq $a[4])
        Write-Host "    FINDING: $k res=$($l.res) containerChanged=$contChanged indexSerialSurvived=$idxSurvived (before=$($l.before) after=$($l.after))"
    }
    # FINDING: tab census growth + pre-existing order stability per side.
    foreach ($pair in @(@('host', $hTabs), @('join', $jTabs))) {
        $side = $pair[0]; $s = $pair[1]
        if ($s.Count -lt 2) { continue }
        $first = $s[0]; $last = $s[$s.Count - 1]
        $stable = $last.list.StartsWith($first.list)
        Write-Host "    FINDING: $side tabs $($first.n)->$($last.n) squad $($first.squad)->$($last.squad) preexistingOrderStable=$stable (first=$($first.list) last=$($last.list))"
    }
    # FINDING: peer spawn-channel reaction to each recruiter's AFTER hand.
    foreach ($k in ($legs.Keys | Sort-Object)) {
        $l = $legs[$k]
        if ($l.res -ne 1) { continue }
        $peerFile = if ($k.StartsWith('host')) { $JoinFile } else { $HostFile }
        $hand = [regex]::Escape($l.after)
        $unres = @(Select-String -Path $peerFile -Pattern "\[spawn\] (unresolved|REQ) hand=$hand" -ErrorAction SilentlyContinue).Count
        $proxy = @(Select-String -Path $peerFile -Pattern "\[spawn\] proxy BOUND hand=$hand" -ErrorAction SilentlyContinue).Count
        Write-Host "    FINDING: $k peer saw unresolved/REQ=$unres proxyBound=$proxy for the recruited hand"
    }
    return (Add-GateResult -Name "recruit_probe" -Status $v `
                -Metrics @{ legs = $legs.Count } -Detail $detail)
}

# recruit_sync (protocol 23): the gated recruitment-convergence oracle. Each
# side recruits a BAKED world NPC and a RUNTIME spawn; the gate is that every
# recruited hand CONVERGED on the peer:
#   1. all four RECRUIT legs res=1 (the local recruits succeeded);
#   2. each recruiter authored its reliable edge ("[recruit] EVT send" x2);
#   3. for each leg the PEER bound ONE local body to the new hand - a
#      "[recruit] REKEY ... ok=1" (its existing copy re-keyed; the baked path)
#      or a "[spawn] proxy BOUND" (minted via the bidirectional describe
#      channel; the runtime path);
#   4. the peer then TRACKED the hand (a SCENARIO PROXY series exists for it -
#      the bound body is actually driven, not just registered).
# Duplicate-mint evidence (REKEY ok=1 AND proxy BOUND for the same hand) fails
# the gate: the whole point over the probe baseline is ONE body per recruit.
function Test-RecruitSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $rRegex = 'SCENARIO RECRUIT who=(host|join) leg=(baked|runtime) res=(-?\d+) before=([\d,]+) after=([\d,]+)'
    $legs = @{}
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $side = $pair[0]
        foreach ($m in @(Select-String -Path $pair[1] -Pattern $rRegex -ErrorAction SilentlyContinue)) {
            $legs["$side/$($m.Matches[0].Groups[2].Value)"] = @{
                res   = [int]$m.Matches[0].Groups[3].Value
                after = $m.Matches[0].Groups[5].Value
            }
        }
    }
    foreach ($k in @('host/baked', 'host/runtime', 'join/baked', 'join/runtime')) {
        if (-not $legs.ContainsKey($k)) { $why += "$k never logged its RECRUIT" }
        elseif ($legs[$k].res -ne 1)    { $why += "$k recruit failed locally (res=$($legs[$k].res))" }
    }
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $sent = @(Select-String -Path $pair[1] -Pattern '\[recruit\] EVT send' -ErrorAction SilentlyContinue).Count
        if ($sent -lt 2) { $why += "$($pair[0]) authored only $sent/2 EVT_RECRUIT edges" }
    }

    $converged = 0
    foreach ($k in ($legs.Keys | Sort-Object)) {
        $l = $legs[$k]
        if ($l.res -ne 1) { continue }
        $peerFile = if ($k.StartsWith('host')) { $JoinFile } else { $HostFile }
        $hand = [regex]::Escape($l.after)                       # t,c,cs,i,s
        $a = $l.after -split ','
        $handIS = [regex]::Escape("$($a[3]),$($a[4]),$($a[0]),$($a[1]),$($a[2])") # i,s,t,c,cs (PROXY line order)
        $rekey = @(Select-String -Path $peerFile -Pattern "\[recruit\] REKEY .* new=$hand ok=1" -ErrorAction SilentlyContinue).Count
        $bound = @(Select-String -Path $peerFile -Pattern "\[spawn\] proxy BOUND hand=$hand" -ErrorAction SilentlyContinue).Count
        $track = @(Select-String -Path $peerFile -Pattern "SCENARIO PROXY hand=$handIS" -ErrorAction SilentlyContinue).Count
        if (($rekey + $bound) -eq 0) { $why += "$k never converged on the peer (no REKEY, no proxy BOUND)" }
        elseif ($rekey -ge 1 -and $bound -ge 1) { $why += "$k DUPLICATED on the peer (rekeyed AND minted a proxy)" }
        else {
            if ($track -eq 0) { $why += "$k bound on the peer but never tracked (no PROXY series)" }
            else { $converged++ }
        }
        Write-Host "    FINDING: $k peer rekey=$rekey proxyBound=$bound proxyTrack=$track"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RECRUIT-SYNC $v - converged=$converged/4 $detail"
    return (Add-GateResult -Name "recruit_sync" -Status $v `
                -Metrics @{ converged = $converged } -Detail $detail)
}

# Shared SQTABS parser (protocol 35): ordered series of @{n; squad; list; t}
# where list = "container:serial:count|..." sorted ascending (the same order
# the Replicator ranks the ownership partition on).
function Get-SqTabsSeries {
    param([string]$File)
    $out = New-Object System.Collections.ArrayList
    foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO SQTABS n=(\d+) squad=(\d+) list=(\S*) t=(\d+)' -ErrorAction SilentlyContinue)) {
        [void]$out.Add(@{ n = [int]$m.Matches[0].Groups[1].Value
                          squad = [int]$m.Matches[0].Groups[2].Value
                          list = $m.Matches[0].Groups[3].Value
                          t = [long]$m.Matches[0].Groups[4].Value })
    }
    return $out
}

# The rank-shift finding: where did each of the FIRST census sample's tab
# containers land in the LAST sample's sorted order? A shifted index = the
# ownership partition reshuffled mid-session (the gap-2 hazard).
function Get-SqRankShifts {
    param($Series)
    if ($Series.Count -lt 2) { return @() }
    $firstPairs = @($Series[0].list -split '\|' | ForEach-Object { ($_ -split ':')[0..1] -join ':' })
    $lastPairs  = @($Series[$Series.Count - 1].list -split '\|' | ForEach-Object { ($_ -split ':')[0..1] -join ':' })
    $shifts = @()
    for ($i = 0; $i -lt $firstPairs.Count; $i++) {
        $newIdx = [array]::IndexOf($lastPairs, $firstPairs[$i])
        $shifts += "$($firstPairs[$i]):$i->$newIdx"
    }
    return $shifts
}

$script:SqMoveRegex = 'SCENARIO SQMOVE who=(host|join) lever=(-?\d+) rc=(-?\d+) before=([\d,]+) after=([\d,]+) t=(\d+)'
$script:SqEdgeRegex = 'SCENARIO SQEDGE who=(host|join) before=([\d,]+) after=([\d,]+) t=(\d+)'

# squad_probe (protocol 35 phase 0): squad-move baseline diagnostic (squadSync
# forced OFF). Gates the LOCAL legs: both sides logged their SQTABS census and
# their lever-0 separate attempt, and - when a move LANDED (rc=1) - the
# pointer-diff SQEDGE caught the same before/after pair (the detection
# mechanism protocol 35 rides). Everything else is FINDINGs feeding the design:
#   * identity: which hand fields a move re-keys (container vs index/serial);
#   * rank reshuffle: did the pre-existing tabs' sorted positions shift when
#     the new tab appeared (the ownership hazard, measured);
#   * move-back levers: does setFaction (1) / addCharacterAt (2) land a member
#     in an EXISTING tab, and does the returning member get its original hand
#     back (no second re-key) or a fresh one;
#   * peer reaction: unresolved-hand telemetry / authority suppression around
#     each landed move (the divergence protocol 35 closes).
function Test-SquadProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $moves = @{}
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $side = $pair[0]
        foreach ($m in @(Select-String -Path $pair[1] -Pattern $script:SqMoveRegex -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            if ($g[1].Value -ne $side) { continue } # each side logs only its own moves
            $moves["$side/L$($g[2].Value)"] = @{
                lever = [int]$g[2].Value; rc = [int]$g[3].Value
                before = $g[4].Value; after = $g[5].Value; t = [long]$g[6].Value
            }
        }
    }
    # Host leads with the separate (L0); the join's tab is solo (a separate
    # would no-op), so its first scripted move is the lever-1 move-in.
    foreach ($k in @('host/L0', 'join/L1')) {
        if (-not $moves.ContainsKey($k)) { $why += "$k never logged its SQMOVE" }
    }
    $hTabs = Get-SqTabsSeries $HostFile
    $jTabs = Get-SqTabsSeries $JoinFile
    if ($hTabs.Count -eq 0) { $why += "host logged no SQTABS census" }
    if ($jTabs.Count -eq 0) { $why += "join logged no SQTABS census" }

    # Detection gate: every LANDED move must be mirrored by a pointer-diff
    # SQEDGE with the same before/after pair on the mover's own log.
    foreach ($k in ($moves.Keys | Sort-Object)) {
        $l = $moves[$k]
        if ($l.rc -ne 1) { continue }
        $file = if ($k.StartsWith('host')) { $HostFile } else { $JoinFile }
        $pat = "SCENARIO SQEDGE who=\w+ before=$([regex]::Escape($l.before)) after=$([regex]::Escape($l.after))"
        $hit = @(Select-String -Path $file -Pattern $pat -ErrorAction SilentlyContinue).Count
        if ($hit -eq 0) { $why += "$k landed but the pointer-diff never caught it (no matching SQEDGE)" }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SQUAD-PROBE $v - $detail"

    # FINDINGS: per-move identity evidence.
    foreach ($k in ($moves.Keys | Sort-Object)) {
        $l = $moves[$k]
        $b = $l.before -split ','; $a = $l.after -split ','
        $contChanged = ($b[1] -ne $a[1]) -or ($b[2] -ne $a[2])
        $idxSurvived = ($b[3] -eq $a[3]) -and ($b[4] -eq $a[4])
        Write-Host "    FINDING: $k rc=$($l.rc) containerChanged=$contChanged indexSerialSurvived=$idxSurvived (before=$($l.before) after=$($l.after))"
    }
    # FINDING: did the host's move-back restore the ORIGINAL hand (no second
    # re-key)? (The join's L0 is a separate-out, not a move-back.)
    if ($moves.ContainsKey('host/L0')) {
        $orig = $moves['host/L0'].before
        foreach ($lev in @(1, 2)) {
            $k = "host/L$lev"
            if (-not $moves.ContainsKey($k) -or $moves[$k].rc -ne 1) { continue }
            $restored = ($moves[$k].after -eq $orig)
            Write-Host "    FINDING: $k move-back restoredOriginalHand=$restored"
        }
    }
    # FINDING: rank stability of the pre-existing tabs (the reshuffle hazard).
    foreach ($pair in @(@('host', $hTabs), @('join', $jTabs))) {
        $shifts = Get-SqRankShifts -Series $pair[1]
        if ($shifts.Count -gt 0) {
            $moved = @($shifts | Where-Object { $_ -match ':(\d+)->(\d+)$' -and $Matches[1] -ne $Matches[2] })
            Write-Host "    FINDING: $($pair[0]) tab ranks first->last [$($shifts -join ' ')] shifted=$($moved.Count)"
        }
    }
    # FINDING: peer reaction to each landed move's NEW hand (sync OFF: the
    # expected gap - unresolved stream key / authority suppression churn).
    foreach ($k in ($moves.Keys | Sort-Object)) {
        $l = $moves[$k]
        if ($l.rc -ne 1) { continue }
        $peerFile = if ($k.StartsWith('host')) { $JoinFile } else { $HostFile }
        $a = $l.after -split ','
        $unres = @(Select-String -Path $peerFile -Pattern "\[spawn\] (unresolved|REQ) hand=$([regex]::Escape($l.after))" -ErrorAction SilentlyContinue).Count
        $supp  = @(Select-String -Path $peerFile -Pattern "\[authority\] suppress NPC hand=$($a[3]),$($a[4])" -ErrorAction SilentlyContinue).Count
        Write-Host "    FINDING: $k peer saw unresolved/REQ=$unres authoritySuppress=$supp for the moved hand"
    }
    return (Add-GateResult -Name "squad_probe" -Status $v `
                -Metrics @{ moves = $moves.Count } -Detail $detail)
}

# squad_sync (protocol 35): the gated squad-management convergence oracle.
# Host separates a member out and moves it back (L0 then L1/L2); the join
# moves its solo member into the host tab and separates it back out (L1/L2
# then L0). The gates:
#   1. every scripted move LANDED locally (rc=1; the join's L2 only fires if
#      its L1 refused, so either counts as the move-in);
#   2. the mover authored a reliable edge per landed move ("[squad] EVT send"
#      with that exact before/after pair);
#   3. the PEER re-keyed its local body onto each new hand ("[squad] REKEY
#      ... ok=1") - and never minted a duplicate proxy for it ("[spawn]
#      proxy BOUND" for a moved hand = the identity break came back);
#   4. no unresolved-hand storm: at most one pre-rekey unresolved line per
#      moved hand on the peer (the reliable edge must win the batch race);
#   5. the peer TRACKED each side's finally-moved hand (SCENARIO PROXY
#      series - the re-keyed body is actually driven, not just bound);
#   6. the rank latch held: the pre-existing tabs' census positions are
#      IDENTICAL first-to-last sample on both sides (no ownership reshuffle).
function Test-SquadSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $moves = @{}
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $side = $pair[0]
        foreach ($m in @(Select-String -Path $pair[1] -Pattern $script:SqMoveRegex -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            if ($g[1].Value -ne $side) { continue }
            $moves["$side/L$($g[2].Value)"] = @{
                lever = [int]$g[2].Value; rc = [int]$g[3].Value
                before = $g[4].Value; after = $g[5].Value; t = [long]$g[6].Value
            }
        }
    }
    # Gate 1: the script's moves all landed.
    if (-not $moves.ContainsKey('host/L0') -or $moves['host/L0'].rc -ne 1) {
        $why += "host separate (L0) missing or refused"
    }
    $hostBack = @('host/L1', 'host/L2') | Where-Object { $moves.ContainsKey($_) -and $moves[$_].rc -eq 1 }
    if (-not $hostBack) { $why += "host move-back (L1/L2) never landed" }
    $joinIn = @('join/L1', 'join/L2') | Where-Object { $moves.ContainsKey($_) -and $moves[$_].rc -eq 1 }
    if (-not $joinIn) { $why += "join move-in (L1/L2) never landed" }
    if (-not $moves.ContainsKey('join/L0') -or $moves['join/L0'].rc -ne 1) {
        $why += "join separate-out (L0) missing or refused"
    }

    # Gates 2-4 per landed move; track each side's chronologically-last hand.
    $finalHand = @{}
    foreach ($k in ($moves.Keys | Sort-Object)) {
        $l = $moves[$k]
        if ($l.rc -ne 1) { continue }
        $side = ($k -split '/')[0]
        $myFile = if ($side -eq 'host') { $HostFile } else { $JoinFile }
        $peerFile = if ($side -eq 'host') { $JoinFile } else { $HostFile }
        $be = [regex]::Escape($l.before); $ae = [regex]::Escape($l.after)
        $sent = @(Select-String -Path $myFile -Pattern "\[squad\] EVT send old=$be new=$ae" -ErrorAction SilentlyContinue).Count
        if ($sent -eq 0) { $why += "$k landed but authored no EVT_SQUAD_MOVE edge" }
        $rekey = @(Select-String -Path $peerFile -Pattern "\[squad\] REKEY old=$be new=$ae ok=1" -ErrorAction SilentlyContinue).Count
        if ($rekey -eq 0) { $why += "$k edge never re-keyed on the peer (no REKEY ok=1)" }
        $bound = @(Select-String -Path $peerFile -Pattern "\[spawn\] proxy BOUND hand=$ae" -ErrorAction SilentlyContinue).Count
        if ($bound -gt 0) { $why += "$k DUPLICATED on the peer (proxy minted despite the re-key)" }
        $unres = @(Select-String -Path $peerFile -Pattern "\[spawn\] unresolved hand=$ae" -ErrorAction SilentlyContinue).Count
        if ($unres -gt 1) { $why += "$k unresolved-hand storm on the peer ($unres lines)" }
        Write-Host "    FINDING: $k sent=$sent rekey=$rekey proxyBound=$bound unresolved=$unres"
        if (-not $finalHand.ContainsKey($side) -or $l.t -gt $finalHand[$side].t) {
            $finalHand[$side] = @{ after = $l.after; t = $l.t }
        }
    }
    # Gate 5: the peer tracked each side's finally-moved hand.
    foreach ($side in $finalHand.Keys) {
        $peerFile = if ($side -eq 'host') { $JoinFile } else { $HostFile }
        $a = $finalHand[$side].after -split ','
        $handIS = [regex]::Escape("$($a[3]),$($a[4]),$($a[0]),$($a[1]),$($a[2])") # i,s,t,c,cs (PROXY order)
        $track = @(Select-String -Path $peerFile -Pattern "SCENARIO PROXY hand=$handIS" -ErrorAction SilentlyContinue).Count
        if ($track -eq 0) { $why += "$side's moved member never tracked on the peer (no PROXY series)" }
        Write-Host "    FINDING: $side final hand $($finalHand[$side].after) peerTrack=$track"
    }
    # Gate 6: rank-latch proof - pre-existing tab positions stable per side.
    foreach ($pair in @(@('host', (Get-SqTabsSeries $HostFile)), @('join', (Get-SqTabsSeries $JoinFile)))) {
        $side = $pair[0]
        if ($pair[1].Count -eq 0) { $why += "$side logged no SQTABS census"; continue }
        $shifts = Get-SqRankShifts -Series $pair[1]
        # A VANISHED tab (->-1: the join's solo tab empties when its member
        # moves out) is not a reshuffle; only a SURVIVING pair changing its
        # sorted position would flip rank-keyed ownership.
        $moved = @($shifts | Where-Object { $_ -match ':(\d+)->(\d+)$' -and $Matches[1] -ne $Matches[2] })
        if ($moved.Count -gt 0) { $why += "$side pre-existing tab ranks SHIFTED [$($moved -join ' ')]" }
        Write-Host "    FINDING: $side tab ranks first->last [$($shifts -join ' ')]"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SQUAD-SYNC $v - $detail"
    return (Add-GateResult -Name "squad_sync" -Status $v `
                -Metrics @{ moves = $moves.Count } -Detail $detail)
}

# Shared FACREL parser: sid -> ordered series of @{us; them; enemy; erecip; t}.
function Get-FacRelSeries {
    param([string]$File)
    $out = @{}
    foreach ($m in @(Select-String -Path $File -Pattern "SCENARIO FACREL sid='([^']*)' us=(-?[\d.]+) them=(-?[\d.]+) enemy=(-?\d+) erecip=(-?\d+) ally=(-?\d+) t=(\d+)" -ErrorAction SilentlyContinue)) {
        $sid = $m.Matches[0].Groups[1].Value
        if (-not $out.ContainsKey($sid)) { $out[$sid] = New-Object System.Collections.ArrayList }
        [void]$out[$sid].Add(@{
            us     = [double]$m.Matches[0].Groups[2].Value
            them   = [double]$m.Matches[0].Groups[3].Value
            enemy  = [int]$m.Matches[0].Groups[4].Value
            erecip = [int]$m.Matches[0].Groups[5].Value
            t      = [long]$m.Matches[0].Groups[7].Value
        })
    }
    return $out
}

$script:FacWriteRegex = "SCENARIO FACWRITE who=(host|join) sid='([^']*)' target=(-?[\d.]+) ok=(\d) before=(-?[\d.]+) after=(-?[\d.]+)"

# faction_probe (protocol 24 phase 0): relation-baseline diagnostic. Gates only
# that the script ran (both FACWRITE lines + FACREL series on both sides);
# everything else is FINDINGs - sid cross-client stability, whether the
# sentinel setRelation stuck locally, whether anything crossed (expected: NO),
# and what the [fac] AFFECT detour saw.
function Test-FactionProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:FacWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:FacWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW) { $why += "host never logged its FACWRITE" }
    if ($null -eq $joinW) { $why += "join never logged its FACWRITE" }
    $hRel = Get-FacRelSeries -File $HostFile
    $jRel = Get-FacRelSeries -File $JoinFile
    if ($hRel.Keys.Count -eq 0) { $why += "host logged no FACREL series" }
    if ($jRel.Keys.Count -eq 0) { $why += "join logged no FACREL series" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  FACTION-PROBE $v - $detail"

    # FINDING: sid census overlap (the wire-identity question).
    $common = @($hRel.Keys | Where-Object { $jRel.ContainsKey($_) })
    Write-Host "    FINDING: FACREL sids host=$($hRel.Keys.Count) join=$($jRel.Keys.Count) common=$($common.Count)"

    # FINDING per write leg: did it stick locally, did it cross to the peer?
    foreach ($leg in @(@($hostW, $jRel, 'host'), @($joinW, $hRel, 'join'))) {
        $w = $leg[0]; $peer = $leg[1]; $side = $leg[2]
        if ($null -eq $w) { continue }
        $sid = $w.Matches[0].Groups[2].Value
        $tgt = [double]$w.Matches[0].Groups[3].Value
        $ok  = $w.Matches[0].Groups[4].Value
        $aft = [double]$w.Matches[0].Groups[6].Value
        $stuck = ($ok -eq '1') -and ([math]::Abs($aft - $tgt) -lt 0.5)
        $crossed = "sid-absent-on-peer"
        if ($peer.ContainsKey($sid)) {
            $end = $peer[$sid][$peer[$sid].Count - 1]
            $crossed = if (([math]::Abs($end.us - $tgt) -lt 0.5) -or
                           ([math]::Abs($end.them - $tgt) -lt 0.5)) { "CROSSED" } else { "did NOT cross (peer us=$($end.us) them=$($end.them))" }
        }
        Write-Host "    FINDING: $side sentinel sid='$sid' target=$tgt stuck=$stuck -> $crossed"
    }
    # FINDING: AFFECT detour evidence volume per side.
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        $ev  = @(Select-String -Path $pair[1] -Pattern '\[fac\] AFFECT-EV '  -ErrorAction SilentlyContinue).Count
        $amt = @(Select-String -Path $pair[1] -Pattern '\[fac\] AFFECT-AMT ' -ErrorAction SilentlyContinue).Count
        Write-Host "    FINDING: $($pair[0]) AFFECT-EV=$ev AFFECT-AMT=$amt"
    }
    return (Add-GateResult -Name "faction_probe" -Status $v `
                -Metrics @{ commonSids = $common.Count } -Detail $detail)
}

# faction_sync (protocol 24): the gated relation-convergence oracle. Each side
# wrote a sentinel relation (host -75 on the first sorted faction, join +65 on
# the second, both table rows); the gate is that each sentinel CONVERGED on
# the peer (final FACREL us AND them for that sid equal the target) and no
# co-visible row ended diverged.
function Test-FactionSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:FacWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:FacWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW) { $why += "host never logged its FACWRITE" }
    if ($null -eq $joinW) { $why += "join never logged its FACWRITE" }
    foreach ($pair in @(@('host', $hostW), @('join', $joinW))) {
        $side = $pair[0]; $w = $pair[1]
        if ($w -and $w.Matches[0].Groups[4].Value -ne '1') { $why += "$side FACWRITE failed locally" }
    }
    $hRel = Get-FacRelSeries -File $HostFile
    $jRel = Get-FacRelSeries -File $JoinFile

    $crossed = 0
    foreach ($leg in @(@($hostW, $jRel, 'host->join'), @($joinW, $hRel, 'join->host'))) {
        $w = $leg[0]; $peer = $leg[1]; $tag = $leg[2]
        if ($null -eq $w) { continue }
        $sid = $w.Matches[0].Groups[2].Value
        $tgt = [double]$w.Matches[0].Groups[3].Value
        if (-not $peer.ContainsKey($sid)) { $why += "$tag sid '$sid' absent from peer FACREL series"; continue }
        $end = $peer[$sid][$peer[$sid].Count - 1]
        if (([math]::Abs($end.us - $tgt) -lt 0.5) -and ([math]::Abs($end.them - $tgt) -lt 0.5)) { $crossed++ }
        else { $why += "$tag sentinel did not converge (peer sid '$sid' ended us=$($end.us) them=$($end.them), want $tgt)" }
    }

    # No drift on any co-visible row at the end of the run.
    $diverged = @()
    foreach ($sid in $hRel.Keys) {
        if (-not $jRel.ContainsKey($sid)) { continue }
        $h = $hRel[$sid][$hRel[$sid].Count - 1]
        $j = $jRel[$sid][$jRel[$sid].Count - 1]
        if (([math]::Abs($h.us - $j.us) -ge 0.5) -or ([math]::Abs($h.them - $j.them) -ge 0.5)) {
            $diverged += "$sid(h:$($h.us)/$($h.them) j:$($j.us)/$($j.them))"
        }
    }
    if ($diverged.Count -gt 0) { $why += ("final relations diverged: " + ($diverged -join ", ")) }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  FACTION-SYNC $v - crossed=$crossed/2 $detail"
    return (Add-GateResult -Name "faction_sync" -Status $v `
                -Metrics @{ crossed = $crossed; diverged = $diverged.Count } -Detail $detail)
}

# Shared GTIME parser: ordered series of @{hours; hourLen; fsm; paused; ok; t}.
function Get-GTimeSeries {
    param([string]$File)
    $out = New-Object System.Collections.ArrayList
    foreach ($m in @(Select-String -Path $File -Pattern "SCENARIO GTIME hours=(-?[\d.]+) hourLen=(-?[\d.]+) fsm=(-?[\d.]+) paused=(\d) ok=(\d) t=(\d+)" -ErrorAction SilentlyContinue)) {
        [void]$out.Add(@{
            hours   = [double]$m.Matches[0].Groups[1].Value
            hourLen = [double]$m.Matches[0].Groups[2].Value
            fsm     = [double]$m.Matches[0].Groups[3].Value
            paused  = [int]$m.Matches[0].Groups[4].Value
            ok      = [int]$m.Matches[0].Groups[5].Value
            t       = [long]$m.Matches[0].Groups[6].Value
        })
    }
    return ,$out
}

# Clock rate in game-hours per real second over a t-window of a GTIME series.
function Get-ClockRate {
    param($Series, [long]$FromMs, [long]$ToMs)
    $w = @($Series | Where-Object { $_.ok -eq 1 -and $_.t -ge $FromMs -and $_.t -le $ToMs })
    if ($w.Count -lt 2) { return $null }
    $dt = ($w[-1].t - $w[0].t) / 1000.0
    if ($dt -le 0) { return $null }
    return ($w[-1].hours - $w[0].hours) / $dt
}

# time_probe (protocol 25 phase 0): game-clock baseline diagnostic. Gates only
# that both sides logged a readable GTIME series and the clock is monotonic;
# everything else is FINDINGs - absolute-vs-relative clock, initial offset,
# drift rate, and whether the clock rate tracks the fsm burst (2x t=15..25s).
function Test-TimeProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $h = Get-GTimeSeries -File $HostFile
    $j = Get-GTimeSeries -File $JoinFile
    $hOk = @($h | Where-Object { $_.ok -eq 1 })
    $jOk = @($j | Where-Object { $_.ok -eq 1 })
    if ($hOk.Count -lt 5) { $why += "host GTIME series too thin (ok=$($hOk.Count))" }
    if ($jOk.Count -lt 5) { $why += "join GTIME series too thin (ok=$($jOk.Count))" }
    foreach ($pair in @(@('host', $hOk), @('join', $jOk))) {
        $s = $pair[1]
        for ($i = 1; $i -lt $s.Count; $i++) {
            if ($s[$i].hours -lt $s[$i-1].hours - 0.0001) {
                $why += "$($pair[0]) clock went BACKWARDS at t=$($s[$i].t)"; break
            }
        }
    }
    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  TIME-PROBE $v - $detail"
    if ($hOk.Count -gt 0 -and $jOk.Count -gt 0) {
        $h0 = $hOk[0]; $j0 = $jOk[0]; $hN = $hOk[-1]; $jN = $jOk[-1]
        Write-Host "    FINDING: host hours $($h0.hours)->$($hN.hours) hourLen=$($h0.hourLen); join $($j0.hours)->$($jN.hours) hourLen=$($j0.hourLen)"
        $off0 = [math]::Round($h0.hours - $j0.hours, 5)
        $offN = [math]::Round($hN.hours - $jN.hours, 5)
        Write-Host "    FINDING: host-join offset start=$off0 end=$offN (drift=$([math]::Round($offN-$off0,5)) game-hours over the run)"
        $pre   = Get-ClockRate -Series $hOk -FromMs 2000  -ToMs 14000
        $burst = Get-ClockRate -Series $hOk -FromMs 16000 -ToMs 24000
        if ($null -ne $pre -and $null -ne $burst -and $pre -gt 0) {
            Write-Host "    FINDING: host clock rate pre-burst=$([math]::Round($pre,6)) gh/s, during 2x burst=$([math]::Round($burst,6)) gh/s (ratio=$([math]::Round($burst/$pre,2)))"
        } else {
            Write-Host "    FINDING: clock rate not measurable (pre=$pre burst=$burst)"
        }
    }
    return (Add-GateResult -Name "time_probe" -Status $v `
                -Metrics @{ hostSamples = $hOk.Count; joinSamples = $jOk.Count } -Detail $detail)
}

# time_sync (protocol 25): the gated clock-convergence oracle. Gates that both
# clocks are readable and monotonic, that the final host-join offset is inside
# tolerance, and that convergence survived the mid-run consensus 2x burst
# (the last 5 s of wall-aligned samples all agree inside tolerance).
# Parse the join's "[time] OFFSET off=Xgh slew=Y ..." trace into a summary of
# the session-start clock catch-up (protocol 25): peak offset, how long the
# slew was engaged, and whether it converged back to 1x before the log ended.
# Returns $null when the log has no OFFSET lines (timeSync off / host log).
function Get-SlewSummary {
    param([string]$File)
    if (-not (Test-Path $File)) { return $null }
    $rx = "^\[(\d{2}:\d{2}:\d{2}\.\d{3})\].*\[time\] OFFSET off=(-?[\d.]+)gh slew=([\d.]+)"
    $samples = @()
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $samples += @{ t    = [TimeSpan]::Parse($g[1].Value)
                       off  = [math]::Abs([double]$g[2].Value)
                       slew = [double]$g[3].Value }
    }
    if ($samples.Count -eq 0) { return $null }
    $engaged = @($samples | Where-Object { $_.slew -lt 0.99 -or $_.slew -gt 1.01 })
    $peakOff = 0.0; $peakSlew = 0.0
    foreach ($s in $samples) {
        if ($s.off -gt $peakOff) { $peakOff = $s.off }
        if ($s.slew -gt $peakSlew) { $peakSlew = $s.slew }
    }
    $slewSecs = 0
    if ($engaged.Count -gt 0) {
        $slewSecs = [math]::Round(($engaged[-1].t - $engaged[0].t).TotalSeconds)
    }
    $last = $samples[-1]
    return @{ peakOffGh = [math]::Round($peakOff, 4); peakSlew = $peakSlew
              slewSecs = $slewSecs; lastSlew = $last.slew
              lastOffGh = [math]::Round($last.off, 4)
              converged = ($last.slew -ge 0.99 -and $last.slew -le 1.01) }
}

function Test-TimeSync {
    param([string]$HostFile, [string]$JoinFile)
    $TOL = 0.02 # game hours (~1.2 game-minutes; ~1.2 real-seconds at default hourLen)
    $why = @()
    $h = @((Get-GTimeSeries -File $HostFile)  | Where-Object { $_.ok -eq 1 })
    $j = @((Get-GTimeSeries -File $JoinFile)  | Where-Object { $_.ok -eq 1 })
    if ($h.Count -lt 5) { $why += "host GTIME series too thin (ok=$($h.Count))" }
    if ($j.Count -lt 5) { $why += "join GTIME series too thin (ok=$($j.Count))" }
    foreach ($pair in @(@('host', $h), @('join', $j))) {
        $s = $pair[1]
        for ($i = 1; $i -lt $s.Count; $i++) {
            if ($s[$i].hours -lt $s[$i-1].hours - 0.0001) {
                $why += "$($pair[0]) clock went BACKWARDS at t=$($s[$i].t)"; break
            }
        }
    }
    $finalOff = $null
    if ($h.Count -gt 0 -and $j.Count -gt 0) {
        # Same-scenario elapsed timers start within the run_test launch skew;
        # pair samples by nearest t (the series are both 1 Hz).
        $tail = @($j | Where-Object { $_.t -ge ($j[-1].t - 5000) })
        $worst = 0.0
        foreach ($js in $tail) {
            $near = $h | Sort-Object { [math]::Abs($_.t - $js.t) } | Select-Object -First 1
            if ($null -eq $near -or [math]::Abs($near.t - $js.t) -gt 1500) { continue }
            $d = [math]::Abs($near.hours - $js.hours)
            if ($d -gt $worst) { $worst = $d }
        }
        $finalOff = [math]::Round($worst, 5)
        if ($worst -gt $TOL) { $why += "final clock offset $finalOff game-hours exceeds tolerance $TOL" }
    }
    # Return-to-normal-speed gate (2026-07-10): catching up is only half the
    # contract - the slew must DISENGAGE once converged, leaving BOTH clients
    # at 1x. Join side: the last [time] OFFSET line must read slew~1.0.
    # Both sides: the final GTIME fsm (effective sim-speed multiplier) must be
    # back at 1x (the scenario's 2x burst window ends well before the tail).
    $slew = Get-SlewSummary -File $JoinFile
    $slewNote = "no slew engaged"
    if ($null -ne $slew) {
        $slewNote = "peakOff=$($slew.peakOffGh)gh peakSlew=$($slew.peakSlew) slewed=$($slew.slewSecs)s lastSlew=$($slew.lastSlew)"
        if (-not $slew.converged) {
            $why += "join slew never returned to 1x (last slew=$($slew.lastSlew), off=$($slew.lastOffGh)gh)"
        }
    }
    foreach ($pair in @(@('host', $h), @('join', $j))) {
        $s = $pair[1]
        if ($s.Count -eq 0) { continue }
        $lastFsm = $s[-1].fsm
        if ([math]::Abs($lastFsm - 1.0) -gt 0.05) {
            $why += "$($pair[0]) final sim speed fsm=$lastFsm (expected 1x after convergence)"
        }
    }
    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  TIME-SYNC $v - finalOffset=$finalOff (tol=$TOL) $detail"
    Write-Host "    FINDING: catch-up $slewNote"
    return (Add-GateResult -Name "time_sync" -Status $v `
                -Metrics @{ finalOffset = $finalOff } -Detail $detail)
}

# Parse the 1 Hz SCENARIO DOOR census into hand -> ordered samples
# @{open; locked; hasLock; state; gate; name; t}.
function Get-DoorSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO DOOR hand=([\d.]+) open=(-?\d+) locked=(-?\d+) hasLock=(-?\d+) state=(-?\d+) gate=(-?\d+) name='([^']*)' pos=\(([^)]*)\) t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[1].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ open = [int]$g[2].Value; locked = [int]$g[3].Value
                          hasLock = [int]$g[4].Value; state = [int]$g[5].Value
                          gate = [int]$g[6].Value; name = $g[7].Value; t = [long]$g[9].Value }
    }
    return $out
}

$script:DoorWriteRegex = 'SCENARIO DOORWRITE who=(host|join) hand=([\d.]+) want=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+)'

# door_probe (protocol 26 phase 0): baked-door baseline diagnostic. Gates only
# that both sides logged a door census and attempted the sentinel toggle;
# everything else is FINDINGs - hand census intersection (the wire-identity
# question), whether the sentinel write stuck and which side it crossed to
# (expected: none, doorSync forced off), and organic state changes.
function Test-DoorProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:DoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:DoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW) { $why += "host never logged its DOORWRITE" }
    if ($null -eq $joinW) { $why += "join never logged its DOORWRITE" }
    $hDoor = Get-DoorSeries -File $HostFile
    $jDoor = Get-DoorSeries -File $JoinFile
    if ($hDoor.Keys.Count -eq 0) { $why += "host census saw no doors (move the scenario to a save with doors in range)" }
    if ($jDoor.Keys.Count -eq 0) { $why += "join census saw no doors" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  DOOR-PROBE $v - $detail"

    # FINDING: hand census overlap (the wire-identity question).
    $common = @($hDoor.Keys | Where-Object { $jDoor.ContainsKey($_) })
    Write-Host "    FINDING: DOOR hands host=$($hDoor.Keys.Count) join=$($jDoor.Keys.Count) common=$($common.Count)"

    # FINDING per write leg: did it stick locally, did it cross to the peer?
    foreach ($leg in @(@($hostW, $jDoor, 'host'), @($joinW, $hDoor, 'join'))) {
        $w = $leg[0]; $peer = $leg[1]; $side = $leg[2]
        if ($null -eq $w) { continue }
        $hand = $w.Matches[0].Groups[2].Value
        $want = [int]$w.Matches[0].Groups[3].Value
        $ok   = $w.Matches[0].Groups[4].Value
        $aft  = [int]$w.Matches[0].Groups[6].Value
        $stuck = ($ok -eq '1') -and ($aft -eq $want)
        $crossed = "hand-absent-on-peer"
        if ($peer.ContainsKey($hand)) {
            $end = $peer[$hand][$peer[$hand].Count - 1]
            $crossed = if ($end.open -eq $want) { "CROSSED (peer open=$($end.open))" } else { "did NOT cross (peer open=$($end.open))" }
        }
        Write-Host "    FINDING: $side sentinel hand=$hand want=$want stuck=$stuck -> $crossed"
    }
    # FINDING: organic open-state flips per side (NPCs using doors).
    foreach ($pair in @(@('host', $hDoor), @('join', $jDoor))) {
        $flips = 0
        foreach ($hand in $pair[1].Keys) {
            $s = $pair[1][$hand]
            for ($i = 1; $i -lt $s.Count; $i++) { if ($s[$i].open -ne $s[$i-1].open) { $flips++ } }
        }
        Write-Host "    FINDING: $($pair[0]) open-state flips across the series=$flips"
    }
    return (Add-GateResult -Name "door_probe" -Status $v `
                -Metrics @{ commonHands = $common.Count } -Detail $detail)
}

# door_sync (protocol 26): the gated door-convergence oracle. Each side toggled
# a sentinel door; the gate is that each sentinel CROSSED - the peer's census
# OBSERVED the writer's target open state at some sample AFTER the write (the
# saves in use can have a single co-visible door, so both legs may hit the SAME
# door and organic NPC use may flip it later; final-state-equals-want would
# misjudge that) - and that no co-visible door ENDED diverged (host and join
# final open states agree per common hand).
function Test-DoorSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:DoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:DoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW) { $why += "host never logged its DOORWRITE" }
    if ($null -eq $joinW) { $why += "join never logged its DOORWRITE" }
    $hDoor = Get-DoorSeries -File $HostFile
    $jDoor = Get-DoorSeries -File $JoinFile
    if ($hDoor.Keys.Count -eq 0) { $why += "host census saw no doors" }
    if ($jDoor.Keys.Count -eq 0) { $why += "join census saw no doors" }

    # Scenario-elapsed t is comparable across the logs (both sides arm within
    # the launch skew); the write line carries its own t.
    $writeTRx = 't=(\d+)$'
    $crossed = 0
    foreach ($leg in @(@($hostW, $jDoor, 'host'), @($joinW, $hDoor, 'join'))) {
        $w = $leg[0]; $peer = $leg[1]; $side = $leg[2]
        if ($null -eq $w) { continue }
        $hand = $w.Matches[0].Groups[2].Value
        $want = [int]$w.Matches[0].Groups[3].Value
        if ($w.Matches[0].Groups[4].Value -ne '1') { $why += "$side sentinel write failed locally"; continue }
        if (-not $peer.ContainsKey($hand)) { $why += "$side sentinel hand=$hand absent from peer census"; continue }
        $wt = 0
        if ($w.Line -match $writeTRx) { $wt = [long]$Matches[1] }
        $seen = @($peer[$hand] | Where-Object { $_.t -ge ($wt - 500) -and $_.open -eq $want })
        if ($seen.Count -gt 0) { $crossed++ }
        else { $why += "$side sentinel did not cross (hand=$hand want=$want never observed on peer after t=$wt)" }
    }

    # No co-visible door may END diverged (final open state per common hand).
    $diverged = 0
    foreach ($hand in @($hDoor.Keys | Where-Object { $jDoor.ContainsKey($_) })) {
        $he = $hDoor[$hand][$hDoor[$hand].Count - 1]
        $je = $jDoor[$hand][$jDoor[$hand].Count - 1]
        if ($he.open -ne $je.open) {
            $diverged++
            $why += "co-visible door hand=$hand ended diverged (host open=$($he.open) join open=$($je.open))"
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  DOOR-SYNC $v - crossed=$crossed diverged=$diverged $detail"
    return (Add-GateResult -Name "door_sync" -Status $v `
                -Metrics @{ crossed = $crossed; diverged = $diverged } -Detail $detail)
}

# Parse the 1 Hz SCENARIO BUILDSITE census into hand -> ordered samples
# @{sid; prog; complete; name; t}. Only construction sites appear (complete
# buildings are filtered out by the enumerator).
function Get-BuildSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO BUILDSITE hand=([\d.]+) sid='([^']*)' prog=([-\d.]+) complete=(-?\d+) name='([^']*)' pos=\(([^)]*)\) t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[1].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ sid = $g[2].Value; prog = [double]$g[3].Value
                          complete = [int]$g[4].Value; name = $g[5].Value
                          t = [long]$g[7].Value }
    }
    return $out
}

$script:BuildPlaceRegex = "SCENARIO BUILDPLACE who=(host|join) rc=(-?\d+) ok=(\d) sid='([^']*)' hand=([\d.]+) pos=\(([^)]*)\) yaw=([-\d.]+) t=(\d+)"
$script:BuildProgRegex  = "SCENARIO BUILDPROG who=(host|join) step=(\d+) write=([-\d.]+) ok=(\d) prog=([-\d.]+) complete=(-?\d+) t=(\d+)"

# build_probe (protocol 27 phase 0): placed-building baseline diagnostic
# (buildSync forced OFF). Gates that both sides ATTEMPTED the programmatic
# placement and that at least one placement was ACCEPTED and the minted site
# appeared in its OWN census (the factory + census levers work somewhere - if
# both refuse, the save is unbuildable and the scenario must move). Everything
# else is FINDINGs feeding the protocol-27 design:
#   * factory-vs-town-rules: rc/ok per side (does createBuilding bypass the
#     UI's placementVerification where the scenario runs?);
#   * runtime hands: census intersection across clients (expected: the OWN
#     site is absent from the peer - host-only hands, nothing crosses);
#   * progress lever: the BUILDPROG ramp (did prog track the writes, did
#     complete flip at >= 1.0 - the self-complete/scale findings).
function Test-BuildProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinP = Select-String -Path $JoinFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its BUILDPLACE" }
    if ($null -eq $joinP) { $why += "join never logged its BUILDPLACE" }
    $hSites = Get-BuildSeries -File $HostFile
    $jSites = Get-BuildSeries -File $JoinFile

    $placedOk = 0
    foreach ($leg in @(@($hostP, $hSites, $jSites, 'host'), @($joinP, $jSites, $hSites, 'join'))) {
        $p = $leg[0]; $own = $leg[1]; $peer = $leg[2]; $side = $leg[3]
        if ($null -eq $p) { continue }
        $g = $p.Matches[0].Groups
        $rc = $g[2].Value; $ok = $g[3].Value; $sid = $g[4].Value; $hand = $g[5].Value
        Write-Host "    FINDING: $side placement rc=$rc ok=$ok sid='$sid' hand=$hand (factory-vs-town-rules answer)"
        if ($ok -ne '1') { continue }
        $placedOk++
        # The minted site must survive into the placer's OWN census.
        if ($own.ContainsKey($hand)) {
            $s = $own[$hand]
            Write-Host "    FINDING: $side own site enumerable, census samples=$($s.Count) firstProg=$($s[0].prog) lastProg=$($s[$s.Count-1].prog) lastComplete=$($s[$s.Count-1].complete)"
        } else {
            # Completion REMOVES a site from the census (enumSitesNear filters
            # complete buildings) - only flag when the ramp never confirmed it.
            $why += "$side minted site hand=$hand never appeared in its own census"
        }
        # Cross-visibility (expected: absent - runtime hand, no channel).
        $vis = if ($peer.ContainsKey($hand)) { "VISIBLE on peer (unexpected - hand resolved?)" } else { "absent from peer census (expected: runtime hand, nothing crosses)" }
        Write-Host "    FINDING: $side site hand=$hand $vis"
    }
    if (($null -ne $hostP) -and ($null -ne $joinP) -and $placedOk -eq 0) {
        $why += "BOTH placements refused - the factory does not bypass placement rules here; move the scenario to a buildable save"
    }

    # FINDING: the progress ramp per side (lever + scale + self-complete).
    foreach ($pair in @(@($HostFile, 'host'), @($JoinFile, 'join'))) {
        $steps = @(Select-String -Path $pair[0] -Pattern $script:BuildProgRegex -ErrorAction SilentlyContinue)
        if ($steps.Count -eq 0) { Write-Host "    FINDING: $($pair[1]) ramp never ran (placement refused or write lever dead)"; continue }
        $last = $steps[$steps.Count - 1].Matches[0].Groups
        Write-Host "    FINDING: $($pair[1]) ramp steps=$($steps.Count) lastWrite=$($last[3].Value) lastProg=$($last[5].Value) complete=$($last[6].Value)"
    }
    # FINDING: census hand overlap across clients (the wire-identity question).
    $common = @($hSites.Keys | Where-Object { $jSites.ContainsKey($_) })
    Write-Host "    FINDING: BUILD hands host=$($hSites.Keys.Count) join=$($jSites.Keys.Count) common=$($common.Count)"

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  BUILD-PROBE $v - placedOk=$placedOk $detail"
    return (Add-GateResult -Name "build_probe" -Status $v `
                -Metrics @{ placedOk = $placedOk; commonHands = $common.Count } -Detail $detail)
}

# build_sync (protocol 27): the gated placed-building convergence oracle. Each
# side placed one building programmatically (the same script as build_probe);
# the gate is the full describe/mint + progress leg in BOTH directions:
#   1. both BUILDPLACE ok=1 (the local placements landed);
#   2. each placement was MINTED on the peer (a "[build] MINT key=<placer
#      hand> ... rc=1" with the placer's key);
#   3. the placer's progress ramp CROSSED: the peer applied at least one
#      STATE row for that key (ok=1) and observed the complete=1 latch.
function Test-BuildSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinP = Select-String -Path $JoinFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its BUILDPLACE" }
    if ($null -eq $joinP) { $why += "join never logged its BUILDPLACE" }

    $minted = 0; $crossed = 0
    foreach ($leg in @(@($hostP, $JoinFile, 'host'), @($joinP, $HostFile, 'join'))) {
        $p = $leg[0]; $peerFile = $leg[1]; $side = $leg[2]
        if ($null -eq $p) { continue }
        $g = $p.Matches[0].Groups
        if ($g[3].Value -ne '1') { $why += "$side placement failed locally (ok=0)"; continue }
        $key = $g[5].Value
        # 2. the peer minted the placer's key
        $mintRx = "\[build\] MINT key=$([regex]::Escape($key)) sid='[^']*' ui=\d rc=(-?\d+) local=([\d.]+)"
        $mint = Select-String -Path $peerFile -Pattern $mintRx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $mint) { $why += "$side placement key=$key never MINTED on peer"; continue }
        if ($mint.Matches[0].Groups[1].Value -ne '1') {
            $why += "$side placement key=$key mint REFUSED on peer (rc=$($mint.Matches[0].Groups[1].Value))"; continue
        }
        $minted++
        # 3. progress rows applied on the peer, up to the complete latch
        $stateRx = "\[build\] STATE-RECV key=$([regex]::Escape($key)) prog=([-\d.]+) complete=(\d) ok=(\d)"
        $rows = @(Select-String -Path $peerFile -Pattern $stateRx -ErrorAction SilentlyContinue)
        $applied = @($rows | Where-Object { $_.Matches[0].Groups[3].Value -eq '1' })
        $done    = @($applied | Where-Object { $_.Matches[0].Groups[2].Value -eq '1' })
        if ($applied.Count -eq 0) { $why += "$side progress never crossed (no applied STATE-RECV for key=$key on peer)"; continue }
        if ($done.Count -eq 0)    { $why += "$side site never completed on peer (no applied complete=1 row for key=$key)"; continue }
        $crossed++
        Write-Host "    FINDING: $side key=$key minted on peer (local=$($mint.Matches[0].Groups[2].Value)), applied rows=$($applied.Count), complete latched"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  BUILD-SYNC $v - minted=$minted crossed=$crossed $detail"
    return (Add-GateResult -Name "build_sync" -Status $v `
                -Metrics @{ minted = $minted; crossed = $crossed } -Detail $detail)
}

# Parse the 1 Hz SCENARIO BDOOR census into doorHand -> ordered samples
# @{bhand; idx; open; locked; state; name; t} (bhand = owning building hand).
function Get-BdoorSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO BDOOR bhand=([\d.]+) idx=(-?\d+) hand=([\d.]+) open=(-?\d+) locked=(-?\d+) state=(-?\d+) name='([^']*)' t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[3].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ bhand = $g[1].Value; idx = [int]$g[2].Value
                          open = [int]$g[4].Value; locked = [int]$g[5].Value
                          state = [int]$g[6].Value; name = $g[7].Value
                          t = [long]$g[8].Value }
    }
    return $out
}

$script:BdoorWriteRegex   = 'SCENARIO BDOORWRITE who=(host|join) bhand=([\d.]+) idx=0 want=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+) t=(\d+)'
$script:BdestroyRegex     = 'SCENARIO BDESTROY who=(host|join) hand=([\d.]+) ok=(\d) stillResolves=(\d) t=(\d+)'

# Peer-side translation: the local hand the peer minted for a placer key
# ("[build] MINT key=... rc=1 local=..."). Returns $null when never minted.
function Get-MintLocalHand {
    param([string]$PeerFile, [string]$PlacerKey)
    $rx = "\[build\] MINT key=$([regex]::Escape($PlacerKey)) sid='[^']*' ui=\d rc=1 local=([\d.]+)"
    $m = Select-String -Path $PeerFile -Pattern $rx -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $m) { return $null }
    return $m.Matches[0].Groups[1].Value
}

# bdoor_probe (protocol 28 phase 0): placed-door + removal baseline diagnostic
# (bdoorSync forced OFF, the protocol-27 mint channel ON). Gates the LOCAL
# legs on both sides: shack placed ok, its door #0 toggled ok (proves minted
# buildings have DoorStuff children + the polite lever works on runtime
# doors), and the host's programmatic destroy worked. Everything else is
# FINDINGs feeding the protocol-28 design:
#   * does the peer's MINTED proxy have doors (census rows whose bhand is the
#     mint's local hand - the translation identity the channel rides on)?
#   * did the toggle CROSS (expected: no - the gap being proven)?
#   * did the host's destroy remove the JOIN's proxy (expected: no - the
#     ghost finding proving the removal gap)?
function Test-BdoorProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinP = Select-String -Path $JoinFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP -or $hostP.Matches[0].Groups[3].Value -ne '1') { $why += "host shack placement missing/failed" }
    if ($null -eq $joinP -or $joinP.Matches[0].Groups[3].Value -ne '1') { $why += "join shack placement missing/failed" }
    $hostW = Select-String -Path $HostFile -Pattern $script:BdoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:BdoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW -or $hostW.Matches[0].Groups[4].Value -ne '1') { $why += "host door toggle missing/failed (no door on the placed shack?)" }
    if ($null -eq $joinW -or $joinW.Matches[0].Groups[4].Value -ne '1') { $why += "join door toggle missing/failed" }
    $hostD = Select-String -Path $HostFile -Pattern $script:BdestroyRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostD -or $hostD.Matches[0].Groups[3].Value -ne '1') { $why += "host destroy missing/failed" }

    $hDoors = Get-BdoorSeries -File $HostFile
    $jDoors = Get-BdoorSeries -File $JoinFile

    # FINDING per side: does the peer's minted PROXY have doors?
    foreach ($leg in @(@($hostP, $JoinFile, $jDoors, 'host'), @($joinP, $HostFile, $hDoors, 'join'))) {
        $p = $leg[0]; $peerFile = $leg[1]; $peerDoors = $leg[2]; $side = $leg[3]
        if ($null -eq $p) { continue }
        $key = $p.Matches[0].Groups[5].Value
        $local = Get-MintLocalHand -PeerFile $peerFile -PlacerKey $key
        if ($null -eq $local) { Write-Host "    FINDING: $side shack key=$key never minted on peer"; continue }
        $proxyDoors = @($peerDoors.Keys | Where-Object { $peerDoors[$_][0].bhand -eq $local })
        Write-Host "    FINDING: $side shack proxy (peer local=$local) has $($proxyDoors.Count) door(s) in the peer census"
        # FINDING: did the toggle cross onto the proxy door (expected: no)?
        $w = if ($side -eq 'host') { $hostW } else { $joinW }
        if ($null -ne $w -and $proxyDoors.Count -gt 0) {
            $want = [int]$w.Matches[0].Groups[3].Value
            $wt   = [long]$w.Matches[0].Groups[7].Value
            $seen = 0
            foreach ($dh in $proxyDoors) {
                $seen += @($peerDoors[$dh] | Where-Object { $_.t -ge ($wt - 500) -and $_.open -eq $want }).Count
            }
            $crossTxt = if ($seen -gt 0) { "CROSSED (unexpected with bdoorSync off)" } else { "did NOT cross (expected: the gap)" }
            Write-Host "    FINDING: $side door toggle want=$want -> $crossTxt"
        }
    }
    # FINDING: the ghost - does the host's destroyed shack proxy survive on the join?
    if ($null -ne $hostP -and $null -ne $hostD) {
        $key = $hostP.Matches[0].Groups[5].Value
        $local = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $key
        $dt = [long]$hostD.Matches[0].Groups[5].Value
        if ($null -ne $local) {
            $late = 0
            foreach ($dh in @($jDoors.Keys | Where-Object { $jDoors[$_][0].bhand -eq $local })) {
                $late += @($jDoors[$dh] | Where-Object { $_.t -ge ($dt + 2000) }).Count
            }
            $ghostTxt = if ($late -gt 0) { "GHOST persists on join ($late door samples after destroy - the removal gap)" } else { "proxy gone from join census after destroy" }
            Write-Host "    FINDING: host destroy at t=$dt -> $ghostTxt"
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  BDOOR-PROBE $v - $detail"
    return (Add-GateResult -Name "bdoor_probe" -Status $v -Metrics @{} -Detail $detail)
}

# bdoor_sync (protocol 28): the gated placed-door + removal convergence
# oracle. Same script as bdoor_probe (shack + ramp + toggle + host destroy),
# bdoorSync ON. Gates, per direction:
#   1. the local legs (BUILDPLACE ok=1, BDOORWRITE ok=1; host BDESTROY ok=1);
#   2. each side's door toggle CROSSED: the peer logged an applied
#      "[bdoor] RECV key=<placer hand> idx=0 ... ok=1" AND its census shows
#      the proxy door at the toggled state after the write;
#   3. the host's destroy REMOVED the join's proxy: the join logged
#      "[build] REMOVE-RECV key=<host hand> ok=1" and its census has NO proxy
#      door samples afterwards (the probe's ghost, exorcised).
function Test-BdoorSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinP = Select-String -Path $JoinFile -Pattern $script:BuildPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP -or $hostP.Matches[0].Groups[3].Value -ne '1') { $why += "host shack placement missing/failed" }
    if ($null -eq $joinP -or $joinP.Matches[0].Groups[3].Value -ne '1') { $why += "join shack placement missing/failed" }
    $hostW = Select-String -Path $HostFile -Pattern $script:BdoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:BdoorWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW -or $hostW.Matches[0].Groups[4].Value -ne '1') { $why += "host door toggle missing/failed" }
    if ($null -eq $joinW -or $joinW.Matches[0].Groups[4].Value -ne '1') { $why += "join door toggle missing/failed" }
    $hostD = Select-String -Path $HostFile -Pattern $script:BdestroyRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostD -or $hostD.Matches[0].Groups[3].Value -ne '1') { $why += "host destroy missing/failed" }

    $hDoors = Get-BdoorSeries -File $HostFile
    $jDoors = Get-BdoorSeries -File $JoinFile

    $crossed = 0
    foreach ($leg in @(@($hostP, $hostW, $JoinFile, $jDoors, 'host'), @($joinP, $joinW, $HostFile, $hDoors, 'join'))) {
        $p = $leg[0]; $w = $leg[1]; $peerFile = $leg[2]; $peerDoors = $leg[3]; $side = $leg[4]
        if ($null -eq $p -or $null -eq $w) { continue }
        $key = $p.Matches[0].Groups[5].Value
        $want = [int]$w.Matches[0].Groups[3].Value
        $wt   = [long]$w.Matches[0].Groups[7].Value
        # 2a. the peer applied a bdoor row for the placer key
        $recvRx = "\[bdoor\] RECV key=$([regex]::Escape($key)) idx=0 open=$want locked=\d was=-?\d/-?\d ok=1"
        $recv = Select-String -Path $peerFile -Pattern $recvRx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $recv) { $why += "$side door toggle never applied on peer (no [bdoor] RECV ok=1 for key=$key)"; continue }
        # 2b. the peer's census shows the proxy door at the toggled state
        $local = Get-MintLocalHand -PeerFile $peerFile -PlacerKey $key
        if ($null -eq $local) { $why += "$side shack key=$key never minted on peer"; continue }
        $seen = 0
        foreach ($dh in @($peerDoors.Keys | Where-Object { $peerDoors[$_][0].bhand -eq $local })) {
            $seen += @($peerDoors[$dh] | Where-Object { $_.t -ge ($wt - 500) -and $_.open -eq $want }).Count
        }
        if ($seen -eq 0) { $why += "$side toggle applied but peer census never showed proxy door open=$want"; continue }
        $crossed++
        Write-Host "    FINDING: $side door toggle want=$want CROSSED (peer applied + $seen census samples)"
    }

    # 3. the removal leg (host destroys; join's proxy must go)
    $removed = 0
    if ($null -ne $hostP -and $null -ne $hostD) {
        $key = $hostP.Matches[0].Groups[5].Value
        $recvRx = "\[build\] REMOVE-RECV key=$([regex]::Escape($key)) ok=1"
        $recv = Select-String -Path $JoinFile -Pattern $recvRx -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $recv) {
            $why += "host destroy never applied on join (no [build] REMOVE-RECV ok=1 for key=$key)"
        } else {
            $local = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $key
            $dt = [long]$hostD.Matches[0].Groups[5].Value
            $late = 0
            if ($null -ne $local) {
                foreach ($dh in @($jDoors.Keys | Where-Object { $jDoors[$_][0].bhand -eq $local })) {
                    $late += @($jDoors[$dh] | Where-Object { $_.t -ge ($dt + 4000) }).Count
                }
            }
            if ($late -gt 0) {
                $why += "join proxy GHOST survived the removal ($late door samples after destroy)"
            } else {
                $removed = 1
                Write-Host "    FINDING: host destroy CROSSED - join proxy gone (0 census samples after t=$dt+4s)"
            }
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  BDOOR-SYNC $v - crossed=$crossed removed=$removed $detail"
    return (Add-GateResult -Name "bdoor_sync" -Status $v `
                -Metrics @{ crossed = $crossed; removed = $removed } -Detail $detail)
}

# Parse the 1 Hz SCENARIO HUNGER census into hand -> ordered samples
# @{hunger; fed; dazed; t} (protocol 29).
function Get-HungerSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO HUNGER hand=([\d.]+) hunger=([-\d.]+) fed=([-\d.]+) dazed=([-\d.]+) t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[1].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ hunger = [double]$g[2].Value; fed = [double]$g[3].Value
                          dazed = [double]$g[4].Value; t = [long]$g[5].Value }
    }
    return $out
}

$script:HungerWriteRegex = 'SCENARIO HUNGERWRITE who=(host|join) hand=([\d.]+) before=([-\d.]+) write=([-\d.]+) after=([-\d.]+) ok=(\d) t=(\d+)'

# hunger_probe (protocol 29 phase 0): hunger baseline diagnostic (hungerSync
# forced OFF; the rest of the medical snapshot streams as usual). Gates the
# LOCAL legs on both sides: the 1 Hz census ran and each side's sentinel
# hunger write (own-tab leader, current * 0.6) stuck. Everything else is
# FINDINGs feeding the protocol-29 fold-in:
#   * hunger scale + per-client decay for the SAME hand (rate agreement);
#   * did the sentinel CROSS (expected: no - the gap being proven)?
#   * owner-vs-copy divergence magnitude at end of run;
#   * dazedOrAlert value range (drunk/drug evidence for the deferred half).
function Test-HungerProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:HungerWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:HungerWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW -or $hostW.Matches[0].Groups[6].Value -ne '1') { $why += "host sentinel hunger write missing/failed" }
    if ($null -eq $joinW -or $joinW.Matches[0].Groups[6].Value -ne '1') { $why += "join sentinel hunger write missing/failed" }
    $hSeries = Get-HungerSeries -File $HostFile
    $jSeries = Get-HungerSeries -File $JoinFile
    if ($hSeries.Keys.Count -eq 0) { $why += "host hunger census empty" }
    if ($jSeries.Keys.Count -eq 0) { $why += "join hunger census empty" }

    # FINDING: scale + per-client decay agreement for every shared hand.
    foreach ($hand in @($hSeries.Keys | Where-Object { $jSeries.ContainsKey($_) })) {
        $hs = $hSeries[$hand]; $js = $jSeries[$hand]
        if ($hs.Count -lt 2 -or $js.Count -lt 2) { continue }
        $hRate = ($hs[$hs.Count-1].hunger - $hs[0].hunger) / [math]::Max(1, ($hs[$hs.Count-1].t - $hs[0].t) / 1000.0)
        $jRate = ($js[$js.Count-1].hunger - $js[0].hunger) / [math]::Max(1, ($js[$js.Count-1].t - $js[0].t) / 1000.0)
        Write-Host ("    FINDING: hand=$hand host first={0:N2} last={1:N2} rate={2:N4}/s | join first={3:N2} last={4:N2} rate={5:N4}/s endGap={6:N2}" -f `
            $hs[0].hunger, $hs[$hs.Count-1].hunger, $hRate, $js[0].hunger, $js[$js.Count-1].hunger, $jRate, `
            [math]::Abs($hs[$hs.Count-1].hunger - $js[$js.Count-1].hunger))
    }
    # FINDING: sentinel crossing per side (expected: no with hungerSync off).
    # Crossing = the peer's copy shows a DROP of at least half the sentinel
    # step within 10 s of the write (drop-relative: the run-171751 lesson -
    # an absolute tolerance wider than the engine's ~0..3 hunger scale calls
    # everything a cross).
    foreach ($leg in @(@($hostW, $jSeries, 'host'), @($joinW, $hSeries, 'join'))) {
        $w = $leg[0]; $peer = $leg[1]; $side = $leg[2]
        if ($null -eq $w) { continue }
        $g = $w.Matches[0].Groups
        $hand = $g[2].Value
        $before = [double]$g[3].Value; $want = [double]$g[4].Value; $wt = [long]$g[7].Value
        $drop = $before - $want
        if (-not $peer.ContainsKey($hand)) { Write-Host "    FINDING: $side sentinel hand=$hand absent from peer census"; continue }
        $pre = @($peer[$hand] | Where-Object { $_.t -lt $wt }) | Select-Object -Last 1
        $preH = if ($null -ne $pre) { $pre.hunger } else { $before }
        $hit = @($peer[$hand] | Where-Object { $_.t -ge $wt -and $_.t -le ($wt + 10000) -and ($preH - $_.hunger) -ge ($drop * 0.5) })
        $crossTxt = if ($hit.Count -gt 0) { "CROSSED (unexpected with hungerSync off)" } else { "did NOT cross (expected: the gap)" }
        Write-Host ("    FINDING: $side sentinel write={0:N2} (drop {1:N2}) -> $crossTxt" -f $want, $drop)
    }
    # FINDING: dazedOrAlert range (drunk/drug-evidence for the deferred half).
    foreach ($pair in @(@($hSeries, 'host'), @($jSeries, 'join'))) {
        $all = @(); foreach ($k in $pair[0].Keys) { $all += @($pair[0][$k] | ForEach-Object { $_.dazed }) }
        if ($all.Count -gt 0) {
            $mn = ($all | Measure-Object -Minimum).Minimum; $mx = ($all | Measure-Object -Maximum).Maximum
            Write-Host ("    FINDING: $($pair[1]) dazedOrAlert range [{0:N3} .. {1:N3}] over $($all.Count) samples" -f $mn, $mx)
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  HUNGER-PROBE $v - $detail"
    return (Add-GateResult -Name "hunger_probe" -Status $v -Metrics @{} -Detail $detail)
}

# hunger_sync (protocol 29): the gated hunger convergence oracle. Same script
# as hunger_probe (1 Hz census + proportional sentinel per side), hungerSync
# ON. Gates, per direction:
#   1. the local leg (HUNGERWRITE ok=1);
#   2. the sentinel CROSSED: the peer's copy of that hand shows at least half
#      the sentinel's drop within 10 s of the write (drop-relative - the
#      engine's scale is ~0..3, absolute tolerances are useless);
#   3. end-of-run owner-vs-copy agreement: for every hand in both censuses
#      the final hunger gap is small (<= 0.35, ~10% of scale - covers the
#      change-gate quantization + one decay interval).
function Test-HungerSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostW = Select-String -Path $HostFile -Pattern $script:HungerWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinW = Select-String -Path $JoinFile -Pattern $script:HungerWriteRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostW -or $hostW.Matches[0].Groups[6].Value -ne '1') { $why += "host sentinel hunger write missing/failed" }
    if ($null -eq $joinW -or $joinW.Matches[0].Groups[6].Value -ne '1') { $why += "join sentinel hunger write missing/failed" }
    $hSeries = Get-HungerSeries -File $HostFile
    $jSeries = Get-HungerSeries -File $JoinFile

    $crossed = 0
    foreach ($leg in @(@($hostW, $jSeries, 'host'), @($joinW, $hSeries, 'join'))) {
        $w = $leg[0]; $peer = $leg[1]; $side = $leg[2]
        if ($null -eq $w) { continue }
        $g = $w.Matches[0].Groups
        $hand = $g[2].Value
        $before = [double]$g[3].Value; $want = [double]$g[4].Value; $wt = [long]$g[7].Value
        $drop = $before - $want
        if (-not $peer.ContainsKey($hand)) { $why += "$side sentinel hand=$hand absent from peer census"; continue }
        $pre = @($peer[$hand] | Where-Object { $_.t -lt $wt }) | Select-Object -Last 1
        $preH = if ($null -ne $pre) { $pre.hunger } else { $before }
        $hit = @($peer[$hand] | Where-Object { $_.t -ge $wt -and $_.t -le ($wt + 10000) -and ($preH - $_.hunger) -ge ($drop * 0.5) })
        if ($hit.Count -eq 0) { $why += "$side sentinel (drop $([math]::Round($drop,2))) never crossed onto the peer copy"; continue }
        $crossed++
        Write-Host ("    FINDING: $side sentinel write={0:N2} (drop {1:N2}) CROSSED ({2} peer samples within 10 s)" -f $want, $drop, $hit.Count)
    }

    # 3. end-of-run agreement for every shared hand.
    $maxGap = 0.0
    foreach ($hand in @($hSeries.Keys | Where-Object { $jSeries.ContainsKey($_) })) {
        $hs = $hSeries[$hand]; $js = $jSeries[$hand]
        $gap = [math]::Abs($hs[$hs.Count-1].hunger - $js[$js.Count-1].hunger)
        if ($gap -gt $maxGap) { $maxGap = $gap }
        if ($gap -gt 0.35) { $why += "hand=$hand final hunger diverged (gap $([math]::Round($gap,2)) > 0.35)" }
    }
    Write-Host ("    FINDING: final owner-vs-copy max hunger gap {0:N3}" -f $maxGap)

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  HUNGER-SYNC $v - crossed=$crossed maxGap=$([math]::Round($maxGap,3)) $detail"
    return (Add-GateResult -Name "hunger_sync" -Status $v `
                -Metrics @{ crossed = $crossed; maxGap = $maxGap } -Detail $detail)
}

# Parse the 1 Hz SCENARIO PROD machine census into hand -> ordered samples
# @{class; power; pwrOut; state; outSid; outAmt; in0Amt; in1Amt; tech; grown;
# sid; name; t} (protocol 33).
function Get-ProdSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO PROD hand=([\d.]+) class=(-?\d+) power=(-?\d+) pwrOut=([-\d.]+) state=(-?\d+) mine=([-\d.]+) out='([^']*)' outAmt=([-\d.]+) outCap=(-?\d+) in0='([^']*)' in0Amt=([-\d.]+) in1='([^']*)' in1Amt=([-\d.]+) tech=(-?\d+) grown=([-\d.]+) died=([-\d.]+) harv=(-?\d+) sid='([^']*)' name='([^']*)' t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[1].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ class = [int]$g[2].Value; power = [int]$g[3].Value
                          pwrOut = [double]$g[4].Value; state = [int]$g[5].Value
                          mine = [double]$g[6].Value; outSid = $g[7].Value
                          outAmt = [double]$g[8].Value; outCap = [int]$g[9].Value
                          in0Amt = [double]$g[11].Value; in1Amt = [double]$g[13].Value
                          tech = [int]$g[14].Value; grown = [double]$g[15].Value
                          sid = $g[18].Value; name = $g[19].Value
                          t = [long]$g[20].Value }
    }
    return $out
}

$script:ProdPlaceRegex    = "SCENARIO PRODPLACE who=(host|join) genRc=(-?\d+) genOk=(\d) genSid='([^']*)' genHand=([\d.]+) benchRc=(-?\d+) benchOk=(\d) benchSid='([^']*)' benchHand=([\d.]+) t=(\d+)"
$script:ProdPowerRegex    = 'SCENARIO PRODWRITE who=(host|join) kind=power want=(-?\d+) ok=(\d) before=(-?\d+) after=(-?\d+) pwrOut=([-\d.]+) t=(\d+)'
$script:ProdOutWriteRegex = "SCENARIO PRODWRITE who=(host|join) kind=(setitem|direct) want=([-\d.]+) ok=(\d) before=([-\d.]+) after=([-\d.]+) out='([^']*)' t=(\d+)"
$script:ProdOpRegex       = 'SCENARIO PRODOP who=(host|join) kind=bench n=(\d+) ok=(\d) state=(-?\d+) outAmt=([-\d.]+) in0Amt=([-\d.]+) t=(\d+)'

# prod_probe (protocol 33 phase 0): production machine baseline diagnostic
# (prodSync forced OFF, the protocol-27 mint channel ON). Gates the LOCAL
# legs: host placed + completed both machines (generator + bench), the power
# toggle applied locally, the native setProductionItem write landed, the
# operate loop ran, and BOTH censuses produced rows. Everything else is
# FINDINGs feeding the protocol-33 design:
#   * census hand intersection across clients (the BAKED-machine wire
#     identity question - placed machines translate via the build maps);
#   * divergence baseline: the host's bench output/input series moved under
#     operate() while the join's minted copy stayed flat (the gap);
#   * write levers: did the direct amount write survive the next census tick
#     (update() clamp question), did switchPowerOn persist;
#   * did the power toggle CROSS (expected: no with prodSync off)?
#   * research evidence: tech levels + any RESEARCH op rows (progress-store
#     location input for the follow-up spike).
function Test-ProdProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:ProdPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its PRODPLACE" }
    elseif ($hostP.Matches[0].Groups[3].Value -ne '1' -or $hostP.Matches[0].Groups[7].Value -ne '1') {
        $why += "host machine placement failed (genOk=$($hostP.Matches[0].Groups[3].Value) benchOk=$($hostP.Matches[0].Groups[7].Value))"
    }
    $hSeries = Get-ProdSeries -File $HostFile
    $jSeries = Get-ProdSeries -File $JoinFile
    if ($hSeries.Keys.Count -eq 0) { $why += "host machine census empty" }
    if ($jSeries.Keys.Count -eq 0) { $why += "join machine census empty" }

    # Local power write leg (host generator).
    $pw = @(Select-String -Path $HostFile -Pattern $script:ProdPowerRegex -ErrorAction SilentlyContinue)
    $pwOk = @($pw | Where-Object { $_.Matches[0].Groups[3].Value -eq '1' -and $_.Matches[0].Groups[2].Value -eq $_.Matches[0].Groups[5].Value })
    if ($pw.Count -eq 0) { $why += "host power write never ran" }
    elseif ($pwOk.Count -eq 0) { $why += "host power writes never applied (after != want)" }
    foreach ($m in $pw) {
        $g = $m.Matches[0].Groups
        Write-Host "    FINDING: host power write want=$($g[2].Value) ok=$($g[3].Value) before=$($g[4].Value) after=$($g[5].Value) pwrOut=$($g[6].Value)"
    }
    # Local output write legs (native setitem must land; direct is a finding).
    $ow = @(Select-String -Path $HostFile -Pattern $script:ProdOutWriteRegex -ErrorAction SilentlyContinue)
    $setItem = @($ow | Where-Object { $_.Matches[0].Groups[2].Value -eq 'setitem' }) | Select-Object -Last 1
    if ($null -eq $setItem -or $setItem.Matches[0].Groups[4].Value -ne '1') { $why += "host native setProductionItem write missing/failed" }
    foreach ($m in $ow) {
        $g = $m.Matches[0].Groups
        Write-Host "    FINDING: host output write kind=$($g[2].Value) want=$($g[3].Value) ok=$($g[4].Value) before=$($g[5].Value) after=$($g[6].Value) out='$($g[7].Value)'"
    }
    # Operate loop ran (the divergence driver).
    $ops = @(Select-String -Path $HostFile -Pattern $script:ProdOpRegex -ErrorAction SilentlyContinue)
    if ($ops.Count -eq 0) { $why += "host operate loop never ran" }
    else {
        $last = $ops[$ops.Count - 1].Matches[0].Groups
        Write-Host "    FINDING: host bench ops=$($last[2].Value) lastState=$($last[4].Value) lastOutAmt=$($last[5].Value) lastIn0Amt=$($last[6].Value)"
    }

    # FINDING: census hand overlap (the BAKED wire-identity question).
    $common = @($hSeries.Keys | Where-Object { $jSeries.ContainsKey($_) })
    Write-Host "    FINDING: PROD hands host=$($hSeries.Keys.Count) join=$($jSeries.Keys.Count) common=$($common.Count)"

    # FINDING: owner-vs-idle divergence for the bench (host hand + the join's
    # minted copy translated through the protocol-27 MINT line).
    if ($null -ne $hostP) {
        $benchKey = $hostP.Matches[0].Groups[9].Value
        if ($hSeries.ContainsKey($benchKey)) {
            $s = $hSeries[$benchKey]
            Write-Host ("    FINDING: host bench outAmt first={0:N3} last={1:N3} in0 first={2:N3} last={3:N3} samples=$($s.Count)" -f `
                $s[0].outAmt, $s[$s.Count-1].outAmt, $s[0].in0Amt, $s[$s.Count-1].in0Amt)
        }
        $local = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $benchKey
        if ($null -ne $local -and $jSeries.ContainsKey($local)) {
            $s = $jSeries[$local]
            Write-Host ("    FINDING: join bench COPY (local=$local) outAmt first={0:N3} last={1:N3} samples=$($s.Count) (flat = the gap)" -f `
                $s[0].outAmt, $s[$s.Count-1].outAmt)
        } else {
            Write-Host "    FINDING: join never minted / never censused the host bench (key=$benchKey local=$local)"
        }
        # FINDING: did the power toggle cross (expected: no with prodSync off)?
        $genKey = $hostP.Matches[0].Groups[5].Value
        $genLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $genKey
        $offW = @($pw | Where-Object { $_.Matches[0].Groups[2].Value -eq '0' }) | Select-Object -Last 1
        if ($null -ne $genLocal -and $jSeries.ContainsKey($genLocal) -and $null -ne $offW) {
            $wt = [long]$offW.Matches[0].Groups[7].Value
            $hit = @($jSeries[$genLocal] | Where-Object { $_.t -ge $wt -and $_.power -eq 0 })
            $crossTxt = if ($hit.Count -gt 0) { "CROSSED (unexpected with prodSync off)" } else { "did NOT cross (expected: the gap)" }
            Write-Host "    FINDING: host power OFF -> join copy $crossTxt"
        }
    }
    # FINDING: research evidence (tech levels + driven ops).
    foreach ($pair in @(@($hSeries, 'host'), @($jSeries, 'join'))) {
        $rb = @($pair[0].Keys | Where-Object { $pair[0][$_][0].class -eq 5 })
        foreach ($k in $rb) {
            $s = $pair[0][$k]
            Write-Host "    FINDING: $($pair[1]) research bench hand=$k tech first=$($s[0].tech) last=$($s[$s.Count-1].tech) name='$($s[0].name)'"
        }
    }
    $res = @(Select-String -Path $HostFile -Pattern 'SCENARIO RESEARCH who=host n=(\d+) ok=(\d) tech=(-?\d+) power=(-?\d+) t=(\d+)' -ErrorAction SilentlyContinue)
    if ($res.Count -gt 0) {
        $last = $res[$res.Count - 1].Matches[0].Groups
        Write-Host "    FINDING: host research ops=$($last[1].Value) lastOk=$($last[2].Value) lastTech=$($last[3].Value)"
    } else {
        Write-Host "    FINDING: no research bench in reach (research leg skipped)"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  PROD-PROBE $v - commonHands=$($common.Count) $detail"
    return (Add-GateResult -Name "prod_probe" -Status $v `
                -Metrics @{ commonHands = $common.Count } -Detail $detail)
}

# prod_sync (protocol 33 full tier): host-authoritative machine state sync.
# Same scenario script as prod_probe but with prodSync ON - so the probe's
# measured gaps must now be CLOSED. Gates:
#   * the probe's local legs (host placed + completed both machines, power
#     toggle applied, native output write landed, operate loop ran);
#   * the wire ran: host sent [prod] rows, the join received + applied some;
#   * the join MINTED both host machines (protocol-27 translation identity);
#   * output convergence: the join bench copy's final outAmt within tolerance
#     of the host's final (the operate-driven divergence the probe measured
#     must now cross);
#   * power crossing: the host's OFF toggle shows up on the join generator
#     copy within 6 s, and the final power state agrees after the ON.
function Test-ProdSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:ProdPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its PRODPLACE" }
    elseif ($hostP.Matches[0].Groups[3].Value -ne '1' -or $hostP.Matches[0].Groups[7].Value -ne '1') {
        $why += "host machine placement failed (genOk=$($hostP.Matches[0].Groups[3].Value) benchOk=$($hostP.Matches[0].Groups[7].Value))"
    }
    $hSeries = Get-ProdSeries -File $HostFile
    $jSeries = Get-ProdSeries -File $JoinFile
    if ($hSeries.Keys.Count -eq 0) { $why += "host machine census empty" }
    if ($jSeries.Keys.Count -eq 0) { $why += "join machine census empty" }

    # Local legs (the probe's gates - the divergence driver must have run).
    $pw = @(Select-String -Path $HostFile -Pattern $script:ProdPowerRegex -ErrorAction SilentlyContinue)
    $pwOk = @($pw | Where-Object { $_.Matches[0].Groups[3].Value -eq '1' -and $_.Matches[0].Groups[2].Value -eq $_.Matches[0].Groups[5].Value })
    if ($pwOk.Count -eq 0) { $why += "host power writes missing/never applied" }
    $ow = @(Select-String -Path $HostFile -Pattern $script:ProdOutWriteRegex -ErrorAction SilentlyContinue)
    $setItem = @($ow | Where-Object { $_.Matches[0].Groups[2].Value -eq 'setitem' }) | Select-Object -Last 1
    if ($null -eq $setItem -or $setItem.Matches[0].Groups[4].Value -ne '1') { $why += "host native setProductionItem write missing/failed" }
    $ops = @(Select-String -Path $HostFile -Pattern $script:ProdOpRegex -ErrorAction SilentlyContinue)
    if ($ops.Count -eq 0) { $why += "host operate loop never ran" }

    # The wire ran on both ends.
    $sent = @(Select-String -Path $HostFile -Pattern '\[prod\] SEND key=' -ErrorAction SilentlyContinue)
    $recv = @(Select-String -Path $JoinFile -Pattern '\[prod\] RECV key=' -ErrorAction SilentlyContinue)
    if ($sent.Count -eq 0) { $why += "host never sent a [prod] row" }
    if ($recv.Count -eq 0) { $why += "join never received/applied a [prod] row" }
    Write-Host "    FINDING: [prod] rows host sent=$($sent.Count) join applied=$($recv.Count)"

    $outGap = -1.0
    if ($null -ne $hostP) {
        # Output convergence on the bench (the operate-driven divergence).
        $benchKey = $hostP.Matches[0].Groups[9].Value
        $benchLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $benchKey
        if ($null -eq $benchLocal) { $why += "join never minted the host bench (key=$benchKey)" }
        elseif (-not $jSeries.ContainsKey($benchLocal)) { $why += "join bench copy (local=$benchLocal) absent from its census" }
        elseif (-not $hSeries.ContainsKey($benchKey)) { $why += "host bench absent from its own census" }
        else {
            $hs = $hSeries[$benchKey]; $js = $jSeries[$benchLocal]
            $hLast = $hs[$hs.Count-1].outAmt; $jLast = $js[$js.Count-1].outAmt
            $outGap = [math]::Abs($hLast - $jLast)
            Write-Host ("    FINDING: bench outAmt host first={0:N3} last={1:N3} join copy first={2:N3} last={3:N3} gap={4:N3}" -f `
                $hs[0].outAmt, $hLast, $js[0].outAmt, $jLast, $outGap)
            # Tolerance: 1 Hz change-gated rows against a 1 Hz census - the
            # copy may lag one operate step; anything wider is divergence.
            if ($outGap -gt 1.0) { $why += "bench output diverged (gap $([math]::Round($outGap,3)) > 1.0)" }
        }
        # Power crossing on the generator (OFF must show up, ON must settle).
        $genKey = $hostP.Matches[0].Groups[5].Value
        $genLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $genKey
        if ($null -eq $genLocal) { $why += "join never minted the host generator (key=$genKey)" }
        elseif (-not $jSeries.ContainsKey($genLocal)) { $why += "join generator copy (local=$genLocal) absent from its census" }
        else {
            $gs = $jSeries[$genLocal]
            $offW = @($pw | Where-Object { $_.Matches[0].Groups[2].Value -eq '0' }) | Select-Object -Last 1
            if ($null -ne $offW) {
                $wt = [long]$offW.Matches[0].Groups[7].Value
                $hit = @($gs | Where-Object { $_.t -ge $wt -and $_.t -le ($wt + 6000) -and $_.power -eq 0 })
                if ($hit.Count -eq 0) { $why += "host power OFF never crossed onto the join generator copy" }
                else { Write-Host "    FINDING: host power OFF CROSSED ($($hit.Count) join samples within 6 s)" }
            }
            $jFinalPwr = $gs[$gs.Count-1].power
            $hFinalPwr = if ($hSeries.ContainsKey($genKey)) { $hSeries[$genKey][$hSeries[$genKey].Count-1].power } else { -1 }
            Write-Host "    FINDING: final generator power host=$hFinalPwr join=$jFinalPwr"
            if ($hFinalPwr -ge 0 -and $jFinalPwr -ne $hFinalPwr) { $why += "final generator power disagrees (host=$hFinalPwr join=$jFinalPwr)" }
        }
    }
    # FINDING: farm state (no farm leg in the scenario - terrain-dependent;
    # any baked farm in reach reports its growth floats for the record).
    foreach ($pair in @(@($hSeries, 'host'), @($jSeries, 'join'))) {
        $fb = @($pair[0].Keys | Where-Object { $pair[0][$_][0].class -eq 4 })
        foreach ($k in $fb) {
            $s = $pair[0][$k]
            Write-Host "    FINDING: $($pair[1]) farm hand=$k grown first=$($s[0].grown) last=$($s[$s.Count-1].grown) name='$($s[0].name)'"
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  PROD-SYNC $v - outGap=$([math]::Round($outGap,3)) sent=$($sent.Count) applied=$($recv.Count) $detail"
    return (Add-GateResult -Name "prod_sync" -Status $v `
                -Metrics @{ outGap = $outGap; sent = $sent.Count; applied = $recv.Count } -Detail $detail)
}

# Parse the 1 Hz SCENARIO RESEARCH subject poll into ordered samples
# @{known; can; t} (protocol 38).
function Get-ResearchSeries {
    param([string]$File)
    $rows = @()
    $m = @(Select-String -Path $File -Pattern "SCENARIO RESEARCH who=\w+ sid='([^']*)' known=(-?\d+) can=(-?\d+) t=(\d+)" -ErrorAction SilentlyContinue)
    foreach ($r in $m) {
        $g = $r.Matches[0].Groups
        $rows += @{ sid = $g[1].Value; known = [int]$g[2].Value; can = [int]$g[3].Value; t = [long]$g[4].Value }
    }
    return ,$rows
}

# research_probe (protocol 38 phase 0): tech-tree baseline diagnostic
# (researchSync forced OFF). Gates:
#   * both clients picked a subject and the sids MATCH (the wire-key
#     stability leg - shared RESEARCH enumeration order);
#   * host startResearch rc=1 and its isKnown flipped to 1 (the write lever);
#   * DIVERGENCE: every join sample between the host's start and the join's
#     own self-start reads known=0 (the unlock must NOT cross with the hatch
#     off - the gap protocol 38 exists to close);
#   * join self-start rc=1 and its isKnown flipped AND stuck to run end
#     (the exact lever applyResearch drives).
function Test-ResearchProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $pickRe = "SCENARIO RESEARCHPICK who=(\w+) rc=(-?\d+) sid='([^']*)' t=(\d+)"
    $hPick = Select-String -Path $HostFile -Pattern $pickRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    $jPick = Select-String -Path $JoinFile -Pattern $pickRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hPick -or $hPick.Matches[0].Groups[2].Value -ne '1') { $why += "host never picked a subject" }
    if ($null -eq $jPick -or $jPick.Matches[0].Groups[2].Value -ne '1') { $why += "join never picked a subject" }
    if ($null -ne $hPick -and $null -ne $jPick) {
        $hSid = $hPick.Matches[0].Groups[3].Value
        $jSid = $jPick.Matches[0].Groups[3].Value
        Write-Host "    FINDING: subject host='$hSid' join='$jSid'"
        if ($hSid -ne $jSid) { $why += "subject sids differ (wire key unstable)" }
    }
    $startRe = "SCENARIO RESEARCHSTART who=(\w+) rc=(-?\d+) sid='([^']*)' known=(-?\d+) t=(\d+)"
    $hStart = Select-String -Path $HostFile -Pattern $startRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    $jStart = Select-String -Path $JoinFile -Pattern $startRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hStart -or $hStart.Matches[0].Groups[2].Value -ne '1') { $why += "host startResearch missing/failed" }
    if ($null -eq $jStart -or $jStart.Matches[0].Groups[2].Value -ne '1') { $why += "join self startResearch missing/failed" }
    $jSeries = Get-ResearchSeries -File $JoinFile
    if ($jSeries.Count -eq 0) { $why += "join subject poll empty" }
    # Divergence window: host starts ~10 s, join self-starts ~25 s; every join
    # sample in between must be known=0 (hatch OFF = the unlock cannot cross).
    if ($null -ne $hStart -and $null -ne $jStart) {
        $ht = [long]$hStart.Matches[0].Groups[5].Value
        $jt = [long]$jStart.Matches[0].Groups[5].Value
        $win = @($jSeries | Where-Object { $_.t -gt $ht -and $_.t -lt $jt })
        $leaked = @($win | Where-Object { $_.known -eq 1 })
        Write-Host "    FINDING: join divergence window samples=$($win.Count) leaked=$($leaked.Count) (host start t=$ht, join self-start t=$jt)"
        if ($win.Count -eq 0) { $why += "no join samples in the divergence window" }
        elseif ($leaked.Count -gt 0) { $why += "unlock CROSSED with the hatch OFF ($($leaked.Count) samples)" }
        # Stickiness: every join sample >= 2 s after its self-start is known=1.
        $stick = @($jSeries | Where-Object { $_.t -ge ($jt + 2000) })
        $bad = @($stick | Where-Object { $_.known -ne 1 })
        Write-Host "    FINDING: join post-start samples=$($stick.Count) notKnown=$($bad.Count)"
        if ($stick.Count -eq 0) { $why += "no join samples after its self-start" }
        elseif ($bad.Count -gt 0) { $why += "join unlock did not STICK ($($bad.Count) samples reverted)" }
    }
    $resRe = "SCENARIO RESEARCHRESULT who=(\w+) pick=(-?\d+) start=(-?\d+) known=(-?\d+) pass=(\d)"
    foreach ($pair in @(@($HostFile, 'host'), @($JoinFile, 'join'))) {
        $r = Select-String -Path $pair[0] -Pattern $resRe -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $r) { $why += "$($pair[1]) never logged its RESEARCHRESULT" }
        else {
            $g = $r.Matches[0].Groups
            Write-Host "    FINDING: $($pair[1]) result pick=$($g[2].Value) start=$($g[3].Value) known=$($g[4].Value) pass=$($g[5].Value)"
            if ($g[4].Value -ne '1') { $why += "$($pair[1]) final isKnown != 1" }
        }
    }
    $sent = @(Select-String -Path $HostFile -Pattern '\[research\] SEND sid=' -ErrorAction SilentlyContinue)
    Write-Host "    FINDING: [research] rows sent with hatch OFF = $($sent.Count) (expected 0)"

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RESEARCH-PROBE $v - $detail"
    return (Add-GateResult -Name "research_probe" -Status $v -Metrics @{} -Detail $detail)
}

# research_sync (protocol 38 full tier): host-authoritative tech-tree sync.
# Same scenario script as research_probe minus the join's self-start - the
# WIRE must flip the join's isKnown. Gates:
#   * both clients picked the SAME subject;
#   * host startResearch rc=1 + final isKnown=1 (the driving edge);
#   * the wire ran: host sent [research] rows, the join received + applied;
#   * crossing: the join's subject poll reads known=1 within 6 s of the
#     host's start (1 Hz publish + reliable row + same-tick apply) and every
#     later sample stays 1 (stickiness);
#   * join final isKnown=1.
function Test-ResearchSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $pickRe = "SCENARIO RESEARCHPICK who=(\w+) rc=(-?\d+) sid='([^']*)' t=(\d+)"
    $hPick = Select-String -Path $HostFile -Pattern $pickRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    $jPick = Select-String -Path $JoinFile -Pattern $pickRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hPick -or $hPick.Matches[0].Groups[2].Value -ne '1') { $why += "host never picked a subject" }
    if ($null -eq $jPick -or $jPick.Matches[0].Groups[2].Value -ne '1') { $why += "join never picked a subject" }
    if ($null -ne $hPick -and $null -ne $jPick -and
        $hPick.Matches[0].Groups[3].Value -ne $jPick.Matches[0].Groups[3].Value) {
        $why += "subject sids differ (wire key unstable)"
    }
    $startRe = "SCENARIO RESEARCHSTART who=host rc=(-?\d+) sid='([^']*)' known=(-?\d+) t=(\d+)"
    $hStart = Select-String -Path $HostFile -Pattern $startRe -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hStart -or $hStart.Matches[0].Groups[1].Value -ne '1') { $why += "host startResearch missing/failed" }
    # The wire ran on both ends.
    $sent = @(Select-String -Path $HostFile -Pattern '\[research\] SEND sid=' -ErrorAction SilentlyContinue)
    $recv = @(Select-String -Path $JoinFile -Pattern '\[research\] RECV sid=' -ErrorAction SilentlyContinue)
    if ($sent.Count -eq 0) { $why += "host never sent a [research] row" }
    if ($recv.Count -eq 0) { $why += "join never received/applied a [research] row" }
    Write-Host "    FINDING: [research] rows host sent=$($sent.Count) join applied=$($recv.Count)"
    # Crossing + stickiness on the join's subject poll.
    $jSeries = Get-ResearchSeries -File $JoinFile
    $crossMs = -1
    if ($jSeries.Count -eq 0) { $why += "join subject poll empty" }
    elseif ($null -ne $hStart) {
        $ht = [long]$hStart.Matches[0].Groups[4].Value
        $hit = @($jSeries | Where-Object { $_.t -ge $ht -and $_.known -eq 1 }) | Select-Object -First 1
        if ($null -eq $hit) { $why += "host unlock never crossed onto the join" }
        else {
            $crossMs = $hit.t - $ht
            Write-Host "    FINDING: unlock CROSSED in ${crossMs} ms (host start t=$ht, join known=1 t=$($hit.t))"
            if ($crossMs -gt 6000) { $why += "crossing too slow ($crossMs ms > 6000)" }
            $after = @($jSeries | Where-Object { $_.t -gt $hit.t })
            $bad = @($after | Where-Object { $_.known -ne 1 })
            if ($bad.Count -gt 0) { $why += "join unlock did not STICK ($($bad.Count) samples reverted)" }
        }
    }
    $resRe = "SCENARIO RESEARCHRESULT who=(\w+) pick=(-?\d+) start=(-?\d+) known=(-?\d+) pass=(\d)"
    foreach ($pair in @(@($HostFile, 'host'), @($JoinFile, 'join'))) {
        $r = Select-String -Path $pair[0] -Pattern $resRe -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $r) { $why += "$($pair[1]) never logged its RESEARCHRESULT" }
        else {
            $g = $r.Matches[0].Groups
            Write-Host "    FINDING: $($pair[1]) result pick=$($g[2].Value) start=$($g[3].Value) known=$($g[4].Value) pass=$($g[5].Value)"
            if ($g[4].Value -ne '1') { $why += "$($pair[1]) final isKnown != 1" }
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RESEARCH-SYNC $v - crossMs=$crossMs sent=$($sent.Count) applied=$($recv.Count) $detail"
    return (Add-GateResult -Name "research_sync" -Status $v `
                -Metrics @{ crossMs = $crossMs; sent = $sent.Count; applied = $recv.Count } -Detail $detail)
}

# Parse the 1 Hz SCENARIO CONT container census into hand -> ordered samples
# @{class; complete; inv; n; qty; hash; firstSid; firstQty; sid; name; t}
# (protocol 34).
function Get-ContSeries {
    param([string]$File)
    $out = @{}
    $rx = "SCENARIO CONT hand=([\d.]+) class=(-?\d+) complete=(\d) inv=(\d) n=(-?\d+) qty=(-?\d+) hash=(\d+) first='([^']*)' firstQty=(-?\d+) sid='([^']*)' name='([^']*)' t=(\d+)"
    foreach ($m in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $hand = $g[1].Value
        if (-not $out.ContainsKey($hand)) { $out[$hand] = @() }
        $out[$hand] += @{ class = [int]$g[2].Value; complete = [int]$g[3].Value
                          inv = [int]$g[4].Value; n = [int]$g[5].Value
                          qty = [int]$g[6].Value; hash = [uint32]$g[7].Value
                          firstSid = $g[8].Value; firstQty = [int]$g[9].Value
                          sid = $g[10].Value; name = $g[11].Value
                          t = [long]$g[12].Value }
    }
    return $out
}

$script:ContPlaceRegex = "SCENARIO CONTPLACE who=(host|join) benchRc=(-?\d+) benchOk=(\d) benchSid='([^']*)' benchHand=([\d.]+) chestRc=(-?\d+) chestOk=(\d) chestSid='([^']*)' chestHand=([\d.]+) t=(\d+)"
$script:ContAddRegex   = "SCENARIO CONTWRITE who=(host|join) kind=add tgt=(\w+) sid='([^']*)' want=(-?\d+) got=(-?\d+) ok=(\d) beforeN=(-?\d+) beforeQty=(-?\d+) afterN=(-?\d+) afterQty=(-?\d+) hash=(\d+) t=(\d+)"
$script:ContReconRegex = "SCENARIO CONTWRITE who=host kind=recon tgt=chest sid='([^']*)' capN=(\d+) beforeQty=(-?\d+) found=(-?\d+) keep=(\d+) changed=(\d) afterN=(-?\d+) afterQty=(-?\d+) hash=(\d+) t=(\d+)"
$script:ContEmptyRegex = 'SCENARIO CONTWRITE who=host kind=empty tgt=bench changed=(\d) beforeN=(-?\d+) beforeQty=(-?\d+) afterN=(-?\d+) afterQty=(-?\d+) t=(\d+)'
$script:ContOpRegex    = "SCENARIO CONTOP who=(host|join) n=(\d+) ok=(\d) contN=(-?\d+) contQty=(-?\d+) hash=(\d+) first='([^']*)' t=(\d+)"

# store_probe (protocol 34 phase 0): storage/machine container baseline
# diagnostic (storeSync forced OFF, the protocol-27 mint channel ON). Gates
# the LOCAL legs: host placed + completed both buildings (bench + chest), the
# fabricate-into-chest add landed, the reconcile removal landed, the operate
# loop ran, and BOTH censuses produced rows. Everything else is FINDINGs
# feeding the protocol-34 design:
#   * census hand intersection across clients + per-row hasInv (do building
#     containers read?);
#   * capacity: max distinct entries per container vs INV_ITEMS_MAX=20;
#   * divergence baseline: the host bench CONTAINER quantity grew under
#     operate() while the join's minted copy stayed flat (the gap); did the
#     chest add cross (expected: no with storeSync off)?
#   * churn: after the host force-emptied the bench container, did its own
#     update() re-produce items (the reconcile fight risk)?
#   * join-side fabricate into its MINTED chest copy (the translated-key
#     apply half).
function Test-StoreProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:ContPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its CONTPLACE" }
    elseif ($hostP.Matches[0].Groups[3].Value -ne '1' -or $hostP.Matches[0].Groups[7].Value -ne '1') {
        $why += "host placement failed (benchOk=$($hostP.Matches[0].Groups[3].Value) chestOk=$($hostP.Matches[0].Groups[7].Value))"
    }
    $hSeries = Get-ContSeries -File $HostFile
    $jSeries = Get-ContSeries -File $JoinFile
    if ($hSeries.Keys.Count -eq 0) { $why += "host container census empty" }
    if ($jSeries.Keys.Count -eq 0) { $why += "join container census empty" }

    # Local add leg (fabricate INTO the placed chest).
    $adds = @(Select-String -Path $HostFile -Pattern $script:ContAddRegex -ErrorAction SilentlyContinue)
    $hostAdd = @($adds | Where-Object { $_.Matches[0].Groups[1].Value -eq 'host' }) | Select-Object -Last 1
    if ($null -eq $hostAdd) { $why += "host chest add never ran" }
    elseif ($hostAdd.Matches[0].Groups[6].Value -ne '1') { $why += "host chest add failed (fabricate-into-building lever)" }
    foreach ($m in $adds) {
        $g = $m.Matches[0].Groups
        Write-Host "    FINDING: host add tgt=$($g[2].Value) sid='$($g[3].Value)' want=$($g[4].Value) got=$($g[5].Value) ok=$($g[6].Value) qty $($g[8].Value)->$($g[10].Value)"
    }
    # Local reconcile-removal leg.
    $recon = Select-String -Path $HostFile -Pattern $script:ContReconRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $recon) { $why += "host chest reconcile never ran" }
    else {
        $g = $recon.Matches[0].Groups
        Write-Host "    FINDING: host recon capN=$($g[2].Value) beforeQty=$($g[3].Value) found=$($g[4].Value) keep=$($g[5].Value) changed=$($g[6].Value) afterQty=$($g[8].Value)"
        if ($g[6].Value -ne '1' -or [int]$g[8].Value -ge [int]$g[3].Value) {
            $why += "host reconcile removal did not stick (changed=$($g[6].Value) beforeQty=$($g[3].Value) afterQty=$($g[8].Value))"
        }
    }
    # Operate loop ran (the divergence driver).
    $ops = @(Select-String -Path $HostFile -Pattern $script:ContOpRegex -ErrorAction SilentlyContinue)
    if ($ops.Count -eq 0) { $why += "host operate loop never ran" }
    else {
        $last = $ops[$ops.Count - 1].Matches[0].Groups
        Write-Host "    FINDING: host bench ops=$($last[2].Value) lastContN=$($last[4].Value) lastContQty=$($last[5].Value) first='$($last[7].Value)'"
    }

    # FINDING: census hand overlap + inventory readability + capacity.
    $common = @($hSeries.Keys | Where-Object { $jSeries.ContainsKey($_) })
    Write-Host "    FINDING: CONT hands host=$($hSeries.Keys.Count) join=$($jSeries.Keys.Count) common=$($common.Count)"
    foreach ($pair in @(@($hSeries, 'host'), @($jSeries, 'join'))) {
        $withInv = @($pair[0].Keys | Where-Object { $pair[0][$_][0].inv -eq 1 })
        $maxN = 0
        foreach ($k in $pair[0].Keys) { foreach ($s in $pair[0][$k]) { if ($s.n -gt $maxN) { $maxN = $s.n } } }
        Write-Host "    FINDING: $($pair[1]) containers=$($pair[0].Keys.Count) withInv=$($withInv.Count) maxEntries=$maxN (INV_ITEMS_MAX=20)"
    }

    if ($null -ne $hostP) {
        # FINDING: owner-vs-idle divergence for the bench CONTAINER (host hand
        # + the join's minted copy translated through the protocol-27 MINT line).
        $benchKey = $hostP.Matches[0].Groups[5].Value
        if ($hSeries.ContainsKey($benchKey)) {
            $s = $hSeries[$benchKey]
            Write-Host "    FINDING: host bench container qty first=$($s[0].qty) last=$($s[$s.Count-1].qty) samples=$($s.Count)"
        }
        $benchLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $benchKey
        if ($null -ne $benchLocal -and $jSeries.ContainsKey($benchLocal)) {
            $s = $jSeries[$benchLocal]
            Write-Host "    FINDING: join bench COPY (local=$benchLocal) qty first=$($s[0].qty) last=$($s[$s.Count-1].qty) samples=$($s.Count) (flat = the gap)"
        } else {
            Write-Host "    FINDING: join never minted / never censused the host bench (key=$benchKey local=$benchLocal)"
        }
        # FINDING: did the chest add cross (expected: no with storeSync off)?
        # Window is bounded by the JOIN's OWN sentinel add (t=40s) - its local
        # fabricate would otherwise read as a false crossing (run-171728).
        $chestKey = $hostP.Matches[0].Groups[9].Value
        $chestLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $chestKey
        if ($null -ne $chestLocal -and $jSeries.ContainsKey($chestLocal) -and $null -ne $hostAdd) {
            $at = [long]$hostAdd.Matches[0].Groups[12].Value
            $jAdd0 = @(Select-String -Path $JoinFile -Pattern $script:ContAddRegex -ErrorAction SilentlyContinue | Where-Object { $_.Matches[0].Groups[1].Value -eq 'join' }) | Select-Object -First 1
            $jAddT = if ($null -ne $jAdd0) { [long]$jAdd0.Matches[0].Groups[12].Value } else { [long]::MaxValue }
            $hit = @($jSeries[$chestLocal] | Where-Object { $_.t -ge $at -and $_.t -lt $jAddT -and $_.qty -gt 0 })
            $crossTxt = if ($hit.Count -gt 0) { "CROSSED (unexpected with storeSync off)" } else { "did NOT cross (expected: the gap)" }
            Write-Host "    FINDING: host chest add -> join copy $crossTxt"
        }
    }
    # FINDING: churn after the force-empty (does update() re-produce?).
    $empty = Select-String -Path $HostFile -Pattern $script:ContEmptyRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $empty -and $null -ne $hostP) {
        $g = $empty.Matches[0].Groups
        Write-Host "    FINDING: host bench force-empty changed=$($g[1].Value) qty $($g[3].Value)->$($g[5].Value)"
        $benchKey = $hostP.Matches[0].Groups[5].Value
        if ($hSeries.ContainsKey($benchKey)) {
            $et = [long]$g[6].Value
            $post = @($hSeries[$benchKey] | Where-Object { $_.t -gt $et })
            if ($post.Count -gt 0) {
                $regrow = @($post | Where-Object { $_.qty -gt 0 })
                $churnTxt = if ($regrow.Count -gt 0) { "REGREW to $($post[$post.Count-1].qty) (churn risk - update() re-produces)" } else { "stayed empty ($($post.Count) samples - no churn)" }
                Write-Host "    FINDING: bench container post-empty $churnTxt"
            }
        }
    } else {
        Write-Host "    FINDING: host bench force-empty never ran"
    }
    # FINDING: join-side fabricate into its minted chest copy.
    $jAdd = @(Select-String -Path $JoinFile -Pattern $script:ContAddRegex -ErrorAction SilentlyContinue | Where-Object { $_.Matches[0].Groups[1].Value -eq 'join' }) | Select-Object -Last 1
    if ($null -ne $jAdd) {
        $g = $jAdd.Matches[0].Groups
        Write-Host "    FINDING: join minted-chest add ok=$($g[6].Value) got=$($g[5].Value) qty $($g[8].Value)->$($g[10].Value)"
    } else {
        Write-Host "    FINDING: join minted-chest add never ran (mint not latched?)"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  STORE-PROBE $v - commonHands=$($common.Count) $detail"
    return (Add-GateResult -Name "store_probe" -Status $v `
                -Metrics @{ commonHands = $common.Count } -Detail $detail)
}

# store_sync (protocol 34 full tier): host-authoritative storage/machine
# container contents. Same scenario script as store_probe (minus the join-side
# add) but with storeSync ON - the probe's measured gaps must now be CLOSED.
# Gates:
#   * the probe's local legs (host placed + completed both buildings, the
#     chest add landed, the reconcile removal landed, the operate loop ran);
#   * the wire ran: the host census-authored the PLACED chest (an [inv] SEND
#     with kind=1) and the join received + applied [inv] rows;
#   * content convergence: the join's minted chest copy filled after the host
#     add (qty > 0) and its FINAL content hash equals the host chest's final
#     hash (the add AND the reconcile-removal both crossed).
function Test-StoreSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $hostP = Select-String -Path $HostFile -Pattern $script:ContPlaceRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostP) { $why += "host never logged its CONTPLACE" }
    elseif ($hostP.Matches[0].Groups[3].Value -ne '1' -or $hostP.Matches[0].Groups[7].Value -ne '1') {
        $why += "host placement failed (benchOk=$($hostP.Matches[0].Groups[3].Value) chestOk=$($hostP.Matches[0].Groups[7].Value))"
    }
    $hSeries = Get-ContSeries -File $HostFile
    $jSeries = Get-ContSeries -File $JoinFile
    if ($hSeries.Keys.Count -eq 0) { $why += "host container census empty" }
    if ($jSeries.Keys.Count -eq 0) { $why += "join container census empty" }

    # Local legs (the probe's gates - the divergence driver must have run).
    $adds = @(Select-String -Path $HostFile -Pattern $script:ContAddRegex -ErrorAction SilentlyContinue)
    $hostAdd = @($adds | Where-Object { $_.Matches[0].Groups[1].Value -eq 'host' }) | Select-Object -Last 1
    if ($null -eq $hostAdd -or $hostAdd.Matches[0].Groups[6].Value -ne '1') { $why += "host chest add missing/failed" }
    $recon = Select-String -Path $HostFile -Pattern $script:ContReconRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $recon -or $recon.Matches[0].Groups[6].Value -ne '1') { $why += "host chest reconcile missing/failed" }
    $ops = @(Select-String -Path $HostFile -Pattern $script:ContOpRegex -ErrorAction SilentlyContinue)
    if ($ops.Count -eq 0) { $why += "host operate loop never ran" }

    # The wire ran on both ends (kind=1 = the census-authored PLACED building).
    $sent  = @(Select-String -Path $HostFile -Pattern '\[inv\] SEND .*kind=1' -ErrorAction SilentlyContinue)
    $recv  = @(Select-String -Path $JoinFile -Pattern '\[inv\] APPLY' -ErrorAction SilentlyContinue)
    if ($sent.Count -eq 0) { $why += "host never census-authored a placed container (no [inv] SEND kind=1)" }
    if ($recv.Count -eq 0) { $why += "join never applied an [inv] snapshot" }
    Write-Host "    FINDING: [inv] placed-key rows host sent=$($sent.Count) join applied(all)=$($recv.Count)"

    $hashMatch = $false
    if ($null -ne $hostP) {
        $chestKey = $hostP.Matches[0].Groups[9].Value
        $chestLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $chestKey
        if ($null -eq $chestLocal) { $why += "join never minted the host chest (key=$chestKey)" }
        elseif (-not $jSeries.ContainsKey($chestLocal)) { $why += "join chest copy (local=$chestLocal) absent from its census" }
        elseif (-not $hSeries.ContainsKey($chestKey)) { $why += "host chest absent from its own census" }
        else {
            $hs = $hSeries[$chestKey]; $js = $jSeries[$chestLocal]
            $hLast = $hs[$hs.Count-1]; $jLast = $js[$js.Count-1]
            Write-Host "    FINDING: chest qty host first=$($hs[0].qty) last=$($hLast.qty) join copy first=$($js[0].qty) last=$($jLast.qty)"
            Write-Host "    FINDING: chest final hash host=$($hLast.hash) join=$($jLast.hash)"
            # The add must have CROSSED (join copy non-empty at some point
            # after the host add)...
            if ($null -ne $hostAdd) {
                $at = [long]$hostAdd.Matches[0].Groups[12].Value
                $filled = @($js | Where-Object { $_.t -ge $at -and $_.qty -gt 0 })
                if ($filled.Count -eq 0) { $why += "host chest add never crossed onto the join copy" }
            }
            # ... and the FINAL states must agree (hash equality = identical
            # multiset, so the reconcile-removal crossed too).
            $hashMatch = ($hLast.hash -eq $jLast.hash)
            if (-not $hashMatch) { $why += "final chest contents disagree (host hash=$($hLast.hash) join=$($jLast.hash))" }
        }
        # FINDING: the bench machine-container series (the probe found operate()
        # does NOT materialize items into the Building inventory - record both
        # sides for the day the engine path changes).
        $benchKey = $hostP.Matches[0].Groups[5].Value
        if ($hSeries.ContainsKey($benchKey)) {
            $s = $hSeries[$benchKey]
            Write-Host "    FINDING: host bench container qty first=$($s[0].qty) last=$($s[$s.Count-1].qty)"
        }
        $benchLocal = Get-MintLocalHand -PeerFile $JoinFile -PlacerKey $benchKey
        if ($null -ne $benchLocal -and $jSeries.ContainsKey($benchLocal)) {
            $s = $jSeries[$benchLocal]
            Write-Host "    FINDING: join bench COPY qty first=$($s[0].qty) last=$($s[$s.Count-1].qty)"
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  STORE-SYNC $v - hashMatch=$hashMatch sent=$($sent.Count) applied=$($recv.Count) $detail"
    return (Add-GateResult -Name "store_sync" -Status $v `
                -Metrics @{ hashMatch = [int]$hashMatch; sent = $sent.Count; applied = $recv.Count } -Detail $detail)
}

# Parse the latejoin 1 Hz censuses (protocol 30). Returns @{ doors = hand ->
# ordered @{open;locked;t}; facs = sid -> ordered @{us;t}; money = rank ->
# ordered @{money;t} }.
function Get-LatejoinCensus {
    param([string]$File)
    $doors = @{}; $facs = @{}; $money = @{}
    foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO LJDOORROW hand=([\d.]+) open=(-?\d+) locked=(-?\d+) t=(\d+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups; $h = $g[1].Value
        if (-not $doors.ContainsKey($h)) { $doors[$h] = @() }
        $doors[$h] += @{ open = [int]$g[2].Value; locked = [int]$g[3].Value; t = [long]$g[4].Value }
    }
    foreach ($m in @(Select-String -Path $File -Pattern "SCENARIO LJFACROW sid='([^']*)' us=([-\d.]+) t=(\d+)" -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups; $s = $g[1].Value
        if (-not $facs.ContainsKey($s)) { $facs[$s] = @() }
        $facs[$s] += @{ us = [double]$g[2].Value; t = [long]$g[3].Value }
    }
    foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO LJMONEYROW rank=(\d+) money=(-?\d+) t=(\d+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups; $r = [int]$g[1].Value
        if (-not $money.ContainsKey($r)) { $money[$r] = @() }
        $money[$r] += @{ money = [int]$g[2].Value; t = [long]$g[3].Value }
    }
    return @{ doors = $doors; facs = $facs; money = $money }
}

# Parse the host's four pre-arm mutation lines. Returns $null when the door
# mutation (the first) is missing entirely.
function Get-LatejoinMutations {
    param([string]$HostFile)
    $out = @{ doorOk = $false; facOk = $false; moneyOk = $false; buildOk = $false; buildDone = $false }
    $m = Select-String -Path $HostFile -Pattern 'SCENARIO LJDOOR hand=([\d.]+) mode=(lock|open) before=(-?\d+) want=(-?\d+) ok=(\d) after=(-?\d+) t=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $m) { return $null }
    $g = $m.Matches[0].Groups
    $out.doorHand = $g[1].Value; $out.doorMode = $g[2].Value
    $out.doorWant = [int]$g[4].Value
    $out.doorOk = ($g[5].Value -eq '1'); $out.doorT = [long]$g[7].Value
    $m = Select-String -Path $HostFile -Pattern "SCENARIO LJFAC sid='([^']*)' target=([-\d.]+) ok=(\d) before=([-\d.]+) after=([-\d.]+) t=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $m) {
        $g = $m.Matches[0].Groups
        $out.facSid = $g[1].Value; $out.facTarget = [double]$g[2].Value
        $out.facOk = ($g[3].Value -eq '1')
    }
    $m = Select-String -Path $HostFile -Pattern 'SCENARIO LJMONEY before=(-?\d+) bump=(\d+) after=(-?\d+) ok=(\d) t=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $m) {
        $g = $m.Matches[0].Groups
        $out.moneyAfter = [int]$g[3].Value; $out.moneyOk = ($g[4].Value -eq '1')
    }
    $m = Select-String -Path $HostFile -Pattern "SCENARIO LJBUILD rc=(-?\d+) ok=(\d) sid='([^']*)' hand=([\d.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $m) {
        $g = $m.Matches[0].Groups
        $out.buildOk = ($g[2].Value -eq '1'); $out.buildSid = $g[3].Value
        $out.buildHand = $g[4].Value
    }
    $m = Select-String -Path $HostFile -Pattern 'SCENARIO LJBUILDPROG step=\d+ write=[\d.]+ ok=1 prog=[\d.]+ complete=1' -ErrorAction SilentlyContinue | Select-Object -Last 1
    $out.buildDone = ($null -ne $m)
    # The arm-time verdict supersedes the initial write lines (a re-asserted
    # door that HELD counts; a mutation that failed by connect time does not).
    $m = Select-String -Path $HostFile -Pattern 'SCENARIO LJMUT door=(\d) fac=(\d) money=(\d) build=(\d) done=(\d)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $m) {
        $g = $m.Matches[0].Groups
        $out.doorOk = ($g[1].Value -eq '1')
    }
    return $out
}

# Per-channel heal latency findings shared by both latejoin oracles: for each
# pre-connect host mutation, the join's arm-clock time of the FIRST census
# sample agreeing with the mutated value (the join arms at peer-ready, i.e.
# right after the connect, so small t = fast heal). Returns @{door;fac;money}
# with -1 = never agreed.
function Get-LatejoinHealLatency {
    param($Mut, $JoinCensus)
    $r = @{ door = -1; fac = -1; money = -1 }
    if ($Mut.doorOk -and $JoinCensus.doors.ContainsKey($Mut.doorHand)) {
        $field = if ($Mut.doorMode -eq 'lock') { 'locked' } else { 'open' }
        $hit = @($JoinCensus.doors[$Mut.doorHand] | Where-Object { $_[$field] -eq $Mut.doorWant }) | Select-Object -First 1
        if ($null -ne $hit) { $r.door = $hit.t }
    }
    if ($Mut.facOk -and $JoinCensus.facs.ContainsKey($Mut.facSid)) {
        $hit = @($JoinCensus.facs[$Mut.facSid] | Where-Object { [math]::Abs($_.us - $Mut.facTarget) -lt 0.5 }) | Select-Object -First 1
        if ($null -ne $hit) { $r.fac = $hit.t }
    }
    if ($Mut.moneyOk -and $JoinCensus.money.ContainsKey(0)) {
        $hit = @($JoinCensus.money[0] | Where-Object { $_.money -eq $Mut.moneyAfter }) | Select-Object -First 1
        if ($null -ne $hit) { $r.money = $hit.t }
    }
    return $r
}

# latejoin_probe (protocol 30 phase 0): the unsynced late-join baseline
# (latejoinSync forced OFF, everything else streaming). The host mutated
# door/faction/money/build BEFORE the join connected. Gates the LOCAL legs:
# all four host pre-arm mutations ok (door toggle, faction sentinel, wallet
# bump, building placed + ramped complete) and both censuses ran. Everything
# else is FINDINGs motivating the connect-edge resync:
#   * per-channel heal latency on the join (door/faction/money are expected
#     to heal via their 10 s / 5 s safety resends - pre-connect sends armed
#     the resend even though nobody heard them);
#   * the building mint (expected: the join NEVER minted - PKT_BUILD_PLACE
#     is a one-shot edge, the permanent loss class);
#   * connect-edge timing (host's "peer present" line seen).
function Test-LatejoinProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $mut = Get-LatejoinMutations -HostFile $HostFile
    if ($null -eq $mut) {
        Write-Host "  LATEJOIN-PROBE FAIL - host never logged its pre-arm mutations"
        return (Add-GateResult -Name "latejoin_probe" -Status FAIL -Detail "host pre-arm mutations missing")
    }
    # The door leg is FINDINGS-ONLY in both tiers: the sync save's one baked
    # door belongs to town AI, which fights any sentinel state (reopens and
    # unlocks its own door - runs 225300/230601), so holding a door mutation
    # to the connect boundary is not reliably provable there. The door
    # channel's resync lever is the same lastSendMs=1 code path the GATED
    # faction rows prove (identical row shape + 10 s safety resend).
    if (-not $mut.facOk)     { $why += "host pre-arm faction sentinel failed" }
    if (-not $mut.moneyOk)   { $why += "host pre-arm wallet bump failed" }
    if (-not $mut.buildOk)   { $why += "host pre-arm building placement failed" }
    if (-not $mut.buildDone) { $why += "host pre-arm building never ramped complete" }
    $hC = Get-LatejoinCensus -File $HostFile
    $jC = Get-LatejoinCensus -File $JoinFile
    if ($hC.doors.Keys.Count -eq 0 -and $hC.money.Keys.Count -eq 0) { $why += "host census empty" }
    if ($jC.doors.Keys.Count -eq 0 -and $jC.money.Keys.Count -eq 0) { $why += "join census empty" }

    # FINDING: connect edge on the host (mutations must PRECEDE it).
    $conn = Select-String -Path $HostFile -Pattern 'handshake: peer present id=' -ErrorAction SilentlyContinue | Select-Object -First 1
    Write-Host ("    FINDING: host connect edge " + $(if ($null -ne $conn) { "logged" } else { "NOT logged" }))

    # FINDING: did the host HOLD the door mutation (the engine can revert a
    # baked door - run 224454)? A reverted door makes the door leg inconclusive.
    if ($mut.doorOk -and $hC.doors.ContainsKey($mut.doorHand)) {
        $field = if ($mut.doorMode -eq 'lock') { 'locked' } else { 'open' }
        $hLast = $hC.doors[$mut.doorHand][$hC.doors[$mut.doorHand].Count - 1]
        Write-Host ("    FINDING: host door $field final=" + $hLast[$field] + " want=" + $mut.doorWant + $(if ($hLast[$field] -eq $mut.doorWant) { " (held)" } else { " (REVERTED - door leg inconclusive)" }))
    }

    # FINDING: per-channel heal latency on the join (arm clock).
    $heal = Get-LatejoinHealLatency -Mut $mut -JoinCensus $jC
    foreach ($ch in @('door', 'fac', 'money')) {
        $t = $heal[$ch]
        $txt = if ($t -lt 0) { "NEVER agreed (the gap)" } else { "first agreed at t=${t}ms post-arm" }
        Write-Host "    FINDING: $ch heal - $txt"
    }
    # FINDING: the pre-connect building on the join (expected: never minted).
    $mint = $null
    if ($null -ne $mut.buildHand) {
        $mint = Select-String -Path $JoinFile -Pattern ("\[build\] MINT key=" + [regex]::Escape($mut.buildHand) + " .*rc=1") -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    Write-Host ("    FINDING: pre-connect building " + $(if ($null -ne $mint) { "MINTED on the join (unexpected with latejoinSync off)" } else { "never minted on the join (expected: the permanent loss)" }))

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  LATEJOIN-PROBE $v - $detail"
    return (Add-GateResult -Name "latejoin_probe" -Status $v `
                -Metrics @{ doorHealMs = $heal.door; facHealMs = $heal.fac; moneyHealMs = $heal.money } -Detail $detail)
}

# latejoin_sync (protocol 30): the connect-edge resync gate (latejoinSync ON).
# Same script as latejoin_probe. Gates:
#   1. all four host pre-arm mutations ok (same local leg as the probe);
#   2. every slow-heal channel CONVERGED on the join fast: door open state,
#      faction sentinel row and host wallet each agree within 20 s of the
#      join's arm (the resync bursts them at the connect edge - without it
#      door/faction wait for a 10 s safety resend at best);
#   3. the pre-connect building MINTED on the join (rc=1 on the placer's key)
#      and latched complete (STATE-RECV complete=1) - the probe's permanent
#      loss, closed.
function Test-LatejoinSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $mut = Get-LatejoinMutations -HostFile $HostFile
    if ($null -eq $mut) {
        Write-Host "  LATEJOIN-SYNC FAIL - host never logged its pre-arm mutations"
        return (Add-GateResult -Name "latejoin_sync" -Status FAIL -Detail "host pre-arm mutations missing")
    }
    # Door leg is FINDINGS-ONLY (town AI fights baked-door sentinels - see
    # Test-LatejoinProbe); faction + money + building are the gated legs.
    if (-not $mut.facOk)     { $why += "host pre-arm faction sentinel failed" }
    if (-not $mut.moneyOk)   { $why += "host pre-arm wallet bump failed" }
    if (-not $mut.buildOk)   { $why += "host pre-arm building placement failed" }
    if (-not $mut.buildDone) { $why += "host pre-arm building never ramped complete" }
    $jC = Get-LatejoinCensus -File $JoinFile

    # 2. converged-fast gates (door reported, not gated).
    $heal = Get-LatejoinHealLatency -Mut $mut -JoinCensus $jC
    $doorTxt = if ($heal.door -lt 0) { "never agreed (town-AI churn - findings-only leg)" } else { "first agreed at t=$($heal.door)ms post-arm" }
    Write-Host "    FINDING: door sentinel - $doorTxt"
    $names = @{ fac = 'faction sentinel'; money = 'host wallet' }
    foreach ($ch in @('fac', 'money')) {
        $t = $heal[$ch]
        if ($t -lt 0) { $why += "$($names[$ch]) never converged on the join" }
        elseif ($t -gt 20000) { $why += "$($names[$ch]) converged too slowly (t=${t}ms > 20 s - resync not effective?)" }
        else { Write-Host "    FINDING: $($names[$ch]) converged at t=${t}ms post-arm" }
    }

    # 3. the pre-connect building minted + complete.
    $minted = $false; $complete = $false
    if ($null -ne $mut.buildHand) {
        $keyRx = [regex]::Escape($mut.buildHand)
        $minted = $null -ne (Select-String -Path $JoinFile -Pattern ("\[build\] MINT key=" + $keyRx + " .*rc=1") -ErrorAction SilentlyContinue | Select-Object -First 1)
        $complete = $null -ne (Select-String -Path $JoinFile -Pattern ("\[build\] STATE-RECV key=" + $keyRx + " .*complete=1") -ErrorAction SilentlyContinue | Select-Object -First 1)
    }
    if (-not $minted)   { $why += "pre-connect building never minted on the join" }
    if (-not $complete) { $why += "pre-connect building never latched complete on the join" }
    if ($minted -and $complete) { Write-Host "    FINDING: pre-connect building MINTED + complete on the join (the probe's permanent gap, closed)" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  LATEJOIN-SYNC $v - doorHeal=$($heal.door)ms facHeal=$($heal.fac)ms moneyHeal=$($heal.money)ms minted=$minted $detail"
    return (Add-GateResult -Name "latejoin_sync" -Status $v `
                -Metrics @{ doorHealMs = $heal.door; facHealMs = $heal.fac; moneyHealMs = $heal.money; minted = $minted } -Detail $detail)
}

# save_probe (protocol 31 phase 12a): the coordinated-save runtime unknowns,
# retired. The HOST issued engine::saveGameAs('coopresume') mid-session with
# the SaveManager::save detour installed (saveSync coordination OFF - pure
# measurement). Gates the LOCAL legs on the host:
#   1. the save detour FIRED ("[save] LOCAL-SAVE name='coopresume'");
#   2. getCurrentGame/getSavePath resolved at runtime (SAVEINFO ok=1 with a
#      non-empty savePath - spike 39's RVAs, validated);
#   3. the folder-quiescence completion edge was OBSERVED (SAVEDONE
#      kind=quiesced with files > 0).
# FINDINGs (not gated): completion latency, folder inventory, the post-save
# getCurrentGame value (does the engine flip it to the new name?), and the
# widest main-thread tick gap while the save wrote (the hitch measurement).
function Test-SaveProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1. the detour edge.
    $edge = Select-String -Path $HostFile -Pattern "\[save\] LOCAL-SAVE name='coopresume' autosave=0" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $edge) { $why += "save detour never fired for 'coopresume'" }

    # 2. runtime path resolution.
    $before = Select-String -Path $HostFile -Pattern "SCENARIO SAVEINFO when=before ok=(\d) curGame='([^']*)' savePath='([^']*)'" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $before) { $why += "host never logged SAVEINFO" }
    else {
        $g = $before.Matches[0].Groups
        if ($g[1].Value -ne '1' -or $g[3].Value -eq '') { $why += "getCurrentGame/getSavePath did not resolve (ok=$($g[1].Value) path='$($g[3].Value)')" }
        else { Write-Host "    FINDING: runtime save identity - curGame='$($g[2].Value)' savePath='$($g[3].Value)' (spike 39 RVAs validated)" }
    }
    $after = Select-String -Path $HostFile -Pattern "SCENARIO SAVEINFO when=after ok=\d curGame='([^']*)'" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $after) {
        $cg = $after.Matches[0].Groups[1].Value
        Write-Host ("    FINDING: post-save getCurrentGame='" + $cg + "'" + $(if ($cg -eq 'coopresume') { " (flipped to the saved name)" } else { " (did NOT flip)" }))
    }

    # 3. completion edge + latency findings.
    $waitMs = -1; $files = 0; $bytes = 0
    $done = Select-String -Path $HostFile -Pattern 'SCENARIO SAVEDONE kind=(quiesced|timeout) files=(\d+) bytes=(\d+) waitMs=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $done) { $why += "folder-quiescence completion never observed (no SAVEDONE)" }
    else {
        $g = $done.Matches[0].Groups
        $files = [int]$g[2].Value; $bytes = [long]$g[3].Value; $waitMs = [int]$g[4].Value
        if ($g[1].Value -ne 'quiesced') { $why += "completion was a TIMEOUT (the save never landed on disk?)" }
        elseif ($files -eq 0) { $why += "SAVEDONE reported an empty folder" }
        else { Write-Host "    FINDING: save completed in ${waitMs}ms - $files files / $bytes bytes (the transfer payload + settle window sizing)" }
    }
    $hitchMs = -1
    $hitch = Select-String -Path $HostFile -Pattern 'SCENARIO SAVEHITCH maxTickGapMs=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $hitch) {
        $hitchMs = [int]$hitch.Matches[0].Groups[1].Value
        Write-Host "    FINDING: widest main-thread tick gap during the save = ${hitchMs}ms (the gameplay hitch)"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SAVE-PROBE $v - waitMs=$waitMs files=$files hitchMs=$hitchMs $detail"
    return (Add-GateResult -Name "save_probe" -Status $v `
                -Metrics @{ waitMs = $waitMs; files = $files; bytes = $bytes; hitchMs = $hitchMs } -Detail $detail)
}

# load_probe (protocol 32 phase 13a): the coordinated-load runtime unknowns,
# retired. The HOST issued a coordinated saveGameAs('coopresume') then a
# MID-SESSION engine::loadSave('coopresume') with the SaveManager::load
# detour installed (loadSync coordination OFF - pure measurement). Gates the
# LOCAL legs on the host:
#   1. the load detour FIRED ("[load] LOCAL-LOAD name='coopresume'");
#   2. the mid-session load was ISSUED cleanly (LOADISSUE ok=1);
#   3. the world actually SWAPPED and came back: the scenario's live
#      drop/return pair (LOADSWAPDONE) AND the Plugin's reload edge
#      ("[load] WORLD-RELOAD");
#   4. the pre-load squad hand RESOLVED again in the fresh world
#      (LOADCENSUS when=after resolved=1) - the same-lineage guarantee.
# FINDINGs (not gated): swap latency + whether mainLoop_hook kept ticking
# through the load screen (hookTicksDuringSwap - the 13b session-reset
# design hinges on it), the widest tick gap around the swap (hitch), the
# transfer-done state at load time, and the JOIN's divergence baseline (it
# deliberately does NOT load in 13a - its half is phase 13b).
function Test-LoadProbe {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1. the detour edge.
    $edge = Select-String -Path $HostFile -Pattern "\[load\] LOCAL-LOAD name='coopresume'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $edge) { $why += "load detour never fired for 'coopresume'" }

    # 2. the mid-session issue (+ the deferred-signal state finding).
    $issue = Select-String -Path $HostFile -Pattern "SCENARIO LOADISSUE name='coopresume' ok=(\d) xferDone=(\d)(?: signal=(-?\d+) delay=(-?\d+))?" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $issue) { $why += "host never issued the mid-session load" }
    else {
        $g = $issue.Matches[0].Groups
        if ($g[1].Value -ne '1') { $why += "engine::loadSave returned failure" }
        Write-Host "    FINDING: load issued with transfer done=$($g[2].Value) signal=$($g[3].Value) delay=$($g[4].Value)"
    }
    $exec = Select-String -Path $HostFile -Pattern 'SCENARIO LOADEXEC ok=(\d) sigBefore=(-?\d+) sigAfter=(-?\d+) liveAfter=(\d)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $exec) {
        $g = $exec.Matches[0].Groups
        Write-Host "    FINDING: manual execute() pump ok=$($g[1].Value) signal $($g[2].Value)->$($g[3].Value) liveAfter=$($g[4].Value) (run 1: load() only SETS the deferred signal; nothing consumes it mid-session)"
    }

    # 3. the world actually swapped - the scenario's completion latch (live
    # drop/return edge OR the synchronous-inside-execute variant).
    $swapDone = Select-String -Path $HostFile -Pattern 'SCENARIO LOADSWAPDONE' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $swapDone) { $why += "world swap never completed (no LOADSWAPDONE)" }
    # The Plugin's reload edge is a FINDING, not a gate: a fully synchronous
    # swap inside execute() never shows mainLoop a non-live frame.
    $swapMs = -1; $hookTicks = -1
    $reload = Select-String -Path $HostFile -Pattern '\[load\] WORLD-RELOAD swapMs=(\d+) hookTicksDuringSwap=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $reload) { Write-Host "    FINDING: no WORLD-RELOAD edge from the Plugin (swap invisible to gameplayLive polling - synchronous, or never happened)" }
    else {
        $g = $reload.Matches[0].Groups
        $swapMs = [int]$g[1].Value; $hookTicks = [int]$g[2].Value
        $ticking = if ($hookTicks -gt 0) { "mainLoop_hook KEPT TICKING through the load screen ($hookTicks ticks)" } else { "mainLoop_hook did NOT tick during the load screen" }
        Write-Host "    FINDING: world swap took ${swapMs}ms; $ticking (13b session-reset design input)"
    }

    # 4. post-swap hand re-resolve.
    $census = Select-String -Path $HostFile -Pattern 'SCENARIO LOADCENSUS when=after resolved=(\d) pos=([-0-9.,]+) n=(\d+) leaderChanged=(\d)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $census) { $why += "post-load census never ran" }
    else {
        $g = $census.Matches[0].Groups
        if ($g[1].Value -ne '1') { $why += "pre-load squad hand did NOT resolve after the swap" }
        else { Write-Host "    FINDING: pre-load hand resolved post-swap at pos=$($g[2].Value) (squad n=$($g[3].Value)); leader Character* changed=$($g[4].Value) (the stale-pointer hazard the session reset covers)" }
    }

    $hitchMs = -1
    $hitch = Select-String -Path $HostFile -Pattern 'SCENARIO LOADHITCH maxTickGapMs=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $hitch) {
        $hitchMs = [int]$hitch.Matches[0].Groups[1].Value
        Write-Host "    FINDING: widest main-thread tick gap across the swap = ${hitchMs}ms"
    }

    # JOIN divergence baseline (not gated): the join must NOT have reloaded.
    if ($JoinFile -and (Test-Path $JoinFile)) {
        $joinReload = Select-String -Path $JoinFile -Pattern '\[load\] WORLD-RELOAD' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $joinReload) { Write-Host "    FINDING: UNEXPECTED - the join reloaded too (loadSync was supposed to be OFF)" }
        else { Write-Host "    FINDING: join never reloaded (expected 13a divergence baseline - the 13b coordination closes this)" }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  LOAD-PROBE $v - swapMs=$swapMs hookTicks=$hookTicks hitchMs=$hitchMs $detail"
    return (Add-GateResult -Name "load_probe" -Status $v `
                -Metrics @{ swapMs = $swapMs; hookTicks = $hookTicks; hitchMs = $hitchMs } -Detail $detail)
}

# save_sync (protocol 31 phase 12c): the coordinated-save round trip, gated
# end to end on BOTH logs:
#   1. host: the detour edge fired for 'coopresume' (LOCAL-SAVE);
#   2. host: the folder QUIESCED (kind=quiesced, files>0);
#   3. host: the transfer completed (XFER-SENT id/files/bytes);
#   4. join: the staged save VERIFIED + COMMITTED (XFER-COMMIT, badCrc=0)
#      with files+bytes EQUAL to what the host sent (the integrity proof);
#   5. host: the join's ACK arrived ok=1 (XFER-ACK).
# Also used by save_stage1 (the resume_test.ps1 stage-1 variant), where the
# host additionally baked a part-built site first - SAVEBUILD/SAVEPROG are
# findings here; stage 2's oracle proves the same-hand claim.
function Test-SaveSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1. the detour edge on the host.
    $edge = Select-String -Path $HostFile -Pattern "\[save\] LOCAL-SAVE name='coopresume'" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $edge) { $why += "host save detour never fired for 'coopresume'" }

    # 2. quiescence.
    $q = Select-String -Path $HostFile -Pattern "\[save\] QUIESCED kind=(\w+) name='coopresume' files=(\d+) bytes=(\d+) waitMs=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $qFiles = 0
    if ($null -eq $q) { $why += "host never logged QUIESCED for 'coopresume'" }
    else {
        $g = $q.Matches[0].Groups
        $qFiles = [int]$g[2].Value
        if ($g[1].Value -ne 'settled') { $why += "completion was a TIMEOUT, not quiescence" }
        elseif ($qFiles -eq 0) { $why += "QUIESCED reported an empty folder" }
        else { Write-Host "    FINDING: save quiesced in $($g[4].Value)ms - $qFiles files / $($g[3].Value) bytes" }
    }

    # 3. the transfer's DONE went out.
    $sent = Select-String -Path $HostFile -Pattern "\[save\] XFER-SENT id=(\d+) files=(\d+) bytes=(\d+) ms=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $sFiles = 0; $sBytes = [long]0; $xferMs = -1
    if ($null -eq $sent) { $why += "host never logged XFER-SENT (transfer incomplete)" }
    else {
        $g = $sent.Matches[0].Groups
        $sFiles = [int]$g[2].Value; $sBytes = [long]$g[3].Value; $xferMs = [int]$g[4].Value
        Write-Host "    FINDING: transfer sent $sFiles files / $sBytes bytes in ${xferMs}ms"
    }

    # 4. the join committed with EQUAL files+bytes and zero bad CRCs.
    $commit = Select-String -Path $JoinFile -Pattern "\[save\] XFER-(COMMIT|FAILED) id=(\d+) name='coopresume' files=(\d+) bytes=(\d+) badCrc=(\d+) ms=(\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $cFiles = 0; $cBytes = [long]0
    if ($null -eq $commit) { $why += "join never logged a commit/fail (transfer never finished on the join)" }
    else {
        $g = $commit.Matches[0].Groups
        $cFiles = [int]$g[3].Value; $cBytes = [long]$g[4].Value
        if ($g[1].Value -ne 'COMMIT') { $why += "join commit FAILED (badCrc=$($g[5].Value))" }
        elseif ($g[5].Value -ne '0') { $why += "join committed with badCrc=$($g[5].Value)" }
        if ($null -ne $sent -and $g[1].Value -eq 'COMMIT') {
            if ($cFiles -ne $sFiles) { $why += "file count mismatch (host sent $sFiles, join committed $cFiles)" }
            if ($cBytes -ne $sBytes) { $why += "byte count mismatch (host sent $sBytes, join committed $cBytes)" }
        }
    }

    # 5. the ACK closed the loop on the host.
    $ack = Select-String -Path $HostFile -Pattern "\[save\] XFER-ACK id=(\d+) ok=(\d)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $ack) { $why += "host never received the join's ACK" }
    elseif ($ack.Matches[0].Groups[2].Value -ne '1') { $why += "join ACKed ok=0" }

    # Stage-1 findings (present only for save_stage1 runs).
    $build = Select-String -Path $HostFile -Pattern "SCENARIO SAVEBUILD rc=\d+ ok=(\d) sid='([^']*)' hand=([\d.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -ne $build) {
        $g = $build.Matches[0].Groups
        Write-Host "    FINDING: stage-1 building ok=$($g[1].Value) sid='$($g[2].Value)' hand=$($g[3].Value) (baked into the transferred save)"
        if ($g[1].Value -ne '1') { $why += "stage-1 building placement failed (nothing to prove at resume)" }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SAVE-SYNC $v - sent=$sFiles/$sBytes committed=$cFiles/$cBytes xferMs=$xferMs $detail"
    return (Add-GateResult -Name "save_sync" -Status $v `
                -Metrics @{ sentFiles = $sFiles; sentBytes = $sBytes; commitFiles = $cFiles; commitBytes = $cBytes; xferMs = $xferMs } -Detail $detail)
}

# save_resume (protocol 31 phase 12c, resume_test.ps1 stage 2): the identity-
# reset proof. Both clients were relaunched on 'coopresume' - the save the
# stage-1 coordinated transfer delivered to the join (stage 2 runs with NO
# harness save mirroring, so the join really loads what the TRANSFER wrote).
# The stage-1 building was baked PART-built (prog ~0.5), so it exists in no
# other save; the gate is that it enumerates on BOTH sides under the SAME
# save-stable hand with matching progress - one save, one hand, both clients.
function Test-SaveResume {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $siteRegex = "SCENARIO RESUMESITE hand=([\d.]+) sid='([^']*)' prog=([\d.]+) complete=(\d)"

    function Get-FinalSites([string]$File) {
        $out = @{}
        foreach ($m in @(Select-String -Path $File -Pattern $siteRegex -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $out[$g[1].Value] = @{ sid = $g[2].Value; prog = [double]$g[3].Value; complete = [int]$g[4].Value }
        }
        return $out
    }
    $hSites = Get-FinalSites $HostFile
    $jSites = Get-FinalSites $JoinFile

    if ($hSites.Count -eq 0) { $why += "host enumerated no construction sites after resume" }
    if ($jSites.Count -eq 0) { $why += "join enumerated no construction sites after resume" }

    $shared = 0
    foreach ($hand in $hSites.Keys) {
        if (-not $jSites.ContainsKey($hand)) {
            $why += "host site hand=$hand missing on the join (hand diverged - the identity reset did NOT happen)"
            continue
        }
        $h = $hSites[$hand]; $j = $jSites[$hand]
        if ($h.sid -ne $j.sid) { $why += "hand=$hand sid mismatch (host '$($h.sid)' join '$($j.sid)')" }
        elseif ([math]::Abs($h.prog - $j.prog) -gt 0.05) { $why += "hand=$hand progress diverged (host $($h.prog) join $($j.prog))" }
        else {
            $shared++
            Write-Host "    FINDING: SAME-HAND site hand=$hand sid='$($h.sid)' prog=$($h.prog) on BOTH clients (the baked-identity proof)"
        }
    }
    foreach ($hand in $jSites.Keys) {
        if (-not $hSites.ContainsKey($hand)) { $why += "join-only site hand=$hand (ghost - saves not identical)" }
    }
    if ($shared -eq 0 -and $why.Count -eq 0) { $why += "no shared same-hand site found" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  SAVE-RESUME $v - hostSites=$($hSites.Count) joinSites=$($jSites.Count) sameHand=$shared $detail"
    return (Add-GateResult -Name "save_resume" -Status $v `
                -Metrics @{ hostSites = $hSites.Count; joinSites = $jSites.Count; sameHand = $shared } -Detail $detail)
}

# load_sync (protocol 32 phase 13c): the coordinated-load round trip, gated
# end to end on BOTH logs. The host placed a session-runtime building,
# coordinated-saved 'coopresume' (the join committed a byte-identical copy),
# then loaded it mid-session; the coordination must have driven the join to
# load the SAME save:
#   1. host: the load edge broadcast the order ("[load] GO->join");
#   2. join: the GO arrived, fingerprint MATCHed, and the join ISSUED its own
#      load ("[load] GO ... MATCH -> loading") - a NACK/transfer fallback on
#      this identical-copy run is a divergence bug, not a fallback;
#   3. BOTH sides completed a world swap (SCENARIO LSSWAPDONE) and ran the
#      protocol-32 session reset ("[load] session reset");
#   4. the pre-load building enumerates on BOTH sides POST-swap under the
#      SAME save-stable hand (LSSITE cross-check - the identity claim that
#      makes all hand-keyed replication valid after a coordinated load).
# FINDINGs: swap evidence per side (WORLD-RELOAD vs synchronous), the join's
# suppression lever state, and any NACK/transfer leg that fired.
function Test-LoadSync {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # Pre-req: the host's build + coordinated save legs.
    $build = Select-String -Path $HostFile -Pattern "SCENARIO LSBUILD rc=\d+ ok=(\d) sid='([^']*)' hand=([\d.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $build) { $why += "host never placed the pre-load building" }
    elseif ($build.Matches[0].Groups[1].Value -ne '1') { $why += "host building placement failed (no identity evidence to carry across the load)" }
    else { Write-Host "    FINDING: pre-load building hand=$($build.Matches[0].Groups[3].Value) sid='$($build.Matches[0].Groups[2].Value)' (exists in NO baked save)" }
    $ackLine = Select-String -Path $HostFile -Pattern 'SCENARIO LSACK ok=(\d)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $ackLine) { $why += "host never saw the join's save-commit ACK (the coordinated save leg broke)" }
    elseif ($ackLine.Matches[0].Groups[1].Value -ne '1') { $why += "join ACKed the pre-load save ok=0" }

    # 1. the host's load edge broadcast the coordinated order.
    $go = Select-String -Path $HostFile -Pattern "\[load\] GO->join id=(\d+) name='coopresume' fp=([0-9a-f]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $go) { $why += "host never broadcast PKT_LOAD_GO" }

    # 2. the join received it, its copy fingerprint-MATCHed, and it loaded.
    $joinGo = Select-String -Path $JoinFile -Pattern "\[load\] GO id=(\d+) name='coopresume' fp=([0-9a-f]+) MATCH -> loading" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $joinGo) {
        $nack = Select-String -Path $JoinFile -Pattern "\[load\] GO id=\d+ name='coopresume' hostFp=([0-9a-f]+) localFp=([0-9a-f]+) (\w+) -> NACK" -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $nack) {
            $g = $nack.Matches[0].Groups
            Write-Host "    FINDING: join NACKed ($($g[3].Value): hostFp=$($g[1].Value) localFp=$($g[2].Value)) - the transfer fallback leg fired"
            # The fallback still has to LAND: the post-transfer load line.
            $late = Select-String -Path $JoinFile -Pattern "\[load\] transfer committed -> loading 'coopresume'" -ErrorAction SilentlyContinue | Select-Object -Last 1
            if ($null -eq $late) { $why += "join's copy diverged AND the transfer fallback never completed its load" }
            else { $why += "join's copy DIVERGED right after a verified commit (fingerprint bug or save mutated between commit and load)" }
        } else {
            $why += "join never received/handled PKT_LOAD_GO"
        }
    }

    # 3. both sides swapped worlds and session-reset.
    foreach ($side in @(@($HostFile, 'host'), @($JoinFile, 'join'))) {
        $file = $side[0]; $tag = $side[1]
        $swap = Select-String -Path $file -Pattern 'SCENARIO LSSWAPDONE' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $swap) { $why += "$tag never completed its world swap (no LSSWAPDONE)" }
        else {
            $reload = Select-String -Path $file -Pattern '\[load\] WORLD-RELOAD swapMs=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
            if ($null -ne $reload) { Write-Host "    FINDING: $tag world swap took $($reload.Matches[0].Groups[1].Value)ms" }
            else { Write-Host "    FINDING: $tag swap was synchronous (no WORLD-RELOAD edge visible to polling)" }
        }
        $reset = Select-String -Path $file -Pattern '\[load\] session reset' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $reset) { $why += "$tag never ran the protocol-32 session reset" }
    }

    # 4. the post-load same-hand site cross-check (the identity claim).
    $siteRegex = "SCENARIO LSSITE hand=([\d.]+) sid='([^']*)' prog=([\d.]+) complete=(\d)"
    function Get-LsSites([string]$File) {
        $out = @{}
        foreach ($m in @(Select-String -Path $File -Pattern $siteRegex -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $out[$g[1].Value] = @{ sid = $g[2].Value; prog = [double]$g[3].Value }
        }
        return $out
    }
    $hSites = Get-LsSites $HostFile
    $jSites = Get-LsSites $JoinFile
    if ($hSites.Count -eq 0) { $why += "host enumerated no sites post-load" }
    if ($jSites.Count -eq 0) { $why += "join enumerated no sites post-load" }
    $shared = 0
    foreach ($hand in $hSites.Keys) {
        if (-not $jSites.ContainsKey($hand)) { $why += "host site hand=$hand missing on the join post-load (identity diverged)"; continue }
        $h = $hSites[$hand]; $j = $jSites[$hand]
        if ($h.sid -ne $j.sid) { $why += "hand=$hand sid mismatch post-load (host '$($h.sid)' join '$($j.sid)')" }
        else {
            $shared++
            Write-Host "    FINDING: SAME-HAND site hand=$hand sid='$($h.sid)' on BOTH clients POST-load (the coordinated-load identity proof)"
        }
    }
    foreach ($hand in $jSites.Keys) {
        if (-not $hSites.ContainsKey($hand)) { $why += "join-only site hand=$hand post-load (ghost - the loads diverged)" }
    }
    if ($shared -eq 0 -and $why.Count -eq 0) { $why += "no shared same-hand site post-load" }

    # Suppression-lever finding (the join's manual loads route to the host).
    $sup = Select-String -Path $JoinFile -Pattern '\[load\] JOIN load suppression ON' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $sup) { Write-Host "    FINDING: join load suppression engaged (host arbitrates loads)" }
    else { Write-Host "    FINDING: join load suppression NEVER engaged (peer-presence edge missing?)" }

    # 5. Portrait presence (protocol 36, blank-avatars session bug): the
    # coordinated save must have captured the portrait atlas - a quiescence
    # that shipped without it (or any load that warned about a missing one)
    # reproduces the blank squad-tab avatars.
    foreach ($side in @(@($HostFile, 'host'), @($JoinFile, 'join'))) {
        if (Select-String -Path $side[0] -Pattern 'WARN quiesced WITHOUT portraits_texture\.png' -Quiet) {
            $why += "$($side[1]) quiesced without portraits_texture.png (portrait gate expired)"
        }
        if (Select-String -Path $side[0] -Pattern '\[load\] WARN save .* has no portraits_texture\.png' -Quiet) {
            $why += "$($side[1]) loaded a save without portraits_texture.png"
        }
    }
    $portrait = Join-Path $env:LOCALAPPDATA 'kenshi\save\coopresume\portraits_texture.png'
    if (-not (Test-Path $portrait)) { $why += "committed 'coopresume' has no portraits_texture.png on disk" }
    else { Write-Host "    FINDING: portraits_texture.png present in the committed save ($([math]::Round((Get-Item $portrait).Length / 1KB, 1)) KB)" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  LOAD-SYNC $v - hostSites=$($hSites.Count) joinSites=$($jSites.Count) sameHand=$shared $detail"
    return (Add-GateResult -Name "load_sync" -Status $v `
                -Metrics @{ hostSites = $hSites.Count; joinSites = $jSites.Count; sameHand = $shared } -Detail $detail)
}

# vendor_trade (protocol 22 phase 1c): the buyer-side purchase composite gate.
# Each side performed the two buyer-side mutations of one purchase (wallet
# debit + item into the tab leader's inventory) on the tab it OWNS; the gate is
# that BOTH effects converged on the peer:
#   1. both TRADE lines ok=1 (the local mutations succeeded);
#   2. the peer's final WALLET for the traded rank equals the trader's wAfter
#      (the debit crossed on PKT_MONEY);
#   3. the final TINV content hash for the traded rank matches host-vs-join
#      (the item crossed on the inventory snapshot channel).
function Test-VendorTrade {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $tradeRegex = 'SCENARIO TRADE who=(host|join) rank=(\d+) ok=(\d) sid=''([^'']*)'' price=(-?\d+) wBefore=(-?\d+) wAfter=(-?\d+)'
    $hostTrade = Select-String -Path $HostFile -Pattern $tradeRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    $joinTrade = Select-String -Path $JoinFile -Pattern $tradeRegex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $hostTrade) { $why += "host never logged its TRADE" }
    elseif ($hostTrade.Matches[0].Groups[3].Value -ne '1') { $why += "host TRADE failed locally" }
    if ($null -eq $joinTrade) { $why += "join never logged its TRADE" }
    elseif ($joinTrade.Matches[0].Groups[3].Value -ne '1') { $why += "join TRADE failed locally" }

    $hw = Get-WalletSeries -File $HostFile
    $jw = Get-WalletSeries -File $JoinFile

    # Final TINV hash per rank per side.
    function Get-TinvFinal([string]$File) {
        $out = @{}
        foreach ($m in @(Select-String -Path $File -Pattern 'SCENARIO TINV rank=(\d+) count=(\d+) hash=(\d+)' -ErrorAction SilentlyContinue)) {
            $out[[int]$m.Matches[0].Groups[1].Value] = @{ count = [int]$m.Matches[0].Groups[2].Value
                                                          hash  = [long]$m.Matches[0].Groups[3].Value }
        }
        return $out
    }
    $hInv = Get-TinvFinal $HostFile
    $jInv = Get-TinvFinal $JoinFile

    $crossed = 0
    foreach ($leg in @(@($hostTrade, $jw, 'host->join'), @($joinTrade, $hw, 'join->host'))) {
        $t = $leg[0]; $peerW = $leg[1]; $tag = $leg[2]
        if ($null -eq $t -or $t.Matches[0].Groups[3].Value -ne '1') { continue }
        $rank   = [int]$t.Matches[0].Groups[2].Value
        $wAfter = [int]$t.Matches[0].Groups[7].Value
        # 2. wallet debit crossed.
        if (-not $peerW.ContainsKey($rank)) { $why += "$tag rank $rank absent from peer WALLET series" }
        else {
            $end = $peerW[$rank][$peerW[$rank].Count - 1].money
            if ($end -eq $wAfter) { $crossed++ }
            else { $why += "$tag wallet debit did not cross (peer rank $rank ended $end, want $wAfter)" }
        }
        # 3. inventory content converged.
        if (-not ($hInv.ContainsKey($rank) -and $jInv.ContainsKey($rank))) {
            $why += "$tag rank $rank TINV series missing on a side"
        } elseif ($hInv[$rank].hash -ne $jInv[$rank].hash) {
            $why += "$tag rank $rank inventory hash diverged (host $($hInv[$rank].hash) join $($jInv[$rank].hash))"
        }
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  VENDOR-TRADE $v - walletCrossed=$crossed/2 $detail"
    return (Add-GateResult -Name "vendor_trade" -Status $v `
                -Metrics @{ walletCrossed = $crossed } -Detail $detail)
}

# spawn_sync (protocol 21, spawnSync ON): the full proxy-replication gate.
#   1. BIND: the join minted a proxy ("[spawn] proxy BOUND hand=...") for at
#      least half the host's near spawns AND at least one far spawn (the
#      teleport-far leg - interest + proxy channel working at distance).
#   2. TRACK: the join's PROXY position series (streamed key + actual proxy
#      body position) follows the host's MEMBER series per hand - time-aligned
#      median within tolerance for at least half the bound hands.
#   3. SUPPRESS: the join's own local runtime spawns still get hidden
#      ([authority] suppress) - proxies must not have broken host authority.
function Test-SpawnSync {
    param([string]$HostFile, [string]$JoinFile, [double]$Tol)
    $hostLegs = Get-SpawnHands -File $HostFile
    $joinLegs = Get-SpawnHands -File $JoinFile
    $near = if ($hostLegs.ContainsKey('near')) { @($hostLegs['near']) } else { @() }
    $far  = if ($hostLegs.ContainsKey('far'))  { @($hostLegs['far'])  } else { @() }
    $jsp  = if ($joinLegs.ContainsKey('join')) { @($joinLegs['join']) } else { @() }
    if ($near.Count -eq 0 -or $far.Count -eq 0) {
        Write-Host "  SPAWN-SYNC FAIL - host spawn legs missing (near=$($near.Count) far=$($far.Count))"
        return (Add-GateResult -Name "spawn_sync" -Status FAIL -Detail "host spawn legs missing")
    }

    # 1. BIND coverage per leg.
    $bound = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[spawn\] proxy BOUND hand=([\d,]+)' -ErrorAction SilentlyContinue)) {
        $bound[$m.Matches[0].Groups[1].Value] = $true
    }
    $nearBound = @($near | Where-Object { $bound.ContainsKey($_) }).Count
    $farBound  = @($far  | Where-Object { $bound.ContainsKey($_) }).Count
    $needNear  = [Math]::Ceiling($near.Count / 2.0)
    $bindOk = ($nearBound -ge $needNear) -and ($farBound -ge 1)
    Write-Host ("  SPAWN-SYNC bind " + $(if ($bindOk) { "PASS" } else { "FAIL" }) +
                " - near $nearBound/$($near.Count) (need >= $needNear), far $farBound/$($far.Count) (need >= 1)")

    # 2. TRACK: PROXY series (join) vs MEMBER series (host) for the bound hands.
    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $P = Get-ScenarioSeries -File $JoinFile -Kind "PROXY"
    $judged = 0; $tracked = 0; $worst = 0.0
    foreach ($hand in ($near + $far)) {
        if (-not $bound.ContainsKey($hand)) { continue }
        $key = Convert-SpawnHandToSeriesKey -Hand $hand
        if ($null -eq $key -or -not $H.ContainsKey($key) -or -not $P.ContainsKey($key)) { continue }
        $dists = New-Object System.Collections.ArrayList
        foreach ($ps in $P[$key]) {
            $best = [double]::MaxValue; $bp = $null
            foreach ($hs in $H[$key]) {
                $dt = [Math]::Abs($hs.t - $ps.t)
                if ($dt -lt $best) { $best = $dt; $bp = $hs.p }
            }
            if ($best -le 800 -and $bp) {
                $dx = $bp[0]-$ps.p[0]; $dy = $bp[1]-$ps.p[1]; $dz = $bp[2]-$ps.p[2]
                [void]$dists.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            }
        }
        if ($dists.Count -lt 5) { continue }
        $sorted = $dists | Sort-Object
        $med = $sorted[[int]($sorted.Count/2)]
        $judged++
        if ($med -le $Tol) { $tracked++ }
        if ($med -gt $worst) { $worst = $med }
    }
    $trackOk = ($judged -ge 1) -and ($tracked -ge [Math]::Ceiling($judged / 2.0))
    Write-Host ("  SPAWN-SYNC track " + $(if ($trackOk) { "PASS" } else { "FAIL" }) +
                " - $tracked/$judged bound proxies within $Tol (worstMedian=$([Math]::Round($worst,1)))")

    # 3. SUPPRESS: join-local spawns still hidden by host authority.
    $jSupp = Measure-JoinSpawnSuppression -JoinFile $JoinFile -JoinHands $jsp
    $suppOk = ($jsp.Count -ge 1) -and ($jSupp -ge [Math]::Ceiling($jsp.Count / 2.0))
    Write-Host ("  SPAWN-SYNC suppress " + $(if ($suppOk) { "PASS" } else { "FAIL" }) +
                " - $jSupp/$($jsp.Count) join-local spawns suppressed")

    $ok = $bindOk -and $trackOk -and $suppOk
    $why = @()
    if (-not $bindOk)  { $why += "proxy bind coverage below gate" }
    if (-not $trackOk) { $why += "bound proxies not tracking the host bodies" }
    if (-not $suppOk)  { $why += "join-local spawns not suppressed" }
    $detail = $why -join "; "
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  SPAWN-SYNC $v $detail"
    return (Add-GateResult -Name "spawn_sync" -Status $v `
                -Metrics @{ near = $near.Count; nearBound = $nearBound
                            far = $far.Count; farBound = $farBound
                            judged = $judged; tracked = $tracked
                            worstMedian = [Math]::Round($worst, 2)
                            joinLocal = $jsp.Count; joinSuppressed = $jSupp } -Detail $detail)
}

# npc_census (protocol 36): wide-radius ghost culling. The join spawns 4
# runtime NPCs and parks them ~600 u out - beyond the ~200 u stream bubble
# (the near suppression pass can never judge them), inside the census radius.
# Gates:
#   1. CHANNEL: the host logged "[census] sent" and the join "[census] recv"
#      (the 1 Hz existence list actually flowed).
#   2. CULL: every join-local ghost hand drew a "[census] cull NPC hand=i,s"
#      line (existence culling reached census range).
#   3. RESTRAINT: wide-radius culls stayed within the ghost budget - a legit
#      census NPC being mass-suppressed would show as extra culled hands
#      (small slack for genuine host/join boundary divergence).
function Test-NpcCensus {
    param([string]$HostFile, [string]$JoinFile)
    $joinLegs = Get-SpawnHands -File $JoinFile
    $ghosts = if ($joinLegs.ContainsKey('ghost')) { @($joinLegs['ghost']) } else { @() }
    if ($ghosts.Count -eq 0) {
        Write-Host "  NPC-CENSUS FAIL - join never spawned its ghost squad"
        return (Add-GateResult -Name "npc_census" -Status FAIL -Detail "no ghost spawns")
    }

    # 1. CHANNEL evidence on both ends.
    $sent = (Test-Path $HostFile) -and (Select-String -Path $HostFile -Pattern '\[census\] sent n=(\d+)' -Quiet)
    $recv = (Test-Path $JoinFile) -and (Select-String -Path $JoinFile -Pattern '\[census\] recv n=(\d+)' -Quiet)
    $chanOk = $sent -and $recv
    Write-Host ("  NPC-CENSUS channel " + $(if ($chanOk) { "PASS" } else { "FAIL" }) +
                " - host sent=$sent join recv=$recv")

    # 2. CULL: every ghost hand suppressed by the WIDE pass.
    $culled = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[census\] cull NPC hand=(\d+,\d+)' -ErrorAction SilentlyContinue)) {
        $culled[$m.Matches[0].Groups[1].Value] = $true
    }
    $gCulled = 0
    foreach ($h in $ghosts) {
        $p = $h.Split(',')
        if ($p.Count -eq 5 -and $culled.ContainsKey("$($p[3]),$($p[4])")) { $gCulled++ }
    }
    $cullOk = ($gCulled -eq $ghosts.Count)
    Write-Host ("  NPC-CENSUS cull " + $(if ($cullOk) { "PASS" } else { "FAIL" }) +
                " - $gCulled/$($ghosts.Count) ghost hands culled at census range")

    # 3. RESTRAINT: no mass-suppression of legitimate census NPCs.
    $extra = $culled.Count - $gCulled
    $restraintOk = ($extra -le 2)
    Write-Host ("  NPC-CENSUS restraint " + $(if ($restraintOk) { "PASS" } else { "FAIL" }) +
                " - $extra non-ghost wide-radius culls (<= 2)")

    $ok = $chanOk -and $cullOk -and $restraintOk
    $why = @()
    if (-not $chanOk)      { $why += "census channel silent" }
    if (-not $cullOk)      { $why += "ghost hands not culled" }
    if (-not $restraintOk) { $why += "legitimate NPCs mass-suppressed" }
    $detail = $why -join "; "
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  NPC-CENSUS $v $detail"
    return (Add-GateResult -Name "npc_census" -Status $v `
                -Metrics @{ ghosts = $ghosts.Count; ghostsCulled = $gCulled
                            extraCulls = $extra; sent = $sent; recv = $recv } -Detail $detail)
}

# ---- Locomotion-quality oracles ----------------------------------------------------

# Smoothness (zero-advance fraction while the source moved). No SMOOTH line or a
# scenario that never drove a moving body -> SKIP.
# Since 2026-07-10 the plugin EXCLUDES frames captured during the session-start
# clock-slew catch-up (join sims at up to 2x while the host streams at 1x - a
# structural zero-step source that measured the slew, not the interp pipeline;
# it drove the historical 0.2-0.9 zeroFrac flake). The excluded count is
# reported as slewSkip=; a run whose motion fell almost entirely inside the
# slew window has too few scored frames for the gate to mean anything -> SKIP.
function Test-Smoothness {
    param([string]$File, [string]$Label = "join", [double]$MaxZeroFrac = 0.40,
          [int]$MinActiveFrames = 200)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO SMOOTH active=(\d+) .*zeroFrac=([\d\.]+).*maxStep=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO SMOOTH line (skipped)"
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no SMOOTH line")
    }
    $active   = [int]$line.Matches[0].Groups[1].Value
    $zeroFrac = [double]$line.Matches[0].Groups[2].Value
    $maxStep  = [double]$line.Matches[0].Groups[3].Value
    $slewSkip = 0
    if ($line.Line -match "slewSkip=(\d+)") { $slewSkip = [int]$Matches[1] }
    if ($active -lt $MinActiveFrames) {
        Write-Host "  [$Label] smoothness SKIP - active=$active (< $MinActiveFrames scored frames; slewSkip=$slewSkip fell in the clock-slew window)"
        return (Add-GateResult -Name "smoothness" -Status SKIP `
                    -Metrics @{ active = $active; slewSkip = $slewSkip } `
                    -Detail "only $active scored frames (slewSkip=$slewSkip)")
    }
    $ok = ($zeroFrac -le $MaxZeroFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] smoothness $v - zeroFrac=$zeroFrac (<= $MaxZeroFrac), maxStep=$maxStep, active=$active, slewSkip=$slewSkip"
    return (Add-GateResult -Name "smoothness" -Status $v -Metrics @{ zeroFrac = $zeroFrac; maxStep = $maxStep; active = $active; slewSkip = $slewSkip })
}

# Anim-truth (the float-bug detector). Too few translating frames -> SKIP.
function Test-AnimTruth {
    param([string]$File, [string]$Label = "join", [double]$MaxFloatFrac = 0.30)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO ANIM .*translate=(\d+) walkTruth=(\d+) floatFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO ANIM line (skipped)"
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Detail "no ANIM line")
    }
    $translate = [int]$line.Matches[0].Groups[1].Value
    $floatFrac = [double]$line.Matches[0].Groups[3].Value
    if ($translate -lt 30) {
        Write-Host "  [$Label] anim-truth SKIP - only $translate translating frame(s)"
        return (Add-GateResult -Name "anim_truth" -Status SKIP -Metrics @{ translate = $translate } -Detail "too few translating frames")
    }
    $ok = ($floatFrac -le $MaxFloatFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] anim-truth $v - floatFrac=$floatFrac (<= $MaxFloatFrac), translateFrames=$translate"
    return (Add-GateResult -Name "anim_truth" -Status $v -Metrics @{ floatFrac = $floatFrac; translate = $translate })
}

# March-in-place (inverse of anim-truth). Too few rest samples -> SKIP.
function Test-MarchInPlace {
    param([string]$File, [string]$Label = "join", [double]$MaxMarchFrac = 0.20)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "march" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO MARCH restSamples=(\d+) march=(\d+) marchFrac=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO MARCH line (skipped)"
        return (Add-GateResult -Name "march" -Status SKIP -Detail "no MARCH line")
    }
    $rest  = [int]$line.Matches[0].Groups[1].Value
    $marchFrac = [double]$line.Matches[0].Groups[3].Value
    if ($rest -lt 30) {
        Write-Host "  [$Label] march-in-place SKIP - only $rest at-rest frame(s)"
        return (Add-GateResult -Name "march" -Status SKIP -Metrics @{ restSamples = $rest } -Detail "too few rest samples")
    }
    $ok = ($marchFrac -le $MaxMarchFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] march-in-place $v - marchFrac=$marchFrac (<= $MaxMarchFrac), restSamples=$rest"
    return (Add-GateResult -Name "march" -Status $v -Metrics @{ marchFrac = $marchFrac; restSamples = $rest })
}

# Snap-rate (2026-07-11 rubber-banding validation): hard teleports per minute of
# steady-state run, from the join's cumulative [interp] counters (snapSq+snapNpc).
# The session-start clock-slew window is excluded (the 2x catch-up legitimately
# outruns the walk-drive): counting starts at the last [interp] sample before the
# first slew=1.0 OFFSET report. When the slew never converges (fast_march holds
# 5x, which pins the slew at its cap) the whole run is scored - the velocity-
# aware snap gate must hold regardless of game speed.
function Test-SnapRate {
    param([string]$File, [string]$Label = "join", [double]$MaxPerMin = 3.0,
          [int]$MinWindowSec = 20, [switch]$SquadOnly,
          [string]$GateName = "snap_rate")
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name $GateName -Status SKIP -Detail "no log")
    }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[interp\] .*snapSq=(\d+) snapNpc=(\d+)"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    if ($lines.Count -lt 2) {
        Write-Host "  [$Label] snap-rate SKIP - no [interp] counter series"
        return (Add-GateResult -Name $GateName -Status SKIP -Detail "no [interp] series")
    }
    $series = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        [pscustomobject]@{
            t   = Convert-StampToMs -Groups $g -OffsetMs 0
            sq  = [long]$g[5].Value
            npc = [long]$g[6].Value
        }
    })
    # Skip the clock catch-up window: baseline at the last sample before the
    # first slew=1.0 report (same exclusion the smoothness oracle applies).
    $startIdx = 0
    $om = Select-String -Path $File -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[time\] OFFSET .*slew=1\.0" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $om) {
        $slewT = Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs 0
        for ($i = 0; $i -lt $series.Count; $i++) {
            if ($series[$i].t -le $slewT) { $startIdx = $i }
        }
    }
    $base = $series[$startIdx]
    $last = $series[$series.Count - 1]
    $winSec = ($last.t - $base.t) / 1000.0
    if ($winSec -lt $MinWindowSec) {
        Write-Host "  [$Label] snap-rate SKIP - scored window ${winSec}s (< ${MinWindowSec}s past the slew)"
        return (Add-GateResult -Name $GateName -Status SKIP `
                    -Metrics @{ windowSec = $winSec } -Detail "window too short")
    }
    $dSq  = $last.sq - $base.sq
    $dNpc = $last.npc - $base.npc
    # SquadOnly (fast_march): at 5x a background NPC resting between stream
    # updates legitimately falls 100+ u behind, and the far-behind teleport is
    # the correct convergence tool - only PLAYER-SQUAD snaps (the visible
    # rubber banding) gate there. At 1x both classes gate.
    $gated = if ($SquadOnly) { $dSq } else { $dSq + $dNpc }
    $rate  = [math]::Round($gated / ($winSec / 60.0), 2)
    $ok = ($rate -le $MaxPerMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $scope = if ($SquadOnly) { "squad" } else { "squad+npc" }
    Write-Host "  [$Label] snap-rate $v - $gated $scope hard snap(s) over $([math]::Round($winSec,0))s = $rate/min (<= $MaxPerMin/min; sq=$dSq npc=$dNpc)"
    return (Add-GateResult -Name $GateName -Status $v `
                -Metrics @{ snapsSq = $dSq; snapsNpc = $dNpc; windowSec = [math]::Round($winSec, 1); ratePerMin = $rate })
}

# Suppress-churn (2026-07-11 pop-in/out fix): a join-side NPC must never cycle
# hidden -> restored -> hidden (the 'Saint'/'Kumo' stream-boundary flicker the
# census-existence veto eliminates). Counts suppress/cull events per hand; any
# hand hidden more than once is churn. Zero suppression activity passes - the
# invariant holds vacuously (ghost culls of join-only spawns stay legitimate,
# each firing once per hand).
function Test-SuppressChurn {
    param([string]$File, [string]$Label = "join", [int]$MaxPerHand = 1)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "suppress_churn" -Status SKIP -Detail "no log")
    }
    $counts = @{}
    foreach ($p in @('\[authority\] suppress NPC hand=(\d+,\d+)',
                     '\[census\] cull NPC hand=(\d+,\d+)')) {
        foreach ($m in @(Select-String -Path $File -Pattern $p -ErrorAction SilentlyContinue)) {
            $h = $m.Matches[0].Groups[1].Value
            if ($counts.ContainsKey($h)) { $counts[$h]++ } else { $counts[$h] = 1 }
        }
    }
    $restores = @(Select-String -Path $File -Pattern '\[(?:authority|census)\] restore NPC hand=' -ErrorAction SilentlyContinue).Count
    $worst = 0; $churned = @()
    foreach ($k in $counts.Keys) {
        if ($counts[$k] -gt $worst) { $worst = $counts[$k] }
        if ($counts[$k] -gt $MaxPerHand) { $churned += $k }
    }
    $ok = ($churned.Count -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($ok) { "" } else { "churned hands: " + ($churned -join " ") }
    Write-Host "  [$Label] suppress-churn $v - $($counts.Count) hand(s) hidden, worst=$worst per hand (<= $MaxPerHand), restores=$restores $detail"
    return (Add-GateResult -Name "suppress_churn" -Status $v `
                -Metrics @{ hands = $counts.Count; worstPerHand = $worst; restores = $restores } -Detail $detail)
}

# spawn_far (2026-07-11 "NPCs spawn on top of the join player" fix): census-range
# proxy minting. The host spawns a runtime squad ~620 u out and walks it toward
# the co-located leaders; the join must mint the proxies while they are still
# FAR away (census-missing scan + reply-side mint gate) instead of at the ~200 u
# stream bubble. Gates:
#   1. BIND coverage: every far hand drew a "[spawn] proxy BOUND" line.
#   2. FAR bind: every bind happened >= MinBindDist from the join's leader
#      anchor (no more materializing on top of the player).
#   3. NO DUPES: at most one BOUND per hand (the mint gate must not double-mint
#      a hand that later streams normally).
#   4. TAKEOVER: at least one proxy's SCENARIO PROXY series reaches within
#      ApproachDist of the anchor - the walking squad entered the stream bubble
#      and the SAME proxy body was driven in (no fresh mint at the boundary).
function Test-SpawnFarBind {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinBindDist = 400.0, [double]$ApproachDist = 350.0)
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "spawn_far" -Status SKIP -Detail "no join log")
    }
    $hostLegs = Get-SpawnHands -File $HostFile
    $far = if ($hostLegs.ContainsKey('far')) { @($hostLegs['far']) } else { @() }
    if ($far.Count -eq 0) {
        Write-Host "  SPAWN-FAR FAIL - host never spawned the far squad"
        return (Add-GateResult -Name "spawn_far" -Status FAIL -Detail "no far spawns")
    }
    $am = Select-String -Path $JoinFile -Pattern 'SCENARIO FARBIND anchor=([-\d\.]+),([-\d\.]+),([-\d\.]+) have=1' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $am) {
        Write-Host "  SPAWN-FAR FAIL - join never logged its leader anchor"
        return (Add-GateResult -Name "spawn_far" -Status FAIL -Detail "no join anchor")
    }
    $ax = [double]$am.Matches[0].Groups[1].Value
    $ay = [double]$am.Matches[0].Groups[2].Value
    $az = [double]$am.Matches[0].Groups[3].Value

    # BOUND lines with the mint position (the host-authoritative spawn point).
    $bounds = @{}
    foreach ($m in @(Select-String -Path $JoinFile -Pattern '\[spawn\] proxy BOUND hand=([\d,]+) .*pos=([-\d\.]+),([-\d\.]+),([-\d\.]+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $h = $g[1].Value
        if (-not $bounds.ContainsKey($h)) { $bounds[$h] = New-Object System.Collections.ArrayList }
        $dx = [double]$g[2].Value - $ax
        $dy = [double]$g[3].Value - $ay
        $dz = [double]$g[4].Value - $az
        [void]$bounds[$h].Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
    }

    $boundN = 0; $dupN = 0; $closestBind = [double]::MaxValue; $farOk = $true
    foreach ($h in $far) {
        if (-not $bounds.ContainsKey($h)) { continue }
        $boundN++
        if ($bounds[$h].Count -gt 1) { $dupN++ }
        $d = $bounds[$h][0]
        if ($d -lt $closestBind) { $closestBind = $d }
        if ($d -lt $MinBindDist) { $farOk = $false }
    }
    $bindOk = ($boundN -eq $far.Count)
    $dupOk  = ($dupN -eq 0)
    $bindTxt = if ($closestBind -eq [double]::MaxValue) { "n/a" } else { [Math]::Round($closestBind, 0) }
    Write-Host ("  SPAWN-FAR bind " + $(if ($bindOk) { "PASS" } else { "FAIL" }) +
                " - $boundN/$($far.Count) far hands minted")
    Write-Host ("  SPAWN-FAR distance " + $(if ($farOk) { "PASS" } else { "FAIL" }) +
                " - closest bind $bindTxt u from join anchor (>= $MinBindDist)")
    Write-Host ("  SPAWN-FAR dupes " + $(if ($dupOk) { "PASS" } else { "FAIL" }) +
                " - $dupN hand(s) bound more than once")

    # TAKEOVER: the proxy body itself must close on the anchor (stream drive).
    $P = Get-ScenarioSeries -File $JoinFile -Kind "PROXY"
    $minApproach = [double]::MaxValue
    foreach ($h in $far) {
        $key = Convert-SpawnHandToSeriesKey -Hand $h
        if ($null -eq $key -or -not $P.ContainsKey($key)) { continue }
        foreach ($ps in $P[$key]) {
            $dx = $ps.p[0]-$ax; $dy = $ps.p[1]-$ay; $dz = $ps.p[2]-$az
            $d = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)
            if ($d -lt $minApproach) { $minApproach = $d }
        }
    }
    $approachOk = ($minApproach -le $ApproachDist)
    $appTxt = if ($minApproach -eq [double]::MaxValue) { "n/a" } else { [Math]::Round($minApproach, 0) }
    Write-Host ("  SPAWN-FAR takeover " + $(if ($approachOk) { "PASS" } else { "FAIL" }) +
                " - closest proxy approach $appTxt u (<= $ApproachDist)")

    $ok = $bindOk -and $farOk -and $dupOk -and $approachOk
    $why = @()
    if (-not $bindOk)     { $why += "far hands never minted" }
    if (-not $farOk)      { $why += "a proxy minted inside $MinBindDist u (on-top materialization)" }
    if (-not $dupOk)      { $why += "duplicate mints" }
    if (-not $approachOk) { $why += "no proxy walked into the stream bubble" }
    $detail = $why -join "; "
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  SPAWN-FAR $v $detail"
    return (Add-GateResult -Name "spawn_far" -Status $v `
                -Metrics @{ far = $far.Count; bound = $boundN; dupes = $dupN
                            closestBind = $bindTxt; minApproach = $appTxt } -Detail $detail)
}

# Rest-flap (2026-07-11 choppiness fix): the join's walk/rest classifier used the
# instantaneous 2-sample velocity, which dips below threshold at every sample
# pair of a walking NPC - each dip parks/halts the body, each recovery restarts
# the walk (the observed stutter, several flips per second). The velPeak
# debounce makes rest entry require ~1-2 s of genuinely still samples, so the
# cumulative restFlip counter on the [interp] line must now grow at genuine-stop
# rate only. The session-start clock-slew window is excluded like Test-SnapRate.
function Test-RestFlap {
    param([string]$File, [string]$Label = "join", [double]$MaxPerMin = 60.0,
          [int]$MinWindowSec = 20)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "rest_flap" -Status SKIP -Detail "no log")
    }
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[interp\] .*restFlip=(\d+)"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    if ($lines.Count -lt 2) {
        Write-Host "  [$Label] rest-flap SKIP - no [interp] restFlip series"
        return (Add-GateResult -Name "rest_flap" -Status SKIP -Detail "no restFlip series")
    }
    $series = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        [pscustomobject]@{
            t = Convert-StampToMs -Groups $g -OffsetMs 0
            n = [long]$g[5].Value
        }
    })
    $startIdx = 0
    $om = Select-String -Path $File -Pattern "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[time\] OFFSET .*slew=1\.0" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $om) {
        $slewT = Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs 0
        for ($i = 0; $i -lt $series.Count; $i++) {
            if ($series[$i].t -le $slewT) { $startIdx = $i }
        }
    }
    $base = $series[$startIdx]
    $last = $series[$series.Count - 1]
    $winSec = ($last.t - $base.t) / 1000.0
    if ($winSec -lt $MinWindowSec) {
        Write-Host "  [$Label] rest-flap SKIP - scored window ${winSec}s (< ${MinWindowSec}s past the slew)"
        return (Add-GateResult -Name "rest_flap" -Status SKIP `
                    -Metrics @{ windowSec = $winSec } -Detail "window too short")
    }
    $flips = $last.n - $base.n
    $rate  = [math]::Round($flips / ($winSec / 60.0), 2)
    $ok = ($rate -le $MaxPerMin)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] rest-flap $v - $flips walk->rest flip(s) over $([math]::Round($winSec,0))s = $rate/min (<= $MaxPerMin/min)"
    return (Add-GateResult -Name "rest_flap" -Status $v `
                -Metrics @{ flips = $flips; windowSec = [math]::Round($winSec, 1); ratePerMin = $rate })
}

# Existence parity (pack-hidden investigation, 2026-07-11). The join emits an
# "[audit] exist ..." line every 5 s classifying every enumerated NPC:
#   drv (streamed/driven) / cen (census-present local copy) / hid (suppressed)
#   / ghost (census-absent, NOT suppressed - the visible-on-join-only class).
# A ghost is legitimate only transiently (the ~1 s suppress debounce), so with
# a FRESH census the fraction of samples showing ghosts must stay low, and no
# ghost population may PERSIST (a wildlife pack the authority never judges
# would show as sustained ghost>0). Samples with fresh=0 are excluded (wide
# culling is deliberately disabled on a stale census).
function Test-ExistenceParity {
    param([string]$File, [string]$Label = "join",
          [double]$MaxGhostFrac = 0.35, [int]$MaxGhostRun = 4,
          [int]$MinSamples = 4)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "existence_parity" -Status SKIP -Detail "no log")
    }
    $pat = "\[audit\] exist near=(\d+) wide=(\d+) drv=(\d+) cen=(\d+) hid=(\d+) ghost=(\d+) supp=(\d+) census=(\d+) fresh=(\d)"
    $lines = @(Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)
    $samples = @(foreach ($m in $lines) {
        $g = $m.Matches[0].Groups
        if ($g[9].Value -eq "1") {
            [pscustomobject]@{ ghost = [int]$g[6].Value; wide = [int]$g[2].Value }
        }
    })
    if ($samples.Count -lt $MinSamples) {
        Write-Host "  [$Label] existence-parity SKIP - $($samples.Count) fresh-census audit sample(s) (< $MinSamples)"
        return (Add-GateResult -Name "existence_parity" -Status SKIP `
                    -Metrics @{ samples = $samples.Count } -Detail "too few fresh audit samples")
    }
    $withGhost = @($samples | Where-Object { $_.ghost -gt 0 }).Count
    $frac = [math]::Round($withGhost / $samples.Count, 3)
    $maxGhost = ($samples | Measure-Object -Property ghost -Maximum).Maximum
    # Longest consecutive run of ghost>0 samples = persistence signal
    # (5 s cadence, so a run of 4 means >= ~15 s of unjudged join-only NPCs).
    $run = 0; $maxRun = 0
    foreach ($s in $samples) {
        if ($s.ghost -gt 0) { $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    $ok = ($frac -le $MaxGhostFrac) -and ($maxRun -le $MaxGhostRun)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] existence-parity $v - ghosts in $withGhost/$($samples.Count) samples (frac=$frac <= $MaxGhostFrac), longest run $maxRun (<= $MaxGhostRun), peak ghost=$maxGhost"
    return (Add-GateResult -Name "existence_parity" -Status $v `
                -Metrics @{ samples = $samples.Count; ghostFrac = $frac
                            maxRun = $maxRun; peakGhost = $maxGhost })
}

# ---- travel_parity oracles ------------------------------------------------------

# Parse timestamped "SCENARIO WNPC hand=.. pos=.. cls=.. name=.." rows into a list
# of @{t; hand(i,s); pos; cls}, times in the HOST clock frame. Rows are grouped
# into dump samples by the caller (a dump emits its rows in one burst).
function Get-WnpcRows {
    param([string]$File)
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return $rows }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WNPC hand=(\d+),(\d+),[\d,]+ pos=([\-\d\.,]+) cls=(\w+) name='([^']*)'"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $p = $g[7].Value.Split(',') | ForEach-Object { [double]$_ }
        [void]$rows.Add(@{ t = $t; hand = "$($g[5].Value),$($g[6].Value)"
                           pos = $p; cls = $g[8].Value; name = $g[9].Value })
    }
    return $rows
}

# Parse timestamped "SCENARIO WORLD n=.. cls=.." dump-summary rows into a list of
# @{t; n; ghost}. One row per 5 s worldstate dump, emitted even when the dump is
# EMPTY (n=0) - the hop corridor is mostly wilderness, so empty dumps are the
# common case and still count as judged parity samples (0 ghosts vs 0 host rows).
# The join row carries the class counts; the host row is just n= (cls=host).
function Get-WorldRows {
    param([string]$File)
    $rows = New-Object System.Collections.ArrayList
    if (-not (Test-Path $File)) { return $rows }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO WORLD n=(\d+) cls=(\w+)(?: drv=\d+ cen=\d+ hid=\d+ ghost=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $ghost = 0
        if ($g[7].Success) { $ghost = [int]$g[7].Value }
        [void]$rows.Add(@{ t = $t; n = [int]$g[5].Value; cls = $g[6].Value; ghost = $ghost })
    }
    return $rows
}

# Group WNPC rows into dump samples: rows closer than $GapMs to the previous row
# belong to the same 5 s worldstate dump.
function Group-WnpcSamples {
    param($Rows, [int]$GapMs = 1500)
    $samples = New-Object System.Collections.ArrayList
    $cur = $null; $lastT = -1
    foreach ($r in ($Rows | Sort-Object { $_.t })) {
        if ($null -eq $cur -or ($r.t - $lastT) -gt $GapMs) {
            $cur = @{ t = $r.t; rows = (New-Object System.Collections.ArrayList) }
            [void]$samples.Add($cur)
        }
        [void]$cur.rows.Add($r); $lastT = $r.t
    }
    return $samples
}

# travel_parity gate 1: the pair actually traveled. The JOIN's own MEMBER series
# must cover >= $MinTravel from its first sample (the hop trek is ~60,000 u),
# and the host's SCENARIO FOLLOW series (self/peer/gap each ~1 s) must show the
# follow HOLDING: median gap <= $MaxMedianGap after the grace window, and every
# hop-opened gap (a teleport leg legitimately opens ~4000 u for a sample or
# two) must CLOSE within $MaxLagRun consecutive samples above $LagGap - the
# host's teleport catch-up has to actually land. If the follow never holds,
# the parity numbers describe two separated worlds and the run is meaningless -
# this gates before travel_parity for that reason.
function Test-FollowTravel {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MinTravel = 40000.0, [double]$MaxMedianGap = 120.0,
          [double]$LagGap = 300.0, [int]$MaxLagRun = 6, [int]$GraceMs = 20000)
    # Join travel: the mover is the join's MEMBER hand with the most samples.
    $mem = Get-ScenarioSeries -File $JoinFile -Kind "MEMBER"
    $best = $null
    foreach ($h in $mem.Keys) {
        if ($null -eq $best -or $mem[$h].Count -gt $mem[$best].Count) { $best = $h }
    }
    if ($null -eq $best -or $mem[$best].Count -lt 5) {
        Write-Host "  follow-travel SKIP - no join MEMBER series"
        return (Add-GateResult -Name "follow_travel" -Status SKIP -Detail "no join MEMBER series")
    }
    $s0 = $mem[$best][0]
    $travel = 0.0
    foreach ($s in $mem[$best]) {
        $dx = $s.p[0] - $s0.p[0]; $dz = $s.p[2] - $s0.p[2]
        $d = [math]::Sqrt($dx * $dx + $dz * $dz)
        if ($d -gt $travel) { $travel = $d }
    }
    $travel = [math]::Round($travel, 1)
    # Host follow quality from the FOLLOW series.
    $gaps = New-Object System.Collections.ArrayList
    $t0 = $null
    if (Test-Path $HostFile) {
        $off = Get-LogClockOffsetMs -File $HostFile
        $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO FOLLOW self=[\-\d\.,]+ peer=[\-\d\.,]+ gap=([\d\.]+)"
        foreach ($m in (Select-String -Path $HostFile -Pattern $pat -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            $t = Convert-StampToMs -Groups $g -OffsetMs $off
            if ($null -eq $t0) { $t0 = $t }
            if (($t - $t0) -lt $GraceMs) { continue } # follow spin-up
            [void]$gaps.Add([double]$g[5].Value)
        }
    }
    if ($gaps.Count -lt 5) {
        Write-Host "  follow-travel SKIP - $($gaps.Count) host FOLLOW sample(s) after grace"
        return (Add-GateResult -Name "follow_travel" -Status SKIP `
                    -Metrics @{ travel = $travel; followSamples = $gaps.Count } `
                    -Detail "too few FOLLOW samples")
    }
    $sorted = @($gaps | Sort-Object)
    $median = [math]::Round($sorted[[int]($sorted.Count / 2)], 1)
    # Lag runs: consecutive samples above $LagGap. A hop legitimately opens a
    # ~4000 u gap; the host's teleport catch-up must close it within
    # $MaxLagRun samples (~seconds) or the follow is not actually holding.
    $maxRun = 0; $run = 0
    foreach ($g in $gaps) {
        if ($g -gt $LagGap) { $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    $ok = ($travel -ge $MinTravel) -and ($median -le $MaxMedianGap) -and ($maxRun -le $MaxLagRun)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  follow-travel $v - join traveled $travel u (>= $MinTravel), host follow median gap $median u (<= $MaxMedianGap), max lag run $maxRun (<= $MaxLagRun samples > $LagGap u), $($gaps.Count) samples"
    return (Add-GateResult -Name "follow_travel" -Status $v `
                -Metrics @{ travel = $travel; medianGap = $median
                            maxLagRun = $maxRun; followSamples = $gaps.Count })
}

# travel_parity gate 2: worldstate cross-comparison while traveling. Every 5 s
# both sides dumped SCENARIO WNPC rows (host cls=host from the census walk, join
# rows carrying the authority class drv/cen/hid/ghost). Per join dump sample:
#   ghost rows   - cls=ghost (visible, census-absent, unsuppressed: the
#                  yellow-name/visible-on-join-only class). Gate on the fraction
#                  of samples containing any + the longest consecutive run,
#                  mirroring Test-ExistenceParity but measured WHILE ROAMING.
#   laggard      - ghost whose hand IS in the host's dump (+-$WinMs): coverage
#                  lag, the join authority just hasn't judged it yet (metric).
#   diverged     - cen row whose host counterpart sits > $MaxDiverge away
#                  (parking should bound census-present divergence; metric).
#   hostOnly     - host rows within $HostOnlyRange of the join leader with no
#                  join row for the hand (invisible-to-join candidates; the
#                  range limit keeps out-of-zone census rows - the host census
#                  reaches 2500 u, far past what the join has loaded - from
#                  drowning the metric; minting legitimately lags).
function Test-TravelParity {
    param([string]$HostFile, [string]$JoinFile,
          [double]$MaxGhostFrac = 0.35, [int]$MaxGhostRun = 4,
          [double]$MaxDiverge = 250.0, [double]$HostOnlyRange = 400.0,
          [int]$WinMs = 6000, [int]$MinSamples = 6)
    # Samples are anchored on the WORLD summary rows, NOT the WNPC row groups:
    # a WORLD row is emitted even for an EMPTY dump (n=0), and the hop corridor
    # is mostly wilderness - an empty join dump aligned with an empty host dump
    # is a perfectly judged parity sample (nothing there on either side).
    $joinWorld = Get-WorldRows -File $JoinFile | Where-Object { $_.cls -ne "host" }
    $hostWorld = Get-WorldRows -File $HostFile | Where-Object { $_.cls -eq "host" }
    $joinRows  = Get-WnpcRows -File $JoinFile
    $hostRows  = Get-WnpcRows -File $HostFile
    # Join leader position over time (for the hostOnly range limit): the join's
    # MEMBER hand with the most samples is the traveling mover.
    $mem = Get-ScenarioSeries -File $JoinFile -Kind "MEMBER"
    $mover = $null
    foreach ($h in $mem.Keys) {
        if ($null -eq $mover -or $mem[$h].Count -gt $mem[$mover].Count) { $mover = $h }
    }
    if ($joinWorld.Count -lt $MinSamples -or $hostWorld.Count -eq 0) {
        Write-Host "  travel-parity SKIP - $($joinWorld.Count) join dump(s), $($hostWorld.Count) host dump(s)"
        return (Add-GateResult -Name "travel_parity" -Status SKIP `
                    -Metrics @{ joinSamples = $joinWorld.Count; hostSamples = $hostWorld.Count } `
                    -Detail "too few worldstate dumps")
    }
    $ghostSamples = 0; $run = 0; $maxRun = 0; $used = 0
    $laggards = 0; $diverged = 0; $hostOnly = 0; $trueGhosts = 0; $peakGhost = 0
    foreach ($ws in $joinWorld) {
        $hostAligned = @($hostWorld | Where-Object { [math]::Abs($_.t - $ws.t) -le $WinMs })
        if ($hostAligned.Count -eq 0) { continue } # no host dump nearby - can't judge
        $used++
        # Detail rows for this sample: WNPC rows within the dump's burst window
        # on the join side, within $WinMs of the sample on the host side.
        $jrows = @($joinRows | Where-Object { [math]::Abs($_.t - $ws.t) -le 1500 })
        $hwin  = @($hostRows | Where-Object { [math]::Abs($_.t - $ws.t) -le $WinMs })
        $hByHand = @{}
        foreach ($hr in $hwin) { $hByHand[$hr.hand] = $hr }
        $ghosts = 0
        $joinHands = @{}
        foreach ($jr in $jrows) {
            $joinHands[$jr.hand] = $true
            if ($jr.cls -eq "ghost") {
                $ghosts++
                if ($hByHand.ContainsKey($jr.hand)) { $laggards++ } else { $trueGhosts++ }
            } elseif ($jr.cls -eq "cen" -and $hByHand.ContainsKey($jr.hand)) {
                $hp = $hByHand[$jr.hand].pos
                $dx = $jr.pos[0] - $hp[0]; $dz = $jr.pos[2] - $hp[2]
                if ([math]::Sqrt($dx * $dx + $dz * $dz) -gt $MaxDiverge) { $diverged++ }
            }
        }
        # Range-limit hostOnly to the join leader's surroundings at this time.
        $jl = $null
        if ($null -ne $mover) {
            foreach ($s in $mem[$mover]) {
                if ($null -eq $jl -or [math]::Abs($s.t - $ws.t) -lt [math]::Abs($jl.t - $ws.t)) { $jl = $s }
            }
        }
        foreach ($hr in $hwin) {
            if ($joinHands.ContainsKey($hr.hand)) { continue }
            if ($null -ne $jl) {
                $dx = $hr.pos[0] - $jl.p[0]; $dz = $hr.pos[2] - $jl.p[2]
                if ([math]::Sqrt($dx * $dx + $dz * $dz) -gt $HostOnlyRange) { continue }
            }
            $hostOnly++
        }
        if ($ghosts -gt $peakGhost) { $peakGhost = $ghosts }
        if ($ghosts -gt 0) { $ghostSamples++; $run++; if ($run -gt $maxRun) { $maxRun = $run } }
        else { $run = 0 }
    }
    if ($used -lt $MinSamples) {
        Write-Host "  travel-parity SKIP - only $used join dump(s) had a host dump within $($WinMs)ms"
        return (Add-GateResult -Name "travel_parity" -Status SKIP `
                    -Metrics @{ judged = $used } -Detail "too few aligned dumps")
    }
    $frac = [math]::Round($ghostSamples / $used, 3)
    $ok = ($frac -le $MaxGhostFrac) -and ($maxRun -le $MaxGhostRun)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  travel-parity $v - ghosts in $ghostSamples/$used samples (frac=$frac <= $MaxGhostFrac), longest run $maxRun (<= $MaxGhostRun), peak=$peakGhost; trueGhost=$trueGhosts laggard=$laggards diverged=$diverged hostOnly=$hostOnly"
    return (Add-GateResult -Name "travel_parity" -Status $v `
                -Metrics @{ judged = $used; ghostFrac = $frac; maxRun = $maxRun
                            peakGhost = $peakGhost; trueGhosts = $trueGhosts
                            laggards = $laggards; diverged = $diverged
                            hostOnly = $hostOnly })
}

# Phase 2 anti-zombie gate: census-band NPCs must MOVE on the join when their
# host originals move. For every pair of consecutive HOST worldstate dumps
# (5 s apart), each hand whose host copy advanced >= $HostMoveMin is a judged
# window; the join copy (nearest join dumps in time, cls != hid) must have
# advanced at least $MoveRatio of the host's distance. Before the mid-band
# tier, a bandit outside the ~200 u stream bubble got NO positional stream at
# all - its local copy stood frozen between 120 u census parks (the "zombie
# NPC" field report). SKIPs when the corridor produced too few judged windows
# (empty wilderness runs are common in travel_parity).
function Test-AntiZombie {
    param([string]$HostFile, [string]$JoinFile,
          [double]$HostMoveMin = 10.0, [double]$MoveRatio = 0.2,
          [double]$MaxZombieFrac = 0.30, [int]$MinWindows = 6,
          [int]$WinMs = 6000)
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "anti_zombie" -Status SKIP -Detail "missing log")
    }
    $hostS = @(Group-WnpcSamples -Rows (Get-WnpcRows -File $HostFile))
    $joinS = @(Group-WnpcSamples -Rows (Get-WnpcRows -File $JoinFile))
    if ($hostS.Count -lt 2 -or $joinS.Count -lt 2) {
        Write-Host "  anti-zombie SKIP - $($hostS.Count) host / $($joinS.Count) join dump(s)"
        return (Add-GateResult -Name "anti_zombie" -Status SKIP `
                    -Metrics @{ hostDumps = $hostS.Count; joinDumps = $joinS.Count } `
                    -Detail "too few worldstate dumps")
    }
    # hand -> position per dump, keyed for pairwise lookup.
    function DumpMap($s) {
        $m = @{}
        foreach ($r in $s.rows) { if ($r.cls -ne 'hid') { $m[$r.hand] = $r.pos } }
        return $m
    }
    $judged = 0; $zombies = 0; $zombieHands = @{}
    for ($i = 0; $i + 1 -lt $hostS.Count; $i++) {
        $h0 = $hostS[$i]; $h1 = $hostS[$i + 1]
        if (($h1.t - $h0.t) -gt 3 * $WinMs) { continue } # dump gap; not a window
        $m0 = DumpMap $h0; $m1 = DumpMap $h1
        # Nearest join dumps to each host dump edge.
        $j0 = $joinS | Sort-Object { [Math]::Abs($_.t - $h0.t) } | Select-Object -First 1
        $j1 = $joinS | Sort-Object { [Math]::Abs($_.t - $h1.t) } | Select-Object -First 1
        if ($null -eq $j0 -or $null -eq $j1) { continue }
        if ([Math]::Abs($j0.t - $h0.t) -gt $WinMs -or
            [Math]::Abs($j1.t - $h1.t) -gt $WinMs -or $j0.t -eq $j1.t) { continue }
        $jm0 = DumpMap $j0; $jm1 = DumpMap $j1
        foreach ($hand in $m0.Keys) {
            if (-not $m1.ContainsKey($hand)) { continue }
            $a = $m0[$hand]; $b = $m1[$hand]
            $hostMove = [Math]::Sqrt(($b[0]-$a[0])*($b[0]-$a[0]) +
                                     ($b[1]-$a[1])*($b[1]-$a[1]) +
                                     ($b[2]-$a[2])*($b[2]-$a[2]))
            if ($hostMove -lt $HostMoveMin) { continue }
            if (-not ($jm0.ContainsKey($hand) -and $jm1.ContainsKey($hand))) { continue }
            $ja = $jm0[$hand]; $jb = $jm1[$hand]
            $joinMove = [Math]::Sqrt(($jb[0]-$ja[0])*($jb[0]-$ja[0]) +
                                     ($jb[1]-$ja[1])*($jb[1]-$ja[1]) +
                                     ($jb[2]-$ja[2])*($jb[2]-$ja[2]))
            $judged++
            if ($joinMove -lt $MoveRatio * $hostMove) {
                $zombies++
                $zombieHands[$hand] = $true
            }
        }
    }
    if ($judged -lt $MinWindows) {
        Write-Host "  anti-zombie SKIP - $judged judged window(s) (< $MinWindows)"
        return (Add-GateResult -Name "anti_zombie" -Status SKIP `
                    -Metrics @{ judged = $judged } -Detail "too few moving-NPC windows")
    }
    $frac = [math]::Round($zombies / $judged, 3)
    $ok = ($frac -le $MaxZombieFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  anti-zombie $v - $zombies/$judged moving-NPC windows frozen on the join (frac=$frac <= $MaxZombieFrac; $($zombieHands.Count) distinct hand(s))"
    return (Add-GateResult -Name "anti_zombie" -Status $v `
                -Metrics @{ judged = $judged; zombieFrac = $frac
                            zombieHands = $zombieHands.Count })
}

# Phase 3 unified entity lifecycle: every join-side authority/mint/drive
# decision reports its outcome as a "[life] hand=.. from=.. to=.. reason=.."
# transition; the sweep self-audits hands stuck in DISCOVERED while MINTABLE
# (census position in a locally loaded zone) as "[life] STUCK .. (mintable)".
# Gates:
#   1. STUCK = 0: a mintable census-vouched hand must resolve through the
#      REQ/mint pipeline within its dwell budget - a stuck hand is the
#      invisible-raid failure (the join fights nothing the host sees).
#   2. ILLEGAL = 0: existence-authority contradictions. DISCOVERED means "no
#      local body exists for this hand"; CULLED means "we hold a local body
#      suppressed under it". A direct edge between them in either direction
#      says two subsystems disagree about whether the body exists.
# Everything else (tier handoffs, mint/cull/restore churn) is recorded as
# metrics for trend analysis, not gated here (churn has its own oracle).
function Test-Lifecycle {
    param([string]$JoinFile, [string]$Label = "join")
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "lifecycle" -Status SKIP -Detail "no join log")
    }
    $trans = @(Select-String -Path $JoinFile `
        -Pattern '\[life\] hand=(\d+,\d+,\d+,\d+,\d+) from=(\w+) to=(\w+) reason=(\S+)' `
        -ErrorAction SilentlyContinue)
    $stuck = @(Select-String -Path $JoinFile `
        -Pattern '\[life\] STUCK hand=(\d+,\d+,\d+,\d+,\d+) state=DISCOVERED age=(\d+)s \(mintable\)' `
        -ErrorAction SilentlyContinue)
    if ($trans.Count -eq 0 -and $stuck.Count -eq 0) {
        Write-Host "  [$Label] lifecycle SKIP - no [life] lines (plugin predates Phase 3?)"
        return (Add-GateResult -Name "lifecycle" -Status SKIP -Detail "no lifecycle lines")
    }
    $hands = @{}; $illegal = @(); $perHand = @{}
    foreach ($m in $trans) {
        $h = $m.Matches[0].Groups[1].Value
        $f = $m.Matches[0].Groups[2].Value
        $t = $m.Matches[0].Groups[3].Value
        $hands[$h] = $true
        if ($perHand.ContainsKey($h)) { $perHand[$h]++ } else { $perHand[$h] = 1 }
        if (($f -eq 'DISCOVERED' -and $t -eq 'CULLED') -or
            ($f -eq 'CULLED' -and $t -eq 'DISCOVERED')) {
            $illegal += "$h ($f->$t)"
        }
    }
    $stuckHands = @{}
    foreach ($m in $stuck) { $stuckHands[$m.Matches[0].Groups[1].Value] = $true }
    $worstHand = 0
    foreach ($k in $perHand.Keys) {
        if ($perHand[$k] -gt $worstHand) { $worstHand = $perHand[$k] }
    }
    $ok = ($stuckHands.Count -eq 0 -and $illegal.Count -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = ""
    if ($stuckHands.Count -gt 0) { $detail += "stuck-mintable hands: " + (@($stuckHands.Keys) -join " ") + "; " }
    if ($illegal.Count -gt 0)    { $detail += "illegal transitions: " + ($illegal -join " ") }
    Write-Host "  [$Label] lifecycle $v - $($trans.Count) transition(s) over $($hands.Count) hand(s), worst=$worstHand/hand, stuck-mintable=$($stuckHands.Count), illegal=$($illegal.Count) $detail"
    return (Add-GateResult -Name "lifecycle" -Status $v `
                -Metrics @{ transitions = $trans.Count; hands = $hands.Count
                            worstPerHand = $worstHand
                            stuckMintable = $stuckHands.Count
                            illegal = $illegal.Count } -Detail $detail)
}

# Phase 1 spawn parity: proxy mint-distance distribution. Every join "[spawn]
# proxy BOUND" line carries mintDist (distance from the join squad at mint) and
# cen (census-sourced vs stream-sourced). Spawn parity means host runtime
# spawns appear on the join at the distance the HOST spawned them (usually far
# - wilderness spawn range is 1000-2000 u), not materializing at the old 600 u
# radius gate. Gate: at most $MaxNearFrac of census-sourced mints land below
# $NearDist. Near mints are not individually wrong (the host CAN spawn an
# ambush on top of the players - dialog ambushes do), so the gate is on the
# distribution, not each mint.
function Test-MintDistance {
    param([string]$JoinFile, [double]$NearDist = 300.0,
          [double]$MaxNearFrac = 0.34, [int]$MinMints = 3)
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "mint_dist" -Status SKIP -Detail "no join log")
    }
    $pat = '\[spawn\] proxy BOUND hand=[\d,]+ .*mintDist=([-\d\.]+) cen=(\d)'
    $cen = New-Object System.Collections.ArrayList
    $all = New-Object System.Collections.ArrayList
    foreach ($m in @(Select-String -Path $JoinFile -Pattern $pat -ErrorAction SilentlyContinue)) {
        $d = [double]$m.Matches[0].Groups[1].Value
        if ($d -lt 0) { continue } # no own-squad reference at mint time
        [void]$all.Add($d)
        if ($m.Matches[0].Groups[2].Value -eq "1") { [void]$cen.Add($d) }
    }
    if ($cen.Count -lt $MinMints) {
        Write-Host "  mint-dist SKIP - $($cen.Count) census-sourced mint(s) (< $MinMints; total $($all.Count))"
        return (Add-GateResult -Name "mint_dist" -Status SKIP `
                    -Metrics @{ mints = $all.Count; censusMints = $cen.Count } `
                    -Detail "too few census mints")
    }
    $near = @($cen | Where-Object { $_ -lt $NearDist }).Count
    $frac = [math]::Round($near / $cen.Count, 3)
    $sorted = @($cen | Sort-Object)
    $median = [math]::Round($sorted[[int]($sorted.Count / 2)], 0)
    $minD   = [math]::Round($sorted[0], 0)
    $ok = ($frac -le $MaxNearFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  mint-dist $v - $near/$($cen.Count) census mints below $NearDist u (frac=$frac <= $MaxNearFrac), median $median u, min $minD u"
    return (Add-GateResult -Name "mint_dist" -Status $v `
                -Metrics @{ censusMints = $cen.Count; nearFrac = $frac
                            medianDist = $median; minDist = $minD })
}

# ---- Manifest + top-level analysis ---------------------------------------------------

# Load scripts/scenarios.psd1 (or an explicit path).
function Get-ScenarioManifest {
    param([string]$Path = "")
    if ($Path -eq "") { $Path = Join-Path $PSScriptRoot "scenarios.psd1" }
    if (-not (Test-Path $Path)) { throw "Scenario manifest not found: $Path" }
    return Import-PowerShellDataFile -Path $Path
}

# Run ONE named oracle. Central dispatch keyed by the oracle ids used in the
# manifest's Gating/Advisory lists.
function Invoke-OneOracle {
    param([string]$Id, [string]$HostLog, [string]$JoinLog, [double]$Tolerance, $ExpectedSkewMs)
    switch ($Id) {
        "crosscheck"    { return (Test-Crosscheck      -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "npc_track"     { return (Test-NpcTrack        -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "coop_presence" { return (Test-CoopPresence    -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "pose"          { return (Test-NpcPose         -HostFile $HostLog -JoinFile $JoinLog) }
        "pose_state"    { return (Test-NpcPoseState    -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_pose"      { return (Test-BedPose         -HostFile $HostLog -JoinFile $JoinLog) }
        "bed_put"       { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 1) }
        "cage_put"      { return (Test-FurnPut         -HostFile $HostLog -JoinFile $JoinLog -Kind 2) }
        "cage_peer"     { return (Test-CagePeer        -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_probe"   { return (Test-SneakProbe      -HostFile $HostLog) }
        "spawn_probe"   { return (Test-SpawnProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "shop_probe"    { return (Test-ShopProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "money_sync"    { return (Test-MoneySync       -HostFile $HostLog -JoinFile $JoinLog) }
        "vendor_trade"  { return (Test-VendorTrade     -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_probe" { return (Test-RecruitProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_sync"  { return (Test-RecruitSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "faction_probe" { return (Test-FactionProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "faction_sync"  { return (Test-FactionSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "time_probe"    { return (Test-TimeProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "time_sync"     { return (Test-TimeSync        -HostFile $HostLog -JoinFile $JoinLog) }
        "door_probe"    { return (Test-DoorProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "door_sync"     { return (Test-DoorSync        -HostFile $HostLog -JoinFile $JoinLog) }
        "build_probe"   { return (Test-BuildProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "build_sync"    { return (Test-BuildSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "bdoor_probe"   { return (Test-BdoorProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "bdoor_sync"    { return (Test-BdoorSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "hunger_probe"  { return (Test-HungerProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "hunger_sync"   { return (Test-HungerSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "latejoin_probe" { return (Test-LatejoinProbe  -HostFile $HostLog -JoinFile $JoinLog) }
        "latejoin_sync"  { return (Test-LatejoinSync   -HostFile $HostLog -JoinFile $JoinLog) }
        "save_probe"     { return (Test-SaveProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "load_probe"     { return (Test-LoadProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "save_sync"      { return (Test-SaveSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "save_resume"    { return (Test-SaveResume     -HostFile $HostLog -JoinFile $JoinLog) }
        "load_sync"      { return (Test-LoadSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "prod_probe"     { return (Test-ProdProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "prod_sync"      { return (Test-ProdSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "research_probe" { return (Test-ResearchProbe  -HostFile $HostLog -JoinFile $JoinLog) }
        "research_sync"  { return (Test-ResearchSync   -HostFile $HostLog -JoinFile $JoinLog) }
        "store_probe"    { return (Test-StoreProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "store_sync"     { return (Test-StoreSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "squad_probe"    { return (Test-SquadProbe     -HostFile $HostLog -JoinFile $JoinLog) }
        "squad_sync"     { return (Test-SquadSync      -HostFile $HostLog -JoinFile $JoinLog) }
        "spawn_sync"    { return (Test-SpawnSync       -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "spawn_far"     { return (Test-SpawnFarBind    -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_census"    { return (Test-NpcCensus       -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_pose"    { return (Test-SneakPose       -HostFile $HostLog -JoinFile $JoinLog) }
        "sneak_detect"  { return (Test-SneakDetect     -HostFile $HostLog -JoinFile $JoinLog) }
        "body_state"    { return (Test-NpcBodyState    -HostFile $HostLog -JoinFile $JoinLog) }
        "craft_order"   { return (Test-CraftOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "down_order"    { return (Test-DownOrder       -HostFile $HostLog -JoinFile $JoinLog) }
        "death_order"   { return (Test-DeathOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_probe"  { return (Test-CombatProbe     -HostFile $HostLog) }
        "combat_order"  { return (Test-CombatOrder     -HostFile $HostLog -JoinFile $JoinLog) }
        "combat_kill"   { return (Test-CombatKill      -HostFile $HostLog -JoinFile $JoinLog) }
        "damage_guard"  { return (Test-DamageGuard     -HostFile $HostLog -JoinFile $JoinLog) }
        "player_combat" { return (Test-PlayerCombat    -HostFile $HostLog -JoinFile $JoinLog) }
        "player_ko"     { return (Test-PlayerKo        -HostFile $HostLog -JoinFile $JoinLog) }
        "medic_order"   { return (Test-MedicOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "limb_loss"     { return (Test-LimbLoss        -HostFile $HostLog -JoinFile $JoinLog) }
        "stats_sync"    { return (Test-StatsSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "carry_order"   { return (Test-CarryOrder      -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_carry"     { return (Test-NpcCarry        -HostFile $HostLog -JoinFile $JoinLog) }
        "npc_vitals"    { return (Test-NpcVitals       -HostFile $HostLog -JoinFile $JoinLog) }
        "speed_sync"    { return (Test-SpeedSync       -HostFile $HostLog -JoinFile $JoinLog) }
        "speed_probe"   { return (Test-SpeedProbe      -HostFile $HostLog) }
        "combat_crowd"  { return (Test-CombatCrowd     -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "split_interest" { return (Test-SplitInterest  -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "inv_sync"      { return (Test-InventorySync   -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_bidir"     { return (Test-InventoryBidir  -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_equip"     { return (Test-InventoryEquip  -HostFile $HostLog -JoinFile $JoinLog) }
        "inv_reequip"   { return (Test-InventoryReequip -HostFile $HostLog -JoinFile $JoinLog) }
        "add_equip"     { return (Test-AddEquip        -HostFile $HostLog -JoinFile $JoinLog) }
        "trade_probe"   { return (Test-TradeProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "trade_peer"    { return (Test-TradePeer       -HostFile $HostLog -JoinFile $JoinLog) }
        "drop_probe"    { return (Test-DropProbe       -HostFile $HostLog) }
        "wi_sync"       { return (Test-WorldItemSync   -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "wi_join"       { return (Test-WorldItemSync   -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance -JoinAuthor -GateName "wi_join") }
        "wpn_relocate"  { return (Test-WpnRelocate     -HostFile $HostLog -JoinFile $JoinLog) }
        "weapon_drop"   { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog) }
        "armor_drop"    { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog -GateName "armor_drop") }
        "weapon_loot"   { return (Test-WeaponLoot      -HostFile $HostLog -JoinFile $JoinLog) }
        "smoothness"    { return (Test-Smoothness      -File $JoinLog) }
        "anim_truth"    { return (Test-AnimTruth       -File $JoinLog) }
        "march"         { return (Test-MarchInPlace    -File $JoinLog) }
        "snap_rate"     { return (Test-SnapRate        -File $JoinLog) }
        "snap_rate_squad" { return (Test-SnapRate      -File $JoinLog -SquadOnly -GateName "snap_rate_squad") }
        "suppress_churn" { return (Test-SuppressChurn  -File $JoinLog) }
        "rest_flap"     { return (Test-RestFlap        -File $JoinLog) }
        "existence_parity" { return (Test-ExistenceParity -File $JoinLog) }
        "follow_travel" { return (Test-FollowTravel    -HostFile $HostLog -JoinFile $JoinLog) }
        "travel_parity" { return (Test-TravelParity    -HostFile $HostLog -JoinFile $JoinLog) }
        "mint_dist"     { return (Test-MintDistance    -JoinFile $JoinLog) }
        "anti_zombie"   { return (Test-AntiZombie      -HostFile $HostLog -JoinFile $JoinLog) }
        "lifecycle"     { return (Test-Lifecycle       -JoinFile $JoinLog) }
        "clock_sync"    { return (Test-ClockSync       -HostFile $HostLog -JoinFile $JoinLog -ExpectedSkewMs $ExpectedSkewMs) }
        default {
            Write-Host "  WARNING: unknown oracle id '$Id' (manifest error)"
            return (Add-GateResult -Name $Id -Status FAIL -Detail "unknown oracle id")
        }
    }
}

# Top-level: analyze one run's host/join logs and produce the overall verdict +
# verdict.json. Used by run_test.ps1 (live) and analyze_run.ps1 (collected logs).
#
# Verdict rule:
#   FAIL if any always-on gate (health/result/check_fail) failed,
#   or any GATING oracle failed,
#   or the scenario's PRIMARY gate is SKIP/missing (the no-signal guard).
#   ADVISORY oracle results are recorded but never gate.
function Invoke-RunAnalysis {
    param(
        [Parameter(Mandatory = $true)][string]$HostLog,
        [string]$JoinLog = "",
        [string]$Scenario = "",
        [double]$Tolerance = 3.0,
        [bool]$JoinExpected = $true,
        [string]$ManifestPath = "",
        [hashtable]$RunInfo = @{},
        $ExpectedSkewMs = $null,
        # True when the run went through the WAN relay proxy: applies the
        # scenario's deliberate WAN-regime adjustments (WanTolerance override +
        # WanDemote gating->advisory moves) declared in the manifest.
        [bool]$WanActive = $false,
        [string]$OutJson = ""
    )
    Reset-GateResults
    $manifest = Get-ScenarioManifest -Path $ManifestPath
    $entry = $null
    if ($Scenario -ne "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
        $entry = $manifest.Scenarios[$Scenario]
    }
    if ($WanActive -and $null -ne $entry) {
        if ($entry.ContainsKey("WanTolerance")) {
            Write-Host "  (WAN regime: tolerance $Tolerance -> $($entry.WanTolerance) per manifest)"
            $Tolerance = $entry.WanTolerance
        }
    }

    # 1. Log health (always).
    $cleanPattern = if ($Scenario -ne "") { "SCENARIO RESULT" } else { "test duration elapsed; exiting" }
    [void](Test-LogHealth -File $HostLog -Label "host" -Required $true -CleanPattern $cleanPattern)
    [void](Test-LogHealth -File $JoinLog -Label "join" -Required $JoinExpected -CleanPattern $cleanPattern)

    # 1b. Clock catch-up visibility (always, advisory). The join closes its
    # load skew by simming at up to 2x (protocol 25); any oracle that scores
    # motion while the slew is engaged is measuring the transient, not the
    # steady state (the smoothness oracle now excludes those frames itself -
    # slewSkip=). This FINDING makes the overlap visible on every run; the
    # time_sync scenario is where convergence is actually GATED.
    if ($JoinLog -ne "" -and (Test-Path $JoinLog)) {
        $slew = Get-SlewSummary -File $JoinLog
        if ($null -ne $slew) {
            $conv = if ($slew.converged) { "converged to 1x" } else { "STILL SLEWING at log end (slew=$($slew.lastSlew))" }
            Write-Host "  FINDING: join clock catch-up - peakOff=$($slew.peakOffGh)gh peakSlew=$($slew.peakSlew)x for ~$($slew.slewSecs)s; $conv"
        }
    }

    $gating = @(); $advisory = @(); $primary = ""
    if ($Scenario -ne "") {
        # 2. In-plugin CHECK lines + SCENARIO RESULT (always, for scenarios).
        [void](Test-NoCheckFail -HostFile $HostLog -JoinFile $JoinLog)
        [void](Test-ScenarioResultPass -File $HostLog -Label "host" -Required $true)
        [void](Test-ScenarioResultPass -File $JoinLog -Label "join" -Required $JoinExpected)

        # 3. Scenario oracles per the manifest.
        if ($null -ne $entry) {
            $gating   = @($entry.Gating)
            $advisory = @($entry.Advisory)
            $primary  = $entry.PrimaryGate
            if ($WanActive -and $entry.ContainsKey("WanDemote")) {
                foreach ($d in @($entry.WanDemote)) {
                    if ($gating -contains $d) {
                        Write-Host "  (WAN regime: gate '$d' demoted to advisory per manifest)"
                        $gating   = @($gating | Where-Object { $_ -ne $d })
                        $advisory += $d
                    }
                }
            }
            foreach ($id in ($gating + $advisory)) {
                [void](Invoke-OneOracle -Id $id -HostLog $HostLog -JoinLog $JoinLog `
                          -Tolerance $Tolerance -ExpectedSkewMs $ExpectedSkewMs)
            }
        } else {
            Write-Host "  WARNING: scenario '$Scenario' not in manifest; running generic cross-check"
            $gating = @("crosscheck"); $primary = "crosscheck"
            [void](Test-Crosscheck -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance)
        }
    }

    # 4. Compute the verdict.
    $gates = Get-GateResults
    $byName = @{}
    foreach ($g in $gates) { $byName[$g.gate] = $g }
    $reasons = @()
    $alwaysOn = @("health_host", "health_join", "check_fail", "result_host", "result_join")
    foreach ($n in $alwaysOn) {
        if ($byName.ContainsKey($n) -and $byName[$n].status -eq "FAIL") { $reasons += "$n FAIL" }
    }
    foreach ($n in $gating) {
        if (-not $byName.ContainsKey($n)) { $reasons += "$n missing"; continue }
        if ($byName[$n].status -eq "FAIL") { $reasons += "$n FAIL" }
    }
    if ($primary -ne "") {
        if (-not $byName.ContainsKey($primary)) {
            $reasons += "primary gate $primary missing (no signal)"
        } elseif ($byName[$primary].status -eq "SKIP") {
            $reasons += "primary gate $primary SKIP (no signal)"
        }
    }
    $pass = ($reasons.Count -eq 0)

    # 5. Summary table: every gate with its status (SKIP visible, advisory tagged).
    Write-Host ""
    Write-Host "== Gate summary =="
    foreach ($g in $gates) {
        $tag = ""
        if ($advisory -contains $g.gate) { $tag = " (advisory)" }
        elseif ($g.gate -eq $primary)    { $tag = " (primary)" }
        Write-Host ("  {0,-14} {1,-4}{2}{3}" -f $g.gate, $g.status, $tag,
                    $(if ($g.detail -ne "") { " - " + $g.detail } else { "" }))
    }
    if (-not $pass) { Write-Host ("  verdict reasons: " + ($reasons -join "; ")) }

    # 6. verdict.json for trending / offline consumption.
    $verdict = [pscustomobject]@{
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        scenario  = $Scenario
        tolerance = $Tolerance
        pass      = $pass
        reasons   = $reasons
        primary   = $primary
        gating    = $gating
        advisory  = $advisory
        run       = $RunInfo
        gates     = $gates
    }
    if ($OutJson -ne "") {
        $verdict | ConvertTo-Json -Depth 6 | Set-Content -Path $OutJson -Encoding UTF8
        Write-Host "  verdict json: $OutJson"
    }
    return $verdict
}

Export-ModuleMember -Function @(
    "Reset-GateResults", "Add-GateResult", "Get-GateResults", "Merge-Status",
    "Get-LogClockOffsetMs", "Get-ClockSyncStats", "Convert-StampToMs",
    "Get-ScenarioLines", "Get-ScenarioSeries", "Get-MarkerTimeMs",
    "Test-LogHealth", "Test-NoCheckFail", "Test-ScenarioResultPass", "Test-ClockSync",
    "Test-Crosscheck", "Measure-NpcSync", "Test-NpcTrack", "Test-CoopPresence",
    "Test-NpcPose", "Test-NpcPoseState", "Test-NpcBodyState", "Test-BedPose",
    "Test-CraftOrder", "Test-DownOrder", "Test-DeathOrder",
    "Test-CombatProbe", "Test-CombatOrder", "Test-CombatKill", "Test-DamageGuard",
    "Get-VitalsSeries", "Test-PlayerCombat", "Test-PlayerKo", "Test-MedicOrder",
    "Test-LimbLoss", "Test-NpcVitals",
    "Get-StatsSeries", "Test-StatsSync",
    "Get-CarrySeries", "Test-CarryOrder", "Test-NpcCarry",
    "Get-FurnSeries", "Test-FurnPut", "Test-CagePeer",
    "Test-SneakProbe",
    "Get-SpawnHands", "Test-SpawnProbe", "Test-SpawnSync", "Test-SpawnFarBind",
    "Test-NpcCensus",
    "Get-WalletSeries", "Test-ShopProbe", "Test-MoneySync", "Test-VendorTrade",
    "Test-RecruitProbe", "Test-RecruitSync",
    "Get-FacRelSeries", "Test-FactionProbe", "Test-FactionSync",
    "Get-GTimeSeries", "Test-TimeProbe", "Test-TimeSync", "Get-SlewSummary",
    "Get-SneakSeries", "Test-SneakPose", "Test-SneakDetect",
    "Test-SpeedSync",
    "Test-SpeedProbe",
    "Test-CombatCrowd",
    "Test-SplitInterest",
    "Test-InventorySync", "Test-InventoryBidir", "Test-InventoryEquip",
    "Test-InventoryReequip", "Test-AddEquip", "Test-TradeProbe", "Test-TradePeer", "Test-DropProbe",
    "Test-WorldItemSync", "Test-WpnRelocate", "Test-WeaponDrop",
    "Test-Smoothness", "Test-AnimTruth", "Test-MarchInPlace",
    "Test-SnapRate", "Test-SuppressChurn", "Test-RestFlap",
    "Test-ExistenceParity",
    "Get-WnpcRows", "Get-WorldRows", "Group-WnpcSamples", "Test-FollowTravel", "Test-TravelParity",
    "Test-MintDistance", "Test-AntiZombie", "Test-Lifecycle",
    "Get-ScenarioManifest", "Invoke-OneOracle", "Invoke-RunAnalysis"
)
