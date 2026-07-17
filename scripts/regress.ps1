<#
.SYNOPSIS
  Tiered regression suite for KenshiCoop: build + deploy once, run the unit
  layer, then the manifest-driven scenario matrix (smoke or full tier, with
  optional WAN / clock-skew variants), and print a single PASS/FAIL summary.
  Appends per-gate metrics to tools\test-runs\history.jsonl for trending.

.DESCRIPTION
  The matrix comes from scripts\scenarios.psd1 (the scenario manifest):
    * -Tier smoke : ONE scenario per wire pipeline (entity stream/presence,
      interest-managed NPCs, reliable events, inventory, world items) - the
      fast per-commit signal (~5 game runs).
    * -Tier full  : every scenario with Tier smoke|full, PLUS a WAN-proxy
      variant of each WanVariant scenario ('bad' profile via -Wan).
  Variants can be forced with -Variants (csv of clean|wan|skew|wanskew), which
  is how the re-validation matrix runs everything under all three regimes.

  Flake policy: a scenario that FAILs is retried ONCE; a retry-pass is recorded
  as FLAKY (counts as pass for the suite verdict but is flagged in the summary
  and in history.jsonl, so repeat flakes surface in report_history.ps1).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regress.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -SkipBuild

.EXAMPLE
  # The re-validation matrix: every tiered scenario under clean + WAN + WAN/skew.
  powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -Variants clean,wan,wanskew
#>
[CmdletBinding()]
param(
    [ValidateSet("smoke", "full")]
    [string]$Tier = "smoke",
    [int]$Port = 27800,
    # Reuse the currently deployed DLL (skip the build + deploy step).
    [switch]$SkipBuild,
    # Stop at the first scenario failure (after its retry).
    [switch]$FailFast,
    # Disable the retry-once flake policy.
    [switch]$NoRetry,
    # Force the variant list (csv of clean|wan|skew|wanskew). Default: 'clean'
    # for smoke; 'clean' + per-scenario WAN variants for full.
    [string]$Variants = "",
    # WAN profile for wan/wanskew variants (scenarios.psd1 WanProfiles).
    [string]$WanProfile = "bad",
    # Injected join clock skew (ms) for skew/wanskew variants.
    [int]$SkewMs = 30000,
    # Run only these scenarios (csv) from the tier - handy for triage.
    [string]$Only = "",
    # Two-machine LAN mode: run the matrix through run_lan_test.ps1 (machine 2
    # hosts via SSH-triggered scheduled task; this machine joins). Variants are
    # recorded as lan/lanwan/lanwanskew in history so LAN trends stay separate.
    [switch]$Lan
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest

# Preferred pipeline order (fundamental first); anything else appends alphabetically.
$preferredOrder = @(
    "leader_move", "coop_presence", "split_interest", "npc_sync", "craft_order",
    "down_order", "death_order", "combat_probe", "combat_order", "combat_kill",
    "carry_order", "npc_carry",
    "inv_order", "inv_bidir", "inv_equip", "inv_reequip", "inv_addequip",
    "world_item_sync", "wpn_relocate", "world_weapon_drop", "world_armor_drop"
)

# ---- 1. Select the scenario matrix -------------------------------------------
$tierNames = @()
foreach ($name in $manifest.Scenarios.Keys) {
    $e = $manifest.Scenarios[$name]
    if ($Tier -eq "smoke" -and $e.Tier -eq "smoke") { $tierNames += $name }
    elseif ($Tier -eq "full" -and ($e.Tier -eq "smoke" -or $e.Tier -eq "full")) { $tierNames += $name }
}
$ordered = @()
foreach ($n in $preferredOrder) { if ($tierNames -contains $n) { $ordered += $n } }
foreach ($n in ($tierNames | Sort-Object)) { if ($ordered -notcontains $n) { $ordered += $n } }
if ($Only -ne "") {
    $onlySet = $Only.Split(',') | ForEach-Object { $_.Trim() }
    $ordered = @($ordered | Where-Object { $onlySet -contains $_ })
}

# Expand into (scenario, variant) run entries.
$forcedVariants = @()
if ($Variants -ne "") { $forcedVariants = $Variants.Split(',') | ForEach-Object { $_.Trim().ToLower() } }
$runsPlanned = @()
foreach ($name in $ordered) {
    $e = $manifest.Scenarios[$name]
    if ($forcedVariants.Count -gt 0) {
        foreach ($v in $forcedVariants) {
            # skew-only and wan variants respect the scenario's WanVariant flag when
            # forced with 'wan'/'wanskew'; 'clean'/'skew' always apply.
            if (($v -eq "wan" -or $v -eq "wanskew") -and -not $e.WanVariant) { continue }
            $runsPlanned += @{ scenario = $name; variant = $v }
        }
    } else {
        $runsPlanned += @{ scenario = $name; variant = "clean" }
        if ($Tier -eq "full" -and $e.WanVariant) {
            $runsPlanned += @{ scenario = $name; variant = "wan" }
        }
    }
}

Write-Host "=== KenshiCoop regression: tier=$Tier ($($runsPlanned.Count) run(s)) ==="
foreach ($r in $runsPlanned) { Write-Host ("  - {0} [{1}]" -f $r.scenario, $r.variant) }

# ---- 2. Build + deploy once ----------------------------------------------------
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== kill stale Kenshi ==="
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

    Write-Host "=== build plugin ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (build failed, exit $LASTEXITCODE)"; exit 1 }

    Write-Host "=== deploy ==="
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (deploy failed, exit $LASTEXITCODE)"; exit 1 }

    Write-Host "=== build prototest (unit layer) ==="
    & cmd.exe /c "`"$scriptDir\build_prototest.cmd`""
    if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (prototest build failed, exit $LASTEXITCODE)"; exit 1 }

    $needWan = @($runsPlanned | Where-Object { $_.variant -eq "wan" -or $_.variant -eq "wanskew" }).Count -gt 0
    if ($needWan) {
        Write-Host "=== build netsim (WAN relay) ==="
        & cmd.exe /c "`"$scriptDir\build_netsim.cmd`""
        if ($LASTEXITCODE -ne 0) { Write-Host "RESULT: FAIL (netsim build failed, exit $LASTEXITCODE)"; exit 1 }
    }
} else {
    Write-Host "=== skip build (reusing deployed DLL + dist binaries) ==="
}

# ---- 3. Unit layer (step 0: fails in milliseconds, before any game launch) -----
$prototest = Join-Path $repoRoot "dist\prototest.exe"
$unitOk = $true
Write-Host ""
Write-Host "############################################################"
Write-Host "# UNIT LAYER: prototest (protocol round-trip / fuzz / hash)"
Write-Host "############################################################"
if (Test-Path $prototest) {
    & $prototest
    $unitOk = ($LASTEXITCODE -eq 0)
    Write-Host ("UNIT LAYER: " + $(if ($unitOk) { "PASS" } else { "FAIL (exit $LASTEXITCODE)" }))
    if (-not $unitOk -and $FailFast) { Write-Host "OVERALL: FAIL (unit layer)"; exit 1 }
} else {
    Write-Host "UNIT LAYER: SKIP - dist\prototest.exe not built (cmd /c scripts\build_prototest.cmd)"
}

# ---- 4. Run the matrix ----------------------------------------------------------
$historyFile = Join-Path $repoRoot "tools\test-runs\history.jsonl"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $historyFile) | Out-Null

function Invoke-OneRun {
    param([string]$Scenario, [string]$Variant)
    $runner = if ($Lan) { "run_lan_test.ps1" } else { "run_test.ps1" }
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass",
              "-File", (Join-Path $scriptDir $runner),
              "-Scenario", $Scenario, "-Port", "$Port")
    if ($Lan) { $args += "-SkipBuild" }   # regress already built+deployed; the DLL push still happens per run
    if ($Variant -eq "wan"     -or $Variant -eq "wanskew") { $args += @("-Wan", $WanProfile) }
    if ($Variant -eq "skew"    -or $Variant -eq "wanskew") { $args += @("-FakeClockSkewMs", "$SkewMs") }
    # Resilience: a child run_test.ps1 can emit to stderr (e.g. a transient
    # "No visible window found" from the screenshot/arrange helpers when a game
    # window is mid-launch). Under the suite's -ErrorActionPreference Stop that
    # NativeCommandError would ABORT the entire matrix. Isolate the native call
    # to Continue + try/catch so one flaky run fails ITSELF (and gets the retry
    # policy) instead of killing the whole regression.
    $out = @()
    $exit = 1
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & powershell @args 2>&1
        $exit = $LASTEXITCODE
    } catch {
        $out = @("run_test invocation threw: $($_.Exception.Message)")
        $exit = 1
    } finally {
        $ErrorActionPreference = $prevEap
    }
    $out | ForEach-Object { Write-Host $_ }
    $outDir = ""
    foreach ($line in $out) {
        if ("$line" -match "out dir:\s+(.+)$") { $outDir = $Matches[1].Trim() }
    }
    return @{ exit = $exit; outDir = $outDir }
}

$results = @()
$allPass = $true
foreach ($r in $runsPlanned) {
    $scenario = $r.scenario; $variant = $r.variant
    Write-Host ""
    Write-Host "############################################################"
    Write-Host "# REGRESS: $scenario [$variant]"
    Write-Host "############################################################"

    $attempt = Invoke-OneRun -Scenario $scenario -Variant $variant
    $pass = ($attempt.exit -eq 0)
    $flaky = $false
    if (-not $pass -and -not $NoRetry) {
        Write-Host ""
        Write-Host "-- $scenario [$variant] FAILED; retrying once (flake policy) --"
        $retry = Invoke-OneRun -Scenario $scenario -Variant $variant
        if ($retry.exit -eq 0) { $pass = $true; $flaky = $true; $attempt = $retry }
    }

    # Pull the verdict.json for the gate table + history.
    $verdict = $null
    $vPath = if ($attempt.outDir -ne "") { Join-Path $attempt.outDir "verdict.json" } else { "" }
    if ($vPath -ne "" -and (Test-Path $vPath)) {
        try { $verdict = Get-Content $vPath -Raw | ConvertFrom-Json } catch {}
    }

    # LAN runs get distinct variant labels (lan/lanwan/lanwanskew) so their
    # trends never mix with loopback history for the same scenario.
    $variantLabel = if ($Lan) { "lan" + $(if ($variant -eq "clean") { "" } else { $variant }) } else { $variant }

    $results += @{
        scenario = $scenario; variant = $variantLabel
        pass = $pass; flaky = $flaky; outDir = $attempt.outDir; verdict = $verdict
    }
    if (-not $pass) { $allPass = $false }

    # Append to history.jsonl (one line per scenario-run, gates + metrics included).
    $hist = [pscustomobject]@{
        timestamp = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
        tier      = $Tier
        scenario  = $scenario
        variant   = $variantLabel
        pass      = $pass
        flaky     = $flaky
        outDir    = $attempt.outDir
        gates     = $(if ($null -ne $verdict) { $verdict.gates } else { @() })
    }
    Add-Content -Path $historyFile -Value ($hist | ConvertTo-Json -Depth 6 -Compress)

    if (-not $pass -and $FailFast) { break }
}

# ---- 5. Single summary -----------------------------------------------------------
Write-Host ""
Write-Host "================= REGRESSION SUMMARY (tier=$Tier) ================="
if (-not $unitOk) { Write-Host "  [FAIL] prototest (unit layer)"; $allPass = $false }
elseif (Test-Path $prototest) { Write-Host "  [PASS] prototest (unit layer)" }
foreach ($r in $results) {
    $v = if ($r.pass -and $r.flaky) { "FLAKY" } elseif ($r.pass) { "PASS" } else { "FAIL" }
    Write-Host ""
    Write-Host ("  [{0}] {1} [{2}]" -f $v, $r.scenario, $r.variant)
    if ($r.outDir -ne "") { Write-Host ("      logs: {0}" -f $r.outDir) }
    if ($null -ne $r.verdict) {
        foreach ($g in $r.verdict.gates) {
            $tag = ""
            if ($r.verdict.advisory -contains $g.gate) { $tag = " (advisory)" }
            elseif ($g.gate -eq $r.verdict.primary)    { $tag = " (primary)" }
            Write-Host ("      {0,-14} {1}{2}" -f $g.gate, $g.status, $tag)
        }
        if (-not $r.pass -and $r.verdict.reasons) {
            Write-Host ("      reasons: " + ($r.verdict.reasons -join "; "))
        }
    }
}
Write-Host ""
Write-Host ("history: $historyFile")
Write-Host ("OVERALL: " + $(if ($allPass) { "PASS" } else { "FAIL" }))
if ($allPass) { exit 0 } else { exit 1 }
