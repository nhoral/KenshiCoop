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
#include <vector>
#include "../../netproto/Wire.h"

namespace coop {

// One received entity plus the network id of the peer that owns it. The owner
// tag lets the receiver apply the right authority rule (drive a peer's entity,
// never one we own). sendMs (wire v35) is the sender's monotonic ms clock at
// capture time - the interp buffer indexes on it (mapped into the local clock)
// instead of the arrival time, so path jitter stays out of the snapshot spacing.
struct InboundEntity {
    u32         ownerId;
    u32         sendMs;
    EntityState e;
};

// One received reliable event (KO/death/revive transition), owner-tagged like an
// entity so the receiver applies it to the right peer's body.
struct InboundEvent {
    u32         ownerId;
    EventPacket ev;
};

// One received container-contents snapshot (Phase 4a): the authoritative owner, the
// container's key, and the full item list. The receiver reconciles its local copy
// of that container to match. count==0 means "now empty". keyKind (protocol 34):
// 0 = cKey is the raw container hand, 1 = cKey is the protocol-27 placer key of a
// session-placed building (the receiver translates through its build maps).
struct InboundInv {
    u32                       ownerId;
    u8                        keyKind;
    u32                       cKey[5]; // type, container, containerSerial, index, serial
    std::vector<InvItemEntry> items;
};

// One received world-item snapshot (Phase W1): the authoritative owner (host) and the
// netId-keyed ground items in its interest sphere. The join reconciles its local proxies
// (spawn new / update moved / leave the rest) to match.
struct InboundWorldItems {
    u32                        ownerId;
    std::vector<WorldItemEntry> items;
};

// One received world-item cull (Phase W1): the netIds whose ground items left the world /
// interest sphere, so the join destroys the matching proxies.
struct InboundWorldRemove {
    u32              ownerId;
    std::vector<u32> netIds;
};

// One received NPC existence census (protocol 36, join side): the hands of
// every world NPC within the host's census radius, flat 5xu32 per NPC. The
// join culls local NPCs absent from this list at long range (existence
// authority); position authority stays with the 20 Hz entity stream.
struct InboundNpcCensus {
    u32                ownerId;
    std::vector<u32>   hands; // count*5, readObjectHand layout
    std::vector<float> pos;   // count*3, host position per row (v38 parking)
};

// One received conservation DROP intent (Phase W2): the owning character + item identity +
// ground position. The receiver relocates ITS OWN copy of the weapon from that character's
// bag to the ground (no fabrication). Owner-tagged so the non-owner is the one that acts.
struct InboundWorldDrop {
    u32             ownerId;
    WorldDropPacket pkt;
};

// One received conservation PICKUP intent (Phase W3): the picking character + item identity.
// The receiver re-homes ITS OWN tracked ground copy of that weapon back into the character's
// bag (world -> bag, no fabrication). Owner-tagged so the non-owner is the one that acts.
struct InboundWorldPickup {
    u32               ownerId;
    WorldPickupPacket pkt;
};

// One received cross-owner TRANSFER intent (protocol 37): a peer performed a
// direct UI drag between two containers, at least one of which it does not
// author. The receiver relocates the REAL item between its own copies of the
// two containers (conservation - no fabrication, no destruction).
struct InboundInvXfer {
    u32           ownerId;
    InvXferPacket pkt;
};

// One received owner-authoritative medical snapshot (phase 2): the subject's
// owner streams its local-only medical model; the receiver writes it onto its
// driven copy of that body.
struct InboundMedical {
    u32           ownerId;
    MedicalPacket pkt;
};

// One received treatment delta (phase 2): first aid administered on a DRIVEN
// copy, forwarded to the body's owner, who applies the bandage levels raise-only.
struct InboundTreatment {
    u32             ownerId;
    TreatmentPacket pkt;
};

// One received game-speed packet (consensus speed sync): a REQUEST from a peer
// (host consumes; join requests never reach a join) or the host's arbitrated
// SET (join applies). pkt.type distinguishes the two.
struct InboundSpeed {
    u32         ownerId;
    SpeedPacket pkt;
};

// One received owner-authoritative character-stats snapshot (protocol 17): the
// subject's owner streams its local-only CharStats; the receiver writes it onto
// its driven copy of that body.
struct InboundStats {
    u32         ownerId;
    StatsPacket pkt;
};

// One received per-tab wallet snapshot (protocol 22): the tab's owner streams
// its Ownerships::money; the receiver writes it onto its local copy of that
// tab's platoon.
struct InboundMoney {
    u32         ownerId;
    MoneyPacket pkt;
};

// One received player-faction relation row (protocol 24): from the host it is
// the authoritative row (the join applies it); from the join it is a forwarded
// intent (the host applies it, then its own stream echoes the row back).
struct InboundFaction {
    u32           ownerId;
    FactionPacket pkt;
};

// One received game-clock sample (protocol 25, join side): the host's
// absolute in-game hours; the join slews its local sim speed to close the
// offset.
struct InboundTime {
    u32        ownerId;
    TimePacket pkt;
};

// One received baked-door state row (protocol 26): a door the sender's table
// saw move; the receiver applies it through the engine's own door actions
// (baseline updated first - echo-free).
struct InboundDoor {
    u32        ownerId;
    DoorPacket pkt;
};

// One received placed-building announcement (protocol 27): the sender placed
// a building; the receiver mints a local construction site and maps the
// sender's key to its own local hand.
struct InboundBuildPlace {
    u32              ownerId;
    BuildPlacePacket pkt;
};

// One received construction-progress row (protocol 27): progress for a
// building the SENDER placed, applied through the receiver's translation map.
struct InboundBuildState {
    u32              ownerId;
    BuildStatePacket pkt;
};

// One received placed-building door row (protocol 28): keyed by the PLACER's
// building hand + door index; the receiver resolves its local door through
// the build maps and applies via the engine's own door actions.
struct InboundBuildDoor {
    u32             ownerId;
    BuildDoorPacket pkt;
};

// One received building removal (protocol 28): the sender dismantled or
// destroyed a building it placed; the receiver destroys its mapped proxy.
struct InboundBuildRemove {
    u32               ownerId;
    BuildRemovePacket pkt;
};

// One received machine state row (protocol 33): the HOST's authoritative
// power/production/farm state for a machine; the join resolves the key
// (baked hand or protocol-27 placer key) and applies through the engine's
// own levers.
struct InboundProd {
    u32        ownerId;
    ProdPacket pkt;
};

// One received known-research row (protocol 38): the HOST reports a RESEARCH
// stringID as known; the join applies via Research::startResearch (idempotent
// against already-known sids).
struct InboundResearch {
    u32            ownerId;
    ResearchPacket pkt;
};

// One received stealth detection-map snapshot (protocol 20): the detection
// AUTHORITY (the host's world, where the sneaker is a driven copy) streams who
// notices the sneaker; the sneaker's OWNER replays the entries between its
// local pair so the marker arrows render natively.
struct InboundStealth {
    u32           ownerId;
    StealthPacket pkt;
};

// One received runtime-spawn query (protocol 21, host side): the join names a
// streamed hand it cannot resolve; the host describes it (PKT_SPAWN_INFO).
struct InboundSpawnReq {
    u32            ownerId;
    SpawnReqPacket pkt;
};

// One received runtime-spawn description (protocol 21, join side): the host's
// template/faction/transform for a hand the join asked about; the join mints a
// local proxy body and binds it to the hand key.
struct InboundSpawnInfo {
    u32             ownerId;
    SpawnInfoPacket pkt;
};

// One received join save request (protocol 31, host side): the join's player
// pressed save (local write suppressed); the host runs the authoritative save.
struct InboundSaveReq {
    u32           ownerId;
    SaveReqPacket pkt;
};

// One received save-transfer announce (protocol 31, join side): the host is
// about to stream fileCount files / totalBytes; the join wipes + creates its
// staging folder.
struct InboundSaveBegin {
    u32             ownerId;
    SaveBeginPacket pkt;
};

// One received save-file chunk (protocol 31, join side): header + the file's
// save-relative path + up to SAVE_CHUNK_MAX payload bytes. Ordered-reliable
// delivery means chunks arrive exactly as the sender paced them.
struct InboundSaveFile {
    u32             ownerId;
    SaveFileHeader  hdr;
    std::string     path; // hdr.pathLen bytes, save-folder-relative
    std::vector<u8> data; // hdr.dataLen bytes
};

// One received save-transfer CRC table (protocol 31, join side): verify the
// staged files and commit (or discard).
struct InboundSaveDone {
    u32              ownerId;
    SaveDoneHeader   hdr;
    std::vector<u32> crcs;
};

// One received commit acknowledgement (protocol 31, host side).
struct InboundSaveAck {
    u32           ownerId;
    SaveAckPacket pkt;
};

// One received coordinated-load order (protocol 32, join side): the host
// loaded a save; the join verifies its copy and follows (or NACKs).
struct InboundLoadGo {
    u32          ownerId;
    LoadGoPacket pkt;
};

// One received join load request (protocol 32, host side): the join's player
// pressed load (suppressed locally); the host arbitrates.
struct InboundLoadReq {
    u32           ownerId;
    LoadReqPacket pkt;
};

// One received copy-missing/diverged answer (protocol 32, host side): the
// join can't load the ordered save; a SaveXfer transfer follows.
struct InboundLoadNack {
    u32            ownerId;
    LoadNackPacket pkt;
};

// One received camera hint (protocol 43, host side): the join's camera world
// center, folded into interestCenters as an extra anchor. Latest wins.
struct InboundCamHint {
    u32           ownerId;
    CamHintPacket pkt;
};

// --- Structural world-state classification (Phase 4) -------------------------
// Every inbound queue is exactly one of two kinds, chosen at its DECLARATION:
//   WorldQ<T>   - describes the CURRENT world; dropped on a session-reset edge
//                 (reload / reconnect / disconnect). It self-registers into the
//                 owner's world-state reset list on construction, so
//                 flushWorldState() clears it automatically - a WorldQ CANNOT be
//                 forgotten from the reset (the class of bug that once let a
//                 cross-owner invXfer intent survive a world reload).
//   SessionQ<T> - OUTLIVES the world swap (the connection persists): presence
//                 edges + the coordinated save/load handshake. Never registered,
//                 so flushWorldState() leaves it intact by construction.
// Both forward the only three operations Inbound performs on a queue - push_back
// (net thread), swap-drain (main thread, via the std::deque& conversion) and
// clear (flush) - so the push/drain methods below are unchanged.
struct IClearableQueue {
    virtual void clearQueue() = 0;
    virtual ~IClearableQueue() {}
};

template<class T>
class WorldQ : public IClearableQueue {
public:
    // cap == 0: unbounded (reliable queues MUST stay unbounded - dropping a
    // save chunk or an event corrupts the stream). cap > 0: a bounded mailbox
    // for an UNRELIABLE latest-wins traffic class (entity/stealth/cam), where a
    // stalled main thread would otherwise let the queue grow without limit; on
    // overflow the OLDEST entry is dropped, which is exactly "newest wins" for
    // continuous state (Phase 4d bounded mailboxes).
    explicit WorldQ(std::vector<IClearableQueue*>& reg, size_t cap = 0)
      : cap_(cap) { reg.push_back(this); }
    void push_back(const T& v) {
        if (cap_ && q_.size() >= cap_) q_.pop_front();
        q_.push_back(v);
    }
    operator std::deque<T>&() { return q_; }
    virtual void clearQueue() { q_.clear(); }
private:
    std::deque<T> q_;
    size_t        cap_;
    WorldQ(const WorldQ&);
    WorldQ& operator=(const WorldQ&);
};

template<class T>
class SessionQ {
public:
    SessionQ() {}
    void push_back(const T& v) { q_.push_back(v); }
    operator std::deque<T>&() { return q_; }
private:
    std::deque<T> q_;
    SessionQ(const SessionQ&);
    SessionQ& operator=(const SessionQ&);
};

class Inbound {
public:
    // WorldQ members self-register into worldReset_ (declared first, below), so
    // the init list wires every world-state queue to the structural reset.
    Inbound()
      : sawRemote_(false), generation_(0),
        // Bounded mailboxes (Phase 4d): the three UNRELIABLE latest-wins queues
        // carry a drop-oldest cap so a stalled main thread cannot grow them
        // without bound. ent_ = ~12 s of a 20 Hz * 17-entity stream; stealth/cam
        // are ~1 Hz refreshes. Every other queue is reliable and stays unbounded.
        ent_(worldReset_, 4096),  evt_(worldReset_),        inv_(worldReset_),
        wi_(worldReset_),         wir_(worldReset_),        npcCensus_(worldReset_),
        wd_(worldReset_),         invXfer_(worldReset_),    wp_(worldReset_),
        med_(worldReset_),        treat_(worldReset_),      speed_(worldReset_),
        stats_(worldReset_),      money_(worldReset_),      faction_(worldReset_),
        time_(worldReset_),       door_(worldReset_),       prod_(worldReset_),
        research_(worldReset_),   buildPlace_(worldReset_), buildState_(worldReset_),
        buildDoor_(worldReset_),  buildRemove_(worldReset_), stealth_(worldReset_, 512),
        spawnReq_(worldReset_),   spawnInfo_(worldReset_),  camHint_(worldReset_, 64) {
        InitializeCriticalSection(&cs_);
    }
    ~Inbound() { DeleteCriticalSection(&cs_); }

    // NET thread: a peer joined (id) / a peer left (id, or OWNER_ID_ALL).
    void pushConnect(u32 id) {
        EnterCriticalSection(&cs_); conn_.push_back(id); LeaveCriticalSection(&cs_);
    }
    void pushLeave(u32 id) {
        EnterCriticalSection(&cs_); leave_.push_back(id); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received entity transform, owner-tagged + send-stamped.
    void pushEntity(u32 ownerId, u32 sendMs, const EntityState& e) {
        InboundEntity ie; ie.ownerId = ownerId; ie.sendMs = sendMs; ie.e = e;
        EnterCriticalSection(&cs_); ent_.push_back(ie); sawRemote_ = true;
        LeaveCriticalSection(&cs_);
    }

    // MAIN thread: has this client EVER received an owned-entity batch from a peer?
    // A peer only publishes its owned entities once IT reaches gameplay, so on the
    // host this flips true exactly when the JOIN is loaded + streaming (the reliable
    // "peer is in-game" gate for time-sensitive host actions like a live spawn).
    bool sawRemoteEntity() {
        EnterCriticalSection(&cs_); bool v = sawRemote_; LeaveCriticalSection(&cs_);
        return v;
    }
    // MAIN thread: the current session generation. Bumped by flushWorldState()
    // on every world-state reset edge (reload / reconnect / disconnect). A
    // reference or index captured under an older generation is known-stale.
    // Phase 0 seed for the Phase 4 wire epoch - cheap now, consumed later by
    // the SessionController + stale-generation packet rejection.
    u32 sessionGeneration() {
        EnterCriticalSection(&cs_); u32 g = generation_; LeaveCriticalSection(&cs_);
        return g;
    }
    // NET thread: one received reliable event, owner-tagged.
    void pushEvent(u32 ownerId, const EventPacket& ev) {
        InboundEvent ievt; ievt.ownerId = ownerId; ievt.ev = ev;
        EnterCriticalSection(&cs_); evt_.push_back(ievt); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received container-contents snapshot, owner-tagged.
    void pushInv(u32 ownerId, u8 keyKind, const u32 cKey[5],
                 const InvItemEntry* items, unsigned int count) {
        InboundInv ii;
        ii.ownerId = ownerId;
        ii.keyKind = keyKind;
        for (int k = 0; k < 5; ++k) ii.cKey[k] = cKey[k];
        if (items && count > 0) ii.items.assign(items, items + count);
        EnterCriticalSection(&cs_); inv_.push_back(ii); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received world-item snapshot, owner-tagged.
    void pushWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count) {
        InboundWorldItems wi;
        wi.ownerId = ownerId;
        if (items && count > 0) wi.items.assign(items, items + count);
        EnterCriticalSection(&cs_); wi_.push_back(wi); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received world-item cull (list of netIds), owner-tagged.
    void pushWorldRemove(u32 ownerId, const u32* netIds, unsigned int count) {
        InboundWorldRemove wr;
        wr.ownerId = ownerId;
        if (netIds && count > 0) wr.netIds.assign(netIds, netIds + count);
        EnterCriticalSection(&cs_); wir_.push_back(wr); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received NPC existence census (protocol 36), owner-tagged.
    void pushNpcCensus(u32 ownerId, const u32* hands, const float* pos,
                       unsigned int count) {
        InboundNpcCensus nc;
        nc.ownerId = ownerId;
        if (hands && count > 0) nc.hands.assign(hands, hands + count * 5);
        if (pos && count > 0) nc.pos.assign(pos, pos + count * 3);
        EnterCriticalSection(&cs_); npcCensus_.push_back(nc); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received conservation DROP intent, owner-tagged.
    void pushWorldDrop(u32 ownerId, const WorldDropPacket& pkt) {
        InboundWorldDrop wd; wd.ownerId = ownerId; wd.pkt = pkt;
        EnterCriticalSection(&cs_); wd_.push_back(wd); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received conservation PICKUP intent, owner-tagged.
    void pushWorldPickup(u32 ownerId, const WorldPickupPacket& pkt) {
        InboundWorldPickup wp; wp.ownerId = ownerId; wp.pkt = pkt;
        EnterCriticalSection(&cs_); wp_.push_back(wp); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received cross-owner transfer intent (protocol 37), owner-tagged.
    void pushInvXfer(u32 ownerId, const InvXferPacket& pkt) {
        InboundInvXfer ix; ix.ownerId = ownerId; ix.pkt = pkt;
        EnterCriticalSection(&cs_); invXfer_.push_back(ix); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received medical snapshot, owner-tagged.
    void pushMedical(u32 ownerId, const MedicalPacket& pkt) {
        InboundMedical im; im.ownerId = ownerId; im.pkt = pkt;
        EnterCriticalSection(&cs_); med_.push_back(im); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received treatment delta, owner-tagged.
    void pushTreatment(u32 ownerId, const TreatmentPacket& pkt) {
        InboundTreatment it; it.ownerId = ownerId; it.pkt = pkt;
        EnterCriticalSection(&cs_); treat_.push_back(it); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received game-speed request/set, owner-tagged.
    void pushSpeed(u32 ownerId, const SpeedPacket& pkt) {
        InboundSpeed is; is.ownerId = ownerId; is.pkt = pkt;
        EnterCriticalSection(&cs_); speed_.push_back(is); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received character-stats snapshot, owner-tagged.
    void pushStats(u32 ownerId, const StatsPacket& pkt) {
        InboundStats ist; ist.ownerId = ownerId; ist.pkt = pkt;
        EnterCriticalSection(&cs_); stats_.push_back(ist); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received per-tab wallet snapshot (protocol 22), owner-tagged.
    void pushMoney(u32 ownerId, const MoneyPacket& pkt) {
        InboundMoney imo; imo.ownerId = ownerId; imo.pkt = pkt;
        EnterCriticalSection(&cs_); money_.push_back(imo); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received faction-relation row (protocol 24), owner-tagged.
    void pushFaction(u32 ownerId, const FactionPacket& pkt) {
        InboundFaction ifa; ifa.ownerId = ownerId; ifa.pkt = pkt;
        EnterCriticalSection(&cs_); faction_.push_back(ifa); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received game-clock sample (protocol 25), owner-tagged.
    void pushTime(u32 ownerId, const TimePacket& pkt) {
        InboundTime iti; iti.ownerId = ownerId; iti.pkt = pkt;
        EnterCriticalSection(&cs_); time_.push_back(iti); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received baked-door state row (protocol 26), owner-tagged.
    void pushDoor(u32 ownerId, const DoorPacket& pkt) {
        InboundDoor ido; ido.ownerId = ownerId; ido.pkt = pkt;
        EnterCriticalSection(&cs_); door_.push_back(ido); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received machine state row (protocol 33), owner-tagged.
    void pushProd(u32 ownerId, const ProdPacket& pkt) {
        InboundProd ip; ip.ownerId = ownerId; ip.pkt = pkt;
        EnterCriticalSection(&cs_); prod_.push_back(ip); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received known-research row (protocol 38), owner-tagged.
    void pushResearch(u32 ownerId, const ResearchPacket& pkt) {
        InboundResearch ir; ir.ownerId = ownerId; ir.pkt = pkt;
        EnterCriticalSection(&cs_); research_.push_back(ir); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received placed-building announcement (protocol 27), owner-tagged.
    void pushBuildPlace(u32 ownerId, const BuildPlacePacket& pkt) {
        InboundBuildPlace ibp; ibp.ownerId = ownerId; ibp.pkt = pkt;
        EnterCriticalSection(&cs_); buildPlace_.push_back(ibp); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received construction-progress row (protocol 27), owner-tagged.
    void pushBuildState(u32 ownerId, const BuildStatePacket& pkt) {
        InboundBuildState ibs; ibs.ownerId = ownerId; ibs.pkt = pkt;
        EnterCriticalSection(&cs_); buildState_.push_back(ibs); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received placed-building door row (protocol 28), owner-tagged.
    void pushBuildDoor(u32 ownerId, const BuildDoorPacket& pkt) {
        InboundBuildDoor ibd; ibd.ownerId = ownerId; ibd.pkt = pkt;
        EnterCriticalSection(&cs_); buildDoor_.push_back(ibd); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received building removal (protocol 28), owner-tagged.
    void pushBuildRemove(u32 ownerId, const BuildRemovePacket& pkt) {
        InboundBuildRemove ibr; ibr.ownerId = ownerId; ibr.pkt = pkt;
        EnterCriticalSection(&cs_); buildRemove_.push_back(ibr); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received stealth detection-map snapshot, owner-tagged.
    void pushStealth(u32 ownerId, const StealthPacket& pkt) {
        InboundStealth isl; isl.ownerId = ownerId; isl.pkt = pkt;
        EnterCriticalSection(&cs_); stealth_.push_back(isl); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received runtime-spawn query (protocol 21), owner-tagged.
    void pushSpawnReq(u32 ownerId, const SpawnReqPacket& pkt) {
        InboundSpawnReq sr; sr.ownerId = ownerId; sr.pkt = pkt;
        EnterCriticalSection(&cs_); spawnReq_.push_back(sr); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received runtime-spawn description (protocol 21), owner-tagged.
    void pushSpawnInfo(u32 ownerId, const SpawnInfoPacket& pkt) {
        InboundSpawnInfo si; si.ownerId = ownerId; si.pkt = pkt;
        EnterCriticalSection(&cs_); spawnInfo_.push_back(si); LeaveCriticalSection(&cs_);
    }
    // NET thread: coordinated-save packets (protocol 31), owner-tagged.
    void pushSaveReq(u32 ownerId, const SaveReqPacket& pkt) {
        InboundSaveReq sr; sr.ownerId = ownerId; sr.pkt = pkt;
        EnterCriticalSection(&cs_); saveReq_.push_back(sr); LeaveCriticalSection(&cs_);
    }
    void pushSaveBegin(u32 ownerId, const SaveBeginPacket& pkt) {
        InboundSaveBegin sb; sb.ownerId = ownerId; sb.pkt = pkt;
        EnterCriticalSection(&cs_); saveBegin_.push_back(sb); LeaveCriticalSection(&cs_);
    }
    void pushSaveFile(u32 ownerId, const SaveFileHeader& hdr,
                      const char* path, const u8* data) {
        InboundSaveFile sf;
        sf.ownerId = ownerId;
        sf.hdr     = hdr;
        sf.path.assign(path, path + hdr.pathLen);
        if (data && hdr.dataLen > 0) sf.data.assign(data, data + hdr.dataLen);
        EnterCriticalSection(&cs_); saveFile_.push_back(sf); LeaveCriticalSection(&cs_);
    }
    void pushSaveDone(u32 ownerId, const SaveDoneHeader& hdr, const u32* crcs) {
        InboundSaveDone sd;
        sd.ownerId = ownerId;
        sd.hdr     = hdr;
        if (crcs && hdr.fileCount > 0) sd.crcs.assign(crcs, crcs + hdr.fileCount);
        EnterCriticalSection(&cs_); saveDone_.push_back(sd); LeaveCriticalSection(&cs_);
    }
    void pushSaveAck(u32 ownerId, const SaveAckPacket& pkt) {
        InboundSaveAck sa; sa.ownerId = ownerId; sa.pkt = pkt;
        EnterCriticalSection(&cs_); saveAck_.push_back(sa); LeaveCriticalSection(&cs_);
    }
    // NET thread: coordinated-load packets (protocol 32), owner-tagged.
    void pushLoadGo(u32 ownerId, const LoadGoPacket& pkt) {
        InboundLoadGo lg; lg.ownerId = ownerId; lg.pkt = pkt;
        EnterCriticalSection(&cs_); loadGo_.push_back(lg); LeaveCriticalSection(&cs_);
    }
    void pushLoadReq(u32 ownerId, const LoadReqPacket& pkt) {
        InboundLoadReq lr; lr.ownerId = ownerId; lr.pkt = pkt;
        EnterCriticalSection(&cs_); loadReq_.push_back(lr); LeaveCriticalSection(&cs_);
    }
    void pushLoadNack(u32 ownerId, const LoadNackPacket& pkt) {
        InboundLoadNack ln; ln.ownerId = ownerId; ln.pkt = pkt;
        EnterCriticalSection(&cs_); loadNack_.push_back(ln); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received camera hint (protocol 43), owner-tagged.
    void pushCamHint(u32 ownerId, const CamHintPacket& pkt) {
        InboundCamHint ch; ch.ownerId = ownerId; ch.pkt = pkt;
        EnterCriticalSection(&cs_); camHint_.push_back(ch); LeaveCriticalSection(&cs_);
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
    void drainEvents(std::deque<InboundEvent>& out) {
        EnterCriticalSection(&cs_); out.swap(evt_); LeaveCriticalSection(&cs_);
    }
    void drainInv(std::deque<InboundInv>& out) {
        EnterCriticalSection(&cs_); out.swap(inv_); LeaveCriticalSection(&cs_);
    }
    void drainWorldItems(std::deque<InboundWorldItems>& out) {
        EnterCriticalSection(&cs_); out.swap(wi_); LeaveCriticalSection(&cs_);
    }
    void drainWorldRemove(std::deque<InboundWorldRemove>& out) {
        EnterCriticalSection(&cs_); out.swap(wir_); LeaveCriticalSection(&cs_);
    }
    void drainNpcCensus(std::deque<InboundNpcCensus>& out) {
        EnterCriticalSection(&cs_); out.swap(npcCensus_); LeaveCriticalSection(&cs_);
    }
    void drainWorldDrops(std::deque<InboundWorldDrop>& out) {
        EnterCriticalSection(&cs_); out.swap(wd_); LeaveCriticalSection(&cs_);
    }
    void drainWorldPickups(std::deque<InboundWorldPickup>& out) {
        EnterCriticalSection(&cs_); out.swap(wp_); LeaveCriticalSection(&cs_);
    }
    void drainInvXfers(std::deque<InboundInvXfer>& out) {
        EnterCriticalSection(&cs_); out.swap(invXfer_); LeaveCriticalSection(&cs_);
    }
    void drainMedical(std::deque<InboundMedical>& out) {
        EnterCriticalSection(&cs_); out.swap(med_); LeaveCriticalSection(&cs_);
    }
    void drainTreatments(std::deque<InboundTreatment>& out) {
        EnterCriticalSection(&cs_); out.swap(treat_); LeaveCriticalSection(&cs_);
    }
    void drainSpeed(std::deque<InboundSpeed>& out) {
        EnterCriticalSection(&cs_); out.swap(speed_); LeaveCriticalSection(&cs_);
    }
    void drainStats(std::deque<InboundStats>& out) {
        EnterCriticalSection(&cs_); out.swap(stats_); LeaveCriticalSection(&cs_);
    }
    void drainFaction(std::deque<InboundFaction>& out) {
        EnterCriticalSection(&cs_); out.swap(faction_); LeaveCriticalSection(&cs_);
    }
    void drainTime(std::deque<InboundTime>& out) {
        EnterCriticalSection(&cs_); out.swap(time_); LeaveCriticalSection(&cs_);
    }
    void drainDoor(std::deque<InboundDoor>& out) {
        EnterCriticalSection(&cs_); out.swap(door_); LeaveCriticalSection(&cs_);
    }
    void drainProd(std::deque<InboundProd>& out) {
        EnterCriticalSection(&cs_); out.swap(prod_); LeaveCriticalSection(&cs_);
    }
    void drainResearch(std::deque<InboundResearch>& out) {
        EnterCriticalSection(&cs_); out.swap(research_); LeaveCriticalSection(&cs_);
    }
    void drainBuildPlace(std::deque<InboundBuildPlace>& out) {
        EnterCriticalSection(&cs_); out.swap(buildPlace_); LeaveCriticalSection(&cs_);
    }
    void drainBuildState(std::deque<InboundBuildState>& out) {
        EnterCriticalSection(&cs_); out.swap(buildState_); LeaveCriticalSection(&cs_);
    }
    void drainBuildDoor(std::deque<InboundBuildDoor>& out) {
        EnterCriticalSection(&cs_); out.swap(buildDoor_); LeaveCriticalSection(&cs_);
    }
    void drainBuildRemove(std::deque<InboundBuildRemove>& out) {
        EnterCriticalSection(&cs_); out.swap(buildRemove_); LeaveCriticalSection(&cs_);
    }
    void drainMoney(std::deque<InboundMoney>& out) {
        EnterCriticalSection(&cs_); out.swap(money_); LeaveCriticalSection(&cs_);
    }
    void drainStealth(std::deque<InboundStealth>& out) {
        EnterCriticalSection(&cs_); out.swap(stealth_); LeaveCriticalSection(&cs_);
    }
    void drainSpawnReqs(std::deque<InboundSpawnReq>& out) {
        EnterCriticalSection(&cs_); out.swap(spawnReq_); LeaveCriticalSection(&cs_);
    }
    void drainSpawnInfos(std::deque<InboundSpawnInfo>& out) {
        EnterCriticalSection(&cs_); out.swap(spawnInfo_); LeaveCriticalSection(&cs_);
    }
    void drainSaveReqs(std::deque<InboundSaveReq>& out) {
        EnterCriticalSection(&cs_); out.swap(saveReq_); LeaveCriticalSection(&cs_);
    }
    void drainSaveBegins(std::deque<InboundSaveBegin>& out) {
        EnterCriticalSection(&cs_); out.swap(saveBegin_); LeaveCriticalSection(&cs_);
    }
    void drainSaveFiles(std::deque<InboundSaveFile>& out) {
        EnterCriticalSection(&cs_); out.swap(saveFile_); LeaveCriticalSection(&cs_);
    }
    void drainSaveDones(std::deque<InboundSaveDone>& out) {
        EnterCriticalSection(&cs_); out.swap(saveDone_); LeaveCriticalSection(&cs_);
    }
    void drainSaveAcks(std::deque<InboundSaveAck>& out) {
        EnterCriticalSection(&cs_); out.swap(saveAck_); LeaveCriticalSection(&cs_);
    }
    void drainLoadGos(std::deque<InboundLoadGo>& out) {
        EnterCriticalSection(&cs_); out.swap(loadGo_); LeaveCriticalSection(&cs_);
    }
    void drainLoadReqs(std::deque<InboundLoadReq>& out) {
        EnterCriticalSection(&cs_); out.swap(loadReq_); LeaveCriticalSection(&cs_);
    }
    void drainLoadNacks(std::deque<InboundLoadNack>& out) {
        EnterCriticalSection(&cs_); out.swap(loadNack_); LeaveCriticalSection(&cs_);
    }
    void drainCamHints(std::deque<InboundCamHint>& out) {
        EnterCriticalSection(&cs_); out.swap(camHint_); LeaveCriticalSection(&cs_);
    }

    // MAIN thread, session reset (protocol 32): drop every queued packet that
    // describes the OLD world after a reload / reconnect / disconnect edge, and
    // start a new session generation.
    //
    // Coverage is STRUCTURAL: every WorldQ registered itself into worldReset_ at
    // construction, so iterating that list clears exactly the world-state queues
    // and nothing else - a queue added later cannot be silently forgotten (the
    // bug that let a cross-owner invXfer transfer intent survive a world reload).
    // SessionQ queues (presence + the coordinated save/load handshake) never
    // register, so they survive by construction - a NACK/GO arriving mid-swap
    // must not be dropped.
    void flushWorldState() {
        EnterCriticalSection(&cs_);
        for (size_t i = 0; i < worldReset_.size(); ++i)
            worldReset_[i]->clearQueue();
        // Peer-readiness is world-scoped: the owned-entity batch that set
        // sawRemote_ described the OLD world, so a fresh session must
        // re-observe the peer before time-sensitive host actions (e.g. a live
        // spawn) or a scenario may arm again.
        sawRemote_ = false;
        ++generation_;
        LeaveCriticalSection(&cs_);
    }

private:
    CRITICAL_SECTION          cs_;
    bool                      sawRemote_;  // set once any peer entity batch arrives
    u32                       generation_; // session generation; bumped on flush
    // World-state reset list: every WorldQ below self-registers here at
    // construction so flushWorldState() clears them structurally. MUST be
    // declared before the WorldQ members (member init order).
    std::vector<IClearableQueue*> worldReset_;

    // SESSION-PRESERVING: presence edges (the connection persists across a swap).
    SessionQ<u32>                  conn_;
    SessionQ<u32>                  leave_;

    // WORLD-STATE: describe the current world; auto-cleared on a session reset.
    WorldQ<InboundEntity>          ent_;
    WorldQ<InboundEvent>           evt_;
    WorldQ<InboundInv>             inv_;
    WorldQ<InboundWorldItems>      wi_;
    WorldQ<InboundWorldRemove>     wir_;
    WorldQ<InboundNpcCensus>       npcCensus_;
    WorldQ<InboundWorldDrop>       wd_;
    WorldQ<InboundInvXfer>         invXfer_;
    WorldQ<InboundWorldPickup>     wp_;
    WorldQ<InboundMedical>         med_;
    WorldQ<InboundTreatment>       treat_;
    WorldQ<InboundSpeed>           speed_;
    WorldQ<InboundStats>           stats_;
    WorldQ<InboundMoney>           money_;
    WorldQ<InboundFaction>         faction_;
    WorldQ<InboundTime>            time_;
    WorldQ<InboundDoor>            door_;
    WorldQ<InboundProd>            prod_;
    WorldQ<InboundResearch>        research_;
    WorldQ<InboundBuildPlace>      buildPlace_;
    WorldQ<InboundBuildState>      buildState_;
    WorldQ<InboundBuildDoor>       buildDoor_;
    WorldQ<InboundBuildRemove>     buildRemove_;
    WorldQ<InboundStealth>         stealth_;
    WorldQ<InboundSpawnReq>        spawnReq_;
    WorldQ<InboundSpawnInfo>       spawnInfo_;
    WorldQ<InboundCamHint>         camHint_;

    // SESSION-PRESERVING: coordinated save (protocol 31) + load (protocol 32)
    // handshake - a GO/NACK/chunk arriving mid-swap must survive the reset.
    SessionQ<InboundSaveReq>       saveReq_;
    SessionQ<InboundSaveBegin>     saveBegin_;
    SessionQ<InboundSaveFile>      saveFile_;
    SessionQ<InboundSaveDone>      saveDone_;
    SessionQ<InboundSaveAck>       saveAck_;
    SessionQ<InboundLoadGo>        loadGo_;
    SessionQ<InboundLoadReq>       loadReq_;
    SessionQ<InboundLoadNack>      loadNack_;

    Inbound(const Inbound&);
    Inbound& operator=(const Inbound&);
};

} // namespace coop

#endif // KENSHICOOP_INBOUND_H
