<#
.SYNOPSIS
  DRESS REHEARSAL of the friend-HOSTS session, end to end, using the LAN
  machine as the "friend": assemble the host-role kit, install + run it on
  machine 2 exactly as the friend would (friend_host.ps1 from the unzipped
  kit), join from this machine through the WAN proxy via join_session.ps1,
  collect the friend-side results zip, and judge both logs offline.

.DESCRIPTION
  PASS means the real internet session only changes the network: kit install
  works, the host comes up from the kit alone, the join connects through a
  degraded link, the scenario passes, and the results zip contains everything
  analyze_run.ps1 needs.

  Uses scripts\lan.config.json (setup_lan_host.ps1 must have run once - the
  scheduled-task pattern and firewall rule are reused).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\rehearse_host_kit.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string]$Scenario = "coop_presence",
    [string]$Save = "",
    [int]$Port = 27800,
    [string]$Wan = "bad",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

Import-Module (Join-Path $scriptDir "CoopOracles.psm1") -Force
$manifest = Get-ScenarioManifest
if ($Save -eq "" -and $manifest.Scenarios.ContainsKey($Scenario)) { $Save = $manifest.Scenarios[$Scenario].Save }
if ($Save -eq "") { throw "No save for scenario '$Scenario'" }

$cfgPath = Join-Path $scriptDir "lan.config.json"
if (-not (Test-Path $cfgPath)) { throw "Missing $cfgPath (run setup_lan_host.ps1 first)" }
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$target = "$($cfg.user)@$($cfg.host)"

function Invoke-LanSsh {
    param([string]$Cmd)
    # Escape embedded quotes (PS 5.1 native-arg quirk) and tolerate stderr noise:
    # under ErrorActionPreference=Stop a mere WARNING on ssh's stderr becomes a
    # terminating NativeCommandError when merged with 2>&1.
    $escaped = $Cmd -replace '"', '\"'
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { $out = & ssh -o BatchMode=yes -o ConnectTimeout=10 $target $escaped 2>&1 }
    finally { $ErrorActionPreference = $prev }
    return @{ exit = $LASTEXITCODE; out = ($out | ForEach-Object { "$_" }) }
}

$kitTestDir = "C:\KenshiCoopKitTest"
$stampDir = Join-Path $repoRoot ("tools\test-runs\kitrehearsal_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
New-Item -ItemType Directory -Force -Path $stampDir | Out-Null

# ---- 1. Assemble the host-role kit -----------------------------------------------
Write-Host "=== 1. assemble host-role kit ==="
$kitArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", (Join-Path $scriptDir "make_remote_kit.ps1"),
             "-Role", "host", "-Save", $Save, "-Scenario", $Scenario, "-Port", "$Port")
if ($SkipBuild) { $kitArgs += "-SkipBuild" }
& powershell @kitArgs
if ($LASTEXITCODE -ne 0) { throw "kit assembly failed" }
$kitZip = Get-ChildItem (Join-Path $repoRoot "dist\KenshiCoop-remote-kit-*.zip") | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Host "  kit: $($kitZip.FullName)"

# Local deploy so the join runs the same build as the kit.
& cmd.exe /c "`"$scriptDir\deploy.cmd`""
if ($LASTEXITCODE -ne 0) { throw "local deploy failed" }

# ---- 2. Install the kit on machine 2 exactly as the friend would ---------------------
Write-Host ""
Write-Host "=== 2. install kit on $($cfg.host) (as the friend would) ==="
[void](Invoke-LanSsh "taskkill /IM Kenshi_x64.exe /F 2>nul & rmdir /S /Q `"$kitTestDir`" 2>nul & mkdir `"$kitTestDir`" & echo OK")
& scp -o BatchMode=yes $kitZip.FullName "${target}:C:/KenshiCoopKitTest/kit.zip"
if ($LASTEXITCODE -ne 0) { throw "kit push failed" }
$r = Invoke-LanSsh "powershell -NoProfile -Command `"Expand-Archive -Path $kitTestDir\kit.zip -DestinationPath $kitTestDir -Force; 'EXPANDED'`""
if ("$($r.out)" -notmatch "EXPANDED") { throw "kit expand failed: $($r.out)" }
Write-Host "  kit expanded -> $kitTestDir"

# ---- 3. Run friend_host.ps1 there via a one-off interactive task ------------------------
Write-Host ""
Write-Host "=== 3. run friend_host.ps1 on $($cfg.host) ==="
$taskCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File $kitTestDir\friend_host.ps1 -NoPrompt"
$r = Invoke-LanSsh "schtasks /create /tn KenshiCoopKitHost /tr `"$taskCmd`" /sc once /st 00:00 /it /f && schtasks /run /tn KenshiCoopKitHost"
if ($r.exit -ne 0) { throw "kit host task failed: $($r.out)" }
Write-Host "  friend_host task triggered; waiting for the host to reach gameplay ..."
$deadline = (Get-Date).AddSeconds(240)
$hostUp = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    # `type` accepts wildcards under cmd, so this reads whatever per-run results
    # folder friend_host.ps1 created without knowing its timestamp.
    $r = Invoke-LanSsh "type `"$kitTestDir\results_*\host.log`" 2>nul | findstr /C:`"gameplay started`""
    if ("$($r.out)" -match "gameplay started") { $hostUp = $true; break }
}
if (-not $hostUp) { Write-Warning "kit host not confirmed in gameplay; continuing best-effort" }
else { Write-Host "  kit host in gameplay." }

# ---- 4. Join from here through the WAN proxy ----------------------------------------------
Write-Host ""
Write-Host "=== 4. join through WAN proxy ($Wan) via join_session.ps1 ==="
$wp = $manifest.WanProfiles[$Wan]
$netsimExe = Join-Path $repoRoot "dist\netsim.exe"
if (-not (Test-Path $netsimExe)) { & cmd.exe /c "`"$scriptDir\build_netsim.cmd`""; if ($LASTEXITCODE -ne 0) { throw "netsim build failed" } }
$proxyPort = $Port + 1
$wanProc = Start-Process -FilePath $netsimExe -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $stampDir "netsim.log") `
    -ArgumentList @("$proxyPort", $cfg.host, "$Port", "$($wp.DelayMs)", "$($wp.JitterMs)", "$($wp.LossPct)")
Start-Sleep -Milliseconds 500
if ($wanProc.HasExited) { throw "netsim exited immediately" }

$joinOutDir = ""
try {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "join_session.ps1") `
        -HostIp "127.0.0.1" -Port $proxyPort -Scenario $Scenario -SkipBuild 2>&1
    $out | ForEach-Object { Write-Host $_ }
    foreach ($line in $out) { if ("$line" -match "join log:\s+(.+join\.log)") { $joinOutDir = Split-Path -Parent $Matches[1].Trim() } }
} finally {
    if ($null -ne $wanProc -and -not $wanProc.HasExited) {
        try { Stop-Process -Id $wanProc.Id -Force -ErrorAction SilentlyContinue } catch {}
    }
}
if ($joinOutDir -eq "") { throw "join_session did not report a join log" }
Copy-Item (Join-Path $joinOutDir "join*.*") $stampDir -Force -ErrorAction SilentlyContinue

# ---- 5. Collect the friend-side results zip -------------------------------------------------
Write-Host ""
Write-Host "=== 5. collect the friend results zip ==="
$deadline = (Get-Date).AddSeconds(180)
$zipName = ""
while ((Get-Date) -lt $deadline) {
    $r = Invoke-LanSsh "dir /B `"$kitTestDir\KenshiCoop-results-*.zip`" 2>nul"
    $cand = @($r.out) | Where-Object { "$_" -match "^KenshiCoop-results-.*\.zip$" } | Select-Object -Last 1
    if ($null -ne $cand) { $zipName = "$cand".Trim(); break }
    Start-Sleep -Seconds 5
}
if ($zipName -eq "") { throw "friend_host produced no results zip" }
& scp -o BatchMode=yes "${target}:C:/KenshiCoopKitTest/$zipName" "$stampDir/"
if ($LASTEXITCODE -ne 0) { throw "results zip collection failed" }
Expand-Archive -Path (Join-Path $stampDir $zipName) -DestinationPath $stampDir -Force
Write-Host "  collected + expanded: $zipName"

# Cleanup remote kit test artifacts.
[void](Invoke-LanSsh "schtasks /delete /tn KenshiCoopKitHost /f 2>nul & rmdir /S /Q `"$kitTestDir`" 2>nul & echo CLEANED")

# ---- 6. Judge --------------------------------------------------------------------------------
Write-Host ""
Write-Host "=== 6. judge collected logs ==="
$hostLog = Join-Path $stampDir "host.log"
$joinLog = Join-Path $stampDir "join.log"
if (-not (Test-Path $hostLog)) { throw "results zip contained no host.log" }
$verdict = Invoke-RunAnalysis -HostLog $hostLog -JoinLog $joinLog -Scenario $Scenario `
              -Tolerance $manifest.Scenarios[$Scenario].Tolerance -JoinExpected $true `
              -RunInfo @{ rehearsal = "host-kit"; wan = $Wan; lanHost = $cfg.host } `
              -WanActive $true -OutJson (Join-Path $stampDir "verdict.json")

Write-Host ""
Write-Host ("HOST-KIT REHEARSAL RESULT: " + $(if ($verdict.pass) { "PASS - kit is ready to send" } else { "FAIL" }))
Write-Host ("  artifacts: $stampDir")
if ($verdict.pass) { exit 0 } else { exit 1 }
