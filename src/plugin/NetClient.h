// NetClient - owns ENet on a dedicated background thread (Milestones 4 & 5).
//
// Threading contract:
//   * The net thread EXCLUSIVELY owns the ENetHost. ENet is not touched from the
//     game thread.
//   * Inbound remote states are handed to the game thread via MainThreadQueue.
//   * The game thread publishes the local player's state via setLocalState();
//     the net thread reads the latest value and transmits it each tick.
//
// VS2010 (v100) compatible: Win32 threads + CRITICAL_SECTION (no std::thread).

#ifndef KENSHICOOP_NETCLIENT_H
#define KENSHICOOP_NETCLIENT_H

#include <windows.h>
#include <string>
#include <vector>
#include <enet/enet.h>

#include "../netproto/Protocol.h"
#include "MainThreadQueue.h"

namespace coop {

class NetClient {
public:
    NetClient();
    ~NetClient();

    // Start listening as host on 'port'. Inbound remote states go to 'inbound'.
    bool startHost(int port, MainThreadQueue* inbound);

    // Start as client connecting to ip:port.
    bool startClient(const std::string& ip, int port, MainThreadQueue* inbound);

    // Signal the net thread to stop and join it.
    void stop();

    // MAIN-thread API: publish this client's local player state for the net
    // thread to transmit. Thread-safe.
    void setLocalState(const PlayerStatePacket& p);

    // MAIN-thread API (host only): publish the latest set of nearby NPC
    // transforms for the net thread to broadcast in batches. Thread-safe;
    // copies under a lock.
    void setNpcStates(const NpcStateEntry* arr, unsigned int count);

    // MAIN-thread API (both peers): publish this client's OWN squad members for
    // the net thread to transmit (host broadcasts; client sends to host) tagged
    // with ownerId. Bidirectional, unlike setNpcStates. Thread-safe.
    void setSquadStates(u32 ownerId, const NpcStateEntry* arr, unsigned int count);

    bool isRunning() const { return running_ != 0; }
    u32  localId()   const { return myId_; }

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();

    // config (set before thread starts)
    bool        isHost_;
    std::string ip_;
    int         port_;

    // owned by the net thread
    ENetHost*   enetHost_;
    ENetPeer*   serverPeer_; // client mode only

    // shared state
    MainThreadQueue*  inbound_;
    CRITICAL_SECTION  localCs_;
    PlayerStatePacket localState_;
    bool              haveLocal_;

    // host-only outbound NPC batch (main thread writes, net thread reads)
    CRITICAL_SECTION           npcCs_;
    std::vector<NpcStateEntry> npcOut_;

    // outbound OWN-squad batch, sent by both peers (main writes, net reads)
    CRITICAL_SECTION           squadCs_;
    std::vector<NpcStateEntry> squadOut_;
    u32                        squadOwnerId_;
    bool                       haveSquad_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    u32           myId_;

    NetClient(const NetClient&);
    NetClient& operator=(const NetClient&);
};

} // namespace coop

#endif // KENSHICOOP_NETCLIENT_H
