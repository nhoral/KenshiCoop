// BuildOwnership.h - pure policy for ownership of protocol-27 placed builds.
//
// A peer-side copy is created from a runtime placement key.  The factory mint
// alone does not make that copy a player-owned building, so the receiver must
// claim it through the same engine state write used by property deeds.  Runtime
// building hands are client-local, however, and must never leak into the deed
// channel, whose raw-hand identity is valid only for save-resident buildings.

#ifndef COOP_BUILD_OWNERSHIP_H
#define COOP_BUILD_OWNERSHIP_H

namespace coop {

inline bool peerBuildNeedsOwnership(int minted, bool removed,
                                    bool ownershipApplied) {
    return minted == 1 && !removed && !ownershipApplied;
}

inline bool deedMayPublishRawHand(bool sessionPlaced) {
    return !sessionPlaced;
}

} // namespace coop

#endif // COOP_BUILD_OWNERSHIP_H
