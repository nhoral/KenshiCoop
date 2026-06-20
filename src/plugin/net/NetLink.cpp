#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetLink.h"
#include "../CoopLog.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace coop {

namespace {
const int        TICK_MS       = 50; // 20 Hz service/transmit cadence
const enet_uint8 CH_RELIABLE   = 0;  // handshake (HELLO/WELCOME)
const enet_uint8 CH_UNRELIABLE = 1;  // entity batches (newest supersedes)

// Net-thread diagnostics. OutputDebugStringA is thread-safe, and CoopLog guards
// its FILE* with a lock, so both are safe to call off the main thread.
void netLog(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}
void netErr(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ERROR: ");
    OutputDebugStringA(msg ? msg : "");
    OutputDebugStringA("\n");
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logErrLine(buf);
}
} // namespace

NetLink::NetLink()
    : isHost_(false), port_(0),
      enetHost_(0), serverPeer_(0), inbound_(0),
      outOwner_(0), haveOut_(false),
      thread_(0), running_(0), stopFlag_(0), myId_(0),
      simDelayMs_(0), simJitterMs_(0), simLossPct_(0) {
    InitializeCriticalSection(&outCs_);
}

NetLink::~NetLink() {
    stop();
    DeleteCriticalSection(&outCs_);
}

bool NetLink::startHost(int port, Inbound* inbound) {
    isHost_ = true; port_ = port; inbound_ = inbound; myId_ = 0;
    return launchThread();
}

bool NetLink::startClient(const std::string& ip, int port, Inbound* inbound) {
    isHost_ = false; ip_ = ip; port_ = port; inbound_ = inbound; myId_ = 0;
    return launchThread();
}

bool NetLink::launchThread() {
    if (enet_initialize() != 0) { netErr("enet_initialize failed"); return false; }
    stopFlag_ = 0;
    thread_ = CreateThread(0, 0, &NetLink::threadEntry, this, 0, 0);
    if (thread_ == 0) { netErr("CreateThread failed"); enet_deinitialize(); return false; }
    return true;
}

void NetLink::stop() {
    if (thread_) {
        InterlockedExchange(&stopFlag_, 1);
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = 0;
        enet_deinitialize();
    }
}

void NetLink::setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count) {
    EnterCriticalSection(&outCs_);
    outOwner_ = ownerId;
    if (arr && count > 0) out_.assign(arr, arr + count);
    else                  out_.clear();
    haveOut_ = true;
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueEvent(const EventPacket& ev) {
    EnterCriticalSection(&outCs_);
    outEvents_.push_back(ev);
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueInvSnapshot(u32 ownerId, const u32 cHand[5],
                               const InvItemEntry* items, unsigned int count) {
    OutInv oi;
    oi.ownerId = ownerId;
    for (int k = 0; k < 5; ++k) oi.cHand[k] = cHand[k];
    if (count > INV_ITEMS_MAX) count = INV_ITEMS_MAX;
    if (items && count > 0) oi.items.assign(items, items + count);
    EnterCriticalSection(&outCs_);
    outInv_.push_back(oi);
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueWorldItems(u32 ownerId, const WorldItemEntry* items, unsigned int count) {
    OutWorldItems ow;
    ow.ownerId = ownerId;
    if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
    if (items && count > 0) ow.items.assign(items, items + count);
    EnterCriticalSection(&outCs_);
    outWorldItems_.push_back(ow);
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueWorldRemove(u32 ownerId, const u32* netIds, unsigned int count) {
    OutWorldRemove ow;
    ow.ownerId = ownerId;
    if (count > 255) count = 255; // u8 count on the wire
    if (netIds && count > 0) ow.netIds.assign(netIds, netIds + count);
    EnterCriticalSection(&outCs_);
    outWorldRemove_.push_back(ow);
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueWorldDrop(const WorldDropPacket& pkt) {
    EnterCriticalSection(&outCs_);
    outWorldDrops_.push_back(pkt);
    LeaveCriticalSection(&outCs_);
}

void NetLink::queueWorldPickup(const WorldPickupPacket& pkt) {
    EnterCriticalSection(&outCs_);
    outWorldPickups_.push_back(pkt);
    LeaveCriticalSection(&outCs_);
}

void NetLink::setNetSim(unsigned int delayMs, unsigned int jitterMs, unsigned int lossPct) {
    simDelayMs_  = delayMs;
    simJitterMs_ = jitterMs;
    simLossPct_  = (lossPct > 100) ? 100 : lossPct;
}

// Deliver one received entity to the game thread, applying the WAN sim if enabled.
// Loopback has ~0 latency, so without this a received "down"/move lands the same
// frame it was sent - exactly the regime we want to stop relying on. With it, the
// entity is parked until base +/- jitter has elapsed (and dropped lossPct% of the
// time), so the join must coast on interpolation/local enforcement between arrivals.
void NetLink::deliverEntity(u32 ownerId, const EntityState& e) {
    if (simDelayMs_ == 0 && simJitterMs_ == 0 && simLossPct_ == 0) {
        if (inbound_) inbound_->pushEntity(ownerId, e);
        return;
    }
    if (simLossPct_ > 0 && (unsigned)(rand() % 100) < simLossPct_) return; // dropped
    int jitter = 0;
    if (simJitterMs_ > 0) jitter = (rand() % (int)(2 * simJitterMs_ + 1)) - (int)simJitterMs_;
    int delay = (int)simDelayMs_ + jitter;
    if (delay < 0) delay = 0;
    Delayed d;
    d.releaseTick = GetTickCount() + (DWORD)delay;
    d.ownerId     = ownerId;
    d.e           = e;
    delayed_.push_back(d);
}

// Release every held entity whose simulated arrival time has passed. Jitter can put
// release times out of order; we scan the whole queue (small N) and keep the rest.
// "Newest supersedes" on the receiver makes any reordering harmless (and realistic).
void NetLink::flushDelayed() {
    if (delayed_.empty()) return;
    DWORD now = GetTickCount();
    std::deque<Delayed> keep;
    for (std::deque<Delayed>::iterator it = delayed_.begin(); it != delayed_.end(); ++it) {
        if ((long)(now - it->releaseTick) >= 0) { if (inbound_) inbound_->pushEntity(it->ownerId, it->e); }
        else                                     { keep.push_back(*it); }
    }
    delayed_.swap(keep);
}

DWORD WINAPI NetLink::threadEntry(LPVOID self) {
    reinterpret_cast<NetLink*>(self)->threadLoop();
    return 0;
}

void NetLink::threadLoop() {
    InterlockedExchange(&running_, 1);

    if (isHost_) {
        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = (enet_uint16)port_;
        enetHost_ = enet_host_create(&addr, 8 /*peers*/, 2 /*channels*/, 0, 0);
        if (!enetHost_) { netErr("host create failed"); InterlockedExchange(&running_, 0); return; }
        netLog("hosting");
    } else {
        enetHost_ = enet_host_create(0, 1, 2, 0, 0);
        if (!enetHost_) { netErr("client create failed"); InterlockedExchange(&running_, 0); return; }
        ENetAddress addr;
        enet_address_set_host(&addr, ip_.c_str());
        addr.port = (enet_uint16)port_;
        serverPeer_ = enet_host_connect(enetHost_, &addr, 2, 0);
        if (!serverPeer_) netErr("connect failed");
        else              netLog("connecting");
    }

    u32   nextId = 1;
    DWORD lastConnectAttempt = GetTickCount();

    while (!stopFlag_) {
        // Client reconnect: if we have no live connection, retry every 2 s so
        // dropping/relaunching the host re-establishes.
        if (!isHost_) {
            bool disconnected =
                (serverPeer_ == 0) ||
                (serverPeer_->state == ENET_PEER_STATE_DISCONNECTED) ||
                (serverPeer_->state == ENET_PEER_STATE_ZOMBIE);
            DWORD now = GetTickCount();
            if (disconnected && (now - lastConnectAttempt) >= 2000) {
                lastConnectAttempt = now;
                if (serverPeer_) { enet_peer_reset(serverPeer_); serverPeer_ = 0; }
                ENetAddress addr;
                enet_address_set_host(&addr, ip_.c_str());
                addr.port = (enet_uint16)port_;
                serverPeer_ = enet_host_connect(enetHost_, &addr, 2, 0);
                netLog(serverPeer_ ? "reconnecting" : "reconnect failed");
            }
        }

        ENetEvent ev;
        while (enet_host_service(enetHost_, &ev, TICK_MS) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    if (isHost_) {
                        // Wait for the client's HELLO before assigning an id, so
                        // a version mismatch is rejected before we admit it.
                        netLog("peer connecting (awaiting HELLO)");
                    } else {
                        // Introduce ourselves with our protocol version.
                        HelloPacket h;
                        h.type = (u8)PKT_HELLO; h.version = PROTOCOL_VERSION; h.nameLen = 0;
                        ENetPacket* out = enet_packet_create(&h, sizeof(h), ENET_PACKET_FLAG_RELIABLE);
                        enet_peer_send(ev.peer, CH_RELIABLE, out);
                        netLog("connected to host; sent HELLO");
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const u8 type = packetType(ev.packet->data, (unsigned)ev.packet->dataLength);
                    if (isHost_ && type == PKT_HELLO) {
                        HelloPacket h;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &h)) {
                            if (h.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: peer v%u, ours v%u; rejecting",
                                          (unsigned)h.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                                enet_peer_disconnect(ev.peer, 0);
                            } else {
                                u32 id = nextId++;
                                ev.peer->data = (void*)(size_t)id;
                                WelcomePacket w;
                                w.type = (u8)PKT_WELCOME; w.version = PROTOCOL_VERSION; w.playerId = id;
                                ENetPacket* out =
                                    enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send(ev.peer, CH_RELIABLE, out);
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "peer connected id=%u (proto v%u)",
                                          (unsigned)id, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                                if (inbound_) inbound_->pushConnect(id);
                            }
                        }
                    } else if (!isHost_ && type == PKT_WELCOME) {
                        WelcomePacket w;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &w)) {
                            if (w.version != PROTOCOL_VERSION) {
                                char b[128];
                                _snprintf(b, sizeof(b) - 1,
                                          "protocol mismatch: host v%u, ours v%u",
                                          (unsigned)w.version, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netErr(b);
                            } else {
                                myId_ = w.playerId;
                                char b[96];
                                _snprintf(b, sizeof(b) - 1,
                                          "peer connected id=%u (proto v%u) - received WELCOME",
                                          (unsigned)myId_, (unsigned)PROTOCOL_VERSION);
                                b[sizeof(b) - 1] = '\0';
                                netLog(b);
                                if (inbound_) inbound_->pushConnect(0); // host id = 0
                            }
                        }
                    } else if (type == PKT_ENTITY_BATCH) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(EntityBatchHeader) && inbound_) {
                            EntityBatchHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need =
                                sizeof(EntityBatchHeader) + (unsigned)hdr.count * sizeof(EntityState);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(EntityBatchHeader);
                                for (unsigned i = 0; i < hdr.count; ++i) {
                                    EntityState e;
                                    std::memcpy(&e, p + i * sizeof(EntityState), sizeof(e));
                                    deliverEntity(hdr.ownerId, e);
                                }
                            }
                        }
                    } else if (type == PKT_EVENT) {
                        // Reliable transition. Delivered immediately (NOT through the
                        // WAN-sim delay buffer): the sim models unreliable-batch loss,
                        // while the whole point of the reliable channel is that these
                        // survive that loss - so we honour their guaranteed delivery.
                        EventPacket evp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &evp)
                            && inbound_) {
                            inbound_->pushEvent(evp.ownerId, evp);
                        }
                    } else if (type == PKT_INV_SNAPSHOT) {
                        // Reliable container-contents snapshot (Phase 4a). Like
                        // PKT_EVENT, delivered immediately (not through the WAN-sim
                        // delay buffer): it rides the reliable channel precisely so a
                        // content change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(InvSnapshotHeader) && inbound_) {
                            InvSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(InvSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(InvItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(InvSnapshotHeader);
                                u32 cHand[5] = { hdr.cType, hdr.cContainer,
                                                 hdr.cContainerSerial, hdr.cIndex, hdr.cSerial };
                                const InvItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const InvItemEntry*>(p) : 0;
                                inbound_->pushInv(hdr.ownerId, cHand, items, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM) {
                        // Reliable world-item snapshot (Phase W1). Delivered immediately
                        // (not through the WAN-sim delay buffer), like the inventory
                        // snapshot - it rides the reliable channel so a ground-item
                        // change survives unreliable-batch loss.
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemSnapshotHeader) && inbound_) {
                            WorldItemSnapshotHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemSnapshotHeader)
                                          + (unsigned)hdr.count * sizeof(WorldItemEntry);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemSnapshotHeader);
                                const WorldItemEntry* items =
                                    (hdr.count > 0) ? reinterpret_cast<const WorldItemEntry*>(p) : 0;
                                inbound_->pushWorldItems(hdr.ownerId, items, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_ITEM_REMOVE) {
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(WorldItemRemoveHeader) && inbound_) {
                            WorldItemRemoveHeader hdr;
                            std::memcpy(&hdr, ev.packet->data, sizeof(hdr));
                            unsigned need = sizeof(WorldItemRemoveHeader)
                                          + (unsigned)hdr.count * sizeof(u32);
                            if (len >= need) {
                                const enet_uint8* p = ev.packet->data + sizeof(WorldItemRemoveHeader);
                                const u32* netIds =
                                    (hdr.count > 0) ? reinterpret_cast<const u32*>(p) : 0;
                                inbound_->pushWorldRemove(hdr.ownerId, netIds, hdr.count);
                            }
                        }
                    } else if (type == PKT_WORLD_DROP) {
                        // Reliable conservation drop intent (Phase W2). Delivered immediately
                        // (not via the WAN-sim buffer) like the other reliable packets.
                        WorldDropPacket wdp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wdp)
                            && inbound_) {
                            inbound_->pushWorldDrop(wdp.ownerId, wdp);
                        }
                    } else if (type == PKT_WORLD_PICKUP) {
                        // Reliable conservation pickup intent (Phase W3), mirror of the drop.
                        WorldPickupPacket wpp;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &wpp)
                            && inbound_) {
                            inbound_->pushWorldPickup(wpp.ownerId, wpp);
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    if (isHost_) {
                        u32 id = (u32)(size_t)ev.peer->data;
                        ev.peer->data = 0;
                        if (inbound_) inbound_->pushLeave(id);
                        char b[64];
                        _snprintf(b, sizeof(b) - 1, "peer disconnected id=%u", (unsigned)id);
                        b[sizeof(b) - 1] = '\0';
                        netLog(b);
                    } else {
                        serverPeer_ = 0;
                        if (inbound_) inbound_->pushLeave(OWNER_ID_ALL);
                        netLog("disconnected from host");
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Release any WAN-sim-delayed inbound entities whose arrival time has come.
        // No-op (and cheap) when the sim is disabled / nothing is pending.
        flushDelayed();

        // Drain + send any queued reliable events on CH_RELIABLE. ENet guarantees
        // delivery + ordering on that channel, so these survive the unreliable-batch
        // loss the WAN sim injects (the reliability proof the death oracle checks).
        std::vector<EventPacket> events;
        EnterCriticalSection(&outCs_);
        events.swap(outEvents_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < events.size(); ++i) {
            ENetPacket* out = enet_packet_create(&events[i], sizeof(EventPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued container-contents snapshots on CH_RELIABLE. Each
        // is [InvSnapshotHeader][InvItemEntry*count]; only enqueued on content-change,
        // so this channel stays quiet. Reliable so the change survives WAN loss.
        std::vector<OutInv> invs;
        EnterCriticalSection(&outCs_);
        invs.swap(outInv_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < invs.size(); ++i) {
            unsigned count = (unsigned)invs[i].items.size();
            if (count > INV_ITEMS_MAX) count = INV_ITEMS_MAX;
            unsigned bytes = sizeof(InvSnapshotHeader) + count * sizeof(InvItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            InvSnapshotHeader hdr;
            hdr.type             = (u8)PKT_INV_SNAPSHOT;
            hdr.ownerId          = invs[i].ownerId;
            hdr.cType            = invs[i].cHand[0];
            hdr.cContainer       = invs[i].cHand[1];
            hdr.cContainerSerial = invs[i].cHand[2];
            hdr.cIndex           = invs[i].cHand[3];
            hdr.cSerial          = invs[i].cHand[4];
            hdr.count            = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &invs[i].items[0],
                            count * sizeof(InvItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued world-item snapshots on CH_RELIABLE (Phase W1). Each
        // is [WorldItemSnapshotHeader][WorldItemEntry*count]; only enqueued for new/
        // changed ground items, so a settled world produces no traffic. Host-authored.
        std::vector<OutWorldItems> wis;
        std::vector<OutWorldRemove> wrs;
        EnterCriticalSection(&outCs_);
        wis.swap(outWorldItems_);
        wrs.swap(outWorldRemove_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < wis.size(); ++i) {
            unsigned count = (unsigned)wis[i].items.size();
            if (count > WORLD_ITEMS_MAX) count = WORLD_ITEMS_MAX;
            unsigned bytes = sizeof(WorldItemSnapshotHeader) + count * sizeof(WorldItemEntry);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemSnapshotHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM;
            hdr.ownerId = wis[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wis[i].items[0],
                            count * sizeof(WorldItemEntry));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }
        // Drain + send any queued world-item culls on CH_RELIABLE (Phase W1).
        for (size_t i = 0; i < wrs.size(); ++i) {
            unsigned count = (unsigned)wrs[i].netIds.size();
            if (count > 255) count = 255;
            unsigned bytes = sizeof(WorldItemRemoveHeader) + count * sizeof(u32);
            ENetPacket* out = enet_packet_create(0, bytes, ENET_PACKET_FLAG_RELIABLE);
            WorldItemRemoveHeader hdr;
            hdr.type    = (u8)PKT_WORLD_ITEM_REMOVE;
            hdr.ownerId = wrs[i].ownerId;
            hdr.count   = (u8)count;
            std::memcpy(out->data, &hdr, sizeof(hdr));
            if (count > 0)
                std::memcpy(out->data + sizeof(hdr), &wrs[i].netIds[0], count * sizeof(u32));
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued conservation DROP intents on CH_RELIABLE (Phase W2). A
        // fixed-size POD per drop, like an event; reliable so a drop is never lost.
        std::vector<WorldDropPacket> drops;
        EnterCriticalSection(&outCs_);
        drops.swap(outWorldDrops_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < drops.size(); ++i) {
            ENetPacket* out = enet_packet_create(&drops[i], sizeof(WorldDropPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Drain + send any queued conservation PICKUP intents on CH_RELIABLE (Phase W3).
        std::vector<WorldPickupPacket> pickups;
        EnterCriticalSection(&outCs_);
        pickups.swap(outWorldPickups_);
        LeaveCriticalSection(&outCs_);
        for (size_t i = 0; i < pickups.size(); ++i) {
            ENetPacket* out = enet_packet_create(&pickups[i], sizeof(WorldPickupPacket),
                                                 ENET_PACKET_FLAG_RELIABLE);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_RELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_RELIABLE, out);
            } else {
                enet_packet_destroy(out);
            }
        }

        // Transmit this peer's owned entities (latest snapshot), chunked so each
        // batch fits one datagram. Unreliable: the newest batch supersedes loss.
        std::vector<EntityState> ents;
        u32  owner = 0;
        bool have  = false;
        EnterCriticalSection(&outCs_);
        ents  = out_;
        owner = outOwner_;
        have  = haveOut_;
        LeaveCriticalSection(&outCs_);

        if (have && !ents.empty()) {
            for (size_t off = 0; off < ents.size(); off += ENTITY_BATCH_MAX) {
                unsigned count = (unsigned)(ents.size() - off);
                if (count > ENTITY_BATCH_MAX) count = ENTITY_BATCH_MAX;

                unsigned bytes = sizeof(EntityBatchHeader) + count * sizeof(EntityState);
                ENetPacket* out = enet_packet_create(0, bytes, 0 /*unreliable*/);
                EntityBatchHeader hdr;
                hdr.type = (u8)PKT_ENTITY_BATCH; hdr.ownerId = owner; hdr.count = (u8)count;
                std::memcpy(out->data, &hdr, sizeof(hdr));
                std::memcpy(out->data + sizeof(hdr), &ents[off], count * sizeof(EntityState));
                if (isHost_) {
                    enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
                } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                    enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
                } else {
                    enet_packet_destroy(out); // no one to send to yet
                }
            }
        }
    }

    if (enetHost_) { enet_host_destroy(enetHost_); enetHost_ = 0; }
    InterlockedExchange(&running_, 0);
}

} // namespace coop
