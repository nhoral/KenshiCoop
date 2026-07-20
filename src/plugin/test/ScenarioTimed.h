// ScenarioTimed.h - Phase 7 (Workstream A): a harness-only base for the common
// bookkeeping shared by the ~50 Scenario*.cpp classes.
//
// It factors out ONLY the non-emitting boilerplate every scenario re-implements:
//   * the scenario id behind Scenario::name(),
//   * the passed_ latch behind Scenario::passed(), and
//   * the periodic-EVIDENCE cadence gate (the ubiquitous
//     `elapsed - last >= cad || last == 0` idiom + its last-stamp).
//
// It emits NO log lines and imposes NO banner, timeline, or pass predicate, so
// migrating a scenario onto it is byte-identical on the wire AND in the oracle
// log: each scenario keeps its own "SCENARIO ..." strings (the load-bearing
// oracle API, resources/CODE_MAP.md), its own per-side durations, and its own
// pass logic. This lives only in the Harness build (the whole test\ tree is
// ExcludedFromBuild in Release).

#ifndef KENSHICOOP_SCENARIO_TIMED_H
#define KENSHICOOP_SCENARIO_TIMED_H

#include "Scenario.h"

namespace coop {

class TimedScenario : public Scenario {
public:
    // name: the exact scenario id the runner/manifest key on (probe/sync twins
    // pass the resolved name). evidenceMs: cadence for evidenceDue(); pass 0 if
    // the scenario does not use the periodic-evidence gate.
    TimedScenario(const char* name, unsigned long evidenceMs)
        : name_(name), evidenceMs_(evidenceMs), lastEvidenceMs_(0),
          passed_(false) {}

    virtual const char* name() const { return name_; }
    virtual bool passed() const { return passed_; }

protected:
    // Periodic-evidence gate: true on the FIRST tick and every evidenceMs_ after
    // - exactly the `elapsed - last >= cad || last == 0` idiom. Advances the
    // last-evidence stamp as a side effect when it returns true.
    bool evidenceDue(unsigned long elapsedMs) {
        if (lastEvidenceMs_ != 0 && (elapsedMs - lastEvidenceMs_) < evidenceMs_)
            return false;
        lastEvidenceMs_ = elapsedMs;
        return true;
    }

    void setPassed(bool v) { passed_ = v; }

    const char*   name_;
    unsigned long evidenceMs_;
    unsigned long lastEvidenceMs_;
    bool          passed_;
};

} // namespace coop

#endif // KENSHICOOP_SCENARIO_TIMED_H
