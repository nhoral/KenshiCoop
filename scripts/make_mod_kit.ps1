<#
.SYNOPSIS
  Package the PLAYER release: a single folder called "KenshiCoop" with the mod
  files inside, that a player copies straight into <Kenshi>\mods\. No install
  scripts, no launchers, no bundled save.

.DESCRIPTION
  Assembles dist\mod-kit\ as:
    KenshiCoop\                 <- the drop-in mod folder (copy this into mods\)
      KenshiCoop.dll              the plugin (protocol-version-matched; a mismatch
                                  is rejected at handshake by design)
      KenshiCoop.mod              mod-list entry so it shows in Kenshi's Mods menu
      RE_Kenshi.json              tells RE_Kenshi to load the plugin
      coop_config.json            only needed for LAN/direct-UDP; Steam play is
                                  configured entirely in-game (F2)
    README.txt                  <- plain copy-the-folder instructions (NOT copied
                                  into mods, so it never clutters the game folder)
  ...then zips it to dist\KenshiCoop-kit.zip (the release artifact the README
  and the GitHub release point at).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1

.EXAMPLE
  # Reuse the current build instead of recompiling.
  powershell -ExecutionPolicy Bypass -File scripts\make_mod_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # Where to find KenshiCoop.mod / RE_Kenshi.json if they aren't in dist\mods.
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipBuild) {
    # The PLAYER release ships the Release config: the shipped DLL excludes the
    # scenario harness (~12k lines) and does not define KENSHICOOP_HARNESS
    # (Phase 1 build separation). The test pipeline uses Harness instead.
    Write-Host "=== build plugin (Release / shipped, no scenario harness) ==="
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`" Release"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

# Resolve the four mod files from the first place each exists.
function Resolve-First([string[]]$candidates, [string]$what) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "$what not found (looked in: $($candidates -join '; '))"
}
$dll  = Resolve-First @(
    (Join-Path $repoRoot "src\plugin\x64\Release\KenshiCoop.dll"),
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.dll")
) "KenshiCoop.dll"

# Canonical shipped-DLL hash (Phase 1 provenance). Package from ONE DLL and
# assert the packaged copy is byte-identical to it, so the release artifact's
# SHA-256 is verifiable rather than a mutable file tracked under dist\.
$canonSha = (Get-FileHash -Algorithm SHA256 $dll).Hash
Write-Host "Canonical Release DLL SHA-256: $canonSha"
Write-Host "  source: $dll"
$json = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\RE_Kenshi.json"),
    (Join-Path $HostDir  "mods\KenshiCoop\RE_Kenshi.json")
) "RE_Kenshi.json"
$mod  = Resolve-First @(
    (Join-Path $repoRoot "dist\mods\KenshiCoop\KenshiCoop.mod"),
    (Join-Path $HostDir  "mods\KenshiCoop\KenshiCoop.mod")
) "KenshiCoop.mod"

# Rebuild dist\mod-kit from scratch so no stale install script survives.
$kitDir  = Join-Path $repoRoot "dist\mod-kit"
$modDir  = Join-Path $kitDir "KenshiCoop"
if (Test-Path $kitDir) { Remove-Item -Recurse -Force $kitDir }
New-Item -ItemType Directory -Force -Path $modDir | Out-Null

Write-Host "=== assembling KenshiCoop mod folder ==="
Copy-Item $dll  (Join-Path $modDir "KenshiCoop.dll")
Copy-Item $json (Join-Path $modDir "RE_Kenshi.json")
Copy-Item $mod  (Join-Path $modDir "KenshiCoop.mod")

# coop_config.json (LAN/UDP only; Steam play needs no config). Written fresh so
# the release always ships a clean default.
@'
{
  // KenshiCoop config. For a normal Steam game you do NOT need to edit this file:
  // your friend's Steam ID is entered in-game (press F2, click "Copy my Steam ID"
  // to share yours, then "Paste friend's Steam ID" to enter theirs), and nothing
  // is written back to disk.
  //
  // This file only matters for a LAN / direct-UDP game: set "transport": "udp"
  // and put the host's address in "ip" (and "port" if you changed it). ip/port are
  // re-read each time you click Connect, so you can edit them without restarting.
  "transport": "steam",
  "ip": "127.0.0.1",
  "port": 27800,
  "autoConnect": false
}
'@ | Set-Content (Join-Path $modDir "coop_config.json") -Encoding UTF8

# Top-level README (sibling to the KenshiCoop folder, so it is NOT copied into
# the game). Plain "copy the folder" instructions - no install script.
@'
KenshiCoop - co-op mod
======================

This zip contains ONE folder: "KenshiCoop". That folder IS the mod.

INSTALL (both players)
----------------------
  1. Right-click the downloaded zip > Properties > Unblock (if shown), then
     extract it.
  2. Copy the "KenshiCoop" folder into your Kenshi mods folder:
       <Kenshi>\mods\
     so you end up with:
       <Kenshi>\mods\KenshiCoop\KenshiCoop.dll   (and the other files)
     The default Steam path is:
       C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
  3. Launch Kenshi and enable "KenshiCoop" in the Mods menu.

PREREQUISITES (both players)
----------------------------
  1. Kenshi 1.0.65 (Steam), set to WINDOWED mode:
     launch Kenshi once, Options > Video > un-check Full Screen.
  2. RE_Kenshi 0.3.1+ (free mod that loads the plugin):
     https://www.nexusmods.com/kenshi/mods/847
  3. For the Steam transport (recommended): Steam RUNNING and ONLINE on both
     machines. No port forwarding, no IPs, no config editing - you swap Steam
     IDs in-game (see PLAY below).

PLAY (Steam - recommended)
--------------------------
  1. Press F2 to open the Co-op panel. It works at the MAIN MENU (before loading
     a game) as well as in-game.
  2. Swap Steam IDs: each player clicks "Copy my Steam ID" and sends it to the
     other (Steam chat, Discord, etc.). When you receive your friend's ID, copy
     it, then click "Paste friend's Steam ID" in your panel. The panel shows the
     ID it captured. (This is per-session - re-paste it if you relaunch Kenshi.)
  3. HOST: load the save you want to play (or start a new game), set Role: HOST,
     leave Transport on STEAM, and toggle Connection to ONLINE.
  4. JOIN: straight from the MAIN MENU - no save needed - set Role: JOIN, leave
     Transport on STEAM, and toggle Connection to ONLINE. The host sends its
     world to you on connect and you load right into it. (You do NOT need the
     host's save beforehand. If you already have an identical copy on disk it is
     used as-is instead of transferring.)
  5. The white status line shows live state (and a banner over your leader shows
     it too, in-game). Toggle Connection to OFFLINE to leave.

PLAY (LAN / direct UDP - advanced)
----------------------------------
  Skip the Steam ID swap. Open <Kenshi>\mods\KenshiCoop\coop_config.json in
  Notepad, set "transport": "udp", and put the host's address in "ip" (and
  "port" if you changed it). In the panel set Transport: UDP, pick Host/Join,
  and go ONLINE. ip/port are re-read whenever you go ONLINE, so no restart is
  needed after an edit.

UNINSTALL
---------
  Delete <Kenshi>\mods\KenshiCoop. Nothing else is touched.

TROUBLESHOOTING
---------------
  * "The co-op plugin has not started": RE_Kenshi didn't load it. Check
    <Kenshi>\RE_Kenshi_log.txt for 'KenshiCoop'; reinstalling RE_Kenshi
    usually fixes it.
  * No connection (Steam): both Steams must be RUNNING and ONLINE, and each side
    must have Pasted the OTHER player's ID (the panel shows the captured ID -
    confirm it matches). If "Paste friend's Steam ID" says the clipboard wasn't
    a Steam ID, have your friend re-copy theirs with "Copy my Steam ID". Look for
    '[steam] session ... active=1' in <Kenshi>\KenshiCoop_*.log.
  * "protocol mismatch": one player has an older/newer build; both should use
    the same release.
'@ | Set-Content (Join-Path $kitDir "README.txt") -Encoding UTF8

# Provenance: assert the PACKAGED DLL is byte-identical to the canonical build,
# then record the hash next to the kit so the release artifact is verifiable.
$packagedDll = Join-Path $modDir "KenshiCoop.dll"
$packagedSha = (Get-FileHash -Algorithm SHA256 $packagedDll).Hash
if ($packagedSha -ne $canonSha) {
    throw "packaged DLL hash ($packagedSha) != canonical Release DLL hash ($canonSha)"
}
$protoLine = Select-String -Path (Join-Path $repoRoot "src\netproto\Wire.h") `
    -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' | Select-Object -First 1
$proto = if ($protoLine) { $protoLine.Matches[0].Groups[1].Value } else { "?" }
@{
    dllSha256       = $canonSha
    protocolVersion = $proto
    builtUtc        = (Get-Date).ToUniversalTime().ToString("o")
    config          = "Release"
} | ConvertTo-Json | Set-Content (Join-Path $kitDir "PROVENANCE.json") -Encoding UTF8
Write-Host "Packaged DLL SHA-256 verified == canonical."

# Zip: the archive contains the KenshiCoop\ folder + README.txt + PROVENANCE.json.
$zip = Join-Path $repoRoot "dist\KenshiCoop-kit.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $kitDir "*") -DestinationPath $zip

Write-Host ""
Write-Host "Mod folder: $modDir"
Write-Host "Kit zipped: $zip"
Get-ChildItem -Recurse $kitDir | ForEach-Object {
    Write-Host ("  " + $_.FullName.Substring($kitDir.Length + 1))
}
