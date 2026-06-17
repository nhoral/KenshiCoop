#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetLink.h"
#include "../CoopLog.h"

#include <cstring>
#include <cstdio>

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
      thread_(0), running_(0), stopFlag_(0), myId_(0) {
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
                                    inbound_->pushEntity(hdr.ownerId, e);
                                }
                            }
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
