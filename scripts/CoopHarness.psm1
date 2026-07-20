# CoopHarness.psm1 - shared launch/environment helpers for the KenshiCoop test
# harness. Phase 2 extraction: the per-scenario ENVIRONMENT (channel A/B knobs +
# log-only diagnostic traces) is now OWNED by the manifest (scripts/scenarios.psd1
# `DiagEnv` block per scenario) and applied here, so run_test.ps1 and
# run_lan_test.ps1 stop carrying duplicated, drifting name-match chains and the
# plugin's Config.cpp stops hard-coding scenario names.
#
# Design:
#   * DiagEnv is MODE-AGNOSTIC (host and join get the same knobs) - every entry
#     here is a channel toggle or a trace flag, never an endpoint/role value.
#   * The applier is HERMETIC: it clears the whole managed keyset to "" first,
#     then applies the scenario's DiagEnv, so no knob leaks from a previous
#     scenario when several run in one process.
#   * Clearing a channel key to "" means the plugin's Config default applies
#     (envOr("...","1")!="0" for the always-on channels; the scenario==""/setup
#     auto rules for invSync/worldSync). DiagEnv only carries the DELTAS from
#     those defaults (a probe forcing a channel OFF, or a scenario forcing
#     invSync/worldSync ON), exactly the set Config.cpp used to name.

Set-StrictMode -Version Latest

# Canonical set of every env var any scenario's DiagEnv may set. Kept in ONE
# place so the applier can clear them all before applying a scenario's deltas,
# and so Contract.Tests can assert the manifest never introduces an unknown key.
$script:CoopDiagEnvKeys = @(
    # --- channel A/B knobs (Config.cpp reads these; DiagEnv carries the deltas) ---
    'KENSHICOOP_INV_SYNC'
    'KENSHICOOP_XFER_SYNC'
    'KENSHICOOP_BLOCK_XFER'
    'KENSHICOOP_WORLD_SYNC'
    'KENSHICOOP_SPEED_SYNC'
    'KENSHICOOP_MONEY_SYNC'
    'KENSHICOOP_SPAWN_SYNC'
    'KENSHICOOP_RECRUIT_SYNC'
    'KENSHICOOP_FACTION_SYNC'
    'KENSHICOOP_TIME_SYNC'
    'KENSHICOOP_DOOR_SYNC'
    'KENSHICOOP_BUILD_SYNC'
    'KENSHICOOP_BDOOR_SYNC'
    'KENSHICOOP_HUNGER_SYNC'
    'KENSHICOOP_SAVE_SYNC'
    'KENSHICOOP_LOAD_SYNC'
    'KENSHICOOP_PROD_SYNC'
    'KENSHICOOP_RESEARCH_SYNC'
    'KENSHICOOP_STORE_SYNC'
    'KENSHICOOP_SQUAD_SYNC'
    'KENSHICOOP_LATEJOIN_SYNC'
    # --- log-only diagnostic traces (read outside Config; never gate behavior) ---
    'KENSHICOOP_INV_DUMP'
    'KENSHICOOP_DEBUG_SPEED'
    'KENSHICOOP_DEBUG_SHACKLE'
    'KENSHICOOP_JAIL_PROBE'
    'KENSHICOOP_TASK_SPIKE'
    'KENSHICOOP_JAIL_OBSERVE'
)

function Get-CoopDiagEnvKeys {
    <#
    .SYNOPSIS
      The canonical list of env vars the manifest DiagEnv is allowed to set.
    #>
    return @($script:CoopDiagEnvKeys)
}

function Set-CoopDiagEnv {
    <#
    .SYNOPSIS
      Apply a scenario's manifest DiagEnv to the current process environment.
    .DESCRIPTION
      Clears the whole managed keyset first (hermetic), then applies the given
      manifest entry's DiagEnv hashtable. Returns the applied map (for logging /
      run.json provenance). Pass $null / an entry without DiagEnv to just clear.
    #>
    param($Entry)

    foreach ($k in $script:CoopDiagEnvKeys) { Set-Item -Path "env:$k" -Value "" }

    $applied = @{}
    if ($null -ne $Entry -and ($Entry -is [hashtable]) -and $Entry.ContainsKey('DiagEnv')) {
        $diag = $Entry.DiagEnv
        foreach ($k in $diag.Keys) {
            if ($script:CoopDiagEnvKeys -notcontains $k) {
                throw "DiagEnv key '$k' is not in CoopHarness::CoopDiagEnvKeys (manifest error)."
            }
            $val = "$($diag[$k])"
            Set-Item -Path "env:$k" -Value $val
            $applied[$k] = $val
        }
    }
    return $applied
}

function Stop-CoopKenshi {
    <#
    .SYNOPSIS
      Kill any stale Kenshi game processes from a previous (possibly crashed) run.
    #>
    param([int]$SettleSec = 2)
    $stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
    if ($stale.Count -gt 0) {
        $stale | Stop-Process -Force -ErrorAction SilentlyContinue
        if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }
    }
    return $stale.Count
}

Export-ModuleMember -Function Get-CoopDiagEnvKeys, Set-CoopDiagEnv, Stop-CoopKenshi
