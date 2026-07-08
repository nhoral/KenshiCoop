<#
.SYNOPSIS
  Package the JOIN side of a remote (internet) co-op test session into a single
  zip the friend can run: mod DLL + fixture save + friend_join.ps1 (install,
  connect to your IP, play the scenario, and bundle the logs/screenshots into a
  results zip to send back for analyze_run.ps1).

.DESCRIPTION
  Kit contents (dist\remote-kit\, zipped to dist\KenshiCoop-remote-kit-<stamp>.zip):
    mod\KenshiCoop.dll        - the CURRENT build (protocol-version-matched to yours;
    mod\RE_Kenshi.json          a mismatch is rejected at handshake by design)
    mod\KenshiCoop.mod        - mod-list placeholder (copied from your install)
    save\<name>\...           - the fixture save (both clients MUST load the identical
                                save: entity identity is resolve-by-hand)
    friend_join.ps1           - the friend's one-command runner
    start_kenshi.ps1          - launcher helper (clicks through Kenshi's OK dialog)
    screenshot.ps1            - window capture helper (for the results bundle)
    upnp_portmap.ps1          - UPnP router-mapping helpers (host-role kits only)
    README.txt                - friend-facing instructions + prerequisites

  Run a DRESS REHEARSAL first (kit installed into the local Kenshi-Join install,
  pointed at 127.0.0.1, ideally with run_test's -Wan proxy in the path) so the
  internet session only changes the network, not the workflow.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1 -SkipBuild

.EXAMPLE
  # Steam P2P kit (no port forwarding / router setup): bake YOUR steamid64 in as
  # the friend-host's peer so their command stays one line.
  powershell -ExecutionPolicy Bypass -File scripts\make_remote_kit.ps1 -Save squad1 -Transport steam -MySteamId 76561198000000000
#>
[CmdletBinding()]
param(
    # Which role the FRIEND plays. 'host' is the real-session topology (friend
    # hosts for the best experience on their machine; you join and iterate) and
    # bundles friend_host.ps1; 'join' bundles friend_join.ps1.
    [ValidateSet("host", "join")]
    [string]$Role = "host",
    # Fixture save to bundle (must exist in %LOCALAPPDATA%\kenshi\save).
    [string]$Save = "squad1",
    # Default scenario the friend script runs (any manifest scenario, or "" = free play).
    [string]$Scenario = "coop_presence",
    [int]$Port = 27800,
    [switch]$SkipBuild,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    # Transport baked into kit.json: "udp" (default; IP + port forwarding) or
    # "steam" (connect by SteamID: NAT punch + Valve relay, no router setup).
    [ValidateSet("udp", "steam")]
    [string]$Transport = "udp",
    # YOUR steamid64 (or short friend code), baked into a steam kit so the friend
    # script already knows its peer: the friend-HOST peers with you (peerSteamId);
    # a friend-JOIN kit connects to you as the host (hostSteamId). Get it from the
    # "[steam] id=" line of any host.log, or steamid.io. Optional: the friend can
    # always pass the id on the command line instead.
    [string]$MySteamId = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    Write-Host "=== build plugin ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

$dll  = Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"
$json = Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"
if (-not (Test-Path $dll))  { throw "DLL not built: $dll" }
if (-not (Test-Path $json)) { throw "RE_Kenshi.json missing: $json" }

$saveSrc = Join-Path $env:LOCALAPPDATA "kenshi\save\$Save"
if (-not (Test-Path $saveSrc)) { throw "Save '$Save' not found at $saveSrc" }

$kitDir = Join-Path $repoRoot "dist\remote-kit"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path "$kitDir\mod" | Out-Null

Write-Host "=== assembling kit ==="
Copy-Item $dll  "$kitDir\mod\KenshiCoop.dll"
Copy-Item $json "$kitDir\mod\RE_Kenshi.json"
$modFile = Join-Path $HostDir "mods\KenshiCoop\KenshiCoop.mod"
if (Test-Path $modFile) { Copy-Item $modFile "$kitDir\mod\KenshiCoop.mod" }
else { Write-Warning "No KenshiCoop.mod in the host install; the friend must create one via FCS." }

New-Item -ItemType Directory -Force -Path "$kitDir\save" | Out-Null
Copy-Item -Recurse $saveSrc "$kitDir\save\$Save"

$friendScript = if ($Role -eq "host") { "friend_host.ps1" } else { "friend_join.ps1" }
Copy-Item (Join-Path $scriptDir $friendScript)      "$kitDir\$friendScript"
Copy-Item (Join-Path $scriptDir "start_kenshi.ps1") "$kitDir\start_kenshi.ps1"
Copy-Item (Join-Path $scriptDir "screenshot.ps1")   "$kitDir\screenshot.ps1"
if ($Role -eq "host") {
    # UPnP router-mapping helpers (dot-sourced by friend_host.ps1).
    Copy-Item (Join-Path $scriptDir "upnp_portmap.ps1") "$kitDir\upnp_portmap.ps1"
}

# Kit defaults consumed by the friend script (keeps the friend's command minimal).
# Steam kits bake YOUR steamid64 as the friend's counterpart id: the friend-HOST
# peers with you (peerSteamId); the friend-JOIN connects to you (hostSteamId).
function ConvertTo-SteamId64([string]$id) {
    $v = [uint64]$id
    if ($v -lt 76561197960265728) { $v += 76561197960265728 }
    return $v
}
$myId64 = if ($MySteamId -ne "") { "$(ConvertTo-SteamId64 $MySteamId)" } else { "" }
$kitMeta = @{
    role      = $Role
    save      = $Save
    scenario  = $Scenario
    port      = $Port
    transport = $Transport
}
if ($Transport -eq "steam" -and $myId64 -ne "") {
    if ($Role -eq "host") { $kitMeta.peerSteamId = $myId64 }
    else                  { $kitMeta.hostSteamId = $myId64 }
}
$kitMeta | ConvertTo-Json | Set-Content "$kitDir\kit.json" -Encoding UTF8

$steamKit = ($Transport -eq "steam")
$howTo = if ($steamKit -and $Role -eq "host") {
@"
This kit uses STEAM P2P: Steam connects the two of you BY STEAMID - no port
forwarding, no router setup, no public IPs. Keep Steam RUNNING and ONLINE.

When it's time to play, from PowerShell in this folder:

$(if ($myId64 -ne "") {
"  powershell -ExecutionPolicy Bypass -File friend_host.ps1

(The other player's SteamID is already baked into this kit.)"
} else {
"  powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <the other player's steamid64>

(They will read you their SteamID; steamid.io converts profile URLs.)"
})

What it does:
  * copies the mod into your Kenshi install (mods\KenshiCoop),
  * copies the shared test save,
  * launches Kenshi as the HOST and prints YOUR SteamID - read it to the
    other player so they can join you,
  * waits for the session (the test scenario starts only when the other
    player is in-game; the game closes itself when the scenario finishes),
  * bundles your log + screenshots into KenshiCoop-results-<stamp>.zip
    next to this script. Send that zip back.
"@
} elseif ($steamKit) {
@"
This kit uses STEAM P2P: Steam connects the two of you BY STEAMID - no port
forwarding, no router setup, no public IPs. Keep Steam RUNNING and ONLINE.

Then, from PowerShell in this folder:

$(if ($myId64 -ne "") {
"  powershell -ExecutionPolicy Bypass -File friend_join.ps1

(The host's SteamID is already baked into this kit.)"
} else {
"  powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <the host's steamid64>"
})

IMPORTANT: also tell the host YOUR SteamID (Steam > profile > Account
details, or steamid.io) - they need it on their side before you connect.

What it does:
  * copies the mod into your Kenshi install (mods\KenshiCoop),
  * copies the shared test save into %LOCALAPPDATA%\kenshi\save,
  * launches Kenshi, auto-loads the save and connects to the host via Steam,
  * runs the agreed test scenario (it exits by itself when done),
  * bundles your log + screenshots into KenshiCoop-results-<stamp>.zip
    next to this script. Send that zip back.
"@
} elseif ($Role -eq "host") {
@"
RECOMMENDED - the day before the session, run the connectivity check
(takes seconds, installs nothing):

  powershell -ExecutionPolicy Bypass -File friend_host.ps1 -CheckOnly

If it prints READY you are set. If it says NEEDS MANUAL PORT FORWARD,
forward UDP $Port to your PC on your router (or enable UPnP in the router
settings and re-run the check) - doing this ahead of time saves the session.

Then, when it's time to play, from PowerShell in this folder:

  powershell -ExecutionPolicy Bypass -File friend_host.ps1

What it does:
  * copies the mod into your Kenshi install (mods\KenshiCoop),
  * copies the shared test save,
  * adds a Windows Firewall rule for UDP $Port,
  * asks your router to forward UDP $Port automatically (UPnP); only if
    your router refuses do you need to set up port forwarding by hand -
    the script tells you either way, and it looks up the public IP you
    send to the joining player,
  * launches Kenshi as the HOST and waits (the test scenario starts only
    when the other player is in-game, and the game closes itself when the
    scenario finishes),
  * bundles your log + screenshots into KenshiCoop-results-<stamp>.zip
    next to this script. Send that zip back.
"@
} else {
@"
Then, from PowerShell in this folder:

  powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <the host's public IP>

What it does:
  * copies the mod into your Kenshi install (mods\KenshiCoop),
  * copies the shared test save into %LOCALAPPDATA%\kenshi\save,
  * launches Kenshi, auto-loads the save and connects to the host,
  * runs the agreed test scenario (it exits by itself when done),
  * bundles your log + screenshots into KenshiCoop-results-<stamp>.zip
    next to this script. Send that zip back.
"@
}

@"
KenshiCoop remote co-op test kit (your role: $($Role.ToUpper()))
================================

You need:
  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode (Options > Video > Full Screen off).
  2. RE_Kenshi 0.3.1+ installed (Nexus mod - it loads the co-op plugin DLL).

$howTo

In free play the full sync set is active by default: positions, combat,
health/limbs, stats, game speed, carried bodies, AND (new) inventory +
equipment changes + items dropped on the ground.

Nothing else on your machine is touched. To remove: delete
<Kenshi>\mods\KenshiCoop and the test save folder.

Troubleshooting:
  * "protocol mismatch" in the log = your kit is older/newer than the other
    player's build; ask for a fresh kit.
$(if ($steamKit) {
"  * No connection (Steam kit): both Steams must be RUNNING and ONLINE (not
    offline mode), and each side must be launched with the OTHER player's
    SteamID. Look for '[steam] session ... active=1' in the log - relay=1
    just means Valve is relaying (works fine, slightly higher ping)."
} else {
"  * No connection: UDP $Port must be reachable on the HOST. friend_host.ps1
    handles the Windows Firewall and asks the router to open the port via
    UPnP; if the script said UPnP failed, forward UDP $Port to the host PC
    manually on the router's 'port forwarding' page. If it warned about
    carrier-grade NAT, forwarding won't help - use a VPN overlay such as
    Tailscale and share that IP instead."
})
"@ | Set-Content "$kitDir\README.txt" -Encoding UTF8

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$zip = Join-Path $repoRoot "dist\KenshiCoop-remote-kit-$stamp.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$kitDir\*" -DestinationPath $zip
Write-Host ""
Write-Host "Kit assembled: $kitDir"
Write-Host "Kit zipped:    $zip"
Write-Host ""
if ($Role -eq "host") {
    Write-Host "Next (friend-HOSTS topology):"
    Write-Host "  1. Dress-rehearse against the LAN machine: scripts\rehearse_host_kit.ps1"
    Write-Host "  2. Send the zip to your friend; schedule the session."
    if ($steamKit) {
        Write-Host "  3. Your side when it starts: scripts\join_session.ps1 -HostSteamId <their steamid64> -Scenario $Scenario"
    } else {
        Write-Host "  3. Your side when it starts: scripts\join_session.ps1 -HostIp <their public IP> -Scenario $Scenario"
    }
    Write-Host "  4. Judge: scripts\analyze_run.ps1 -HostLog <theirs> -JoinLog <yours> -Scenario $Scenario"
} else {
    Write-Host "Next (friend-JOINS topology):"
    Write-Host "  1. Dress-rehearse locally: scripts\rehearse_remote_kit.ps1"
    Write-Host "  2. Send the zip to your friend; schedule the session."
    if ($steamKit) {
        Write-Host "  3. Host side: scripts\host_session.ps1 -Scenario $Scenario -PeerSteamId <their steamid64>"
    } else {
        Write-Host "  3. Host side: scripts\host_session.ps1 -Scenario $Scenario"
    }
    Write-Host "  4. Judge: scripts\analyze_run.ps1 -HostLog <yours> -JoinLog <theirs> -Scenario $Scenario"
}
