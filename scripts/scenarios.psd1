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
        coop_presence = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'coop_presence'
            Gating   = @('coop_presence', 'march', 'clock_sync')
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
                         'smoothness', 'anim_truth', 'march', 'clock_sync')
            Advisory = @()
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
        craft_order = @{
            Save = 'craft1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'craft_order'
            Gating   = @('craft_order', 'pose_state', 'march', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth')
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
            Advisory = @('smoothness', 'anim_truth', 'march')
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
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
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

        # speed_sync: consensus game-speed (pause/1x/2x/3x). Scenario-simulated
        # clicks (host 3x -> join 1x -> join 3x) exercise min-arbitration both
        # ways, then a bar NPC on the host leader trips the combat 1x cap. Gates
        # on the two SPEED series matching (time-aligned), each transition's
        # follow latency, and the combat demotion. WanVariant: the SET/REQ
        # channel is reliable and must converge under loss.
        speed_sync = @{
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
            Gating   = @('combat_crowd', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $true
        }

        # ---- inventory ---------------------------------------------------------------
        inv_order = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_sync'
            Gating   = @('inv_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_bidir = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_bidir'
            Gating   = @('inv_bidir', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }
        inv_equip = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_equip'
            Gating   = @('inv_equip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_reequip = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'inv_reequip'
            Gating   = @('inv_reequip', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        inv_addequip = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'add_equip'
            Gating   = @('add_equip')
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
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wi_sync'
            Gating   = @('wi_sync', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        wpn_relocate = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'wpn_relocate'
            Gating   = @('wpn_relocate')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'full'; WanVariant = $false
        }
        world_weapon_drop = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = 'weapon_drop'
            Gating   = @('weapon_drop', 'clock_sync')
            Advisory = @('smoothness', 'anim_truth', 'march')
            Tier = 'smoke'; WanVariant = $true
        }

        # ---- diagnostics (never in a tier) --------------------------------------------
        inv_wpnseq = @{
            Save = 'squad1'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # local [recon] trace diagnostic
        }
        spike = @{
            Save = 'c'; Setup = ''; Tolerance = 3.0
            PrimaryGate = ''
            Gating   = @()
            Advisory = @()
            Tier = 'none'; WanVariant = $false   # run_spike.ps1 judges captures itself
        }
    }
}
