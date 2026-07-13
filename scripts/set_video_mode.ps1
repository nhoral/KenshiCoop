<#
.SYNOPSIS
  Set the windowed Video Mode (client size) in BOTH Kenshi installs' kenshi.cfg.

.DESCRIPTION
  Kenshi reads its window size from kenshi.cfg ("Video Mode=W x H @ 32-bit
  colour [0]"); the window tiler (arrange_windows.ps1) only MOVES windows, so
  the size each session wants must be written here before launch. The two
  harnesses call this with their own layout:
    * manual_session.ps1 -> ultrawide layout (3440x1440 split into two
      side-by-side windows; 1700x1350 client leaves room for borders, title
      bar and taskbar - the proven fit from the 2026-07-11 free-play session)
    * run_test.ps1       -> automated layout (1280x1024 pair on the laptop
      primary monitor, the long-standing screenshot-stable baseline)
  Idempotent: only rewrites a cfg whose current mode differs. Also enforces
  windowed mode (Full Screen=No), which side-by-side placement requires.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\set_video_mode.ps1 -Width 1700 -Height 1350
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join"
)

$ErrorActionPreference = "Stop"

foreach ($dir in @($HostDir, $JoinDir)) {
    $cfg = Join-Path $dir "kenshi.cfg"
    if (-not (Test-Path $cfg)) { Write-Warning "kenshi.cfg not found: $cfg"; continue }
    $lines = Get-Content $cfg
    $want = "Video Mode=$Width x $Height @ 32-bit colour [0]"
    $changed = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^Video Mode=") {
            if ($lines[$i] -ne $want) { $lines[$i] = $want; $changed = $true }
        } elseif ($lines[$i] -match "^Full Screen=" -and $lines[$i] -ne "Full Screen=No") {
            $lines[$i] = "Full Screen=No"; $changed = $true
        }
    }
    if ($changed) {
        Set-Content -Path $cfg -Value $lines -Encoding ASCII
        Write-Host "  video mode -> ${Width}x${Height} (windowed) in $cfg"
    } else {
        Write-Host "  video mode already ${Width}x${Height} (windowed) in $cfg"
    }
}
