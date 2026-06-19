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
    bool          probeAiSuspend;  // KENSHICOOP_PROBE_AISUSPEND == "1" (join only):
                                   // detour Character::periodicUpdate to suspend the
                                   // AI decision layer for host-driven NPCs (keeps
                                   // animation; stops self-tasking) - faction-safe.

    // Debug WAN simulation: artificially delay (and optionally drop) inbound entity
    // batches so the same loopback harness exercises the latency path - render
    // interpolation, dead reckoning, stale-state enforcement - that a real internet
    // link would impose. All zero (default) = no simulation, immediate delivery.
    unsigned int  netSimDelayMs;   // KENSHICOOP_NETSIM_DELAY_MS  (base one-way delay)
    unsigned int  netSimJitterMs;  // KENSHICOOP_NETSIM_JITTER_MS (+/- uniform variance)
    unsigned int  netSimLossPct;   // KENSHICOOP_NETSIM_LOSS_PCT  (0-100 drop chance)

    // Bidirectional ownership partition (KENSHICOOP_OWN_SQUAD, CSV of unsigned ints;
    // KENSHICOOP_OWN_RANK accepted as an alias). Both clients load the SAME save and
    // thus the SAME player squad; each client OWNS a disjoint set of SQUAD TABS chosen
    // by save-stable tab rank (distinct hand-containers, sorted; 0 = first tab). Every
    // member of an owned tab is controlled locally + streamed; the peer's tabs are
    // driven from its stream. Default: host owns {0}, join owns {1} - one squad tab
    // each. On a single-tab save the join owns nothing (one-directional, as before).
    std::set<unsigned int> ownRanks;
};

// Read every KENSHICOOP_* var into 'out', applying host/join defaults.
void loadConfig(Config& out);

} // namespace coop

#endif // KENSHICOOP_CONFIG_H
