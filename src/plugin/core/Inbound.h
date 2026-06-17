// Inbound - the net->game thread bridge.
//
// Kenshi's engine is single-threaded and NOT thread-safe, so the background net
// thread must never touch game memory. It only pushes small PODs (copied by
// value) into these queues; the main-thread tick hook drains them at a known-safe
// point. Built for VS2010 (v100): a Win32 CRITICAL_SECTION (std::mutex needs
// VS2012+).

#ifndef KENSHICOOP_INBOUND_H
#define KENSHICOOP_INBOUND_H

#include <windows.h>
#include <deque>
#include "../../netproto/Wire.h"

namespace coop {

// One received entity plus the network id of the peer that owns it. The owner
// tag lets the receiver apply the right authority rule (drive a peer's entity,
// never one we own).
struct InboundEntity {
    u32         ownerId;
    EntityState e;
};

class Inbound {
public:
    Inbound()  { InitializeCriticalSection(&cs_); }
    ~Inbound() { DeleteCriticalSection(&cs_); }

    // NET thread: a peer joined (id) / a peer left (id, or OWNER_ID_ALL).
    void pushConnect(u32 id) {
        EnterCriticalSection(&cs_); conn_.push_back(id); LeaveCriticalSection(&cs_);
    }
    void pushLeave(u32 id) {
        EnterCriticalSection(&cs_); leave_.push_back(id); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received entity transform, owner-tagged.
    void pushEntity(u32 ownerId, const EntityState& e) {
        InboundEntity ie; ie.ownerId = ownerId; ie.e = e;
        EnterCriticalSection(&cs_); ent_.push_back(ie); LeaveCriticalSection(&cs_);
    }

    // MAIN thread: move all pending items into 'out' (empty on entry).
    void drainConnects(std::deque<u32>& out) {
        EnterCriticalSection(&cs_); out.swap(conn_); LeaveCriticalSection(&cs_);
    }
    void drainLeaves(std::deque<u32>& out) {
        EnterCriticalSection(&cs_); out.swap(leave_); LeaveCriticalSection(&cs_);
    }
    void drainEntities(std::deque<InboundEntity>& out) {
        EnterCriticalSection(&cs_); out.swap(ent_); LeaveCriticalSection(&cs_);
    }

private:
    CRITICAL_SECTION          cs_;
    std::deque<u32>           conn_;
    std::deque<u32>           leave_;
    std::deque<InboundEntity> ent_;

    Inbound(const Inbound&);
    Inbound& operator=(const Inbound&);
};

} // namespace coop

#endif // KENSHICOOP_INBOUND_H
