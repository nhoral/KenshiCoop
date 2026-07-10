// netsim - a WAN-conditions UDP relay proxy for KenshiCoop validation.
//
// Sits BETWEEN the join client and the host:
//     join  ->  127.0.0.1:listenPort  ->  netsim  ->  hostIp:hostPort
// and applies base delay + uniform jitter + percentage loss to EVERY datagram
// in BOTH directions - connection handshake, reliable channel, unreliable
// channel, all packet families alike.
//
// This is the crucial difference from the in-plugin KENSHICOOP_NETSIM_* sim
// (which delays/drops only received ENTITY batches, above ENet): here the loss
// happens BELOW ENet, so ENet's real retransmission/ordering machinery is
// engaged - a dropped reliable datagram is genuinely re-sent and arrives LATE,
// which is exactly the behaviour a real internet link produces and the thing
// loopback runs could never exercise. Jitter naturally reorders datagrams.
//
// Usage:
//   netsim <listenPort> <hostIp> <hostPort> <delayMs> <jitterMs> <lossPct> [seed [stallAtS stallForS]]
//
// stallAtS/stallForS (starved-replica validation): starting stallAtS seconds
// after the FIRST join datagram (i.e. session-relative, not process-relative,
// so game boot time doesn't eat the window), drop EVERY datagram in BOTH
// directions for stallForS seconds - a scripted total outage that starves the
// entity stream past the interp staleMs and exercises the guard-hold path.
//
// Single-client by design (the harness runs one join); stats print every 5 s.
// Stop with Ctrl+C or by killing the process (run_test.ps1 does the latter).

#define _CRT_SECURE_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN 1

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#pragma comment(lib, "ws2_32.lib")

namespace {

const int MAX_DGRAM = 4096; // ENet datagrams are ~1400 B; leave headroom.

struct Held {
    DWORD        releaseTick;
    int          len;
    bool         toHost;             // true: join->host, false: host->join
    unsigned char data[MAX_DGRAM];
};

unsigned long g_fwdUp = 0, g_fwdDown = 0, g_dropUp = 0, g_dropDown = 0;
unsigned long g_heldMax = 0;

int uniformJitter(int jitterMs) {
    if (jitterMs <= 0) return 0;
    return (std::rand() % (2 * jitterMs + 1)) - jitterMs;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
            "usage: netsim <listenPort> <hostIp> <hostPort> <delayMs> <jitterMs> <lossPct> [seed]\n");
        return 2;
    }
    const int listenPort = std::atoi(argv[1]);
    const char* hostIp   = argv[2];
    const int hostPort   = std::atoi(argv[3]);
    const int delayMs    = std::atoi(argv[4]);
    const int jitterMs   = std::atoi(argv[5]);
    const int lossPct    = std::atoi(argv[6]);
    std::srand(argc >= 8 ? (unsigned)std::atoi(argv[7]) : (unsigned)GetTickCount());
    const int stallAtS   = (argc >= 10) ? std::atoi(argv[8]) : 0;
    const int stallForS  = (argc >= 10) ? std::atoi(argv[9]) : 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "netsim: WSAStartup failed\n");
        return 1;
    }

    // clientSock: bound to listenPort; the join sends here and we reply here.
    SOCKET clientSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // hostSock: unbound; we send to the host from here and receive its replies.
    SOCKET hostSock   = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (clientSock == INVALID_SOCKET || hostSock == INVALID_SOCKET) {
        std::fprintf(stderr, "netsim: socket() failed\n");
        return 1;
    }

    sockaddr_in bindAddr;
    std::memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons((unsigned short)listenPort);
    if (bind(clientSock, (sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
        std::fprintf(stderr, "netsim: bind(%d) failed (%d)\n", listenPort, WSAGetLastError());
        return 1;
    }

    sockaddr_in hostAddr;
    std::memset(&hostAddr, 0, sizeof(hostAddr));
    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons((unsigned short)hostPort);
    hostAddr.sin_addr.s_addr = inet_addr(hostIp);
    if (hostAddr.sin_addr.s_addr == INADDR_NONE) {
        std::fprintf(stderr, "netsim: bad host ip '%s'\n", hostIp);
        return 1;
    }

    // Non-blocking: the ingest loops drain each socket until it is empty.
    u_long nb = 1;
    ioctlsocket(clientSock, FIONBIO, &nb);
    nb = 1;
    ioctlsocket(hostSock, FIONBIO, &nb);

    sockaddr_in clientAddr;          // learned from the first join datagram
    std::memset(&clientAddr, 0, sizeof(clientAddr));
    bool haveClient = false;

    std::printf("netsim: relaying :%d <-> %s:%d  delay=%dms jitter=+/-%dms loss=%d%%",
                listenPort, hostIp, hostPort, delayMs, jitterMs, lossPct);
    if (stallForS > 0)
        std::printf("  stall=%ds@+%ds", stallForS, stallAtS);
    std::printf("\n");
    std::fflush(stdout);

    std::deque<Held> held;
    DWORD lastStats = GetTickCount();
    DWORD firstClientTick = 0;   // session start = first join datagram
    bool  stallAnnounced = false, stallEnded = false;
    unsigned long dropStall = 0;

    for (;;) {
        // Wait (briefly) for traffic on either socket.
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(clientSock, &rd);
        FD_SET(hostSock, &rd);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 1000; // 1 ms tick
        select(0, &rd, 0, 0, &tv);

        DWORD now = GetTickCount();

        // Scripted total-outage window (see header comment). Session-relative:
        // the clock starts at the first join datagram.
        bool stalled = false;
        if (stallForS > 0 && firstClientTick != 0) {
            DWORD rel = now - firstClientTick;
            DWORD s0 = (DWORD)stallAtS * 1000u;
            DWORD s1 = s0 + (DWORD)stallForS * 1000u;
            stalled = (rel >= s0 && rel < s1);
            if (stalled && !stallAnnounced) {
                stallAnnounced = true;
                std::printf("netsim: STALL BEGIN (t=+%lus, %ds outage)\n",
                            (unsigned long)(rel / 1000), stallForS);
                std::fflush(stdout);
            }
            if (!stalled && stallAnnounced && !stallEnded && rel >= s1) {
                stallEnded = true;
                std::printf("netsim: STALL END (dropped=%lu)\n", dropStall);
                std::fflush(stdout);
            }
        }

        // Ingest from the join side.
        if (FD_ISSET(clientSock, &rd)) {
            for (;;) {
                Held h;
                sockaddr_in from; int flen = sizeof(from);
                int n = recvfrom(clientSock, (char*)h.data, MAX_DGRAM, 0, (sockaddr*)&from, &flen);
                if (n <= 0) break;
                clientAddr = from; haveClient = true;
                if (firstClientTick == 0) firstClientTick = now;
                if (stalled) { ++dropStall; ++g_dropUp; continue; }
                if (lossPct > 0 && (std::rand() % 100) < lossPct) { ++g_dropUp; continue; }
                int d = delayMs + uniformJitter(jitterMs);
                if (d < 0) d = 0;
                h.releaseTick = now + (DWORD)d;
                h.len = n; h.toHost = true;
                held.push_back(h);
            }
        }

        // Ingest from the host side.
        if (FD_ISSET(hostSock, &rd)) {
            for (;;) {
                Held h;
                sockaddr_in from; int flen = sizeof(from);
                int n = recvfrom(hostSock, (char*)h.data, MAX_DGRAM, 0, (sockaddr*)&from, &flen);
                if (n <= 0) break;
                if (stalled) { ++dropStall; ++g_dropDown; continue; }
                if (lossPct > 0 && (std::rand() % 100) < lossPct) { ++g_dropDown; continue; }
                int d = delayMs + uniformJitter(jitterMs);
                if (d < 0) d = 0;
                h.releaseTick = now + (DWORD)d;
                h.len = n; h.toHost = false;
                held.push_back(h);
            }
        }

        if ((unsigned long)held.size() > g_heldMax) g_heldMax = (unsigned long)held.size();

        // Release matured datagrams. Jitter puts release times out of order in
        // the queue; scanning the whole (small) queue each tick both releases
        // on time and produces the natural reordering a real WAN shows.
        std::deque<Held> keep;
        for (std::deque<Held>::iterator it = held.begin(); it != held.end(); ++it) {
            if ((long)(now - it->releaseTick) >= 0) {
                if (it->toHost) {
                    sendto(hostSock, (const char*)it->data, it->len, 0,
                           (sockaddr*)&hostAddr, sizeof(hostAddr));
                    ++g_fwdUp;
                } else if (haveClient) {
                    sendto(clientSock, (const char*)it->data, it->len, 0,
                           (sockaddr*)&clientAddr, sizeof(clientAddr));
                    ++g_fwdDown;
                }
            } else {
                keep.push_back(*it);
            }
        }
        held.swap(keep);

        if (now - lastStats >= 5000) {
            lastStats = now;
            std::printf("netsim: up fwd=%lu drop=%lu | down fwd=%lu drop=%lu | held now=%lu max=%lu\n",
                        g_fwdUp, g_dropUp, g_fwdDown, g_dropDown,
                        (unsigned long)held.size(), g_heldMax);
            std::fflush(stdout);
        }
    }

    // Unreachable (killed by the harness), but keep the compiler honest.
    // closesocket(clientSock); closesocket(hostSock); WSACleanup();
    // return 0;
}
