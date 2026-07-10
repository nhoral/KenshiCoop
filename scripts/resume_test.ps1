<#
.SYNOPSIS
  Two-stage coordinated-save + session-resume proof (protocol 31 phase 12c).

.DESCRIPTION
  Stage 1 (scenario save_stage1): a normal co-op session where the HOST places
  a construction site and ramps it PART-way (session-runtime state that exists
  in NO baked save), then issues one coordinated save 'coopresume'. The
  Test-SaveSync oracle gates the full round trip: detour edge -> folder
  quiescence -> paced in-band folder transfer -> the join's staged,
  CRC-verified, atomically-committed copy -> ACK.

  Stage 2 (scenario resume_check): both clients RELAUNCH loading 'coopresume'
  - the save the stage-1 transfer delivered. Deliberately NO -Sync: the join
  loads what the TRANSFER wrote, not a harness mirror. The Test-SaveResume
  oracle gates that the stage-1 building enumerates on BOTH clients under the
  SAME save-stable hand with matching progress - the identity-reset claim
  (one save, one hand, both sides), proven.

  NOTE (loopback): host and join installs share %LOCALAPPDATA%\kenshi\save
  (User save location=1 in both settings.cfg), so on one machine the join's
  commit rewrites the folder the host just wrote - byte-identical, and the
  staging/CRC/commit path is still fully exercised. Two-machine runs (the
  remote kit) are where the transfer delivers to a genuinely separate disk.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\resume_test.ps1
#>
[CmdletBinding()]
param(
    [string]$OutDir = "",
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    # Seconds to let the freshly-committed save settle on disk between stages.
    [int]$InterStageSec = 5,
    [switch]$SkipStage1
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\resume_$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage1Dir = Join-Path $OutDir "stage1"
$stage2Dir = Join-Path $OutDir "stage2"

$runTest  = Join-Path $scriptDir "run_test.ps1"
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
$common = @(
    "-Port", "$Port",
    "-HostDir", $HostDir,
    "-JoinDir", $JoinDir
)

Write-Host "== Coordinated save + resume proof (protocol 31) =="
Write-Host "  out dir: $OutDir"
Write-Host ""

# ---- Stage 1: bake session state + coordinated save + transfer -------------------
$stage1Pass = $true
if (-not $SkipStage1) {
    Write-Host "== STAGE 1: save_stage1 (build -> coordinated save -> in-band transfer) =="
    & powershell -NoProfile -ExecutionPolicy Bypass -File $runTest `
        -Scenario save_stage1 -OutDir $stage1Dir @common
    $stage1Pass = ($LASTEXITCODE -eq 0)
    Write-Host ""
    Write-Host ("STAGE 1: " + $(if ($stage1Pass) { "PASS" } else { "FAIL" }))
    if (-not $stage1Pass) {
        Write-Host "Stage 1 failed; not running stage 2 (nothing trustworthy to resume)."
        Write-Host "RESUME-TEST RESULT: FAIL"
        exit 1
    }
} else {
    Write-Host "== STAGE 1 skipped (-SkipStage1): resuming the existing 'coopresume' =="
}

# The save the resume rides on must exist before relaunching (run_test's own
# fail-fast would also catch it, but this names the actual failure).
if (-not (Test-Path (Join-Path $saveRoot "coopresume"))) {
    Write-Host "ERROR: '$saveRoot\coopresume' does not exist after stage 1."
    Write-Host "RESUME-TEST RESULT: FAIL"
    exit 1
}

Start-Sleep -Seconds $InterStageSec

# ---- Stage 2: relaunch both on the transferred save + same-hand gate --------------
Write-Host ""
Write-Host "== STAGE 2: resume_check (both relaunch on 'coopresume', same-hand gate) =="
& powershell -NoProfile -ExecutionPolicy Bypass -File $runTest `
    -Scenario resume_check -OutDir $stage2Dir @common
$stage2Pass = ($LASTEXITCODE -eq 0)

Write-Host ""
Write-Host "== Resume-proof summary =="
Write-Host ("  stage 1 (save + transfer):  " + $(if ($SkipStage1) { "SKIPPED" } elseif ($stage1Pass) { "PASS" } else { "FAIL" }))
Write-Host ("  stage 2 (same-hand resume): " + $(if ($stage2Pass) { "PASS" } else { "FAIL" }))
Write-Host "  stage 1 verdict: $stage1Dir\verdict.json"
Write-Host "  stage 2 verdict: $stage2Dir\verdict.json"

$pass = $stage1Pass -and $stage2Pass
Write-Host ""
Write-Host ("RESUME-TEST RESULT: " + $(if ($pass) { "PASS" } else { "FAIL" }))
if ($pass) { exit 0 } else { exit 1 }
