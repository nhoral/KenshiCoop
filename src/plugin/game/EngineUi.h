// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

class GameWorld;

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 that lets the player pick role + transport
// (buttons/checkboxes - the only reliably interactive DatapanelGUI controls;
// MyGUI comboboxes/editboxes have no usable RVAs and don't receive keyboard focus
// during gameplay) and Connect/Disconnect. The friend's Steam ID is entered by
// clipboard: "Copy my Steam ID" puts the player's own id on the clipboard to
// share, and "Paste friend's Steam ID" reads the friend's id back in. A valid
// paste is remembered across launches (persisted to coop_last_peer.txt) so it
// pre-fills next time; it stays overridable by pasting a different friend's id.
// The UDP endpoint (ip/port) still comes from
// coop_config.json. The GUI layer stays session-agnostic: live status is passed IN
// via *st and the user's actions are handed BACK through the callbacks (the plugin
// root owns the session/config wiring). Main-thread only; SEH-guarded.
struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        detail;      // one-line status string for the panel/overlay
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect; the UDP endpoint is re-read from the config there.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId);
typedef void (*CoopDisconnectFn)();
void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect);

// Persistent co-op connection-status overlay: a single ScreenLabel tracked to the
// local leader (the spike-47/48 screenshot-proven render path) whose caption shows
// the live session status, colored by state (0 = offline/red, 1 = waiting/yellow,
// 2 = connected/green). Recreated if the leader pointer changes (world reload) and
// updated in place via _NV_setCaption when the text/state changes. Pass show=false
// to remove it. Main-thread only; SEH-guarded.
void coopOverlayTick(GameWorld* gw, const char* text, int state, bool show);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
