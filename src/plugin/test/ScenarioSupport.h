// ScenarioSupport.h - the PRIVATE shared surface for the Scenario*.cpp domain
// TUs (monolith split of Scenario.cpp, 2026-07-12): the common include
// prelude, the shared SCENARIO-line emitters + squad-tab classification
// helpers (defined in ScenarioSupport.cpp), and the per-domain factory hooks
// that Scenario.cpp's makeScenario chains.
//
// The log emitters are load-bearing API: "SCENARIO MEMBER/RECV/VITALS ..."
// phrasing and field order are what the PowerShell oracles key on (see
// resources/CODE_MAP.md, log-tag index). Never change a format string here.
// Scenario classes themselves stay INSIDE their domain TU (anonymous
// namespace) - only the maker function crosses TUs, so Scenario.h and every
// caller stay unchanged.

#ifndef KENSHICOOP_SCENARIO_SUPPORT_H
#define KENSHICOOP_SCENARIO_SUPPORT_H

#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "ScenarioTimed.h" // Phase 7: shared timed-scenario base (duration/cadence/passed)
#include "../CoopLog.h"
#include "../game/Engine.h"
#include "../game/EngineScenario.h" // Phase 5a: deterministic test-scene builders
#include "../game/EngineProbe.h"    // Phase 5a: spike-401/451/402 diagnostic probes
#include "../sync/SaveXfer.h" // save_probe / save_sync (protocol 31)

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <string>

namespace coop {

// ---- Shared helpers (ScenarioSupport.cpp; docs at the definitions) ----------

// One "SCENARIO <kind> hand=i,s,t,c,cs pos=x,y,z" line for character 'c'.
bool logScenarioLine(const char* kind, Character* c);
// Same, straight from a captured EntityState (adds task/pelvis/crouch/idle/bs).
void logScenarioEntity(const char* kind, const EntityState& e);
// Extended "SCENARIO VITALS" line for the body at hand h (readObjectHand layout).
void logVitalsLine(const unsigned int h[5], unsigned long t);
// Squad-tab classification (mirrors the Replicator's ownership partition).
bool tabHandLess(const EntityState& a, const EntityState& b);
bool tabCtnrLess(const EntityState& a, const EntityState& b);
bool tabCtnrEq(const EntityState& a, const EntityState& b);
int  tabRankOf(const EntityState* sq, unsigned int n, unsigned int i);
int  tabLeaderIdx(const EntityState* sq, unsigned int n, unsigned int rank);
// Fill h[5] (readObjectHand layout) from a captured EntityState's hand fields.
void handFromEntity(const EntityState& e, unsigned int h[5]);

// ---- Per-domain factory hooks (each returns 0 when the name is not its own;
// ---- Scenario.cpp's makeScenario chains them) --------------------------------

Scenario* makeMovementScenario(const std::string& name);  // ScenarioMovement.cpp
Scenario* makeNpcScenario(const std::string& name);       // ScenarioNpc.cpp
Scenario* makeCombatScenario(const std::string& name);    // ScenarioCombat.cpp
Scenario* makeMedicalScenario(const std::string& name);   // ScenarioMedical.cpp
Scenario* makeInventoryScenario(const std::string& name); // ScenarioInventory.cpp
Scenario* makeWorldItemScenario(const std::string& name); // ScenarioWorldItems.cpp
Scenario* makeCharStateScenario(const std::string& name); // ScenarioCharState.cpp
Scenario* makeProbeScenario(const std::string& name);     // ScenarioProbes.cpp
Scenario* makeBuildingScenario(const std::string& name);  // ScenarioBuildings.cpp
Scenario* makeSessionScenario(const std::string& name);   // ScenarioSession.cpp

} // namespace coop

#endif // KENSHICOOP_SCENARIO_SUPPORT_H
