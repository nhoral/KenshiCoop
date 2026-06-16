// nettest - standalone ENet host/join harness (Milestone 3).
//
// Purpose: de-risk the networking layer with zero Kenshi build dependencies.
// Two processes connect over ENet and exchange PlayerStatePackets, proving that
// host/join, channels, and the shared serialization in netproto/Protocol.h all
// round-trip correctly.
//
//   nettest host <port>            [baseX baseY baseZ]
//   nettest join <ip> <port>       [baseX baseY baseZ]
//
// Both sides spin a simple loop: every ~50ms (20 Hz, matching our planned tick)
// they send a PlayerStatePacket with a moving fake position, and they print any
// packets they receive.
//
// If base X/Y/Z are supplied, the fake position slowly orbits that point in a
// small circle (radius ~5) instead of using the default far-away path. Pass
// your in-game character's coordinates (see "local player pos" in the RE_Kenshi
// log) so the spawned ghost appears right next to you and visibly circles.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include <enet/enet.h>

#include "../netproto/Protocol.h"

namespace {

const int    TICK_MS   = 50;     // 20 Hz
const enet_uint8 CH_RELIABLE   = 0;
const enet_uint8 CH_UNRELIABLE = 1;

// Optional orbit center (the player's in-game position). When set, fake states
// circle this point so a spawned ghost is visible right beside the character.
bool  g_haveBase = false;
float g_baseX = 0.0f, g_baseY = 0.0f, g_baseZ = 0.0f;
const float ORBIT_RADIUS = 5.0f;

void printPlayerState(const coop::PlayerStatePacket& p) {
    std::printf("  <- PLAYER_STATE id=%u tick=%u pos=(%.2f, %.2f, %.2f) hdg=%.2f\n",
                p.playerId, p.tick, p.x, p.y, p.z, p.heading);
}

// Dispatch a received buffer.
void handlePacket(const ENetPacket* pkt) {
    coop::u8 type = coop::packetType(pkt->data, (unsigned)pkt->dataLength);
    switch (type) {
        case coop::PKT_PLAYER_STATE: {
            coop::PlayerStatePacket p;
            if (coop::readPacket(pkt->data, (unsigned)pkt->dataLength, &p))
                printPlayerState(p);
            break;
        }
        case coop::PKT_HELLO:
            std::printf("  <- HELLO\n");
            break;
        case coop::PKT_WELCOME: {
            coop::WelcomePacket w;
            if (coop::readPacket(pkt->data, (unsigned)pkt->dataLength, &w))
                std::printf("  <- WELCOME playerId=%u\n", w.playerId);
            break;
        }
        default:
            std::printf("  <- unknown packet type=%u len=%u\n",
                        type, (unsigned)pkt->dataLength);
            break;
    }
}

void sendPlayerState(ENetPeer* peer, coop::u32 playerId, coop::u32 tick) {
    coop::PlayerStatePacket p;
    std::memset(&p, 0, sizeof(p));
    p.type     = coop::PKT_PLAYER_STATE;
    p.playerId = playerId;
    p.tick     = tick;
    if (g_haveBase) {
        // Slow circle around the player's position so the ghost orbits visibly,
        // facing its direction of travel (tangent = t + 90deg) so it looks like
        // it is walking the circle rather than spinning in place.
        float t   = (float)tick * 0.03f;
        p.x       = g_baseX + ORBIT_RADIUS * std::cos(t);
        p.y       = g_baseY;
        p.z       = g_baseZ + ORBIT_RADIUS * std::sin(t);
        p.heading = t + 1.5708f;
    } else {
        // Default far-away fake motion (values just change on the other end).
        p.x       = 100.0f + (float)tick;
        p.y       = 0.0f;
        p.z       = 200.0f + 5.0f * std::sin((float)tick * 0.1f);
        p.heading = 0.0f;
    }

    ENetPacket* out = enet_packet_create(&p, sizeof(p), 0 /* unreliable */);
    enet_peer_send(peer, CH_UNRELIABLE, out);
}

int runHost(int port) {
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = (enet_uint16)port;

    ENetHost* host = enet_host_create(&addr, 4 /*peers*/, 2 /*channels*/, 0, 0);
    if (!host) { std::fprintf(stderr, "host create failed\n"); return 1; }
    std::printf("[host] listening on port %d\n", port);

    coop::u32 tick = 0;
    coop::u32 nextId = 1;
    for (;;) {
        ENetEvent ev;
        // Service the network for up to one tick worth of time.
        while (enet_host_service(host, &ev, TICK_MS) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    coop::u32 id = nextId++;
                    ev.peer->data = (void*)(size_t)id;
                    std::printf("[host] client connected, assigned id=%u\n", id);
                    coop::WelcomePacket w;
                    w.type = coop::PKT_WELCOME; w.playerId = id;
                    ENetPacket* out = enet_packet_create(&w, sizeof(w),
                                                          ENET_PACKET_FLAG_RELIABLE);
                    enet_peer_send(ev.peer, CH_RELIABLE, out);
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                    handlePacket(ev.packet);
                    enet_packet_destroy(ev.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    std::printf("[host] client disconnected\n");
                    break;
                default: break;
            }
        }
        // Broadcast our (host's) own fake player state every tick.
        ENetPeer* p = host->peers;
        for (size_t i = 0; i < host->peerCount; ++i) {
            if (p[i].state == ENET_PEER_STATE_CONNECTED)
                sendPlayerState(&p[i], 0 /*host is player 0*/, tick);
        }
        ++tick;
    }
}

int runClient(const char* ip, int port) {
    ENetHost* client = enet_host_create(0, 1, 2, 0, 0);
    if (!client) { std::fprintf(stderr, "client create failed\n"); return 1; }

    ENetAddress addr;
    enet_address_set_host(&addr, ip);
    addr.port = (enet_uint16)port;

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    if (!peer) { std::fprintf(stderr, "no peer\n"); return 1; }
    std::printf("[client] connecting to %s:%d ...\n", ip, port);

    coop::u32 tick = 0;
    coop::u32 myId = 0;
    for (;;) {
        ENetEvent ev;
        while (enet_host_service(client, &ev, TICK_MS) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    std::printf("[client] connected\n");
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    if (coop::packetType(ev.packet->data,
                                         (unsigned)ev.packet->dataLength)
                        == coop::PKT_WELCOME) {
                        coop::WelcomePacket w;
                        if (coop::readPacket(ev.packet->data,
                                             (unsigned)ev.packet->dataLength, &w))
                            myId = w.playerId;
                    }
                    handlePacket(ev.packet);
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                    std::printf("[client] disconnected\n");
                    return 0;
                default: break;
            }
        }
        if (peer->state == ENET_PEER_STATE_CONNECTED)
            sendPlayerState(peer, myId, tick);
        ++tick;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage:\n"
            "  nettest host <port> [baseX baseY baseZ]\n"
            "  nettest join <ip> <port> [baseX baseY baseZ]\n");
        return 2;
    }
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    atexit(enet_deinitialize);

    if (std::strcmp(argv[1], "host") == 0 && argc >= 3) {
        if (argc >= 6) {
            g_haveBase = true;
            g_baseX = (float)std::atof(argv[3]);
            g_baseY = (float)std::atof(argv[4]);
            g_baseZ = (float)std::atof(argv[5]);
        }
        return runHost(std::atoi(argv[2]));
    } else if (std::strcmp(argv[1], "join") == 0 && argc >= 4) {
        if (argc >= 7) {
            g_haveBase = true;
            g_baseX = (float)std::atof(argv[4]);
            g_baseY = (float)std::atof(argv[5]);
            g_baseZ = (float)std::atof(argv[6]);
        }
        return runClient(argv[2], std::atoi(argv[3]));
    }
    std::fprintf(stderr, "bad args\n");
    return 2;
}
