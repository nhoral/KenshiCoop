<#
.SYNOPSIS
  Two-machine LAN test runner: the LAN machine (machine 2, provisioned by
  setup_lan_host.ps1) plays HOST via its KenshiCoopHost scheduled task; this
  machine launches only the JOIN - mirroring the real session topology where
  the friend hosts and you iterate on the join client.

.DESCRIPTION
  Mirrors run_test.ps1's contract: manifest-driven save/setup/tolerance/gates,
  -Wan <profile> (the netsim proxy runs LOCALLY in front of the LAN link, so
  the run is LAN-real AND WAN-degraded), -FakeClockSkewMs (join side), exit
  code = verdict, verdict.json in the out dir. Differences:
    * host launch/collect goes over SSH/SCP (args JSON -> schtasks /run ->
      poll -> scp results back)
    * the freshly built DLL is pushed to machine 2 EVERY run (the protocol
      handshake rejects mixed builds by design - lockstep is mandatory)
    * CLOCKSYNC now measures a REAL two-machine clock offset

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario coop_presence

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\run_lan_test.ps1 -Scenario combat_kill -Wan bad -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Scenario = "",
    [string]$Save = "",
    [string]$Setup = "",
    [double]$Tolerance = 0,
    [int]$Seconds = 0,
    [int]$Port = 27800,
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [string]$OutDir = "",
    [switch]$SkipBuild,
    # Push the fixture save to machine 2 even if it already exists there.
    [switch]$SyncSave,
    [string]$Wan = "",
    [int]$FakeClockSkewMs = 0,
    # Timeout profile; 'lan' covers machine 2's independent load times.
    [string]$Profile = "lan",
    [int]$Frames = 5,
    [int]$FrameIntervalMs = 16
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
Import-Module (Join-Path $scriptDir "CoopHarness.psm1") -Force
$manifest = Get-ScenarioManifest

# ---- LAN config -----------------------------------------------------------------
$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (run setup_lan_host.ps1 first)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"
$dropFwd = $cfg.dropDir -replace '\\', '/'

function Invoke-LanSsh {
    param([string]$Cmd)
    # PowerShell 5.1 does not escape embedded double quotes for native args;
    # escape them so quoted remote paths (spaces) survive the trip. Tolerate
    # stderr noise (ssh WARNINGs) that 2>&1 + EAP=Stop would turn terminating.
    $escaped = $Cmd -replace '"', '\"'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

# ---- Manifest defaults ---------------------------------------------------------------
$manifestEntry = $null
if ($Scenario -ne "" -and $manifest.Scenarios.ContainsKey($Scenario)) {
    $manifestEntry = $manifest.Scenarios[$Scenario]
    if ($Save -eq "")     { $Save = $manifestEntry.Save }
    if ($Setup -eq "" -and $manifestEntry.Setup -ne "") { $Setup = $manifestEntry.Setup }
    if ($Tolerance -eq 0) { $Tolerance = $manifestEntry.Tolerance }
}
if ($Tolerance -eq 0) { $Tolerance = 3.0 }
if ($Save -eq "") { throw "No -Save given and scenario '$Scenario' has no manifest default." }
if ($Seconds -eq 0) { $Seconds = if ($Scenario -ne "") { 150 } else { 60 } }

$prof = $manifest.Profiles[$Profile]
if ($null -eq $prof) { throw "Unknown profile '$Profile'" }
$armTimeoutMs = $prof.ArmTimeoutMs

if ($OutDir -eq "") {
    $stamp  = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $repoRoot "tools\test-runs\lan_$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$runId   = Split-Path -Leaf $OutDir
$joinLog = Join-Path $OutDir "join.log"
$joinPng = Join-Path $OutDir "join.png"

$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe" }
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
if (-not (Test-Path (Join-Path $saveRoot $Save))) { throw "Save '$Save' not found locally in $saveRoot" }

Write-Host "== KenshiCoop LAN test run =="
Write-Host "  remote host: $target ($($cfg.kenshiDir))"
Write-Host "  save:        $Save"
Write-Host "  scenario:    $Scenario (tolerance $Tolerance u)"
if ($Wan -ne "") { Write-Host "  wan:         $Wan (local proxy in front of the LAN link)" }
if ($FakeClockSkewMs -ne 0) { Write-Host "  skew:        join clock +${FakeClockSkewMs}ms (on top of the REAL clock offset)" }
Write-Host "  out dir:     $OutDir"

# ---- Pre-flight: Steam same-account check (advisory) -----------------------------------
try {
    $localUser = (Get-ItemProperty 'HKCU:\Software\Valve\Steam\ActiveProcess' -ErrorAction Stop).ActiveUser
    $r = Invoke-LanSsh "reg query HKCU\Software\Valve\Steam\ActiveProcess /v ActiveUser"
    $remoteUser = 0
    if ("$($r.out)" -match "ActiveUser\s+REG_DWORD\s+0x([0-9a-fA-F]+)") { $remoteUser = [Convert]::ToInt64($Matches[1], 16) }
    if ($localUser -ne 0 -and $remoteUser -ne 0 -and $localUser -eq $remoteUser) {
        Write-Warning "Both machines show the SAME Steam account ONLINE (id $localUser)."
        Write-Warning "Put one client in OFFLINE mode or the second launch may sign the first out."
    }
} catch {}

# ---- 1. Build + deploy locally, push DLL to machine 2 (lockstep) --------------------------
if (-not $SkipBuild) {
    Write-Host "=== build + local deploy ==="
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }
    & cmd.exe /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    & cmd.exe /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "local deploy failed" }
}
Write-Host "=== push DLL to $($cfg.host) (protocol lockstep) ==="
$dll = Join-Path $repoRoot "src\plugin\x64\Harness\KenshiCoop.dll"
$modDirFwd = "$($cfg.kenshiDir -replace '\\', '/')/mods/KenshiCoop"
& scp -o BatchMode=yes $dll "${target}:$modDirFwd/"
if ($LASTEXITCODE -ne 0) { throw "DLL push failed (is Kenshi running on $($cfg.host)?)" }

# ---- 2. Fixture save (push if missing or -SyncSave) -----------------------------------------
$r = Invoke-LanSsh "if exist `"$($cfg.kenshiDir)\save\$Save`" (echo SAVE-OK) else (echo SAVE-MISSING)"
if ($SyncSave -or "$($r.out)" -notmatch "SAVE-OK") {
    Write-Host "=== push save '$Save' ==="
    $src = Join-Path $saveRoot $Save
    $r2 = Invoke-LanSsh "echo %LOCALAPPDATA%"
    $remoteLocal = ("" + (@($r2.out) | Select-Object -Last 1)).Trim()
    foreach ($destBase in @("$remoteLocal\kenshi\save", "$($cfg.kenshiDir)\save")) {
        [void](Invoke-LanSsh "mkdir `"$destBase`" 2>nul & echo OK")
        & scp -o BatchMode=yes -r "$src" ("${target}:" + ($destBase -replace '\\', '/') + "/")
        if ($LASTEXITCODE -ne 0) { Write-Warning "save push to $destBase failed" }
    }
}

# ---- 3. Trigger the remote host ---------------------------------------------------------------
# Env built HERE (single source of truth = the manifest), applied verbatim there.
$hostAnchor = "SCENARIO MEMBER"; $hostShotDelay = 1
if ($Scenario -eq "combat_kill") { $hostAnchor = "SCENARIO KO enforced"; $hostShotDelay = 2 }
$runArgs = [pscustomobject]@{
    runId        = $runId
    seconds      = $Seconds
    anchor       = $(if ($Scenario -ne "") { $hostAnchor } else { "" })
    shotDelaySec = $hostShotDelay
    kenshiDir    = $cfg.kenshiDir
    env          = [pscustomobject]@{
        KENSHICOOP_MODE               = "host"
        KENSHICOOP_IP                 = "0.0.0.0"
        KENSHICOOP_PORT               = "$Port"
        KENSHICOOP_SAVE               = $Save
        KENSHICOOP_TEST_SECONDS       = "$Seconds"
        KENSHICOOP_SCENARIO           = $Scenario
        KENSHICOOP_SETUP              = $Setup
        KENSHICOOP_PROBE_AISUSPEND    = ""
        KENSHICOOP_NETSIM_DELAY_MS    = "0"
        KENSHICOOP_NETSIM_JITTER_MS   = "0"
        KENSHICOOP_NETSIM_LOSS_PCT    = "0"
        KENSHICOOP_FAKE_CLOCK_SKEW_MS = "0"
        KENSHICOOP_ARM_TIMEOUT_MS     = "$armTimeoutMs"
    }
}
# Merge the scenario's manifest DiagEnv into the remote host env (channel A/B
# knobs + log-only traces - the single source of truth Config.cpp no longer
# hard-codes). The remote runner applies run_args.json env verbatim.
if ($null -ne $manifestEntry -and $manifestEntry.ContainsKey('DiagEnv')) {
    foreach ($k in $manifestEntry.DiagEnv.Keys) {
        Add-Member -InputObject $runArgs.env -NotePropertyName $k -NotePropertyValue "$($manifestEntry.DiagEnv[$k])" -Force
    }
}
$argsLocal = Join-Path $OutDir "run_args.json"
$runArgs | ConvertTo-Json -Depth 4 | Set-Content -Path $argsLocal -Encoding UTF8
& scp -o BatchMode=yes $argsLocal "${target}:$dropFwd/run_args.json"
if ($LASTEXITCODE -ne 0) { throw "args push failed" }

# Clean any stale remote game, then fire the task.
[void](Invoke-LanSsh "taskkill /IM Kenshi_x64.exe /F 2>nul & taskkill /IM kenshi_x64.exe /F 2>nul & echo CLEAN")
$r = Invoke-LanSsh "schtasks /run /tn KenshiCoopHost"
if ($r.exit -ne 0) { throw "task trigger failed: $($r.out)" }
Write-Host "Remote host task triggered; waiting for it to reach gameplay ..."

$remoteResults = "$($cfg.dropDir)\results\$runId"
$remoteLogCmd  = "type `"$remoteResults\host.log`" 2>nul"
$deadline = (Get-Date).AddSeconds($prof.StartTimeoutSec)
$hostUp = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $r = Invoke-LanSsh $remoteLogCmd
    if ("$($r.out)" -match "gameplay started") { $hostUp = $true; break }
}
if (-not $hostUp) { Write-Warning "Remote host did not reach gameplay within $($prof.StartTimeoutSec)s; continuing best-effort." }
else { Write-Host "Remote host in gameplay." }

# ---- 4. Local WAN proxy (optional) + local JOIN --------------------------------------------------
$wanProc = $null
$joinIp = $cfg.host; $joinPort = $Port
if ($Wan -ne "") {
    $wp = $manifest.WanProfiles[$Wan]
    if ($null -eq $wp) { throw "Unknown WAN profile '$Wan'" }
    $netsimExe = Join-Path $repoRoot "dist\netsim.exe"
    if (-not (Test-Path $netsimExe)) { throw "dist\netsim.exe not built (cmd /c scripts\build_netsim.cmd)" }
    $proxyPort = $Port + 1
    $wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $OutDir "netsim.log") `
        -ArgumentList @("$proxyPort", $cfg.host, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
    Start-Sleep -Milliseconds 500
    if ($wanProc.HasExited) { throw "netsim exited immediately" }
    $joinIp = "127.0.0.1"; $joinPort = $proxyPort
    Write-Host "WAN proxy up: 127.0.0.1:$proxyPort -> $($cfg.host):$Port ($($wp.DelayMs)ms +/-$($wp.JitterMs)ms, $($wp.LossPct)%)"
}

try {
    $env:KENSHICOOP_MODE               = "join"
    $env:KENSHICOOP_IP                 = $joinIp
    $env:KENSHICOOP_PORT               = "$joinPort"
    $env:KENSHICOOP_SAVE               = $Save
    $env:KENSHICOOP_TEST_SECONDS       = "$Seconds"
    $env:KENSHICOOP_LOG                = $joinLog
    $env:KENSHICOOP_SCENARIO           = $Scenario
    $env:KENSHICOOP_SETUP              = ""
    $env:KENSHICOOP_PROBE_AISUSPEND    = ""
    # Per-scenario channel A/B knobs + diagnostic traces from the manifest DiagEnv
    # (mirrors the remote host env above; hermetic clear + apply on the join side).
    [void](Set-CoopDiagEnv -Entry $manifestEntry)
    $env:KENSHICOOP_NETSIM_DELAY_MS    = "0"
    $env:KENSHICOOP_NETSIM_JITTER_MS   = "0"
    $env:KENSHICOOP_NETSIM_LOSS_PCT    = "0"
    $env:KENSHICOOP_FAKE_CLOCK_SKEW_MS = "$FakeClockSkewMs"
    $env:KENSHICOOP_ARM_TIMEOUT_MS     = "$armTimeoutMs"

    Write-Host "Launching local JOIN -> ${joinIp}:$joinPort ..."
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $joinExe -WorkDir $JoinDir -TimeoutSec $prof.StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $joinPid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $joinPid = [int]$Matches[1] }
    if ($joinPid -eq 0) { throw "Join failed to get past the launcher." }

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

    if ($Scenario -ne "") {
        if (Wait-ForLogLine -File $joinLog -Pattern "SCENARIO RECV" -TimeoutSec $prof.JoinAnchorTimeoutSec) {
            Write-Host "Join streaming; capturing screenshot."
            Start-Sleep -Seconds 1
            try {
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "screenshot.ps1") `
                    -ProcessId $joinPid -Out $joinPng -Frames $Frames -IntervalMs $FrameIntervalMs
            } catch { Write-Warning "join screenshot failed: $($_.Exception.Message)" }
        } else {
            Write-Warning "No join SCENARIO RECV within $($prof.JoinAnchorTimeoutSec)s."
        }
    }

    # Wait for the join to self-exit (hard-kill backstop).
    $grace = $Seconds + $prof.KillGraceSec
    try { Wait-Process -Id $joinPid -Timeout $grace -ErrorAction Stop }
    catch {
        if ($null -ne (Get-Process -Id $joinPid -ErrorAction SilentlyContinue)) {
            Write-Warning "Join did not self-exit; killing."
            Stop-Process -Id $joinPid -Force -ErrorAction SilentlyContinue
        }
    }
} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { Stop-Process -Id $wanProc.Id -Force -ErrorAction SilentlyContinue } catch {}
    }
}

# ---- 5. Collect the remote results -----------------------------------------------------------------
Write-Host "Waiting for the remote host to finish ..."
$deadline = (Get-Date).AddSeconds($prof.KillGraceSec + 120)
$done = $false
while ((Get-Date) -lt $deadline) {
    $r = Invoke-LanSsh "if exist `"$remoteResults\done.txt`" (echo DONE) else (echo WAIT)"
    if ("$($r.out)" -match "DONE") { $done = $true; break }
    Start-Sleep -Seconds 5
}
if (-not $done) {
    Write-Warning "Remote done marker never appeared; killing remote game and collecting anyway."
    [void](Invoke-LanSsh "taskkill /IM Kenshi_x64.exe /F 2>nul & echo KILLED")
}
& scp -o BatchMode=yes -r "${target}:$dropFwd/results/$runId/*" "$OutDir/"
if ($LASTEXITCODE -ne 0) { Write-Warning "results collection failed" }
[void](Invoke-LanSsh "rmdir /S /Q `"$remoteResults`" 2>nul & echo CLEANED")

$hostLog = Join-Path $OutDir "host.log"
Write-Host ""
Write-Host "== Results =="
Write-Host "  host log: $hostLog (collected from $($cfg.host))"
Write-Host "  join log: $joinLog"

# ---- 6. Judge with the shared oracle library --------------------------------------------------------
Write-Host ""
if ($Scenario -ne "") { Write-Host "== Scenario checks: $Scenario ==" }
$runInfo = @{
    lanHost    = $cfg.host
    save       = $Save
    setup      = $Setup
    profile    = $Profile
    wan        = $Wan
    fakeSkewMs = $FakeClockSkewMs
    outDir     = $OutDir
    topology   = "remote-host"
}
# On two REAL machines the wall clocks already differ by an unknown amount, so
# the "estimated offset must equal -injectedSkew" assertion from loopback runs
# is not measurable here (estimate = realOffset - skew). The injected skew still
# stresses alignment - the time-aligned oracles passing IS the validation - so
# clock_sync gates only on having a healthy estimate.
$expectedSkew = $null
$verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
              -Tolerance $Tolerance -JoinExpected $true `
              -RunInfo $runInfo -ExpectedSkewMs $expectedSkew `
              -WanActive ($Wan -ne "") `
              -OutJson (Join-Path $OutDir "verdict.json")

Write-Host ""
Write-Host ("RESULT: " + $(if ($verdict.pass) { "PASS" } else { "FAIL" }))
if ($verdict.pass) { exit 0 } else { exit 1 }
