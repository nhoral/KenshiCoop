# oracles/Npc.ps1 - NPC streaming / body-state / char-state oracles (monolith
# split of CoopOracles.psm1, 2026-07-12): MEMBER-vs-RECV cross-checks
# (Test-Crosscheck, Measure-NpcSync, Test-NpcTrack, Test-CoopPresence), pose +
# body-state (Test-NpcPose*, Test-NpcBodyState, Test-BedPose), live
# transitions (Test-CraftOrder/DownOrder/DeathOrder), carry + furniture +
# sneak + speed (Get-CarrySeries..Test-NpcCarry, Test-Sneak*, Test-Speed*),
# split_interest, runtime-spawn probes (Get-SpawnHands..Test-SpawnProbe,
# Test-SpawnSync, Test-NpcCensus). Dot-sourced by CoopOracles.psm1.
# Must NOT: change gate names or SCENARIO regexes (resources/CODE_MAP.md).
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

# bed_wake (conscious bed EXIT / wake-and-move). bed_pose only proved ENTER +
# HOLD; this proves the transition OUT. Anchors on the host's "SCENARIO BEDWAKE
# ORDER/MOVE" markers (the ORDER carries the subject hand) and gates:
#   1. host entered:  host MEMBER pre-move series holds BODY_IN_BED for >= MinBed
#      samples (the sleeper actually got into the bed and streamed it),
#   2. host moved:    after the MOVE marker the host MEMBER series travels >
#      MoveDist from the bed position (the host really left + walked),
#   3. join left bed: the join RECV tail (post-move + settle) is OUT of bed for
#      >= LeaveRatio of samples - the fix's whole point (no stuck sleeping); a
#      "[furn] BED FAST-EXIT" on the join is reported as corroboration,
#   4. join followed: median time-aligned host<->join gap over those out-of-bed
#      join tail samples <= PosTol (the copy tracked the host, not frozen).
# BODY_IN_BED is 1<<5 = 32 (netproto/Wire.h).
function Test-BedWake {
    param([string]$HostFile, [string]$JoinFile,
          [double]$PosTol = 6.0, [double]$MoveDist = 8.0,
          [int]$MinBed = 6, [int]$MinTail = 4, [double]$LeaveRatio = 0.7,
          [int]$SettleMs = 4000, [int]$MaxDt = 800)
    $BED = 32
    $mo = Select-String -Path $HostFile -Pattern "SCENARIO BEDWAKE ORDER issued hand=(\d+),(\d+) task=(\d+) ok=1" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mo) {
        Write-Host "  BED-WAKE FAIL - host never issued the bed order (ok=1 marker missing)"
        return (Add-GateResult -Name "bed_wake" -Status FAIL -Detail "no BEDWAKE ORDER ok=1 marker")
    }
    $mm = Select-String -Path $HostFile -Pattern "SCENARIO BEDWAKE MOVE issued" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mm) {
        Write-Host "  BED-WAKE FAIL - host never issued the wake-move marker"
        return (Add-GateResult -Name "bed_wake" -Status FAIL -Detail "no BEDWAKE MOVE marker")
    }
    $g = $mo.Matches[0].Groups
    $keyPrefix = "$($g[1].Value),$($g[2].Value),"
    $tMove = Get-MarkerTimeMs -File $HostFile -Pattern "SCENARIO BEDWAKE MOVE issued"

    $H = Get-ScenarioSeries -File $HostFile -Kind "MEMBER"
    $J = Get-ScenarioSeries -File $JoinFile -Kind "RECV"
    $hKey = $H.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
    $jKey = $J.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
    if ($null -eq $hKey -or $null -eq $jKey) {
        Write-Host "  BED-WAKE FAIL - subject series missing (host=$([bool]$hKey) join=$([bool]$jKey))"
        return (Add-GateResult -Name "bed_wake" -Status FAIL -Detail "subject series missing")
    }

    # Gate 1 - host entered the bed (pre-move BODY_IN_BED samples).
    $hPre    = @($H[$hKey] | Where-Object { $_.t -lt $tMove })
    $hPreBed = @($hPre | Where-Object { ($_.bs -band $BED) -ne 0 })
    $hostEntered = ($hPreBed.Count -ge $MinBed)

    # Bed position (median of the host in-bed samples) to measure the walk away.
    $bedPos = $null
    if ($hPreBed.Count -gt 0) {
        $xs = @($hPreBed | ForEach-Object { $_.p[0] } | Sort-Object)
        $ys = @($hPreBed | ForEach-Object { $_.p[1] } | Sort-Object)
        $zs = @($hPreBed | ForEach-Object { $_.p[2] } | Sort-Object)
        $mi = [int][Math]::Floor($xs.Count / 2)
        $bedPos = @($xs[$mi], $ys[$mi], $zs[$mi])
    }

    # Gate 2 - host walked clear of the bed after the move.
    $hPost = @($H[$hKey] | Where-Object { $_.t -ge $tMove })
    $hostMaxAway = -1.0
    if ($null -ne $bedPos) {
        foreach ($hs in $hPost) {
            $dx = $hs.p[0]-$bedPos[0]; $dy = $hs.p[1]-$bedPos[1]; $dz = $hs.p[2]-$bedPos[2]
            $dd = [Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz)
            if ($dd -gt $hostMaxAway) { $hostMaxAway = $dd }
        }
    }
    $hostMoved = ($hostMaxAway -gt $MoveDist)

    # Gate 3 - the join copy LEFT the bed on the settled tail.
    $jTail  = @($J[$jKey] | Where-Object { $_.t -ge ($tMove + $SettleMs) })
    $jOut   = @($jTail | Where-Object { ($_.bs -band $BED) -eq 0 })
    $leaveRatioVal = if ($jTail.Count -gt 0) { [Math]::Round($jOut.Count / $jTail.Count, 3) } else { 0.0 }
    $joinLeft = ($jTail.Count -ge $MinTail -and $leaveRatioVal -ge $LeaveRatio)
    $fastExit = @(Select-String -Path $JoinFile -Pattern "\[furn\] BED FAST-EXIT" -ErrorAction SilentlyContinue).Count

    # Gate 4 - the join followed the host after waking (co-located over the tail
    # out-of-bed samples, time-aligned to the host MEMBER series).
    $gaps = New-Object System.Collections.ArrayList
    foreach ($js in $jOut) {
        $best = [double]::MaxValue; $bh = $null
        foreach ($hs in $hPost) {
            $dt = [Math]::Abs($hs.t - $js.t)
            if ($dt -lt $best) { $best = $dt; $bh = $hs }
        }
        if ($best -le $MaxDt -and $null -ne $bh) {
            $dx = $js.p[0]-$bh.p[0]; $dy = $js.p[1]-$bh.p[1]; $dz = $js.p[2]-$bh.p[2]
            [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
        }
    }
    $medGap = -1.0
    if ($gaps.Count -gt 0) {
        $sorted = @($gaps | Sort-Object)
        $medGap = [Math]::Round($sorted[[int][Math]::Floor($sorted.Count / 2)], 2)
    }
    $joinFollowed = ($gaps.Count -gt 0 -and $medGap -le $PosTol)

    $ok = ($hostEntered -and $hostMoved -and $joinLeft -and $joinFollowed)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $why = @()
    if (-not $hostEntered) { $why += "host in-bed samples $($hPreBed.Count) < $MinBed (never slept)" }
    if (-not $hostMoved)   { $why += "host max-away $([Math]::Round($hostMaxAway,2)) <= $MoveDist (never walked off the bed)" }
    if (-not $joinLeft)    { $why += "join out-of-bed ratio $leaveRatioVal < $LeaveRatio over $($jTail.Count) tail samples (STUCK SLEEPING)" }
    if (-not $joinFollowed){ $why += "join follow medGap $medGap > $PosTol (or no aligned pairs)" }
    $detail = $why -join "; "
    Write-Host "  BED-WAKE $v - hostBed=$($hPreBed.Count) hostAway=$([Math]::Round($hostMaxAway,2)) joinLeaveRatio=$leaveRatioVal($($jOut.Count)/$($jTail.Count)) fastExit=$fastExit medGap=$medGap $detail"
    return (Add-GateResult -Name "bed_wake" -Status $v `
                -Metrics @{ hostBed = $hPreBed.Count; hostAway = [Math]::Round($hostMaxAway,2);
                            joinLeaveRatio = $leaveRatioVal; joinTail = $jTail.Count;
                            fastExit = $fastExit; medGap = $medGap } -Detail $detail)
}

# bed_lay (UNCONSCIOUS place-in-bed LAYING POSE + wake-and-exit). bed_pose proved
# a conscious SLEEP ORDER lays down; bed_put proved unconscious OCCUPANCY crosses.
# This proves the "carry a KO'd squadmate to a bed" case renders the LAYING pose on
# BOTH clients AND that the body can get back OUT when it wakes. (A CONSCIOUS
# placement was ruled out first: Kenshi itself nondeterministically stands a
# conscious placed body on the mattress and the join mirrors that faithfully, so
# it is base-game behavior, not a coop bug - run 2026-07-17.) Two windows over
# opposite ownership:
#   A: host owns M2 (MEMBER host log; join drives -> RECV join log)
#   B: join owns L1 (MEMBER join log; host drives -> RECV host log)
# Each window drives KO -> drop in bed -> revive -> take out + move. Per window:
#   LAY   1. owner IN_BED >= MinBed samples in [put,wake) with a LOW (laying) pelvis,
#         2. peer  IN_BED >= MinBed samples in [put,wake) with a LOW pelvis (mirrored),
#         3. co-located while laying (median gap <= PosTol),
#   EXIT  4. owner OUT of bed for >= LeaveRatio of the settled post-wake tail,
#         5. peer  OUT of bed for >= LeaveRatio of that tail (it can get up + leave),
#         6. co-located over the peer's out-of-bed tail (follows the host).
# "Laying" = in-bed median pelvis dropped >= LayDrop below the pre-KO STANDING
# baseline (or under the absolute LayMax). BODY_IN_BED = 1<<5 = 32; BODY_DOWN|
# RAGDOLL|DEAD = 7 (netproto/Wire.h). Absolute pelvis medians are always printed.
function Test-BedLay {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinBed = 5, [double]$LayDrop = 3.0, [double]$LayMax = 6.0,
          [double]$PosTol = 5.0, [double]$LeaveRatio = 0.6, [int]$MinExit = 4,
          [int]$SettleMs = 4000, [int]$MaxDt = 800)
    $BED = 32

    function _Med($arr) {
        $a = @($arr | Sort-Object)
        if ($a.Count -eq 0) { return -1.0 }
        return [Math]::Round([double]$a[[int][Math]::Floor($a.Count / 2)], 2)
    }
    # Median time-aligned distance between a peer sample set and the owner series.
    function _GapMed($peerSamples, $ownerSamples, $maxDt) {
        $gaps = New-Object System.Collections.ArrayList
        foreach ($ps in $peerSamples) {
            $best = [double]::MaxValue; $bo = $null
            foreach ($os in $ownerSamples) {
                $dt = [Math]::Abs($os.t - $ps.t)
                if ($dt -lt $best) { $best = $dt; $bo = $os }
            }
            if ($best -le $maxDt -and $null -ne $bo) {
                $dx = $ps.p[0]-$bo.p[0]; $dy = $ps.p[1]-$bo.p[1]; $dz = $ps.p[2]-$bo.p[2]
                [void]$gaps.Add([Math]::Sqrt($dx*$dx + $dy*$dy + $dz*$dz))
            }
        }
        return @{ med = (_Med $gaps); n = $gaps.Count }
    }

    # Evaluate one KO-place-revive window. $ownerFile carries the OWNER's MEMBER
    # series + the PUT/wake markers; $peerFile carries the driven RECV copy.
    function _Win([string]$tag, [string]$ownerFile, [string]$peerFile) {
        $m = Select-String -Path $ownerFile -Pattern "SCENARIO BEDLAY PUT $tag hand=(\d+),(\d+) kind=1 ok=1" -EA SilentlyContinue | Select-Object -First 1
        if ($null -eq $m) { return @{ ok = $false; why = "win ${tag}: no PUT ok=1 marker"; hasData = $false } }
        $g = $m.Matches[0].Groups
        $keyPrefix = "$($g[1].Value),$($g[2].Value),"
        $tPut  = Get-MarkerTimeMs -File $ownerFile -Pattern "SCENARIO BEDLAY PUT $tag hand="
        $tWake = Get-MarkerTimeMs -File $ownerFile -Pattern "SCENARIO BEDLAY $tag wake hand="
        if ($tWake -le 0) { return @{ ok = $false; why = "win ${tag}: no wake marker"; hasData = $false } }

        $O = Get-ScenarioSeries -File $ownerFile -Kind "MEMBER"
        $P = Get-ScenarioSeries -File $peerFile  -Kind "RECV"
        $oKey = $O.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
        $pKey = $P.Keys | Where-Object { $_.StartsWith($keyPrefix) } | Select-Object -First 1
        if ($null -eq $oKey -or $null -eq $pKey) {
            return @{ ok = $false; why = "win ${tag}: subject series missing (owner=$([bool]$oKey) peer=$([bool]$pKey))"; hasData = $false }
        }
        $oAll = $O[$oKey]; $pAll = $P[$pKey]
        $DOWN = 3   # BODY_DOWN | BODY_RAGDOLL (an unconscious body is collapsed)

        # Pre-KO STANDING baseline (owner, upright, not down, not in bed).
        $stand = @($oAll | Where-Object { $_.t -lt $tPut -and ($_.bs -band $BED) -eq 0 -and ($_.bs -band 7) -eq 0 -and $_.pelvis -gt 0.5 } | ForEach-Object { $_.pelvis })
        $standMed = _Med $stand

        # ---- LAY phase [tPut, tWake) ----
        # An UNCONSCIOUS body cannot stand: "laying in the bed" == IN_BED AND
        # collapsed (DOWN/RAGDOLL). That body-state pair is the reliable signal on
        # BOTH clients; the pelvis is only corroboration and is UNREADABLE on a
        # ragdoll (readPoseState returns a -99 sentinel), so it is never required on
        # the owner. Where the driven copy DOES report a valid pelvis it must be LOW
        # (the visual pose the player sees is laying, not standing on the mattress).
        $oLay = @($oAll | Where-Object { $_.t -ge $tPut -and $_.t -lt $tWake -and ($_.bs -band $BED) -ne 0 -and ($_.bs -band $DOWN) -ne 0 })
        $pLay = @($pAll | Where-Object { $_.t -ge $tPut -and $_.t -lt $tWake -and ($_.bs -band $BED) -ne 0 -and ($_.bs -band $DOWN) -ne 0 })
        $oLayPel = _Med @($oLay | Where-Object { $_.pelvis -gt 0 } | ForEach-Object { $_.pelvis })
        $pLayPel = _Med @($pLay | Where-Object { $_.pelvis -gt 0 } | ForEach-Object { $_.pelvis })
        $layGap = _GapMed $pLay @($oAll | Where-Object { $_.t -ge $tPut -and $_.t -lt $tWake }) $MaxDt

        $LayOf = {
            param($pel)
            if ($pel -lt 0) { return $true }   # unreadable (ragdoll -99) -> DOWN bit carries it
            if ($standMed -gt 0 -and $pel -le ($standMed - $LayDrop)) { return $true }
            return ($pel -le $LayMax)
        }
        $ownerOcc  = ($oLay.Count -ge $MinBed)
        $peerOcc   = ($pLay.Count -ge $MinBed)
        $ownerLay  = (& $LayOf $oLayPel)
        $peerLay   = (& $LayOf $pLayPel)
        $layColoc  = ($layGap.n -gt 0 -and $layGap.med -le $PosTol)

        # ---- EXIT phase [tWake + settle, end] ----
        $oTail = @($oAll | Where-Object { $_.t -ge ($tWake + $SettleMs) })
        $pTail = @($pAll | Where-Object { $_.t -ge ($tWake + $SettleMs) })
        $oOut  = @($oTail | Where-Object { ($_.bs -band $BED) -eq 0 })
        $pOut  = @($pTail | Where-Object { ($_.bs -band $BED) -eq 0 })
        $oOutRatio = if ($oTail.Count -gt 0) { [Math]::Round($oOut.Count / $oTail.Count, 3) } else { 0.0 }
        $pOutRatio = if ($pTail.Count -gt 0) { [Math]::Round($pOut.Count / $pTail.Count, 3) } else { 0.0 }
        $exitGap = _GapMed $pOut @($oAll | Where-Object { $_.t -ge $tWake }) $MaxDt
        $ownerExit = ($oTail.Count -ge $MinExit -and $oOutRatio -ge $LeaveRatio)
        $peerExit  = ($pTail.Count -ge $MinExit -and $pOutRatio -ge $LeaveRatio)
        $exitColoc = ($exitGap.n -gt 0 -and $exitGap.med -le $PosTol)

        $why = @()
        if (-not $ownerOcc) { $why += "win $tag owner collapsed-in-bed samples $($oLay.Count) < $MinBed (KO body never lay in bed)" }
        if (-not $peerOcc)  { $why += "win $tag peer collapsed-in-bed samples $($pLay.Count) < $MinBed (NOT LAYING on driven copy - standing/not-in-bed)" }
        if (-not $ownerLay) { $why += "win $tag owner lay-pelvis $oLayPel not low (stand=$standMed need<=stand-$LayDrop or <=$LayMax)" }
        if (-not $peerLay)  { $why += "win $tag peer lay-pelvis $pLayPel NOT LOW (standing on bed? stand=$standMed need<=stand-$LayDrop or <=$LayMax)" }
        if (-not $layColoc) { $why += "win $tag lay medGap $($layGap.med) > $PosTol (or no pairs)" }
        if (-not $ownerExit){ $why += "win $tag owner stayed in bed (outRatio $oOutRatio < $LeaveRatio over $($oTail.Count))" }
        if (-not $peerExit) { $why += "win $tag peer STUCK IN BED (outRatio $pOutRatio < $LeaveRatio over $($pTail.Count))" }
        if (-not $exitColoc){ $why += "win $tag exit medGap $($exitGap.med) > $PosTol (or no pairs)" }

        return @{
            ok = ($ownerOcc -and $peerOcc -and $ownerLay -and $peerLay -and $layColoc -and `
                  $ownerExit -and $peerExit -and $exitColoc)
            why = ($why -join "; "); hasData = $true
            standMed = $standMed; oLayPel = $oLayPel; pLayPel = $pLayPel
            oLay = $oLay.Count; pLay = $pLay.Count; layGap = $layGap.med
            oOutRatio = $oOutRatio; pOutRatio = $pOutRatio; exitGap = $exitGap.med
        }
    }

    $A = _Win "A" $HostFile $JoinFile   # host owns M2, join drives
    $B = _Win "B" $JoinFile $HostFile   # join owns L1, host drives

    $ok = $A.ok -and $B.ok
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = @($A.why, $B.why | Where-Object { $_ }) -join "; "
    $sa = if ($A.hasData) { "A[lay owner=$($A.oLayPel) peer=$($A.pLayPel) stand=$($A.standMed) occ=$($A.oLay)/$($A.pLay) gap=$($A.layGap) | exit out=$($A.oOutRatio)/$($A.pOutRatio) gap=$($A.exitGap)]" } else { "A[$($A.why)]" }
    $sb = if ($B.hasData) { "B[lay owner=$($B.oLayPel) peer=$($B.pLayPel) stand=$($B.standMed) occ=$($B.oLay)/$($B.pLay) gap=$($B.layGap) | exit out=$($B.oOutRatio)/$($B.pOutRatio) gap=$($B.exitGap)]" } else { "B[$($B.why)]" }
    Write-Host "  BED-LAY $v - $sa $sb"
    if (-not $ok -and $detail) { Write-Host "    -> $detail" }
    return (Add-GateResult -Name "bed_lay" -Status $v `
                -Metrics @{ aPeerLayPel = $A.pLayPel; aPeerOut = $A.pOutRatio;
                            bPeerLayPel = $B.pLayPel; bPeerOut = $B.pOutRatio } `
                -Detail $detail)
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

# mine_pose (2026-07-14 mining-sync fix): a player mining an ore node operates a
# MINE BUILDING (task 87/221 "Operating machine") whose operate spot sits ~8-9 m
# from the resolved building origin. The join must REPRODUCE that work pose
# (applyTaskOrder r=2 = posed at fixture), not PARK it (r=3 = fixture rejected as
# "far") the way the old single 6 m seat gate did - a parked miner shows no mining
# animation on the peer. Scans a JOIN log for the pose-apply results on operate-
# machine tasks and asserts posed dominates. Runs against a MANUAL mining session
# log (the automated harness has no terrain resource nodes); invoke as:
#   . scripts\oracles\Npc.ps1; Test-MinePose -JoinFile <Kenshi-Join>\KenshiCoop_join.log
function Test-MinePose {
    param([string]$JoinFile, [int[]]$WorkTasks = @(87, 221), [double]$MinRatio = 0.70)
    if (-not (Test-Path $JoinFile)) {
        Write-Host "  MINE-POSE FAIL - join log not found: $JoinFile"
        return (Add-GateResult -Name "mine_pose" -Status FAIL -Detail "no join log")
    }
    $rx = [regex]'\[pose\] applyOrder .*\btask=(\d+)\b.*\br=(-?\d+)\b'
    $posed = 0; $parked = 0; $other = 0
    foreach ($m in (Select-String -Path $JoinFile -Pattern $rx -AllMatches).Matches) {
        $task = [int]$m.Groups[1].Value
        $r    = [int]$m.Groups[2].Value
        if ($WorkTasks -notcontains $task) { continue }
        if     ($r -eq 2) { $posed++ }
        elseif ($r -eq 3) { $parked++ }
        else              { $other++ }
    }
    $total = $posed + $parked
    if ($total -lt 1) {
        Write-Host "  MINE-POSE FAIL - no operate-machine pose applies (task in $($WorkTasks -join ',')) in join log; order a unit to mine an ore node first"
        return (Add-GateResult -Name "mine_pose" -Status FAIL -Detail "no operate-machine pose applies")
    }
    $ratio = [Math]::Round($posed / $total, 3)
    $ok = ($posed -ge 1 -and $ratio -ge $MinRatio)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  MINE-POSE [join] $v - operate pose posed(r=2) $posed / parked(r=3) $parked (ratio=$ratio >= $MinRatio), other=$other, tasks=$($WorkTasks -join ',')"
    return (Add-GateResult -Name "mine_pose" -Status $v -Metrics @{ posed = $posed; parked = $parked; ratio = $ratio })
}

# mine_clear (2026-07-15 job-removal fix): removing a job on the host while the
# character stays STATIONARY streams task=NONE continuously; the join must RELEASE
# the held operate pose after TASK_CLEAR_MS (logs "[pose] task-clear ...") instead
# of holding the mine order forever, and must NOT immediately re-pose the same hand
# afterwards. Scans a JOIN log: asserts >=1 task-clear fired and (if a work-task
# operate pose preceded it) that no r=2 operate apply for that hand appears AFTER the
# last task-clear. Runs against a MANUAL session log where you added then removed a
# mining job; invoke as:
#   . scripts\oracles\Npc.ps1; Test-MineClear -JoinFile <Kenshi-Join>\KenshiCoop_join.log
function Test-MineClear {
    param([string]$JoinFile, [int[]]$WorkTasks = @(87, 221))
    if (-not (Test-Path $JoinFile)) {
        Write-Host "  MINE-CLEAR FAIL - join log not found: $JoinFile"
        return (Add-GateResult -Name "mine_clear" -Status FAIL -Detail "no join log")
    }
    # Line ordinal is a monotonic proxy for time (the plugin writes in tick order),
    # so "re-pose after clear" = an r=2 operate apply on a line INDEX past the last
    # task-clear for the same hand. Track per-hand.
    $clearRx = [regex]'\[pose\] task-clear hand=(\d+,\d+)\b'
    $poseRx  = [regex]'\[pose\] applyOrder hand=(\d+,\d+) task=(\d+)\b.*\br=(-?\d+)\b'
    $lastClear = @{}   # hand -> last line index a task-clear fired
    $clears = 0
    $rePosed = 0
    $idx = 0
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path $JoinFile))) {
        $idx++
        $cm = $clearRx.Match($line)
        if ($cm.Success) { $clears++; $lastClear[$cm.Groups[1].Value] = $idx; continue }
        $pm = $poseRx.Match($line)
        if ($pm.Success) {
            $hand = $pm.Groups[1].Value
            $task = [int]$pm.Groups[2].Value
            $r    = [int]$pm.Groups[3].Value
            if ($WorkTasks -contains $task -and $r -eq 2 -and
                $lastClear.ContainsKey($hand) -and $idx -gt $lastClear[$hand]) {
                $rePosed++   # a work pose re-armed on this hand AFTER its clear
            }
        }
    }
    if ($clears -lt 1) {
        Write-Host "  MINE-CLEAR FAIL - no '[pose] task-clear' in join log; on the host, add then REMOVE the mining job (leave the character standing) so the join releases the held pose"
        return (Add-GateResult -Name "mine_clear" -Status FAIL -Detail "no task-clear")
    }
    $ok = ($rePosed -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  MINE-CLEAR [join] $v - task-clear fired $clears, work re-pose after clear $rePosed (want 0)"
    return (Add-GateResult -Name "mine_clear" -Status $v -Metrics @{ clears = $clears; rePosed = $rePosed })
}

# jail_hold (2026-07-15 jail-oscillation fix): a host-owned squad member arrested
# and caged is DRIVEN on the join. Before the fix the join re-seated it into the
# cage every FURN_HEAL_MS while its (un-suspended) local AI broke it out and fought
# the guards - a sustained "[furn] HEAL ENTER ... kind=2" loop for the same occupant
# (the "teleported in and out of jail" bug). The fix AI-suspends a caged driven body
# so, after the first re-seat, it STAYS put and the heal loop stops. Scans a JOIN log:
# groups cage (kind=2) HEAL ENTERs by occupant and asserts no single occupant re-enters
# more than MaxEnters times (no sustained oscillation). Log-judged against a MANUAL jail
# session; invoke as:
#   . scripts\oracles\Npc.ps1; Test-JailHold -JoinFile <Kenshi-Join>\KenshiCoop_join.log
function Test-JailHold {
    param([string]$JoinFile, [int]$MaxEnters = 3)
    if (-not (Test-Path $JoinFile)) {
        Write-Host "  JAIL-HOLD FAIL - join log not found: $JoinFile"
        return (Add-GateResult -Name "jail_hold" -Status FAIL -Detail "no join log")
    }
    # kind=2 = cage/prison. Count HEAL ENTER per occupant; a fixed body enters once
    # (or a couple times as it settles), an oscillating one re-enters indefinitely.
    $rx = [regex]'\[furn\] HEAL ENTER occ=(\d+,\d+) kind=2\b'
    $enters = @{}
    foreach ($m in (Select-String -Path $JoinFile -Pattern $rx -AllMatches).Matches) {
        $occ = $m.Groups[1].Value
        if ($enters.ContainsKey($occ)) { $enters[$occ]++ } else { $enters[$occ] = 1 }
    }
    if ($enters.Count -lt 1) {
        Write-Host "  JAIL-HOLD FAIL - no cage HEAL ENTER (kind=2) in join log; on the host, get a squad member arrested/jailed so the join drives it into the cage"
        return (Add-GateResult -Name "jail_hold" -Status FAIL -Detail "no cage occupancy")
    }
    $worst = ($enters.Values | Measure-Object -Maximum).Maximum
    $worstOcc = ($enters.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    $ok = ($worst -le $MaxEnters)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host "  JAIL-HOLD [join] $v - worst caged occupant re-entered $worst times (occ=$worstOcc, max $MaxEnters), occupants=$($enters.Count)"
    return (Add-GateResult -Name "jail_hold" -Status $v -Metrics @{ worstEnters = $worst; occupants = $enters.Count })
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
          [int]$InKind = 0,
          [int]$MaxLatencyMs = 12000, [double]$PosTol = 3.0)
    # $Kind identifies the scenario/gate + the FURNACT 'kind=' marker; $InKind is
    # the occupancy value the FURN series must read. They differ for pole_put
    # (Kind 4 = the pole gate, but a pole is IN_PRISON so the occupant reads in=2).
    if ($InKind -eq 0) { $InKind = $Kind }
    $gate = if ($Kind -eq 4) { "pole_put" } elseif ($Kind -eq 3) { "chain_put" } elseif ($Kind -eq 2) { "cage_put" } else { "bed_put" }
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
        $aIn = @($Aall | Where-Object { $_.t -ge $Tp -and $_.t -le ($Td + 1500) -and $_.in -eq $InKind })
        if ($aIn.Count -lt 1) { $bad += "${t}: subject never read in=$InKind on the AUTHOR (engine put failed?)"; continue }
        # 1. Enter crossed: the peer's local copy reads the occupancy.
        $Pall = Get-FurnSeries -File $d.peerLog -HandIS $d.subject
        if ($Pall.Count -lt 3) { $bad += "${t}: peer furn series too short ($($Pall.Count))"; continue }
        $pIn = @($Pall | Where-Object { $_.t -ge $Tp -and $_.in -eq $InKind })
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

# chain_put (protocol 41, chained/pole prisoner): identical shape to bed_put/
# cage_put but kind=3 (Character::isChained -> setChainedMode). The scenario
# self-chains each subject (no baked fixture), so this gates that the chained
# STATE crosses: author reads in=3 locally, the peer's driven copy reads in=3
# within MaxLatencyMs, the copies hold together while chained, and both read
# in=0 after release. Confirms the wire/apply path; the real pole visual is
# validated by the manual 'pole' save re-test.
function Test-ChainPut {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$PosTol = 3.0)
    return (Test-FurnPut -HostFile $HostFile -JoinFile $JoinFile -Kind 3 `
                         -MaxLatencyMs $MaxLatencyMs -PosTol $PosTol)
}

# pole_put (protocol 19 kind=4, prisoner POLE): identical shape to cage_put but
# the subject is placed on a baked standing PRISONER POLE instead of a cage box.
# A pole is the SAME containment as a cage (setPrisonMode -> occupant reads
# in=2), so the FURNACT marker is kind=4 (the pole gate) while the FURN
# occupancy is checked as in=2. Gates that the pole placement crosses to the
# peer (enter within budget), the copies hold together on the pole, and both
# read in=0 after release - the controlled, deterministic 'body on a pole' test.
function Test-PolePut {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MaxLatencyMs = 12000, [double]$PosTol = 3.0)
    return (Test-FurnPut -HostFile $HostFile -JoinFile $JoinFile -Kind 4 -InKind 2 `
                         -MaxLatencyMs $MaxLatencyMs -PosTol $PosTol)
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

# Parse "SCENARIO SHACKLE hand=i,s t=ms chained=.. shackleItem=.. lock=.. owner=i,s"
# lines into per-hand series. Returns hashtable hand -> list of
# @{T; Chained; ShackleItem; Lock; Owner}. Used by Test-ShackleProbe (Phase 6 6a).
function Get-ShackleSeries {
    param([string]$File)
    $series = @{}
    $rx = 'SCENARIO SHACKLE hand=(\d+),(\d+) t=(\d+) chained=(\d) shackleItem=(\d) lock=(\d) owner=(\d+),(\d+)'
    foreach ($l in @(Select-String -Path $File -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $l.Matches[0].Groups
        $hand = "$($g[1].Value),$($g[2].Value)"
        if (-not $series.ContainsKey($hand)) { $series[$hand] = New-Object System.Collections.ArrayList }
        [void]$series[$hand].Add([pscustomobject]@{
            T = [long]$g[3].Value; Chained = [int]$g[4].Value
            ShackleItem = [int]$g[5].Value; Lock = [int]$g[6].Value
            Owner = "$($g[7].Value),$($g[8].Value)" })
    }
    return $series
}

# Test-ShackleProbe (Phase 6 6a evidence spike, log-only, BOTH clients): confirm
# each client found the shackled prisoner(s) on the camp save, then compare the
# owner's and peer's steady chained/lock view per hand to surface the reported
# "peer PC unlocks the shackles" desync. This is a CHARACTERIZATION gate: it
# PASSes when both clients produced a usable series with >= 1 shackled sighting;
# the parity metrics (divergentHands) are the deliverable that informs the
# 6b GO/NO-GO. It does not FAIL on divergence - divergence IS the evidence.
function Test-ShackleProbe {
    param([string]$HostFile, [string]$JoinFile)
    $hs = Get-ShackleSeries -File $HostFile
    $js = Get-ShackleSeries -File $JoinFile
    $hostHands = @($hs.Keys)
    $joinHands = @($js.Keys)
    $m = @{
        hostHands   = $hostHands.Count
        joinHands   = $joinHands.Count
        hostSamples = (@($hs.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum
        joinSamples = (@($js.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum
    }
    $bad = @()
    if ($hostHands.Count -lt 1) { $bad += "host saw no shackled body (SCENARIO SHACKLE never emitted)" }
    if ($joinHands.Count -lt 1) { $bad += "join saw no shackled body (SCENARIO SHACKLE never emitted)" }

    # Cross-client parity on shared hands: compare the last steady chained/lock
    # state. A mismatch is the peer-unlock fingerprint (owner still locked,
    # peer's driven copy cleared - or vice versa).
    $shared = @($hostHands | Where-Object { $js.ContainsKey($_) })
    $m.sharedHands = $shared.Count
    $divergeHands = @()
    foreach ($h in $shared) {
        $hLast = $hs[$h] | Select-Object -Last 1
        $jLast = $js[$h] | Select-Object -Last 1
        if ($hLast.Chained -ne $jLast.Chained -or $hLast.Lock -ne $jLast.Lock) {
            $divergeHands += ("{0} host(ch={1},lk={2}) join(ch={3},lk={4})" -f `
                $h, $hLast.Chained, $hLast.Lock, $jLast.Chained, $jLast.Lock)
        }
    }
    $m.divergentHands = $divergeHands.Count

    $ok = ($hostHands.Count -ge 1 -and $joinHands.Count -ge 1)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($bad.Count -gt 0) { $bad -join "; " } else { "" }
    if ($divergeHands.Count -gt 0) {
        Write-Host ("  SHACKLE-PROBE divergence (characterization): " + ($divergeHands -join " | "))
        if ($detail) { $detail += "; " }
        $detail += "divergentHands=$($divergeHands.Count)"
    }
    Write-Host ("  SHACKLE-PROBE $v - hostHands=$($hostHands.Count) joinHands=$($joinHands.Count) " +
                "shared=$($shared.Count) divergent=$($divergeHands.Count) " +
                "hostSamples=$($m.hostSamples) joinSamples=$($m.joinSamples)")
    return (Add-GateResult -Name "shackle_probe" -Status $v -Metrics $m -Detail $detail)
}

# Test-ShackleSync (Phase 6 6b validation, BOTH clients): the STRICT counterpart
# to Test-ShackleProbe. With the protocol-42 locked bit + the non-owner unlock
# guard shipping, a shared prisoner whose owner reports chained/locked while the
# peer's driven copy reports it CLEARED (the "other client's PC unlocked the
# shackle" desync) is now a FAIL. Requires both clients to have observed >= 1
# shackled body; then FAILs on any shared-hand steady-state chained/lock
# divergence. The sharedHands=0 case (the camp hand-identity caveat noted in 6a,
# where prisoners enumerate under different local hands on each client) cannot be
# asserted here and PASSes with a note - the manual gate is authoritative there.
function Test-ShackleSync {
    param([string]$HostFile, [string]$JoinFile)
    $hs = Get-ShackleSeries -File $HostFile
    $js = Get-ShackleSeries -File $JoinFile
    $hostHands = @($hs.Keys)
    $joinHands = @($js.Keys)
    $m = @{
        hostHands   = $hostHands.Count
        joinHands   = $joinHands.Count
        hostSamples = (@($hs.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum
        joinSamples = (@($js.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum
    }
    $bad = @()
    if ($hostHands.Count -lt 1) { $bad += "host saw no shackled body (SCENARIO SHACKLE never emitted)" }
    if ($joinHands.Count -lt 1) { $bad += "join saw no shackled body (SCENARIO SHACKLE never emitted)" }

    # Steady-state parity on shared hands: last (chained,lock) must agree. A
    # mismatch is the peer-unlock fingerprint the 6b guard is meant to prevent.
    $shared = @($hostHands | Where-Object { $js.ContainsKey($_) })
    $m.sharedHands = $shared.Count
    $divergeHands = @()
    foreach ($h in $shared) {
        $hLast = $hs[$h] | Select-Object -Last 1
        $jLast = $js[$h] | Select-Object -Last 1
        if ($hLast.Chained -ne $jLast.Chained -or $hLast.Lock -ne $jLast.Lock) {
            $divergeHands += ("{0} host(ch={1},lk={2}) join(ch={3},lk={4})" -f `
                $h, $hLast.Chained, $hLast.Lock, $jLast.Chained, $jLast.Lock)
        }
    }
    $m.divergentHands = $divergeHands.Count

    # STRICT: observed on both clients AND no shared-hand divergence.
    $observed = ($hostHands.Count -ge 1 -and $joinHands.Count -ge 1)
    $ok = ($observed -and $divergeHands.Count -eq 0)
    $v = if ($ok) { "PASS" } else { "FAIL" }
    $detail = if ($bad.Count -gt 0) { $bad -join "; " } else { "" }
    if ($divergeHands.Count -gt 0) {
        if ($detail) { $detail += "; " }
        $detail += "shackle divergence: " + ($divergeHands -join " | ")
    } elseif ($observed -and $shared.Count -eq 0) {
        if ($detail) { $detail += "; " }
        $detail += "no shared hands (identity caveat) - manual gate authoritative"
    }
    Write-Host ("  SHACKLE-SYNC $v - hostHands=$($hostHands.Count) joinHands=$($joinHands.Count) " +
                "shared=$($shared.Count) divergent=$($divergeHands.Count) " +
                "hostSamples=$($m.hostSamples) joinSamples=$($m.joinSamples)")
    return (Add-GateResult -Name "shackle_sync" -Status $v -Metrics $m -Detail $detail)
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
          [int]$TransitionWinMs = 2000, [int]$MinTransitions = 4,
          [double]$MaxBlankFrac = 0.10)
    $series = {
        param($file)
        $off = Get-LogClockOffsetMs -File $file
        $list = New-Object System.Collections.ArrayList
        # buttons= (Phase 5) is optional so older logs still parse.
        $pat = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO SPEED t=\d+ mult=([\d\.]+) paused=(\d)(?: nbtn=-?\d+ buttons=([01]*))?'
        foreach ($mi in (Select-String -Path $file -Pattern $pat -ErrorAction SilentlyContinue)) {
            $gg = $mi.Matches[0].Groups
            $tt = Convert-StampToMs -Groups $gg -OffsetMs $off
            $mu = [double]$gg[5].Value
            if ([int]$gg[6].Value -eq 1) { $mu = 0.0 }
            $btn = if ($gg[7].Success) { $gg[7].Value } else { $null }
            [void]$list.Add([pscustomobject]@{ t = $tt; mult = $mu; btn = $btn })
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

    # Indicator (speed-button highlight) gate [Phase 5]: the buttons show the
    # VOTE tier and must NEVER blank out (all-zero). The pre-fix corruption left
    # the indicator EMPTY whenever a programmatic / replicated speed change or
    # the combat cap fired (setGameSpeed dehighlights the tier button and only
    # the inline UI click reselects it; spike 2026-07-17: buttons=0000). The
    # blank-guard + continuous reconcile must keep a tier lit at all times.
    # Skipped for older logs without buttons=.
    foreach ($side in @(@{ n = "host"; S = $H }, @{ n = "join"; S = $J })) {
        $withBtn = @($side.S | Where-Object { $null -ne $_.btn -and $_.btn -ne '' })
        if ($withBtn.Count -lt 10) { continue }
        $warm = [double]$withBtn[0].t + 3000  # skip the arming/settle window
        $judged = @($withBtn | Where-Object { [double]$_.t -ge $warm })
        if ($judged.Count -lt 10) { continue }
        $blank = @($judged | Where-Object { $_.btn -notmatch '1' })
        $bf = [Math]::Round($blank.Count / $judged.Count, 3)
        $m[($side.n + "IndicatorBlank")] = $bf
        if ($bf -gt $MaxBlankFrac) {
            $bad += "$($side.n) speed indicator blank for $bf of samples (need <= $MaxBlankFrac - indicator wiped by a programmatic/cap speed change)"
        }
    }

    if ($bad.Count -gt 0) {
        Write-Host ("  SPEED-SYNC FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "speed_sync" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  SPEED-SYNC PASS - $($sets.Count) transitions (max follow $($m.maxFollowMs)ms, lone raise denied), " +
                "match $frac over $consider samples, combat window 1x host=$($m.hostCombat1x) join=$($m.joinCombat1x), " +
                "indicator blank host=$($m.hostIndicatorBlank) join=$($m.joinIndicatorBlank)")
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

