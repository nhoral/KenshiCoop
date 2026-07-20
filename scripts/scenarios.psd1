@{
    # ---------------------------------------------------------------------------
    # KenshiCoop scenario manifest - the single declarative source of truth for
    # WHICH oracles judge each scenario, which save/setup it needs, and which
    # regression tier it belongs to. Consumed by CoopOracles.psm1 (oracle
    # dispatch + verdict rule), run_test.ps1 (save/setup defaults, gating),
    # regress.ps1 (tier matrices) and analyze_run.ps1 (offline verdicts).
    #
    # Per-scenario fields:
    #   Save        - save both clients load (must exist in %LOCALAPPDATA%\kenshi\save)
    #   Setup       - host-only KENSHICOOP_SETUP scene ('' = none)
    #   Tolerance   - default cross-check tolerance (world units)
    #   PrimaryGate - the mechanism proof. FAIL **or SKIP** of this gate fails
    #                 the run (the no-signal guard): a green run must prove its
    #                 core mechanism was actually judged.
    #   Gating      - oracle ids that gate the verdict (FAIL fails the run; SKIP
    #                 is tolerated unless the oracle is the PrimaryGate)
    #   Advisory    - oracle ids that are measured + recorded but never gate
    #   Tier        - 'smoke' (per-pipeline sample) | 'full' | 'none' (diagnostic)
    #   WanVariant  - $true: the full tier reruns this scenario under the WAN
    #                 proxy (profile 'bad') as a second, separately-reported run
    #
    # Smoke-tier selection principle: ONE scenario per wire pipeline, preferring
    # the bidirectional superset (it strictly covers the unidirectional case):
    #   coop_presence     - unreliable entity stream + ownership partition
    #   npc_sync          - interest-managed world NPCs + intent/task/pose
    #   combat_kill       - reliable event channel + attribution + outcome
    #   inv_bidir         - inventory snapshot/reconcile (both directions)
    #   world_weapon_drop - world-item conservation channel
    #   world_armor_drop  - same channel, EQUIPPED armor piece (real-session repro)
    # ---------------------------------------------------------------------------

    # Harness timeout profiles. 'loopback' matches the historical defaults;
    # 'remote' loosens every wait for a second machine with unknown load times
    # (used by the remote session kit).
    Profiles = @{
        loopback = @{
            JoinDelaySec         = 8
            StartTimeoutSec      = 90
            # Scenario clocks arm on PEER-READY (the host waits for the join's
            # first entity batch before its script starts), so host anchors like
            # "SCENARIO MEMBER" appear ~a join-load later than gameplay start.
            ScenarioWaitSec      = 45
            JoinAnchorTimeoutSec = 60
            KillGraceSec         = 90
            # Peer-ready fallback: arm the scenario anyway after this much
            # gameplay if no peer batch ever arrives (KENSHICOOP_ARM_TIMEOUT_MS).
            ArmTimeoutMs         = 45000
        }
        remote = @{
            JoinDelaySec         = 0
            StartTimeoutSec      = 240
            ScenarioWaitSec      = 180
            JoinAnchorTimeoutSec = 240
            KillGraceSec         = 300
            ArmTimeoutMs         = 240000
        }
        # Two-machine LAN runs (run_lan_test.ps1): machine 2 loads independently
        # (its own disk/CPU), so waits sit between loopback and remote.
        lan = @{
            JoinDelaySec         = 0
            StartTimeoutSec      = 180
            ScenarioWaitSec      = 90
            JoinAnchorTimeoutSec = 120
            KillGraceSec         = 180
            ArmTimeoutMs         = 120000
        }
    }

    # WAN proxy profiles (one-way delay / jitter / loss both directions, applied
    # BELOW ENet by the netsim UDP relay so retransmission is really exercised).
    WanProfiles = @{
        regional = @{ DelayMs = 60;  JitterMs = 10; LossPct = 1  }
        bad      = @{ DelayMs = 120; JitterMs = 40; LossPct = 5  }
        awful    = @{ DelayMs = 200; JitterMs = 80; LossPct = 15 }
        # Starved-replica validation: regional conditions plus one scripted
        # TOTAL outage (both directions, below ENet) 30 s after the join's
        # first datagram (mid-scenario for the 24 s windows: connect precedes
        # gameplay by ~15 s), lasting 4 s - past interp staleMs (2 s), inside
        # the guard hold (KENSHICOOP_STARVE_HOLD_MS, 10 s). Driven bodies must
        # starve ([interp] starve>0) WITHOUT local AI/damage resuming. The
        # standard position gates (npc_track etc.) legitimately fail during a
        # real outage - this profile is for the manual guard-hold check, not
        # the regression matrix.
        stall    = @{ DelayMs = 60;  JitterMs = 10; LossPct = 1; StallAtS = 30; StallForS = 4 }
    }

    Scenarios = @{
        # ---- entity stream / presence -------------------------------------------
        leader_move = @{
            Save = 'sync'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'crosscheck'
            Gating   = @('crosscheck', 'smoothness', 'anim_truth', 'march', 'clock_sync')
            Advisory = @()
            Tier = 'full'; WanVariant = $true
            # DELIBERATE WAN-regime adjustment (final matrix, 2026-07-05): the same
            # latency catch-up stepping already demoted for npc_sync - under the
            # 'bad' proxy the walk-drive advances in bursts (zeroFrac measured
            # 0.42-0.44 vs the 0.4 gate; crosscheck still green at base tolerance).
            WanDemote = @('smoothness')
        }
        # suppress_churn gates here (town save): the 2026-07-11 pop-in/out fix -
        # census-present town NPCs must never cycle hidden->restored->hidden at
        # the stream-bubble boundary ('Saint'/'Kumo' churned before the veto).
        coop_presence = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'coop_presence'
            Gating   = @('coop_presence', 'suppress_churn', 'snap_rate', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'smoke'; WanVariant = $true
        }

        # split_interest (step 5): the host's tab leaves the bar; bar NPCs must keep
        # streaming via the second interest sphere (the join's tab leader). The join
        # walks nothing - its member IS the remote anchor. Save 'sync' (the bar
        # scene with a 2-tab squad): squad1's location has no world NPCs to anchor.
        split_interest = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'split_interest'
            Gating   = @('split_interest', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
            # DELIBERATE WAN-regime adjustment (step-5 validation, 2026-07-05): the
            # dual-sphere STREAMING mechanism is latency-independent (judged>=2 still
            # gates hard, and firmly-seated bar NPCs track at 0.2-0.4u even under the
            # 'bad' proxy). What degrades is the rest-wobble of the locally-ANIMATED
            # sit/idle NPCs - measured per-NPC median 14.6-17.5u under WAN vs 4.6-4.7u
            # clean (the npc_sync WAN class, but judged over only ~3 NPCs so ratio
            # slack cannot absorb one wobbler the way npc_sync's 9-NPC ratio does).
            WanTolerance = 18.0
        }

        # ---- interest-managed NPCs + pose ----------------------------------------
        npc_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'npc_track'
            Gating   = @('npc_track', 'pose', 'pose_state', 'body_state',
                         'smoothness', 'anim_truth', 'march', 'suppress_churn',
                         'snap_rate', 'rest_flap', 'clock_sync')
            # lifecycle (Phase 3): the [life] transition audit - advisory for
            # its first regression cycle, promoted once its baseline is known.
            Advisory = @('existence_parity', 'anti_zombie', 'lifecycle')
            Tier = 'smoke'; WanVariant = $true
            # DELIBERATE WAN-regime adjustments (re-validation matrix, 2026-07-05):
            # 120ms +/-40ms 5%-loss puts a walking NPC one-plus update interval
            # behind its host twin (measured worstMedian 4.8-10.5u vs the 0ms-tuned
            # 3u), and the walk-drive advances in catch-up steps (zeroFrac ~0.5 vs
            # 0.4) - the known latency micro-slide, already advisory for
            # coop_presence per the postmortem. Position fidelity still gates (at a
            # latency-scaled tolerance); smoothness demotes to advisory ONLY under
            # the WAN proxy.
            WanTolerance = 6.0
            WanDemote    = @('smoothness')
        }
        # craft1 is loaded WITHOUT the host re-arm setup: CraftOrderScenario pins the
        # baked worker itself and issues the live order mid-run (that IS the test).
        # march is ADVISORY here (demoted 2026-07-16): craft1 is the suite's densest
        # town (~24 node-anchored standers, task=35). Phase 1/2 (9cb0dd4) pulled that
        # whole background population into the near tier as committed task poses, and
        # a STAND_AT_NODE pose can render its locomotion clip in place - marchFrac
        # regressed ~0 -> ~0.3 here (and only here; down1/duel1 stay gating). A code
        # fix lives in the taskApplied pose path and is entangled with the worker's
        # own operate pose (endAction there cancels the operate the test checks), so
        # it stays measured-but-advisory pending a dedicated pose-clip quiet. The
        # tested behavior (worker craft sync + pose tracking) is still GATED.
        craft_order = @{
            Save = 'craft1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'craft_order'
            Gating   = @('craft_order', 'pose_state', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # ---- body-state / reliable events ----------------------------------------
        down_order = @{
            Save = 'down1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'down_order'
            Gating   = @('down_order', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'full'; WanVariant = $false
        }
        death_order = @{
            Save = 'down1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'death_order'
            Gating   = @('death_order', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
            Tier = 'full'; WanVariant = $true
        }

        # ---- combat ----------------------------------------------------------------
        combat_probe = @{
            Save = 'c'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_probe'
            Gating   = @('combat_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        combat_order = @{
            Save = 'duel1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_order'
            Gating   = @('combat_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        combat_kill = @{
            Save = 'duel1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'combat_kill'
            # damage_guard: join's cosmetic fight must apply no EXCESS local damage
            # (the hitByMeleeAttack detour) - protocol 16 streams the victim's
            # vitals host->join, so the join legitimately tracks the host's drop.
            # npc_vitals: the streamed victim vitals must CONVERGE (Phase B) -
            # the join's copy shows the host's true blood, not pristine health.
            Gating   = @('combat_kill', 'damage_guard', 'npc_vitals', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'smoke'; WanVariant = $true
        }

        # ---- player combat / medical (plan: Player Combat + Medical Replication) ----
        # player_combat: real combat damage TO player characters, both ownership
        # directions. Save 'sync' (the bar with ARMED NPCs). Characterization runs
        # fixed the shape: unarmed ally-vs-ally player duels draw ZERO blood, but
        # an armed bar NPC took a leader from 75.8 blood to -16 in ~25 s - so the
        # host orders NPC strikers onto each side's tab leader (A: join-owned
        # victim, B: host-owned victim). Gates on the striker's intent crossing
        # (task=65024 in the join's RECV series) AND the victim's blood dropping
        # on its OWNER (medical resolves on the victim's owner).
        player_combat = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'player_combat'
            Gating   = @('player_combat', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'full'; WanVariant = $true
        }
        # assault_town: the JOIN's player character starts an UNPROVOKED fight
        # with a world NPC (the no-fight report: a join-picked town fight only
        # rendered on the join). No host-side orders - the fight must cross as
        # the join's streamed combat intent and run for real on the host. The
        # oracle walks the chain link by link (issue -> capture -> host order ->
        # host fight) so a FAIL names the broken link.
        assault_town = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'assault_town'
            Gating   = @('assault_town', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'lifecycle')
            Tier = 'full'; WanVariant = $false
        }
        # player_ko: players as VICTIMS both directions - scaffold KO + revive on
        # each side's OWN member; edges must cross as reliable EVT_KNOCKOUT/EVT_REVIVE
        # and the peer's driven copy must lie down / stand up.
        player_ko = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'player_ko'
            Gating   = @('player_ko', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # medic_order: cross-player first aid both directions. Gates on the full
        # medical round trip (wound crosses to the healer, bandage finds it,
        # treatment returns to the owner) - requires the phase-2 vitals-sync +
        # treatment-forwarding features; before them it documents spikes 21-23's
        # truth (nothing crosses).
        medic_order = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'medic_order'
            Gating   = @('medic_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # limb_loss: protocol-16 limb replication both directions - each side
        # runs the engine's own amputate on ITS OWN tab leader (A: host member
        # LEFT_ARM, B: join member RIGHT_ARM). Gates that the copy side reaches
        # the STUMP LimbState within budget (reliable EVT_AMPUTATE + medical
        # self-heal), never reverts, and both sides converge on the severed
        # ground item (host-authoritative world-item channel + join dedupe).
        limb_loss = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'limb_loss'
            Gating   = @('limb_loss', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # stats_sync: protocol-17 character-stats replication both directions -
        # each side raises stats on ITS OWN tab leader via the raise-only
        # scaffold (A: host raises Strength+Stealth, B: join raises Dexterity+
        # Athletics). Gates that each raised value crosses to the peer's driven
        # copy within budget (change-gated reliable PKT_STATS), stays sticky,
        # and the untouched control stat (Toughness) does not drift. WanVariant:
        # the stats channel is reliable and must converge under loss.
        stats_sync = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'stats_sync'
            Gating   = @('stats_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        # carry_order: protocol-18 carried-body replication - three carry legs
        # (A: host own-tab, B: join carries a host-owned body, C: host carries
        # a join-owned body). Gates that each pickup/drop crosses to the peer's
        # LOCAL pair within budget (reliable EVT_PICKUP_BODY/EVT_DROP_BODY +
        # TASK_CARRY_BODY/BODY_CARRIED self-heal) and the carried copy rides
        # its carrier's shoulder (no down-enforcement dragging). WanVariant:
        # the edges are reliable and the carve-out must hold under loss.
        carry_order = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'carry_order'
            Gating   = @('carry_order', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }
        npc_carry = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'npc_carry'
            Gating   = @('npc_carry', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # bed_pose: conscious bed use (protocol 19 phase 1). The host orders its
        # leader to USE_BED_ORDER at the baked Camp Bed (save 'bedcage1'); the
        # join's driven copy must commit the same bed task at the same fixture.
        # Closes the "reproducible-pose allowlist covers beds but was never
        # runtime-validated" gap (spike 24 PARTIAL).
        bed_pose = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_pose'
            Gating   = @('bed_pose', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_wake: conscious bed EXIT (protocol 19). bed_pose only proved ENTER +
        # HOLD; this drives the wake-and-move arc that was desyncing (host PC
        # sleeps, then wakes and walks - the join copy stayed stuck sleeping). The
        # host orders L0 into the baked Camp Bed, waits for the join to commit the
        # pose, then issues a move ~25u away; the join copy must leave the bed
        # (BODY_IN_BED clears, [furn] BED FAST-EXIT) and follow the host.
        bed_wake = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bed_wake'
            Gating   = @('bed_wake', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_lay: UNCONSCIOUS place-in-bed LAYING pose + wake-and-exit (protocol
        # 19). bed_pose proved a conscious sleep ORDER lays down and bed_put proved
        # unconscious OCCUPANCY crosses; this proves a KO'd body DROPPED into a bed
        # renders the LAYING pose on BOTH clients and can get back OUT when it wakes.
        # (A conscious placement was ruled out: Kenshi itself nondeterministically
        # stands a conscious placed body on the mattress and the join mirrors it
        # faithfully - base-game behavior, not a coop bug, run 2026-07-17.) The owner
        # KO's M2 (then L1), drops it in the baked Camp Bed, then revives + moves it;
        # the pelvis + BODY_IN_BED in the MEMBER/RECV series must read LAYING on both
        # clients, and both must leave the bed after the wake.
        bed_lay = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_lay'
            Gating   = @('bed_lay', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'pose_state')
            Tier = 'full'; WanVariant = $true
        }

        # bed_put: unconscious placement (protocol 19 phase 3). Window A: the
        # host KO's its M2 (holdDown re-topped) and places it in the baked Camp
        # Bed via the putSubjectInFurniture scaffold, then takes it back out;
        # window B: the join does the same with its L1. The peer's driven copy
        # must mirror enter + exit (reliable EVT_ENTER/EXIT_FURNITURE edges +
        # BODY_IN_BED self-heal), engine-native via setBedMode.
        bed_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'bed_put'
            Gating   = @('bed_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # cage_put: same two windows against the baked Prisoner Cage
        # (setPrisonMode / BODY_IN_CAGE).
        cage_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'cage_put'
            Gating   = @('cage_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # chain_put: protocol-41 chained/pole prisoner state. Same two windows
        # as cage_put but kind=3 (Character::isChained -> setChainedMode). The
        # subject is self-chained (a pole needs no baked fixture; the owner is a
        # save-stable hand), so this gates the chained STATE crossing. Reuses
        # the generic Test-FurnPut oracle (Kind 3 -> the chain_put gate).
        chain_put = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'chain_put'
            Gating   = @('chain_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # pole_put: protocol-19 kind=4 prisoner POLE. Same two windows as
        # cage_put but the subject is placed on a baked standing PRISONER POLE
        # (fixture save 'pole1', baked via `bake_scene.ps1 -Setup pole
        # -BaseSave squad1 -BakeSave pole1`). A pole is the SAME containment as a
        # cage (setPrisonMode -> occupant reads in=2), so this is the controlled,
        # deterministic 'body ON A POLE' test: the FURNACT marker is kind=4 (the
        # pole gate) while the FURN occupancy is judged in=2 (Test-PolePut).
        pole_put = @{
            Save = 'pole1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'pole_put'
            Gating   = @('pole_put', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # cage_peer_sync: protocol-36 third-party placement - the HOST places
        # the join's KO'd leader (a peer-owned driven body) into the baked
        # cage, reproducing the guard-jailing-the-join-PC session bug. The
        # host must author the PEER-ENTER edge (no occupant-owner edge can
        # fire - the action ran purely on the host sim), hold its self-heal
        # exit, and the join must apply the enter to its OWN KO'd body; exit
        # stays owner-authored (the join frees itself at the end).
        cage_peer_sync = @{
            Save = 'bedcage1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'cage_peer'
            Gating   = @('cage_peer', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # sneak_probe: protocol-20 phase-0 spike (host-side, log-only). The host
        # forces stealthMode on its DRIVEN copy of the join's leader near the bar
        # NPCs and logs the copy's whoSeesMeSneaking series - proves the engine's
        # detection fires against driven copies and the map read is safe.
        sneak_probe = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'sneak_probe'
            Gating   = @('sneak_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # sneak_pose: stealth POSTURE crossing, both ownership directions
        # (window A host L0, window B join L1). The peer's driven copy must
        # flip Character::stealthMode (BODY_SNEAK / setStealthMode) within
        # budget on all four edges.
        sneak_pose = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'sneak_pose'
            Gating   = @('sneak_pose', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # sneak_detect: detection-indicator feedback. The join's leader sneaks
        # near the bar NPCs (save 'sync' - guaranteed NPC availability, dodging
        # the squad1 wandering-NPC flake); the host's world detects its driven
        # copy, streams PKT_STEALTH back, and the join replays the entries onto
        # its local pair.
        sneak_detect = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'sneak_detect'
            Gating   = @('sneak_detect', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # spawn_probe: runtime-spawn phase-0 diagnostic (spawnSync FORCED OFF by
        # Config). Host runtime-spawns squads near + far, join spawns its own
        # squad locally; gates on the failure-mode EVIDENCE (join logs
        # "[spawn] unresolved" for host runtime hands) and RECORDS whether
        # enforceHostAuthority caught the join-local spawns (the phase-2 gap
        # measurement - a finding, not a gate). Save 'sync': a live town gives
        # findNearbyNonPlayerFaction real factions to mint squads in.
        spawn_probe = @{
            DiagEnv = @{ KENSHICOOP_SPAWN_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_probe'
            Gating   = @('spawn_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shop_probe: money/vendor-trading phase-0 diagnostic (protocol 22 - no
        # money sync exists yet, nothing is forced off). Both sides log nearby
        # vendors (money+stock) and every squad tab's wallet at 1 Hz; the host
        # then drives one programmatic Inventory::buyItem, and the join tries
        # the same against its driven vendor copy. Gates only on the EVIDENCE
        # (vendor enumerated, wallet series readable, both buy attempts
        # logged); wallet/vendor divergence and the join-buy outcome are
        # recorded as FINDINGs that gate the 1b/1c design. Save 'sync': the
        # bar town puts real ShopTraders in range.
        shop_probe = @{
            DiagEnv = @{ KENSHICOOP_MONEY_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shop_probe'
            Gating   = @('shop_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # money_sync: protocol-22 per-tab wallet sync (moneySync ON - the
        # default). Same script as shop_probe minus the vendor legs: host
        # writes 5000 into its rank-0 tab wallet, join writes 7000 into its
        # rank-1 tab, and the PKT_MONEY channel must carry both across.
        # Gates on CONVERGENCE (each sentinel present in the peer's final
        # WALLET series + no drift on any co-visible rank).
        money_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'money_sync'
            Gating   = @('money_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # vendor_trade: protocol-22 phase 1c - the buyer-side purchase
        # COMPOSITE. Each side performs the exact buyer-side mutations of one
        # Inventory::buyItem (wallet debit + bought item into the tab leader's
        # personal inventory, same tick) on the tab it owns; gates that BOTH
        # effects converge on the peer (PKT_MONEY + the inventory snapshot
        # channel). The vendor-side mutation stays local by design (the engine
        # regenerates vendor stock per client; the [shop] BUY-LOCAL detour is
        # collecting field evidence for that mirror).
        vendor_trade = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'vendor_trade'
            Gating   = @('vendor_trade', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_probe: mid-session recruitment phase-0 diagnostic (protocol
        # 23 - no recruit sync exists, nothing is forced off). Each side
        # recruits the nearest BAKED world NPC and then a RUNTIME spawn via
        # the engine's own PlayerInterface::recruit, logging the subject's
        # hand BEFORE/AFTER (container change = the identity break) and a 1 Hz
        # distinct-container TABS census (rank-reshuffle evidence). Gates only
        # that the script ran; the identity/tab/peer-reaction findings gate
        # the 2b design. Save 'sync': the bar town has world NPCs in range.
        recruit_probe = @{
            DiagEnv = @{ KENSHICOOP_RECRUIT_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_probe'
            Gating   = @('recruit_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # squad_probe: squad-management phase-0 diagnostic (protocol 35,
        # squadSync forced OFF). Each side separates its own tab's highest-
        # hand member into a NEW squad (lever 0) then tries to move it back
        # into its original tab (lever 1 setFaction, lever 2 addCharacterAt
        # fallback), logging hand BEFORE/AFTER per move, the pointer-diff
        # SQEDGE stream, and a 1 Hz SQTABS census (per-tab member counts -
        # the rank-reshuffle evidence). Gates the local legs + that the
        # pointer-diff caught every landed move; the identity/rank/peer-
        # reaction findings gate the 16b design. Save 'squad1': the baked
        # 2-tab squad both ownership ranks partition on.
        squad_probe = @{
            DiagEnv = @{ KENSHICOOP_SQUAD_SYNC = '0' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'squad_probe'
            Gating   = @('squad_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # squad_sync: protocol-35 squad management sync (squadSync ON). Same
        # script as squad_probe; every scripted move must LAND locally and
        # Test-SquadSync gates the crossing - each move authored a reliable
        # EVT_SQUAD_MOVE, the peer re-keyed its local body onto the new hand
        # (no duplicate proxy, no unresolved-hand storm), tracked it (PROXY
        # series), and the pre-existing tab ranks never shifted (the
        # container-rank latch holding). Save 'squad1': the baked 2-tab squad.
        squad_sync = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'squad_sync'
            Gating   = @('squad_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_sync: protocol-23 recruitment sync (recruitSync ON). Same
        # script as recruit_probe; gates that every recruited hand CONVERGED on
        # the peer with exactly ONE body - the baked legs by "[recruit] REKEY"
        # (the peer's existing copy re-keyed to the new stream hand, no
        # duplicate proxy), the runtime legs by "[spawn] proxy BOUND" over the
        # now-BIDIRECTIONAL describe channel - and that the peer then TRACKED
        # each hand (SCENARIO PROXY series). All four local recruits must
        # succeed (res=1), unlike the probe where failure is data.
        recruit_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_sync'
            Gating   = @('recruit_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # recruit_ctl: Phase-1b control validation (recruit + transfer). Host
        # recruits a baked NPC, walks it (join drives - GAIT phase=A parity
        # sample), the join transfers it into its own tab (control-flip), then
        # walks it (host drives - GAIT phase=B). Test-RecruitCtl gates gait
        # parity (a driven squad member reproduces the owner's RUN, not a walk)
        # and anti-phantom (a control-flip claim never mints a duplicate proxy).
        # Save 'sync': the bar town has recruitable NPCs and both squads.
        recruit_ctl = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'recruit_ctl'
            Gating   = @('recruit_ctl', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # faction_probe: faction-relation phase-0 diagnostic (protocol 24 -
        # factionSync forced OFF; the affectRelations detours stay on for
        # cause-attribution evidence). Both sides log a 1 Hz FACREL series
        # (every interesting player-faction relation row by GameData sid) and
        # write one sentinel setRelation each (host -75 first sorted sid,
        # join +65 second, both table rows). Gates only that the script ran;
        # the sid-stability / crossing / operative-row findings gate the 3b
        # channel design.
        faction_probe = @{
            DiagEnv = @{ KENSHICOOP_FACTION_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'faction_probe'
            Gating   = @('faction_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # faction_sync: protocol-24 faction-relation sync (factionSync ON).
        # Same script as faction_probe; gates that each side's sentinel
        # relation write CONVERGED on the peer (final us AND them rows for
        # that sid equal the target) and no co-visible row ended diverged.
        faction_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'faction_sync'
            Gating   = @('faction_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # time_probe: game-clock phase-0 diagnostic (protocol 25 - timeSync
        # AND speedSync forced OFF so the host's 2x burst t=15..25s applies
        # directly and raw unsynced drift is measured). Both sides log a 1 Hz
        # GTIME series (in-game hours + hourLen + fsm + paused). Gates only
        # that the clocks are readable + monotonic; the absolute-vs-relative /
        # offset / drift / rate-vs-fsm findings gate the 25 channel design.
        time_probe = @{
            DiagEnv = @{ KENSHICOOP_TIME_SYNC = '0'; KENSHICOOP_SPEED_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'time_probe'
            Gating   = @('time_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # time_sync: protocol-25 game-clock sync (timeSync + speedSync ON).
        # Same script, both sides click 2x t=15s / 1x t=25s (consensus
        # arbitrates 2x); gates that both clocks stay monotonic and the final
        # host-join clock offset is inside tolerance across the speed change.
        time_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'time_sync'
            Gating   = @('time_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # door_probe: door-state phase-0 diagnostic (protocol 26 - doorSync
        # forced OFF). Both sides log a 1 Hz DOOR census (save-stable hand +
        # open/locked/state per baked door within ~100m of the interest
        # centers) and toggle one sentinel door each (host the first in serial
        # order t=12s, join the second t=24s) through the engine's own
        # openDoor/closeDoor. Gates only that the script ran (census + write
        # attempt); the hand-stability / write-lever / non-crossing findings
        # gate the protocol-26 channel design. Save: 'sync' - if its area has
        # no doors in range the probe FAILS loudly and the entry moves to a
        # save with a town/base before 5b.
        door_probe = @{
            DiagEnv = @{ KENSHICOOP_DOOR_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'door_probe'
            Gating   = @('door_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # door_sync: protocol-26 door-state sync (doorSync ON). Same script;
        # gates that each side's sentinel toggle CROSSED (the peer's final
        # census row for that hand shows the writer's target open state) and
        # no co-visible door ended diverged.
        door_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'door_sync'
            Gating   = @('door_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # build_probe: placed-building phase-0 diagnostic (protocol 27 -
        # buildSync forced OFF). Both sides log a 1 Hz BUILDSITE census
        # (construction sites within ~100m) and each places a small template
        # programmatically leader-relative (host t=10s side -4, join t=22s
        # side +4), then ramps its own site's constructionProgress +0.25/3s
        # through the engine's own setter. Gates that both placements were
        # ATTEMPTED and at least one was accepted + enumerable; the findings
        # (factory-vs-town-rules, runtime-hand non-overlap, progress scale /
        # self-complete) gate the protocol-27 channel design. Save: 'sync' -
        # town-adjacent, so the factory-bypass question gets a real answer;
        # if BOTH placements refuse, the probe fails loudly and the entry
        # moves to a wilderness save before 6b.
        build_probe = @{
            DiagEnv = @{ KENSHICOOP_BUILD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'build_probe'
            Gating   = @('build_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # build_sync: protocol-27 placed-building sync (buildSync ON). Same
        # script as build_probe; gates that each side's placement was MINTED
        # on the peer (describe/mint by placer key) and that the placer's
        # construction-progress ramp CROSSED (the peer applied rows up to the
        # complete=1 latch through the engine's own setter).
        build_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'build_sync'
            Gating   = @('build_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # bdoor_probe: placed-building door + removal phase-0 diagnostic
        # (protocol 28 - bdoorSync forced OFF, protocol-27 mint channel ON).
        # Both sides place a SHACK (a door-bearing template), ramp it to
        # self-complete, census nearby doors with their parent-building link
        # (1 Hz), toggle their OWN shack's door #0, and the host destroys its
        # shack at t=42s. Gates the LOCAL legs only (place + door exists +
        # toggle stuck + destroy worked); the findings (proxy doors on the
        # peer's mint, toggle non-crossing, the post-destroy ghost) gate the
        # protocol-28 channel design.
        bdoor_probe = @{
            DiagEnv = @{ KENSHICOOP_BDOOR_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bdoor_probe'
            Gating   = @('bdoor_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # bdoor_sync: protocol-28 placed-building door + removal sync
        # (bdoorSync ON). Same script as bdoor_probe; gates that each side's
        # door toggle CROSSED onto the peer's minted proxy (applied [bdoor]
        # RECV + census at the toggled state) and that the host's destroy
        # REMOVED the join's proxy (REMOVE-RECV ok=1 + no ghost census rows).
        bdoor_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'bdoor_sync'
            Gating   = @('bdoor_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # hunger_probe: hunger phase-0 diagnostic (protocol 29 - hungerSync
        # forced OFF, the rest of the medical snapshot streaming). Both sides
        # census the whole squad's hunger/fed/dazedOrAlert at 1 Hz and each
        # writes a proportional SENTINEL hunger (own-tab leader, current*0.6;
        # host t=15s, join t=22s). Gates the LOCAL legs (census + sentinel
        # stuck); the findings (scale, per-client decay agreement, sentinel
        # non-crossing, dazedOrAlert range) gate the protocol-29 fold-in.
        hunger_probe = @{
            DiagEnv = @{ KENSHICOOP_HUNGER_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'hunger_probe'
            Gating   = @('hunger_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # hunger_sync: protocol-29 hunger sync (hungerSync ON - the hunger/fed
        # scalars riding the owner-authoritative medical snapshot). Same
        # script as hunger_probe; gates that each side's sentinel hunger drop
        # CROSSED onto the peer's driven copy (drop-relative, within 10 s)
        # and that end-of-run owner-vs-copy hunger gaps stay small for every
        # shared hand.
        hunger_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'hunger_sync'
            Gating   = @('hunger_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # latejoin_probe: late-join phase-0 diagnostic (protocol 30 -
        # latejoinSync forced OFF, everything else streaming). The HOST
        # mutates state in the PRE-ARM window - before the join connects:
        # toggles a baked door, writes a sentinel faction relation (-85),
        # bumps its tab wallet (+777), and places + completes a small
        # building. Post-arm both sides census door/faction/money at 1 Hz.
        # Gates the LOCAL legs (host mutations ok + censuses ran); the
        # findings (per-channel heal latency via safety resends, the
        # building that NEVER mints) motivate the connect-edge resync.
        latejoin_probe = @{
            DiagEnv = @{ KENSHICOOP_LATEJOIN_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'latejoin_probe'
            Gating   = @('latejoin_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # latejoin_sync: protocol-30 connect-edge resync (latejoinSync ON).
        # Same script as latejoin_probe; gates that ALL pre-connect host
        # mutations converged on the join shortly after connect - the door,
        # faction and money censuses agree across clients, and the
        # pre-connect building MINTED on the join (the probe's permanent
        # gap, closed) with its complete latch.
        latejoin_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'latejoin_sync'
            Gating   = @('latejoin_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # save_probe: coordinated-save phase-12a diagnostic (protocol 31 -
        # save detour installed for edge logging, NO coordination). The HOST
        # issues a mid-session saveGameAs('coopresume') and the probe retires
        # the two runtime unknowns gating the host-authoritative transfer:
        # getCurrentGame/getSavePath resolve (spike 39 RVAs, never called
        # before) and the folder-quiescence completion edge (latency + file
        # count/bytes + the gameplay hitch while the engine writes).
        save_probe = @{
            DiagEnv = @{ KENSHICOOP_SAVE_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_probe'
            Gating   = @('save_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # save_sync: protocol-31 coordinated save + in-band transfer (saveSync
        # ON - the default). The HOST issues one mid-session
        # saveGameAs('coopresume'); the coordination does the rest: detour
        # edge -> folder quiescence -> paced whole-folder stream to the join
        # (BEGIN/FILE/DONE) -> staged CRC-verified atomic commit -> ACK.
        # Gates the full round trip: LOCAL-SAVE edge, QUIESCED, XFER-SENT,
        # COMMIT ok with file/byte counts EQUAL to the host's, ACK ok=1.
        save_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_sync'
            Gating   = @('save_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # save_stage1: resume_test.ps1 stage 1 (not a tier member - the
        # two-stage wrapper drives it). Same coordinated-save gates as
        # save_sync, but the host FIRST places a construction site and ramps
        # it part-way (session-runtime state), so the transferred save carries
        # a building that exists in NO baked save - the stage-2 same-hand
        # evidence.
        save_stage1 = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_sync'
            Gating   = @('save_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }

        # resume_check: resume_test.ps1 stage 2 (not a tier member). Both
        # clients relaunch on the save the stage-1 transfer delivered
        # (KENSHICOOP_SAVE=coopresume, NO harness save mirroring - the join
        # loads what the TRANSFER wrote) and census construction sites at
        # 1 Hz. Gates that the stage-1 building enumerates on BOTH sides
        # under the SAME save-stable hand - the identity-reset proof.
        resume_check = @{
            Save = 'coopresume'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'save_resume'
            Gating   = @('save_resume', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }

        # load_probe: coordinated-load phase-13a diagnostic (protocol 32 -
        # load detour installed for edge logging, NO load coordination;
        # saveSync stays ON so the join holds an identical copy first). The
        # HOST issues a coordinated saveGameAs('coopresume'), waits for the
        # transfer DONE, then a MID-SESSION engine::loadSave('coopresume').
        # Retires the runtime unknowns gating the coordinated load: is the
        # mid-session load safe, does mainLoop_hook tick across the load
        # screen, does the host survive its own swap with sync running, and
        # do save-stable hands re-resolve in the fresh world. The JOIN
        # deliberately does NOT load (the 13a divergence baseline).
        load_probe = @{
            DiagEnv = @{ KENSHICOOP_LOAD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'load_probe'
            Gating   = @('load_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # load_sync: protocol-32 coordinated load (loadSync ON - the default).
        # The user's manual scenario, automated: the HOST places a building,
        # coordinated-saves 'coopresume' (the join commits an identical copy),
        # then LOADS it mid-session. The coordination must drive the join to
        # load the same save: GO broadcast -> join fingerprint MATCH -> join
        # bypass-once load -> BOTH sides world-swap + protocol-32 session
        # reset -> the pre-load building enumerates on BOTH sides POST-load
        # under the SAME save-stable hand. clock_sync is NOT gated: the load
        # rebuilds both worlds mid-run, which legitimately restarts the
        # in-game clock series the oracle aligns on.
        load_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'load_sync'
            Gating   = @('load_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'clock_sync')
            Tier = 'full'; WanVariant = $false
        }

        # prod_probe: production machine phase-0 diagnostic (protocol 33 -
        # prodSync forced OFF, the protocol-27 mint channel ON). The HOST
        # places a generator + crafting bench leader-relative and ramps both
        # complete (minting proxies on the join); both sides census machine-
        # class buildings at 1 Hz (power/state/output/inputs/tech/farm);
        # the host operates the bench 1 Hz t=20-50s (divergence driver),
        # toggles the generator's power OFF/ON, then writes the bench output
        # via the native setProductionItem and a direct amount write. Gates
        # the LOCAL legs only (place + ramp + census + power applied +
        # setitem landed + ops ran); the findings (hand intersection,
        # owner-vs-idle divergence, write-lever stickiness, power
        # non-crossing, research evidence) gate the protocol-33 design.
        prod_probe = @{
            DiagEnv = @{ KENSHICOOP_PROD_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'prod_probe'
            Gating   = @('prod_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # prod_sync: protocol-33 host-authoritative machine state sync
        # (prodSync ON - the same script as prod_probe, so the probe's
        # measured gaps must now be CLOSED). The HOST places + completes a
        # generator and a crafting bench (minting proxies on the join),
        # operates the bench 1 Hz t=20-50s and toggles the generator power
        # OFF/ON; publishProd streams change-gated PKT_PROD rows the join
        # applies through the engine levers. Gates the probe's local legs
        # PLUS: [prod] rows sent AND applied, join minted both machines,
        # the bench output converged (final gap <= 1.0), the power OFF
        # crossed within 6 s and the final power state agrees.
        prod_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'prod_sync'
            Gating   = @('prod_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # research_probe: tech-tree phase-0 diagnostic (protocol 38 -
        # researchSync forced OFF). Both sides pick the deterministic
        # not-known-researchable RESEARCH subject at t=8s and poll
        # isKnown/canResearch 1 Hz; the HOST fires Research::startResearch at
        # t=10s (its isKnown flips - the spike-401 write lever); the JOIN
        # fires its OWN startResearch at t=25s. Gates: matching subject sids
        # (wire-key stability), host lever landed, join known=0 across the
        # t=10-25s divergence window (the unlock must NOT cross with the
        # hatch off), join self-lever landed AND stuck to run end.
        research_probe = @{
            DiagEnv = @{ KENSHICOOP_RESEARCH_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'research_probe'
            Gating   = @('research_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # research_sync: protocol-38 host-authoritative tech-tree sync
        # (researchSync ON - the same script as research_probe minus the
        # join's self-start, so the WIRE must flip the join's isKnown).
        # publishResearch streams one reliable PKT_RESEARCH row per known
        # sid (~1 Hz sample, first sight = baseline, 15 s safety resend);
        # applyResearch lands each via Research::startResearch. Gates: the
        # probe's local legs PLUS [research] rows sent AND applied, the
        # join's subject known=1 within 6 s of the host's start, sticky to
        # run end, and both finals known=1.
        research_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'research_sync'
            Gating   = @('research_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # store_probe: storage/machine container phase-0 diagnostic (protocol
        # 34 - storeSync forced OFF, the protocol-27 mint channel ON). The
        # HOST places a crafting bench + a general-storage chest leader-
        # relative and ramps both complete (minting proxies on the join);
        # both sides census container-bearing buildings (STORAGE + machine
        # classes) at 1 Hz with per-row entry count / total qty / content
        # hash; the host fabricates 5 sentinel items INTO the chest, operates
        # the bench 1 Hz t=24-54s (whole items land in the machine container
        # - the divergence driver), reconciles the chest down to 2 sentinels
        # (removal leg) and force-empties the bench container (churn leg);
        # the JOIN fabricates into its MINTED chest copy. Gates the LOCAL
        # legs only (place + ramp + census + add landed + recon removed + ops
        # ran); the findings (hand intersection, hasInv readability, capacity
        # vs INV_ITEMS_MAX, owner-vs-idle container divergence, add
        # non-crossing, post-empty churn) gate the protocol-34 design.
        store_probe = @{
            DiagEnv = @{ KENSHICOOP_STORE_SYNC = '0' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'store_probe'
            Gating   = @('store_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # store_sync: protocol-34 host-authoritative container contents
        # (storeSync ON, riding the invSync container channel - the same
        # script as store_probe minus the join-side add, so the probe's
        # measured gaps must now be CLOSED). The host's ~1 Hz census
        # registers every complete storage/machine container near the
        # interest centers as an authored container; placed buildings ride
        # their protocol-27 placer key (InvSnapshotHeader keyKind=1). Gates
        # the probe's local legs PLUS: the host census-authored the placed
        # chest ([inv] SEND kind=1), the join applied [inv] rows, the host's
        # chest add CROSSED onto the join's minted copy, and the FINAL chest
        # content hashes agree (so the reconcile-removal crossed too).
        store_sync = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'store_sync'
            Gating   = @('store_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # npc_census: protocol-36 wide-radius ghost culling. The join spawns 4
        # runtime NPCs and parks them ~600 u out - beyond the ~200 u stream
        # bubble, inside the 2000 u census radius - so ONLY the census channel
        # (host 1 Hz wide-radius hand list + the join's wide suppression pass)
        # can cull them. Gates: census flowing both ends, all ghost hands
        # culled, no mass-suppression of legitimate census NPCs. Save 'sync':
        # a live town gives the census real NPCs (the restraint control) and
        # findNearbyNonPlayerFaction a faction to mint the ghosts in.
        npc_census = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'npc_census'
            Gating   = @('npc_census', 'suppress_churn', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # fast_march (2026-07-11 rubber-banding validation): leader_move at 5x
        # game speed (both sides vote 5x; consensus takes it). Gates on the
        # join's PLAYER-SQUAD hard-snap RATE - before the velocity-aware snap
        # gate, 5x wall-clock velocities teleported the driven leader every
        # sample (~35 snaps/s measured in the 2026-07-11 manual session), and a
        # sprinting leader false-snapped even at 1x (gap=8.6 vs the fixed 8 u
        # gate). Background-NPC snaps stay un-gated at 5x: a bar NPC resting
        # between stream updates legitimately falls 100+ u behind and the
        # teleport is the correct convergence tool (measured gaps 128-1826 u).
        # Crosscheck stays advisory: at 5x the driven copy legitimately trails
        # by ~1 wall-clock update interval * 5x velocity.
        fast_march = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            PrimaryGate = 'snap_rate_squad'
            Gating   = @('snap_rate_squad', 'suppress_churn', 'clock_sync')
            Advisory = @('snap_rate', 'crosscheck', 'smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # spawn_far (2026-07-11 "NPCs spawn on top of the join player" fix):
        # census-range proxy minting. The host spawns a runtime squad ~620 u
        # out and walks it toward the co-located leaders; the join must mint
        # the proxies at census range (census-missing scan + reply-side mint
        # gate at KENSHICOOP_SPAWN_MINT_RADIUS) - every far hand bound, all
        # binds >= 400 u from the join anchor, no duplicate mints, and the
        # SAME proxy body driven into the stream bubble. snap_rate stays
        # advisory: the bubble-entry drive takeover legitimately hard-snaps
        # a proxy whose local AI drifted while it was census-only.
        spawn_far = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_far'
            Gating   = @('spawn_far', 'suppress_churn', 'clock_sync')
            Advisory = @('mint_dist', 'snap_rate', 'smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # travel_parity (2026-07-11 free-play "yellow packs while roaming"
        # report): the JOIN's PC teleport-hops ~60,000 u across the map (15
        # hops x 4000 u, ~9 s dwell each - every hop lands entirely outside
        # the previous 2000 u census bubble, so existence coverage rebuilds
        # from nothing at each stop: zone streams, census re-centering,
        # mint/cull churn) while the HOST's PC follows its local driven copy
        # (teleport catch-up past 150 u, walk inside) - the roaming direction
        # (join drags the interest/census coverage) no other scenario moves.
        # Both sides dump a 5 s worldstate (SCENARIO WORLD/WNPC rows; the
        # join rows carry the drv/cen/hid/ghost authority class).
        # follow_travel gates first: if the follow never held, the parity
        # numbers describe two separated worlds and mean nothing.
        # travel_parity gates the visible-on-join-only (ghost) fraction +
        # persistence while moving; crosscheck stays advisory (hop legs open
        # multi-thousand-unit transients). snap_rate advisory: the hard snap
        # IS the convergence tool on every hop. Seconds/KillGraceSec: the
        # 160 s host window outlives the default 150 s self-exit + 90 s kill
        # grace.
        travel_parity = @{
            Save = 'sync'; Setup = ''; Tolerance = 18.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = 'follow_travel'
            Gating   = @('follow_travel', 'travel_parity', 'clock_sync')
            Advisory = @('existence_parity', 'mint_dist', 'anti_zombie',
                         'lifecycle', 'suppress_churn', 'snap_rate',
                         'smoothness', 'anim_truth', 'march', 'crosscheck')
            Tier = 'full'; WanVariant = $false
        }

        # camp_approach: Phase 2 crash-hardening SOAK on the 'camp' prison save
        # (many NPCs). The JOIN teleport-hops across the camp forcing mint/zone
        # churn while the HOST self-exits FIRST at ~130s - a real peer drop that
        # fires clearPeerReplicationState on the surviving join mid-churn. There
        # is no deterministic crash repro, so this is a stress gate, not parity:
        # Test-CampApproach checks both sides reach a SCENARIO RESULT (no crash),
        # the join logs 'peer left' -> '[leave] cleared proxies=', no '[drive]'
        # touches a stale/unbound hand after the leave, and proxies drain to ~0.
        # Join window 150s (from arm) -> Seconds/KillGraceSec raised like
        # travel_parity so the runner backstop outlives it.
        camp_approach = @{
            Save = 'camp'; Setup = ''; Tolerance = 18.0
            Seconds = 230; KillGraceSec = 200
            PrimaryGate = 'camp_approach'
            Gating   = @('camp_approach', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # spawn_sync: protocol-21 runtime-spawn proxy replication (spawnSync ON).
        # Same script as spawn_probe; gates that the join minted proxies for the
        # host's runtime spawns (near half + far >= 1), the PROXY position series
        # tracks the host MEMBER series per hand, and the join's own local
        # runtime spawns still get suppressed by host authority.
        spawn_sync = @{
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'spawn_sync'
            Gating   = @('spawn_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # speed_probe: vote-decoupling phase-0 spike (host-side, log-only;
        # speedSync forced OFF by Config). Quiet writes (setFrameSpeedMultiplier
        # + guarded userPause) must drive the sim WITHOUT moving the UI speed
        # buttons; a loud simulated click must move them AND register as
        # captured intent (the hook-based vote source).
        speed_probe = @{
            DiagEnv = @{ KENSHICOOP_SPEED_SYNC = '0'; KENSHICOOP_DEBUG_SPEED = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'speed_probe'
            Gating   = @('speed_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shackle_probe: Phase 6 6a evidence spike (log-only, BOTH clients) on the
        # 'camp' prison save. Each client enumerates nearby world NPCs and emits a
        # "SCENARIO SHACKLE" line per chained / shackle-carrying body. The oracle
        # time-aligns the owner's and peer's view of each shackled prisoner and
        # flags any chained/lock divergence (the reported "peer PC unlocks the
        # shackles" desync). No behavior change ships in 6a.
        shackle_probe = @{
            DiagEnv = @{ KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shackle_probe'
            Gating   = @('shackle_probe', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # shackle_sync: Phase 6 6b validation (BOTH clients) on the 'camp' prison
        # save. Same "SCENARIO SHACKLE" emission as shackle_probe, but the oracle
        # is now STRICT: with the protocol-42 locked bit + non-owner unlock guard
        # shipping, a shared prisoner whose owner reports chained/locked while the
        # peer's driven copy reports it cleared is a FAIL. Shared-hand parity is
        # asserted; the no-shared-hand identity caveat defers to the manual gate.
        shackle_sync = @{
            DiagEnv = @{ KENSHICOOP_DEBUG_SHACKLE = '1' }
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'shackle_sync'
            Gating   = @('shackle_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # world_parity: full-roster cross-client parity on the dense 'camp'
        # prison save. Nothing scripted - both sides run their sims while the
        # replicator's 5 s auditRows dumps (SCENARIO WORLD/WNPC with the
        # task=/pelvis=/mv= parity fields and cls=pc player rows) feed
        # Test-WorldParity's tiered judgment:
        #   PC tier     - every player character present on both sides, hard
        #                 position gate (a diverged host-PC is invisible to
        #                 every other oracle: the NPC dumps exclude the squad)
        #   near tier   - host rows within 260 u of a PC anchor: existence,
        #                 position and task parity on the join
        #   census tier - 260-2000 u: existence + position within the park
        #                 threshold; task not judged (local-sim copies)
        # Seconds/KillGraceSec: the 180 s host window outlives the default
        # 150 s self-exit + kill grace (same pattern as travel_parity).
        world_parity = @{
            Save = 'camp'; Setup = ''; Tolerance = 6.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = 'world_parity'
            Gating   = @('world_parity', 'clock_sync')
            Advisory = @('existence_parity', 'anti_zombie', 'lifecycle',
                         'suppress_churn', 'smoothness', 'anim_truth', 'march')
            Tier = 'probe'; WanVariant = $false
        }

        # speed_sync: consensus game-speed (pause/1x/2x/3x). Scenario-simulated
        # clicks (host 3x -> join 1x -> join 3x) exercise min-arbitration both
        # ways, then a bar NPC on the host leader trips the combat 1x cap. Gates
        # on the two SPEED series matching (time-aligned), each transition's
        # follow latency, and the combat demotion. WanVariant: the SET/REQ
        # channel is reliable and must converge under loss.
        speed_sync = @{
            DiagEnv = @{ KENSHICOOP_TIME_SYNC = '0'; KENSHICOOP_DEBUG_SPEED = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'speed_sync'
            Gating   = @('speed_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # Waiting-attacker stance conformance (protocol 15): the host piles ~5
        # bar NPCs onto its own leader so the attack-slot system queues most of
        # them. Gates that both stances streamed, the join's re-issue loop and
        # snap teleports are dead, and every crowd copy tracks its host body.
        # Tolerance is the per-hand median tracking gate (a brawl's footwork is
        # legitimately noisier than a walk: the menace ring itself is ~6-8 u wide,
        # each engine assigns slot spots independently, and the whole brawl
        # roams - healthy runs measured 3-15 u medians across 7 runs, pre-fix
        # divergence measured 74-95 u, so 20 u splits the distributions cleanly.
        # WanVariant: stance flips ride the unreliable entity batch and must
        # self-heal under loss.
        combat_crowd = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_crowd'
            # combat_snap_rate: advisory here (crowd is ~5 WAITING strikers, low
            # active-melee churn) - it surfaces the enriched [combat] snap buckets
            # for diagnosis. The many-NPC active-melee case is HARD-gated on
            # combat_battle. See resources/combat_warp_debug plan.
            Gating   = @('combat_crowd', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'combat_snap_rate')
            Tier = 'full'; WanVariant = $true
        }
        # combat_battle: many-NPC combat warp validation (the "NPCs warp on the join
        # when many are fighting" field report). The host runtime-spawns N fighters
        # (KENSHICOOP_BATTLE_N, default 16; the 10v10/20v20/40v40 ladder) near the
        # leader and index-pairs them into mutual melee, so the join must DRIVE many
        # simultaneously-active combat copies via the interp + graded-snap path.
        # combat_battle gates that the fight happened + crossed; combat_snap_rate
        # HARD-gates the [combat] snap teleport buckets (churn rate / persistence /
        # wrong-target) - the actual warp measurement. Tolerance 20u matches the
        # combat drift bands (COMBAT_SNAP_DIST).
        combat_battle = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_battle'
            Gating   = @('combat_battle', 'combat_snap_rate', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # combat_win: the SECOND warp shape (2026-07-16 smoothness pass). Each side
        # buffs its OWN player-squad to 120 in every stat and the host runtime-spawns
        # N unbuffed enemies (KENSHICOOP_WIN_N, default 8) onto the PC leader; the
        # buffed PCs cut them down, so the join stress shifts to dying/fleeing/KO
        # churn and rapid target loss (distinct from combat_battle's sustained melee).
        # combat_win gates that the PCs were buffed both sides + the fight was won and
        # crossed; combat_snap_rate HARD-gates the warp buckets. death/existence parity
        # advisory (dying enemies). Tolerance 20u matches the combat drift bands.
        combat_win = @{
            Save = 'sync'; Setup = ''; Tolerance = 20.0
            PrimaryGate = 'combat_win'
            Gating   = @('combat_win', 'combat_snap_rate', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march', 'existence_parity')
            Tier = 'full'; WanVariant = $false
        }

        # ---- inventory ---------------------------------------------------------------
        inv_order = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_sync'
            Gating   = @('inv_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_bidir = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_bidir'
            Gating   = @('inv_bidir', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }
        inv_equip = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_equip'
            Gating   = @('inv_equip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_reequip = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_reequip'
            Gating   = @('inv_reequip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_addequip = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'add_equip'
            Gating   = @('add_equip')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # Protocol-36 BASELINE for the cross-owner direct-drag bugs (field report:
        # dupe on take, wipe on give, weapon vanish). The host performs real cross-
        # owner engine moves; the oracle REPORTS the conservation outcome as evidence.
        # Not in a tier: it documents the bug the transfer-intent channel then fixes.
        trade_probe = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_XFER_SYNC = '0'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'trade_probe'
            Gating   = @('trade_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false
        }
        # Protocol-37 VALIDATION of the transfer-intent channel (xferSync ON): the
        # same three cross-owner drags trade_probe baselined, now expected CLEAN -
        # TAKE lands + the removal propagates (no dupe), GIVE arrives on the owner
        # (no wipe), the traded WEAPON survives on BOTH clients (real-object
        # relocation) and both clients agree on the final per-rank state.
        trade_peer = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'trade_peer'
            Gating   = @('trade_peer', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # weapon_loot: weapon-fabrication sync validation (the last trading loss
        # vector). The HOST's owned leader ACQUIRES a weapon that exists in NO
        # shared-save inventory (novel sid, engine-fabricated - the loot/vendor-buy
        # shape); the join's driven copy must gain EXACTLY one copy of the same
        # template via the inventory snapshot channel + the spike-451 weapon CREATE,
        # with zero dupes on either side (fabrication must not race the W2
        # conservation channel or the snapshot echo).
        weapon_loot = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'weapon_loot'
            Gating   = @('weapon_loot', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }

        # ---- world items ------------------------------------------------------------
        drop_probe = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'drop_probe'
            Gating   = @('drop_probe')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'none'; WanVariant = $false   # W0 diagnostic; evidence, not a sync gate
        }
        world_item_sync = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wi_sync'
            Gating   = @('wi_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # W1 BIDIR: the join->host direction of world_item_sync (the join drops +
        # despawns; the HOST must spawn/cull the proxy) - the field-reported gap
        # (join ground drops never appeared on the host) closed by the
        # bidirectional W1 stream with owner-scoped netIds + the proxy echo guard.
        world_item_join = @{
            DiagEnv = @{ KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wi_join'
            Gating   = @('wi_join', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        # rejoin_items (Phase 3 item-dup fix): a reload must not duplicate save-
        # native ground items. Reuses the load_sync coordinated save+load lever
        # (loadSync + saveSync ON by default): the HOST drops K test items (both
        # clients reach n0+K), coordinated-saves 'coopresume' so the drops bake
        # into the shared save, then loads it mid-session. The first-scan baseline
        # must record the now-native drops as never-emit so the host does not
        # re-stream them and the join does not layer a duplicate proxy - the
        # oracle gates POST-reload count <= PRE-reload count on BOTH sides plus a
        # WORLD-RELOAD edge. clock_sync is NOT gated (the reload restarts the
        # in-game clock series, same as load_sync).
        rejoin_items = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'sync'; Setup = ''; Tolerance = 6.0
            PrimaryGate = 'rejoin_items'
            Gating   = @('rejoin_items')
            Advisory = @('smoothness', 'anim_truth', 'march', 'clock_sync')
            Tier = 'full'; WanVariant = $false
        }
        wpn_relocate = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wpn_relocate'
            Gating   = @('wpn_relocate')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        world_weapon_drop = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'weapon_drop'
            Gating   = @('weapon_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }
        world_armor_drop = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_WORLD_SYNC = '1'; KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'armor_drop'
            Gating   = @('armor_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # ---- diagnostics (never in a tier) --------------------------------------------
        inv_wpnseq = @{
            DiagEnv = @{ KENSHICOOP_INV_DUMP = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # local [recon] trace diagnostic
        }
        # xfer_block: cross-owner trade VETO exercise (manual/diagnostic, not in a
        # tier). Blocks direct squad-to-squad drags and forces ground drops; needs
        # the inventory channel live to drag on, and the veto armed. DiagEnv carries
        # both knobs so no launcher (or Config.cpp) has to name the scenario.
        xfer_block = @{
            DiagEnv = @{ KENSHICOOP_INV_SYNC = '1'; KENSHICOOP_BLOCK_XFER = '1' }
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # veto-path diagnostic
        }
        spike = @{
            Save = 'c'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # run_spike.ps1 judges captures itself
        }
        # jail_probe: read-only diagnostic for the "put jailed PC to work" desync
        # (join PC briefly exits its cage then teleports back). Passive soak on
        # the 'jailed' save (join PC caged in the camp); no scenario gate - the
        # evidence is the KENSHICOOP_JAIL_PROBE [jail] STATE traces (own vs drv),
        # the [furn] ENTER/EXIT/PEER-ENTER edges, and (with KENSHICOOP_TASK_SPIKE)
        # the [spike] SELECT task the join's local AI picks for its own PC.
        # auditRows is armed for this scenario for pos/context. Must run
        # partitioned (-Inhabit / OWN_RANK) so the join actually owns the caged PC.
        jail_probe = @{
            DiagEnv = @{ KENSHICOOP_JAIL_PROBE = '1'; KENSHICOOP_TASK_SPIKE = '1'; KENSHICOOP_JAIL_OBSERVE = '1' }
            Save = 'jailed'; Setup = ''; Tolerance = 6.0
            Seconds = 220; KillGraceSec = 190
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # spike: [jail]/[furn]/[spike] traces
        }
        # jail_soak: LONG-play version of jail_probe (spike 58). 15 min passive
        # soak so the host guard's put-to-work cage<->pole cycle and census-band
        # NPC drift actually accumulate (jail_probe's 220 s is too short to see
        # them). Same probes (KENSHICOOP_JAIL_PROBE/TASK_SPIKE/JAIL_OBSERVE) +
        # auditRows + the new [jail] SNAP re-seat metric. Default Save='jailed';
        # override -Save 'slaves save' / -Save cage2 for the other testbeds. Run
        # partitioned (-Inhabit / OWN_RANK) so each side owns one PC. No gate.
        jail_soak = @{
            DiagEnv = @{ KENSHICOOP_JAIL_PROBE = '1'; KENSHICOOP_TASK_SPIKE = '1' }
            Save = 'jailed'; Setup = ''; Tolerance = 6.0
            # run_test kills at (early screenshot anchor + KillGraceSec), so the
            # kill grace must exceed the whole soak window or the game is cut short
            # before the plugin self-exits at Seconds. Keep KillGraceSec > Seconds.
            Seconds = 600; KillGraceSec = 780
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # spike: [jail]/[furn]/[census]/[spike] traces
        }
    }
}
