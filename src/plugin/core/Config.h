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

    // Phase 4a inventory sync (KENSHICOOP_INV_SYNC: "1" force on, "0" force off,
    // unset = ON for real sessions [scenario == ""] and the inventory scenarios).
    // When on, both clients stream the contents of every squad member they OWN
    // (equipped slots included) plus the host's registered storage container; the
    // peer reconciles its local copies. Promoted to a real-session default after
    // the 2026-07-07 remote session: equipment changes never crossed with it off.
    // Scripted test scenarios outside the auto-on list keep it off.
    bool          invSync;

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
};

// Read every KENSHICOOP_* var into 'out', applying host/join defaults.
void loadConfig(Config& out);

} // namespace coop

#endif // KENSHICOOP_CONFIG_H
