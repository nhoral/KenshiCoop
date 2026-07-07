#define _CRT_SECURE_NO_WARNINGS 1 // getenv/atoi are fine; silence VC10 C4996

#include "Config.h"
#include <cstdlib>

namespace coop {
namespace {

std::string envOr(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return std::string(v ? v : def);
}

} // namespace

void loadConfig(Config& c) {
    std::string mode = envOr("KENSHICOOP_MODE", "host");
    c.isHost      = (mode != "join");
    c.ip          = envOr("KENSHICOOP_IP", "127.0.0.1");
    c.port        = std::atoi(envOr("KENSHICOOP_PORT", "27800").c_str());
    c.save        = envOr("KENSHICOOP_SAVE", "");
    c.testSeconds = std::atoi(envOr("KENSHICOOP_TEST_SECONDS", "0").c_str());

    std::string defLog = c.isHost ? "KenshiCoop_host.log" : "KenshiCoop_join.log";
    c.logPath  = envOr("KENSHICOOP_LOG", defLog.c_str());
    c.scenario = envOr("KENSHICOOP_SCENARIO", "");

    int d = std::atoi(envOr("KENSHICOOP_AUTOLOAD_DELAY_MS", "5000").c_str());
    c.autoLoadDelayMs = (d > 0) ? (unsigned long)d : 5000ul;

    c.setupScene = envOr("KENSHICOOP_SETUP", "");
    c.probeRecruit = envOr("KENSHICOOP_PROBE_RECRUIT", "") == "1";
    // AI-suspend is the DEFAULT quieting layer for the join's driven world NPCs
    // (review 2026-07-05). KENSHICOOP_AI_SUSPEND=0 is the escape hatch; the legacy
    // probe env still forces it on so old harness call sites keep working.
    c.aiSuspend = (envOr("KENSHICOOP_AI_SUSPEND", "1") != "0") ||
                  (envOr("KENSHICOOP_PROBE_AISUSPEND", "") == "1");
    c.noDetach  = envOr("KENSHICOOP_NO_DETACH", "") == "1";
    c.damageGuard = envOr("KENSHICOOP_DAMAGE_GUARD", "1") != "0";
    // Divergence-gated authority promoted to DEFAULT ON (step-4 A/B, 2026-07-05:
    // trusted-set engaged in 4/4 runs - grants 5-8, trusted ~5 of 12 driven - with
    // npc_track/pose gates at parity; the single red run was a host crash that
    // retry-passed). "0" is the escape hatch.
    c.gateAuthority = envOr("KENSHICOOP_GATE_AUTHORITY", "1") != "0";

    // Inventory sync (Phase 4a): explicit env, OR auto-on when the inventory bake
    // scene / the inv_order scenario is active (so the test + manual gate just work).
    c.invSync = (envOr("KENSHICOOP_INV_SYNC", "") == "1") ||
                (c.setupScene == "inventory") ||
                (c.scenario == "inv_order") || (c.scenario == "inv_bidir") ||
                (c.scenario == "inv_equip") || (c.scenario == "inv_reequip") ||
                (c.scenario == "world_weapon_drop"); // W2 drop must beat the inv-reconcile destroy

    // World-item sync (Phase W1/W2): explicit env, OR auto-on for the world_item_* family
    // (the drop_probe diagnostic is host-only and needs no world stream), the W2
    // conservation-drop scenario (which rides the world-drop channel), and limb_loss
    // (protocol 16: the severed-limb GROUND item replicates via this channel - the
    // host's real item streams as a W1 proxy, the join's local copy is deduped).
    c.worldSync = (envOr("KENSHICOOP_WORLD_SYNC", "") == "1") ||
                  (c.scenario.compare(0, 11, "world_item_") == 0) ||
                  (c.scenario == "world_weapon_drop") ||
                  (c.scenario == "limb_loss");

    // Medical sync (phase 2 of the player-combat/medical plan): DEFAULT ON -
    // owner-authoritative vitals for player-squad members + treatment forwarding.
    // Without it, spikes 21-23's truth holds: driven copies' vitals diverge
    // forever and cross-player first aid is lost. "0" is the A/B escape hatch.
    c.medSync = envOr("KENSHICOOP_MED_SYNC", "1") != "0";

    // Consensus game-speed sync: DEFAULT ON - requests min-arbitrated by the
    // host, combat caps fast-forward at 1x. "0" is the A/B escape hatch.
    c.speedSync = envOr("KENSHICOOP_SPEED_SYNC", "1") != "0";

    // Character stats sync (protocol 17): DEFAULT ON - owner-authoritative
    // CharStats stream for player-squad members. Without it a driven copy
    // keeps save-load stats all session, and the peer's engine resolves REAL
    // fights with those stale numbers. "0" is the A/B escape hatch.
    c.statsSync = envOr("KENSHICOOP_STATS_SYNC", "1") != "0";

    // Carried-body sync (protocol 18): DEFAULT ON - reliable pickup/drop
    // edges + self-healing carried state for player-squad members. Without
    // it the peer's down-enforcement drags/teleports a carried KO'd body
    // along the ground behind its carrier. "0" is the A/B escape hatch.
    c.carrySync = envOr("KENSHICOOP_CARRY_SYNC", "1") != "0";

    int delay  = std::atoi(envOr("KENSHICOOP_NETSIM_DELAY_MS", "0").c_str());
    int jitter = std::atoi(envOr("KENSHICOOP_NETSIM_JITTER_MS", "0").c_str());
    int loss   = std::atoi(envOr("KENSHICOOP_NETSIM_LOSS_PCT", "0").c_str());
    c.netSimDelayMs  = (delay  > 0) ? (unsigned int)delay  : 0u;
    c.netSimJitterMs = (jitter > 0) ? (unsigned int)jitter : 0u;
    c.netSimLossPct  = (loss   > 0) ? (unsigned int)(loss > 100 ? 100 : loss) : 0u;

    c.fakeClockSkewMs = (long)std::atoi(envOr("KENSHICOOP_FAKE_CLOCK_SKEW_MS", "0").c_str());

    int armTimeout = std::atoi(envOr("KENSHICOOP_ARM_TIMEOUT_MS", "45000").c_str());
    c.scenarioArmTimeoutMs = (armTimeout > 0) ? (unsigned long)armTimeout : 0ul;

    // Ownership squad-tab ranks: parse a CSV of unsigned ints (e.g. "0", "1", "1,2").
    // KENSHICOOP_OWN_SQUAD is primary; KENSHICOOP_OWN_RANK is an accepted alias. Empty
    // env -> default (host owns tab {0} / join owns tab {1}).
    c.ownRanks.clear();
    {
        std::string ranks = envOr("KENSHICOOP_OWN_SQUAD", envOr("KENSHICOOP_OWN_RANK", "").c_str());
        unsigned int v = 0; bool have = false;
        for (size_t i = 0; i < ranks.size(); ++i) {
            char ch = ranks[i];
            if (ch >= '0' && ch <= '9') { v = v * 10u + (unsigned int)(ch - '0'); have = true; }
            else if (have) { c.ownRanks.insert(v); v = 0; have = false; }
        }
        if (have) c.ownRanks.insert(v);
        if (c.ownRanks.empty()) c.ownRanks.insert(c.isHost ? 0u : 1u);
    }
}

} // namespace coop
