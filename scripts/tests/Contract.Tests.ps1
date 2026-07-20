<#
.SYNOPSIS
  Zero-game contract + drift fixtures for the KenshiCoop harness (Phase 0 safety
  net). Runs in milliseconds with NO game launch and NO built DLL - it only reads
  the manifest, the oracle library and the C++ scenario factory, so it can gate
  every commit and every later refactor phase.

.DESCRIPTION
  Asserts the harness contracts the refactor must not silently break:
    * manifest schema   - every scenario declares the required fields with sane
                          types / enum values
    * scenario drift    - the manifest scenario set and the C++ makeScenario
                          factory agree (a manifest name with no maker would
                          load nothing; a maker with no manifest entry would run
                          under the generic cross-check with no real gate)
    * oracle registry   - every oracle id referenced by the manifest resolves in
                          CoopOracles.psm1's dispatch, and an unknown id is
                          rejected (never silently passed)
    * verdict rule      - the no-signal guard (a SKIP of the PRIMARY gate fails
                          the run) and verdict.json serialization round-trip
  Also includes NEGATIVE fixtures that prove each checker actually fires on bad
  input (a malformed manifest entry, an unknown oracle id, a drifted scenario
  name, a primary-gate SKIP), so a green run means the guards work - not that
  they were skipped.

  Exit code = number of failed assertions (0 = PASS), matching prototest so
  verify.ps1 can sum them.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path      # scripts\tests
$scriptsRoot = Split-Path -Parent $scriptDir                       # scripts
$repoRoot  = Split-Path -Parent $scriptsRoot                       # repo root

Import-Module (Join-Path $scriptsRoot "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptsRoot "CoopHarness.psm1") -Force

# ---- tiny assert harness ------------------------------------------------------
$script:Pass = 0
$script:Fail = 0
function Check {
    param([string]$Name, [bool]$Cond)
    if ($Cond) { $script:Pass++; Write-Host "  ok   $Name" }
    else       { $script:Fail++; Write-Host "  FAIL $Name" }
}

# ---- shared inputs ------------------------------------------------------------
$manifest = Get-ScenarioManifest
$scenarios = $manifest.Scenarios

# The oracle registry = the dispatch case ids in CoopOracles.psm1's
# Invoke-OneOracle switch (Phase 0 stand-in for the Phase 2 registry hashtable).
function Get-OracleRegistry {
    $psm = Join-Path $scriptsRoot "CoopOracles.psm1"
    $ids = New-Object System.Collections.Generic.HashSet[string]
    foreach ($m in (Select-String -Path $psm -Pattern '^\s*"([a-z0-9_]+)"\s*\{\s*return')) {
        [void]$ids.Add($m.Matches[0].Groups[1].Value)
    }
    return $ids
}

# The set of scenario names the C++ factory can actually construct, parsed from
# the `if (name == "...")` makers in src/plugin/test/*.cpp.
function Get-CppScenarioNames {
    $names = New-Object System.Collections.Generic.HashSet[string]
    $files = Get-ChildItem -Path (Join-Path $repoRoot "src\plugin\test") -Filter *.cpp
    foreach ($f in $files) {
        foreach ($m in (Select-String -Path $f.FullName -Pattern 'name\s*==\s*"([^"]+)"')) {
            foreach ($mm in $m.Matches) { [void]$names.Add($mm.Groups[1].Value) }
        }
    }
    return $names
}

# Scenario names that exist in the C++ factory but INTENTIONALLY have no manifest
# entry (pure diagnostics driven outside the tiered matrix). Documented here so
# the drift check stays a hard gate for everything else. (xfer_block moved INTO
# the manifest in Phase 2 so it can carry its own DiagEnv instead of a Config
# name-check, so it is no longer allowlisted.)
$manifestlessCpp = @('world_item_drop')

# Return a list of schema problems for one scenario entry (empty = valid).
function Get-SchemaProblems {
    param([string]$Name, $Entry)
    $problems = @()
    $validTiers = @('smoke', 'full', 'probe', 'none')
    foreach ($k in @('Save', 'PrimaryGate', 'Gating', 'Advisory', 'Tier')) {
        if (-not $Entry.ContainsKey($k)) { $problems += "$Name missing '$k'" }
    }
    if ($Entry.ContainsKey('Tier') -and ($validTiers -notcontains $Entry.Tier)) {
        $problems += "$Name Tier '$($Entry.Tier)' not in {$($validTiers -join ',')}"
    }
    if ($Entry.ContainsKey('Gating')  -and $Entry.Gating  -isnot [array] -and $null -ne $Entry.Gating -and $Entry.Gating.Count -eq $null) {
        # a single-element @('x') is [object[]]; a bare string is the bug we guard
        if ($Entry.Gating -is [string]) { $problems += "$Name Gating is a bare string, not an array" }
    }
    if ($Entry.ContainsKey('PrimaryGate') -and $Entry.PrimaryGate -isnot [string]) {
        $problems += "$Name PrimaryGate is not a string"
    }
    return $problems
}

# ---- 1. manifest schema -------------------------------------------------------
Write-Host "== manifest schema =="
Check "manifest has Scenarios"   ($null -ne $scenarios -and $scenarios.Keys.Count -gt 0)
Check "manifest has Profiles"    ($null -ne $manifest.Profiles)
Check "manifest has WanProfiles" ($null -ne $manifest.WanProfiles)

$schemaProblems = @()
foreach ($name in $scenarios.Keys) {
    $schemaProblems += Get-SchemaProblems -Name $name -Entry $scenarios[$name]
}
if ($schemaProblems.Count -gt 0) { $schemaProblems | ForEach-Object { Write-Host "      $_" } }
Check "every scenario satisfies the schema" ($schemaProblems.Count -eq 0)

# NEGATIVE: a synthetic entry missing PrimaryGate must be flagged.
$badEntry = @{ Save = 'x'; Gating = @(); Advisory = @(); Tier = 'full' }
Check "schema checker flags a missing PrimaryGate" ((Get-SchemaProblems -Name 'bad' -Entry $badEntry).Count -gt 0)
# NEGATIVE: an invalid Tier must be flagged.
$badTier = @{ Save = 'x'; PrimaryGate = 'crosscheck'; Gating = @(); Advisory = @(); Tier = 'weekly' }
Check "schema checker flags an invalid Tier" ((Get-SchemaProblems -Name 'bad' -Entry $badTier).Count -gt 0)

# ---- 2. oracle registry -------------------------------------------------------
Write-Host "== oracle registry =="
$registry = Get-OracleRegistry
Check "oracle registry is non-empty" ($registry.Count -gt 0)
Check "registry contains a known oracle (crosscheck)" ($registry.Contains('crosscheck'))

$unknownRefs = @()
foreach ($name in $scenarios.Keys) {
    $e = $scenarios[$name]
    $ids = @()
    if ($e.PrimaryGate -ne "") { $ids += $e.PrimaryGate }
    $ids += @($e.Gating)
    $ids += @($e.Advisory)
    foreach ($id in $ids) {
        if ($id -ne "" -and -not $registry.Contains($id)) { $unknownRefs += "$name -> '$id'" }
    }
}
if ($unknownRefs.Count -gt 0) { $unknownRefs | ForEach-Object { Write-Host "      unknown oracle: $_" } }
Check "every manifest oracle id resolves in the registry" ($unknownRefs.Count -eq 0)

# NEGATIVE: Invoke-OneOracle must reject an unknown id (never silently pass).
$tmpH = [System.IO.Path]::GetTempFileName()
$tmpJ = [System.IO.Path]::GetTempFileName()
Reset-GateResults
$st = Invoke-OneOracle -Id 'definitely_not_an_oracle' -HostLog $tmpH -JoinLog $tmpJ -Tolerance 3.0 -ExpectedSkewMs $null
Check "unknown oracle id returns FAIL" ($st -eq 'FAIL')
$g = Get-GateResults | Where-Object { $_.gate -eq 'definitely_not_an_oracle' } | Select-Object -First 1
Check "unknown oracle id records 'unknown oracle id'" ($null -ne $g -and $g.detail -eq 'unknown oracle id')

# ---- 3. scenario drift (manifest <-> C++ factory) -----------------------------
Write-Host "== scenario name drift =="
$cppNames = Get-CppScenarioNames
Check "parsed C++ scenario names" ($cppNames.Count -gt 0)

# 3a. Every manifest scenario MUST be constructible by the factory.
$manifestNoMaker = @()
foreach ($name in $scenarios.Keys) {
    if (-not $cppNames.Contains($name)) { $manifestNoMaker += $name }
}
if ($manifestNoMaker.Count -gt 0) { Write-Host ("      manifest names with no C++ maker: " + ($manifestNoMaker -join ', ')) }
Check "every manifest scenario has a C++ maker" ($manifestNoMaker.Count -eq 0)

# 3b. Every C++ maker MUST have a manifest entry (or be an allowlisted diagnostic).
$cppNoManifest = @()
foreach ($n in $cppNames) {
    if (-not $scenarios.ContainsKey($n) -and ($manifestlessCpp -notcontains $n)) { $cppNoManifest += $n }
}
if ($cppNoManifest.Count -gt 0) { Write-Host ("      C++ makers with no manifest entry: " + ($cppNoManifest -join ', ')) }
Check "every C++ maker has a manifest entry (or is allowlisted)" ($cppNoManifest.Count -eq 0)

# NEGATIVE: a fabricated manifest name absent from the factory must be detected.
$fakeMissing = 'scenario_that_cannot_exist'
Check "drift checker detects a manifest name with no maker" (-not $cppNames.Contains($fakeMissing))

# ---- 4. verdict rule + serialization ------------------------------------------
Write-Host "== verdict rule + serialization =="
$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kc_contract_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$hostLog = Join-Path $tmpDir "host.log"
$joinLog = Join-Path $tmpDir "join.log"
# Clean logs: reached gameplay, clean scenario exit, >=3 CLOCKSYNC samples, but
# NO MEMBER/RECV series (so any position oracle legitimately SKIPs = no signal).
@(
    "[10:00:00.000] HOST KenshiCoop: gameplay started",
    "[10:00:02.000] HOST SCENARIO RESULT PASS"
) | Set-Content -Path $hostLog -Encoding UTF8
@(
    "[10:00:00.000] JOIN KenshiCoop: gameplay started",
    "[10:00:00.100] JOIN CLOCKSYNC offset=5 rtt=10 n=1",
    "[10:00:00.600] JOIN CLOCKSYNC offset=4 rtt=8 n=2",
    "[10:00:01.100] JOIN CLOCKSYNC offset=4 rtt=8 n=3",
    "[10:00:02.000] JOIN SCENARIO RESULT PASS"
) | Set-Content -Path $joinLog -Encoding UTF8

# 4a. POSITIVE: a diagnostic scenario with no primary/gating passes on clean logs
#     and serializes a round-trippable verdict.json.
$okJson = Join-Path $tmpDir "verdict_ok.json"
$vOk = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario 'spike' -OutJson $okJson 2>&1 |
       Select-Object -Last 1
# Invoke-RunAnalysis returns the verdict object as the last pipeline item.
$vOk = & {
    Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario 'spike' -OutJson $okJson | Out-Null
    Get-Content $okJson -Raw | ConvertFrom-Json
}
Check "clean no-gate scenario verdict.json written" (Test-Path $okJson)
Check "verdict.json round-trips (has pass/primary/gates)" ($null -ne $vOk -and $null -ne $vOk.gates -and $vOk.PSObject.Properties.Name -contains 'pass')
Check "clean no-gate scenario PASSES" ($vOk.pass -eq $true)

# 4b. NEGATIVE (no-signal guard): a scenario whose PRIMARY gate cannot be judged
#     on these logs must FAIL, citing the primary gate (as a FAIL or a SKIP -
#     either way the mechanism proof drives the verdict).
$skipJson = Join-Path $tmpDir "verdict_skip.json"
$vSkip = & {
    Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario 'leader_move' -OutJson $skipJson | Out-Null
    Get-Content $skipJson -Raw | ConvertFrom-Json
}
Check "primary-gate no-signal run FAILS" ($vSkip.pass -eq $false)
$primaryReason = @($vSkip.reasons) -join '; '
Check "failure reason names the primary gate" ($primaryReason -match 'crosscheck')

# 4c. unknown-scenario handling: does not throw, falls back to generic
#     cross-check, and (with no signal) fails rather than silently passing.
$unkJson = Join-Path $tmpDir "verdict_unknown.json"
$threw = $false
try {
    $vUnk = & {
        Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario 'no_such_scenario_xyz' -OutJson $unkJson | Out-Null
        Get-Content $unkJson -Raw | ConvertFrom-Json
    }
} catch { $threw = $true }
Check "unknown scenario does not throw" (-not $threw)
Check "unknown scenario falls back to crosscheck primary" ($vUnk.primary -eq 'crosscheck')

# ---- 5. manifest DiagEnv contract (Phase 2) -----------------------------------
# The per-scenario channel A/B knobs + log-only diagnostic traces moved OUT of
# the plugin's Config.cpp (which used to hard-code scenario names) and INTO the
# manifest DiagEnv, applied by CoopHarness. These fixtures guard that migration:
#   5a. every DiagEnv key is a known harness knob (no typos leak silently)
#   5b. the channel deltas Config.cpp used to encode are present in the manifest
#       (a deliberate two-source cross-check for the probe/inv/world scenarios
#       that the tiered matrix does not run live)
#   5c. Config.cpp no longer names scenarios for channel toggles (only the two
#       real-session `scenario == ""` defaults remain)
Write-Host "== manifest DiagEnv contract =="
$diagKeys = @(Get-CoopDiagEnvKeys)
Check "CoopHarness exposes a non-empty DiagEnv keyset" ($diagKeys.Count -gt 0)

$badDiag = @()
foreach ($name in $scenarios.Keys) {
    $e = $scenarios[$name]
    if (-not $e.ContainsKey('DiagEnv')) { continue }
    foreach ($k in $e.DiagEnv.Keys) {
        if ($diagKeys -notcontains $k) { $badDiag += "$name -> unknown key '$k'" }
        elseif ("$($e.DiagEnv[$k])" -notin @('0', '1')) { $badDiag += "$name -> '$k' value '$($e.DiagEnv[$k])' not 0/1" }
    }
}
if ($badDiag.Count -gt 0) { $badDiag | ForEach-Object { Write-Host "      $_" } }
Check "every DiagEnv key is known + valued 0/1" ($badDiag.Count -eq 0)

# 5b. Spec table = the exact channel deltas Config.cpp USED to hard-code. If this
#     drifts from the manifest, a probe would silently run with the wrong channel
#     state (its baseline invalidated). Kept here on purpose as the second source.
$diagSpec = @{
    speed_probe    = @{ KENSHICOOP_SPEED_SYNC = '0' }
    shop_probe     = @{ KENSHICOOP_MONEY_SYNC = '0' }
    spawn_probe    = @{ KENSHICOOP_SPAWN_SYNC = '0' }
    recruit_probe  = @{ KENSHICOOP_RECRUIT_SYNC = '0' }
    faction_probe  = @{ KENSHICOOP_FACTION_SYNC = '0' }
    time_probe     = @{ KENSHICOOP_TIME_SYNC = '0'; KENSHICOOP_SPEED_SYNC = '0' }
    door_probe     = @{ KENSHICOOP_DOOR_SYNC = '0' }
    build_probe    = @{ KENSHICOOP_BUILD_SYNC = '0' }
    bdoor_probe    = @{ KENSHICOOP_BDOOR_SYNC = '0' }
    hunger_probe   = @{ KENSHICOOP_HUNGER_SYNC = '0' }
    save_probe     = @{ KENSHICOOP_SAVE_SYNC = '0' }
    load_probe     = @{ KENSHICOOP_LOAD_SYNC = '0' }
    prod_probe     = @{ KENSHICOOP_PROD_SYNC = '0' }
    research_probe = @{ KENSHICOOP_RESEARCH_SYNC = '0' }
    store_probe    = @{ KENSHICOOP_STORE_SYNC = '0' }
    squad_probe    = @{ KENSHICOOP_SQUAD_SYNC = '0' }
    latejoin_probe = @{ KENSHICOOP_LATEJOIN_SYNC = '0' }
    speed_sync     = @{ KENSHICOOP_TIME_SYNC = '0' }
    trade_probe    = @{ KENSHICOOP_XFER_SYNC = '0'; KENSHICOOP_INV_SYNC = '1' }
    xfer_block     = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_BLOCK_XFER = '1' }
    inv_order      = @{ KENSHICOOP_INV_SYNC = '1' }
    inv_bidir      = @{ KENSHICOOP_INV_SYNC = '1' }
    inv_equip      = @{ KENSHICOOP_INV_SYNC = '1' }
    inv_reequip    = @{ KENSHICOOP_INV_SYNC = '1' }
    vendor_trade   = @{ KENSHICOOP_INV_SYNC = '1' }
    store_sync     = @{ KENSHICOOP_INV_SYNC = '1' }
    trade_peer     = @{ KENSHICOOP_INV_SYNC = '1' }
    weapon_loot    = @{ KENSHICOOP_INV_SYNC = '1' }
    world_weapon_drop = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1' }
    world_armor_drop  = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1' }
    world_item_sync = @{ KENSHICOOP_WORLD_SYNC = '1' }
    world_item_join = @{ KENSHICOOP_WORLD_SYNC = '1' }
    limb_loss       = @{ KENSHICOOP_WORLD_SYNC = '1' }
}
$specMiss = @()
foreach ($name in $diagSpec.Keys) {
    if (-not $scenarios.ContainsKey($name)) { $specMiss += "$name absent from manifest"; continue }
    $e = $scenarios[$name]
    $diag = if ($e.ContainsKey('DiagEnv')) { $e.DiagEnv } else { @{} }
    foreach ($k in $diagSpec[$name].Keys) {
        if ("$($diag[$k])" -ne "$($diagSpec[$name][$k])") {
            $specMiss += "$name -> $k expected '$($diagSpec[$name][$k])' got '$($diag[$k])'"
        }
    }
}
if ($specMiss.Count -gt 0) { $specMiss | ForEach-Object { Write-Host "      $_" } }
Check "manifest DiagEnv matches the Config channel spec" ($specMiss.Count -eq 0)

# 5c. Config.cpp must not reintroduce per-scenario channel names: the only
#     `c.scenario == "..."` allowed is the empty real-session default.
$configCpp = Join-Path $repoRoot "src\plugin\core\Config.cpp"
$scenarioNameHits = @()
foreach ($m in (Select-String -Path $configCpp -Pattern 'c\.scenario\s*==\s*"([^"]*)"')) {
    foreach ($mm in $m.Matches) {
        if ($mm.Groups[1].Value -ne "") { $scenarioNameHits += "line $($m.LineNumber): $($mm.Value)" }
    }
}
# scenario.compare(...) prefix-matching is likewise a hard-coded name test.
foreach ($m in (Select-String -Path $configCpp -Pattern 'scenario\.compare')) {
    $scenarioNameHits += "line $($m.LineNumber): scenario.compare(...)"
}
if ($scenarioNameHits.Count -gt 0) { $scenarioNameHits | ForEach-Object { Write-Host "      $_" } }
Check "Config.cpp has no per-scenario channel name-checks" ($scenarioNameHits.Count -eq 0)

# ---- Phase 5a: engine boundary dependency check -------------------------------
# The PUBLIC engine headers (the SEH-guarded facade Replicator/Scenario/Plugin
# include) must never pull a game-internal header (<kenshi/..>, <core/..>,
# <mygui/..>, <ogre/..>): those live ONLY in the adapter (EngineInternal.h) and
# the domain .cpp TUs. This is the "no direct Kenshi-internal include outside the
# approved adapter" barrier the domain split established - it keeps every public
# consumer compiling against pointers/PODs, not the engine ABI.
Write-Host "== engine boundary dependency check (Phase 5a) =="
$gameDir = Join-Path $repoRoot "src\plugin\game"
$publicEngineHeaders = @("Engine.h", "EngineSync.h", "EngineScenario.h",
                         "EngineProbe.h", "EngineUi.h")
$internalIncludeRe = '^\s*#\s*include\s*[<"](kenshi|core|mygui|ogre)/'
$leaks = @()
$missingHdr = @()
foreach ($h in $publicEngineHeaders) {
    $p = Join-Path $gameDir $h
    if (-not (Test-Path $p)) { $missingHdr += $h; continue }
    foreach ($m in (Select-String -Path $p -Pattern $internalIncludeRe)) {
        $leaks += "$h line $($m.LineNumber): $($m.Line.Trim())"
    }
}
if ($missingHdr.Count -gt 0) { $missingHdr | ForEach-Object { Write-Host "      missing $_" } }
Check "all narrow public engine headers exist" ($missingHdr.Count -eq 0)
if ($leaks.Count -gt 0) { $leaks | ForEach-Object { Write-Host "      $_" } }
Check "public engine headers pull no game-internal include" ($leaks.Count -eq 0)

# Positive control: the adapter EngineInternal.h SHOULD carry the game-internal
# prelude (otherwise the check above is passing vacuously against the wrong root).
$adapter = Join-Path $gameDir "EngineInternal.h"
$adapterHasInternal = (Test-Path $adapter) -and `
    ((Select-String -Path $adapter -Pattern $internalIncludeRe).Count -gt 0)
Check "adapter EngineInternal.h carries the game-internal prelude" $adapterHasInternal

# ---- cleanup ------------------------------------------------------------------
Remove-Item -Path $tmpH, $tmpJ -Force -ErrorAction SilentlyContinue
Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

# ---- summary ------------------------------------------------------------------
Write-Host ""
Write-Host ("contract fixtures: {0}/{1} checks passed{2}" -f `
    $script:Pass, ($script:Pass + $script:Fail), $(if ($script:Fail) { " - FAIL" } else { " - PASS" }))
exit $script:Fail
