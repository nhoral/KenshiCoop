<#
.SYNOPSIS
  Capture the Phase 0 refactor baseline: the frozen contract facts every later
  phase diffs against. Snapshots the game-INDEPENDENT half deterministically
  (protocol version, packet-size inventory, canonical DLL SHA-256, manifest hash,
  config source hash, commit/dirty), and records WHERE the game-DEPENDENT half
  (the two-client regression verdicts) is stored so the two together form the
  baseline artifact set the plan requires.

.DESCRIPTION
  Writes tools\baseline\baseline_<timestamp>.json and refreshes
  tools\baseline\latest.json. Run once at the start of the refactor (and re-run
  only when a delta is intentional and documented).

  The game-dependent verdicts are NOT launched here (they need Kenshi + Steam +
  the shared saves). Capture them separately and they land under
  tools\test-runs\ (regress.ps1 writes verdict.json per run + history.jsonl):

    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full
    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -Variants clean,wan
    # focused: save_sync, load_sync, latejoin_sync, rejoin_items, camp_approach
    powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Only save_sync,load_sync,latejoin_sync

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\capture_baseline.ps1
#>
[CmdletBinding()]
param(
    # Path to the canonical Release DLL to hash (defaults to the build output).
    [string]$Dll = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

function Get-Sha256 {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash
}

# ---- protocol version (from Wire.h) -------------------------------------------
$wire = Join-Path $repoRoot "src\netproto\Wire.h"
$proto = $null
$pm = Select-String -Path $wire -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' | Select-Object -First 1
if ($null -ne $pm) { $proto = [int]$pm.Matches[0].Groups[1].Value }

# ---- packet-size inventory + canonical hashes (from prototest) ----------------
$prototest = Join-Path $repoRoot "dist\prototest.exe"
$sizes = @{}
$protoFromTest = $null
$invEntryHash = $null
if (Test-Path $prototest) {
    $out = & $prototest 2>&1
    foreach ($line in $out) {
        if ("$line" -match 'ok\s+sizeof\(([A-Za-z0-9_]+)\)\s+\(=\s*(\d+)\)') {
            $sizes[$Matches[1]] = [int]$Matches[2]
        }
        if ("$line" -match 'PROTOCOL_VERSION.*\(=\s*(\d+)\)') { $protoFromTest = [int]$Matches[1] }
        if ("$line" -match 'canonical invEntryHash.*=\s*(\d+)') { $invEntryHash = [long]$Matches[1] }
    }
} else {
    Write-Host "WARNING: dist\prototest.exe not found - build it (scripts\verify.ps1) for the packet-size inventory."
}

# ---- DLL hashes ---------------------------------------------------------------
# Phase 1 build separation: the regression pipeline runs the Harness DLL (with
# the scenario runner); the shipped canonical is the Release DLL. Record whichever
# the caller points at as the primary $dllHash (default: the test/Harness DLL that
# the baseline verdicts actually ran against), plus the shipped Release hash when
# a Release build is present, so both are diffable later.
if ($Dll -eq "") {
    $harnessDll = Join-Path $repoRoot "src\plugin\x64\Harness\KenshiCoop.dll"
    $releaseDll = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
    if (Test-Path $harnessDll) { $Dll = $harnessDll } else { $Dll = $releaseDll }
}
$dllHash = Get-Sha256 -Path $Dll
$releaseDllPath = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
$shippedDllHash = if (Test-Path $releaseDllPath) { Get-Sha256 -Path $releaseDllPath } else { "" }

# ---- manifest + config source hashes ------------------------------------------
$manifestHash = Get-Sha256 -Path (Join-Path $repoRoot "scripts\scenarios.psd1")
$configHash = @{
    "Config.cpp" = Get-Sha256 -Path (Join-Path $repoRoot "src\plugin\core\Config.cpp")
    "Config.h"   = Get-Sha256 -Path (Join-Path $repoRoot "src\plugin\core\Config.h")
}

# ---- commit provenance --------------------------------------------------------
$commit = ""; $dirty = $true
try { $commit = (& git -C $repoRoot rev-parse HEAD).Trim() } catch {}
try { $dirty = ((& git -C $repoRoot status --porcelain) | Measure-Object).Count -gt 0 } catch {}

# ---- assemble + write ---------------------------------------------------------
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$baseline = [pscustomobject]@{
    capturedAt      = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    commit          = $commit
    dirty           = $dirty
    protocolVersion = $proto
    protocolFromTest = $protoFromTest
    packetSizes     = $sizes
    invEntryHash    = $invEntryHash
    canonicalDll    = @{ path = $Dll; sha256 = $dllHash }
    shippedDll      = @{ path = $releaseDllPath; sha256 = $shippedDllHash }
    manifestSha256  = $manifestHash
    configSha256    = $configHash
    gameVerdicts    = @{
        note   = "Two-client verdicts are NOT captured here (need the game). Run regress.ps1; they land under tools\test-runs\ (verdict.json per run + history.jsonl)."
        source = "tools\test-runs\history.jsonl"
    }
}

$outDir = Join-Path $repoRoot "tools\baseline"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir "baseline_$stamp.json"
$latest  = Join-Path $outDir "latest.json"
$json = $baseline | ConvertTo-Json -Depth 6
$json | Set-Content -Path $outFile -Encoding UTF8
$json | Set-Content -Path $latest  -Encoding UTF8

Write-Host "=== Phase 0 baseline captured ==="
Write-Host ("  protocol version : {0}" -f $proto)
Write-Host ("  packet types     : {0}" -f $sizes.Keys.Count)
Write-Host ("  canonical DLL    : {0}" -f $(if ($dllHash) { $dllHash.Substring(0, 16) + "..." } else { "MISSING (build first)" }))
Write-Host ("  manifest sha256  : {0}" -f $(if ($manifestHash) { $manifestHash.Substring(0, 16) + "..." } else { "n/a" }))
Write-Host ("  commit / dirty   : {0} / {1}" -f $(if ($commit) { $commit.Substring(0, 10) } else { "?" }), $dirty)
Write-Host ("  written          : {0}" -f $outFile)
Write-Host ("  latest           : {0}" -f $latest)
Write-Host ""
Write-Host "Next: capture the two-client verdicts with scripts\regress.ps1 (see the script header)."
