// SteamP2P - Steam P2P datagram transport over Kenshi's own steam_api64.dll.
//
// Kenshi ships the legacy ISteamNetworking P2P API (SendP2PPacket/ReadP2PPacket,
// confirmed in the DLL's flat exports; interface era SteamClient017/SteamUser019).
// Valve brokers connections BY STEAMID: UDP NAT hole-punch first, silent relay
// through Valve's network when punching fails. That removes IPs, port forwarding
// and router/CGNAT problems from the co-op session entirely.
//
// Design: this module does NOT replace the wire protocol - it is a datagram pipe.
// NetLink keeps running the stock ENet protocol (HELLO/WELCOME, channels,
// reliability, reconnect); the vendored ENet's socket layer is redirected here
// via enet_set_socket_hooks() (patch 0002), so every ENet datagram rides one
// unreliable Steam P2P packet on channel 0. UDP stays the default transport.
//
// Two-player assumption (mirrors NetLink): ONE tunnel peer, configured up front
// with the other player's steamid64 (two-code exchange; sending to a SteamID
// implicitly accepts its inbound session, so no Steam callback plumbing needed).
//
// Threading: init()/setPeer()/setPingPeer() are called on the main thread before
// the net thread launches; the ENet hooks and tick() run on the net thread. The
// flat ISteamNetworking calls are thread-safe (IPC into the Steam client).

#ifndef KENSHICOOP_STEAMP2P_H
#define KENSHICOOP_STEAMP2P_H

namespace coop {
namespace steamp2p {

typedef unsigned long long SteamId; // steamid64

// Resolve the flat API from the game's already-loaded steam_api64.dll and log
// "[steam] id=<steamid64> loggedOn=<0|1> iface=<version>". Idempotent; returns
// false (and logs why) when Steam isn't available.
bool init();
bool ready();
SteamId selfId();

// Configure the single tunnel peer. Proactively accepts its inbound session
// and allows Valve-relay fallback. Call before the net thread starts.
void setPeer(SteamId id);

// Accept an inbound P2P session from a specific SteamID. Used by the Steam
// invite layer's P2PSessionRequest_t callback so a session opens even if the
// request arrives before setPeer() pre-accepts it. No-op until init() succeeds.
void accept(SteamId id);

// Spike harness (KENSHICOOP_STEAM_PING=<steamid64>): ping/echo on P2P channel 1
// + periodic session-state logging, driven by tick() from the net thread. Works
// with either transport, so a UDP build can still prove Steam reachability.
void setPingPeer(SteamId id);

// Net-thread heartbeat: spike pings/echoes + session-state change logging.
// Cheap no-op when init() hasn't succeeded.
void tick();

// Install/remove the ENet socket hooks that tunnel channel 0 over Steam P2P.
// 'port' only fabricates the fake ENetAddress reported to ENet (the tunnel is
// addressless). Install BEFORE enet_host_create, remove after enet_host_destroy.
bool installEnetHooks(int port);
void removeEnetHooks();

// Close the P2P sessions (peer + ping peer).
void shutdown();

} // namespace steamp2p
} // namespace coop

#endif // KENSHICOOP_STEAMP2P_H
