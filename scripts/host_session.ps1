<#
.SYNOPSIS
  Host side of a remote (internet) co-op test session: pre-flight checklist,
  launch the host client with the agreed scenario, collect host.log, and print
  the analyze_run.ps1 command to judge the session once the friend's results
  zip arrives.

.DESCRIPTION
  Pairs with the kit built by scripts\make_remote_kit.ps1 (the friend runs
  friend_join.ps1 from it). The DLL deployed here MUST be the same build as the
  kit's (the protocol-version handshake rejects a mismatch by design - rebuild
  and re-send the kit rather than mixing builds).

  Scenario mode: the host runs the compiled scenario and self-exits like an
  automated run (generous 10-min backstop for slow remote loads). Free play
  (-Scenario free): no scenario, no self-exit; close the window to end.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario coop_presence

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario free -Save squad1

.EXAMPLE
  # Steam P2P transport: no UPnP, no port forwarding, no public IPs. Pass the
  # FRIEND's steamid64 (or short friend code); they join with -HostSteamId set
  # to YOURS (printed by this script once the game is up).
  powershell -ExecutionPolicy Bypass -File scripts\host_session.ps1 -Scenario free -Save squad1 -PeerSteamId 76561198000000000
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    # Save to load (defaults from the scenario manifest; must match the kit's).
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [switch]$SkipBuild,
    [int]$FreePlayMinutes = 0,
    # Skip the automatic UPnP router mapping (manual port forward only).
    [switch]$NoUpnp,
    # Steam P2P transport: the JOINING player's steamid64 (or short friend code).
    # When set, Steam brokers the connection (NAT punch / Valve relay) and the
    # whole UPnP / port-forward / public-IP checklist is skipped.
    [string]$PeerSteamId = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

# Short Steam friend codes are steamid64 minus the account-universe base.
function ConvertTo-SteamId64([string]$id) {
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}
$useSteam = ($PeerSteamId -ne "")

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest

$isFree = ($Scenario -eq "free" -or $Scenario -eq "")
if ($isFree) { $Scenario = "" }
if ($Save -eq "" -and -not $isFree -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $Save = $manifest.Scenarios[$Scenario].Save
}
if ($Save -eq "") { throw "Pass -Save (free play has no manifest default)." }

if (-not $SkipBuild) {
    Write-Host "=== build + deploy ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "deploy failed" }
    Write-Host ""
    Write-Host "NOTE: if you rebuilt since making the kit, REBUILD THE KIT too"
    Write-Host "      (scripts\make_remote_kit.ps1) - the protocol handshake rejects mixed builds."
}

# ---- Pre-flight checklist -------------------------------------------------------
$upnp = $null
if ($useSteam) {
    $peerId64 = ConvertTo-SteamId64 $PeerSteamId
    Write-Host ""
    Write-Host "=================== REMOTE SESSION PRE-FLIGHT (STEAM P2P) ==================="
    Write-Host " Transport: Steam P2P - no router setup, no port forwarding, no public IPs."
    Write-Host " 1. Both machines: Steam RUNNING and ONLINE (not offline mode)."
    Write-Host " 2. This host will peer with the friend's SteamID: $peerId64"
    Write-Host " 3. Your own SteamID is printed below once the game is up - read it to the"
    Write-Host "    friend; they run: friend_join.ps1 -HostSteamId <your id>"
    Write-Host " 4. Session plan: start with coop_presence, then escalate through the"
    Write-Host "    smoke set (npc_sync, combat_kill, inv_bidir, world_weapon_drop)."
    Write-Host "============================================================================="
    Write-Host ""
} else {
. (Join-Path $scriptDir "upnp_portmap.ps1")

if (-not $NoUpnp) {
    Write-Host ""
    Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
    $upnp = Add-UpnpMapping -Port $Port
}

$publicIp = "(unknown)"
try {
    $publicIp = (Invoke-WebRequest -Uri "https://api.ipify.org" -UseBasicParsing -TimeoutSec 5).Content.Trim()
} catch {}
Write-Host ""
Write-Host "=================== REMOTE SESSION PRE-FLIGHT ==================="
if ($null -ne $upnp -and $upnp.Ok) {
    Write-Host " 1. Router: UPnP mapping added automatically (UDP $Port -> $($upnp.LanIp))."
    Write-Host "    No router setup needed (removed again when the session ends)."
    if ($upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp) {
        Write-Host "    NOTE: router external IP $($upnp.ExternalIp) != internet-visible $publicIp -"
        Write-Host "    likely carrier-grade NAT; the connection may fail regardless of forwarding."
    }
} else {
    if ($null -ne $upnp) { Write-Host " 1. Automatic UPnP mapping FAILED ($($upnp.Error))." }
    else                 { Write-Host " 1. Automatic UPnP mapping skipped (-NoUpnp)." }
    Write-Host "    Router: forward UDP port $Port to this machine."
}
Write-Host " 2. Windows Firewall: allow inbound UDP $Port (or kenshi_x64.exe)."
Write-Host "    One-time (admin): netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$Port"
Write-Host " 3. Tell your friend your public IP: $publicIp"
Write-Host "    Friend runs: powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp $publicIp"
Write-Host " 4. Session plan: start with coop_presence, then escalate through the"
Write-Host "    smoke set (npc_sync, combat_kill, inv_bidir, world_weapon_drop)."
Write-Host "=================================================================="
Write-Host ""
}

# ---- Launch host -----------------------------------------------------------------
$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $repoRoot "tools\test-runs\remote_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$hostLog = Join-Path $outDir "host.log"

$env:KENSHICOOP_MODE         = "host"
$env:KENSHICOOP_IP           = "0.0.0.0"
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $Save
$env:KENSHICOOP_LOG          = $hostLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = if (-not $isFree -and $manifest.Scenarios.ContainsKey($Scenario)) { $manifest.Scenarios[$Scenario].Setup } else { "" }
$env:KENSHICOOP_TEST_SECONDS = if (-not $isFree) { "600" }
                               elseif ($FreePlayMinutes -gt 0) { "$($FreePlayMinutes * 60)" }
                               else { "0" }
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Peer-ready arming: the host's scenario clock starts when the friend's stream
# arrives, so scripted actions never fire before the friend is in-game.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "240000"
if ($useSteam) {
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$(ConvertTo-SteamId64 $PeerSteamId)"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

try {
    Write-Host "Launching HOST (save '$Save', scenario '$Scenario', port $Port) ..."
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath (Join-Path $HostDir "kenshi_x64.exe") -WorkDir $HostDir -TimeoutSec 120 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $gamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
    if ($gamePid -eq 0) { throw "Host failed to get past the launcher." }

    # Steam transport: surface our own SteamID (the plugin logs "[steam] id=...")
    # so the user can read it to the friend for their -HostSteamId.
    if ($useSteam) {
        $deadline = (Get-Date).AddSeconds(120)
        $myId = $null
        while ((Get-Date) -lt $deadline -and $null -eq $myId) {
            if (Test-Path $hostLog) {
                $hit = Select-String -Path $hostLog -Pattern "\[steam\] id=(\d+)" -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($null -ne $hit) { $myId = $hit.Matches[0].Groups[1].Value }
            }
            if ($null -eq $myId) { Start-Sleep -Milliseconds 500 }
        }
        Write-Host ""
        if ($null -ne $myId) {
            Write-Host ">>> YOUR SteamID: $myId  - the friend joins with:" -ForegroundColor Green
            Write-Host ">>>   powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId $myId" -ForegroundColor Green
        } else {
            Write-Warning "No '[steam] id=' line in host.log yet - check that Steam is running/online."
        }
    }

    Write-Host ""
    Write-Host "Host is up (PID $gamePid). Waiting for it to exit (scenario self-exit / you close it) ..."
    Wait-Process -Id $gamePid -ErrorAction SilentlyContinue
} finally {
    # Don't leave the router mapping open after the session.
    if ($null -ne $upnp -and $upnp.Ok) {
        Remove-UpnpMapping -Port $Port
        Write-Host "UPnP mapping removed."
    }
}

Write-Host ""
Write-Host "== Session over =="
Write-Host "  host log: $hostLog"
Write-Host ""
Write-Host "When the friend's results zip arrives, extract join.log into $outDir and judge:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -RunDir $outDir -Scenario $(if ($isFree) { '<scenario>' } else { $Scenario })"
