# oracles/World.ps1 - economy / faction / time / door / build / production /
# container oracles (monolith split of CoopOracles.psm1, 2026-07-12):
# Get-WalletSeries, Test-ShopProbe/MoneySync/VendorTrade, Test-Recruit*,
# Test-Squad*, Test-Faction*, Test-Time* (+ Get-SlewSummary), Test-Door*,
# Test-Build*, Test-Bdoor*, Test-Hunger*, Test-Prod*, Test-Research*,
# Test-Store*. Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the $script:*Regex marker patterns -
# they are the C++ log contract (resources/CODE_MAP.md).
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
        # Phase 1b membership: the peer must make the recruited body a REAL squad
        # member (insert=1), so it shows in the panel on BOTH games. The lever's
        # immediate playerSquad recheck is a lazy-refresh false negative, so gate
        # on the setFaction success signal (insert=1), not the recheck.
        $member = @(Select-String -Path $peerFile -Pattern "\[recruit\] MEMBER new=$hand insert=1" -ErrorAction SilentlyContinue).Count
        if (($rekey + $bound) -eq 0) { $why += "$k never converged on the peer (no REKEY, no proxy BOUND)" }
        elseif ($rekey -ge 1 -and $bound -ge 1) { $why += "$k DUPLICATED on the peer (rekeyed AND minted a proxy)" }
        else {
            if ($track -eq 0) { $why += "$k bound on the peer but never tracked (no PROXY series)" }
            elseif ($member -eq 0) { $why += "$k bound on the peer but never joined the squad (no MEMBER insert=1)" }
            else { $converged++ }
        }
        Write-Host "    FINDING: $k peer rekey=$rekey proxyBound=$bound proxyTrack=$track member=$member"
    }

    # Phase 1b squad parity: with cross-game membership, a recruit is a member on
    # BOTH games, so the two squads must end the run the SAME size. A mismatch is
    # the duplication class of bug (an escaped-pin re-container the owner minted a
    # second copy of - recruit_sync run 095843: join 8 vs host 6).
    $sqRegex = 'SCENARIO TABS n=\d+ squad=(\d+)'
    $hTabs = @(Select-String -Path $HostFile -Pattern $sqRegex -ErrorAction SilentlyContinue)
    $jTabs = @(Select-String -Path $JoinFile -Pattern $sqRegex -ErrorAction SilentlyContinue)
    if ($hTabs.Count -gt 0 -and $jTabs.Count -gt 0) {
        $hSquad = [int]$hTabs[-1].Matches[0].Groups[1].Value
        $jSquad = [int]$jTabs[-1].Matches[0].Groups[1].Value
        if ($hSquad -ne $jSquad) {
            $why += "squad size diverged (host=$hSquad join=$jSquad) - recruit duplication"
        }
        Write-Host "    FINDING: end squad size host=$hSquad join=$jSquad"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RECRUIT-SYNC $v - converged=$converged/4 $detail"
    return (Add-GateResult -Name "recruit_sync" -Status $v `
                -Metrics @{ converged = $converged } -Detail $detail)
}

# recruit_ctl (Phase 1b control validation): layered on recruit convergence, it
# gates the two manual-test regressions:
#   * gait-parity - a DRIVEN squad member must reproduce the OWNER's RUN, not a
#     walk. The scenario walks the recruit while the OTHER client drives it and
#     logs SCENARIO GAIT on both sides (role=own on the owner, role=drive on the
#     driver). Per phase we compare the median MOVING speed: a driver crawling
#     while the owner runs is the bug (manual 2026-07-17: Adi walked).
#   * anti-phantom - a control-flip transfer must NEVER mint a proxy for the
#     hand it just CLAIMED (the phantom "Squint" that chased Adi: the host's
#     in-flight batches for the now-owned hand minted a duplicate).
# Phase A owner=host/driver=join; phase B (after the join's transfer)
# owner=join/driver=host, so the fix is validated in BOTH drive directions.
function Test-RecruitCtl {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()
    $RUN_MIN    = 1.0   # owner median above this = it genuinely moved (not idle)
    $GAIT_RATIO = 0.5   # driver median must be >= this fraction of the owner's

    # --- Preconditions: the path was actually exercised -----------------------
    $recruit = @(Select-String -Path $HostFile -Pattern 'SCENARIO CTL recruit res=1' -ErrorAction SilentlyContinue).Count
    if ($recruit -eq 0) { $why += "host never recruited (no SCENARIO CTL recruit res=1)" }
    $found = @(Select-String -Path $JoinFile -Pattern 'SCENARIO CTL found ' -ErrorAction SilentlyContinue).Count
    if ($found -eq 0) { $why += "join never found the recruit as a new member" }
    $move = @(Select-String -Path $HostFile -Pattern 'SCENARIO CTL move rc=1' -ErrorAction SilentlyContinue).Count
    if ($move -eq 0) { $why += "host never transferred the recruit into the join tab (no move rc=1)" }
    # The transfer is host-authored INTO the join's tab, so the join RECEIVES it
    # and its rekeyPeerBody claims ownership (CONTROL-FLIP) - that is what makes
    # the phantom race fire, so a missing flip means the path was not exercised.
    $flip = @(Select-String -Path $JoinFile -Pattern '\] CONTROL-FLIP claim new=' -ErrorAction SilentlyContinue).Count
    if ($flip -eq 0) { $why += "control-flip never fired on the join (transfer did not claim ownership)" }

    # --- Gait parity ----------------------------------------------------------
    $gRegex = 'SCENARIO GAIT who=(host|join) phase=([AB]) role=(own|drive) moving=(\d) speed=([\d.]+)'
    function _ctlSpeeds($file, $phase, $role, $rx) {
        $out = @()
        foreach ($m in @(Select-String -Path $file -Pattern $rx -ErrorAction SilentlyContinue)) {
            $g = $m.Matches[0].Groups
            if ($g[2].Value -ne $phase) { continue }
            if ($g[3].Value -ne $role)  { continue }
            if ($g[4].Value -ne '1')    { continue }  # moving samples only
            $out += [double]$g[5].Value
        }
        return ,$out
    }
    function _ctlMedian($a) {
        if ($a.Count -eq 0) { return 0.0 }
        $s = @($a | Sort-Object)
        return [double]$s[[int]([math]::Floor($s.Count / 2))]
    }
    # Phase A (owner=host, driver=join) is the DRIVEN-gait regression the user
    # reported (Adi walked on the join) - a HARD gate. Phase B (owner=join,
    # driver=host, after a HOST-authored cross-tab transfer) exercises the
    # author-side control-RELEASE path, which is a separate capability (the
    # author currently pins the moved body owned unconditionally); it is recorded
    # ADVISORY here until that release + owner-side run of a just-claimed body
    # land (a synthetic non-inhabit run has no rank latch, so it is not a
    # reliable gate).
    foreach ($ph in @(
        @{ p = 'A'; ownerFile = $HostFile; drvFile = $JoinFile; hard = $true },
        @{ p = 'B'; ownerFile = $JoinFile; drvFile = $HostFile; hard = $false })) {
        $own = _ctlSpeeds $ph.ownerFile $ph.p 'own'   $gRegex
        $drv = _ctlSpeeds $ph.drvFile   $ph.p 'drive' $gRegex
        $ownMed = _ctlMedian $own
        $drvMed = _ctlMedian $drv
        Write-Host ("    FINDING: gait phase={0} owner n={1} med={2} driver n={3} med={4} hard={5}" -f `
            $ph.p, $own.Count, [math]::Round($ownMed, 2), $drv.Count, [math]::Round($drvMed, 2), $ph.hard)
        $fail = $null
        if ($own.Count -lt 5 -or $ownMed -lt $RUN_MIN) {
            $fail = "phase $($ph.p): owner never ran (n=$($own.Count) med=$([math]::Round($ownMed,2)))"
        } elseif ($drv.Count -lt [int](0.3 * $own.Count)) {
            $fail = "phase $($ph.p): driver rarely moved (n=$($drv.Count) vs owner $($own.Count)) - walk/stall"
        } elseif ($drvMed -lt ($GAIT_RATIO * $ownMed)) {
            $fail = "phase $($ph.p): driver gait too slow (med=$([math]::Round($drvMed,2)) < $GAIT_RATIO x owner $([math]::Round($ownMed,2))) - walking not running"
        }
        if ($null -ne $fail) {
            if ($ph.hard) { $why += $fail }
            else { Write-Host "    ADVISORY: $fail (author-side control-release follow-up)" }
        }
    }

    # --- Anti-phantom: a claimed hand must never be proxy-minted ---------------
    $phantom = 0
    foreach ($pair in @(@('host', $HostFile), @('join', $JoinFile))) {
        foreach ($m in @(Select-String -Path $pair[1] -Pattern '\] CONTROL-FLIP claim new=([\d,]+)' -ErrorAction SilentlyContinue)) {
            $h = [regex]::Escape($m.Matches[0].Groups[1].Value)
            $pb = @(Select-String -Path $pair[1] -Pattern "\[spawn\] proxy BOUND hand=$h" -ErrorAction SilentlyContinue).Count
            if ($pb -gt 0) {
                $phantom++
                $why += "$($pair[0]) minted a PHANTOM proxy for claimed hand $($m.Matches[0].Groups[1].Value)"
            }
        }
    }
    Write-Host "    FINDING: control-flips=$flip phantomMints=$phantom"

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  RECRUIT-CTL $v - $detail"
    return (Add-GateResult -Name "recruit_ctl" -Status $v `
                -Metrics @{ phantom = $phantom; flips = $flip } -Detail $detail)
}

# Test-CampApproach (Phase 2 crash-hardening SOAK): validate the fix MECHANISMS
# from the flushed plugin logs of a camp_approach run. There is no deterministic
# repro of the original approach crash, so this proves: (1) both clients reach
# scenario completion (no crash / no truncated log); (2) the surviving join saw
# the host drop and (3) ran clearPeerReplicationState (B1); (4) the join did NOT
# crash AFTER the drop (its SCENARIO RESULT follows the peer-left). Stale-unbind
# (B2 guard firing) and mint volume are reported as findings, not failures - a
# STALE unbind is the guard WORKING.
function Test-CampApproach {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1) Both processes reached scenario completion = they did not crash. The
    #    join is the survivor that churns through the drop - its RESULT is the
    #    core "did the hardening hold" signal.
    $hostResult = @(Select-String -Path $HostFile -Pattern 'SCENARIO RESULT ' -ErrorAction SilentlyContinue).Count
    $joinResults = @(Select-String -Path $JoinFile -Pattern 'SCENARIO RESULT ' -ErrorAction SilentlyContinue | ForEach-Object { $_.LineNumber })
    if ($hostResult -eq 0) { $why += "host never reached SCENARIO RESULT (crash/truncated log before its self-exit)" }
    if ($joinResults.Count -eq 0) { $why += "join never reached SCENARIO RESULT (crash during churn or on the peer drop - the failure this hardening targets)" }

    # 2) The peer-drop leg actually exercised: the join must have observed the
    #    host leave (asymmetric self-exit closes the socket -> transport leave).
    $leftLines = @(Select-String -Path $JoinFile -Pattern 'handshake: peer left' -ErrorAction SilentlyContinue | ForEach-Object { $_.LineNumber })
    if ($leftLines.Count -eq 0) { $why += "join never saw the host drop (no 'handshake: peer left' - peer-drop leg did not exercise)" }

    # 3) B1 fired on the survivor. Report the MAX cleared count across all leave
    #    lines (a teardown-time second leave logs cleared=0 after the maps were
    #    already emptied by the real drop's clear).
    $cleared = -1
    $cm = @(Select-String -Path $JoinFile -Pattern '\[leave\] cleared proxies=(\d+)' -ErrorAction SilentlyContinue)
    if ($cm.Count -eq 0) { $why += "join never ran clearPeerReplicationState (no '[leave] cleared proxies=' after the drop)" }
    else { $cleared = ($cm | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Maximum).Maximum }

    # 4) Survivor did not crash AFTER the drop: a SCENARIO RESULT must appear
    #    LATER in the join log than the FIRST peer-left (the real host drop). The
    #    join self-exits at its scheduled window ~30 s after the drop, and only a
    #    surviving process reaches SCENARIO RESULT at all (a crash never logs it).
    #    NB: a teardown-time SECOND peer-left can follow the RESULT during the 4 s
    #    exit hold - so we anchor on the FIRST leave, not the last.
    if ($leftLines.Count -gt 0 -and $joinResults.Count -gt 0) {
        $firstLeft = ($leftLines | Measure-Object -Minimum).Minimum
        $afterLeave = @($joinResults | Where-Object { $_ -gt $firstLeft }).Count
        if ($afterLeave -eq 0) {
            $why += "join's SCENARIO RESULT precedes every peer-left - it did not survive the drop window"
        }
    }

    # Findings (not gates): B2 guard activity + mint churn volume.
    $stale = @(Select-String -Path $JoinFile -Pattern '\[drive\] STALE unbind' -ErrorAction SilentlyContinue).Count
    $bound = @(Select-String -Path $JoinFile -Pattern '\[spawn\] proxy BOUND ' -ErrorAction SilentlyContinue).Count
    Write-Host ("    FINDING: join proxyBOUND={0} staleUnbind={1} clearedOnLeave={2} peerLeftLines={3} hostResult={4}" -f `
        $bound, $stale, $cleared, $leftLines.Count, $hostResult)
    if ($bound -eq 0) { Write-Host "    ADVISORY: join minted no proxies (little mint churn this run - camp NPCs may be baked-resolved)" }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  CAMP-APPROACH $v - $detail"
    return (Add-GateResult -Name "camp_approach" -Status $v `
                -Metrics @{ bound = $bound; stale = $stale; cleared = $cleared } -Detail $detail)
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

