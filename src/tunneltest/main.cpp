// tunneltest - proves the ENet-over-socket-hooks tunnel (patch 0002) carries
// the FULL ENet protocol under Steam P2P's constraints, in one process, with
// no game and no Steam:
//
//   * a real ENet server + client are created over the hook seam (no UDP
//     sockets exist); datagrams travel through in-memory queues that model the
//     Steam pipe: hard 1200-byte datagram ceiling (Steam's unreliable P2P
//     packet cap - anything larger is REJECTED, exactly like SendP2PPacket
//     would) and deterministic packet loss;
//   * the connect handshake, both channels, reliable delivery under loss,
//     and reliable FRAGMENTATION of packets far larger than one datagram
//     (inventory/medical snapshots) must all survive.
//
// This is the single-machine stand-in for the parts of the Steam transport
// that a self-ping cannot exercise. What it cannot prove: the NAT punch /
// relay brokering itself (needs two Steam accounts on two networks).
//
// VC10 / C++03, same toolchain as the plugin.

#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here

#include <enet/enet.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (ok) ++g_pass; else ++g_fail;
}

// ---- The fake Steam pipe -------------------------------------------------------
// Two endpoints (server socket 1001, client socket 1002). Each datagram is an
// opaque byte blob; delivery enforces the Steam unreliable-P2P constraints.

static const ENetSocket SOCK_SERVER = (ENetSocket)1001;
static const ENetSocket SOCK_CLIENT = (ENetSocket)1002;
static const unsigned short SERVER_PORT = 27800;
static const int STEAM_MAX_DATAGRAM = 1200; // Steam unreliable P2P packet cap

struct Datagram { std::vector<unsigned char> bytes; };
static std::deque<Datagram> g_toServer, g_toClient;

static int g_created = 0;          // create-order: server first, then client
static unsigned int g_sent = 0;    // total datagrams offered to the pipe
static unsigned int g_dropped = 0; // datagrams eaten by simulated loss
static unsigned int g_oversize = 0;// datagrams REJECTED for busting the cap
static unsigned int g_maxSeen = 0; // largest accepted datagram
static unsigned int g_lossPct = 0; // simulated loss (deterministic PRNG)

static unsigned int nextRand() { // xorshift32, deterministic run-to-run
    static unsigned int s = 0xC0FFEEu;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static ENetSocket ENET_CALLBACK hookCreate(ENetSocketType type) {
    (void)type;
    ++g_created;
    return (g_created == 1) ? SOCK_SERVER : SOCK_CLIENT;
}

static int ENET_CALLBACK hookBind(ENetSocket s, const ENetAddress* a) {
    (void)s; (void)a;
    return 0;
}

static int ENET_CALLBACK hookSend(ENetSocket s, const ENetAddress* addr,
                                  const ENetBuffer* buffers, size_t count) {
    unsigned char buf[8192];
    unsigned int len = 0;
    size_t i;
    (void)addr; // single peer each way, like the Steam tunnel
    for (i = 0; i < count; ++i) {
        if (len + buffers[i].dataLength > sizeof(buf)) return -1;
        std::memcpy(buf + len, buffers[i].data, buffers[i].dataLength);
        len += (unsigned int)buffers[i].dataLength;
    }
    ++g_sent;
    if ((int)len > STEAM_MAX_DATAGRAM) { ++g_oversize; return -1; } // Steam would reject
    if (len > g_maxSeen) g_maxSeen = len;
    if (g_lossPct > 0 && (nextRand() % 100u) < g_lossPct) { ++g_dropped; return (int)len; }
    Datagram d;
    d.bytes.assign(buf, buf + len);
    if (s == SOCK_SERVER) g_toClient.push_back(d); else g_toServer.push_back(d);
    return (int)len;
}

static int ENET_CALLBACK hookReceive(ENetSocket s, ENetAddress* addr,
                                     ENetBuffer* buffers, size_t count) {
    std::deque<Datagram>& q = (s == SOCK_SERVER) ? g_toServer : g_toClient;
    if (q.empty() || count < 1) return 0;
    Datagram& d = q.front();
    unsigned int n = (unsigned int)d.bytes.size();
    if (n > buffers[0].dataLength) { q.pop_front(); return -2; }
    if (n > 0) std::memcpy(buffers[0].data, &d.bytes[0], n);
    if (addr != 0) {
        // Fabricated identities, mirroring SteamP2P's fake ENetAddress: the
        // client must see the exact address it connected to.
        if (s == SOCK_CLIENT) { addr->host = 0x01000001u; addr->port = SERVER_PORT; }
        else                  { addr->host = 0x01000002u; addr->port = 34567; }
    }
    q.pop_front();
    return (int)n;
}

static int ENET_CALLBACK hookWait(ENetSocket s, enet_uint32* condition, enet_uint32 timeout) {
    // Single-threaded pump: never actually blocks (both hosts share the thread).
    enet_uint32 want = *condition;
    (void)timeout;
    *condition = ENET_SOCKET_WAIT_NONE;
    if (want & ENET_SOCKET_WAIT_SEND) *condition |= ENET_SOCKET_WAIT_SEND;
    if ((want & ENET_SOCKET_WAIT_RECEIVE) &&
        !((s == SOCK_SERVER) ? g_toServer.empty() : g_toClient.empty()))
        *condition |= ENET_SOCKET_WAIT_RECEIVE;
    return 0;
}

static void ENET_CALLBACK hookDestroy(ENetSocket s) { (void)s; }

// ---- Test driver ----------------------------------------------------------------

struct Got {
    bool connected, disconnected;
    std::vector<unsigned char> lastPacket;
    enet_uint8 lastChannel;
    unsigned int packets;
    Got() : connected(false), disconnected(false), lastChannel(0xFF), packets(0) {}
};

static void pump(ENetHost* h, Got& got) {
    ENetEvent ev;
    while (enet_host_service(h, &ev, 0) > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:    got.connected = true; break;
        case ENET_EVENT_TYPE_DISCONNECT: got.disconnected = true; break;
        case ENET_EVENT_TYPE_RECEIVE:
            got.lastPacket.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            got.lastChannel = ev.channelID;
            ++got.packets;
            enet_packet_destroy(ev.packet);
            break;
        default: break;
        }
    }
}

int main() {
    std::printf("tunneltest: ENet protocol over the socket-hook seam "
                "(Steam pipe model: %d-byte datagram cap + loss)\n", STEAM_MAX_DATAGRAM);

    check(enet_initialize() == 0, "enet_initialize");

    ENetSocketHooks hooks;
    std::memset(&hooks, 0, sizeof(hooks));
    hooks.socket_create  = &hookCreate;
    hooks.socket_bind    = &hookBind;
    hooks.socket_send    = &hookSend;
    hooks.socket_receive = &hookReceive;
    hooks.socket_wait    = &hookWait;
    hooks.socket_destroy = &hookDestroy;
    enet_set_socket_hooks(&hooks);

    ENetAddress bindAddr; bindAddr.host = ENET_HOST_ANY; bindAddr.port = SERVER_PORT;
    ENetHost* server = enet_host_create(&bindAddr, 8, 2, 0, 0);
    check(server != 0, "server enet_host_create over hooks (fake socket)");
    ENetHost* client = enet_host_create(0, 1, 2, 0, 0);
    check(client != 0, "client enet_host_create over hooks");
    if (!server || !client) return 1;

    // The NetLink clamp: every ENet datagram must fit one Steam P2P packet.
    server->mtu = STEAM_MAX_DATAGRAM;
    client->mtu = STEAM_MAX_DATAGRAM;

    ENetAddress connectAddr; connectAddr.host = 0x01000001u; connectAddr.port = SERVER_PORT;
    ENetPeer* peer = enet_host_connect(client, &connectAddr, 2, 0);
    check(peer != 0, "enet_host_connect to fake address");
    if (peer) peer->mtu = STEAM_MAX_DATAGRAM;

    Got sGot, cGot;
    int i;

    // 1) Handshake with a clean pipe.
    for (i = 0; i < 2000 && !(sGot.connected && cGot.connected); ++i) {
        pump(server, sGot); pump(client, cGot); Sleep(0);
    }
    check(cGot.connected, "client CONNECT event (handshake over tunnel)");
    check(sGot.connected, "server CONNECT event");

    // 2) Unreliable channel 0, client -> server (the entity-stream shape).
    {
        const char* msg = "entity-batch-stand-in";
        enet_peer_send(peer, 0, enet_packet_create(msg, std::strlen(msg) + 1, 0));
        for (i = 0; i < 2000 && sGot.packets < 1; ++i) { pump(client, cGot); pump(server, sGot); Sleep(0); }
        check(sGot.packets >= 1, "unreliable ch0 datagram delivered");
        check(sGot.packets >= 1 && sGot.lastChannel == 0, "  ... on channel 0");
        check(sGot.packets >= 1 &&
              std::strcmp((const char*)&sGot.lastPacket[0], msg) == 0, "  ... payload intact");
    }

    // 3) Reliable channel 1, server -> client: a packet ~7x the datagram cap
    //    (inventory-snapshot shape), under 15% loss. ENet must fragment every
    //    piece under 1200 bytes and retransmit through the drops. ENet's
    //    retransmit timers are WALL-CLOCK based, so this loop must let real
    //    time pass (Sleep(1)) for the RTO to ever fire.
    {
        std::vector<unsigned char> big(8000);
        size_t k;
        for (k = 0; k < big.size(); ++k) big[k] = (unsigned char)(k * 31u + 7u);
        g_lossPct = 15;
        unsigned int sentBefore = g_sent;
        enet_host_broadcast(server, 1, enet_packet_create(&big[0], big.size(), ENET_PACKET_FLAG_RELIABLE));
        DWORD deadline = GetTickCount() + 20000;
        while (GetTickCount() < deadline && cGot.packets < 1) {
            pump(server, sGot); pump(client, cGot); Sleep(1);
        }
        g_lossPct = 0;
        check(cGot.packets >= 1, "reliable 8000-byte packet delivered under 15% loss");
        check(cGot.packets >= 1 && cGot.lastChannel == 1, "  ... on channel 1");
        bool same = (cGot.lastPacket.size() == big.size()) &&
                    (std::memcmp(&cGot.lastPacket[0], &big[0], big.size()) == 0);
        check(same, "  ... reassembled byte-for-byte from fragments");
        check(g_sent - sentBefore > 7, "  ... actually fragmented (>7 datagrams for 8000 bytes)");
        check(g_dropped > 0, "  ... loss actually exercised (retransmits happened)");
    }

    // 4) The Steam-cap contract: with the MTU clamp in place, NOTHING ENet
    //    emitted ever exceeded one P2P packet.
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "no datagram exceeded %d bytes (max seen %u, %u sent)",
                  STEAM_MAX_DATAGRAM, g_maxSeen, g_sent);
        b[sizeof(b) - 1] = '\0';
        check(g_oversize == 0, b);
    }

    // 5) Clean disconnect over the tunnel.
    {
        enet_peer_disconnect(peer, 0);
        for (i = 0; i < 2000 && !(cGot.disconnected && sGot.disconnected); ++i) {
            pump(server, sGot); pump(client, cGot); Sleep(0);
        }
        check(cGot.disconnected, "client DISCONNECT event");
        check(sGot.disconnected, "server DISCONNECT event");
    }

    enet_host_destroy(client);
    enet_host_destroy(server);
    enet_set_socket_hooks(0);
    enet_deinitialize();

    std::printf("\ntunneltest: %d/%d checks passed - %s\n",
                g_pass, g_pass + g_fail, g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
