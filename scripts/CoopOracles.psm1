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
        return (Add-GateResult -Name "wi_sync" -Status FAIL -Metrics @{ host = $H.Count; join = $J.Count } -Detail "insufficient samples")
    }
    $drop    = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO WI DROP .*dropped=[1-9]' -Quiet)
    $despawn = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO WI DESPAWN destroyed=[1-9]' -Quiet)
    $hostPresent = $H | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinPresent = $J | Where-Object { $_.n -ge 1 } | Select-Object -First 1
    $joinSpawned = ($joinPresent -ne $null)
    $hostSaw     = ($hostPresent -ne $null)
    $posMatch = $false; $hashMatch = $false; $dxz = -1.0
    if ($hostSaw -and $joinSpawned) {
        $dx = $hostPresent.x - $joinPresent.x; $dz = $hostPresent.z - $joinPresent.z
        $dxz = [math]::Sqrt($dx * $dx + $dz * $dz)
        $posMatch  = ($dxz -le $Tol)
        $hashMatch = ($hostPresent.hash -eq $joinPresent.hash)
    }
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
    return (Add-GateResult -Name "wi_sync" -Status $v -Metrics @{
        drop = $drop; despawn = $despawn; joinSpawned = $joinSpawned
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

# ---- Locomotion-quality oracles ----------------------------------------------------

# Smoothness (zero-advance fraction while the source moved). No SMOOTH line or a
# scenario that never drove a moving body -> SKIP.
function Test-Smoothness {
    param([string]$File, [string]$Label = "join", [double]$MaxZeroFrac = 0.40)
    if (-not (Test-Path $File)) {
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no log")
    }
    $line = Select-String -Path $File -Pattern "SCENARIO SMOOTH .*zeroFrac=([\d\.]+).*maxStep=([\d\.]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $line) {
        Write-Host "  [$Label] no SCENARIO SMOOTH line (skipped)"
        return (Add-GateResult -Name "smoothness" -Status SKIP -Detail "no SMOOTH line")
    }
    $zeroFrac = [double]$line.Matches[0].Groups[1].Value
    $maxStep  = [double]$line.Matches[0].Groups[2].Value
    $ok = ($zeroFrac -le $MaxZeroFrac)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  [$Label] smoothness $v - zeroFrac=$zeroFrac (<= $MaxZeroFrac), maxStep=$maxStep"
    return (Add-GateResult -Name "smoothness" -Status $v -Metrics @{ zeroFrac = $zeroFrac; maxStep = $maxStep })
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
        "sneak_probe"   { return (Test-SneakProbe      -HostFile $HostLog) }
        "spawn_probe"   { return (Test-SpawnProbe      -HostFile $HostLog -JoinFile $JoinLog) }
        "shop_probe"    { return (Test-ShopProbe       -HostFile $HostLog -JoinFile $JoinLog) }
        "money_sync"    { return (Test-MoneySync       -HostFile $HostLog -JoinFile $JoinLog) }
        "vendor_trade"  { return (Test-VendorTrade     -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_probe" { return (Test-RecruitProbe    -HostFile $HostLog -JoinFile $JoinLog) }
        "recruit_sync"  { return (Test-RecruitSync     -HostFile $HostLog -JoinFile $JoinLog) }
        "spawn_sync"    { return (Test-SpawnSync       -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
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
        "drop_probe"    { return (Test-DropProbe       -HostFile $HostLog) }
        "wi_sync"       { return (Test-WorldItemSync   -HostFile $HostLog -JoinFile $JoinLog -Tol $Tolerance) }
        "wpn_relocate"  { return (Test-WpnRelocate     -HostFile $HostLog -JoinFile $JoinLog) }
        "weapon_drop"   { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog) }
        "armor_drop"    { return (Test-WeaponDrop      -HostFile $HostLog -JoinFile $JoinLog -GateName "armor_drop") }
        "smoothness"    { return (Test-Smoothness      -File $JoinLog) }
        "anim_truth"    { return (Test-AnimTruth       -File $JoinLog) }
        "march"         { return (Test-MarchInPlace    -File $JoinLog) }
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
    "Get-FurnSeries", "Test-FurnPut",
    "Test-SneakProbe",
    "Get-SpawnHands", "Test-SpawnProbe", "Test-SpawnSync",
    "Get-WalletSeries", "Test-ShopProbe", "Test-MoneySync", "Test-VendorTrade",
    "Test-RecruitProbe", "Test-RecruitSync",
    "Get-SneakSeries", "Test-SneakPose", "Test-SneakDetect",
    "Test-SpeedSync",
    "Test-SpeedProbe",
    "Test-CombatCrowd",
    "Test-SplitInterest",
    "Test-InventorySync", "Test-InventoryBidir", "Test-InventoryEquip",
    "Test-InventoryReequip", "Test-AddEquip", "Test-DropProbe",
    "Test-WorldItemSync", "Test-WpnRelocate", "Test-WeaponDrop",
    "Test-Smoothness", "Test-AnimTruth", "Test-MarchInPlace",
    "Get-ScenarioManifest", "Invoke-OneOracle", "Invoke-RunAnalysis"
)
