// SyncContext - the per-tick call environment handed to a replicated channel
// (Phase 6).
//
// Every channel publish/apply used to take its own loose slice of the same
// environment: publish(GameWorld*, NetLink&, ownerId), apply(GameWorld*,
// Inbound&). Bundling that slice into ONE value gives the change-gated channels
// a single uniform signature -- publish(const SyncContext&) / apply(const
// SyncContext&) -- which is the precondition for driving them from a channel
// registry (a table of member-function pointers needs one call shape).
//
// It carries only the tick ENVIRONMENT, never persistent channel state: the
// world + the two transports, plus the local owner id and host role. Longer-
// lived shared state (ownHands_/ownRanks_/tab census, per-channel baselines and
// seq counters) stays where it belongs -- on the Replicator -- so the context
// is a cheap by-reference POD rebuilt each tick.
//
//   net : valid on publish paths (send half); null on apply-only calls.
//   in  : valid on apply paths (drain half); null on publish-only calls.
//
// nowMs() is deliberately NOT cached here: each channel still reads its own
// clock at its own point in the tick, so the sample/resend timing is byte-for-
// byte what it was before the context existed.

#ifndef KENSHICOOP_SYNC_CONTEXT_H
#define KENSHICOOP_SYNC_CONTEXT_H

class GameWorld;

namespace coop {

class NetLink;
class Inbound;

struct SyncContext {
    GameWorld*   gw;      // the live world (interest sampling / engine writes)
    NetLink*     net;     // send half; null on apply-only calls
    Inbound*     in;      // drain half; null on publish-only calls
    unsigned int localId; // this client's owner id (== the old `ownerId` arg)
    bool         isHost;  // host role (host-authoritative channel direction)
};

} // namespace coop

#endif // KENSHICOOP_SYNC_CONTEXT_H
