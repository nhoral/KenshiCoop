<#
.SYNOPSIS
  Remote-side host launcher for LAN validation runs. Lives in the drop dir on
  the LAN machine (pushed by setup_lan_host.ps1) and is triggered via the
  KenshiCoopHost scheduled task so the game launches in the INTERACTIVE session
  (a plain SSH-spawned process cannot render on the desktop).

.DESCRIPTION
  Reads run_args.json (pushed per run by run_lan_test.ps1 on the dev machine):
    runId       - results subfolder name (unique per run)
    seconds     - hard-kill grace measured from gameplay start
    anchor      - host log line to time the screenshot on ('' = none)
    shotDelaySec- extra wait after the anchor before capturing
    env         - flat map of every KENSHICOOP_* variable for the host role
                  (built by the dev machine from the scenario manifest, so all
                  role/scenario logic stays in one place)

  Writes results\<runId>\host.log + host*.png + done.txt (status marker the dev
  machine polls for, then collects via scp).
#>
$ErrorActionPreference = "Stop"
$dropDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$argsFile = Join-Path $dropDir "run_args.json"

if (-not (Test-Path $argsFile)) { throw "No run_args.json in $dropDir" }
$runArgs = Get-Content $argsFile -Raw | ConvertFrom-Json

$resultsDir = Join-Path $dropDir ("results\" + $runArgs.runId)
New-Item -ItemType Directory -Force -Path $resultsDir | Out-Null
$hostLog = Join-Path $resultsDir "host.log"
$doneFile = Join-Path $resultsDir "done.txt"

function Finish {
    param([string]$Status, [string]$Detail = "")
    Set-Content -Path $doneFile -Value ("{0}`n{1}" -f $Status, $Detail) -Encoding UTF8
}

try {
    # Clean slate: a stale instance holds the mod DLL lock and confuses PID discovery.
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) { $stale | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

    # Apply the env exactly as the dev machine built it, then pin the log path here.
    foreach ($p in $runArgs.env.PSObject.Properties) {
        Set-Item -Path ("Env:" + $p.Name) -Value ("" + $p.Value)
    }
    $env:KENSHICOOP_LOG = $hostLog

    $kenshiDir = $runArgs.kenshiDir
    $out = & (Join-Path $dropDir "start_kenshi.ps1") -ExePath (Join-Path $kenshiDir "kenshi_x64.exe") -WorkDir $kenshiDir -TimeoutSec 120 6>&1
    $out | ForEach-Object { Add-Content -Path (Join-Path $resultsDir "launcher.log") -Value "$_" }
    $gamePid = 0
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { $gamePid = [int]$Matches[1] }
    if ($gamePid -eq 0) { Finish "LAUNCH-FAIL" "no game pid"; exit 1 }

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

    # Screenshot at the scenario anchor (best-effort; the log verdict is what gates).
    if ($runArgs.anchor -ne "") {
        if (Wait-ForLogLine -File $hostLog -Pattern $runArgs.anchor -TimeoutSec 180) {
            Start-Sleep -Seconds ([int]$runArgs.shotDelaySec)
            try {
                & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dropDir "screenshot.ps1") `
                    -ProcessId $gamePid -Out (Join-Path $resultsDir "host.png") -Frames 5 -IntervalMs 16
            } catch {}
        }
    }

    # Wait for scenario self-exit; hard-kill after gameplay + seconds + grace so an
    # unattended run can never wedge the machine.
    $started = Wait-ForLogLine -File $hostLog -Pattern "gameplay started" -TimeoutSec 240
    $graceSec = [int]$runArgs.seconds + 120
    try { Wait-Process -Id $gamePid -Timeout $graceSec -ErrorAction Stop }
    catch {
        if ($null -ne (Get-Process -Id $gamePid -ErrorAction SilentlyContinue)) {
            Stop-Process -Id $gamePid -Force -ErrorAction SilentlyContinue
            Finish "KILLED" "did not self-exit within ${graceSec}s (started=$started)"
            exit 0
        }
    }
    Finish "OK" "self-exited (started=$started)"
    exit 0
} catch {
    Finish "ERROR" $_.Exception.Message
    exit 1
}
