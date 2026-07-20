<#
.SYNOPSIS
  Functional test runner for KenshiCoop: launch host + join, auto-load a save,
  run for a fixed time (or a compiled scenario), self-exit, then collect
  per-client logs and screenshots and judge the run with the shared oracle
  library (scripts/CoopOracles.psm1 + scripts/scenarios.psd1).

.DESCRIPTION
  Relies on the plugin's env-var driven behavior:
    KENSHICOOP_SAVE          - save to auto-load on the title screen
    KENSHICOOP_TEST_SECONDS  - self-exit this many seconds after gameplay starts
    KENSHICOOP_LOG           - dedicated, per-line-flushed log file
  Each client writes its own log; we time a screenshot of each window while both
  are still in-game, then wait for them to self-exit (with a hard-timeout kill as
  a safety net).

  Host runs from the Steam install; join runs from the separate Kenshi-Join
  install (scripts\setup_join_install.cmd). With -Sync, host saves are mirrored
  into the join install first (required so both can load the same save).

  Scenario runs are judged by the manifest-declared oracle set (three-state
  PASS/FAIL/SKIP gates, no-signal guard on the primary gate) and produce a
  verdict.json in the out dir for trending. See scripts/CoopOracles.psm1.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_test.ps1 -Scenario coop_presence

.EXAMPLE
  # Same scenario through the WAN relay proxy ('bad' profile: 120ms +/-40ms, 5% loss
  # both directions, below ENet) with 30s of injected join clock skew:
  powershell -ExecutionPolicy Bypass -File scripts\run_test.ps1 -Scenario coop_presence -Wan bad -FakeClockSkewMs 30000
#>
[CmdletBinding()]
param(
    # Save both clients load. Defaults from the scenario manifest when -Scenario
    # is given; required otherwise.
    [string]$Save = "",
    [int]$Seconds = 60,
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [switch]$Sync,
    [switch]$NoKill,
    [int]$JoinDelaySec = 8,
    [int]$ShotLeadSec = 5,
    [int]$StartTimeoutSec = 90,
    [int]$Frames = 5,
    [int]$FrameIntervalMs = 16,
    # Scenario mode: both clients run the compiled scenario named here
    # (KENSHICOOP_SCENARIO). The scenario self-exits when complete; the verdict
    # comes from the manifest-declared oracle set.
    [string]$Scenario = "",
    # Host-only setup scene (KENSHICOOP_SETUP). Defaults from the manifest.
    [string]$Setup = "",
    [double]$Tolerance = 0,
    [int]$ScenarioShotDelaySec = 5,
    [int]$JoinAnchorTimeoutSec = 45,
    [switch]$ProbeAiSuspend,
    [int]$ScenarioWaitSec = 25,
    [switch]$Arrange,
    [switch]$NoArrange,
    [ValidateSet("widest", "primary")]
    [string]$ArrangeMonitor = "primary",
    [int]$ArrangeRepeatSec = 75,
    # Legacy in-plugin WAN sim (inbound ENTITY batches only, above ENet). Kept for
    # targeted entity-only experiments; full-fidelity WAN testing should use -Wan.
    [int]$NetSimDelayMs = 0,
    [int]$NetSimJitterMs = 0,
    [int]$NetSimLossPct = 0,
    # WAN relay proxy profile (scenarios.psd1 WanProfiles: regional|bad|awful).
    # Launches dist\netsim.exe between the join and the host so delay/jitter/loss
    # applies to ALL datagrams in BOTH directions BELOW ENet - reliable-channel
    # retransmission, handshake and all packet families are genuinely exercised.
    [string]$Wan = "",
    # Inject a fake wall-clock skew (ms) into the JOIN (KENSHICOOP_FAKE_CLOCK_SKEW_MS):
    # its log timestamps AND its time-sync pings shift together, so the run
    # validates that CLOCKSYNC offset estimation + oracle alignment recover it.
    [int]$FakeClockSkewMs = 0,
    # Harness timeout profile from the manifest (loopback|remote).
    [string]$Profile = "loopback"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
$manifest = Get-ScenarioManifest

# ---- Resolve manifest defaults ------------------------------------------------
$manifestEntry = $null
if ($Scenario -ne "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $manifestEntry = $manifest.Scenarios[$Scenario]
    if ($Save -eq "")      { $Save = $manifestEntry.Save }
    if ($Setup -eq "" -and $manifestEntry.Setup -ne "") { $Setup = $manifestEntry.Setup }
    if ($Tolerance -eq 0)  { $Tolerance = $manifestEntry.Tolerance }
}
if ($Tolerance -eq 0) { $Tolerance = 3.0 }
if ($Save -eq "") { throw "No -Save given and scenario '$Scenario' has no manifest default." }

# Timeout profile (explicit parameters win over the profile).
$armTimeoutMs = 45000
if ($manifest.Profiles.ContainsKey($Profile)) {
    $prof = $manifest.Profiles[$Profile]
    if (-not $PSBoundParameters.ContainsKey("JoinDelaySec"))         { $JoinDelaySec         = $prof.JoinDelaySec }
    if (-not $PSBoundParameters.ContainsKey("StartTimeoutSec"))      { $StartTimeoutSec      = $prof.StartTimeoutSec }
    if (-not $PSBoundParameters.ContainsKey("ScenarioWaitSec"))      { $ScenarioWaitSec      = $prof.ScenarioWaitSec }
    if (-not $PSBoundParameters.ContainsKey("JoinAnchorTimeoutSec")) { $JoinAnchorTimeoutSec = $prof.JoinAnchorTimeoutSec }
    if ($prof.ContainsKey("ArmTimeoutMs"))                           { $armTimeoutMs         = $prof.ArmTimeoutMs }
}
# Spike captures keep the legacy immediate arming (many are host-only probes
# that must not idle out their capture window waiting for a peer).
if ($Scenario -eq "spike") { $armTimeoutMs = 0 }

# Scenario clocks now start at PEER-READY (~a join-load after host gameplay), so
# the self-exit backstop measured from gameplay start needs headroom for the
# arming delay + the longest host scenario window (60 s) - unless the caller
# explicitly chose a duration. Long scenarios (travel_parity's 2400 u march)
# override the backstop per-entry in the manifest (Seconds / KillGraceSec).
if ($Scenario -ne "" -and -not $PSBoundParameters.ContainsKey("Seconds")) {
    $Seconds = 150
    if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey("Seconds")) {
        $Seconds = $manifestEntry.Seconds
    }
}

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

# Fail fast on a bad save name (auto-loading a non-existent save crashes the game).
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $Save))) {
    $avail = (Get-ChildItem $saveRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ", "
    throw "Save '$Save' not found in $saveRoot. Available saves: $avail"
}

$hostLog = Join-Path $OutDir "host.log"
$joinLog = Join-Path $OutDir "join.log"
$hostPng = Join-Path $OutDir "host.png"
$joinPng = Join-Path $OutDir "join.png"

# ---- WAN relay proxy (below-ENet delay/jitter/loss) -----------------------------
$wanProc = $null
$wanSeed  = $null
$joinIp   = $Ip
$joinPort = $Port
if ($Wan -ne "") {
    if (-not $manifest.WanProfiles.ContainsKey($Wan)) {
        $names = ($manifest.WanProfiles.Keys | Sort-Object) -join ", "
        throw "Unknown WAN profile '$Wan'. Available: $names"
    }
    $wp = $manifest.WanProfiles[$Wan]
    $netsimExe = Join-Path $repoRoot "dist\netsim.exe"
    if (-not (Test-Path $netsimExe)) {
        throw "dist\netsim.exe not found - build it first: cmd /c scripts\build_netsim.cmd"
    }
    $proxyPort = $Port + 1
    Write-Host "Starting WAN relay proxy '$Wan' (delay $($wp.DelayMs)ms +/-$($wp.JitterMs)ms, loss $($wp.LossPct)%) on port $proxyPort -> ${Ip}:$Port"
    $wanLog = Join-Path $OutDir "netsim.log"
    $wanArgs = @("$proxyPort", $Ip, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
    if ($wp.ContainsKey('StallForS') -and [int]$wp.StallForS -gt 0) {
        # Scripted total outage (starved-replica validation): seed + stall window.
        # The seed is recorded in run.json so a flaky stall run is reproducible.
        $wanSeed = Get-Random -Maximum 1000000
        $wanArgs += @("$wanSeed", "$($wp.StallAtS)", "$($wp.StallForS)")
        Write-Host "  (scripted stall: $($wp.StallForS)s total outage at +$($wp.StallAtS)s after first join datagram)"
    }
    $wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $wanLog `
        -ArgumentList $wanArgs
    Start-Sleep -Milliseconds 500
    if ($wanProc.HasExited) { throw "netsim.exe exited immediately (see $wanLog)" }
    $joinIp   = "127.0.0.1"
    $joinPort = $proxyPort
}

Write-Host "== KenshiCoop test run =="
Write-Host "  save:     $Save"
Write-Host "  seconds:  $Seconds"
if ($Scenario -ne "") { Write-Host "  scenario: $Scenario (tolerance $Tolerance u)" }
if ($Wan -ne "") { Write-Host "  wan:      $Wan (proxy port $joinPort)" }
if ($FakeClockSkewMs -ne 0) { Write-Host "  skew:     join clock +${FakeClockSkewMs}ms (injected)" }
if ($NetSimDelayMs -or $NetSimJitterMs -or $NetSimLossPct) {
    Write-Host "  net sim:  delay ${NetSimDelayMs}ms +/-${NetSimJitterMs}ms, loss ${NetSimLossPct}% (legacy in-plugin, entities only)"
}
Write-Host "  out dir:  $OutDir"

# ---- run.json provenance --------------------------------------------------------
# One machine-readable record of exactly WHAT this run exercised: scenario +
# resolved save/setup, the manifest DiagEnv knobs actually applied, the WAN/skew
# regime (with the stall seed for reproducibility), and the build under test
# (git commit + PROTOCOL_VERSION). Written before launch so it survives a crash.
try {
    $gitCommit = (& git -C $repoRoot rev-parse --short HEAD 2>$null)
    $gitDirty  = [bool]((& git -C $repoRoot status --porcelain 2>$null) )
} catch { $gitCommit = ""; $gitDirty = $false }
$protoVer = ""
try {
    $wireH = Join-Path $repoRoot "src\netproto\Wire.h"
    $pv = Select-String -Path $wireH -Pattern 'PROTOCOL_VERSION\s*=\s*(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pv) { $protoVer = $pv.Matches[0].Groups[1].Value }
} catch {}
$variant = "clean"
if ($Wan -ne "" -and $FakeClockSkewMs -ne 0) { $variant = "wanskew" }
elseif ($Wan -ne "") { $variant = "wan" }
elseif ($FakeClockSkewMs -ne 0) { $variant = "skew" }
$runJson = [ordered]@{
    timestamp     = (Get-Date -Format "yyyy-MM-ddTHH:mm:ss")
    scenario      = $Scenario
    variant       = $variant
    save          = $Save
    setup         = $Setup
    tolerance     = $Tolerance
    seconds       = $Seconds
    port          = $Port
    profile       = $Profile
    diagEnv       = $(if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey('DiagEnv')) { $manifestEntry.DiagEnv } else { @{} })
    wan           = $Wan
    wanProfile    = $(if ($Wan -ne "") { $manifest.WanProfiles[$Wan] } else { $null })
    wanSeed       = $wanSeed
    fakeSkewMs    = $FakeClockSkewMs
    netSimDelayMs = $NetSimDelayMs
    netSimJitterMs= $NetSimJitterMs
    netSimLossPct = $NetSimLossPct
    gitCommit     = $gitCommit
    gitDirty      = $gitDirty
    protocolVersion = $protoVer
    outDir        = $OutDir
}
$runJson | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $OutDir "run.json") -Encoding UTF8

# Clear any leftover Kenshi instances from a previous (possibly crashed) run.
if (-not $NoKill) {
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        Write-Host "  killing $($stale.Count) stale Kenshi process(es) before starting ..."
        $stale | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
}

if ($Sync) {
    Write-Host "Syncing saves host -> join ..."
    & cmd /c "`"$scriptDir\sync_save.cmd`" `"$HostDir`" `"$JoinDir`""
    if ($LASTEXITCODE -ne 0) { throw "sync_save.cmd failed ($LASTEXITCODE)" }
}

function Set-CoopEnv {
    param([string]$Mode, [string]$Log)
    $env:KENSHICOOP_MODE         = $Mode
    # Loopback regression is ALWAYS direct-UDP on 127.0.0.1: two Kenshi instances
    # on one machine (same Steam account) cannot establish a Steam P2P session, so
    # force the transport here rather than inheriting the deployed coop_config.json
    # (which may be left on transport=steam + a real steamPeer from a manual friend
    # session - that silently breaks every scenario's connection). Env overrides the
    # config file, so this makes the harness self-contained. LAN runs use a separate
    # runner (run_lan_test.ps1) and are unaffected.
    $env:KENSHICOOP_TRANSPORT    = "udp"
    $env:KENSHICOOP_STEAM_PEER   = "0"
    # The join connects through the WAN proxy when one is active.
    $env:KENSHICOOP_PORT         = if ($Mode -eq "join") { "$joinPort" } else { "$Port" }
    $env:KENSHICOOP_IP           = if ($Mode -eq "join") { $joinIp } else { $Ip }
    $env:KENSHICOOP_SAVE         = $Save
    $env:KENSHICOOP_TEST_SECONDS = "$Seconds"
    $env:KENSHICOOP_LOG          = $Log
    $env:KENSHICOOP_SCENARIO     = $Scenario
    # Join-only AI-suspend probe (mode+flag specific, so NOT a manifest knob).
    $env:KENSHICOOP_PROBE_AISUSPEND = if ($Mode -eq "join" -and $ProbeAiSuspend) { "1" } else { "" }
    # Per-scenario channel A/B knobs (invSync/worldSync ON, probe channels OFF)
    # and log-only diagnostic traces ([recon]/[wi]/[speeddbg]/[shackledbg]/[jail]
    # /[spike]) come from the manifest DiagEnv - the single source of truth the
    # plugin's Config.cpp no longer hard-codes. Applied hermetically (the whole
    # managed keyset is cleared first) and MODE-AGNOSTIC (host + join match).
    [void](Set-CoopDiagEnv -Entry $manifestEntry)
    # Host-only setup/re-arm scene.
    $env:KENSHICOOP_SETUP = if ($Mode -eq "host") { $Setup } else { "" }
    # Legacy in-plugin WAN sim (both clients; entities only).
    $env:KENSHICOOP_NETSIM_DELAY_MS  = "$NetSimDelayMs"
    $env:KENSHICOOP_NETSIM_JITTER_MS = "$NetSimJitterMs"
    $env:KENSHICOOP_NETSIM_LOSS_PCT  = "$NetSimLossPct"
    # Fake wall-clock skew: JOIN only (the host is the reference clock).
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = if ($Mode -eq "join") { "$FakeClockSkewMs" } else { "0" }
    # Peer-ready arming fallback (0 = legacy immediate arming; spike runs).
    $env:KENSHICOOP_ARM_TIMEOUT_MS = "$armTimeoutMs"
}

# Wait until a regex appears in a (growing) file, or timeout. Returns $true/$false.
function Wait-ForLogLine {
    param([string]$File, [string]$Pattern, [int]$TimeoutSec)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $File) {
            $hit = Select-String -Path $File -Pattern $Pattern -SimpleMatch:$false -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Take-Shot {
    param([int]$ProcId, [string]$Out, [string]$Label)
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") -ProcessId $ProcId -Out $Out -Frames $Frames -IntervalMs $FrameIntervalMs
        Write-Host "  captured $Label ($Frames frame(s)) -> $Out"
    } catch {
        Write-Warning "screenshot ($Label) failed: $($_.Exception.Message)"
    }
}

# Launch a Kenshi instance and get it past Kenshi's Win32 launcher.
function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

# Automated layout: 1280x1024 pair on the PRIMARY (laptop) monitor - the
# screenshot-stable baseline. Written every run because manual_session.ps1
# resizes the same kenshi.cfg files for the ultrawide manual layout; this
# guarantees automated runs always come back to the known-good geometry.
Write-Host "Window layout: automated (1280x1024 x2, $ArrangeMonitor monitor)"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
    -Width 1280 -Height 1024 -HostDir $HostDir -JoinDir $JoinDir

Write-Host "Launching HOST (and clicking through the launcher) ..."
Set-CoopEnv -Mode "host" -Log $hostLog
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

# Serialize the zone loads: two Kenshi instances loading the same save
# concurrently can starve the (background, unfocused) host - measured run
# 134544: the host's 12 s load took 2.4 min and only completed once the join
# exited, so the windows never overlapped and the run burned for nothing.
# Wait for the host to reach gameplay first (fall back to the old fixed
# stagger if it never does - the analysis will flag health_host anyway).
Write-Host "Waiting for HOST to reach gameplay before launching JOIN (timeout ${StartTimeoutSec}s) ..."
if (Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec) {
    Write-Host "Host in gameplay; launching JOIN after ${JoinDelaySec}s settle ..."
} else {
    Write-Warning "Host not in gameplay after ${StartTimeoutSec}s; launching JOIN anyway."
}
Start-Sleep -Seconds $JoinDelaySec

Write-Host "Launching JOIN (and clicking through the launcher) ..."
Set-CoopEnv -Mode "join" -Log $joinLog
$joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
if ($joinPid -eq 0) { Write-Warning "Join failed to get past the launcher; continuing with host only." }

Write-Host "Host game PID=$hostPid  Join game PID=$joinPid"

# Auto-arrange the windows side by side (background, re-pinned through load).
if (-not $NoArrange -and $hostPid -ne 0) {
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host "Arranging windows side by side ($ArrangeMonitor monitor, host left / join right; re-pinning ${ArrangeRepeatSec}s) ..."
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$hostPid", "-JoinPid", "$joinPid",
        "-Monitor", $ArrangeMonitor, "-TimeoutSec", "90", "-RepeatSec", "$ArrangeRepeatSec"
    ) | Out-Null
}

# Anchor the screenshot to when the HOST reaches gameplay.
$started = Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec $StartTimeoutSec
if ($started) {
    Write-Host "Host reached gameplay; waiting before screenshot ..."
} else {
    Write-Warning "Did not see 'gameplay started' in host log within $StartTimeoutSec s; capturing on a best-effort basis."
}

function Test-Alive { param([int]$ProcId) return ($ProcId -ne 0 -and $null -ne (Get-Process -Id $ProcId -ErrorAction SilentlyContinue)) }

$shotsTaken = $false
if ($Scenario -ne "") {
    # Capture each client at ITS OWN anchor so BOTH screenshots show in-game,
    # mid-action state despite the launch stagger.
    $hostAnchor = "SCENARIO MEMBER"
    $hostShotDelay = 1
    $joinShotDelay = 1
    if ($Scenario -eq "combat_kill") {
        $hostAnchor = "SCENARIO KO enforced"
        $hostShotDelay = 2
        $joinShotDelay = 4
    }
    if (Wait-ForLogLine -File $hostLog -Pattern $hostAnchor -TimeoutSec $ScenarioWaitSec) {
        Write-Host "Saw host '$hostAnchor'; capturing host shortly after."
        Start-Sleep -Seconds $hostShotDelay
    } else {
        Write-Warning "No host '$hostAnchor' within $ScenarioWaitSec s; capturing host best-effort."
    }
    if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
    else { Write-Warning "Host already exited before screenshot." }

    if ($joinPid -ne 0) {
        if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec $JoinAnchorTimeoutSec) {
            Write-Host "Saw join SCENARIO RECV; capturing join shortly after."
            Start-Sleep -Seconds $joinShotDelay
        } else {
            Write-Warning "No join SCENARIO RECV within $JoinAnchorTimeoutSec s; capturing join best-effort."
        }
        if (Test-Alive $joinPid) { Take-Shot -ProcId $joinPid -Out $joinPng -Label "join" }
        else { Write-Warning "Join already exited before screenshot." }
    }
    $shotsTaken = $true
} else {
    $lead = $Seconds - $ShotLeadSec
    if ($lead -lt 0) { $lead = 0 }
    Start-Sleep -Seconds $lead
}

if (-not $shotsTaken) {
    if (Test-Alive $hostPid) { Take-Shot -ProcId $hostPid -Out $hostPng -Label "host" }
    else { Write-Warning "Host already exited before screenshot." }
    if (Test-Alive $joinPid) { Take-Shot -ProcId $joinPid -Out $joinPng -Label "join" }
    else { Write-Warning "Join already exited before screenshot." }
}

# Wait for self-exit, with a hard-timeout kill so unattended runs never hang.
$killGrace = if ($Scenario -ne "") { 75 } else { $ShotLeadSec + 60 }
if ($manifest.Profiles.ContainsKey($Profile)) { $killGrace = [Math]::Max($killGrace, $manifest.Profiles[$Profile].KillGraceSec) }
# Long scenarios: the kill grace runs from the (early) screenshot anchors, so a
# scenario window longer than ~KillGraceSec gets killed before RESULT unless
# the manifest entry raises it.
if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey("KillGraceSec")) {
    $killGrace = [Math]::Max($killGrace, $manifestEntry.KillGraceSec)
}
$killDeadline = (Get-Date).AddSeconds($killGrace)
foreach ($p in @($hostPid, $joinPid)) {
    if ($p -eq 0) { continue }
    $remain = [int]([Math]::Max(1, ($killDeadline - (Get-Date)).TotalSeconds))
    try { Wait-Process -Id $p -Timeout $remain -ErrorAction Stop }
    catch {
        if (Test-Alive $p) {
            Write-Warning "PID $p did not self-exit; killing."
            try { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
}

# Tear down the WAN proxy.
if ($null -ne $wanProc -and -not $wanProc.HasExited) {
    try { Stop-Process -Id $wanProc.Id -Force -ErrorAction SilentlyContinue } catch {}
}

Write-Host ""
Write-Host "== Results =="
Write-Host "  host log: $hostLog"
Write-Host "  join log: $joinLog"
if ($Frames -gt 1) {
    Write-Host "  host png: $hostPng (+ frames host_1..host_$Frames.png)"
    Write-Host "  join png: $joinPng (+ frames join_1..join_$Frames.png)"
} else {
    Write-Host "  host png: $hostPng"
    Write-Host "  join png: $joinPng"
}

# ---- Judge the run with the shared oracle library --------------------------------
Write-Host ""
if ($Scenario -ne "") { Write-Host "== Scenario checks: $Scenario ==" }

$runInfo = @{
    save          = $Save
    setup         = $Setup
    profile       = $Profile
    wan           = $Wan
    fakeSkewMs    = $FakeClockSkewMs
    netSimDelayMs = $NetSimDelayMs
    netSimJitterMs= $NetSimJitterMs
    netSimLossPct = $NetSimLossPct
    outDir        = $OutDir
}
$expectedSkew = if ($FakeClockSkewMs -ne 0) { $FakeClockSkewMs } else { $null }
$verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
              -Tolerance $Tolerance -JoinExpected ($joinPid -ne 0) `
              -RunInfo $runInfo -ExpectedSkewMs $expectedSkew `
              -WanActive ($Wan -ne "") `
              -OutJson (Join-Path $OutDir "verdict.json")

Write-Host ""
Write-Host ("RESULT: " + $(if ($verdict.pass) { "PASS" } else { "FAIL" }))
if ($verdict.pass) { exit 0 } else { exit 1 }
