// Scenario - deterministic, log-emitting test scripts driven from the tick hook.
//
// A scenario runs on BOTH clients (it branches on ctx.isHost). The authoritative
// side emits "SCENARIO MEMBER hand=.. pos=.." lines; the observing side emits
// "SCENARIO RECV hand=.. pos=.." lines. The PowerShell runner cross-checks
// MEMBER vs RECV positions per hand within a tolerance, in addition to requiring
// "SCENARIO RESULT PASS" from each client. Keep these schema strings stable -
// run_test.ps1 parses them.

#ifndef KENSHICOOP_SCENARIO_H
#define KENSHICOOP_SCENARIO_H

#include <string>
#include "../../netproto/Wire.h"

class GameWorld;

namespace coop {

struct ScenarioContext {
    GameWorld*    gw;
    bool          isHost;
    u32           localId;
    unsigned long elapsedMs; // since onStart
    unsigned int  tick;      // scenario tick counter
};

class Scenario {
public:
    virtual ~Scenario() {}
    virtual const char* name() const = 0;
    virtual void onStart(const ScenarioContext& ctx) = 0;
    // Returns true when the scenario is complete (caller logs RESULT then holds).
    virtual bool onTick(const ScenarioContext& ctx) = 0;
    virtual bool passed() const = 0;
};

// Build the scenario named 'name', or 0 if unknown.
Scenario* makeScenario(const std::string& name);

} // namespace coop

#endif // KENSHICOOP_SCENARIO_H
