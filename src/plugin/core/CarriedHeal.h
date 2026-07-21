// CarriedHeal.h - owner-side carried-body reconcile policy (pure, zero
// game/Win32 deps).
//
// Carry edges are CARRIER-authored (EVT_PICKUP_BODY / EVT_DROP_BODY; the host
// authors them for world-NPC carriers, doctrine 28), and applyTargets never
// drives a body in ownHands_ - so an OWNED body sitting on someone's shoulder
// has no self-heal path of its own: if the host's EVT_DROP_BODY fails to apply
// on the local carrier copy (resolve fail, carrier re-containered, or the local
// sim never executed the pickup) the owner stays carried indefinitely
// (SYNC_GAPS 16b, 2026-07-11 field report: the join PC, KO'd and hauled by a
// cross-visible enemy pack, was put down in the host's world but stayed carried
// on the join).
//
// The rule: the owner of a carried body reconciles its LOCAL isBeingCarried
// against the carrier's streamed TASK_CARRY_BODY claim. While some live stream
// still claims the body as its carry subject, the local carry is believed. When
// NO stream claims it, a debounce window arms (stance samples ride the lossy
// batch - a one-tick gap must never rip a genuine carry apart, the carry-side
// carryNoSeeTick lesson); only after the window elapses does the owner release
// the body locally. Firing resets the window, so a release that fails to take
// (carrier not found this pass) re-arms and retries a full window later.
// Tested in src/prototest/main.cpp (testCarriedHeal); used by
// Replicator::applyTargets in ReplicatorDrive.cpp.

#ifndef COOP_CARRIED_HEAL_H
#define COOP_CARRIED_HEAL_H

namespace coop {

enum CarriedHealAct {
    CARRIED_HEAL_NONE = 0, // in sync (or debounce still running) - do nothing
    CARRIED_HEAL_ARM  = 1, // divergence first seen - debounce window armed
    CARRIED_HEAL_FIRE = 2  // window elapsed - execute the local release now
};

// One reconcile step for one owned body. beingCarried = the LOCAL
// Character::isBeingCarried truth; streamClaims = some live streamed row still
// reports TASK_CARRY_BODY with this body as subject; *noSeeTick = the per-hand
// debounce anchor (0 = disarmed), updated in place exactly like the carrier-side
// carryNoSeeTick.
inline CarriedHealAct carriedHealStep(bool beingCarried, bool streamClaims,
                                      unsigned long nowMs, unsigned long dropMs,
                                      unsigned long* noSeeTick) {
    if (!beingCarried || streamClaims) { // in sync: disarm any pending window
        *noSeeTick = 0;
        return CARRIED_HEAL_NONE;
    }
    if (*noSeeTick == 0) {              // first unclaimed tick: arm, don't act
        *noSeeTick = nowMs;
        return CARRIED_HEAL_ARM;
    }
    if ((nowMs - *noSeeTick) > dropMs) { // window elapsed: release + re-arm
        *noSeeTick = 0;
        return CARRIED_HEAL_FIRE;
    }
    return CARRIED_HEAL_NONE;           // window still running
}

} // namespace coop

#endif // COOP_CARRIED_HEAL_H
