// EngineCaps - owned capability registry over the resolved engine function-ptr
// table (Phase 5d).
//
// The adapter resolves ~100 engine entry points once at load
// (EngineInternal.cpp resolve(), each `g_xFn = GetRealAddress(&Class::method)`).
// Historically each feature's health was implied by a scattered, hand-written
// `if (!g_aFn || !g_bFn) logErr("... (X sync off)")` right after its group -
// inconsistent phrasing, easy to forget, and invisible to the rest of the code
// (callers only saw a null pointer). This module turns that into an OWNED,
// TABLE-DRIVEN, queryable registry:
//
//   * The set of engine capabilities (CAP_*) the plugin depends on.
//   * A data table of {slot, name, cap, required} rows naming which resolved
//     pointers back each capability (the memberPtr + typed cast necessarily
//     STAYS at the inline GetRealAddress call site - a member-function pointer
//     is a heterogeneous compile-time type that cannot live in a uniform C++03
//     array - so the row keys on the resolved `void**` slot instead).
//   * A pure evaluation (capEvaluate) that folds the table into a per-capability
//     availability array, and a fail-closed query (capAvailable) that reports
//     false until resolve() has actually evaluated the table.
//
// Fail-closed: an unresolved REQUIRED row marks its whole capability
// unavailable and emits a NAMED "[engine] CAP-MISS op=<name> cap=<cap>" line the
// oracles can watch, so a missing/incompatible runtime image degrades to a
// dependent-feature-off with a named cause instead of a silent null deref.
//
// Pure logic (capEvaluate/capName/capRowResolved/capCoreOk) lives inline with no
// game or logger dependency so the unit layer (prototest) can exercise it
// directly against synthetic rows.

#ifndef KENSHICOOP_ENGINE_CAPS_H
#define KENSHICOOP_ENGINE_CAPS_H

namespace coop {
namespace engine {

// Engine capabilities the plugin depends on. Order is a diagnostic contract
// (capName tokens are parsed by humans/oracles); append new caps before
// CAP_COUNT, never reorder. CAP_HAND_RESOLVE is the CORE capability: without the
// hand->object resolve path nothing else can run (capCoreOk keys on it).
enum Capability {
    CAP_SAVELOAD = 0,   // SaveManager get/load (coordinated save/load)
    CAP_SAVEPATH,       // getCurrentGame/getSavePath (save-path discovery)
    CAP_HAND_RESOLVE,   // hand::getCharacter + 5-arg hand ctor (CORE)
    CAP_NPC_STREAM,     // getCharactersWithinSphere (NPC interest streaming)
    CAP_LIMB,           // amputate/crushLimb (limb-loss replication)
    CAP_STATS,          // CharStats::getStatRef (stats sync)
    CAP_CARRY,          // pickupObject/dropCarriedObject (carry sync)
    CAP_FURNITURE,      // setBedMode/setPrisonMode (occupancy sync)
    CAP_CHAIN,          // setChainedMode (chained/pole sync)
    CAP_SHACKLE,        // getChainedModeShackles (shackle-item read)
    CAP_SLAVE,          // Character::isSlave (jail-probe slave read)
    CAP_STEALTH,        // setStealthMode/notifyICanSeeYouSneaking (stealth sync)
    CAP_CAMERA,         // CameraClass getCenter/isInitialised (camera interest)
    CAP_SPEED,          // setGameSpeed/userPause (consensus game-speed)
    CAP_QUIET_SPEED,    // setFrameSpeedMultiplier (quiet speed apply)
    CAP_DOOR,           // DoorStuff isOpen/openDoor/closeDoor (door sync)
    CAP_BUILD,          // construction-progress setters (build sync)
    CAP_MACHINE,        // machine power/production levers (prod sync)
    CAP_TIME,           // game-clock reads (time sync)
    CAP_WALLET,         // platoon/money accessors (money sync)
    CAP_FACTION,        // faction-relation get/set (faction sync)
    CAP_COMBAT_ESCALATE,// Character::attackTarget (combat force-escalation)
    CAP_COUNT
};

// One capability-table row: the resolved pointer SLOT (address of a g_xFn
// global, as void**), a stable NAME token for logs/oracles, the CAP it backs,
// and whether it is REQUIRED (an unresolved required row fails its capability;
// an unresolved optional row is tracked but does not gate the capability).
struct CapRow {
    void**      slot;
    const char* name;
    Capability  cap;
    bool        required;
};

// A row is resolved iff its slot exists and holds a non-null pointer.
inline bool capRowResolved(const CapRow& r) {
    return r.slot != 0 && *r.slot != 0;
}

// Stable token for a capability (the oracle/diagnostic contract); "unknown" for
// out-of-range. Keep in lockstep with the Capability enum order.
inline const char* capName(Capability c) {
    static const char* const kNames[CAP_COUNT] = {
        "saveload", "savepath", "hand_resolve", "npc_stream", "limb", "stats",
        "carry", "furniture", "chain", "shackle", "slave", "stealth", "camera",
        "speed", "quiet_speed", "door", "build", "machine", "time", "wallet",
        "faction", "combat_escalate"
    };
    if (c < 0 || c >= CAP_COUNT) return "unknown";
    return kNames[c];
}

// Pure fold of the row table into a per-capability availability array
// (availOut must have CAP_COUNT entries). A capability is AVAILABLE iff it has
// at least one REQUIRED row AND every one of its required rows resolved. This is
// fail-closed: a capability with no required rows, or any required row missing,
// comes out false. Optional rows never flip availability.
inline void capEvaluate(const CapRow* rows, int n, bool* availOut) {
    int i;
    for (i = 0; i < CAP_COUNT; ++i) availOut[i] = false;      // fail-closed base
    // Two passes so a later optional row can't "resolve" a cap a required row failed.
    for (i = 0; i < n; ++i) {
        if (rows[i].required && rows[i].cap >= 0 && rows[i].cap < CAP_COUNT)
            availOut[rows[i].cap] = true;                     // provisionally on
    }
    for (i = 0; i < n; ++i) {
        if (rows[i].required && !capRowResolved(rows[i]) &&
            rows[i].cap >= 0 && rows[i].cap < CAP_COUNT)
            availOut[rows[i].cap] = false;                    // any miss = off
    }
}

// The runtime image is usable at all only if the CORE hand-resolve capability
// came up; everything downstream funnels through it. A false here means the
// mapped executable is fundamentally incompatible (unsupported fingerprint).
inline bool capCoreOk(const bool* avail) {
    return avail && avail[CAP_HAND_RESOLVE];
}

// Fail-closed runtime query: true only after resolve() has evaluated the table
// AND the capability's required rows all resolved. Defined in EngineInternal.cpp
// (reads the owned availability array).
bool capAvailable(Capability c);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_CAPS_H
