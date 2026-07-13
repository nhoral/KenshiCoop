// Scenario.cpp - the scenario factory (monolith split, 2026-07-12): the ONLY
// public entry point (makeScenario, declared in Scenario.h) chains the
// per-domain factory hooks from ScenarioSupport.h. Every scenario class lives
// TU-private in its domain file (ScenarioMovement/Npc/Combat/Medical/
// Inventory/WorldItems/CharState/Probes/Buildings/Session.cpp); shared
// SCENARIO-line emitters live in ScenarioSupport.cpp.
// Must NOT: contain scenario logic - add new scenarios in a domain TU and
// register them in that TU's maker (resources/CODE_MAP.md, scenario plane).

#include "ScenarioSupport.h"

namespace coop {

Scenario* makeScenario(const std::string& name) {
    Scenario* s = 0;
    if ((s = makeProbeScenario(name)))     return s; // 'spike' + economy/faction/time
    if ((s = makeMovementScenario(name)))  return s;
    if ((s = makeNpcScenario(name)))       return s;
    if ((s = makeCombatScenario(name)))    return s;
    if ((s = makeMedicalScenario(name)))   return s;
    if ((s = makeInventoryScenario(name))) return s;
    if ((s = makeWorldItemScenario(name))) return s;
    if ((s = makeCharStateScenario(name))) return s;
    if ((s = makeBuildingScenario(name)))  return s;
    if ((s = makeSessionScenario(name)))   return s;
    return 0;
}

} // namespace coop
