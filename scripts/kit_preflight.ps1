# Shared preflight helpers for the friend kit scripts (friend_host.ps1 /
# friend_join.ps1 dot-source this). Every check exists because a real user hit
# the failure: RE_Kenshi missing = the plugin silently never loads; Steam
# closed = the P2P transport can't broker; Mark-of-the-Web on a downloaded
# zip = scripts/DLL blocked; fullscreen = alt-tab pain and unreliable
# screenshots. Fail fast with the exact fix instead of a silent dead session.

function Invoke-KitUnblock {
    # Strip Mark-of-the-Web from everything in the kit (zips downloaded from
    # the internet tag every extracted file; a tagged DLL can be refused by
    # the loader). Harmless when nothing is tagged.
    param([string]$KitDir)
    try {
        Get-ChildItem -Path $KitDir -Recurse -File -ErrorAction SilentlyContinue |
            Unblock-File -ErrorAction SilentlyContinue
    } catch {}
}

function Test-CoopPrereqs {
    # Throws (stopping the script) only for problems that guarantee a dead
    # session; anything survivable is a warning.
    param(
        [string]$KenshiDir,
        [bool]$UseSteam
    )
    Write-Host "Checking your setup ..."

    # RE_Kenshi: its loader DLL sits in the Kenshi install root. Without it
    # the co-op plugin is never loaded and the game runs vanilla.
    $rek = (Test-Path (Join-Path $KenshiDir "RE_Kenshi.dll")) -or
           (Test-Path (Join-Path $KenshiDir "RE_Kenshi")) -or
           (Test-Path (Join-Path $KenshiDir "dinput8.dll"))
    if (-not $rek) {
        throw ("RE_Kenshi not found in '$KenshiDir'. It is required (it loads the " +
               "co-op plugin into the game). Install it from " +
               "https://www.nexusmods.com/kenshi/mods/847 and run this again.")
    }
    Write-Host "  [ok] RE_Kenshi detected."

    if ($UseSteam) {
        $steam = Get-Process -Name steam -ErrorAction SilentlyContinue
        if ($null -eq $steam) {
            throw ("Steam is not running. The connection goes through Steam, so start " +
                   "Steam, log in (online, not offline mode), and run this again.")
        }
        Write-Host "  [ok] Steam is running (make sure you are ONLINE, not in offline mode)."
    }

    # Windowed mode: only warn - the session still works fullscreen, it is
    # just awkward (alt-tab to read this window) and screenshots can fail.
    # Kenshi's launcher only writes a fullscreen line when it is enabled, so
    # no line = windowed = fine.
    $cfg = Join-Path $KenshiDir "settings.cfg"
    if (Test-Path $cfg) {
        $fs = Select-String -Path $cfg -Pattern '^\s*full\s*screen\s*=\s*1' -ErrorAction SilentlyContinue |
              Select-Object -First 1
        if ($null -ne $fs) {
            Write-Warning "Kenshi appears to be set to FULL SCREEN. Windowed mode is strongly"
            Write-Warning "recommended: launch Kenshi once, Options > Video > un-check Full Screen."
        } else {
            Write-Host "  [ok] Kenshi video mode looks windowed."
        }
    }
}

function Wait-PluginLoaded {
    # The co-op plugin creates its log file the moment RE_Kenshi loads it
    # (before any menu/save). If the game is up but the log never appears,
    # RE_Kenshi did not load the plugin - tell the user exactly where to look
    # instead of letting them stare at a vanilla game.
    param(
        [string]$LogPath,
        [string]$KenshiDir,
        [int]$TimeoutSec = 120
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $LogPath) {
            Write-Host "  [ok] Co-op plugin loaded."
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning "The co-op plugin has not started (no log after $TimeoutSec s)."
    Write-Warning "The game may be running WITHOUT co-op. Check that RE_Kenshi is working:"
    Write-Warning "  $KenshiDir\RE_Kenshi_log.txt should mention 'KenshiCoop'."
    Write-Warning "If RE_Kenshi is missing, install it: https://www.nexusmods.com/kenshi/mods/847"
    return $false
}
