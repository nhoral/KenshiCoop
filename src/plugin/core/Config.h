// Config - parse the KENSHICOOP_* environment variables once at load.
//
// Mode/role and all runtime knobs are env-driven so host vs join is chosen
// without a recompile (the test harness sets these before launching Kenshi).

#ifndef KENSHICOOP_CONFIG_H
#define KENSHICOOP_CONFIG_H

#include <string>

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
    std::string   setupScene;      // KENSHICOOP_SETUP ("" = off; "chair"/"npc")
                                   // host-only one-shot world spawn to bake a
                                   // deterministic test scene into a save.
    bool          probeRecruit;    // KENSHICOOP_PROBE_RECRUIT == "1" (join only):
                                   // recruit diverged NPCs into the player squad
                                   // to validate the AI-gating "inhabit" lever.
    bool          probeAiSuspend;  // KENSHICOOP_PROBE_AISUSPEND == "1" (join only):
                                   // detour Character::periodicUpdate to suspend the
                                   // AI decision layer for host-driven NPCs (keeps
                                   // animation; stops self-tasking) - faction-safe.
};

// Read every KENSHICOOP_* var into 'out', applying host/join defaults.
void loadConfig(Config& out);

} // namespace coop

#endif // KENSHICOOP_CONFIG_H
