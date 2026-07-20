// EngineFaults - typed, throttled fault accounting for the SEH-guarded engine.
//
// Every guarded engine call swallows a structured exception with
//   __except (EXCEPTION_EXECUTE_HANDLER) { return 0/false; }
// so a transient bad pointer degrades to a no-op instead of crashing the game
// (Engine.h contract). The cost is that a genuine fault becomes INDISTINGUISHABLE
// from a legitimate "not found" - it vanishes silently. This module turns that
// swallow into an observable, TYPED signal: each __except bumps its operation's
// counter and emits a throttled "[engine] FAULT op=<name> n=<count>" log line the
// PowerShell oracles can watch (see resources/CODE_MAP.md).
//
// Because Phase 5b funneled every hand resolve/capture through a handful of
// central helpers (resolveChar/resolveObject/handOf/charHandOf/invOf), wiring
// noteFault() into just those covers the whole ~156-call-site surface.
//
// Main-thread only (the engine-mutation invariant): the counters are plain
// statics with no locking. Pure throttle logic (faultShouldLog) lives inline so
// the unit layer (prototest) can lock it without linking the game logger.

#ifndef KENSHICOOP_ENGINE_FAULTS_H
#define KENSHICOOP_ENGINE_FAULTS_H

namespace coop {
namespace engine {

// Typed fault classes. Order is a diagnostic contract (faultOpName tokens are
// parsed by the oracles); append new ops before FAULT_OP_COUNT, never reorder.
enum FaultOp {
    FAULT_RESOLVE_CHAR = 0, // resolveChar / resolve(EntityState) / resolveCharByHand
    FAULT_RESOLVE_OBJECT,   // resolveObject / resolveObjectByHand
    FAULT_HAND_READ,        // handOf / charHandOf (readObjectHand / readHand)
    FAULT_INV_OF,           // invOf (RootObject::getInventory)
    FAULT_CAPTURE,          // captureOne (squad/NPC EntityState capture)
    FAULT_APPLY,            // motion/transform apply (reserved for later wiring)
    FAULT_OTHER,            // unclassified guarded entry (reserved)
    FAULT_OP_COUNT
};

// Pure throttle decision (no I/O - unit-testable): given this op's running hit
// count, the current wall-clock ms, and the op's last-logged timestamp, decide
// whether to emit a line NOW. Rule: ALWAYS log the first hit (count==1), then at
// most once per intervalMs. Updates *lastMs when it returns true. Unsigned
// subtraction tolerates the midnight wrap of wallClockMs within one interval.
inline bool faultShouldLog(unsigned int count, unsigned long nowMs,
                           unsigned long* lastMs, unsigned long intervalMs) {
    if (!lastMs) return count <= 1;
    if (count <= 1) { *lastMs = nowMs; return true; }
    if ((nowMs - *lastMs) >= intervalMs) { *lastMs = nowMs; return true; }
    return false;
}

// Bump op's counter and emit a throttled "[engine] FAULT op=<name> n=<count>"
// line. Call from an __except block. Main-thread only; never throws.
void noteFault(FaultOp op);

// Stable token for op (the oracle contract); "unknown" for out-of-range.
const char* faultOpName(FaultOp op);

// Running fault count for op (diagnostic / test). 0 for out-of-range.
unsigned int faultCount(FaultOp op);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_FAULTS_H
