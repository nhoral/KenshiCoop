# oracles/Inventory.ps1 - inventory + world-item oracles (monolith split of
# CoopOracles.psm1, 2026-07-12): Test-InventorySync/Bidir/Equip/Reequip,
# Test-AddEquip, Test-TradeProbe, Test-TradePeer, Test-DropProbe,
# Test-WorldItemSync, Test-WpnRelocate, Test-WeaponDrop, Test-WeaponLoot.
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the INV/TINV/DROP/GEAR marker regexes -
# they are the C++ log contract (resources/CODE_MAP.md).
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

# rejoin_items (Phase 3 item-dup fix): a reload must not duplicate save-native
# ground items. The HOST drops K items (both clients reach n0+K), coordinated-
# saves so the drops bake into the shared save, then loads it mid-session. The
# first-scan baseline must record the now-native drops as never-emit; WITHOUT it
# the host re-streams them and the join layers a duplicate proxy per reload. The
# gate: POST-reload ground-item count must NOT grow past the PRE-reload count on
# EITHER side (equal is the fixed behavior), a reload edge actually happened, and
# the host authored the drop + coordinated save + mid-session load.
function Test-RejoinItems {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    $parse = {
        param($file)
        $o = [pscustomobject]@{ base = -1; pre = -1; post = -1; postSeen = $false
                                swapDone = $false; reload = $false; verdictPass = $null }
        if (Test-Path $file) {
            foreach ($ln in Get-Content $file) {
                if ($ln -match 'SCENARIO RI BASELINE n=(-?\d+)') { $o.base = [int]$matches[1] }
                if ($ln -match 'SCENARIO RI PRERELOAD n=(-?\d+)') { $o.pre = [int]$matches[1] }
                if ($ln -match 'SCENARIO RI POSTRELOAD n=(-?\d+) pre=(-?\d+)') {
                    $o.post = [int]$matches[1]
                    if ($o.pre -lt 0) { $o.pre = [int]$matches[2] }
                    $o.postSeen = $true
                }
                if ($ln -match 'SCENARIO RI SWAPDONE')     { $o.swapDone = $true }
                if ($ln -match '\[load\] WORLD-RELOAD')    { $o.reload = $true }
                if ($ln -match 'SCENARIO RI verdict .*pass=(\d+)') { $o.verdictPass = ([int]$matches[1] -eq 1) }
            }
        }
        return $o
    }
    $H = & $parse $HostFile
    $J = & $parse $JoinFile

    # Host authoring legs.
    $drop = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI DROP .*dropped=[1-9]' -Quiet -ErrorAction SilentlyContinue)
    $save = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI SAVE .*ok=1' -Quiet -ErrorAction SilentlyContinue)
    $load = [bool](Select-String -Path $HostFile -Pattern 'SCENARIO RI LOAD .*ok=1' -Quiet -ErrorAction SilentlyContinue)
    if (-not $drop) { $why += "host never dropped test items (no 'SCENARIO RI DROP dropped>=1')" }
    if (-not $save) { $why += "host coordinated save failed (no 'SCENARIO RI SAVE ok=1')" }
    if (-not $load) { $why += "host mid-session load failed (no 'SCENARIO RI LOAD ok=1')" }

    # The drops must have actually entered the world+save (host grew over its own
    # baseline), otherwise the test never exercised the item-dup path.
    if ($H.base -ge 0 -and $H.pre -ge 0 -and $H.pre -le $H.base) {
        $why += "host drops never registered (baseline=$($H.base) >= preReload=$($H.pre) - nothing to duplicate)"
    }

    # A reload edge must have happened on BOTH sides (the coordinated load must
    # drive the join, not just the host).
    if (-not ($H.reload -or $H.swapDone)) { $why += "host saw no reload edge (no WORLD-RELOAD / RI SWAPDONE)" }
    if (-not ($J.reload -or $J.swapDone)) { $why += "join saw no reload edge (coordinated load did not drive the join)" }

    if (-not $H.postSeen) { $why += "host missing POSTRELOAD census" }
    if (-not $J.postSeen) { $why += "join missing POSTRELOAD census" }

    # THE gate (cross-client parity): both clients loaded the byte-identical save,
    # so once co-located after the swap they must converge to the SAME native
    # count. A join that mints a proxy on top of each save-native (the reload dup
    # bug) shows a count that EXCEEDS the host's authoritative native count. So a
    # join-post GREATER than host-post = duplication. (join-pre is NOT a valid
    # baseline: the drops land near the host leader, often outside the join's 60u
    # interest sphere pre-reload, so the join legitimately reaches the full count
    # only after the reload co-locates it with the saved items.)
    if ($H.postSeen -and $J.postSeen -and $H.post -ge 0 -and $J.post -gt $H.post) {
        $why += "join DUPLICATED items on reload (join post=$($J.post) EXCEEDS host post=$($H.post) - a proxy minted on top of each save-native)"
    }
    # Host must not balloon against its own pre-reload count either.
    if ($H.postSeen -and $H.pre -ge 0 -and $H.post -gt $H.pre) {
        $why += "host DUPLICATED items on reload (pre=$($H.pre) -> post=$($H.post))"
    }

    Write-Host ("    FINDING: host base={0} pre={1} post={2} reload={3} | join pre={4} post={5} reload={6} | parity(join<=host)={7} | drop={8} save={9} load={10}" -f `
        $H.base, $H.pre, $H.post, ($H.reload -or $H.swapDone), $J.pre, $J.post, ($J.reload -or $J.swapDone), `
        ($J.post -le $H.post), $drop, $save, $load)

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  REJOIN-ITEMS $v - $detail"
    return (Add-GateResult -Name "rejoin_items" -Status $v -Metrics @{
        hostBase = $H.base; hostPre = $H.pre; hostPost = $H.post
        joinPre = $J.pre; joinPost = $J.post
        drop = $drop; save = $save; load = $load } -Detail $detail)
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

