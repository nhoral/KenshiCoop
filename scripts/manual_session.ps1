<#
.SYNOPSIS
  Launch a HOST + JOIN co-op session for MANUAL validation (you drive, no
  self-exit, no screenshots). The host auto-spawns a few distinct-hand squad
  members so you can move them and watch the join render/follow them live.

.DESCRIPTION
  Unlike run_test.ps1 (timed/scenario runs that self-exit and screenshot), this
  leaves both clients running so you can play. It drives the plugin via env vars:
    KENSHICOOP_SAVE         - save both clients auto-load
    KENSHICOOP_TEST_SECONDS - forced 0 here (NO self-exit; you close the windows)
    KENSHICOOP_SCENARIO     - forced empty (normal co-op tick, no scenario)
    KENSHICOOP_AUTOSPAWN    - host only: spawn N distinct squad members once
  Those spawned units get fresh hands (not in the join's own squad), so the join
  renders them as proxies - exercising the cross-client squad-render path on a
  single shared save, no second save needed.

  Host runs from the Steam install; join from the Kenshi-Join install. With
  -Sync the host save is mirrored into the join install first.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "c" -Sync

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "c" -AutoSpawn 5 -SkipBuild -Sync

.EXAMPLE
  # Long-run visual pop/snap inspection: 'zoom' save (outside town, camera far
  # out) + colored authority markers on the join (green DRV / red HID / yellow LOC).
  powershell -ExecutionPolicy Bypass -File scripts\manual_session.ps1 -Save "zoom" -Inhabit -DebugMarkers -Tile
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Save,
    # Save the JOIN client loads. Defaults to $Save (shared-save mode). Pass a
    # DIFFERENT save (with its own characters) so the two squads have distinct
    # hands and fully render each other - including the player-controlled leaders.
    [string]$JoinSave = "",
    [int]$AutoSpawn = 3,
    # Inhabit mode: both clients load the SAME save (so NPC sync works) but each
    # OWNS a different subset of the shared squad. Default split: host owns the
    # leader (index 0), join inhabits everyone else (~0). Overridable via
    # -HostOwn/-JoinOwn (KENSHICOOP_OWN_INDICES: "0", "~0", "1,2", ""=own all).
    # Forces shared save + no autospawn.
    [switch]$Inhabit,
    [string]$HostOwn = "0",
    [string]$JoinOwn = "~0",
    [int]$Port = 27800,
    [string]$Ip = "127.0.0.1",
    [string]$HostDir = "C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
    [string]$JoinDir = "$env:USERPROFILE\Kenshi-Join",
    [switch]$Sync,
    [switch]$SkipBuild,
    [switch]$SkipDeploy,
    [switch]$NoJoin,
    [int]$JoinDelaySec = 8,
    [int]$StartTimeoutSec = 90,
    # Host-only deterministic test-scene setup (KENSHICOOP_SETUP). "chair" spawns a
    # seat in front of the player; "npc" also spawns a loose world NPC. The host
    # then stays up (no self-exit) so you can arrange the pose and SAVE the game.
    [string]$SetupScene = "",
    # AI-gating probe (join only, KENSHICOOP_PROBE_RECRUIT=1): recruit diverged
    # NPCs into the player squad to validate the "inhabit" lever.
    [switch]$ProbeRecruit,
    # AI-suspend probe (join only, KENSHICOOP_PROBE_AISUSPEND=1): detour
    # Character::periodicUpdate so host-driven NPCs stop self-tasking (decision
    # layer off) while still animating. Faction is untouched.
    [switch]$ProbeAiSuspend,
    # Bidirectional inventory sync (KENSHICOOP_INV_SYNC=1 on BOTH clients): each side
    # streams the contents of the squad tabs it owns (host tab 0, join tab 1) and
    # reconciles the peer's. Add an item to a join-owned character and watch it appear
    # on the host (and vice versa). Pair with -SetupScene inventory to also seed the
    # leader so the host->join direction is visible at startup.
    [switch]$InvSync,
    # World-item sync (KENSHICOOP_WORLD_SYNC=1 on BOTH clients, Phase W1): the HOST streams
    # free GROUND items in the interest sphere; the JOIN spawns local proxies so a dropped
    # item appears on the join at the same spot (and is culled when it despawns). In W1 only
    # host-authored drops sync (join-originated DROP intent is W2). Drop an item from a host
    # squad member's bag and watch it appear on the join.
    [switch]$WorldSync,
    # Diagnostic: log a full inventory dump (loose _allItems + every section + weapon
    # accessors) on each inventory SEND (host capture) and APPLY (peer reconcile result),
    # so we can see exactly where an unequipped weapon goes. Sets KENSHICOOP_INV_DUMP=1.
    [switch]$InvDump,
    # Debug authority markers (KENSHICOOP_DEBUG_MARKERS=1, spike-47 HUD labels):
    # the JOIN pins a colored label to every judged body - green DRV = host-
    # driven, red HID = suppressed/culled, yellow LOC = local-sim copy present
    # in the host census. Makes pops and ghosts self-explaining on screen
    # (pair with -Save zoom for long-run wide-camera inspection).
    [switch]$DebugMarkers,
    # Tile the two game windows side-by-side (host left, join right) the same way the
    # automated tests do (scripts\arrange_windows.ps1, re-pinned through the load
    # screen). Requires windowed mode (kenshi.cfg Full Screen=No).
    [switch]$Tile,
    [ValidateSet("widest", "primary")]
    [string]$TileMonitor = "primary",
    # How long to keep re-pinning. Must outlast host launch + join delay + both load
    # screens (Kenshi re-centers on gameplay entry), matching run_test.ps1's default.
    [int]$TileRepeatSec = 75
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not $NoJoin -and -not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

if ($JoinSave -eq "") { $JoinSave = $Save }

if ($Inhabit) {
    $JoinSave  = $Save   # shared save: NPC resolve-by-hand needs identical hands
    $AutoSpawn = 0       # inhabit drives EXISTING squad members, not spawned ones
}

# Validate the saves exist (auto-loading a missing save crashes the game). Both
# installs read named saves from the same per-user folder, so a save created in
# either client is visible to both - no copy/sync needed.
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
foreach ($s in @($Save, $JoinSave) | Select-Object -Unique) {
    if (-not (Test-Path (Join-Path $saveRoot $s))) {
        $avail = (Get-ChildItem $saveRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object Name) -join ", "
        throw "Save '$s' not found in $saveRoot. Available saves: $avail"
    }
}

Write-Host "== KenshiCoop MANUAL session =="
Write-Host "  host save:  $Save"
Write-Host "  join save:  $JoinSave"
Write-Host "  autospawn:  $AutoSpawn (host only)"
Write-Host "  self-exit:  OFF (close the windows yourself when done)"
if ($Inhabit) {
    Write-Host "  mode:       INHABIT (shared save, partitioned squad ownership)"
    Write-Host "  host owns:  $HostOwn   join owns:  $JoinOwn  (squad-member rank: 0 = leader)"
} elseif ($Save -ne $JoinSave) {
    # Distinct-save is a v1 dead-end: NPC identity is resolve-by-hand, which needs
    # IDENTICAL saves on both clients. Two different saves mint different hands, so
    # NPCs (and squads) fail to resolve and desync. Kept only for experiments.
    Write-Warning "DISTINCT-SAVE MODE IS UNSUPPORTED FOR v1."
    Write-Warning "  Host loads '$Save' but join loads '$JoinSave'. NPC sync is resolve-by-hand and"
    Write-Warning "  REQUIRES the same save on both clients - expect NPC/squad desync. Use -Inhabit"
    Write-Warning "  (shared save + partitioned ownership) for the supported two-player path."
} else {
    Write-Host "  NOTE: shared save in OWN-ALL mode - both clients claim the same squad, so the"
    Write-Host "        leaders share a hand and won't render cross-client. Pass -Inhabit to"
    Write-Host "        partition ownership so each player's character renders + drives on the other."
}

# Clean slate so build/deploy aren't blocked by a loaded DLL / running game.
$stale = @(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
    Write-Host "  killing $($stale.Count) stale Kenshi process(es) ..."
    $stale | Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 20; $i++) {
        if (@(Get-Process -Name "Kenshi_x64", "kenshi_x64" -ErrorAction SilentlyContinue).Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }
}

# Build and deploy are INDEPENDENT steps. -SkipBuild reuses the existing built
# DLL but STILL DEPLOYS it (the old behavior skipped deploy too, which silently
# ran a stale DLL in the installs - the bug that invalidated the inhabit test).
# Only -SkipDeploy skips the copy into the installs.
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== build plugin ==="
    & cmd /c "`"$scriptDir\build_plugin.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build_plugin.cmd failed ($LASTEXITCODE)" }
}
if (-not $SkipDeploy) {
    Write-Host ""
    Write-Host "=== deploy ==="
    & cmd /c "`"$scriptDir\deploy.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "deploy.cmd failed ($LASTEXITCODE)" }
    # Surface exactly which DLL is now deployed so we never validate a stale build.
    $deployed = Join-Path $HostDir "mods\KenshiCoop\KenshiCoop.dll"
    if (Test-Path $deployed) {
        $info = Get-Item $deployed
        Write-Host ("  deployed DLL: {0}  ({1:yyyy-MM-dd HH:mm:ss}, {2} bytes)" -f $deployed, $info.LastWriteTime, $info.Length)
    }
} else {
    Write-Host ""
    Write-Host "=== deploy SKIPPED (-SkipDeploy): installs keep their current DLL ==="
}

if ($Sync -and ($Save -eq $JoinSave)) {
    Write-Host ""
    Write-Host "Syncing saves host -> join ..."
    & cmd /c "`"$scriptDir\sync_save.cmd`" `"$HostDir`" `"$JoinDir`""
    if ($LASTEXITCODE -ne 0) { throw "sync_save.cmd failed ($LASTEXITCODE)" }
} elseif ($Sync) {
    Write-Host ""
    Write-Host "Skipping -Sync: host and join load different saves (distinct-save mode)."
}

function Set-CoopEnv {
    param([string]$Mode, [string]$SaveName, [int]$Spawn, [string]$Own = "")
    $env:KENSHICOOP_MODE         = $Mode
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_IP           = $Ip
    $env:KENSHICOOP_SAVE         = $SaveName
    $env:KENSHICOOP_TEST_SECONDS = "0"     # manual: never self-exit
    $env:KENSHICOOP_SCENARIO     = ""      # manual: no scenario
    $env:KENSHICOOP_AUTOSPAWN    = "$Spawn"
    $env:KENSHICOOP_OWN_INDICES  = $Own    # inhabit partition ("" = own all)
    # Host-only one-shot world spawn for baking a deterministic test scene.
    $env:KENSHICOOP_SETUP        = if ($Mode -eq "join") { "" } else { $SetupScene }
    $env:KENSHICOOP_PROBE_RECRUIT = if ($Mode -eq "join" -and $ProbeRecruit) { "1" } else { "" }
    $env:KENSHICOOP_PROBE_AISUSPEND = if ($Mode -eq "join" -and $ProbeAiSuspend) { "1" } else { "" }
    # Inventory sync is bidirectional, so enable it on BOTH clients.
    $env:KENSHICOOP_INV_SYNC     = if ($InvSync) { "1" } else { "" }
    # World-item sync is host-authored + join-observed, but the gate is read on both
    # clients (host publishes, join applies), so enable it on BOTH.
    $env:KENSHICOOP_WORLD_SYNC   = if ($WorldSync) { "1" } else { "" }
    $env:KENSHICOOP_INV_DUMP     = if ($InvDump) { "1" } else { "" }
    # Authority markers render on the DRIVEN side; harmless on both, so set both.
    $env:KENSHICOOP_DEBUG_MARKERS = if ($DebugMarkers) { "1" } else { "" }
    # Per-mode log next to the install so host/join don't clobber each other.
    $env:KENSHICOOP_LOG          = if ($Mode -eq "join") { "KenshiCoop_join.log" } else { "KenshiCoop_host.log" }
}

function Start-PastLauncher {
    param([string]$Exe, [string]$WorkDir)
    $out = & (Join-Path $scriptDir "start_kenshi.ps1") -ExePath $Exe -WorkDir $WorkDir -TimeoutSec $StartTimeoutSec 6>&1
    $out | ForEach-Object { Write-Host "    $_" }
    $line = $out | Where-Object { "$_" -match "GAMEPID=(\d+)" } | Select-Object -First 1
    if ($line -and ("$line" -match "GAMEPID=(\d+)")) { return [int]$Matches[1] }
    return 0
}

$joinPid = 0
Write-Host ""
$hostOwnEnv = if ($Inhabit) { $HostOwn } else { "" }
$joinOwnEnv = if ($Inhabit) { $JoinOwn } else { "" }

Write-Host "Launching HOST (save $Save, autospawn $AutoSpawn) ..."
Set-CoopEnv -Mode "host" -SaveName $Save -Spawn $AutoSpawn -Own $hostOwnEnv
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

if (-not $NoJoin) {
    Write-Host "Waiting $JoinDelaySec s before launching JOIN ..."
    Start-Sleep -Seconds $JoinDelaySec
    Write-Host "Launching JOIN (save $JoinSave) ..."
    Set-CoopEnv -Mode "join" -SaveName $JoinSave -Spawn 0 -Own $joinOwnEnv
    $joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
    if ($joinPid -eq 0) { Write-Warning "Join failed to get past the launcher; host is up alone." }
}

Write-Host ""
Write-Host "== Session live =="
Write-Host "  host PID=$hostPid  join PID=$joinPid"
Write-Host "  The host spawns $AutoSpawn squad members a few seconds after gameplay starts."
Write-Host "  Select them on the HOST and move them around; watch the JOIN render and"
Write-Host "  follow them. Close both windows when you're done (no auto-exit)."

if ($Tile -and $hostPid -ne 0) {
    # Same mechanism the automated tests use (scripts\arrange_windows.ps1): place the
    # two windows side by side (host left / join right) and RE-PIN through the load
    # screen, because Kenshi re-centers its window on the load->gameplay switch. Runs in
    # the background so this launcher returns immediately. Requires windowed mode
    # (kenshi.cfg Full Screen=No); it MOVES only, preserving each window's native size.
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host ""
    Write-Host "Arranging windows side by side ($TileMonitor monitor, host left / join right; re-pinning ${TileRepeatSec}s) ..."
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$hostPid", "-JoinPid", "$joinPid",
        "-Monitor", $TileMonitor, "-TimeoutSec", "90", "-RepeatSec", "$TileRepeatSec"
    ) | Out-Null
}

# Build-freshness guard: confirm the running plugin is the one we expect. In
# -Inhabit mode the plugin logs "inhabit ownership = ..." once at load; if that
# line never appears, the installs are running a stale DLL (the deploy-skip trap)
# and any validation would be meaningless. Surface it loudly instead of silently
# validating the wrong build.
if ($Inhabit) {
    $hostLog = Join-Path $HostDir "KenshiCoop_host.log"
    Write-Host ""
    Write-Host "Confirming the deployed build is the inhabit build (watching host log) ..."
    $deadline = (Get-Date).AddSeconds($StartTimeoutSec)
    $seen = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $hostLog) {
            # The plugin's load line reads "ownership ranks = {..}" (older builds
            # said "inhabit ownership = ..." - the grep must match the current one).
            $hit = Select-String -Path $hostLog -Pattern "ownership ranks = " -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $hit) { $seen = $true; Write-Host "  OK: $($hit.Line.Trim())"; break }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $seen) {
        Write-Warning "Did NOT see 'inhabit ownership' in the host log within $StartTimeoutSec s."
        Write-Warning "The installs may be running a STALE DLL - re-run without -SkipDeploy."
    }
}
