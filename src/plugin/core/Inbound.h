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
// never one we own).
struct InboundEntity {
    u32         ownerId;
    EntityState e;
};

// One received reliable event (KO/death/revive transition), owner-tagged like an
// entity so the receiver applies it to the right peer's body.
struct InboundEvent {
    u32         ownerId;
    EventPacket ev;
};

// One received container-contents snapshot (Phase 4a): the authoritative owner, the
// container's hand, and the full item list. The receiver reconciles its local copy
// of that container to match. count==0 means "now empty".
struct InboundInv {
    u32                       ownerId;
    u32                       cHand[5]; // type, container, containerSerial, index, serial
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

class Inbound {
public:
    Inbound()  { InitializeCriticalSection(&cs_); sawRemote_ = false; }
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
    // NET thread: one received reliable event, owner-tagged.
    void pushEvent(u32 ownerId, const EventPacket& ev) {
        InboundEvent ievt; ievt.ownerId = ownerId; ievt.ev = ev;
        EnterCriticalSection(&cs_); evt_.push_back(ievt); LeaveCriticalSection(&cs_);
    }
    // NET thread: one received container-contents snapshot, owner-tagged.
    void pushInv(u32 ownerId, const u32 cHand[5], const InvItemEntry* items,
                 unsigned int count) {
        InboundInv ii;
        ii.ownerId = ownerId;
        for (int k = 0; k < 5; ++k) ii.cHand[k] = cHand[k];
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
    void drainWorldDrops(std::deque<InboundWorldDrop>& out) {
        EnterCriticalSection(&cs_); out.swap(wd_); LeaveCriticalSection(&cs_);
    }
    void drainWorldPickups(std::deque<InboundWorldPickup>& out) {
        EnterCriticalSection(&cs_); out.swap(wp_); LeaveCriticalSection(&cs_);
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

private:
    CRITICAL_SECTION          cs_;
    bool                      sawRemote_; // set once any peer entity batch arrives
    std::deque<u32>           conn_;
    std::deque<u32>           leave_;
    std::deque<InboundEntity> ent_;
    std::deque<InboundEvent>  evt_;
    std::deque<InboundInv>    inv_;
    std::deque<InboundWorldItems>  wi_;
    std::deque<InboundWorldRemove> wir_;
    std::deque<InboundWorldDrop>   wd_;
    std::deque<InboundWorldPickup> wp_;
    std::deque<InboundMedical>     med_;
    std::deque<InboundTreatment>   treat_;
    std::deque<InboundSpeed>       speed_;
    std::deque<InboundStats>       stats_;
    std::deque<InboundMoney>       money_;
    std::deque<InboundStealth>     stealth_;
    std::deque<InboundSpawnReq>    spawnReq_;
    std::deque<InboundSpawnInfo>   spawnInfo_;

    Inbound(const Inbound&);
    Inbound& operator=(const Inbound&);
};

} // namespace coop

#endif // KENSHICOOP_INBOUND_H
