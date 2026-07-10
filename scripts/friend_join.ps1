<#
.SYNOPSIS
  The friend-side runner for a KenshiCoop remote test session. Ships INSIDE the
  remote kit (scripts\make_remote_kit.ps1); runs from the unzipped kit folder.

.DESCRIPTION
  One command:
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <host public IP>

  Steam P2P (no IPs, no port forwarding - the host tells you their SteamID):
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their steamid64>
  You must ALSO tell the host YOUR SteamID (Steam > profile > Account details,
  or steamid.io); they pass it as -PeerSteamId on their side.

  Installs the bundled mod + save into the local Kenshi, launches the game as
  the JOIN client pointed at the host, lets the agreed scenario run to its own
  self-exit (remote profile: generous timeouts), captures screenshots at the
  scenario anchor, and bundles log + screenshots + a small system report into
  KenshiCoop-results-<stamp>.zip for sending back.

  RESUMING a previous session (-Resume): after the first session, every save
  the HOST makes is streamed to you in-band (coordinated save, protocol 31)
  and committed as 'coopresume' in your save folder - no manual save copy
  needed ever again. -Resume skips the kit save install and loads that
  transferred save; the host resumes with the same name on their side:
    powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <ip> -Resume

  Judged later on the host side by scripts\analyze_run.ps1 - clock alignment
  between the two machines comes from the plugin's wire time-sync (CLOCKSYNC),
  so the friend's wall clock does NOT need to match the host's.
#>
[CmdletBinding()]
param(
    [string]$HostIp = "",
    # Steam P2P transport: the HOST's steamid64 (or short friend code). No IPs or
    # port forwarding needed; Steam brokers the connection. The host needs YOUR
    # SteamID on their side too.
    [string]$HostSteamId = "",
    # Kenshi install dir (auto-detected from the default Steam path when empty).
    [string]$KenshiDir = "",
    # Override the kit's default scenario ("" = kit default; "free" = no scenario,
    # normal co-op play with no self-exit).
    [string]$Scenario = "",
    [int]$Port = 0,
    # How long a free-play session runs before self-exit (0 = never).
    [int]$FreePlayMinutes = 0,
    # Resume the previous session: load the 'coopresume' save the host's
    # coordinated save streamed to this machine (protocol 31) instead of
    # installing the kit's baked save. Defaults to free play (no scenario).
    [switch]$Resume,
    # Save name the coordinated-save flow writes (must match the host's).
    [string]$ResumeSave = "coopresume"
)

$ErrorActionPreference = "Stop"
$kitDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Short Steam friend codes are steamid64 minus the account-universe base.
function ConvertTo-SteamId64([string]$id) {
    $id = "$id".Trim()
    if ($id -notmatch '^\d{1,17}$') {
        throw ("'$id' is not a Steam ID. Use the other player's friend code (Steam > " +
               "Friends > Add a Friend - the number at the top) or their 17-digit " +
               "SteamID64 (steamid.io converts profile URLs).")
    }
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}

# Preflight helpers (RE_Kenshi / Steam / Mark-of-the-Web / windowed checks).
. (Join-Path $kitDir "kit_preflight.ps1")
Invoke-KitUnblock -KitDir $kitDir

# ---- Kit defaults --------------------------------------------------------------
$kit = Get-Content (Join-Path $kitDir "kit.json") -Raw | ConvertFrom-Json
$save = $kit.save
if ($Resume) {
    # The save the host's coordinated save (protocol 31) streamed here last
    # session; resume = both sides load the identical file. Free play unless
    # a scenario is asked for explicitly.
    $save = $ResumeSave
    if ($Scenario -eq "") { $Scenario = "free" }
}
if ($Scenario -eq "") { $Scenario = $kit.scenario }
if ($Scenario -eq "free") { $Scenario = "" }
if ($Port -eq 0) { $Port = [int]$kit.port }
# The kit can pre-bake the Steam transport (kit.json: transport + hostSteamId).
if ($HostSteamId -eq "" -and $kit.PSObject.Properties["hostSteamId"] -and "$($kit.hostSteamId)" -ne "" -and "$($kit.hostSteamId)" -ne "0") {
    $HostSteamId = "$($kit.hostSteamId)"
}
$useSteam = ($HostSteamId -ne "")
# Print YOUR friend code first (read from Steam) so the two of you can swap
# codes right off this screen - the host needs YOURS as much as you need
# theirs. Shown even if the launch then stops for a missing host id.
if ($useSteam -or "$($kit.transport)" -eq "steam") { Show-MySteamId }
if (-not $useSteam -and $HostIp -eq "") { throw "Pass -HostIp <ip> (UDP) or -HostSteamId <steamid64> (Steam P2P)." }
# Validate the ID up front (clear error now beats a dead session later).
if ($useSteam) { [void](ConvertTo-SteamId64 $HostSteamId) }

# ---- Locate Kenshi ---------------------------------------------------------------
if ($KenshiDir -eq "") {
    foreach ($cand in @(
        "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
        "C:\Program Files\Steam\steamapps\common\Kenshi")) {
        if (Test-Path (Join-Path $cand "kenshi_x64.exe")) { $KenshiDir = $cand; break }
    }
}
if ($KenshiDir -eq "" -or -not (Test-Path (Join-Path $KenshiDir "kenshi_x64.exe"))) {
    throw "Kenshi not found. Pass -KenshiDir 'C:\path\to\Kenshi'."
}
Write-Host "Kenshi install: $KenshiDir"

Test-CoopPrereqs -KenshiDir $KenshiDir -UseSteam $useSteam

# ---- Install mod + save ------------------------------------------------------------
$modDst = Join-Path $KenshiDir "mods\KenshiCoop"
New-Item -ItemType Directory -Force -Path $modDst | Out-Null
Copy-Item -Force (Join-Path $kitDir "mod\*") $modDst
Write-Host "Mod installed -> $modDst"

$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
New-Item -ItemType Directory -Force -Path $saveRoot | Out-Null
if ($Resume) {
    # No install: the save was DELIVERED by the host's coordinated save last
    # session (streamed in-band, CRC-verified, committed atomically). It must
    # already exist here or there is nothing to resume.
    $found = (Test-Path (Join-Path $saveRoot $save)) -or
             (Test-Path (Join-Path $KenshiDir "save\$save"))
    if (-not $found) {
        throw ("Resume save '$save' not found in '$saveRoot' or '$KenshiDir\save'. " +
               "Run at least one connected session first (the host's save is " +
               "streamed to this machine automatically), or drop -Resume.")
    }
    Write-Host "Resuming on transferred save '$save' (no kit save installed)."
} else {
    $saveDst = Join-Path $saveRoot $save
    if (Test-Path $saveDst) { Remove-Item -Recurse -Force $saveDst }
    Copy-Item -Recurse (Join-Path $kitDir "save\$save") $saveDst
    # Some installs read saves from <Kenshi>\save instead of %LOCALAPPDATA%; mirror
    # to both so auto-load finds it either way.
    $installSaveRoot = Join-Path $KenshiDir "save"
    if (Test-Path $installSaveRoot) {
        $dst2 = Join-Path $installSaveRoot $save
        if (Test-Path $dst2) { Remove-Item -Recurse -Force $dst2 }
        Copy-Item -Recurse (Join-Path $kitDir "save\$save") $dst2
    }
    Write-Host "Save '$save' installed."
}

# ---- Output folder ------------------------------------------------------------------
$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $kitDir "results_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$joinLog = Join-Path $outDir "join.log"

# ---- Env + launch --------------------------------------------------------------------
$env:KENSHICOOP_MODE         = "join"
$env:KENSHICOOP_IP           = $HostIp
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $save
$env:KENSHICOOP_LOG          = $joinLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = ""
$env:KENSHICOOP_TEST_SECONDS = if ($Scenario -ne "") { "600" }   # hard backstop
                               elseif ($FreePlayMinutes -gt 0) { "$($FreePlayMinutes * 60)" }
                               else { "0" }
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Peer-ready arming: the scenario clock starts when the host's stream arrives;
# generous fallback for a slow internet connect.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "240000"
if ($useSteam) {
    $hostId64 = ConvertTo-SteamId64 $HostSteamId
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$hostId64"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

$target = if ($useSteam) { "steam:$(ConvertTo-SteamId64 $HostSteamId)" } else { "${HostIp}:$Port" }
Write-Host ""
if ($useSteam) {
    Write-Host "Transport: STEAM P2P (no IP / port forwarding). Make sure Steam is ONLINE"
    Write-Host "and that the host launched with YOUR SteamID as their peer."
}
Write-Host "Launching Kenshi as JOIN -> $target  (save '$save', scenario '$Scenario')"
$out = & (Join-Path $kitDir "start_kenshi.ps1") -ExePath (Join-Path $KenshiDir "kenshi_x64.exe") -WorkDir $KenshiDir -TimeoutSec 240 6>&1
$out | ForEach-Object { Write-Host "    $_" }
$gamePid = 0
$line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
if ($gamePid -eq 0) { throw "Kenshi failed to get past the launcher." }
Write-Host "Game PID: $gamePid"

# Confirm RE_Kenshi actually loaded the plugin (its log appears immediately);
# otherwise the game is running vanilla and waiting for a connection is moot.
[void](Wait-PluginLoaded -LogPath $joinLog -KenshiDir $KenshiDir -TimeoutSec 120)

function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ---- Monitor: connection, gameplay, scenario anchor, screenshots ------------------------
if (Wait-ForLogLine -File $joinLog -Pattern "peer connected" -TimeoutSec 120) {
    Write-Host "CONNECTED to the host."
} elseif ($useSteam) {
    Write-Warning "No connection after 120 s. Check that BOTH Steams are online and that"
    Write-Warning "the host used YOUR SteamID as their peer. Results still collected."
} else {
    Write-Warning "No connection after 120 s. Check the host's port forwarding / firewall."
    Write-Warning "Leaving the game up in case it connects late; results still collected."
}
if (Wait-ForLogLine -File $joinLog -Pattern "gameplay started" -TimeoutSec 240) {
    Write-Host "In-game."
}
if ($Scenario -ne "") {
    if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec 180) {
        Write-Host "Scenario streaming; capturing screenshots."
        Start-Sleep -Seconds 2
        try {
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kitDir "screenshot.ps1") `
                -ProcessId $gamePid -Out (Join-Path $outDir "join.png") -Frames 5 -IntervalMs 16
        } catch { Write-Warning "screenshot failed: $($_.Exception.Message)" }
    }
    Write-Host "Waiting for the scenario to finish (self-exit; up to 10 min hard cap) ..."
    try { Wait-Process -Id $gamePid -Timeout 660 -ErrorAction Stop }
    catch {
        Write-Warning "Game did not self-exit; closing it."
        try { Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue } catch {}
    }
} else {
    Write-Host ""
    Write-Host "FREE PLAY: play together; close the game window when the session ends."
    Write-Host "(This script waits and collects the results when Kenshi exits.)"
    Wait-Process -Id $gamePid -ErrorAction SilentlyContinue
}

# ---- Bundle results -----------------------------------------------------------------------
@"
kit save:      $save
scenario:      $Scenario
transport:     $(if ($useSteam) { "steam (host steamid $(ConvertTo-SteamId64 $HostSteamId))" } else { "udp" })
host ip:       $HostIp
port:          $Port
machine:       $env:COMPUTERNAME
os:            $([System.Environment]::OSVersion.VersionString)
utc offset:    $((Get-TimeZone).BaseUtcOffset)
run stamp:     $stamp
"@ | Set-Content (Join-Path $outDir "session_info.txt") -Encoding UTF8

$zip = Join-Path $kitDir "KenshiCoop-results-$stamp.zip"
Compress-Archive -Path "$outDir\*" -DestinationPath $zip -Force
Write-Host ""
Write-Host "==================================================================="
Write-Host " Results bundled: $zip"
Write-Host " Send this file back to the host."
Write-Host "==================================================================="
