# start_kenshi.ps1 - launch a Kenshi instance and get it past the launcher.
#
# Kenshi's startup shows a Win32/MFC graphics launcher (Video settings / Mods /
# Changelog, with an OK button) and blocks there until OK is clicked. RE_Kenshi
# and our plugin only load AFTER that click. The root kenshi_x64.exe is just a
# loader that relaunches the real game as a separate process named "Kenshi_x64",
# so we also have to discover that real process (a different PID).
#
# This script:
#   1. snapshots existing Kenshi_x64 PIDs,
#   2. launches the loader (default: the Steam install's kenshi_x64.exe),
#   3. waits for a NEW Kenshi_x64 window that has an "OK" button,
#   4. clicks OK via BM_CLICK (cross-process; no screen coordinates needed),
#   5. returns the real game's process Id (written to stdout as "GAMEPID=<id>").
#
# Env vars (KENSHICOOP_*) must be set by the caller before invoking; they are
# inherited by the launched process and propagate to the relaunched game.

param(
    [string]$ExePath = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\kenshi_x64.exe",
    [string]$WorkDir = "",
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = "Stop"
if (-not $WorkDir) { $WorkDir = Split-Path $ExePath }

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class KenLauncher {
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public const uint BM_CLICK = 0x00F5;
  static IntPtr found;
  public static IntPtr FindButton(IntPtr top, string text) {
    found = IntPtr.Zero;
    EnumChildWindows(top, (h,p) => {
      var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "Button") {
        var t = new StringBuilder(64); GetWindowText(h, t, 64);
        if (t.ToString() == text) { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static bool ClickButton(IntPtr top, string text) {
    IntPtr b = FindButton(top, text);
    if (b == IntPtr.Zero) return false;
    SendMessage(b, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
    return true;
  }
}
"@

$before = @{}
foreach ($p in (Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue)) { $before[$p.Id] = $true }

Write-Host ("Launching: " + $ExePath)
Start-Process -FilePath $ExePath -WorkingDirectory $WorkDir | Out-Null

# Find a NEW Kenshi_x64 window that exposes the launcher's OK button, then click it.
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$game = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $cands = Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue |
             Where-Object { -not $before.ContainsKey($_.Id) -and $_.MainWindowHandle -ne 0 }
    foreach ($c in $cands) {
        if ([KenLauncher]::FindButton($c.MainWindowHandle, "OK") -ne [IntPtr]::Zero) { $game = $c; break }
    }
    if ($game) { break }
}
if (-not $game) { Write-Error "Timed out waiting for the Kenshi launcher (OK button)."; exit 2 }

Write-Host ("Launcher up: PID=" + $game.Id + " '" + $game.MainWindowTitle + "' - clicking OK")
if (-not [KenLauncher]::ClickButton($game.MainWindowHandle, "OK")) {
    Write-Error "Found launcher window but failed to click OK."; exit 3
}

# Emit on the output stream (not Write-Host) so callers can capture it.
Write-Output ("GAMEPID=" + $game.Id)
exit 0
