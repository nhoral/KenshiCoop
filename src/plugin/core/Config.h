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
    std::string   setupScene;      // KENSHICOOP_SETUP ("" = off; "chair"/"npc"/"craft"/"down"/"downhold"/"duel"/"squad")
                                   // host-only one-shot world spawn to bake a
                                   // deterministic test scene into a save.
    bool          probeRecruit;    // KENSHICOOP_PROBE_RECRUIT == "1" (join only):
                                   // recruit diverged NPCs into the player squad
                                   // to validate the AI-gating "inhabit" lever.
    bool          aiSuspend;      // KENSHICOOP_AI_SUSPEND != "0" (join only; DEFAULT ON):
                                   // detour Character::periodicUpdate to suspend the
                                   // AI decision layer for host-driven NPCs (keeps
                                   // animation; stops self-tasking) - faction-safe.
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

    // Phase 4a inventory sync (KENSHICOOP_INV_SYNC == "1"). When on, the HOST registers
    // the baked storage container nearest the leader (else the leader's own inventory)
    // as an owned container and streams its contents; the JOIN reconciles its local
    // copy. Default off so unrelated tests are unaffected; auto-enabled when the
    // 'inventory' setup scene or the 'inv_order' scenario is active.
    bool          invSync;

    // Phase W1 world-item sync (KENSHICOOP_WORLD_SYNC == "1"). When on, the HOST streams
    // free GROUND items inside the players' interest sphere (host-authoritative, netId-
    // keyed) and the JOIN spawns/updates/culls local proxies to match. Default off;
    // auto-enabled for the world_item_* scenarios.
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
};

// Read every KENSHICOOP_* var into 'out', applying host/join defaults.
void loadConfig(Config& out);

} // namespace coop

#endif // KENSHICOOP_CONFIG_H
