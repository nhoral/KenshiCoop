#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "NetClient.h"
#include "CoopLog.h"

#include <cstring>
#include <cstdio>

namespace coop {

namespace {
const int        TICK_MS        = 50; // 20 Hz service/transmit cadence
const enet_uint8 CH_RELIABLE    = 0;
const enet_uint8 CH_UNRELIABLE  = 1;

// Net-thread diagnostics. OutputDebugStringA is thread-safe, unlike the
// KenshiLib UI/log helpers, so it is safe to call off the main thread. We also
// mirror into the dedicated coop log (CoopLog guards its FILE* with a lock, so
// it is safe to call from this background thread).
void netLog(const char* msg) {
    OutputDebugStringA("[KenshiCoop/net] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, "[net] %s", msg ? msg : "");
    buf[sizeof(buf) - 1] = '\0';
    coop::logLine(buf);
}
} // namespace

NetClient::NetClient()
    : isHost_(false), port_(0),
      enetHost_(0), serverPeer_(0),
      inbound_(0), haveLocal_(false),
      thread_(0), running_(0), stopFlag_(0), myId_(0) {
    InitializeCriticalSection(&localCs_);
    InitializeCriticalSection(&npcCs_);
    std::memset(&localState_, 0, sizeof(localState_));
}

NetClient::~NetClient() {
    stop();
    DeleteCriticalSection(&npcCs_);
    DeleteCriticalSection(&localCs_);
}

bool NetClient::startHost(int port, MainThreadQueue* inbound) {
    isHost_  = true;
    port_    = port;
    inbound_ = inbound;
    myId_    = 0; // host is always player 0
    return launchThread();
}

bool NetClient::startClient(const std::string& ip, int port, MainThreadQueue* inbound) {
    isHost_  = false;
    ip_      = ip;
    port_    = port;
    inbound_ = inbound;
    myId_    = 0; // assigned by host via WELCOME
    return launchThread();
}

bool NetClient::launchThread() {
    if (enet_initialize() != 0) {
        netLog("enet_initialize failed");
        return false;
    }
    stopFlag_ = 0;
    thread_ = CreateThread(0, 0, &NetClient::threadEntry, this, 0, 0);
    if (thread_ == 0) {
        netLog("CreateThread failed");
        enet_deinitialize();
        return false;
    }
    return true;
}

void NetClient::stop() {
    if (thread_) {
        InterlockedExchange(&stopFlag_, 1);
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = 0;
        enet_deinitialize();
    }
}

void NetClient::setLocalState(const PlayerStatePacket& p) {
    EnterCriticalSection(&localCs_);
    localState_ = p;
    haveLocal_  = true;
    LeaveCriticalSection(&localCs_);
}

void NetClient::setNpcStates(const NpcStateEntry* arr, unsigned int count) {
    EnterCriticalSection(&npcCs_);
    npcOut_.assign(arr, arr + count);
    LeaveCriticalSection(&npcCs_);
}

DWORD WINAPI NetClient::threadEntry(LPVOID self) {
    reinterpret_cast<NetClient*>(self)->threadLoop();
    return 0;
}

void NetClient::threadLoop() {
    InterlockedExchange(&running_, 1);

    if (isHost_) {
        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = (enet_uint16)port_;
        enetHost_ = enet_host_create(&addr, 8 /*peers*/, 2 /*channels*/, 0, 0);
        if (!enetHost_) { netLog("host create failed"); InterlockedExchange(&running_, 0); return; }
        netLog("hosting");
    } else {
        enetHost_ = enet_host_create(0, 1, 2, 0, 0);
        if (!enetHost_) { netLog("client create failed"); InterlockedExchange(&running_, 0); return; }
        ENetAddress addr;
        enet_address_set_host(&addr, ip_.c_str());
        addr.port = (enet_uint16)port_;
        serverPeer_ = enet_host_connect(enetHost_, &addr, 2, 0);
        if (!serverPeer_) { netLog("connect failed"); }
        else              { netLog("connecting"); }
    }

    u32 nextId = 1;
    DWORD lastConnectAttempt = GetTickCount();

    while (!stopFlag_) {
        // Client only: if we have no live connection to the host, retry every
        // couple of seconds so dropping/relaunching the host re-establishes.
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
                        u32 id = nextId++;
                        ev.peer->data = (void*)(size_t)id;
                        WelcomePacket w;
                        w.type = PKT_WELCOME; w.playerId = id;
                        ENetPacket* out =
                            enet_packet_create(&w, sizeof(w), ENET_PACKET_FLAG_RELIABLE);
                        enet_peer_send(ev.peer, CH_RELIABLE, out);
                        netLog("peer connected");
                    } else {
                        netLog("connected to host");
                    }
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    u8 type = packetType(ev.packet->data, (unsigned)ev.packet->dataLength);
                    if (type == PKT_PLAYER_STATE) {
                        PlayerStatePacket p;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &p)
                            && inbound_) {
                            inbound_->push(p); // hand off to game thread
                        }
                    } else if (type == PKT_WELCOME) {
                        WelcomePacket w;
                        if (readPacket(ev.packet->data, (unsigned)ev.packet->dataLength, &w)) {
                            myId_ = w.playerId; // simple write; game thread only reads
                            netLog("received WELCOME");
                        }
                    } else if (type == PKT_NPC_STATE) {
                        // [u8 type][u8 count][NpcStateEntry * count]
                        const unsigned len = (unsigned)ev.packet->dataLength;
                        if (len >= sizeof(NpcBatchHeader) && inbound_) {
                            NpcBatchHeader h;
                            std::memcpy(&h, ev.packet->data, sizeof(h));
                            unsigned need =
                                sizeof(NpcBatchHeader) + (unsigned)h.count * sizeof(NpcStateEntry);
                            if (len >= need) {
                                const enet_uint8* p =
                                    ev.packet->data + sizeof(NpcBatchHeader);
                                for (unsigned i = 0; i < h.count; ++i) {
                                    NpcStateEntry e;
                                    std::memcpy(&e, p + i * sizeof(NpcStateEntry), sizeof(e));
                                    inbound_->pushNpc(e);
                                }
                            }
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    // Tell the game thread to despawn the relevant ghost(s) via
                    // the existing inbound queue (a PKT_PLAYER_LEFT marker).
                    PlayerStatePacket left;
                    std::memset(&left, 0, sizeof(left));
                    left.type = PKT_PLAYER_LEFT;
                    if (isHost_) {
                        left.playerId = (u32)(size_t)ev.peer->data;
                        ev.peer->data = 0;
                        netLog("peer disconnected");
                    } else {
                        // Lost the host: clear every remote ghost.
                        left.playerId = PLAYER_ID_ALL;
                        serverPeer_   = 0;
                        netLog("disconnected from host");
                    }
                    if (inbound_) inbound_->push(left);
                    break;
                }
                default:
                    break;
            }
        }

        // Transmit our most recent local state, if the game thread has set it.
        PlayerStatePacket local;
        bool have;
        EnterCriticalSection(&localCs_);
        local = localState_;
        have  = haveLocal_;
        LeaveCriticalSection(&localCs_);

        if (have) {
            local.type     = PKT_PLAYER_STATE;
            local.playerId = myId_;
            ENetPacket* out = enet_packet_create(&local, sizeof(local), 0 /*unreliable*/);
            if (isHost_) {
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            } else if (serverPeer_ && serverPeer_->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_send(serverPeer_, CH_UNRELIABLE, out);
            } else {
                enet_packet_destroy(out); // no one to send to yet
            }
        }

        // Host only: broadcast the latest NPC transforms, chunked into batches
        // that fit one datagram. Unreliable: the newest batch supersedes losses.
        if (isHost_) {
            std::vector<NpcStateEntry> npcs;
            EnterCriticalSection(&npcCs_);
            npcs = npcOut_; // copy, so we keep re-broadcasting between gathers
            LeaveCriticalSection(&npcCs_);

            for (size_t off = 0; off < npcs.size(); off += NPC_BATCH_MAX) {
                unsigned count = (unsigned)(npcs.size() - off);
                if (count > NPC_BATCH_MAX) count = NPC_BATCH_MAX;

                unsigned bytes =
                    sizeof(NpcBatchHeader) + count * sizeof(NpcStateEntry);
                ENetPacket* out = enet_packet_create(0, bytes, 0 /*unreliable*/);
                NpcBatchHeader h;
                h.type  = PKT_NPC_STATE;
                h.count = (u8)count;
                std::memcpy(out->data, &h, sizeof(h));
                std::memcpy(out->data + sizeof(h), &npcs[off],
                            count * sizeof(NpcStateEntry));
                enet_host_broadcast(enetHost_, CH_UNRELIABLE, out);
            }
        }
    }

    if (enetHost_) {
        enet_host_destroy(enetHost_);
        enetHost_ = 0;
    }
    InterlockedExchange(&running_, 0);
}

} // namespace coop
