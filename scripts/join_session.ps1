<#
.SYNOPSIS
  Your side of the real remote session (friend-HOSTS topology): launch the
  local JOIN client at the friend's public IP with remote-profile timeouts,
  collect join.log + screenshots, and print the analyze_run.ps1 command to
  judge once the friend's results zip arrives.

.DESCRIPTION
  Pairs with the host-role kit (make_remote_kit.ps1 -Role host -> the friend
  runs friend_host.ps1). Build lockstep matters: the DLL deployed locally must
  be the same build as the kit's (the protocol handshake rejects mixed builds).

  Scenario mode runs the compiled scenario and self-exits; free play
  (-Scenario free) keeps running until you close the window.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostIp 203.0.113.7

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostIp 203.0.113.7 -Scenario free -Save squad1

.EXAMPLE
  # Steam P2P transport: no IPs, no port forwarding. -HostSteamId is the host's
  # steamid64 (or short friend code); they must launch with YOUR SteamID as
  # -PeerSteamId (two-code exchange).
  powershell -ExecutionPolicy Bypass -File scripts\join_session.ps1 -HostSteamId 76561198000000000 -Scenario free -Save squad1
#>
[CmdletBinding()]
param(
    [string]$HostIp = "",
    # Steam P2P transport: the HOST's steamid64 (or short friend code). When set,
    # the connection is brokered by Steam (NAT punch / Valve relay) and -HostIp
    # is ignored entirely.
    [string]$HostSteamId = "",
    [string]$Scenario = "coop_presence",
    # Save to load (defaults from the scenario manifest; must match the kit's).
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [switch]$SkipBuild,
    [int]$FreePlayMinutes = 0
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

$useSteam = ($HostSteamId -ne "")
if (-not $useSteam -and $HostIp -eq "") { throw "Pass -HostIp <ip> (UDP) or -HostSteamId <steamid64> (Steam P2P)." }

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
    Write-Host "NOTE: if you rebuilt since sending the kit, REBUILD + RESEND the kit"
    Write-Host "      (make_remote_kit.ps1 -Role host) - the handshake rejects mixed builds."
}

$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe" }

$prof = $manifest.Profiles["remote"]
$stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
$outDir = Join-Path $repoRoot "tools\test-runs\session_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$joinLog = Join-Path $outDir "join.log"

$env:KENSHICOOP_MODE         = "join"
$env:KENSHICOOP_IP           = $HostIp
$env:KENSHICOOP_PORT         = "$Port"
$env:KENSHICOOP_SAVE         = $Save
$env:KENSHICOOP_LOG          = $joinLog
$env:KENSHICOOP_SCENARIO     = $Scenario
$env:KENSHICOOP_SETUP        = ""
$env:KENSHICOOP_TEST_SECONDS = if (-not $isFree) { "600" }
                               elseif ($FreePlayMinutes -gt 0) { "$($FreePlayMinutes * 60)" }
                               else { "0" }
$env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
$env:KENSHICOOP_ARM_TIMEOUT_MS     = "$($prof.ArmTimeoutMs)"
if ($useSteam) {
    $hostId64 = ConvertTo-SteamId64 $HostSteamId
    $env:KENSHICOOP_TRANSPORT  = "steam"
    $env:KENSHICOOP_STEAM_PEER = "$hostId64"
    Write-Host "Transport: STEAM P2P -> host SteamID $hostId64 (no IP / port forwarding)."
    Write-Host "Reminder: the host must launch with YOUR SteamID as -PeerSteamId."
} else {
    $env:KENSHICOOP_TRANSPORT  = "udp"
    $env:KENSHICOOP_STEAM_PEER = "0"
}

$target = if ($useSteam) { "steam:$(ConvertTo-SteamId64 $HostSteamId)" } else { "${HostIp}:$Port" }
Write-Host "Launching JOIN -> $target (save '$Save', scenario '$Scenario') ..."
$out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $joinExe -WorkDir $JoinDir -TimeoutSec $prof.StartTimeoutSec 6>&1
$out | ForEach-Object { Write-Host "    $_" }
$gamePid = 0
$line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
if ($gamePid -eq 0) { throw "Join failed to get past the launcher." }

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

if (Wait-ForLogLine -File $joinLog -Pattern "peer connected" -TimeoutSec 300) {
    Write-Host "CONNECTED to the host."
} elseif ($useSteam) {
    Write-Warning "No connection after 5 min - is the host up, in Steam-online mode, and launched with YOUR SteamID as -PeerSteamId?"
} else {
    Write-Warning "No connection after 5 min - is the host up + port-forwarded?"
}

if ($Scenario -ne "") {
    if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec $prof.JoinAnchorTimeoutSec) {
        Start-Sleep -Seconds 2
        try {
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") `
                -ProcessId $gamePid -Out (Join-Path $outDir "join.png") -Frames 5 -IntervalMs 16
        } catch {}
    }
    Write-Host "Waiting for the scenario to finish (self-exit; 11 min hard cap) ..."
    try { Wait-Process -Id $gamePid -Timeout 660 -ErrorAction Stop }
    catch {
        Write-Warning "Join did not self-exit; killing."
        try { Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue } catch {}
    }
} else {
    Write-Host "FREE PLAY: close the game window when the session ends."
    Wait-Process -Id $gamePid -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "== Session over =="
Write-Host "  join log: $joinLog"
Write-Host ""
Write-Host "When the friend's results zip arrives, extract host.log into $outDir and judge:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_run.ps1 -RunDir $outDir -Scenario $(if ($isFree) { '<scenario>' } else { $Scenario })"
