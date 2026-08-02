<#
.SYNOPSIS
  Three-player harness (protocol 49): launch host + two joins on loopback UDP,
  let the session run, then judge the logs.

  Two modes:
    * SMOKE (default, -Scenario ""): the free-play session is the scenario and
      the gates are direct log greps - admit (rank claims), roster propagation,
      relay-side liveness, speed-vote reduction, and the absence of the legacy
      "3+ players unsupported" path.
    * SCENARIO (-Scenario coop_presence): all three instances run the named
      scenario (the rank-generalized coop_presence: each client WALKS its own
      tab's member and logs MEMBER for its own rank + RECV for every other
      rank). On top of the smoke gates, the run gates SCENARIO RESULT PASS on
      all three logs and the SIX-direction positional cross-check
      (Test-CoopPresence3p) - including the join1<->join2 pairs that only
      exist via the host relay. Needs the 3-tab fixture (default save
      'squad3'; ranks 0/1/2 each hold a member).

  -Wan <profile> (scenario mode): route EACH join through its own netsim relay
  (dist\netsim.exe, profiles from scenarios.psd1 WanProfiles) so the relayed
  join<->join traffic really crosses two impaired links.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_3p_smoke.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_3p_smoke.ps1 -Scenario coop_presence
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_3p_smoke.ps1 -Scenario coop_presence -Wan bad
#>
[CmdletBinding()]
param(
    [string]$Save = "",
    [string]$Scenario = "",
    [double]$Tolerance = 0,
    [string]$Wan = "",
    [int]$Seconds = 0,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir  = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir  = "$env:USERPROFILE\Kenshi-Join",
    [string]$Join2Dir = "$env:USERPROFILE\Kenshi-Join2",
    [int]$StartTimeoutSec = 120,
    [int]$JoinDelaySec = 8,
    [string]$OutDir = "",
    # Window staging (mirrors run_test.ps1): spread the three client windows
    # host | join1 | join2 on the widest monitor, re-pinned through the loads.
    [switch]$NoArrange,
    [int]$ArrangeRepeatSec = 150
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

# Mode-dependent defaults. Scenario runs need the 3-tab fixture and a longer
# session: the scenario clocks arm only when the LAST participant streams
# (KENSHICOOP_ARM_MIN_PEERS=2), and the host window is 44 s after that.
if ($Save -eq "")   { $Save   = if ($Scenario -ne "") { "squad3" } else { "squad1" } }
if ($Seconds -eq 0) { $Seconds = if ($Scenario -ne "") { 150 } else { 90 } }
if ($Tolerance -le 0) {
    # Manifest default (Invoke-RunAnalysis3p applies WanTolerance / the 2x
    # relay allowance itself when -Wan is active).
    $m = Import-PowerShellDataFile -Path (Join-Path $scriptDir "scenarios.psd1")
    $Tolerance = if ($Scenario -ne "" -and $m.Scenarios.ContainsKey($Scenario)) {
        [double]$m.Scenarios[$Scenario].Tolerance } else { 3.0 }
}
$armTimeoutMs = if ($Scenario -ne "") { 120000 } else { 45000 }

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $sub    = if ($Scenario -ne "") { "3p_$Scenario" } else { "3p_smoke" }
    if ($Wan -ne "") { $sub = "${sub}_wan_$Wan" }
    $OutDir = Join-Path $repoRoot "out\$sub\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$hostLog  = Join-Path $OutDir "host.log"
$join1Log = Join-Path $OutDir "join1.log"
$join2Log = Join-Path $OutDir "join2.log"

# ---- Preflight ---------------------------------------------------------------
foreach ($d in @($HostDir, $JoinDir, $Join2Dir)) {
    if (-not (Test-Path (Join-Path $d "kenshi_x64.exe"))) { throw "No Kenshi at $d" }
    if (-not (Test-Path (Join-Path $d "mods\KenshiCoop\KenshiCoop.dll"))) { throw "KenshiCoop not deployed in $d" }
}

# Stale instances would steal the port / fight the save restore.
$stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
    Write-Host "killing $($stale.Count) stale Kenshi process(es) ..."
    $stale | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# Pristine fixture (shared %LOCALAPPDATA% save root - all three read it).
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "deploy_saves.ps1") -Save $Save
if ($LASTEXITCODE -ne 0) { throw "deploy_saves.ps1 failed for '$Save'" }

# Windowed video mode in all three installs (three fullscreen instances would
# fight for the display). set_video_mode covers host+join; patch join2 alike.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
    -Width 1280 -Height 1024 -HostDir $HostDir -JoinDir $JoinDir
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
    -Width 1280 -Height 1024 -HostDir $Join2Dir -JoinDir $Join2Dir

# ---- WAN relay proxies (below-ENet delay/jitter/loss, ONE PER JOIN) -----------
# netsim.exe is single-client by design, so a 3P WAN run gets two instances:
# join1 -> Port+1, join2 -> Port+2, both forwarding to the host at ${Ip}:$Port.
$wanProcs   = @()
$join1Ip    = $Ip;  $join1Port = $Port
$join2Ip    = $Ip;  $join2Port = $Port
if ($Wan -ne "") {
    $manifest = Import-PowerShellDataFile -Path (Join-Path $scriptDir "scenarios.psd1")
    if (-not $manifest.WanProfiles.ContainsKey($Wan)) {
        $names = ($manifest.WanProfiles.Keys | Sort-Object) -join ", "
        throw "Unknown WAN profile '$Wan'. Available: $names"
    }
    $wp = $manifest.WanProfiles[$Wan]
    $netsimExe = Join-Path $repoRoot "dist\netsim.exe"
    if (-not (Test-Path $netsimExe)) {
        throw "dist\netsim.exe not found - build it first: cmd /c scripts\build_netsim.cmd"
    }
    foreach ($j in @(1, 2)) {
        $proxyPort = $Port + $j
        Write-Host "Starting WAN relay proxy '$Wan' for join$j (delay $($wp.DelayMs)ms +/-$($wp.JitterMs)ms, loss $($wp.LossPct)%) on port $proxyPort -> ${Ip}:$Port"
        $wanLog = Join-Path $OutDir "netsim_join$j.log"
        $p = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $wanLog `
            -ArgumentList @("$proxyPort", $Ip, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
        Start-Sleep -Milliseconds 500
        if ($p.HasExited) { throw "netsim.exe (join$j) exited immediately (see $wanLog)" }
        $wanProcs += $p
    }
    $join1Ip = "127.0.0.1"; $join1Port = $Port + 1
    $join2Ip = "127.0.0.1"; $join2Port = $Port + 2
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

function Set-CoopEnv {
    param([string]$Mode, [string]$Log, [string]$RankClaim,
          [string]$PeerIp = "", [int]$PeerPort = 0)
    $env:KENSHICOOP_MODE           = $Mode
    $env:KENSHICOOP_TRANSPORT      = "udp"      # loopback: Steam P2P impossible on one account
    $env:KENSHICOOP_STEAM_PEER     = "0"
    $env:KENSHICOOP_IP             = if ($PeerIp -ne "") { $PeerIp } else { $Ip }
    $env:KENSHICOOP_PORT           = if ($PeerPort -ne 0) { "$PeerPort" } else { "$Port" }
    $env:KENSHICOOP_SAVE           = $Save
    $env:KENSHICOOP_TEST_SECONDS   = "$Seconds"
    $env:KENSHICOOP_LOG            = $Log
    $env:KENSHICOOP_SCENARIO       = $Scenario
    $env:KENSHICOOP_SETUP          = ""
    $env:KENSHICOOP_OWN_RANK_CLAIM = $RankClaim  # "" = role default (join -> 1)
    $env:KENSHICOOP_OWN_SQUAD      = ""
    $env:KENSHICOOP_OWN_RANK       = ""
    $env:KENSHICOOP_ARM_TIMEOUT_MS = "$armTimeoutMs"
    # Scenario mode: arm every instance's scenario clock only once it has seen
    # entity batches from BOTH peers (host: both joins; each join: the host AND
    # the other join via relay) so the three windows overlap.
    $env:KENSHICOOP_ARM_MIN_PEERS  = if ($Scenario -ne "") { "2" } else { "1" }
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
    $env:KENSHICOOP_NETSIM_DELAY_MS = "0"
    $env:KENSHICOOP_NETSIM_JITTER_MS = "0"
    $env:KENSHICOOP_NETSIM_LOSS_PCT = "0"
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

# ---- Launch ------------------------------------------------------------------
Write-Host "== 3-player run: save=$Save scenario='$Scenario' wan='$Wan' seconds=$Seconds =="
Write-Host "Launching HOST ..."
Set-CoopEnv -Mode "host" -Log $hostLog -RankClaim ""
$hostPid = Start-PastLauncher -Exe (Join-Path $HostDir "kenshi_x64.exe") -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

Write-Host "Waiting for HOST gameplay (timeout ${StartTimeoutSec}s) ..."
if (-not (Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec)) {
    Write-Warning "Host not in gameplay after ${StartTimeoutSec}s; continuing anyway."
}
Start-Sleep -Seconds $JoinDelaySec

Write-Host "Launching JOIN 1 (squad slot 1) ..."
Set-CoopEnv -Mode "join" -Log $join1Log -RankClaim "" -PeerIp $join1Ip -PeerPort $join1Port
$join1Pid = Start-PastLauncher -Exe (Join-Path $JoinDir "kenshi_x64.exe") -WorkDir $JoinDir
if ($join1Pid -eq 0) { Write-Warning "Join 1 failed to get past the launcher." }

# Serialize the loads: wait for join1's admit before launching join2.
[void](Wait-ForLogLine -File $hostLog -Pattern "peer connected id=1" -TimeoutSec $StartTimeoutSec)
Start-Sleep -Seconds $JoinDelaySec

Write-Host "Launching JOIN 2 (squad slot 2) ..."
Set-CoopEnv -Mode "join" -Log $join2Log -RankClaim "2" -PeerIp $join2Ip -PeerPort $join2Port
$join2Pid = Start-PastLauncher -Exe (Join-Path $Join2Dir "kenshi_x64.exe") -WorkDir $Join2Dir
if ($join2Pid -eq 0) { Write-Warning "Join 2 failed to get past the launcher." }

Write-Host "PIDs: host=$hostPid join1=$join1Pid join2=$join2Pid"

# Spread the three windows out (host | join1 | join2), like the 2P harness.
# Launched in the background right after the last client; it polls for all
# three game windows and re-pins the placement through the load screens
# (Kenshi re-centers its window on the load->gameplay switch).
if (-not $NoArrange -and $hostPid -ne 0) {
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host "Arranging windows (host | join1 | join2; re-pinning ${ArrangeRepeatSec}s) ..."
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$hostPid", "-JoinPid", "$join1Pid", "-Join2Pid", "$join2Pid",
        "-TimeoutSec", "120", "-RepeatSec", "$ArrangeRepeatSec"
    ) | Out-Null
}

# ---- Wait out the session ----------------------------------------------------
# Self-exit fires TEST_SECONDS after each instance's gameplay start; the last
# joiner starts latest, so give the whole rig a generous deadline then sweep.
$deadline = (Get-Date).AddSeconds($Seconds + $StartTimeoutSec + 120)
while ((Get-Date) -lt $deadline) {
    $alive = @($hostPid, $join1Pid, $join2Pid) | Where-Object { $_ -ne 0 -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) }
    if ($alive.Count -eq 0) { break }
    Start-Sleep -Seconds 5
}
$left = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($left.Count -gt 0) {
    Write-Warning "$($left.Count) instance(s) still alive at deadline; killing."
    $left | Stop-Process -Force -ErrorAction SilentlyContinue
}
foreach ($p in $wanProcs) {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}

# ---- Judge -------------------------------------------------------------------
$fails = New-Object System.Collections.Generic.List[string]
$notes = New-Object System.Collections.Generic.List[string]

function Gate {
    param([string]$Name, [string]$File, [string]$Pattern, [bool]$MustExist = $true)
    $hit = $null
    if (Test-Path $File) {
        $hit = Select-String -Path $File -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    $found = ($null -ne $hit)
    if ($found -eq $MustExist) {
        Write-Host ("  ok   {0}" -f $Name)
        if ($found) { $notes.Add("$Name :: $($hit.Line.Trim())") }
    } else {
        Write-Host ("  FAIL {0}" -f $Name)
        $fails.Add($Name)
    }
}

Write-Host "`n== 3-player smoke gates =="
# Admit: both joins accepted with their claimed slots.
Gate "host admits join1 (id=1 rank=1)" $hostLog "peer connected id=1 rank=1"
Gate "host admits join2 (id=2 rank=2)" $hostLog "peer connected id=2 rank=2"
# Roster propagation: each join learns the other through PKT_PEER_STATUS.
Gate "join1 sees join2 in roster" $join1Log "peer roster: id=2 rank=2 joined"
Gate "join2 sees join1 in roster" $join2Log "peer roster: id=1 rank=1 joined"
# Game-thread handshake surfaced on the joins for the OTHER join's owner id.
Gate "join1 game thread saw owner 2" $join1Log "handshake: peer present id=2"
Gate "join2 game thread saw owner 1" $join2Log "handshake: peer present id=1"
# Speed consensus: the host reduced over BOTH joins' votes.
Gate "host received join1 speed vote" $hostLog "\[speed\] REQ RECV owner=1"
Gate "host received join2 speed vote" $hostLog "\[speed\] REQ RECV owner=2"
# The legacy 2-player refusals must be GONE.
Gate "no '3+ players unsupported'" $hostLog "3\+ players unsupported" $false
Gate "no 'session full'"           $hostLog "session full"            $false
Gate "no 'not joinable'"           $hostLog "not joinable"            $false
foreach ($pair in @(@("host", $hostLog), @("join1", $join1Log), @("join2", $join2Log))) {
    Gate "no protocol mismatch ($($pair[0]))" $pair[1] "protocol mismatch" $false
}

if ($Scenario -ne "") {
    Write-Host "`n== 3-player scenario gates ($Scenario) =="
    Gate "host scenario armed"  $hostLog  "SCENARIO arm trigger="
    Gate "join1 scenario armed" $join1Log "SCENARIO arm trigger="
    Gate "join2 scenario armed" $join2Log "SCENARIO arm trigger="

    # Full oracle verdict over the three logs: the 2P regression battery
    # (health, CHECK FAIL, SCENARIO RESULT, and the manifest's gating/advisory
    # oracles) once per host<->join pair, PLUS the relayed join<->join
    # cross-check as the run's primary gate. Writes verdict.json for trending.
    Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
    $verdict = Invoke-RunAnalysis3p -HostLog $hostLog -Join1Log $join1Log -Join2Log $join2Log `
        -Scenario $Scenario -Tolerance $Tolerance -WanActive ($Wan -ne "") `
        -RunInfo @{ save = $Save; seconds = $Seconds; wan = $Wan; port = $Port } `
        -OutJson (Join-Path $OutDir "verdict.json")
    if (-not $verdict.pass) { $fails.Add("oracle verdict ($($verdict.reasons -join '; '))") }
    else { $notes.Add("oracle verdict :: PASS (tol=$($verdict.tolerance), relayTol=$($verdict.relayTolerance))") }
}

Write-Host "`n== supporting evidence =="
foreach ($n in $notes) { Write-Host "  $n" }

Write-Host "`nLogs: $OutDir"
$label = if ($Scenario -ne "") { "3P $Scenario" } else { "3P SMOKE" }
if ($Wan -ne "") { $label = "$label (wan=$Wan)" }
if ($fails.Count -eq 0) {
    Write-Host "$label RESULT: PASS"
    exit 0
} else {
    Write-Host "$label RESULT: FAIL ($($fails.Count) gate(s): $($fails -join ', '))"
    exit 1
}
