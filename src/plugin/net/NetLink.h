// NetLink - owns ENet on a dedicated background thread.
//
// Threading contract:
//   * The net thread EXCLUSIVELY owns the ENetHost; the game thread never
//     touches ENet.
//   * Inbound events (peer connect/leave, received EntityState) are handed to
//     the game thread via the Inbound queue.
//   * The game thread publishes this peer's owned entities via setOwnedEntities();
//     the net thread reads the latest snapshot and transmits it each tick.
//
// VS2010 (v100) compatible: Win32 threads + CRITICAL_SECTION (no std::thread).

#ifndef KENSHICOOP_NETLINK_H
#define KENSHICOOP_NETLINK_H

#include <windows.h>
#include <string>
#include <vector>
#include <enet/enet.h>

#include "../../netproto/Wire.h"
#include "../core/Inbound.h"

namespace coop {

class NetLink {
public:
    NetLink();
    ~NetLink();

    // Start as host on 'port' / as client to 'ip:port'. Inbound events go to
    // 'inbound'. Returns false if ENet init or the thread launch failed.
    bool startHost(int port, Inbound* inbound);
    bool startClient(const std::string& ip, int port, Inbound* inbound);
    void stop();

    // MAIN thread: publish this peer's owned entities (copied under lock). The
    // net thread re-broadcasts the latest snapshot each tick. Pass count 0 to
    // publish nothing.
    void setOwnedEntities(u32 ownerId, const EntityState* arr, unsigned int count);

    bool isRunning() const { return running_ != 0; }
    u32  localId()   const { return myId_; } // host = 0; client = id from WELCOME

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void threadLoop();
    bool launchThread();

    bool        isHost_;
    std::string ip_;
    int         port_;

    ENetHost*   enetHost_;   // net thread only
    ENetPeer*   serverPeer_; // client only; net thread only
    Inbound*    inbound_;

    CRITICAL_SECTION         outCs_;
    std::vector<EntityState> out_;
    u32                      outOwner_;
    bool                     haveOut_;

    HANDLE        thread_;
    volatile LONG running_;
    volatile LONG stopFlag_;
    u32           myId_;

    NetLink(const NetLink&);
    NetLink& operator=(const NetLink&);
};

} // namespace coop

#endif // KENSHICOOP_NETLINK_H
