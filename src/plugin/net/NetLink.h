// NetLink - owns ENet on a dedicated background thread.
//
// Threading contract:
//   * The net thread EXCLUSIVELY owns the ENetHost; the game thread never
//     touches ENet.
//   * Inbound events (peer connect/leave, received EntityState) are handed to
//     the game thread via the Inbound queue.
//   * The game thread publishes this peer's owned entities via setOwnedEntities();
//     the net thread reads the latest snapshot and transmits it each tick.
//
// VS2010 (v100) compatible: Win32 threads + CRITICAL_SECTION (no std::thread).

#ifndef KENSHICOOP_NETLINK_H
#define KENSHICOOP_NETLINK_H

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <enet/enet.h>

#include "../../netproto/Wire.h"
#include "../core/Inbound.h"

namespace coop {

class NetLink {
public:
    NetLink();
    ~NetLink();

    // Start as host on 'port' / as client to 'ip:port'. Inbound events go to
    // 'inbound'. Returns false if ENet init or the thread launch failed.
    bool startHost(int port, Inbound* inbound);
    bool startClient(const std::string& ip, int port, Inbound* inbound);
    void stop();

    // MAIN thread: publish this peer's owned entities (copied under lock). The
    // net thread re-broadcasts the latest snapshot each tick. Pass count 0 to
    // publish nothing.
    void setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count);

    // MAIN thread: queue a reliable one-shot event (KO/death/revive). The net thread
    // drains and sends it on the RELIABLE channel next tick (host broadcasts to all
    // peers; client sends to the host). Thread-safe; copied under lock.
    void queueEvent(const EventPacket& ev);

    // MAIN thread: queue a reliable container-contents snapshot (Phase 4a). The net
    // thread serializes [InvSnapshotHeader][InvItemEntry*count] and sends it on the
    // RELIABLE channel next tick. count may be 0 ("container now empty"). Copied
    // under lock; only enqueued on content-change so the reliable channel stays cheap.
    void queueInvSnapshot(u32 ownerId, const u32 cHand[5],
                          const InvItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item snapshot (Phase W1). The net thread
    // serializes [WorldItemSnapshotHeader][WorldItemEntry*count] and sends it on the
    // RELIABLE channel next tick. Only enqueued for new/changed ground items, so the
    // channel stays quiet for a settled world. Copied under lock.
    void queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item cull (Phase W1) - the netIds of ground
    // items that left the world / interest sphere. [WorldItemRemoveHeader][u32*count].
    void queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count);

    // MAIN thread: queue a reliable conservation DROP intent (Phase W2). A fixed-size POD
    // (like an event), sent once on the RELIABLE channel; the peer relocates its own copy
    // of the weapon to the ground. Copied under lock.
    void queueWorldDrop(const WorldDropPacket& pkt);

    // MAIN thread: queue a reliable conservation PICKUP intent (Phase W3), mirror of the
    // drop. The peer re-homes its tracked ground copy back into the character's bag.
    void queueWorldPickup(const WorldPickupPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative medical snapshot (phase 2,
    // player-squad only). Change-gated by the caller so the channel stays quiet.
    void queueMedical(const MedicalPacket& pkt);

    // MAIN thread: queue a reliable treatment delta (first aid administered on a
    // driven copy, forwarded to the body's owner).
    void queueTreatment(const TreatmentPacket& pkt);

    // MAIN thread: queue a reliable game-speed packet (REQ join->host, SET
    // host->join). Change-gated by the caller; pkt.type selects the direction.
    void queueSpeed(const SpeedPacket& pkt);

    // MAIN thread: queue a reliable owner-authoritative character-stats snapshot
    // (protocol 17, player-squad only). Change-gated by the caller.
    void queueStats(const StatsPacket& pkt);

    // Debug WAN simulation. When delayMs > 0, received entity batches are held in a
    // net-thread queue and delivered to the game thread only after delayMs +/- jitter
    // has elapsed (lossPct of them are dropped outright). Must be called before
    // startHost/startClient. All-zero = disabled (immediate delivery). See Config.
    void setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct);

    bool isRunning() const { return running_ != 0; }
    u32  localId()   const { return myId_; } // host = 0; client = id from WELCOME

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();

    // Net-thread-only: route a received entity through the WAN sim (delay/drop) when
    // enabled, else deliver immediately. flushDelayed() releases matured entries.
    void deliverEntity(u32 ownerId, const EntityState& e);
    void flushDelayed();

    bool        isHost_;
    std::string ip_;
    int         port_;

    ENetHost*   enetHost_;   // net thread only
    ENetPeer*   serverPeer_; // client only; net thread only
    Inbound*    inbound_;

    CRITICAL_SECTION         outCs_;
    std::vector<EntityState> out_;
    u32                      outOwner_;
    bool                     haveOut_;
    // Reliable events queued by the main thread, drained + sent by the net thread.
    // Guarded by outCs_ (same publish lock as out_).
    std::vector<EventPacket> outEvents_;
    // Reliable container-contents snapshots queued by the main thread, drained +
    // serialized by the net thread. Variable-length, so each carries its own item
    // list. Guarded by outCs_.
    struct OutInv {
        u32                       ownerId;
        u32                       cHand[5];
        std::vector<InvItemEntry> items;
    };
    std::vector<OutInv>      outInv_;
    // Reliable world-item snapshots / culls queued by the main thread (Phase W1),
    // drained + serialized by the net thread. Guarded by outCs_.
    struct OutWorldItems { u32 ownerId; std::vector<WorldItemEntry> items; };
    struct OutWorldRemove { u32 ownerId; std::vector<u32> netIds; };
    std::vector<OutWorldItems>  outWorldItems_;
    std::vector<OutWorldRemove> outWorldRemove_;
    // Reliable conservation DROP intents (Phase W2), fixed-size PODs. Guarded by outCs_.
    std::vector<WorldDropPacket> outWorldDrops_;
    std::vector<WorldPickupPacket> outWorldPickups_;
    // Reliable medical snapshots + treatment deltas (phase 2). Guarded by outCs_.
    std::vector<MedicalPacket>   outMedical_;
    std::vector<TreatmentPacket> outTreatments_;
    // Reliable game-speed REQ/SET packets (consensus speed sync). Guarded by outCs_.
    std::vector<SpeedPacket>     outSpeed_;
    // Reliable character-stats snapshots (protocol 17). Guarded by outCs_.
    std::vector<StatsPacket>     outStats_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    u32           myId_;

    // WAN sim config (set before launch; read-only on the net thread thereafter).
    unsigned int  simDelayMs_;
    unsigned int  simJitterMs_;
    unsigned int  simLossPct_;
    // Held-back inbound entities awaiting their simulated arrival time. Net-thread
    // only (received and flushed on the same thread), so it needs no lock.
    struct Delayed { DWORD releaseTick; u32 ownerId; EntityState e; };
    std::deque<Delayed> delayed_;

    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};

} // namespace coop

#endif // KENSHICOOP_NETLINK_H
