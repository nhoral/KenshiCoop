<#
.SYNOPSIS
  One-time provisioning of the LAN host machine (machine 2) for remote-host
  validation runs: push the mod + fixture saves + runner scripts, open the UDP
  firewall port, and register the KenshiCoopHost scheduled task that launches
  the game in the interactive session.

.DESCRIPTION
  Connection settings come from scripts\lan.config.json (copy the .example).
  SSH key auth to the machine must already work (same setup as the VideoEditor
  GPU-PC flow: OpenSSH server + administrators_authorized_keys).

  Prerequisites on the LAN machine (verified where possible):
    * Kenshi installed (kenshiDir), RE_Kenshi 0.3.1+ installed
    * Windowed mode (Options > Video > Full Screen off) - screenshots + window
      management need it
    * A user logged into the desktop (the scheduled task runs interactively)
    * STEAM ACCOUNT NOTE: with one Steam account on both machines, put ONE
      client in OFFLINE mode (validated 2026-07-05: concurrent play then works
      and neither session is signed out).

  Safe to re-run (idempotent): re-pushes artifacts, re-registers the task.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup_lan_host.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\setup_lan_host.ps1 -Saves squad1,sync,duel1
#>
[CmdletBinding()]
param(
    # Fixture saves to push (defaults to every save the tiered manifest uses).
    [string[]]$Saves = @(),
    [int]$Port = 27800,
    [switch]$SkipSaves
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force

# ---- Config -------------------------------------------------------------------
$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (copy lan.config.example.json and fill in)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"
$dropFwd = $cfg.dropDir -replace '\\', '/'

function Invoke-LanSsh {
    param([string]$Cmd)
    # PowerShell 5.1 does NOT escape embedded double quotes when building the
    # native command line, so a remote cmd like: if exist "C:\Program Files..."
    # arrives with its quotes stripped and breaks on the space. Escape them.
    # Also tolerate stderr noise: under ErrorActionPreference=Stop a mere ssh
    # WARNING on stderr becomes a terminating NativeCommandError via 2>&1.
    $escaped = $Cmd -replace '"', '\"'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

Write-Host "== LAN host provisioning: $target =="

# ---- 1. Connectivity + prerequisites ----------------------------------------------
$r = Invoke-LanSsh "echo SSH-OK && hostname"
if ($r.exit -ne 0 -or "$($r.out)" -notmatch "SSH-OK") { throw "SSH to $target failed: $($r.out)" }
Write-Host "  ssh: OK ($((@($r.out) | Select-Object -Last 1)))"

$r = Invoke-LanSsh "if exist `"$($cfg.kenshiDir)\kenshi_x64.exe`" (echo KENSHI-OK) else (echo KENSHI-MISSING)"
if ("$($r.out)" -notmatch "KENSHI-OK") { throw "Kenshi not found at $($cfg.kenshiDir) on $($cfg.host)" }
Write-Host "  kenshi: OK"

# RE_Kenshi presence (best-effort: its loader DLL in the install root).
$r = Invoke-LanSsh "if exist `"$($cfg.kenshiDir)\RE_Kenshi*`" (echo REK-OK) else (if exist `"$($cfg.kenshiDir)\dinput8.dll`" (echo REK-OK) else (echo REK-UNKNOWN))"
Write-Host ("  re_kenshi: " + $(if ("$($r.out)" -match "REK-OK") { "detected" } else { "NOT detected - verify manually (Nexus mod 847)" }))

# ---- 2. Drop dir + runner scripts ---------------------------------------------------
[void](Invoke-LanSsh "mkdir `"$($cfg.dropDir)`" 2>nul & mkdir `"$($cfg.dropDir)\results`" 2>nul & echo DIR-OK")
& scp -o BatchMode=yes `
    (Join-Path $scriptDir "lan_host_run.ps1") `
    (Join-Path $scriptDir "start_kenshi.ps1") `
    (Join-Path $scriptDir "screenshot.ps1") `
    "${target}:$dropFwd/"
if ($LASTEXITCODE -ne 0) { throw "scp of runner scripts failed" }
Write-Host "  runner scripts: pushed -> $($cfg.dropDir)"

# ---- 3. Mod deploy --------------------------------------------------------------------
$dll  = Join-Path $repoRoot "src\plugin\x64\Harness\KenshiCoop.dll"
$json = Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"
$mod  = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\KenshiCoop\KenshiCoop.mod"
if (-not (Test-Path $dll)) { throw "Build the plugin first (scripts\build_plugin.cmd): $dll" }
$modDirFwd = "$($cfg.kenshiDir -replace '\\', '/')/mods/KenshiCoop"
[void](Invoke-LanSsh "mkdir `"$($cfg.kenshiDir)\mods\KenshiCoop`" 2>nul & echo OK")
$push = @($dll, $json)
if (Test-Path $mod) { $push += $mod }
& scp -o BatchMode=yes @push "${target}:$modDirFwd/"
if ($LASTEXITCODE -ne 0) { throw "scp of mod failed (is Kenshi running on $($cfg.host)?)" }
Write-Host "  mod: pushed -> $($cfg.kenshiDir)\mods\KenshiCoop"

# ---- 4. Fixture saves -------------------------------------------------------------------
if (-not $SkipSaves) {
    if ($Saves.Count -eq 0) {
        # Every save the tiered manifest references.
        $manifest = Get-ScenarioManifest
        $Saves = @($manifest.Scenarios.Keys | ForEach-Object { $manifest.Scenarios[$_] } |
                   Where-Object { $_.Tier -ne "none" } | ForEach-Object { $_.Save } | Sort-Object -Unique)
    }
    $saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
    foreach ($s in $Saves) {
        $src = Join-Path $saveRoot $s
        if (-not (Test-Path $src)) { Write-Warning "save '$s' not found locally; skipped"; continue }
        # Push to BOTH candidate save locations (per-user + install), mirroring
        # friend_join.ps1 - which one Kenshi reads depends on its settings.cfg.
        foreach ($remoteBase in @("%LOCALAPPDATA%\kenshi\save", "$($cfg.kenshiDir)\save")) {
            $r = Invoke-LanSsh "mkdir `"$remoteBase\$s`" 2>nul & echo OK"
        }
        $r = Invoke-LanSsh "echo %LOCALAPPDATA%"
        $remoteLocal = ("" + (@($r.out) | Select-Object -Last 1)).Trim()
        foreach ($destBase in @("$remoteLocal\kenshi\save", "$($cfg.kenshiDir)\save")) {
            $destFwd = ($destBase -replace '\\', '/')
            & scp -o BatchMode=yes -r "$src" "${target}:$destFwd/"
            if ($LASTEXITCODE -ne 0) { Write-Warning "scp of save '$s' to $destBase failed" }
        }
        Write-Host "  save '$s': pushed"
    }
}

# ---- 5. Firewall rule ---------------------------------------------------------------------
$r = Invoke-LanSsh "netsh advfirewall firewall show rule name=KenshiCoop >nul 2>&1 && echo RULE-EXISTS || (netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$Port && echo RULE-ADDED)"
if ("$($r.out)" -match "RULE-(EXISTS|ADDED)") { Write-Host "  firewall: UDP $Port allowed ($($Matches[1]))" }
else { Write-Warning "firewall rule could not be verified/added: $($r.out) (admin SSH required)" }

# ---- 6. Scheduled task -----------------------------------------------------------------------
$taskCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File $($cfg.dropDir)\lan_host_run.ps1"
$r = Invoke-LanSsh "schtasks /create /tn KenshiCoopHost /tr `"$taskCmd`" /sc once /st 00:00 /it /f"
if ($r.exit -ne 0) { throw "scheduled task registration failed: $($r.out)" }
Write-Host "  scheduled task: KenshiCoopHost registered (interactive)"

Write-Host ""
Write-Host "== Provisioning complete =="
Write-Host "Reminders for $($cfg.host):"
Write-Host "  * Keep a user logged into the desktop (the task launches interactively)."
Write-Host "  * Windowed mode must be on (Full Screen=No)."
Write-Host "  * Same Steam account on both machines: keep ONE client in OFFLINE mode."
Write-Host "  * Enable the KenshiCoop mod once in Kenshi's Mods tab if this is the first install."
Write-Host ""
Write-Host "Next: powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario coop_presence"
