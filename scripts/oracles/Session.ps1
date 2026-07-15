# oracles/Session.ps1 - session-lifecycle oracles (monolith split of
# CoopOracles.psm1, 2026-07-12): latejoin (Get-LatejoinCensus/Mutations/
# HealLatency, Test-LatejoinProbe/Sync), save/load (Test-SaveProbe,
# Test-LoadProbe, Test-SaveSync, Test-SaveResume, Test-LoadSync).
# Dot-sourced by CoopOracles.psm1 (module scope).
# Must NOT: change gate names or the LJ*/SAVE/LOAD marker regexes -
# they are the C++ log contract (resources/CODE_MAP.md).
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

# connect_bootstrap (push-save-on-connect): the seamless-join proof. The HOST
# is in a game; a JOIN with no matching save goes ONLINE. On the connect edge
# (or the host's gameplay-start edge if the join beat it) the host BAKES its
# live world and announces it with a LOAD_GO. The join - which may still be at
# the main menu - either loads it directly (identical on-disk copy) or NACKs,
# pulls the folder over the existing fallback transfer, and loads after commit.
# Gated on both logs:
#   1. host: connect-push armed ([boot] baking save '<name>' ...);
#   2. host: the freshly-baked save announced ([boot] GO->join ...);
#   3. join: it ENTERED the host's world - either a direct MATCH load
#      ([load] GO ... MATCH -> loading) or a post-transfer load
#      ([load] transfer committed -> loading '<name>').
# Findings (not gated): whether the join was MISSING/DIVERGED (needed the
# transfer) and, if so, the fallback-transfer edge on the host.
function Test-ConnectBootstrap {
    param([string]$HostFile, [string]$JoinFile)
    $why = @()

    # 1. the host armed a connect-push.
    $bake = Select-String -Path $HostFile -Pattern "\[boot\] baking save '([^']*)' to push to join on connect" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $pushName = ''
    if ($null -eq $bake) { $why += "host never armed a connect-push ([boot] baking save)" }
    else { $pushName = $bake.Matches[0].Groups[1].Value }

    # 2. the host announced the baked save.
    $go = Select-String -Path $HostFile -Pattern "\[boot\] GO->join id=(\d+) name='([^']*)' fp=([0-9a-fA-F]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $go) { $why += "host never announced the baked save ([boot] GO->join)" }
    else { Write-Host "    FINDING: host pushed save '$($go.Matches[0].Groups[2].Value)' fp=$($go.Matches[0].Groups[3].Value) on connect" }

    # 3. the join entered the host's world (direct MATCH or post-transfer load).
    $matchLoad = Select-String -Path $JoinFile -Pattern "\[load\] GO id=\d+ name='([^']*)' fp=[0-9a-fA-F]+ MATCH -> loading" -ErrorAction SilentlyContinue | Select-Object -Last 1
    $xferLoad  = Select-String -Path $JoinFile -Pattern "\[load\] transfer committed -> loading '([^']*)'" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($null -eq $matchLoad -and $null -eq $xferLoad) {
        $why += "join never loaded the pushed save (no MATCH load, no post-transfer load)"
    }
    elseif ($null -ne $xferLoad) {
        Write-Host "    FINDING: join needed the folder - loaded '$($xferLoad.Matches[0].Groups[1].Value)' after the fallback transfer"
        # The MISSING/DIVERGED NACK that drove the transfer, plus the host edge.
        $nack = Select-String -Path $JoinFile -Pattern "\[load\] GO id=\d+ name='[^']*' hostFp=[0-9a-fA-F]+ localFp=[0-9a-fA-F]+ (MISSING|DIVERGED) -> NACK" -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -ne $nack) { Write-Host "    FINDING: join copy was $($nack.Matches[0].Groups[1].Value) before the push (the seamless-join case)" }
        $fb = Select-String -Path $HostFile -Pattern "\[load\] starting fallback transfer name='([^']*)'" -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($null -eq $fb) { $why += "join NACKed but the host never started the fallback transfer" }
    }
    else {
        Write-Host "    FINDING: join already had an identical copy - direct MATCH load of '$($matchLoad.Matches[0].Groups[1].Value)'"
    }

    $v = if ($why.Count -eq 0) { "PASS" } else { "FAIL" }
    $detail = $why -join "; "
    Write-Host "  CONNECT-BOOTSTRAP $v - pushName='$pushName' $detail"
    return (Add-GateResult -Name "connect_bootstrap" -Status $v -Detail $detail)
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

