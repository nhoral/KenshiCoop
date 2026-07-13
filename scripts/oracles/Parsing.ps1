# oracles/Parsing.ps1 - clock-offset + log-parsing helpers (monolith split of
# CoopOracles.psm1, 2026-07-12): Get-LogClockOffsetMs, Get-ClockSyncStats,
# Convert-StampToMs, Get-ScenarioLines, Get-ScenarioSeries, Get-MarkerTimeMs.
# Dot-sourced by CoopOracles.psm1 into the MODULE scope, so $script: state
# ($script:ClockOffsetCache) is shared with every other fragment.
# Must NOT: contain oracles (Test-*) - parsers only. Regex phrasing is the
# C++-to-oracle log contract (resources/CODE_MAP.md, log-tag index).
# ---- Clock-offset helpers -----------------------------------------------------

# Per-log wall-clock offset (ms) relative to the HOST clock, estimated by the
# plugin's wire time-sync and logged as "CLOCKSYNC offset=<ms> rtt=<ms>". We take
# the minimum-RTT sample (the classic NTP filter: least queueing noise). A log
# with no CLOCKSYNC lines (host, or a pre-time-sync DLL) gets offset 0.
function Get-LogClockOffsetMs {
    param([string]$File)
    if (-not (Test-Path $File)) { return 0 }
    if ($script:ClockOffsetCache.ContainsKey($File)) { return $script:ClockOffsetCache[$File] }
    $best = $null; $bestRtt = [int]::MaxValue
    foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
        $off = [int]$m.Matches[0].Groups[1].Value
        $rtt = [int]$m.Matches[0].Groups[2].Value
        if ($rtt -le $bestRtt) { $bestRtt = $rtt; $best = $off }
    }
    $result = if ($null -ne $best) { $best } else { 0 }
    $script:ClockOffsetCache[$File] = $result
    return $result
}

# Count of CLOCKSYNC samples + the min-RTT sample, for the clock_sync gate.
function Get-ClockSyncStats {
    param([string]$File)
    $n = 0; $bestOff = $null; $bestRtt = [int]::MaxValue
    if (Test-Path $File) {
        foreach ($m in (Select-String -Path $File -Pattern 'CLOCKSYNC offset=(-?\d+) rtt=(\d+)' -ErrorAction SilentlyContinue)) {
            $n++
            $off = [int]$m.Matches[0].Groups[1].Value
            $rtt = [int]$m.Matches[0].Groups[2].Value
            if ($rtt -le $bestRtt) { $bestRtt = $rtt; $bestOff = $off }
        }
    }
    return [pscustomobject]@{ samples = $n; offsetMs = $bestOff; rttMs = $(if ($n -gt 0) { $bestRtt } else { $null }) }
}

# Parse a "[HH:MM:SS.mmm]" stamp match-group set into ms-since-midnight, applying
# the log's clock offset so all returned times are in the HOST clock frame.
function Convert-StampToMs {
    param($Groups, [int]$OffsetMs)
    $t = ([int]$Groups[1].Value * 3600 + [int]$Groups[2].Value * 60 + [int]$Groups[3].Value) * 1000 + [int]$Groups[4].Value
    return $t + $OffsetMs
}

# ---- Log parsing helpers ------------------------------------------------------

# Parse SCENARIO <Kind> hand=.. pos=.. lines from a log into hand -> "x,y,z"
# (latest logged position per hand wins). $Kind is MEMBER or RECV.
function Get-ScenarioLines {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $pat = "SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $map[$g[1].Value] = $g[2].Value
    }
    return $map
}

# Parse timestamped SCENARIO <Kind> lines into hand -> list of @{t=ms; p=@(x,y,z)}.
# Timestamps are converted into the HOST clock frame via the log's CLOCKSYNC
# offset (0 when absent), so host and join series are directly time-comparable
# even across machines with disagreeing wall clocks.
function Get-ScenarioSeries {
    param([string]$File, [string]$Kind)
    $map = @{}
    if (-not (Test-Path $File)) { return $map }
    $off = Get-LogClockOffsetMs -File $File
    $pat = "\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*SCENARIO $Kind hand=([\d,]+) pos=([\-\d\.,]+)(?: task=(\d+))?(?: pelvis=(-?[\d\.]+))?(?: crouch=(-?\d+))?(?: idle=(-?\d+))?(?: bs=(\d+))?"
    foreach ($m in (Select-String -Path $File -Pattern $pat -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $t = Convert-StampToMs -Groups $g -OffsetMs $off
        $hand = $g[5].Value
        $pos  = $g[6].Value.Split(',') | ForEach-Object { [double]$_ }
        $task = if ($g[7].Success) { [int]$g[7].Value } else { 65535 }
        $pelvis = if ($g[8].Success) { [double]$g[8].Value } else { -1.0 }
        $crouch = if ($g[9].Success) { [int]$g[9].Value } else { -1 }
        $bs   = if ($g[11].Success) { [int]$g[11].Value } else { 0 }
        if (-not $map.ContainsKey($hand)) { $map[$hand] = New-Object System.Collections.ArrayList }
        [void]$map[$hand].Add(@{ t = $t; p = $pos; task = $task; pelvis = $pelvis; crouch = $crouch; bs = $bs })
    }
    return $map
}

# Find the first timestamped marker line matching $Pattern in $File and return its
# ms-since-midnight in the HOST clock frame, or $null.
function Get-MarkerTimeMs {
    param([string]$File, [string]$Pattern)
    $om = Select-String -Path $File -Pattern ("\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*" + $Pattern) -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $om) { return $null }
    $off = Get-LogClockOffsetMs -File $File
    return (Convert-StampToMs -Groups $om.Matches[0].Groups -OffsetMs $off)
}

