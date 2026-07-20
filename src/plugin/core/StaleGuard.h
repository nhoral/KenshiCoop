// StaleGuard.h - per-sender stale-row guard for the symmetric state channels
// (pure, zero game/Win32 deps).
//
// The faction (protocol 24), door (26) and placed-building-door (28) channels
// are SYMMETRIC: both clients publish rows for the SAME key, each stamping its
// own independent seq counter (facSeqOut_/doorSeqOut_/bdoorSeqOut_). The
// original guard compared the incoming seq against a single high-water mark
// shared by ALL senders of the row, so whichever side happened to run the
// higher counter starved the other side's fresh rows as "stale" - one player
// silently stopped seeing the other's changes on that row (fix 2026-07-19).
// The guard is therefore keyed by the packet's ownerId (already on the wire in
// all three packets): each sender is judged only against its own counter.
//
// Contract (mirrors the seq producers in ReplicatorChannels.cpp):
//   * seq counters are per-sender, monotonically increasing, starting at 1
//     (0 is reserved: "nothing seen yet from this sender").
//   * a row is APPLIED exactly when its seq is newer than the last one
//     applied from THIS sender; duplicates (safety resends) and reordered
//     stragglers drop.
//   * per-sender progress is independent: a slow sender's seq=1 must land
//     even after a fast sender pushed seq=500 on the same row.
// Tested in src/prototest/main.cpp (testStaleGuard); used by
// Replicator::applyFactions / applyDoors / applyBuildDoors in
// ReplicatorChannels.cpp.

#ifndef COOP_STALE_GUARD_H
#define COOP_STALE_GUARD_H

#include <map>

namespace coop {

// Returns true if a row stamped (ownerId, seq) must be applied, advancing the
// per-sender high-water mark; false drops it as stale/duplicate. seqSeen is
// the row's per-sender map (FacRow/DoorRow/BdoorRow::seqSeen).
inline bool staleRowAccept(std::map<unsigned int, unsigned int>& seqSeen,
                           unsigned int ownerId, unsigned int seq) {
    unsigned int& seen = seqSeen[ownerId];
    if (seen != 0 && seq <= seen) return false; // stale row (this sender)
    seen = seq;
    return true;
}

} // namespace coop

#endif // COOP_STALE_GUARD_H
