# oracles/Combat.ps1 - combat oracles (monolith split of CoopOracles.psm1,
# 2026-07-12): Test-CombatProbe/CombatOrder/CombatKill, Test-DamageGuard,
# Test-PlayerCombat, Test-AssaultTown, Test-PlayerKo, Test-CombatCrowd.
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the "[combat]"/PCOMBAT/ASSAULT marker
# regexes - they are the C++ log contract (resources/CODE_MAP.md).

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

# assault_town (join-initiated town combat): the JOIN's player character starts an
# unprovoked fight with a world NPC; the fight must appear on the HOST. The gate
# walks the wire chain link by link so a FAIL names the broken link:
#   1. issued  - join ordered its own leader onto a victim (scenario marker)
#   2. cap     - the join's replicator CAPTURED + streamed the combat intent
#                ("[combat] CAP hand=<atk>" on the join)
#   3. applied - the host's drive path ORDERED its join-PC copy into the fight
#                ("[combat] order hand=<atk> ... r=2" on the host)
#   4. fight   - the host's local engine actually runs it ("hostview fight=1"
#                lines; MinFight samples at 1 Hz)
function Test-AssaultTown {
    param([string]$HostFile, [string]$JoinFile, [int]$MinFight = 3)
    $mi = Select-String -Path $JoinFile -Pattern 'SCENARIO ASSAULT issued atk=(\d+),(\d+) vic=(\d+),(\d+) ok=(\d)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $mi) {
        Write-Host "  ASSAULT-TOWN FAIL - join never issued the assault (no victim pick?)"
        return (Add-GateResult -Name "assault_town" -Status FAIL -Detail "no assault order (link 1)")
    }
    $atk = $mi.Matches[0].Groups[1].Value + ',' + $mi.Matches[0].Groups[2].Value
    $vic = $mi.Matches[0].Groups[3].Value + ',' + $mi.Matches[0].Groups[4].Value

    # 2. Capture: the join streamed a combat task for its leader (any target -
    # a bar brawl legitimately retargets); strict victim match kept as metric.
    $capAll = @(Select-String -Path $JoinFile -Pattern ('\[combat\] CAP hand=' + $atk + ' ') -ErrorAction SilentlyContinue).Count
    $capVic = @(Select-String -Path $JoinFile -Pattern ('\[combat\] CAP hand=' + $atk + ' task=\d+ tgt=\d+,\d+,\d+,' + $vic + '$') -ErrorAction SilentlyContinue).Count

    # 3. Applied: the host's combat branch ordered its driven join-PC copy
    # (r=2 = ordered; r=1 = target hand didn't resolve on the host).
    $ordOk   = @(Select-String -Path $HostFile -Pattern ('\[combat\] order hand=' + $atk + ' tgt=\d+,\d+ .*r=2') -ErrorAction SilentlyContinue).Count
    $ordMiss = @(Select-String -Path $HostFile -Pattern ('\[combat\] order hand=' + $atk + ' tgt=\d+,\d+ .*r=1') -ErrorAction SilentlyContinue).Count
    $ordVic  = @(Select-String -Path $HostFile -Pattern ('\[combat\] order hand=' + $atk + ' tgt=' + $vic + ' .*r=2') -ErrorAction SilentlyContinue).Count

    # 4. Fight: the host's local combat read of the join-PC copy.
    $fight = @(Select-String -Path $HostFile -Pattern 'SCENARIO ASSAULT hostview fight=1' -ErrorAction SilentlyContinue).Count

    $m = @{ cap = $capAll; capVic = $capVic; ordered = $ordOk; orderedVic = $ordVic
            orderMiss = $ordMiss; hostFight = $fight }
    $bad = @()
    if ($capAll -lt 1) { $bad += "join never streamed a combat intent for $atk (link 2: capture)" }
    elseif ($ordOk -lt 1) {
        if ($ordMiss -ge 1) { $bad += "host saw the intent but the target hand never resolved ($ordMiss r=1 orders; link 3: target resolve)" }
        else                { $bad += "host never ordered the join-PC copy (0 [combat] order lines; link 3: apply)" }
    }
    if ($fight -lt $MinFight) { $bad += "host-side fight never ran (hostview fight=1 x$fight, need >= $MinFight; link 4)" }
    if ($bad.Count -gt 0) {
        Write-Host ("  ASSAULT-TOWN FAIL - " + ($bad -join "; ") + " [cap=$capAll capVic=$capVic ord=$ordOk ordVic=$ordVic miss=$ordMiss fight=$fight]")
        return (Add-GateResult -Name "assault_town" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  ASSAULT-TOWN PASS - atk=$atk vic=$vic cap=$capAll (vic-match $capVic) host-orders=$ordOk (vic-match $ordVic) hostview-fight=$fight")
    return (Add-GateResult -Name "assault_town" -Status PASS -Metrics $m)
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
    # (Phase 1 warp diagnosis enriched the line with srcVel/localFight/wrongTgt/
    # arming/seg/n between drift and wait; the .* keeps this tolerant of them.)
    foreach ($sm in (Select-String -Path $JoinFile -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[combat\] snap hand=(\d+),(\d+) drift=[\d\.]+.* wait=(\d)' -ErrorAction SilentlyContinue)) {
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

# combat_snap_rate (2026-07-15 many-NPC combat warp): the JOIN warp during fights
# is combat-drive hard TELEPORTS ([combat] snap = engine::applyRaw), not locomotion
# starvation (Test-SnapRate covers that, keyed off [interp]). This oracle keys off
# the Phase 1-enriched [combat] snap line and classifies each teleport by root
# cause so a high rate of the AVOIDABLE kind fails while a legitimate sprint-chase
# does not:
#   * CHURN  - localFight=1 on a near-STATIONARY source (srcVel < ChurnVel) at a
#              moderate drift (< ChaseDrift): both sims fight the right body but the
#              local footwork diverged - convergence should absorb this, a teleport
#              is the visible warp. THIS is what the fix targets.
#   * CHASE  - high srcVel or drift >= ChaseDrift: the host body genuinely left
#              (sprint-chase measured 45-110 u); a teleport IS the right tool. Not
#              gated (counted for context only).
#   * WRONGT - wrongTgt=1: the local copy fights the WRONG body, so it stands in
#              the wrong place and snaps repeatedly - an order/target problem.
# Gates (all after an engagement SETTLE window):
#   1. CHURN RATE   - avoidable teleports/min <= MaxChurnPerMin.
#   2. PERSISTENCE  - no single hand snaps more than MaxPersistPerHand times (a
#                     body that keeps snapping is PERSISTENTLY divergent, not
#                     occasionally churning - the [combat] snap n= / stats
#                     maxPersist signal).
#   3. WRONG TARGET - wrongTgt snaps <= MaxWrongTgt.
# SKIP when no combat happened (no [combat] order) so a peaceful run never falses.
function Test-CombatSnapRate {
    param([string]$JoinFile, [string]$Label = "join",
          [double]$MaxChurnPerMin = 12.0, [int]$MaxPersistPerHand = 6,
          [int]$MaxWrongTgt = 8,
          [double]$ChurnVel = 8.0, [double]$ChaseDrift = 45.0,
          [int]$SettleMs = 8000, [int]$MinWindowSec = 15)
    if (-not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "combat_snap_rate" -Status SKIP -Detail "no join log")
    }
    $joff = Get-LogClockOffsetMs -File $JoinFile
    # Engagement start = first [combat] order on the join (a fight was reproduced).
    $tOrder = Get-MarkerTimeMs -File $JoinFile -Pattern '\[combat\] order hand='
    if ($null -eq $tOrder) {
        Write-Host "  [$Label] combat-snap-rate SKIP - no [combat] order (no fight reproduced)"
        return (Add-GateResult -Name "combat_snap_rate" -Status SKIP -Detail "no combat")
    }
    $settleEnd = [double]$tOrder + $SettleMs
    # Window end = last combat-drive activity (order/snap/stats) on the join.
    $lastT = $tOrder
    foreach ($lm in (Select-String -Path $JoinFile -Pattern '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[combat\] (?:order|snap|stats)' -ErrorAction SilentlyContinue)) {
        $lt = Convert-StampToMs -Groups $lm.Matches[0].Groups -OffsetMs $joff
        if ([double]$lt -gt [double]$lastT) { $lastT = $lt }
    }
    $winSec = ([double]$lastT - $settleEnd) / 1000.0
    if ($winSec -lt $MinWindowSec) {
        Write-Host "  [$Label] combat-snap-rate SKIP - scored window $([math]::Round($winSec,0))s (< ${MinWindowSec}s past settle)"
        return (Add-GateResult -Name "combat_snap_rate" -Status SKIP `
                    -Metrics @{ windowSec = [math]::Round($winSec, 1) } -Detail "window too short")
    }
    # Enriched [combat] snap line: hand, drift, srcVel, localFight, wrongTgt,
    # arming, wait, seg, n (the .* tolerates future field additions).
    $rx = '\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*\[combat\] snap hand=(\d+),(\d+) drift=([\d\.]+) srcVel=([\d\.]+) localFight=(\d) wrongTgt=(\d) arming=(\d) wait=(\d) seg=\d+ n=(\d+)'
    $churn = 0; $chase = 0; $wrongT = 0; $total = 0
    $persist = @{}; $drifts = @()
    foreach ($sm in (Select-String -Path $JoinFile -Pattern $rx -ErrorAction SilentlyContinue)) {
        $g = $sm.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $joff
        if ([double]$t -lt $settleEnd) { continue }
        $hand      = $g[5].Value + "," + $g[6].Value
        $drift     = [double]$g[7].Value
        $srcVel    = [double]$g[8].Value
        $localFight= ($g[9].Value -eq "1")
        $wrong     = ($g[10].Value -eq "1")
        $n         = [int]$g[13].Value
        $total++
        $drifts += $drift
        if ($persist[$hand] -lt $n) { $persist[$hand] = $n }
        if ($wrong) { $wrongT++ }
        if (($srcVel -ge $ChurnVel) -or ($drift -ge $ChaseDrift)) { $chase++ }
        elseif ($localFight) { $churn++ }
        else { $churn++ }  # stationary source, mid drift, not actively fighting = still avoidable
    }
    $maxPersist = 0
    foreach ($k in $persist.Keys) { if ($persist[$k] -gt $maxPersist) { $maxPersist = $persist[$k] } }
    $churnRate = [math]::Round($churn / ($winSec / 60.0), 2)
    $totalRate = [math]::Round($total / ($winSec / 60.0), 2)
    $medDrift = 0.0
    if ($drifts.Count -gt 0) {
        $sorted = @($drifts | Sort-Object)
        $medDrift = [math]::Round($sorted[[int]([math]::Floor($sorted.Count / 2))], 1)
    }
    $m = @{ total = $total; churn = $churn; chase = $chase; wrongTgt = $wrongT
            churnRatePerMin = $churnRate; totalRatePerMin = $totalRate
            maxPersistPerHand = $maxPersist; medianDrift = $medDrift
            windowSec = [math]::Round($winSec, 1) }
    $bad = @()
    if ($churnRate -gt $MaxChurnPerMin) { $bad += "churn snaps $churnRate/min (> $MaxChurnPerMin)" }
    if ($maxPersist -gt $MaxPersistPerHand) { $bad += "a hand snapped ${maxPersist}x (> $MaxPersistPerHand - persistent divergence)" }
    if ($wrongT -gt $MaxWrongTgt) { $bad += "$wrongT wrong-target snap(s) (> $MaxWrongTgt)" }
    if ($bad.Count -gt 0) {
        Write-Host ("  [$Label] combat-snap-rate FAIL - " + ($bad -join "; ") +
                    " [total $total = churn $churn + chase $chase; medDrift ${medDrift}u; window $([math]::Round($winSec,0))s]")
        return (Add-GateResult -Name "combat_snap_rate" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  [$Label] combat-snap-rate PASS - churn $churnRate/min (<= $MaxChurnPerMin), maxPersist $maxPersist (<= $MaxPersistPerHand), " +
                "wrongTgt $wrongT (<= $MaxWrongTgt); total $total = churn $churn + chase $chase, medDrift ${medDrift}u over $([math]::Round($winSec,0))s")
    return (Add-GateResult -Name "combat_snap_rate" -Status PASS -Metrics $m)
}

# combat_battle (2026-07-16 many-NPC combat warp): PRESENCE/engagement gate for the
# combat_battle scenario (the actual snap quality is judged by combat_snap_rate,
# gated alongside this). Proves the many-NPC fight actually happened so a spawn/
# engage failure fails LOUDLY instead of letting combat_snap_rate SKIP:
#   1. HOST spawned the battle - "SCENARIO BATTLE spawned=N/..." with N >= MinBattlers
#      and a "SCENARIO BATTLE issued" marker (the pairs were ordered into melee).
#   2. JOIN reproduced it - the join issued combat orders for the streamed fighters
#      ("[combat] order") for at least MinJoinOrders distinct hands (the fights
#      crossed and the join drove them - the precondition for any snap measurement).
function Test-CombatBattle {
    param([string]$HostFile, [string]$JoinFile,
          [int]$MinBattlers = 4, [int]$MinJoinOrders = 3)
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "combat_battle" -Status FAIL -Detail "missing log")
    }
    $sp = Select-String -Path $HostFile -Pattern 'SCENARIO BATTLE spawned=(\d+)/(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $sp) {
        Write-Host "  COMBAT-BATTLE FAIL - host never spawned the battle"
        return (Add-GateResult -Name "combat_battle" -Status FAIL -Detail "no spawn marker")
    }
    $spawned = [int]$sp.Matches[0].Groups[1].Value
    $want    = [int]$sp.Matches[0].Groups[2].Value
    $issued  = @(Select-String -Path $HostFile -Pattern 'SCENARIO BATTLE issued n=(\d+)' -ErrorAction SilentlyContinue).Count
    # Distinct join-side combat hands ordered (the fights the join reproduced).
    $joinHands = @{}
    foreach ($jm in (Select-String -Path $JoinFile -Pattern '\[combat\] order hand=(\d+),(\d+)' -ErrorAction SilentlyContinue)) {
        $joinHands[$jm.Matches[0].Groups[1].Value + ',' + $jm.Matches[0].Groups[2].Value] = $true
    }
    $m = @{ spawned = $spawned; want = $want; joinOrderedHands = $joinHands.Count }
    $bad = @()
    if ($spawned -lt $MinBattlers) { $bad += "only $spawned/$want battlers spawned (< $MinBattlers)" }
    if ($issued -lt 1) { $bad += "host never issued the battle orders" }
    if ($joinHands.Count -lt $MinJoinOrders) { $bad += "join reproduced only $($joinHands.Count) fighter(s) (< $MinJoinOrders)" }
    if ($bad.Count -gt 0) {
        Write-Host ("  COMBAT-BATTLE FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "combat_battle" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host "  COMBAT-BATTLE PASS - $spawned/$want battlers spawned, join drove $($joinHands.Count) fighter(s)"
    return (Add-GateResult -Name "combat_battle" -Status PASS -Metrics $m)
}

# combat_win (2026-07-16 smoothness pass, second warp shape): each side buffs its
# OWN player-squad to 120 in every stat and the host runtime-spawns N enemies onto
# the PC leader; the buffed PCs win, so the join stress is dying/fleeing/KO churn.
# Presence/outcome oracle (the warp itself is gated by combat_snap_rate):
#   1. BOTH sides buffed their own PCs (SCENARIO WIN buff rank=0 on host, rank=1 on join)
#   2. host spawned >= MinEnemies and issued the attack orders
#   3. the fight was WON (SCENARIO WIN down >= 1 on the host) and CROSSED (the join
#      reproduced the enemy copies - SCENARIO RECV present)
function Test-CombatWin {
    param([string]$HostFile, [string]$JoinFile, [int]$MinEnemies = 4)
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        return (Add-GateResult -Name "combat_win" -Status FAIL -Detail "missing log")
    }
    $hostBuff = @(Select-String -Path $HostFile -Pattern 'SCENARIO WIN buff rank=0' -ErrorAction SilentlyContinue).Count
    $joinBuff = @(Select-String -Path $JoinFile -Pattern 'SCENARIO WIN buff rank=1' -ErrorAction SilentlyContinue).Count
    $sp = Select-String -Path $HostFile -Pattern 'SCENARIO WIN spawned=(\d+)/(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    $spawned = if ($sp) { [int]$sp.Matches[0].Groups[1].Value } else { 0 }
    $want    = if ($sp) { [int]$sp.Matches[0].Groups[2].Value } else { 0 }
    $issued  = @(Select-String -Path $HostFile -Pattern 'SCENARIO WIN issued n=(\d+)' -ErrorAction SilentlyContinue).Count
    $peak = {
        param($file)
        $mx = 0
        foreach ($mm in (Select-String -Path $file -Pattern 'SCENARIO WIN down=(\d+)/' -ErrorAction SilentlyContinue)) {
            $v = [int]$mm.Matches[0].Groups[1].Value
            if ($v -gt $mx) { $mx = $v }
        }
        return $mx
    }
    $hostDown = & $peak $HostFile
    $joinDown = & $peak $JoinFile
    $joinRecv = @(Select-String -Path $JoinFile -Pattern 'SCENARIO RECV ' -ErrorAction SilentlyContinue).Count
    $m = @{ hostBuff=$hostBuff; joinBuff=$joinBuff; spawned=$spawned; want=$want;
            issued=$issued; hostDown=$hostDown; joinDown=$joinDown; joinRecv=$joinRecv }
    $bad = @()
    if ($hostBuff -lt 1) { $bad += "host never buffed its PCs (no rank=0 buff)" }
    if ($joinBuff -lt 1) { $bad += "join never buffed its PCs (no rank=1 buff)" }
    if ($spawned -lt $MinEnemies) { $bad += "only $spawned/$want enemies spawned (< $MinEnemies)" }
    if ($issued -lt 1) { $bad += "host never issued the attack orders" }
    if ($hostDown -lt 1) { $bad += "no enemy went down (the PCs did not win)" }
    if ($joinRecv -lt 1) { $bad += "join never reproduced the enemy copies" }
    if ($bad.Count -gt 0) {
        Write-Host ("  COMBAT-WIN FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "combat_win" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host "  COMBAT-WIN PASS - buffed host=$hostBuff join=$joinBuff PC(s); $spawned/$want enemies, down host=$hostDown join=$joinDown"
    return (Add-GateResult -Name "combat_win" -Status PASS -Metrics $m)
}

# death_parity (2026-07-15 owner-authoritative death fix): cross-checks a MANUAL
# side-by-side session for the "dead on one game, alive on the other" desync.
# Death is authored by a body's OWNER (reliable EVT_DEATH, ev=2) and reproduced on
# the peer; a body can also RE-CONTAINER as it dies (squad move), which used to
# WIPE the peer's death pin (rekeyPeerBody erased the Driven latch). Gates, from
# both logs (bodies matched on the stable index,serial pair - the container/tab
# fields legitimately change across a re-key):
#   1. PARITY - every EVT_DEATH SEND on one side has a matching ev=2 RECV on the
#      other (the death crossed). Unmatched sends = a death that never reached the
#      peer.
#   2. LATCH CARRY - report "[event] REKEY-LATCH ... death=1" (the fix carrying a
#      dead body's pin onto its new hand key); informational unless a violation
#      below fires.
#   3. VETO SANITY - "[death] veto" un-kills a driven copy the OWNER still reports
#      alive; it must NEVER fire for a body that side ALSO received an EVT_DEATH
#      for (that would be un-killing an owner-authored corpse). Any such overlap
#      FAILs.
# Log-judged against a MANUAL fight session; invoke as:
#   . scripts\oracles\Combat.ps1
#   Test-DeathParity -HostFile <Kenshi>\KenshiCoop_host.log -JoinFile <Kenshi-Join>\KenshiCoop_join.log
function Test-DeathParity {
    param([string]$HostFile, [string]$JoinFile)
    if (-not (Test-Path $HostFile) -or -not (Test-Path $JoinFile)) {
        Write-Host "  DEATH-PARITY FAIL - log(s) not found (host=$HostFile join=$JoinFile)"
        return (Add-GateResult -Name "death_parity" -Status FAIL -Detail "missing log")
    }
    # index,serial are the last two of the 5-field hand (type,container,cs,index,serial):
    # they SURVIVE a container re-key, so they are the stable identity to match on.
    $sendRx = [regex]'\[event\] SEND id=\d+ ev=2 hand=\d+,\d+,\d+,(\d+),(\d+)'
    $recvRx = [regex]'\[event\] RECV id=\d+ ev=2 .*hand=\d+,\d+,\d+,(\d+),(\d+)'
    $latchRx = [regex]'\[event\] REKEY-LATCH .*death=1'
    $vetoRx  = [regex]'\[death\] veto hand=(\d+,\d+)'
    $collect = {
        param($file, $rx)
        $set = @{}
        foreach ($mm in (Select-String -Path $file -Pattern $rx -AllMatches).Matches) {
            $set[$mm.Groups[1].Value + ',' + $mm.Groups[2].Value] = $true
        }
        return $set
    }
    $hSends = & $collect $HostFile $sendRx
    $jSends = & $collect $JoinFile $sendRx
    $hRecvs = & $collect $HostFile $recvRx
    $jRecvs = & $collect $JoinFile $recvRx
    $totalSends = $hSends.Count + $jSends.Count
    if ($totalSends -lt 1) {
        Write-Host "  DEATH-PARITY SKIP - no EVT_DEATH (ev=2) in either log; get a squad member killed so a death is authored"
        return (Add-GateResult -Name "death_parity" -Status SKIP -Detail "no deaths in session")
    }
    # 1. Parity: host sends must be received on the join; join sends on the host.
    $unmatched = @()
    foreach ($k in $hSends.Keys) { if (-not $jRecvs.ContainsKey($k)) { $unmatched += "host->join $k" } }
    foreach ($k in $jSends.Keys) { if (-not $hRecvs.ContainsKey($k)) { $unmatched += "join->host $k" } }
    # 2. Latch carry (informational count).
    $latch = @(Select-String -Path $HostFile -Pattern $latchRx -ErrorAction SilentlyContinue).Count +
             @(Select-String -Path $JoinFile -Pattern $latchRx -ErrorAction SilentlyContinue).Count
    # 3. Veto sanity: a veto on a body that side ALSO got an owner death for = bug.
    $vetoBad = @()
    $vetoTotal = 0
    foreach ($pair in @(@{ f = $HostFile; dead = $hRecvs }, @{ f = $JoinFile; dead = $jRecvs })) {
        foreach ($vm in (Select-String -Path $pair.f -Pattern $vetoRx -AllMatches).Matches) {
            $vetoTotal++
            $is = $vm.Groups[1].Value
            if ($pair.dead.ContainsKey($is)) { $vetoBad += "$is (owner-dead)" }
        }
    }
    $m = @{ hostSends = $hSends.Count; joinSends = $jSends.Count
            hostRecvs = $hRecvs.Count; joinRecvs = $jRecvs.Count
            unmatched = $unmatched.Count; rekeyLatch = $latch
            vetoes = $vetoTotal; vetoViolations = $vetoBad.Count }
    $bad = @()
    if ($unmatched.Count -gt 0) { $bad += "$($unmatched.Count) death(s) never reached the peer: " + ($unmatched -join ', ') }
    if ($vetoBad.Count -gt 0)   { $bad += "$($vetoBad.Count) veto(es) fired on an owner-dead body: " + ($vetoBad -join ', ') }
    if ($bad.Count -gt 0) {
        Write-Host ("  DEATH-PARITY FAIL - " + ($bad -join "; "))
        return (Add-GateResult -Name "death_parity" -Status FAIL -Metrics $m -Detail ($bad -join "; "))
    }
    Write-Host ("  DEATH-PARITY PASS - deaths host->join $($hSends.Count) join->host $($jSends.Count), all received; " +
                "rekey-latch carries=$latch, vetoes=$vetoTotal (0 on owner-dead bodies)")
    return (Add-GateResult -Name "death_parity" -Status PASS -Metrics $m)
}
