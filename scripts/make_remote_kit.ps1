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

$dll  = Join-Path $repoRoot "src\plugin\x64\Harness\KenshiCoop.dll"
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
# Preflight helpers (RE_Kenshi / Steam / Mark-of-the-Web checks), dot-sourced
# by the friend script.
Copy-Item (Join-Path $scriptDir "kit_preflight.ps1") "$kitDir\kit_preflight.ps1"
# Double-click launcher matching the kit's role: prompts for the other
# player's Steam code and runs the friend script (no PowerShell knowledge
# needed).
$launcher = if ($Role -eq "host") { "HOST.cmd" } else { "JOIN.cmd" }
Copy-Item (Join-Path $scriptDir $launcher) "$kitDir\$launcher"
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

$steamKit   = ($Transport -eq "steam")
$launcherUC = $launcher
$psCmd = if ($Role -eq "host") {
    if ($steamKit) { "powershell -ExecutionPolicy Bypass -File friend_host.ps1 -PeerSteamId <their code>" }
    else           { "powershell -ExecutionPolicy Bypass -File friend_host.ps1" }
} else {
    if ($steamKit) { "powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostSteamId <their code>" }
    else           { "powershell -ExecutionPolicy Bypass -File friend_join.ps1 -HostIp <the host's public IP>" }
}

@"
KenshiCoop co-op kit  (your role: $($Role.ToUpper()))
=====================================

QUICK START
-----------

Step 0 - one-time prerequisites (both players):

  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode: launch Kenshi once,
     Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ installed - a free mod that loads the co-op plugin:
     https://www.nexusmods.com/kenshi/mods/847
$(if ($steamKit) {
"  3. Steam RUNNING and ONLINE on both machines. That is the whole network
     setup - the connection goes through Steam, so there is no port
     forwarding, no router setup, and no IP addresses."
} else {
"  3. This is a direct-UDP kit: the HOST's UDP port $Port must be reachable
     from the internet. The host script sets up the Windows Firewall and
     asks the router to forward the port automatically (UPnP); run
     'powershell -ExecutionPolicy Bypass -File friend_host.ps1 -CheckOnly'
     the day before to verify off the clock."
})

$(if ($steamKit) {
"Step 1 - swap Steam friend codes:

  Each player needs the OTHER player's code before connecting. The
  launcher reads YOUR code from Steam and prints it on screen the moment
  you start it - just read it to the other player and type theirs in.
  (Codes are also in Steam under Friends > Add a Friend; a 17-digit
  SteamID64 works too.)"
} else {
"Step 1 - get the HOST's public IP:

  The host player finds it at whatismyip.com (friend_host.ps1 also prints
  it) and sends it to the joining player before launching."
})

Step 2 - launch:

  Double-click $launcherUC in this folder. It asks for the other player's
  $(if ($steamKit) { "code" } else { "details" }), then does everything else: checks your setup (and tells you
  exactly what to fix if something is missing), installs the mod and the
  shared starter save, launches Kenshi, and connects the two games. When
  both of you are in-game, you're playing co-op.

  Prefer a terminal? The same flow is:
    $psCmd

WHAT SYNCS
----------

In free play the full sync set is active by default: positions, combat,
health/limbs, stats, game speed, carried bodies, inventory + equipment
changes, items dropped on the ground, direct trades between the two
squads, and squad management (recruits you hire AND units you move
between squad tabs mid-session stay tracked on the other machine - note
they won't appear in the other player's squad UI, that's a known
limitation).

Base-building syncs too: placed buildings, construction progress, doors,
dismantling, production machines - power switches, generators, crafting
bench / furnace / drill output, input fuel and farm growth - and
container CONTENTS: every storage chest and machine inventory near the
players holds the same items on both machines.

Saves are coordinated: any save either player makes during a connected
session becomes ONE shared save - the host's game writes it and streams
the whole save folder to the other machine automatically (verified +
committed only when it arrives intact). Loading works the same way: when
either player loads a save mid-session, both games load the identical
save and the session continues from there (expect a normal load screen
on both sides).

CHOOSING A SAVE
---------------

The launcher asks which save to play (both players must pick the same):

  [1] The bundled starter save - the default. A fresh start on the
      two-squad co-op save that ships with this kit.
  [2] Your own save, picked in-game - both games start on the bundled
      save so they can connect; once BOTH players are in-game, the HOST
      opens Kenshi's menu > Load and picks any save. The other player's
      game follows automatically (the save is streamed over first if
      their copy differs). This is also how you RESUME a previous co-op
      session: any save either player made last time is already on both
      machines - just load it. Tip: the second player controls the
      save's SECOND squad tab - move some units into a new squad tab to
      give them a crew.

UNINSTALL
---------

Nothing else on your machine is touched. To remove: delete
<Kenshi>\mods\KenshiCoop and the test save folder.

TROUBLESHOOTING
---------------

  * "Running scripts is disabled" or Windows SmartScreen blocks the kit:
    right-click the downloaded zip BEFORE extracting, Properties > Unblock.
    The $launcherUC launcher also bypasses this automatically.
  * "RE_Kenshi not found": install RE_Kenshi into your Kenshi folder
    (https://www.nexusmods.com/kenshi/mods/847) and run the launcher again.
  * "The co-op plugin has not started": the game launched but RE_Kenshi did
    not load the plugin. Check <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop';
    reinstalling RE_Kenshi usually fixes it.
$(if ($steamKit) {
"  * No connection: both Steams must be RUNNING and ONLINE (not offline
    mode), and each side must be launched with the OTHER player's code -
    a typo'd code fails silently, so re-check both. Look for
    '[steam] session ... active=1' in the log - relay=1 just means Valve
    is relaying (works fine, slightly higher ping)."
} else {
"  * No connection: UDP $Port must be reachable on the HOST. friend_host.ps1
    handles the Windows Firewall and asks the router to open the port via
    UPnP; if the script said UPnP failed, forward UDP $Port to the host PC
    manually on the router's 'port forwarding' page. If it warned about
    carrier-grade NAT, forwarding won't help - use a VPN overlay such as
    Tailscale and share that IP instead."
})
  * "protocol mismatch" in the log: your kit is older/newer than the other
    player's build; both players should get the latest kit.
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
