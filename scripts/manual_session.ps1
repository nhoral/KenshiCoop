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
    # Host-only recruit helper (KENSHICOOP_AUTORECRUIT=N seconds): N s after
    # gameplay settles, the host ONCE programmatically recruits the nearest
    # non-player world NPC via the same PlayerInterface::recruit the dialog
    # "join me" hits. Use to validate recruit sync on a populated save that has
    # no dialog-hireable NPC (camp/squad1). 0 = off.
    [int]$AutoRecruit = 0,
    # Host-only bounty helper (KENSHICOOP_AUTOCRIME=N seconds): N s after gameplay
    # settles, the host ONCE programmatically assigns a test bounty (500) to the
    # player-squad member at -AutoCrimeIndex via unfairAddToBounty. In -Inhabit
    # mode a non-zero index is a JOIN-owned character's driven copy, so this
    # reproduces the H2 witness-local state and validates protocol-44 sync
    # deterministically (host publishes, join applies). 0 = off.
    [int]$AutoCrime = 0,
    [int]$AutoCrimeIndex = 1,
    # Inhabit mode: both clients load the SAME save (so NPC sync works) but each
    # OWNS a different subset of the shared squad. Default split: host owns the
    # leader (index 0), join inhabits everyone else (~0). Overridable via
    # -HostOwn/-JoinOwn (KENSHICOOP_OWN_INDICES: "0", "~0", "1,2", ""=own all).
    # Forces shared save + no autospawn.
    [switch]$Inhabit,
    [string]$HostOwn = "0",
    [string]$JoinOwn = "~0",
    # Push-save-on-connect test: launch the JOIN with NO save so it sits at the
    # main menu (Continue / New Game / ...). The host loads $Save and, on the
    # join's connect, bakes + announces its live world (LOAD_GO); the join loads
    # it straight from the menu. On a single machine both installs share the save
    # folder, so the join fingerprint MATCHES and it loads from disk (the connect
    # trigger + title-screen load pump); the folder-transfer half only engages
    # when the join genuinely lacks the save (a real second machine). Implies
    # -Inhabit ownership so each side drives a distinct squad subset.
    [switch]$JoinFromMenu,
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
    # Phase 6 shackle diagnostic (KENSHICOOP_DEBUG_SHACKLE=1 on both clients):
    # emits the ~1 Hz [shackledbg] per-body chained/lock trace so a manual camp
    # session captures the exact tick a peer's driven copy diverges from the
    # owner (the reported "peer PC unlocks the shackles" desync).
    [switch]$DebugShackle,
    # Jail long-play diagnostics (spike 58, KENSHICOOP_JAIL_PROBE/TASK_SPIKE=1 on
    # both clients): arms the [jail] STATE/SNAP captive traces + [spike] SELECT
    # task-selection + auditRows (SCENARIO WNPC/WORLD) so a manual
    # jailed/slaves/cage2 session captures the guard put-to-work cage<->pole
    # oscillation and census drift on BOTH sides with the NORMAL self-heal on
    # (i.e. real replicated behavior). On exit both logs are copied into
    # tools/manual-sessions/<stamp>/. All read-only, OFF by default.
    [switch]$JailProbe,
    # Jail OBSERVE (spike 57 phase A, KENSHICOOP_JAIL_OBSERVE=1): OPT-IN, requires
    # -JailProbe. Runs peer-owned captives UNOPPOSED (drive/suspend/self-heal OFF)
    # to classify the guard put-to-work trajectory. WARNING: this DISABLES the
    # captive self-heal, so captives WILL exit furniture and walk off on the
    # observing side (a diagnostic divergence, NOT a real desync). Do NOT use for
    # parity/visual A-B; use plain -JailProbe for that.
    [switch]$JailObserve,
    # Bounty/crime probe (spike 59, KENSHICOOP_BOUNTY_PROBE=1 on BOTH clients):
    # arms the read-only [bounty] observer (STATE/ROW per body with live crime or
    # bounty state, + 10 s SCAN heartbeat) so a manual session that commits a
    # witnessed crime captures whether the bounty/crime state forks host<->join
    # and which fields move. Reads the inline BountyManager at Character+0xF0
    # (me-sentinel guarded); read-only, zero behavior change, OFF by default.
    [switch]$BountyProbe,
    # Manual sessions tile the two game windows side-by-side BY DEFAULT (host left,
    # join right; scripts\arrange_windows.ps1, re-pinned through the load screen) on
    # the ULTRAWIDE (widest monitor, 3440x1440): each client window is sized to half
    # the ultrawide via kenshi.cfg Video Mode (set_video_mode.ps1) so visual A/B
    # validation FILLS the screen. -NoTile skips both the resize and the tiling.
    # (-Tile is legacy/accepted; tiling is on unless -NoTile.)
    [switch]$Tile,
    [switch]$NoTile,
    [ValidateSet("widest", "primary")]
    [string]$TileMonitor = "widest",
    # Client (render area) size per window for the manual layout: 1720x1440 fills the
    # 3440x1440 ultrawide edge-to-edge (2x1720 = 3440 wide, full 1440 height). This is
    # the MANUAL-only resolution; automated runs (run_test.ps1) keep the smaller
    # 1280x1024 primary-monitor layout, so the two harnesses never fight over the cfg.
    [int]$WindowW = 1720,
    [int]$WindowH = 1440,
    # How long to keep re-pinning. Must outlast host launch + join delay + both load
    # screens (Kenshi re-centers on gameplay entry), matching run_test.ps1's default.
    [int]$TileRepeatSec = 75
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Tiling is the manual-session default; -NoTile opts out (-Tile kept for compat).
$doTile = -not $NoTile

$hostExe = Join-Path $HostDir "kenshi_x64.exe"
$joinExe = Join-Path $JoinDir "kenshi_x64.exe"
if (-not (Test-Path $hostExe)) { throw "Host Kenshi not found: $hostExe" }
if (-not $NoJoin -and -not (Test-Path $joinExe)) { throw "Join Kenshi not found: $joinExe (run scripts\setup_join_install.cmd)" }

if ($JailObserve -and -not $JailProbe) {
    throw "-JailObserve requires -JailProbe (it only toggles observe mode on top of the jail probes)."
}

if ($JoinSave -eq "") { $JoinSave = $Save }

if ($Inhabit -or $JoinFromMenu) {
    $JoinSave  = $Save   # shared save: NPC resolve-by-hand needs identical hands
    $AutoSpawn = 0       # inhabit drives EXISTING squad members, not spawned ones
}

# Validate the saves exist (auto-loading a missing save crashes the game). Both
# installs read named saves from the same per-user folder, so a save created in
# either client is visible to both - no copy/sync needed.
$saveRoot = Join-Path $env:LOCALAPPDATA "kenshi\save"
# JoinFromMenu launches the join with no save (it sits at the menu), so only the
# host's save must exist up front.
$savesToCheck = if ($JoinFromMenu) { @($Save) } else { @($Save, $JoinSave) }
foreach ($s in $savesToCheck | Select-Object -Unique) {
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

# Size both clients for the manual layout BEFORE launch (Kenshi reads kenshi.cfg
# Video Mode at startup; the tiler only moves). run_test.ps1 writes the automated
# layout back on its next run, so the two harnesses never fight over the cfg.
if ($doTile) {
    Write-Host ""
    Write-Host "=== window layout: manual (${WindowW}x${WindowH} x2, $TileMonitor monitor) ==="
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scriptDir "set_video_mode.ps1") `
        -Width $WindowW -Height $WindowH -HostDir $HostDir -JoinDir $JoinDir
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
    # Manual sessions run BOTH clients on this one machine (same Steam account), so
    # they must use direct-UDP loopback: a same-machine Steam P2P session can't
    # establish (active=0/err=4). Force it here rather than inheriting the deployed
    # coop_config.json, which may be left on transport=steam + a real steamPeer from
    # a friend session (that silently breaks the loopback connection). Env overrides
    # the file. For a real two-machine Steam test, use the in-game F2 panel instead.
    $env:KENSHICOOP_TRANSPORT    = "udp"
    $env:KENSHICOOP_STEAM_PEER   = "0"
    # Auto-connect at load using the env role/transport above. EXCEPTION: with
    # -JoinFromMenu the JOIN must WAIT at the main menu so the user can bring up
    # the F2 panel and go ONLINE by hand (the whole point of that mode - the join
    # user configures the connection at the menu). The host still auto-connects
    # (it hosts + loads its save); on the join's manual connect the host pushes
    # its world.
    $env:KENSHICOOP_AUTOCONNECT  = if ($Mode -eq "join" -and $JoinFromMenu) { "0" } else { "1" }
    $env:KENSHICOOP_PORT         = "$Port"
    $env:KENSHICOOP_IP           = $Ip
    $env:KENSHICOOP_SAVE         = $SaveName
    $env:KENSHICOOP_TEST_SECONDS = "0"     # manual: never self-exit
    $env:KENSHICOOP_SCENARIO     = ""      # manual: no scenario
    $env:KENSHICOOP_AUTOSPAWN    = "$Spawn"
    $env:KENSHICOOP_OWN_INDICES  = $Own    # inhabit partition ("" = own all)
    # Host-only one-shot world spawn for baking a deterministic test scene.
    $env:KENSHICOOP_SETUP        = if ($Mode -eq "join") { "" } else { $SetupScene }
    # Host-only auto-recruit (N s after gameplay): recruit nearest world NPC.
    $env:KENSHICOOP_AUTORECRUIT  = if ($Mode -eq "join") { "" } else { "$AutoRecruit" }
    # Host-only auto-crime (protocol 44 validation): inject a test bounty on a
    # driven join-owned PC N s in, so the channel carries it to the owner.
    $env:KENSHICOOP_AUTOCRIME       = if ($Mode -eq "join") { "" } else { "$AutoCrime" }
    $env:KENSHICOOP_AUTOCRIME_INDEX = if ($Mode -eq "join") { "" } else { "$AutoCrimeIndex" }
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
    # Phase 6 shackle trace on both clients (see -DebugShackle).
    $env:KENSHICOOP_DEBUG_SHACKLE = if ($DebugShackle) { "1" } else { "" }
    # Jail long-play probes on BOTH clients (spike 58, see -JailProbe): STATE/SNAP
    # + task-selection traces + auditRows (auditRows keys off KENSHICOOP_JAIL_PROBE
    # in Plugin.cpp when no scenario name is set). NOTE: JAIL_OBSERVE is NOT armed
    # by -JailProbe - observe disables the captive self-heal (captives exit
    # furniture + walk off on the observing side), which is a diagnostic
    # divergence, not real behavior. It is opt-in via -JailObserve.
    $env:KENSHICOOP_JAIL_PROBE   = if ($JailProbe) { "1" } else { "" }
    $env:KENSHICOOP_TASK_SPIKE   = if ($JailProbe) { "1" } else { "" }
    $env:KENSHICOOP_JAIL_OBSERVE = if ($JailObserve) { "1" } else { "" }
    # Bounty/crime read-only probe on BOTH clients (spike 59, see -BountyProbe):
    # bounties are a player-facing per-character system, so both squads are read
    # on both sides - the fork evidence is the host-vs-join ROW diff per hand.
    $env:KENSHICOOP_BOUNTY_PROBE = if ($BountyProbe) { "1" } else { "" }
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
$hostOwnEnv = if ($Inhabit -or $JoinFromMenu) { $HostOwn } else { "" }
$joinOwnEnv = if ($Inhabit -or $JoinFromMenu) { $JoinOwn } else { "" }

Write-Host "Launching HOST (save $Save, autospawn $AutoSpawn) ..."
Set-CoopEnv -Mode "host" -SaveName $Save -Spawn $AutoSpawn -Own $hostOwnEnv
$hostPid = Start-PastLauncher -Exe $hostExe -WorkDir $HostDir
if ($hostPid -eq 0) { throw "Host failed to get past the launcher." }

if (-not $NoJoin) {
    Write-Host "Waiting $JoinDelaySec s before launching JOIN ..."
    Start-Sleep -Seconds $JoinDelaySec
    # JoinFromMenu: empty save name -> the join stays at the main menu and the
    # host's push-on-connect pulls it into the world.
    $joinSaveName = if ($JoinFromMenu) { "" } else { $JoinSave }
    Write-Host "Launching JOIN (save $(if ($JoinFromMenu) { '<main menu - no save>' } else { $JoinSave })) ..."
    Set-CoopEnv -Mode "join" -SaveName $joinSaveName -Spawn 0 -Own $joinOwnEnv
    $joinPid = Start-PastLauncher -Exe $joinExe -WorkDir $JoinDir
    if ($joinPid -eq 0) { Write-Warning "Join failed to get past the launcher; host is up alone." }
}

Write-Host ""
Write-Host "== Session live =="
Write-Host "  host PID=$hostPid  join PID=$joinPid"
if ($JoinFromMenu) {
    Write-Host "  JOIN is waiting at the MAIN MENU (no save, autoconnect OFF)."
    Write-Host "  On the JOIN window: press F2 -> set Connection to ONLINE (role JOIN, UDP is"
    Write-Host "  preset for loopback). The host then pushes its world and the join loads in"
    Write-Host "  straight from the menu - no save needed on the join. Close windows when done."
} else {
    Write-Host "  The host spawns $AutoSpawn squad members a few seconds after gameplay starts."
    Write-Host "  Select them on the HOST and move them around; watch the JOIN render and"
    Write-Host "  follow them. Close both windows when you're done (no auto-exit)."
}

if ($doTile -and $hostPid -ne 0) {
    # Same mechanism the automated tests use (scripts\arrange_windows.ps1): place the
    # two windows side by side (host left / join right) and RE-PIN through the load
    # screen, because Kenshi re-centers its window on the load->gameplay switch. Runs in
    # the background so this launcher returns immediately. Requires windowed mode
    # (kenshi.cfg Full Screen=No); it MOVES only, preserving each window's native size.
    $arrangeScript = Join-Path $scriptDir "arrange_windows.ps1"
    Write-Host ""
    Write-Host "Arranging windows side by side ($TileMonitor monitor, host left / join right; re-pinning ${TileRepeatSec}s) ..."
    # -ClientW/-ClientH: enforce the manual layout size at placement time too.
    # Kenshi is system-DPI-aware, so when the primary (laptop) monitor runs a
    # different DPI scale than the ultrawide, Windows RESCALES the window as the
    # tiler moves it across - a move-only pin lands it the wrong size.
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$arrangeScript`"",
        "-HostPid", "$hostPid", "-JoinPid", "$joinPid",
        "-Monitor", $TileMonitor, "-TimeoutSec", "90", "-RepeatSec", "$TileRepeatSec",
        "-ClientW", "$WindowW", "-ClientH", "$WindowH"
    ) | Out-Null
}

# Jail long-play (spike 58): auto-collect BOTH logs when the session ends. The
# launcher returns immediately (manual play), so a hidden background waiter
# blocks on the two game PIDs and copies host+join logs into a stamped dir the
# moment you close the windows - no manual copy step, no clobbered evidence.
if ($JailProbe -and $hostPid -ne 0) {
    $repoRoot   = Split-Path -Parent $scriptDir
    $stamp      = Get-Date -Format "yyyyMMdd_HHmmss"
    $dest       = Join-Path $repoRoot ("tools\manual-sessions\jail_{0}_{1}" -f $Save.Replace(' ', '_'), $stamp)
    $hostLogSrc = Join-Path $HostDir "KenshiCoop_host.log"
    $joinLogSrc = Join-Path $JoinDir "KenshiCoop_join.log"
    $pidCsv     = if ($joinPid -ne 0) { "$hostPid,$joinPid" } else { "$hostPid" }
    $waiterPs   = Join-Path $env:TEMP ("kc_jail_waiter_{0}.ps1" -f $stamp)
    @"
`$ErrorActionPreference = 'SilentlyContinue'
Wait-Process -Id $pidCsv
New-Item -ItemType Directory -Force -Path '$dest' | Out-Null
Copy-Item '$hostLogSrc' (Join-Path '$dest' 'KenshiCoop_host.log')
Copy-Item '$joinLogSrc' (Join-Path '$dest' 'KenshiCoop_join.log')
"@ | Set-Content -Path $waiterPs -Encoding UTF8
    Start-Process -WindowStyle Hidden -FilePath "powershell" -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$waiterPs`""
    ) | Out-Null
    Write-Host ""
    if ($JailObserve) {
        Write-Host "  [jail] probes ARMED on both clients: STATE/SNAP + [spike] SELECT + auditRows + OBSERVE."
        Write-Host "  [jail] WARNING: -JailObserve disables the captive self-heal - captives WILL exit"
        Write-Host "  [jail]          furniture + walk off on the observing side (diagnostic, NOT a real desync)."
    } else {
        Write-Host "  [jail] probes ARMED on both clients: STATE/SNAP + [spike] SELECT + auditRows (self-heal ON)."
    }
    Write-Host "  [jail] on exit both logs -> $dest"
}

# Build-freshness guard: confirm the running plugin is the one we expect. In
# -Inhabit mode the plugin logs "inhabit ownership = ..." once at load; if that
# line never appears, the installs are running a stale DLL (the deploy-skip trap)
# and any validation would be meaningless. Surface it loudly instead of silently
# validating the wrong build.
if ($Inhabit -or $JoinFromMenu) {
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
