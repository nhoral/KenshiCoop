<#
.SYNOPSIS
  The friend-side HOST runner for a KenshiCoop remote test session. Ships
  INSIDE the host-role remote kit (scripts\make_remote_kit.ps1 -Role host);
  runs from the unzipped kit folder. This is the real-session topology: the
  friend hosts (best experience on their machine), you join and iterate.

.DESCRIPTION
  One command:
    powershell -ExecutionPolicy Bypass -File friend_host.ps1

  Steam P2P (no port forwarding / router setup at all):
    powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <joining player's steamid64>
  Your own SteamID is printed once the game is up - read it to the joining
  player (they pass it as -HostSteamId). The kit may pre-bake -PeerSteamId.

  Installs the bundled mod + save into the local Kenshi, adds the Windows
  Firewall rule, asks the router to forward the UDP port automatically via
  UPnP (falling back to the manual port-forward checklist if the router
  refuses), prints this machine's public IP (send it to the joining player),
  launches the game as the HOST, waits for the session to end (scenario
  self-exit, or the friend closes the window in free play), and bundles
  host.log + screenshots into KenshiCoop-results-<stamp>.zip to send back
  for offline judging (analyze_run.ps1).

  Run with -CheckOnly the day before a scheduled session to verify the
  firewall + router setup without installing or launching anything.

  Scenario timing note: the host's scripted actions ARM only when the joining
  player's stream arrives (peer-ready), so nothing happens before both are
  in-game - the friend just needs to leave the window running.
#>
[CmdletBinding()]
param(
    [string]$KenshiDir = "",
    # Override the kit's default scenario ("" = kit default; "free" = normal
    # co-op play, no scripted scenario, no self-exit).
    [string]$Scenario = "",
    [int]$Port = 0,
    [int]$FreePlayMinutes = 0,
    # Skip the interactive "press Enter when ready" gate (used by the
    # automated dress rehearsal; humans should leave it on).
    [switch]$NoPrompt,
    # Skip the automatic UPnP router mapping (manual port forward only).
    [switch]$NoUpnp,
    # Preflight only: firewall rule + UPnP + public-IP check, then exit.
    # Installs nothing, launches nothing. Run this the day before a scheduled
    # session so router problems surface off the clock.
    [switch]$CheckOnly,
    # Steam P2P transport: the JOINING player's steamid64 (or short friend code).
    # When set, Steam brokers the connection - the firewall / UPnP / public-IP
    # checklist is skipped entirely. The kit may pre-bake this (kit.json).
    [string]$PeerSteamId = "",
    # Resume the previous session: load the 'coopresume' save the coordinated
    # save (protocol 31) wrote last session instead of installing the kit's
    # baked save. BOTH sides must resume. Defaults to free play (no scenario).
    [switch]$Resume,
    # Save name the coordinated-save flow writes (must match the join's).
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
    # The save the coordinated save (protocol 31) wrote last session; resume =
    # both sides load the identical file. Free play unless a scenario is asked
    # for explicitly.
    $save = $ResumeSave
    if ($Scenario -eq "") { $Scenario = "free" }
}
if ($Scenario -eq "") { $Scenario = $kit.scenario }
if ($Scenario -eq "free") { $Scenario = "" }
if ($Port -eq 0) { $Port = [int]$kit.port }
# The kit can pre-bake the Steam transport (kit.json: transport + peerSteamId).
if ($PeerSteamId -eq "" -and $kit.PSObject.Properties["peerSteamId"] -and "$($kit.peerSteamId)" -ne "" -and "$($kit.peerSteamId)" -ne "0") {
    $PeerSteamId = "$($kit.peerSteamId)"
}
$useSteam = ($PeerSteamId -ne "")
# Validate the ID up front (clear error now beats a dead session later).
if ($useSteam) { [void](ConvertTo-SteamId64 $PeerSteamId) }
# Print YOUR friend code first (read from Steam) so the two of you can swap
# codes right off this screen - no profile-page spelunking.
if ($useSteam -or "$($kit.transport)" -eq "steam") { Show-MySteamId }

# ---- Connectivity helpers ----------------------------------------------------------
. (Join-Path $kitDir "upnp_portmap.ps1")

function Ensure-CoopFirewallRule {
    param([int]$RulePort)
    try {
        $rule = & netsh advfirewall firewall show rule name=KenshiCoop 2>&1
        if ("$rule" -notmatch "KenshiCoop") {
            & netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$RulePort | Out-Null
            Write-Host "Firewall: inbound UDP $RulePort allowed (rule added)."
        } else {
            Write-Host "Firewall: KenshiCoop rule already present."
        }
        return $true
    } catch {
        Write-Warning "Could not add the firewall rule automatically (needs admin PowerShell)."
        Write-Warning "Run once as admin: netsh advfirewall firewall add rule name=KenshiCoop dir=in action=allow protocol=UDP localport=$RulePort"
        return $false
    }
}

function Get-PublicIp {
    try { return (Invoke-WebRequest -Uri "https://api.ipify.org" -UseBasicParsing -TimeoutSec 5).Content.Trim() }
    catch { return "" }
}

# ---- Preflight-only mode (-CheckOnly) ------------------------------------------------
if ($CheckOnly -and $useSteam) {
    Write-Host ""
    Write-Host "=== PRE-SESSION CHECK (Steam P2P transport) ==="
    Write-Host " Steam brokers the connection by SteamID - no firewall rule, router"
    Write-Host " forwarding or public IP needed. Just make sure that on session day:"
    Write-Host "   1. Steam is RUNNING and ONLINE (not offline mode) on both machines."
    Write-Host "   2. You and the other player have exchanged SteamIDs."
    Write-Host " READY."
    exit 0
}
if ($CheckOnly) {
    Write-Host ""
    Write-Host "=== PRE-SESSION CONNECTIVITY CHECK (installs nothing, launches nothing) ==="
    $fwOk = Ensure-CoopFirewallRule $Port
    $publicIp = Get-PublicIp
    if ($publicIp -ne "") { Write-Host "Public IP: $publicIp" }
    else                  { Write-Host "Public IP: (could not detect - use whatismyip.com)" }

    Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
    $upnp = Add-UpnpMapping -Port $Port
    $hairpin = "skipped"
    if ($upnp.Ok) {
        Write-Host "UPnP: mapping accepted (UDP $Port -> $($upnp.LanIp))."
        $probeIp = if ($upnp.ExternalIp) { $upnp.ExternalIp } else { $publicIp }
        if ($probeIp -match '^\d') {
            Write-Host "Probing reachability via your own public IP (NAT hairpin) ..."
            $hairpin = Test-UpnpHairpin -Port $Port -ExternalIp $probeIp
        }
        Remove-UpnpMapping -Port $Port
    } else {
        Write-Host "UPnP: FAILED - $($upnp.Error)"
    }

    $cgnat = ($upnp.Ok -and $upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp)

    Write-Host ""
    Write-Host "=================== CHECK RESULT ==================="
    if ($upnp.Ok -and -not $cgnat) {
        Write-Host " READY: the session script will open the port automatically."
        if ($hairpin -eq "reachable") {
            Write-Host " (Reachability probe PASSED - strongest possible pre-session signal.)"
        } elseif ($hairpin -eq "inconclusive") {
            Write-Host " (Reachability probe inconclusive - normal; many routers can't self-test.)"
        }
    } elseif ($cgnat) {
        Write-Host " WARNING: your router reports external IP $($upnp.ExternalIp) but the"
        Write-Host " internet sees $publicIp - you appear to be behind a second NAT"
        Write-Host " (carrier-grade NAT). Port forwarding will NOT help; tell the other"
        Write-Host " player so you can arrange a VPN overlay (e.g. Tailscale) instead."
    } else {
        Write-Host " NEEDS MANUAL PORT FORWARD: automatic UPnP did not work."
        Write-Host " Forward UDP $Port to this PC on your router's 'port forwarding'"
        Write-Host " page (or enable UPnP in the router settings and re-run this check)."
    }
    if (-not $fwOk) { Write-Host " Also fix the Windows Firewall rule (see warning above)." }
    Write-Host "===================================================="
    if ($upnp.Ok -and -not $cgnat -and $fwOk) { exit 0 } else { exit 1 }
}

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
    # No install: the save was written by last session's coordinated save
    # (protocol 31). It must already exist here or there is nothing to resume.
    $found = (Test-Path (Join-Path $saveRoot $save)) -or
             (Test-Path (Join-Path $KenshiDir "save\$save"))
    if (-not $found) {
        throw ("Resume save '$save' not found in '$saveRoot' or '$KenshiDir\save'. " +
               "Run at least one connected session first (any save during it is " +
               "coordinated + shared automatically), or drop -Resume.")
    }
    Write-Host "Resuming on coordinated save '$save' (no kit save installed)."
} else {
    foreach ($destBase in @($saveRoot, (Join-Path $KenshiDir "save"))) {
        if ($destBase -like "*\save" -and -not (Test-Path (Split-Path -Parent $destBase))) { continue }
        New-Item -ItemType Directory -Force -Path $destBase | Out-Null
        $dst = Join-Path $destBase $save
        if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
        Copy-Item -Recurse (Join-Path $kitDir "save\$save") $dst
    }
    Write-Host "Save '$save' installed."
}

# ---- Firewall (local) + router mapping + checklist --------------------------------------
$upnp = $null
$upnpNote = "n/a (steam transport)"
$publicIp = "(n/a - steam transport)"
if ($useSteam) {
    $peerId64 = ConvertTo-SteamId64 $PeerSteamId
    Write-Host ""
    Write-Host "=============== HOST CHECKLIST (STEAM P2P) ==============="
    Write-Host " 1. Steam connects you BY STEAMID - no router setup, no port"
    Write-Host "    forwarding, no public IPs. Keep Steam ONLINE."
    Write-Host " 2. Peering with the joining player's SteamID: $peerId64"
    Write-Host " 3. YOUR SteamID is printed after launch - read it to them."
    Write-Host " 4. Leave this window open; the game closes itself when the"
    Write-Host "    test scenario finishes (free play: close the game when done)."
    Write-Host "==========================================================="
    Write-Host ""
} else {
[void](Ensure-CoopFirewallRule $Port)

$upnpNote = "disabled (-NoUpnp)"
if (-not $NoUpnp) {
    Write-Host "Asking the router to forward UDP $Port automatically (UPnP) ..."
    $upnp = Add-UpnpMapping -Port $Port
    $upnpNote = if ($upnp.Ok) { "mapped UDP $Port -> $($upnp.LanIp) (router external IP: $($upnp.ExternalIp))" }
                else          { "failed: $($upnp.Error)" }
}

$publicIp = Get-PublicIp
if ($publicIp -eq "") { $publicIp = "(could not detect - use whatismyip.com)" }
Write-Host ""
Write-Host "=================== HOST CHECKLIST ==================="
if ($null -ne $upnp -and $upnp.Ok) {
    Write-Host " 1. Router: UPnP mapping added automatically (UDP $Port -> $($upnp.LanIp))."
    Write-Host "    No router setup needed."
    if ($upnp.ExternalIp -and $publicIp -match '^\d' -and $upnp.ExternalIp -ne $publicIp) {
        Write-Host "    NOTE: your router reports external IP $($upnp.ExternalIp) but the internet"
        Write-Host "    sees $publicIp - you may be behind a second NAT (carrier-grade NAT)"
        Write-Host "    and the connection may still fail. Manual forwarding won't help there;"
        Write-Host "    a VPN overlay (e.g. Tailscale) is the workaround."
    }
} else {
    if ($null -ne $upnp) { Write-Host " 1. Automatic UPnP mapping FAILED ($($upnp.Error))." }
    else                 { Write-Host " 1. Automatic UPnP mapping skipped (-NoUpnp)." }
    Write-Host "    Router: forward UDP port $Port to this PC (see your router's"
    Write-Host "    'port forwarding' page; this PC's LAN IP is usually 192.168.x.x)."
}
Write-Host " 2. Tell the joining player your public IP: $publicIp"
Write-Host " 3. Leave this window open; the game closes itself when the test"
Write-Host "    scenario finishes (free play: close the game when done)."
Write-Host "======================================================="
Write-Host ""
}
if (-not $NoPrompt) { Read-Host "Press Enter to launch the host" | Out-Null }

# ---- Output + env -----------------------------------------------------------------------
$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $kitDir "results_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$hostLog = Join-Path $outDir "host.log"

$env:KENSHICOOP_MODE         = "host"
$env:KENSHICOOP_IP           = "0.0.0.0"
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $save
$env:KENSHICOOP_LOG          = $hostLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = ""
$env:KENSHICOOP_TEST_SECONDS = if ($Scenario -ne "") { "600" }
                               elseif ($FreePlayMinutes -gt 0) { "$($FreePlayMinutes * 60)" }
                               else { "0" }
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
# Scenario actions arm when the joining player's stream arrives; generous
# fallback for a slow internet connect.
$env:KENSHICOOP_ARM_TIMEOUT_MS = "240000"
if ($useSteam) {
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$(ConvertTo-SteamId64 $PeerSteamId)"
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

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

try {
    Write-Host "Launching Kenshi as HOST on UDP $Port (save '$save', scenario '$Scenario')"
    $out = & (Join-Path $kitDir "start_kenshi.ps1") -ExePath (Join-Path $KenshiDir "kenshi_x64.exe") -WorkDir $KenshiDir -TimeoutSec 240 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $gamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
    if ($gamePid -eq 0) { throw "Kenshi failed to get past the launcher." }
    Write-Host "Game PID: $gamePid"

    # Confirm RE_Kenshi actually loaded the plugin (its log appears immediately);
    # otherwise the game is running vanilla and nobody will ever connect.
    [void](Wait-PluginLoaded -LogPath $hostLog -KenshiDir $KenshiDir -TimeoutSec 120)

    # Steam transport: surface this machine's SteamID (plugin logs "[steam] id=")
    # so the friend can read it to the joining player for their -HostSteamId.
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
            Write-Host ">>> YOUR SteamID: $myId" -ForegroundColor Green
            Write-Host ">>> The joining player runs: join_session.ps1 -HostSteamId $myId" -ForegroundColor Green
        } else {
            Write-Warning "No '[steam] id=' line in host.log - is Steam running and online?"
        }
    }

    if (Wait-ForLogLine -File $hostLog -Pattern "peer connected" -TimeoutSec 300) {
        Write-Host "JOINING PLAYER CONNECTED."
    } elseif ($useSteam) {
        Write-Warning "No connection after 5 min - check that BOTH Steams are online and the"
        Write-Warning "SteamIDs were exchanged correctly (each side peers with the OTHER's id)."
        Write-Warning "Leaving the game up in case they connect late."
    } else {
        Write-Warning "No connection after 5 min - check the port forward + firewall."
        Write-Warning "Leaving the game up in case they connect late."
    }

    # Screenshot once the scenario is live (best-effort evidence for the results zip).
    if ($Scenario -ne "") {
        if (Wait-ForLogLine -File $hostLog -Pattern "SCENARIO MEMBER" -TimeoutSec 300) {
            Start-Sleep -Seconds 2
            try {
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kitDir "screenshot.ps1") `
                    -ProcessId $gamePid -Out (Join-Path $outDir "host.png") -Frames 5 -IntervalMs 16
            } catch {}
        }
        Write-Host "Waiting for the scenario to finish (self-exit; 10 min hard cap) ..."
        try { Wait-Process -Id $gamePid -Timeout 660 -ErrorAction Stop }
        catch {
            Write-Warning "Game did not self-exit; closing it."
            try { Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue } catch {}
        }
    } else {
        Write-Host ""
        Write-Host "FREE PLAY: play together; close the game window when the session ends."
        Wait-Process -Id $gamePid -ErrorAction SilentlyContinue
    }
} finally {
    # Don't leave the router mapping open after the session.
    if ($null -ne $upnp -and $upnp.Ok) {
        Remove-UpnpMapping -Port $Port
        Write-Host "UPnP mapping removed."
    }
}

# ---- Bundle results -----------------------------------------------------------------------
@"
role:          host
kit save:      $save
scenario:      $Scenario
transport:     $(if ($useSteam) { "steam (peer $(ConvertTo-SteamId64 $PeerSteamId))" } else { "udp" })
port:          $Port
public ip:     $publicIp
upnp:          $upnpNote
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
Write-Host " Send this file back to the joining player."
Write-Host "==================================================================="
