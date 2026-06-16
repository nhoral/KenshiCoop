// MainThreadQueue - the thread bridge (Milestone 4).
//
// Kenshi's engine is single-threaded and NOT thread-safe. The network thread
// must therefore never touch game memory directly. Instead it pushes small PODs
// (copied by value) into this queue, and the game's main-thread tick hook drains
// them at a known-safe point.
//
// Built for the VS2010 (v100) toolchain: no std::mutex/std::thread (those need
// VS2012+). We use a Win32 CRITICAL_SECTION, which is always available.

#ifndef KENSHICOOP_MAINTHREADQUEUE_H
#define KENSHICOOP_MAINTHREADQUEUE_H

#include <windows.h>
#include <deque>
#include "../netproto/Protocol.h"

namespace coop {

class MainThreadQueue {
public:
    MainThreadQueue()  { InitializeCriticalSection(&cs_); }
    ~MainThreadQueue() { DeleteCriticalSection(&cs_); }

    // Called from the NET thread. Copies the POD in under the lock.
    void push(const PlayerStatePacket& p) {
        EnterCriticalSection(&cs_);
        q_.push_back(p);
        LeaveCriticalSection(&cs_);
    }

    // Called from the MAIN (game) thread inside the tick hook. Moves all pending
    // items into 'out' (which should be empty on entry) and leaves the internal
    // queue empty, minimizing time spent holding the lock.
    void drain(std::deque<PlayerStatePacket>& out) {
        EnterCriticalSection(&cs_);
        out.swap(q_);
        LeaveCriticalSection(&cs_);
    }

    // NET thread: enqueue one received NPC transform (Phase 2).
    void pushNpc(const NpcStateEntry& e) {
        EnterCriticalSection(&cs_);
        npc_.push_back(e);
        LeaveCriticalSection(&cs_);
    }

    // MAIN thread: drain all pending NPC transforms.
    void drainNpc(std::deque<NpcStateEntry>& out) {
        EnterCriticalSection(&cs_);
        out.swap(npc_);
        LeaveCriticalSection(&cs_);
    }

private:
    CRITICAL_SECTION cs_;
    std::deque<PlayerStatePacket> q_;
    std::deque<NpcStateEntry>     npc_;

    MainThreadQueue(const MainThreadQueue&);
    MainThreadQueue& operator=(const MainThreadQueue&);
};

} // namespace coop

#endif // KENSHICOOP_MAINTHREADQUEUE_H
