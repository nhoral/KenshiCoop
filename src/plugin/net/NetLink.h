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
#include <map>
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
    // keyKind (protocol 34): 0 = cKey is the raw container hand, 1 = cKey is the
    // protocol-27 placer key of a session-placed building (receiver translates).
    void queueInvSnapshot(u32 ownerId, u8 keyKind, const u32 cKey[5],
                          const InvItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item snapshot (Phase W1). The net thread
    // serializes [WorldItemSnapshotHeader][WorldItemEntry*count] and sends it on the
    // RELIABLE channel next tick. Only enqueued for new/changed ground items, so the
    // channel stays quiet for a settled world. Copied under lock.
    void queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count);

    // MAIN thread: queue a reliable world-item cull (Phase W1) - the netIds of ground
    // items that left the world / interest sphere. [WorldItemRemoveHeader][u32*count].
    void queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count);

    // MAIN thread: queue a reliable wide-radius NPC existence census (protocol
    // 36, host -> join, 1 Hz). 'hands' is count*5 u32s (readObjectHand layout);
    // 'pos' is count*3 floats (v38: host position per row, park authority).
    // [NpcCensusHeader][u32 hand[5] * count][f32 pos[3] * count].
    void queueNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                        unsigned int count);

    // MAIN thread: queue a reliable conservation DROP intent (Phase W2). A fixed-size POD
    // (like an event), sent once on the RELIABLE channel; the peer relocates its own copy
    // of the weapon to the ground. Copied under lock.
    void queueWorldDrop(const WorldDropPacket& pkt);

    // MAIN thread: queue a reliable conservation PICKUP intent (Phase W3), mirror of the
    // drop. The peer re-homes its tracked ground copy back into the character's bag.
    void queueWorldPickup(const WorldPickupPacket& pkt);

    // MAIN thread: queue a reliable cross-owner TRANSFER intent (protocol 37). The peer
    // relocates the real item between its own copies of the two containers.
    void queueInvXfer(const InvXferPacket& pkt);

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

    // MAIN thread: queue a reliable owner-authoritative per-tab wallet snapshot
    // (protocol 22). Change-gated by the caller (the PKT_STATS pacing).
    void queueMoney(const MoneyPacket& pkt);
    void queueFaction(const FactionPacket& pkt);
    void queueTime(const TimePacket& pkt);
    void queueDoor(const DoorPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative machine state row
    // (protocol 33). Change-gated + safety-resent by the caller.
    void queueProd(const ProdPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative known-research row
    // (protocol 38). First-sight sent + safety-resent by the caller.
    void queueResearch(const ResearchPacket& pkt);
    // MAIN thread: queue a reliable host-authoritative bounty/crime row
    // (protocol 45). Change-gated + safety-resent by the caller (host only).
    void queueBounty(const BountyPacket& pkt);
    void queueBuildPlace(const BuildPlacePacket& pkt);
    void queueBuildState(const BuildStatePacket& pkt);
    void queueBuildDoor(const BuildDoorPacket& pkt);
    void queueBuildRemove(const BuildRemovePacket& pkt);

    // MAIN thread: queue an UNRELIABLE stealth detection-map snapshot (protocol
    // 20, host -> the sneaker's owner). Latest wins; change-gated + throttled by
    // the caller, so loss just delays an arrow update one snapshot.
    void queueStealth(const StealthPacket& pkt);

    // MAIN thread: queue an UNRELIABLE camera hint (protocol 43, join -> host,
    // ~1 Hz). Latest wins; loss just delays the anchor one hint.
    void queueCamHint(const CamHintPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn query (protocol 21, join ->
    // host). Debounced per hand by the caller.
    void queueSpawnReq(const SpawnReqPacket& pkt);

    // MAIN thread: queue a reliable runtime-spawn description (protocol 21,
    // host -> join). Reply-cached by the caller.
    void queueSpawnInfo(const SpawnInfoPacket& pkt);

    // MAIN thread: coordinated-save packets (protocol 31). REQ join -> host
    // (a suppressed local save forwarded for arbitration); BEGIN/FILE/DONE
    // host -> join (the paced folder transfer; FILE is variable-length:
    // header + relative path + payload, serialized by the net thread); ACK
    // join -> host (staged save verified + committed). All CH_RELIABLE - the
    // ordered stream is what makes the chunk protocol stateless per chunk.
    void queueSaveReq(const SaveReqPacket& pkt);
    void queueSaveBegin(const SaveBeginPacket& pkt);
    void queueSaveFile(const SaveFileHeader& hdr, const char* relPath,
                       const unsigned char* data, unsigned int dataLen);
    void queueSaveDone(const SaveDoneHeader& hdr, const u32* crcs, unsigned int count);
    void queueSaveAck(const SaveAckPacket& pkt);

    // MAIN thread: coordinated-load packets (protocol 32). GO host -> join
    // (load this save now, fingerprint attached); REQ join -> host (a
    // suppressed local load forwarded for arbitration); NACK join -> host
    // (copy missing/diverged - answer with a SaveXfer). All CH_RELIABLE.
    void queueLoadGo(const LoadGoPacket& pkt);
    void queueLoadReq(const LoadReqPacket& pkt);
    void queueLoadNack(const LoadNackPacket& pkt);

    // Debug WAN simulation. When delayMs > 0, received entity batches are held in a
    // net-thread queue and delivered to the game thread only after delayMs +/- jitter
    // has elapsed (lossPct of them are dropped outright). Must be called before
    // startHost/startClient. All-zero = disabled (immediate delivery). See Config.
    void setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct);

    // Steam P2P transport: tunnel the ENet protocol over Steam P2P to 'peerSteamId'
    // (steamid64) instead of UDP. The wire protocol, channels, reliability and
    // reconnect logic are unchanged - only the datagram pipe differs (ENet socket
    // hooks installed on the net thread; MTU clamped to Steam's 1200-byte
    // unreliable ceiling). Must be called before startHost/startClient. 0 = UDP.
    void setSteamTransport(unsigned long long peerSteamId);

    // MAIN thread: advance this peer's session epoch (protocol 44). Called on
    // every session-reset edge (coordinated world reload, connect/disconnect
    // teardown). Subsequent entity batches carry the new epoch, so the peer
    // drops any still-in-flight batch from the prior session; the pending owned-
    // entity snapshot is also dropped so a stale one is not re-stamped with the
    // new epoch and mistaken for fresh. Thread-safe (InterlockedIncrement + the
    // publish lock for the snapshot clear).
    void bumpSessionEpoch();

    bool isRunning() const { return running_ != 0; }
    // host = 0; client = id from WELCOME. myId_ is written by the NET thread when
    // the WELCOME arrives and read here on the MAIN thread, so it is a volatile
    // LONG written via InterlockedExchange; an aligned 32-bit volatile read is
    // atomic on x86/x64 and the volatile bars the compiler from caching a stale
    // value (Phase 4: myId_ cross-thread safety).
    u32  localId()   const { return (u32)myId_; }

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();

    // Net-thread-only: route a received entity through the WAN sim (delay/drop) when
    // enabled, else deliver immediately. flushDelayed() releases matured entries.
    void deliverEntity(u32 ownerId, u32 sendMs, const EntityState& e);
    void flushDelayed();

    // Net-thread-only (protocol 44): gate an incoming entity batch by its session
    // epoch. Returns false (drop) if 'epoch' is older than the newest accepted
    // from 'ownerId'; otherwise records it and returns true. epochSeen_ is reset
    // at every connection edge so a reconnecting peer restarting at epoch 0 is
    // never locked out.
    bool acceptEpoch(u32 ownerId, u32 epoch);

    bool        isHost_;
    std::string ip_;
    int         port_;

    ENetHost*   enetHost_;   // net thread only
    ENetPeer*   serverPeer_; // client only; net thread only
    Inbound*    inbound_;

    CRITICAL_SECTION         outCs_;
    std::vector<EntityState> out_;
    u32                      outOwner_;
    u32                      outStampMs_; // capture-time stamp for the batch header (v35)
    bool                     haveOut_;
    // Reliable events queued by the main thread, drained + sent by the net thread.
    // Guarded by outCs_ (same publish lock as out_).
    std::vector<EventPacket> outEvents_;
    // Reliable container-contents snapshots queued by the main thread, drained +
    // serialized by the net thread. Variable-length, so each carries its own item
    // list. Guarded by outCs_.
    struct OutInv {
        u32                       ownerId;
        u8                        keyKind; // protocol 34: 0 raw hand, 1 placer key
        u32                       cKey[5];
        std::vector<InvItemEntry> items;
    };
    std::vector<OutInv>      outInv_;
    // Reliable world-item snapshots / culls queued by the main thread (Phase W1),
    // drained + serialized by the net thread. Guarded by outCs_.
    struct OutWorldItems { u32 ownerId; std::vector<WorldItemEntry> items; };
    struct OutWorldRemove { u32 ownerId; std::vector<u32> netIds; };
    std::vector<OutWorldItems>  outWorldItems_;
    std::vector<OutWorldRemove> outWorldRemove_;
    // Reliable NPC existence census (protocol 36): 5xu32 hands, flat. Guarded
    // by outCs_. 1 Hz from the host, so at most a couple pending at once.
    struct OutNpcCensus { u32 ownerId; std::vector<u32> hands; std::vector<float> pos; };
    std::vector<OutNpcCensus> outNpcCensus_;
    // Reliable conservation DROP intents (Phase W2), fixed-size PODs. Guarded by outCs_.
    std::vector<WorldDropPacket> outWorldDrops_;
    std::vector<WorldPickupPacket> outWorldPickups_;
    // Reliable cross-owner transfer intents (protocol 37). Guarded by outCs_.
    std::vector<InvXferPacket>   outInvXfers_;
    // Reliable medical snapshots + treatment deltas (phase 2). Guarded by outCs_.
    std::vector<MedicalPacket>   outMedical_;
    std::vector<TreatmentPacket> outTreatments_;
    // Reliable game-speed REQ/SET packets (consensus speed sync). Guarded by outCs_.
    std::vector<SpeedPacket>     outSpeed_;
    // Reliable character-stats snapshots (protocol 17). Guarded by outCs_.
    std::vector<StatsPacket>     outStats_;
    // Reliable per-tab wallet snapshots (protocol 22). Guarded by outCs_.
    std::vector<MoneyPacket>     outMoney_;
    std::vector<FactionPacket>   outFaction_;
    std::vector<TimePacket>      outTime_;
    std::vector<DoorPacket>      outDoor_;
    // Reliable machine state rows (protocol 33). Guarded by outCs_.
    std::vector<ProdPacket>      outProd_;
    // Reliable known-research rows (protocol 38). Guarded by outCs_.
    std::vector<ResearchPacket>  outResearch_;
    // Reliable bounty/crime rows (protocol 45). Guarded by outCs_.
    std::vector<BountyPacket>    outBounty_;
    std::vector<BuildPlacePacket> outBuildPlace_;
    std::vector<BuildStatePacket> outBuildState_;
    std::vector<BuildDoorPacket>  outBuildDoor_;
    std::vector<BuildRemovePacket> outBuildRemove_;
    // Unreliable stealth detection-map snapshots (protocol 20). Guarded by outCs_.
    std::vector<StealthPacket>   outStealth_;
    // Unreliable camera hints (protocol 43, ~1 Hz latest-wins). Guarded by outCs_.
    std::vector<CamHintPacket>   outCamHint_;
    // Reliable runtime-spawn query/description packets (protocol 21). Guarded by outCs_.
    std::vector<SpawnReqPacket>  outSpawnReq_;
    std::vector<SpawnInfoPacket> outSpawnInfo_;
    // Reliable coordinated-save packets (protocol 31). FILE carries its
    // variable tail (relative path + payload) pre-flattened; DONE carries its
    // CRC table. Guarded by outCs_.
    struct OutSaveFile { SaveFileHeader hdr; std::vector<u8> tail; };
    struct OutSaveDone { SaveDoneHeader hdr; std::vector<u32> crcs; };
    std::vector<SaveReqPacket>   outSaveReq_;
    std::vector<SaveBeginPacket> outSaveBegin_;
    std::vector<OutSaveFile>     outSaveFile_;
    std::vector<OutSaveDone>     outSaveDone_;
    std::vector<SaveAckPacket>   outSaveAck_;
    // Reliable coordinated-load packets (protocol 32). Guarded by outCs_.
    std::vector<LoadGoPacket>    outLoadGo_;
    std::vector<LoadReqPacket>   outLoadReq_;
    std::vector<LoadNackPacket>  outLoadNack_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    // Written by the NET thread on WELCOME (InterlockedExchange) and read on the
    // MAIN thread via localId(); volatile LONG so the read is atomic + uncached.
    volatile LONG myId_;

    // Session epoch (protocol 44). sendEpoch_ is bumped by the MAIN thread
    // (InterlockedIncrement in bumpSessionEpoch) and read by the NET thread when
    // it stamps an outgoing entity batch - a volatile LONG, so the read is atomic
    // + uncached. epochSeen_ is NET-thread-only (touched only in the receive
    // ladder + connect/disconnect handlers), so it needs no lock.
    volatile LONG        sendEpoch_;
    std::map<u32, u32>   epochSeen_; // newest accepted epoch per ownerId

    // Steam P2P transport (set before launch; read-only on the net thread
    // thereafter). 0 = stock UDP transport.
    unsigned long long steamPeer_;

    // WAN sim config (set before launch; read-only on the net thread thereafter).
    unsigned int  simDelayMs_;
    unsigned int  simJitterMs_;
    unsigned int  simLossPct_;
    // Held-back inbound entities awaiting their simulated arrival time. Net-thread
    // only (received and flushed on the same thread), so it needs no lock.
    struct Delayed { DWORD releaseTick; u32 ownerId; u32 sendMs; EntityState e; };
    std::deque<Delayed> delayed_;

    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};

} // namespace coop

#endif // KENSHICOOP_NETLINK_H
