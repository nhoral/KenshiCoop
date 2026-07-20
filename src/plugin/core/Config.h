// Config - parse the KENSHICOOP_* environment variables once at load.
//
// Mode/role and all runtime knobs are env-driven so host vs join is chosen
// without a recompile (the test harness sets these before launching Kenshi).

#ifndef KENSHICOOP_CONFIG_H
#define KENSHICOOP_CONFIG_H

#include <string>
#include <set>

namespace coop {

struct Config {
    bool          isHost;          // KENSHICOOP_MODE != "join"
    std::string   ip;              // KENSHICOOP_IP   (join target)
    int           port;            // KENSHICOOP_PORT
    std::string   save;            // KENSHICOOP_SAVE (auto-load; empty = manual)
    int           testSeconds;     // KENSHICOOP_TEST_SECONDS (0 = no self-exit)
    std::string   logPath;         // KENSHICOOP_LOG
    std::string   scenario;        // KENSHICOOP_SCENARIO (empty = normal tick)
    unsigned long autoLoadDelayMs; // KENSHICOOP_AUTOLOAD_DELAY_MS
    std::string   setupScene;      // KENSHICOOP_SETUP ("" = off; "chair"/"npc"/"craft"/"down"/"downhold"/"duel"/"squad"/"inventory"/"bedcage")
                                   // host-only one-shot world spawn to bake a
                                   // deterministic test scene into a save.
    std::string   bakeSave;        // KENSHICOOP_BAKESAVE ("" = manual save): after
                                   // a setup scene spawns, auto-write the fixture
                                   // save under this name (SaveManager::save).
    bool          probeRecruit;    // KENSHICOOP_PROBE_RECRUIT == "1" (join only):
                                   // recruit diverged NPCs into the player squad
                                   // to validate the AI-gating "inhabit" lever.
    bool          aiSuspend;      // KENSHICOOP_AI_SUSPEND != "0" (BOTH roles; DEFAULT ON):
                                   // detour Character::periodicUpdate to suspend the
                                   // AI decision layer for any peer-DRIVEN body (keeps
                                   // animation; stops self-tasking) - faction-safe.
                                   // Phase 1b: host too, for units transferred into
                                   // the join's tab (host then drives them).
                                   // Promoted from probe to the default quieting layer
                                   // (2026-07-05 review: pose_state 0.972 vs 0.962 with
                                   // it on). "0" is the escape hatch; the legacy
                                   // KENSHICOOP_PROBE_AISUSPEND=1 still forces it on.

    // Debug WAN simulation: artificially delay (and optionally drop) inbound entity
    // batches so the same loopback harness exercises the latency path - render
    // interpolation, dead reckoning, stale-state enforcement - that a real internet
    // link would impose. All zero (default) = no simulation, immediate delivery.
    unsigned int  netSimDelayMs;   // KENSHICOOP_NETSIM_DELAY_MS  (base one-way delay)
    unsigned int  netSimJitterMs;  // KENSHICOOP_NETSIM_JITTER_MS (+/- uniform variance)
    unsigned int  netSimLossPct;   // KENSHICOOP_NETSIM_LOSS_PCT  (0-100 drop chance)

    // Protocol 36 movement-smoothness live tuning (defaults = the historical
    // constants; 0/absent = default). The interp knobs feed InterpConfig, the
    // drive knobs feed the walk-drive's hard-snap / catch-up gains - all for
    // WAN A/B runs without a rebuild.
    bool         sendStamp;         // KENSHICOOP_SEND_STAMP != "0": index interp
                                    // rings on the sender's capture stamp (wire
                                    // v35); "0" = legacy arrival-time indexing
                                    // (receiver-local A/B, no wire change)
    unsigned int interpMinDelayMs;  // KENSHICOOP_INTERP_MIN_DELAY_MS  (50)
    unsigned int interpMaxDelayMs;  // KENSHICOOP_INTERP_MAX_DELAY_MS  (200)
    unsigned int interpMaxExtrapMs; // KENSHICOOP_INTERP_MAX_EXTRAP_MS (250)
    unsigned int interpStaleMs;     // KENSHICOOP_INTERP_STALE_MS      (2000)
    float        interpSnapDist;    // KENSHICOOP_INTERP_SNAP_DIST     (50 u)
    float        catchupK;          // KENSHICOOP_CATCHUP_K            (2.0)
    float        snapDist;          // KENSHICOOP_SNAP_DIST            (8 u)
    float        snapSeconds;       // KENSHICOOP_SNAP_SECONDS         (0.75 s)
                                    // velocity-aware hard-snap gate: teleport a
                                    // driven body only when it trails the newest
                                    // sample by more than this much travel time
                                    // (snapDist stays the slow-mover floor)
    // Combat-drive convergence bands (2026-07-16 smoothness pass). All default to
    // the ReplicatorUtil constants when the env is unset/0 (the drive keeps its own
    // constant-initialized member unless the value is > 0), so ONE build sweeps them.
    float        combatSoftDist;    // KENSHICOOP_COMBAT_SOFT_DIST     (6 u)
    float        combatSnapDist;    // KENSHICOOP_COMBAT_SNAP_DIST     (20 u churn ceiling)
    float        combatBigSnapDist; // KENSHICOOP_COMBAT_BIG_SNAP_DIST (60 u true-leave)
    float        combatSlideMax;    // KENSHICOOP_COMBAT_SLIDE_MAX     (60 u/s slide cap floor)
    unsigned int combatConvergeMs;  // KENSHICOOP_COMBAT_CONVERGE_MS   (400 ms hysteresis)

    // Protocol 36 NPC existence census: wide-radius ghost-culling reach in
    // world units. The host broadcasts the hand list of every world NPC within
    // this radius at 1 Hz; the join suppresses local NPCs absent from it.
    // 0 disables the census channel entirely (stream-bubble culling only).
    float        censusRadius;      // KENSHICOOP_CENSUS_RADIUS        (2000 u)

    // Census-mint reach (2026-07-11): how far from the join's own squad a
    // census-missing host NPC may be proxy-minted, so host runtime spawns
    // (raids) appear at render range and walk in instead of materializing at
    // the ~200 u stream bubble. 0 disables (legacy stream-bubble minting).
    float        spawnMintRadius;   // KENSHICOOP_SPAWN_MINT_RADIUS    (600 u)

    // v38 census position parking (pack-hidden fix, 2026-07-11): how far a
    // census-PRESENT local NPC copy may drift from the host's census position
    // before the join parks it back onto the host's spot (wide pass only,
    // per-key cooldown). The census carries positions per row; existence
    // culling is untouched. 0 disables parking.
    float        censusParkDist;    // KENSHICOOP_CENSUS_PARK          (120 u)

    // Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI, DEFAULT ON): the join
    // suspends the local AI of a census-band body (census-present, unstreamed)
    // that diverges past censusParkDist_, so a captive/working slave can't flee
    // and aggro the join's guards while the host has it working. Divergence-
    // gated; well-tracking census NPCs keep their local AI. A/B escape hatch.
    bool         censusFreezeAi;     // KENSHICOOP_CENSUS_FREEZE_AI     (on)

    // Camera-anchored interest (KENSHICOOP_CAM_INTEREST, DEFAULT ON,
    // protocol 43): interestCenters grows from the two squad-tab-leader
    // spheres to up to FOUR anchors - + the local camera center + the peer's
    // ~1 Hz camera hint (PKT_CAM_HINT), deduped within ~100 u. NPCs where a
    // player is LOOKING (but no PC stands) stay streamed/listed. A/B hatch.
    bool         camInterest;        // KENSHICOOP_CAM_INTEREST         (on)

    // Task-selection observation spike: passively hook CharBody::setCurrentAction
    // (the AI/order selection->execution seam) and log the chosen task tuple per
    // body. Off by default; a diagnostic for the "stream selection, not motion"
    // design direction. Changes no behavior.
    bool         taskSelectSpike;    // KENSHICOOP_TASK_SPIKE           (off)

    // Jail put-to-work desync spike: emit correlated [jail] STATE traces for
    // captive bodies (owned PC in publishOwned, driven copy in applyTargets) so
    // the twitch (brief cage-exit then re-cage) can be pinned. Read-only.
    bool         jailProbe;          // KENSHICOOP_JAIL_PROBE           (off)

    // Jail put-to-work observation spike (Phase A): host stops driving/
    // suspending/self-healing a peer-owned captive so its local sim runs
    // unopposed and its trajectory ([jail] OBSERVE) reveals the guard's intent
    // (relocate to a work spot vs walk a job round). Read-only diagnostic.
    bool         jailObserve;        // KENSHICOOP_JAIL_OBSERVE         (off)

    // Starved-replica guard hold: how long (ms) a driven body whose stream
    // went stale keeps its AI-suspend + damage-guard before releasing to
    // local simulation - a WAN stall must not become an authority transfer.
    // 0 = legacy release-on-stale.
    unsigned int starveHoldMs;      // KENSHICOOP_STARVE_HOLD_MS       (10000)

    // Injected fake wall-clock skew (KENSHICOOP_FAKE_CLOCK_SKEW_MS, may be
    // negative; join only in practice). Shifts coop::wallClockMs() - i.e. BOTH
    // the log-line timestamps AND the wire time-sync - so a loopback run can
    // validate that CLOCKSYNC offset estimation + oracle clock alignment recover
    // a genuine two-machine clock disagreement. 0 = real clock.
    long          fakeClockSkewMs;

    // Step-2 pruning experiment (KENSHICOOP_NO_DETACH == "1", join only): skip the
    // sitter detachFromTownAI in applyRest, betting that default AI-suspend alone
    // stops town-AI re-tasking. Off by default; for manual A/B runs only.
    bool          noDetach;

    // Divergence-gated authority (KENSHICOOP_GATE_AUTHORITY != "0", join only,
    // DEFAULT ON - promoted 2026-07-05 after the step-4 A/B): world NPCs whose
    // local AI sustainedly agrees with the host's raw task are TRUSTED (not
    // suspended, not driven) until they diverge or drift; divergence re-engages
    // the drive the same tick. Doctrine 18 in INTENT_REPLICATION.md.
    bool          gateAuthority;

    // Damage guard, BOTH sides (KENSHICOOP_DAMAGE_GUARD != "0"; DEFAULT ON):
    // detour Character::hitByMeleeAttack so locally-simulated (cosmetic) fights
    // apply no damage to DRIVEN bodies - Kenshi's medical model is local-only,
    // so cosmetic damage would diverge forever. The guard set is "every body
    // this client drives": peer world-NPC copies on the join; peer SQUAD-member
    // copies on the host (extended 2026-07-06 after phase-1 player_combat
    // measured the host copy of a join victim bleeding 40+ while join copies
    // stayed 0). Outcomes stay owner-authoritative (KO/death/revive events).
    // "0" is the escape hatch.
    bool          damageGuard;

    // Peer-ready scenario arming (KENSHICOOP_ARM_TIMEOUT_MS). A scenario's clock
    // (onStart + elapsedMs) does not begin at gameplay start; it begins when this
    // client first RECEIVES a peer's owned-entity batch (Inbound::sawRemoteEntity
    // - on the host that is exactly "the join is loaded + streaming"), so no
    // host-side scripted action can fire before the join is in-game. This value
    // is the fallback: arm anyway after this many ms of gameplay (covers a peer
    // that never connects / host-only diagnostics). 0 = arm immediately at
    // gameplay start (the legacy behaviour; spike runs use this).
    unsigned long scenarioArmTimeoutMs;

    // Bidirectional ownership partition (KENSHICOOP_OWN_SQUAD, CSV of unsigned ints;
    // KENSHICOOP_OWN_RANK accepted as an alias). Both clients load the SAME save and
    // thus the SAME player squad; each client OWNS a disjoint set of SQUAD TABS chosen
    // by save-stable tab rank (distinct hand-containers, sorted; 0 = first tab). Every
    // member of an owned tab is controlled locally + streamed; the peer's tabs are
    // driven from its stream. Default: host owns {0}, join owns {1} - one squad tab
    // each. On a single-tab save the join owns nothing (one-directional, as before).
    std::set<unsigned int> ownRanks;
    // True only when ownRanks came from KENSHICOOP_OWN_SQUAD/OWN_RANK. When false
    // the ranks are the role default and must follow a mid-session role switch
    // (F2 panel Host<->Join), so the client owns {1} and does not claim the
    // host's rank-0 player squad (which would freeze that unit locally).
    bool          ownRanksFromEnv;

    // Phase 4a inventory sync (KENSHICOOP_INV_SYNC: "1" force on, "0" force off,
    // unset = ON for real sessions [scenario == ""] and the inventory scenarios).
    // When on, both clients stream the contents of every squad member they OWN
    // (equipped slots included) plus the host's registered storage container; the
    // peer reconciles its local copies. Promoted to a real-session default after
    // the 2026-07-07 remote session: equipment changes never crossed with it off.
    // Scripted test scenarios outside the auto-on list keep it off.
    bool          invSync;

    // Protocol 37 cross-owner transfer intents (KENSHICOOP_XFER_SYNC: "1" force on,
    // "0" force off, unset = ON whenever invSync is on). When on, BOTH clients run
    // the completed-drag detector over every tracked container (own + received) and
    // author reliable PKT_INV_XFER intents for moves that cross the single-writer
    // ownership boundary; receivers relocate the real item between their own copies
    // (conservation: no fabrication or destruction, so traded gear survives).
    // SUPERSEDED by blockXfer: when blockXfer is on, cross-owner drags are refused
    // at the engine (nothing to replicate), so xferSync is forced OFF.
    bool          xferSync;

    // Cross-owner trade veto (KENSHICOOP_BLOCK_XFER: "1" force on, "0" force off,
    // unset = OFF for real sessions - Protocol 37 replicate-the-trade is the
    // real-session default - and auto-ON ONLY for the xfer_block scenario, which
    // exists to keep the veto code exercised).
    // When on, a UI inventory drag whose SOURCE and DESTINATION squad characters
    // are owned by DIFFERENT clients is refused at the engine (Inventory::tryAddItem
    // detour) - the item stays in the source bag and nothing crosses the ownership
    // boundary. This makes ground-drop the only cross-client transfer path (the
    // dupe/wipe/weapon-vanish class of drag bug can't happen if the drag can't
    // complete). Same-owner drags (a player managing their own squads) are always
    // allowed; world containers and the vendor buyItem path are untouched. Retires
    // Protocol 37 (forces xferSync off). "0" is the escape hatch (restores the
    // Protocol 37 replicate-the-trade behaviour).
    bool          blockXfer;

    // Phase W1/W2 world-item sync (KENSHICOOP_WORLD_SYNC: "1" force on, "0" force
    // off, unset = ON for real sessions [scenario == ""] and the world_item_* /
    // drop / limb_loss scenarios). When on, the HOST streams free GROUND items in
    // the interest sphere (host-authoritative, netId-keyed proxies on the join)
    // and BOTH clients author gear drop/pickup conservation intents (W2/W3).
    // Promoted to a real-session default after the 2026-07-07 remote session:
    // dropped gear was invisible cross-client with it off.
    bool          worldSync;

    // Phase-2 medical sync (KENSHICOOP_MED_SYNC != "0"; DEFAULT ON): owner-
    // authoritative vitals stream for player-squad members (blood, bleed,
    // per-limb flesh + bandaging onto the peer's driven copies, change-gated
    // reliable) + treatment forwarding (first aid administered on a driven copy
    // returns to the body's owner as a raise-only bandage delta). World NPCs
    // stay on the events-only model. "0" is the A/B escape hatch.
    bool          medSync;

    // Consensus game-speed sync (KENSHICOOP_SPEED_SYNC != "0"; DEFAULT ON):
    // each client's UI speed (pause/1x/2x/3x) is a REQUEST; the host arbitrates
    // effective = min(requests), capped at 1x while either player squad is in
    // combat, and broadcasts the result both engines apply. Divergent speeds
    // diverge every rate-based local simulation (medical, hunger, cosmetic
    // fights). Pause travels as speed 0. "0" is the A/B escape hatch.
    bool          speedSync;

    // Character stats sync (KENSHICOOP_STATS_SYNC != "0"; DEFAULT ON;
    // protocol 17): owner-authoritative CharStats stream for player-squad
    // members (attributes/skills/xp onto the peer's driven copies, change-
    // gated reliable). Without it, driven copies keep save-load stats all
    // session - and the peer's engine resolves REAL fights with those stale
    // numbers. "0" is the A/B escape hatch.
    bool          statsSync;

    // Carried-body sync (KENSHICOOP_CARRY_SYNC != "0"; DEFAULT ON;
    // protocol 18): reliable pickup/drop edges + self-healing carried state
    // for player-squad members, executed engine-native on each machine's
    // local pair. Without it the peer sees the KO'd body dragged/teleported
    // along the ground behind its carrier. "0" is the A/B escape hatch.
    bool          carrySync;

    // KENSHICOOP_FURN_SYNC (default ON): furniture-occupancy sync (protocol 19)
    // - reliable bed/cage enter/exit edges + self-healing occupancy state,
    // executed engine-native (setBedMode/setPrisonMode) between each machine's
    // local pair. "0" is the A/B escape hatch.
    bool          furnSync;

    // KENSHICOOP_CHAIN_SYNC (default ON): chained/pole prisoner sync
    // (protocol 41) - a captive shackled to a prisoner POLE is chained
    // (Character::isChained + setChainedMode), a different engine system from a
    // cage (inSomething==IN_PRISON). Rides the furniture pipeline as kind=3.
    // Without it a poled unit never crosses the wire (the join leaves it at the
    // last carried/KO stage). "0" is the A/B escape hatch (beds/cages keep
    // working) if it ever freezes a walking slave.
    bool          chainSync;

    // KENSHICOOP_STEALTH_SYNC (default ON): stealth sync (protocol 20) -
    // continuous BODY_SNEAK posture apply on driven copies (engine-native
    // setStealthMode) + the host-authored detection-indicator feedback stream
    // (PKT_STEALTH -> the sneaker's owner replays notifyICanSeeYouSneaking).
    // "0" is the A/B escape hatch.
    bool          stealthSync;

    // KENSHICOOP_MONEY_SYNC (default ON): per-tab wallet sync (protocol 22) -
    // each client streams the Ownerships::money of every squad tab it OWNS
    // (change-gated reliable, keyed by tab rank); the receiver writes the
    // peer tab's wallet via Ownerships::setMoney. Without it any purchase /
    // sale / bounty changes cats on one client only (shop_probe finding).
    // "0" is the A/B escape hatch.
    bool          moneySync;

    // KENSHICOOP_SPAWN_SYNC (default ON): runtime-spawn proxy replication
    // (protocol 21) - the join requests a description (PKT_SPAWN_REQ) for any
    // streamed hand it cannot resolve (a host RUNTIME spawn: roaming squad,
    // dialog ambush) and mints a local proxy body from the host's reply
    // (PKT_SPAWN_INFO). Without it the host fights enemies the join can't
    // see. Forced OFF for spawn_probe (the diagnostic baselines the failure
    // modes). "0" is the A/B escape hatch.
    bool          spawnSync;

    // KENSHICOOP_RECRUIT_SYNC (default ON): recruitment sync (protocol 23) -
    // a detour on PlayerInterface::recruit authors a reliable EVT_RECRUIT
    // (old hand -> new hand) for every successful local recruit; the peer
    // RE-KEYS its existing local copy of the recruited body to the new
    // stream key (no duplicate proxy), and recruited hands are owned by
    // their RECRUITER regardless of tab rank. Without it a recruit exists
    // on the recruiting client only (recruit_probe finding). Forced OFF for
    // recruit_probe (the diagnostic baselines the unsynced behaviour).
    // "0" is the A/B escape hatch.
    bool          recruitSync;

    // KENSHICOOP_FACTION_SYNC (default ON): faction-relation sync (protocol
    // 24) - the host streams the player-faction relation table (keyed by
    // faction GameData stringID, change-gated reliable) and the join applies
    // it via FactionRelations::setRelation; join-side relation mutations
    // (detoured affectRelations) forward to the host as reliable intents.
    // Without it attacking a faction flips hostility on one client only.
    // Forced OFF for faction_probe (the diagnostic baselines the unsynced
    // relations). "0" is the A/B escape hatch.
    bool          factionSync;

    // KENSHICOOP_TIME_SYNC (default ON): game-clock sync (protocol 25) - the
    // host broadcasts its absolute in-game clock (PKT_TIME, ~1 Hz reliable);
    // the join measures the offset and corrects it (step if a writable clock
    // base exists, otherwise slew by scaling its local sim speed QUIETLY on
    // top of the arbitrated consensus speed). Without it each client
    // integrates its own clock from its own load/pause moments and day/night
    // diverges. Forced OFF for time_probe (with speedSync, so the probe
    // measures raw unsynced drift and its speed burst isn't re-arbitrated).
    // "0" is the A/B escape hatch.
    bool          timeSync;

    // KENSHICOOP_DOOR_SYNC (default ON): door/gate state sync (protocol 26) -
    // a symmetric change-gated channel: both clients sample nearby baked
    // doors ~1 Hz and stream rows whose (open, locked) moved vs a per-hand
    // baseline; received rows apply through the engine's own door actions.
    // Without it one player walks through a gate the other sees closed.
    // Forced OFF for door_probe (it measures the unsynced baseline). "0" is
    // the A/B escape hatch.
    bool          doorSync;

    // KENSHICOOP_BUILD_SYNC (default ON): placed-building sync (protocol 27)
    // - a placer-authoritative describe/mint channel: a local placement
    // (UI commit detour or programmatic) streams its template sid +
    // transform keyed by the PLACER's hand; the peer mints a local proxy
    // site and applies streamed construction-progress rows through the
    // engine's own setter. Without it a building one player places does not
    // exist for the other. Forced OFF for build_probe (it measures the
    // unsynced baseline). "0" is the A/B escape hatch.
    bool          buildSync;

    // KENSHICOOP_BDOOR_SYNC (default ON): placed-building door + dismantle
    // sync (protocol 28) - door rows on session-placed buildings translated
    // through the protocol-27 build maps (keyed by the placer's building
    // hand + door index), and placer-authoritative building removal
    // (PKT_BUILD_REMOVE) so a dismantled/destroyed placed building removes
    // its proxy on the peer. Forced OFF for bdoor_probe (it measures the
    // unsynced baseline with the mint channel still on). "0" is the A/B
    // escape hatch.
    bool          bdoorSync;

    // KENSHICOOP_HUNGER_SYNC (default ON): hunger sync (protocol 29) - the
    // owner-authoritative hunger/fed scalars ride the PKT_MEDICAL snapshot
    // (a sub-gate of medSync: OFF sends/applies the fields as -1 = not
    // carried while the rest of the medical stream is untouched). Without it
    // each client decays every character's hunger locally and eating only
    // happens on the owner's client, so driven copies starve in the peer's
    // view. Forced OFF for hunger_probe (it measures the unsynced baseline).
    // "0" is the A/B escape hatch.
    bool          hungerSync;

    // KENSHICOOP_SAVE_SYNC (default ON): coordinated save + session resume
    // (protocol 31) - every local save on the HOST (menu, quicksave,
    // autosave, programmatic) triggers the host-authoritative flow: wait for
    // the save folder to quiesce, then stream the whole folder to the join
    // (PKT_SAVE_BEGIN/FILE/DONE, staged + CRC-verified + atomically
    // committed, PKT_SAVE_ACK back). A save initiated on the JOIN is
    // suppressed locally and forwarded as PKT_SAVE_REQ (one authoritative
    // save; the transfer delivers the join's copy). Resume = both clients
    // load the identical file. Forced OFF for save_probe (it measures the
    // raw save behaviour with the detour alone). "0" is the A/B escape
    // hatch.
    bool          saveSync;

    // KENSHICOOP_LOAD_SYNC (default ON): coordinated load (protocol 32) -
    // a mid-session load on the HOST (menu or programmatic - the
    // SaveManager::load detour catches them all) broadcasts PKT_LOAD_GO
    // (name + folder fingerprint); the join fingerprint-verifies its local
    // copy and loads the identical save, falling back to the protocol-31
    // SaveXfer transfer when its copy is missing/diverged (PKT_LOAD_NACK).
    // A load initiated on the JOIN is suppressed locally and forwarded as
    // PKT_LOAD_REQ (the host arbitrates). Both sides run a full session
    // reset on their own world-reload edge. Forced OFF for load_probe (it
    // measures the raw swap behaviour with the detour + edge detection
    // alone). "0" is the A/B escape hatch.
    bool          loadSync;

    // KENSHICOOP_PROD_SYNC (default ON): production machine sync (protocol
    // 33) - the HOST samples machine-class buildings (production / crafting /
    // furnace / farm / research) in the interest spheres ~1 Hz and streams
    // power state, production state, output/input buffer amounts and farm
    // growth floats (change-gated reliable, keyed by hand with the
    // protocol-27 translation for session-placed machines); the join applies
    // through the engine's own levers (switchPowerOn / setProductionItem /
    // direct amount writes). Without it every machine simulates per-client
    // and stored output, fuel, power and crop growth silently fork. Forced
    // OFF for prod_probe (it measures the unsynced baseline). "0" is the A/B
    // escape hatch.
    bool          prodSync;

    // KENSHICOOP_RESEARCH_SYNC (default ON): research tech-tree sync
    // (protocol 38) - the HOST samples its Research store's known set ~1 Hz
    // (Research::isKnown over the shared RESEARCH GameData enumeration) and
    // streams one reliable PKT_RESEARCH row per known stringID (first sight
    // is the session baseline, then a safety resend); the join applies each
    // row via Research::startResearch - idempotent against already-known
    // sids. Without it the tech tree is per-client: a tech the host
    // researches never unlocks on the join (spike 401). Forced OFF for
    // research_probe (it measures the unsynced baseline). "0" is the A/B
    // escape hatch.
    bool          researchSync;

    // KENSHICOOP_BOUNTY_SYNC (default ON): bounty/crime sync (protocol 45) -
    // the HOST is the witness authority (H2, settled by the 2026-07-20 live
    // run): it samples every durable bounty row on the bodies it carries (its
    // driven copies of remote PCs, where a join-owned PC's bounty lives, plus
    // host-owned PCs) ~1 Hz and streams change-gated PKT_BOUNTY rows keyed
    // per-(character hand, faction sid); the owning client applies each row onto
    // its own clean copy via the engine's own levers (unfairAddToBounty raise /
    // clearBounty drop). Without it a character's wanted level is per-client:
    // a crime one player commits leaves the peer's copy unwanted (spikes 59/60).
    // Unidirectional host->clients (no echo path). "0" is the A/B escape hatch.
    bool          bountySync;

    // KENSHICOOP_STORE_SYNC (default ON): storage/machine container sync
    // (protocol 34) - the HOST censuses container-bearing buildings (storage
    // chests + the machine classes) in the interest spheres ~1 Hz and
    // registers each with the container-inventory channel, replacing the
    // single-container v1 registration; contents stream as the existing
    // change-gated PKT_INV_SNAPSHOT (hash + settle window + safety resend),
    // keyed by hand with the protocol-27 translation for session-placed
    // buildings. The join reconciles through applyContainerContents. Without
    // it every chest and machine inventory forks per-client the moment items
    // land in it. Layered on invSync (no container channel, no store sync).
    // Forced OFF for store_probe (it measures the unsynced baseline). "0" is
    // the A/B escape hatch.
    bool          storeSync;

    // KENSHICOOP_SQUAD_SYNC (default ON): squad management sync (protocol 35)
    // - a squad-tab MOVE re-containers the body (its hand changes) exactly
    // like a recruit, but no engine function owns the UI path, so the roster
    // is POLLED ~1 Hz (pointer -> hand diff; the Character* survives the
    // re-container). Each edge streams as EVT_SQUAD_MOVE (the EVT_RECRUIT
    // shape); the peer re-keys its local body to the new stream key and the
    // ownership pins keep the mover authoritative regardless of how the tab
    // ranks shuffle. Without it a moved unit freezes/forks on the peer.
    // Forced OFF for squad_probe (it measures the unsynced baseline). "0" is
    // the A/B escape hatch.
    bool          squadSync;

    // KENSHICOOP_LATEJOIN_SYNC (default ON): late-join/reconnect resync
    // (protocol 30, no wire change) - on the peer-connect edge the
    // Replicator re-announces every live placed-building PLACE (+ REMOVE
    // for removed ones) and forces an immediate full resend across all
    // change-gated reliable channels. Without it, state that moved before
    // the connect heals only per-channel safety resend (up to 10 s), and a
    // building placed before the connect never exists on the peer at all.
    // Forced OFF for latejoin_probe (it measures the unsynced baseline).
    // "0" is the A/B escape hatch.
    bool          latejoinSync;

    // Transport selection (KENSHICOOP_TRANSPORT): "udp" (default) or "steam".
    // "steam" tunnels the unchanged ENet protocol over Steam P2P (legacy
    // ISteamNetworking in the game's own steam_api64.dll): connections are
    // made BY STEAMID with automatic NAT punching and Valve-relay fallback -
    // no port forwarding, no public IPs, immune to CGNAT. Requires
    // steamPeer below; falls back to UDP (loudly) when Steam is unavailable.
    std::string   transport;

    // The co-op partner's steamid64 (KENSHICOOP_STEAM_PEER). Two-code
    // exchange: EACH side is configured with the OTHER's SteamID (sending to
    // a SteamID implicitly accepts its session - no Steam callback plumbing).
    unsigned long long steamPeer;

    // Steam reachability spike (KENSHICOOP_STEAM_PING=<steamid64>): ping/echo
    // that peer on P2P channel 1 every 2 s and log RTT + punch-vs-relay,
    // independent of the transport in use. 0 = off.
    unsigned long long steamPing;

    // In-game co-op panel session control (2026-07-13). When the mod is driven
    // by the F2 panel instead of the env launchers, networking is DEFERRED at
    // load: the session (host listen / client connect) only starts when the
    // player hits Connect in the panel, using the role/transport/peer chosen
    // there. autoConnect (KENSHICOOP_AUTOCONNECT=1) restores the legacy behaviour
    // - start the session immediately from the env/config role+transport+peer.
    // A test scenario or KENSHICOOP_TEST_SECONDS ALWAYS auto-starts regardless
    // (the unattended harness never touches the panel); see Plugin.cpp.
    bool               autoConnect;
};

// Read every KENSHICOOP_* var into 'out', applying host/join defaults. Values in
// coop_config.json (next to the DLL) are used as the defaults, so precedence is
// env var > config file > hard-coded default.
void loadConfig(Config& out);

// Re-read only the connection target (steamPeer / ip / port) from
// coop_config.json into 'c'. Called on the panel's Connect so a friend-code edit
// applies without restarting the game. No-op for keys absent from the file.
void reloadPeerFromFile(Config& c);

// One-line summary of the RESOLVED (effective) config - every sync channel's
// on/off state plus the key tuning knobs - for the startup log. Makes "which
// config did this run actually use?" answerable from the log alone (real
// sessions included), instead of having to reconstruct it from env + file.
std::string describeConfig(const Config& c);

} // namespace coop

#endif // KENSHICOOP_CONFIG_H
