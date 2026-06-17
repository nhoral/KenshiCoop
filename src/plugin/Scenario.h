// Scenario - compiled, scripted functional-test sequences for KenshiCoop.
//
// A *scenario* is a named state machine that runs in mainLoop_hook AFTER the
// auto-loaded save reaches live gameplay. It sets up a deterministic in-game
// state (spawn NPCs, configure the squad), performs actions (move/teleport),
// and logs the observed state via the CHECK / SCENARIO MEMBER schema. Both
// clients run the same scenario object: the HOST drives the authoritative
// actions, the JOIN observes replicated state, and run_test.ps1 compares the
// two logs numerically for the verdict.
//
// Scenarios are selected at launch by the KENSHICOOP_SCENARIO environment
// variable and built through makeScenario() (the registry). A scenario calls
// ONLY the action facade in ScenarioApi.h (never the engine directly), so all
// game mutation stays on the main thread behind the existing SEH guards.
//
// To add a scenario: subclass coop::Scenario, implement name()/onStart()/
// onTick()/passed(), then register it in makeScenario() (Scenario.cpp).

#ifndef KENSHICOOP_SCENARIO_H
#define KENSHICOOP_SCENARIO_H

#include <windows.h>
#include <string>

#include "../netproto/Protocol.h"

class GameWorld;

namespace coop {

// Everything a scenario needs to know about the current frame. Filled by
// mainLoop_hook and passed to onStart()/onTick().
struct ScenarioContext {
    GameWorld* gw;        // live game world (never null when the scenario runs)
    bool       isHost;    // this client is the authoritative host
    u32        localId;   // our network player id
    DWORD      elapsedMs; // wall-clock ms since the scenario armed (onStart)
    unsigned   tick;      // scenario tick counter (increments each onTick)
};

// Abstract scenario. A scenario is a deterministic, time-gated state machine.
class Scenario {
public:
    virtual ~Scenario() {}

    // Stable identifier, also used in log lines (must match KENSHICOOP_SCENARIO).
    virtual const char* name() const = 0;

    // Called once, on the first in-game frame after gameplay starts.
    virtual void onStart(const ScenarioContext& ctx) = 0;

    // Called every frame after onStart. Return true when the scenario is
    // complete (the harness then logs SCENARIO RESULT and exits the process).
    virtual bool onTick(const ScenarioContext& ctx) = 0;

    // Final in-plugin verdict, queried once after onTick() returns true.
    virtual bool passed() const = 0;
};

// Registry: construct a scenario by name, or return 0 if the name is unknown.
// The caller owns the returned object (freed at process exit).
Scenario* makeScenario(const std::string& name);

} // namespace coop

#endif // KENSHICOOP_SCENARIO_H
