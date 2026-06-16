<#
.SYNOPSIS
  Capture a single window to a PNG, by process id (preferred) or window-title match.

.DESCRIPTION
  Used by the KenshiCoop test runner so an agent can visually evaluate the host
  and join clients. Capture is OS-level (no game code), so it works even when the
  window is unfocused or overlapped by the other client.

  Primary path: Win32 PrintWindow with PW_RENDERFULLCONTENT, which grabs a
  window's real client content without requiring it to be foreground. If that
  comes back blank (some D3D windows), it falls back to a BitBlt of the on-screen
  window rectangle.

  Both Kenshi clients are the same kenshi_x64.exe image, so -ProcessId is the
  reliable discriminator between host and join.

.PARAMETER ProcessId
  PID of the target process. The largest visible top-level window owned by this
  PID is captured.

.PARAMETER WindowTitle
  Substring to match against window titles (used only when -ProcessId is absent).

.PARAMETER Out
  Output PNG path. Parent directory is created if needed.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\screenshot.ps1 -ProcessId 12345 -Out host.png
#>
[CmdletBinding()]
param(
    [int]$ProcessId = 0,
    [string]$WindowTitle = "",
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Frames = 1,
    [int]$IntervalMs = 16
)

$ErrorActionPreference = "Stop"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class WinCap
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr hdc, int x, int y, int w, int h, IntPtr src, int sx, int sy, int rop);

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);

    public const uint PW_RENDERFULLCONTENT = 2;
    public const int  SRCCOPY = 0x00CC0020;

    // Make this process DPI-aware so GetClientRect / PrintWindow operate in real
    // (physical) pixels. Without this, on a scaled display (e.g. 175%) the client
    // rect comes back in logical pixels (1280 -> ~731), the bitmap is undersized,
    // and PrintWindow renders only the top-left of the real frame - a crop. Must
    // run before any window/DC calls. Prefer per-monitor-v2; fall back to legacy.
    public static void EnableDpiAwareness()
    {
        try { if (SetProcessDpiAwarenessContext((IntPtr)(-4)) != IntPtr.Zero) return; } catch {}
        try { SetProcessDPIAware(); } catch {}
    }

    // Largest visible, non-minimized top-level window owned by 'pid'.
    public static IntPtr FindMainWindow(uint pid)
    {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            if (!IsWindowVisible(h) || IsIconic(h)) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    // Returns true if the bitmap has any non-black pixel (sampled), i.e. not blank.
    public static bool LooksNonBlank(Bitmap bmp)
    {
        int stepX = Math.Max(1, bmp.Width / 40);
        int stepY = Math.Max(1, bmp.Height / 40);
        for (int y = 0; y < bmp.Height; y += stepY)
            for (int x = 0; x < bmp.Width; x += stepX)
            {
                Color c = bmp.GetPixel(x, y);
                if (c.R > 8 || c.G > 8 || c.B > 8) return true;
            }
        return false;
    }

    // Capture window client content; PrintWindow first, BitBlt fallback.
    public static Bitmap Capture(IntPtr hWnd)
    {
        RECT cr; GetClientRect(hWnd, out cr);
        int w = cr.Right - cr.Left, h = cr.Bottom - cr.Top;
        if (w <= 0 || h <= 0)
        {
            RECT wr; GetWindowRect(hWnd, out wr);
            w = wr.Right - wr.Left; h = wr.Bottom - wr.Top;
        }
        if (w <= 0 || h <= 0) return null;

        Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp))
        {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(hWnd, hdc, PW_RENDERFULLCONTENT);
            g.ReleaseHdc(hdc);
            if (ok && LooksNonBlank(bmp)) return bmp;
        }

        // Fallback: BitBlt the window's on-screen DC (requires it be visible).
        Bitmap bmp2 = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp2))
        {
            IntPtr hdc = g.GetHdc();
            IntPtr src = GetDC(hWnd);
            BitBlt(hdc, 0, 0, w, h, src, 0, 0, SRCCOPY);
            ReleaseDC(hWnd, src);
            g.ReleaseHdc(hdc);
        }
        return bmp2;
    }

    // Capture a short burst of 'frames' bitmaps spaced ~intervalMs apart. Frames
    // are held in memory so PNG encoding (done by the caller afterwards) doesn't
    // bloat the inter-frame timing. A Stopwatch paces each capture toward its
    // target offset; we only sleep when ahead of schedule, so if a single
    // PrintWindow costs more than intervalMs the frames just space out naturally
    // rather than being captured artificially fast.
    public static Bitmap[] CaptureSequence(IntPtr hWnd, int frames, int intervalMs)
    {
        if (frames < 1) frames = 1;
        Bitmap[] outp = new Bitmap[frames];
        var sw = System.Diagnostics.Stopwatch.StartNew();
        for (int i = 0; i < frames; i++)
        {
            long target = (long)i * intervalMs;
            long now = sw.ElapsedMilliseconds;
            if (now < target) System.Threading.Thread.Sleep((int)(target - now));
            outp[i] = Capture(hWnd);
        }
        return outp;
    }
}
"@

function Resolve-Hwnd {
    if ($ProcessId -gt 0) {
        $h = [WinCap]::FindMainWindow([uint32]$ProcessId)
        if ($h -ne [IntPtr]::Zero) { return $h }
        # Last resort: the process's reported MainWindowHandle.
        try {
            $p = Get-Process -Id $ProcessId -ErrorAction Stop
            if ($p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
        } catch {}
        throw "No visible window found for PID $ProcessId."
    }
    if ($WindowTitle -ne "") {
        $p = Get-Process | Where-Object { $_.MainWindowTitle -like "*$WindowTitle*" -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($null -ne $p) { return $p.MainWindowHandle }
        throw "No window matching title '*$WindowTitle*'."
    }
    throw "Provide -ProcessId or -WindowTitle."
}

# Must happen before any GetClientRect/GetWindowRect/PrintWindow calls.
[WinCap]::EnableDpiAwareness()

$hwnd = Resolve-Hwnd

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

if ($Frames -lt 1) { $Frames = 1 }

$bmps = [WinCap]::CaptureSequence($hwnd, $Frames, $IntervalMs)

if ($Frames -le 1) {
    # Single-frame: preserve exact original behavior (one file at $Out).
    $bmp = $bmps[0]
    if ($null -eq $bmp) { throw "Capture failed (zero-size window)." }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "Saved screenshot: $Out"
} else {
    # Burst: write numbered frames <base>_1..<base>_N alongside $Out, and also
    # keep frame 1 at the original $Out so tooling reading host.png/join.png works.
    $dirp = [System.IO.Path]::GetDirectoryName($Out)
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Out)
    $ext  = [System.IO.Path]::GetExtension($Out)
    for ($i = 0; $i -lt $bmps.Length; $i++) {
        $bmp = $bmps[$i]
        if ($null -eq $bmp) { Write-Warning "Frame $($i + 1) capture failed (zero-size window)."; continue }
        $framePath = Join-Path $dirp ("{0}_{1}{2}" -f $base, ($i + 1), $ext)
        $bmp.Save($framePath, [System.Drawing.Imaging.ImageFormat]::Png)
        if ($i -eq 0) { $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png) }
        $bmp.Dispose()
        Write-Host "Saved frame: $framePath"
    }
}
