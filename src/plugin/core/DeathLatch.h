// DeathLatch.h - down/death latch carry policy across a hand re-key (pure, zero
// game/Win32 deps).
//
// A driven body that the peer has pinned DOWN or DEAD carries that state as a
// reliable latch on its per-hand Driven record (deathLatched / koLatched, set by
// EVT_DEATH / EVT_KNOCKOUT). When the OWNER re-containers the body (recruit or a
// squad-tab move) it starts streaming under a NEW hand key, and the peer re-keys
// its local record - dropping the OLD key's Driven record. If the latch is not
// carried onto the new key, a corpse that re-containers loses its pin and the
// peer's local AI / KO-timer stands it back up: the "dead on one game, alive on
// the other" desync observed in the 2026-07-15 bone-dog fight (serial 3332275456
// died under container 121, then an EVT_SQUAD_MOVE re-keyed it and un-pinned the
// body).
//
// The rule is a monotone OR-merge: the new key's latch must be at least as
// pinned as the old key's. OR (rather than overwrite) so a latch ALREADY present
// on the new key - e.g. a fresh EVT_DEATH that beat the re-key edge - is never
// cleared. Tested in src/prototest/main.cpp (testDeathRekey); used by
// Replicator::rekeyPeerBody in ReplicatorSpawn.cpp.

#ifndef COOP_DEATH_LATCH_H
#define COOP_DEATH_LATCH_H

namespace coop {

struct LatchState {
    bool death; // EVT_DEATH pinned the body down permanently
    bool ko;    // EVT_KNOCKOUT pinned the body down
    bool down;  // body is currently held in ragdoll (Stage 2 downApplied)
    LatchState() : death(false), ko(false), down(false) {}
    LatchState(bool d, bool k, bool dn) : death(d), ko(k), down(dn) {}
};

// Merge the OLD hand's latch into the NEW hand's on a re-key. Monotone: never
// loses a down/death that was pinned on either key.
inline LatchState rekeyCarryLatch(const LatchState& oldK, const LatchState& newK) {
    return LatchState(newK.death || oldK.death,
                      newK.ko    || oldK.ko,
                      newK.down  || oldK.down);
}

} // namespace coop

#endif // COOP_DEATH_LATCH_H
