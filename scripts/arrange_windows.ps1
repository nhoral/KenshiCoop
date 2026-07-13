<#
.SYNOPSIS
  Place the two Kenshi client windows SIDE BY SIDE (host left, join right) on the
  widest monitor, so a co-op test can be watched without one window hiding the other.

.DESCRIPTION
  Both clients run windowed (kenshi.cfg: Full Screen=No), but spawn at the same
  default position and overlap. This finds the (up to two) visible Kenshi_x64 game
  windows and moves them next to each other. It is DPI-aware so multi-monitor /
  scaled-display coordinates are physical pixels (the same fix screenshot.ps1 uses),
  and it MOVES only (keeps each window's native size) to avoid disturbing the D3D
  swapchain. It polls until both windows exist, so it can be launched right after the
  clients and will arrange them once they're past the launcher.

.PARAMETER TimeoutSec
  How long to wait for two game windows to appear before arranging whatever is found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\arrange_windows.ps1
#>
[CmdletBinding()]
param(
    [int]$TimeoutSec = 60,
    [int]$GapPx = 24,
    # Explicit PIDs put the HOST on the left and JOIN on the right reliably. Without
    # them we fall back to PID order, which is NOT launch order (Windows reuses PIDs),
    # so the labels can swap. Pass these whenever you know them.
    [int]$HostPid = 0,
    [int]$JoinPid = 0,
    # Which monitor to stage on: "widest" (default, e.g. an ultrawide - most room) or
    # "primary" (the monitor at virtual origin 0,0, i.e. the main laptop/desktop screen).
    [ValidateSet("widest", "primary")]
    [string]$Monitor = "widest",
    # Keep re-applying the placement for this many seconds. Kenshi re-centers its
    # window when it switches from the load screen to gameplay, so a one-shot move
    # made during loading gets undone. Re-applying every second through the load
    # period pins it. 0 = arrange once and exit.
    [int]$RepeatSec = 0,
    # Enforce this CLIENT (render area) size in physical pixels on both windows,
    # re-applied along with the position. Needed when the staging monitor's DPI
    # scale differs from the primary's: Kenshi is system-DPI-aware, so Windows
    # rescales its window when it crosses onto a different-DPI monitor (e.g.
    # 175% laptop primary -> 100% ultrawide shrinks it by 96/168) - a move-only
    # pin leaves the window the wrong size. 0 = move only (legacy behavior).
    [int]$ClientW = 0,
    [int]$ClientH = 0
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class WinArrange
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);

    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);

    public delegate bool MonProc(IntPtr m, IntPtr dc, ref RECT r, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonProc cb, IntPtr l);

    public const uint SWP_NOSIZE = 0x0001, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;
    public const int  SW_RESTORE = 9;

    public static void EnableDpiAwareness()
    {
        try { if (SetProcessDpiAwarenessContext((IntPtr)(-4))) return; } catch {}
        try { SetProcessDPIAware(); } catch {}
    }

    // Largest visible, non-minimized top-level window owned by 'pid'.
    public static IntPtr MainWindow(uint pid)
    {
        IntPtr best = IntPtr.Zero; long bestArea = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            if (!IsWindowVisible(h) || IsIconic(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (a > bestArea) { bestArea = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    // All monitor rects (physical pixels under DPI awareness).
    public static List<RECT> Monitors()
    {
        var list = new List<RECT>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr m, IntPtr dc, ref RECT r, IntPtr l) {
            list.Add(r); return true;
        }, IntPtr.Zero);
        return list;
    }

    public static RECT GetRect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
    public static RECT GetClient(IntPtr h) { RECT r; GetClientRect(h, out r); return r; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);

    public static void MoveTo(IntPtr h, int x, int y)
    {
        ShowWindow(h, SW_RESTORE); // un-minimize if needed
        SetWindowPos(h, IntPtr.Zero, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Move AND size in one call (outer window size in physical pixels).
    public static void MoveSizeTo(IntPtr h, int x, int y, int w, int hh)
    {
        ShowWindow(h, SW_RESTORE);
        SetWindowPos(h, IntPtr.Zero, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
"@

[WinArrange]::EnableDpiAwareness()

# Find the two Kenshi game windows (process image is Kenshi_x64; the root loader is
# kenshi_x64 but exits). When explicit PIDs are given, wait for exactly those; else
# poll for any two. Poll until ready or we time out.
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$wantPids = @($HostPid, $JoinPid) | Where-Object { $_ -gt 0 }
$wins = @()
while ((Get-Date) -lt $deadline) {
    $found = @()
    if ($wantPids.Count -gt 0) {
        foreach ($pid0 in $wantPids) {
            $h = [WinArrange]::MainWindow([uint32]$pid0)
            if ($h -ne [IntPtr]::Zero) { $found += [pscustomobject]@{ Pid = $pid0; Hwnd = $h } }
        }
        if ($found.Count -ge $wantPids.Count) { $wins = $found; break }
    } else {
        $procs = @(Get-Process -Name "Kenshi_x64" -ErrorAction SilentlyContinue)
        foreach ($p in $procs) {
            $h = [WinArrange]::MainWindow([uint32]$p.Id)
            if ($h -ne [IntPtr]::Zero) { $found += [pscustomobject]@{ Pid = $p.Id; Hwnd = $h } }
        }
        if ($found.Count -ge 2) { $wins = $found; break }
    }
    if ($found.Count -ge 1) { $wins = $found }  # keep best-effort partial
    Start-Sleep -Milliseconds 750
}

if ($wins.Count -lt 1) { Write-Warning "No Kenshi game windows found within $TimeoutSec s."; exit 1 }

# Pick the staging monitor: "primary" is the one at the virtual origin (0,0); else
# the widest (e.g. an ultrawide), which has the most room for two windows.
$mons = [WinArrange]::Monitors()
if ($Monitor -eq "primary") {
    $mon = $mons | Where-Object { $_.Left -le 0 -and $_.Top -le 0 -and $_.Right -gt 0 -and $_.Bottom -gt 0 } | Select-Object -First 1
    if ($null -eq $mon) { $mon = $mons | Sort-Object { ($_.Left * $_.Left) + ($_.Top * $_.Top) } | Select-Object -First 1 }
} else {
    $mon = $mons | Sort-Object { $_.Right - $_.Left } -Descending | Select-Object -First 1
}
$monW = $mon.Right - $mon.Left
$monH = $mon.Bottom - $mon.Top
Write-Host ("Staging monitor ($Monitor): {0}x{1} at ({2},{3})" -f $monW, $monH, $mon.Left, $mon.Top)

# Host on the left, join on the right. Prefer the explicit PID order; otherwise fall
# back to PID sort (NOTE: not necessarily launch order, so labels may be approximate).
function Get-Ordered {
    if ($HostPid -gt 0 -or $JoinPid -gt 0) {
        $o = @()
        foreach ($p0 in @($HostPid, $JoinPid)) {
            $h = [WinArrange]::MainWindow([uint32]$p0)
            if ($h -ne [IntPtr]::Zero) { $o += [pscustomobject]@{ Pid = $p0; Hwnd = $h } }
        }
        return $o
    }
    return @($wins | Sort-Object Pid)
}

# Apply the side-by-side placement once. Re-resolves window handles each call so it
# still works after Kenshi recreates its window on the load->gameplay switch.
function Set-Placement {
    param([bool]$Verbose0)
    $ord = Get-Ordered
    if ($ord.Count -lt 1) { return }
    $enforceSize = ($ClientW -gt 0 -and $ClientH -gt 0)
    # Target OUTER size PER WINDOW: requested client size + that window's own
    # chrome (borders + title bar, measured live - the two installs' windows can
    # carry different chrome, so a shared measurement leaves one client short).
    $r0 = [WinArrange]::GetRect($ord[0].Hwnd)
    $wW = $r0.Right - $r0.Left
    $wH = $r0.Bottom - $r0.Top
    if ($wW -le 0 -or $wH -le 0) { return }
    $sizes = @()
    foreach ($w in $ord) {
        if ($enforceSize) {
            $r = [WinArrange]::GetRect($w.Hwnd)
            $c = [WinArrange]::GetClient($w.Hwnd)
            $chromeW = ($r.Right - $r.Left) - ($c.Right - $c.Left)
            $chromeH = ($r.Bottom - $r.Top) - ($c.Bottom - $c.Top)
            if ($chromeW -lt 0 -or $chromeW -gt 80)  { $chromeW = 24 } # sane fallback
            if ($chromeH -lt 0 -or $chromeH -gt 120) { $chromeH = 64 }
            $sizes += [pscustomobject]@{ W = $ClientW + $chromeW; H = $ClientH + $chromeH }
        } else {
            $sizes += [pscustomobject]@{ W = $wW; H = $wH }
        }
    }
    # Layout math uses the widest target so the pair always fits.
    $slotW = ($sizes | Measure-Object -Property W -Maximum).Maximum
    $slotH = ($sizes | Measure-Object -Property H -Maximum).Maximum
    $gap = $GapPx
    $total = ($slotW * 2) + $gap
    if ($total -gt $monW) { $gap = 0; $total = $slotW * 2 }
    $startX = $mon.Left + [int][Math]::Max(0, ($monW - $total) / 2)
    $y = $mon.Top + [int][Math]::Max(0, ($monH - $slotH) / 2)
    $labels = @("host (left)", "join (right)")
    for ($i = 0; $i -lt $ord.Count -and $i -lt 2; $i++) {
        $x = $startX + ($i * ($slotW + $gap))
        $tw = $sizes[$i].W; $th = $sizes[$i].H
        if ($enforceSize) {
            # Skip the resize when already conformant (a same-size SetWindowPos is
            # still a WM_SIZE to the D3D swapchain; don't spam it every second).
            $cur = [WinArrange]::GetRect($ord[$i].Hwnd)
            $needSize = ((($cur.Right - $cur.Left) -ne $tw) -or (($cur.Bottom - $cur.Top) -ne $th))
            if ($needSize) { [WinArrange]::MoveSizeTo($ord[$i].Hwnd, $x, $y, $tw, $th) }
            elseif ($cur.Left -ne $x -or $cur.Top -ne $y) { [WinArrange]::MoveTo($ord[$i].Hwnd, $x, $y) }
            if ($Verbose0) { Write-Host ("  placed PID {0} -> ({1},{2}) {3}x{4} (resized={5})  [{6}]" -f $ord[$i].Pid, $x, $y, $tw, $th, $needSize, $labels[$i]) }
        } else {
            [WinArrange]::MoveTo($ord[$i].Hwnd, $x, $y)
            if ($Verbose0) { Write-Host ("  moved PID {0} -> ({1},{2})  [{3}]" -f $ord[$i].Pid, $x, $y, $labels[$i]) }
        }
    }
}

Set-Placement -Verbose0 $true
Write-Host "Arranged window(s) side by side."

# Re-apply through the load period so the gameplay-entry window recreate can't undo it.
if ($RepeatSec -gt 0) {
    $stop = (Get-Date).AddSeconds($RepeatSec)
    while ((Get-Date) -lt $stop) {
        Start-Sleep -Milliseconds 1000
        Set-Placement -Verbose0 $false
    }
    Write-Host "Re-pinned placement for $RepeatSec s."
}
exit 0
