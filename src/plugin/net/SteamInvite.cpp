#define _CRT_SECURE_NO_WARNINGS 1 // _snprintf is fine here; silence VC10 C4996

#include "SteamInvite.h"
#include "SteamP2P.h"        // selfId() + accept() (shared Steam client)
#include "../CoopLog.h"
#include "../../netproto/Wire.h" // coop::PROTOCOL_VERSION (lobby-data version gate)

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib> // atoi (lobby protocol-version gate)

namespace coop {
namespace steaminvite {
namespace {

// ---- Steamworks callback ABI (hand-rolled; no SDK headers) -------------------
// CCallbackBase (steam_api_common.h) layout the client's callback manager expects:
//   [0] vptr
//   [8] uint8  m_nCallbackFlags
//   [12] int   m_iCallback
// vtable order: Run(void*) / Run(void*,bool,SteamAPICall_t) / GetCallbackSizeBytes().
// We match it with a plain class whose 3 virtuals are declared in that exact order;
// MSVC lays vptr at 0 and packs the two members at 8/12. Our own fields live past
// offset 16 where the manager never looks. Dispatch routes on m_iCallback.

typedef unsigned long long SteamAPICall_t;

void dispatch(int iCallback, void* param, bool ioFailure); // fwd

class SteamCallback {
public:
    virtual void Run(void* param) { dispatch(m_iCallback, param, false); }
    virtual void Run(void* param, bool ioFailure, SteamAPICall_t) {
        dispatch(m_iCallback, param, ioFailure);
    }
    virtual int GetCallbackSizeBytes() { return m_size; }

    unsigned char m_nCallbackFlags; // offset 8  (written by RegisterCallback)
    int           m_iCallback;      // offset 12 (callback id)
    int           m_size;           // ours: sizeof the callback param struct

    void arm(int id, int sz) { m_nCallbackFlags = 0; m_iCallback = id; m_size = sz; }
};

// ---- Callback ids ------------------------------------------------------------
const int k_iSteamFriendsCallbacks     = 300;
const int k_iSteamMatchmakingCallbacks = 500;
const int k_iSteamNetworkingCallbacks  = 1200;
const int cb_LobbyCreated           = k_iSteamMatchmakingCallbacks + 13; // 513
const int cb_LobbyEnter             = k_iSteamMatchmakingCallbacks + 4;  // 504
const int cb_GameLobbyJoinRequested = k_iSteamMatchmakingCallbacks + 33; // 533
const int cb_P2PSessionRequest      = k_iSteamNetworkingCallbacks  + 2;  // 1202

// ---- Callback param structs (Steamworks #pragma pack(8) layouts) -------------
#pragma pack(push, 8)
struct LobbyCreated_t          { int m_eResult; unsigned long long m_ulSteamIDLobby; };
struct GameLobbyJoinRequested_t{ unsigned long long m_steamIDLobby; unsigned long long m_steamIDFriend; };
struct P2PSessionRequest_t     { unsigned long long m_steamIDRemote; };
struct LobbyEnter_t {
    unsigned long long m_ulSteamIDLobby;
    unsigned int       m_rgfChatPermissions;
    unsigned char      m_bLocked;
    unsigned int       m_EChatRoomEnterResponse;
};
#pragma pack(pop)

const int k_EResultOK           = 1;
const int k_ELobbyTypeFriendsOnly = 1;

// ---- Flat API surface --------------------------------------------------------
typedef int   (*GetHSteamFn)();
typedef void* (*SteamClientFn)();
typedef void* (*ClientGetIfaceFn)(void* client, int hUser, int hPipe, const char* version);

typedef SteamAPICall_t (*MmCreateLobbyFn)(void* mm, int eLobbyType, int cMaxMembers);
typedef SteamAPICall_t (*MmJoinLobbyFn)(void* mm, unsigned long long lobby);
typedef void  (*MmLeaveLobbyFn)(void* mm, unsigned long long lobby);
typedef bool  (*MmInviteToLobbyFn)(void* mm, unsigned long long lobby, unsigned long long invitee);
typedef bool  (*MmSetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k, const char* v);
typedef const char* (*MmGetLobbyDataFn)(void* mm, unsigned long long lobby, const char* k);
typedef unsigned long long (*MmGetLobbyOwnerFn)(void* mm, unsigned long long lobby);
typedef int   (*MmGetNumMembersFn)(void* mm, unsigned long long lobby);
typedef unsigned long long (*MmGetMemberByIndexFn)(void* mm, unsigned long long lobby, int i);
typedef void  (*FriendsInviteDialogFn)(void* friends, unsigned long long lobby);
typedef int   (*FrGetCountFn)(void* friends, int eFriendFlags);
typedef unsigned long long (*FrGetByIndexFn)(void* friends, int i, int eFriendFlags);
typedef const char* (*FrGetNameFn)(void* friends, unsigned long long id);
typedef int   (*FrGetStateFn)(void* friends, unsigned long long id);

#pragma pack(push, 8)
struct FriendGameInfo_t {
    unsigned long long m_gameID;       // CGameID (appID in low 24 bits for app type)
    unsigned int       m_unGameIP;
    unsigned short     m_usGamePort;
    unsigned short     m_usQueryPort;
    unsigned long long m_steamIDLobby;
};
#pragma pack(pop)
typedef bool  (*FrGetGamePlayedFn)(void* friends, unsigned long long id, FriendGameInfo_t* out);

const unsigned KENSHI_APPID = 233860;
typedef void  (*RegisterCbFn)(void* cb, int iCallback);
typedef void  (*RegisterCrFn)(void* cb, SteamAPICall_t call, int iCallback);
typedef void  (*UnregisterCbFn)(void* cb);
typedef void  (*RunCallbacksFn)();

void*                 g_mm       = 0; // ISteamMatchmaking*
void*                 g_friends  = 0; // ISteamFriends*
MmCreateLobbyFn       g_createLobby = 0;
MmJoinLobbyFn         g_joinLobby   = 0;
MmLeaveLobbyFn        g_leaveLobby  = 0;
MmInviteToLobbyFn     g_inviteToLobby = 0;
MmSetLobbyDataFn      g_setData     = 0;
MmGetLobbyDataFn      g_getData     = 0;
MmGetLobbyOwnerFn     g_getOwner    = 0;
MmGetNumMembersFn     g_getNum      = 0;
MmGetMemberByIndexFn  g_getMember   = 0;
FriendsInviteDialogFn g_inviteDlg   = 0;
FrGetCountFn          g_frCount     = 0;
FrGetByIndexFn        g_frByIndex   = 0;
FrGetNameFn           g_frName      = 0;
FrGetStateFn          g_frState     = 0;
FrGetGamePlayedFn     g_frGame      = 0;
RegisterCbFn          g_regCb       = 0;
RegisterCrFn          g_regCr       = 0;
UnregisterCbFn        g_unregCb     = 0;
RunCallbacksFn        g_runCb       = 0;

bool      g_ready     = false;
ConnectFn g_onConnect = 0;

// Invite/lobby state (main thread only).
unsigned long long g_lobby        = 0;    // our active lobby (0 = none)
bool               g_weAreHost    = false;// created the lobby (vs joined a friend's)
bool               g_hostFired    = false;// host already connected to the arrived friend
bool               g_creating     = false;// CreateLobby call in flight
bool               g_picker       = false;// in-panel friend picker is active
unsigned long long g_pendingInvitee = 0;  // friend clicked before the lobby existed
char               g_status[128]  = {0};

// Cached friend list for the in-panel picker (our own copy - GetFriendPersonaName
// returns a Steam-owned pointer only valid until the next call). Sorted in-Kenshi
// first, then online, then offline. state: 0 offline, 1 online, 2 in Kenshi.
struct FriendRow { unsigned long long id; char name[64]; int state; };
const int          MAX_FRIENDS = 32;
FriendRow          g_friendRows[MAX_FRIENDS];
int                g_friendN     = 0;
unsigned int       g_lastFriendRefresh = 0;

SteamCallback g_cbLobbyCreated;
SteamCallback g_cbLobbyEnter;
SteamCallback g_cbJoinRequested;
SteamCallback g_cbP2PRequest;

void steamLog(const char* msg) {
    char b[256];
    _snprintf(b, sizeof(b) - 1, "[invite] %s", msg ? msg : "");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
void setStatus(const char* s) {
    _snprintf(g_status, sizeof(g_status) - 1, "%s", s ? s : "");
    g_status[sizeof(g_status) - 1] = '\0';
}

// True if a friend is currently playing Kenshi (checked via GetFriendGamePlayed).
bool friendInKenshi(unsigned long long fid) {
    if (!g_frGame) return false;
    FriendGameInfo_t gi;
    std::memset(&gi, 0, sizeof(gi));
    if (!g_frGame(g_friends, fid, &gi)) return false;
    return (unsigned)(gi.m_gameID & 0xFFFFFFu) == KENSHI_APPID;
}

// (Re)build the friend cache for the in-panel picker: enumerate immediate friends,
// copy names into our own buffers, tag state (2 = in Kenshi, 1 = online, 0 = off),
// and sort in-Kenshi > online > offline so the useful ones surface first.
void refreshFriends() {
    g_friendN = 0;
    if (!g_frCount || !g_frByIndex || !g_frName) return;
    const int k_EFriendFlagImmediate = 0x04;
    int n = g_frCount(g_friends, k_EFriendFlagImmediate);
    for (int i = 0; i < n && g_friendN < MAX_FRIENDS; ++i) {
        unsigned long long fid = g_frByIndex(g_friends, i, k_EFriendFlagImmediate);
        if (fid == 0) continue;
        FriendRow& r = g_friendRows[g_friendN];
        r.id = fid;
        const char* nm = g_frName(g_friends, fid);
        _snprintf(r.name, sizeof(r.name) - 1, "%s", nm ? nm : "(unknown)");
        r.name[sizeof(r.name) - 1] = '\0';
        int persona = g_frState ? g_frState(g_friends, fid) : 0; // 0 = offline
        r.state = friendInKenshi(fid) ? 2 : (persona != 0 ? 1 : 0);
        ++g_friendN;
    }
    // Simple insertion sort by state descending (small N).
    for (int i = 1; i < g_friendN; ++i) {
        FriendRow key = g_friendRows[i];
        int j = i - 1;
        while (j >= 0 && g_friendRows[j].state < key.state) { g_friendRows[j + 1] = g_friendRows[j]; --j; }
        g_friendRows[j + 1] = key;
    }
    char b[96];
    _snprintf(b, sizeof(b) - 1, "friend list refreshed: %d friend(s)", g_friendN);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
}

// ---- Callback handlers (main thread, via Kenshi's SteamAPI_RunCallbacks) -----

void onLobbyCreated(LobbyCreated_t* r, bool ioFailure) {
    g_creating = false;
    if (ioFailure || !r || r->m_eResult != k_EResultOK) {
        steamLog("lobby creation FAILED");
        setStatus("Lobby creation failed - try again or type the ID");
        return;
    }
    g_lobby     = r->m_ulSteamIDLobby;
    g_weAreHost = true;
    g_hostFired = false;
    char pv[16];
    _snprintf(pv, sizeof(pv) - 1, "%u", (unsigned)coop::PROTOCOL_VERSION);
    pv[sizeof(pv) - 1] = '\0';
    if (g_setData) {
        g_setData(g_mm, g_lobby, "kc_protocol", pv);
        g_setData(g_mm, g_lobby, "kc_game", "KenshiCoop");
    }
    char b[96];
    _snprintf(b, sizeof(b) - 1, "lobby created %llu (protocol %s)", g_lobby, pv);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    // If a friend was picked before the lobby existed, invite them now.
    if (g_pendingInvitee != 0) {
        unsigned long long who = g_pendingInvitee;
        g_pendingInvitee = 0;
        if (g_inviteToLobby) g_inviteToLobby(g_mm, g_lobby, who);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "sent lobby invite to %llu", who);
        c[sizeof(c) - 1] = '\0';
        steamLog(c);
        setStatus("Invite sent - waiting for friend to accept...");
    } else {
        setStatus("Pick a friend to invite");
    }
}

void onGameLobbyJoinRequested(GameLobbyJoinRequested_t* r) {
    if (!r || !g_joinLobby) return;
    // The friend clicked our invite / "Join Game". Enter their lobby; the actual
    // connect happens in onLobbyEnter once membership + owner are known.
    g_weAreHost = false;
    g_hostFired = false;
    char b[96];
    _snprintf(b, sizeof(b) - 1, "join requested for lobby %llu - entering...",
              r->m_steamIDLobby);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    setStatus("Joining friend's game...");
    g_joinLobby(g_mm, r->m_steamIDLobby);
}

void onLobbyEnter(LobbyEnter_t* r) {
    if (!r) return;
    unsigned long long lobby = r->m_ulSteamIDLobby;
    g_lobby = lobby;
    unsigned long long owner = g_getOwner ? g_getOwner(g_mm, lobby) : 0;
    SteamId self = (SteamId)coop::steamp2p::selfId();

    if (owner == 0) { steamLog("entered lobby but owner unknown"); return; }
    if (owner == self) {
        // We are the host entering our own lobby - wait for the friend (tick()).
        steamLog("entered own lobby - waiting for friend to join");
        return;
    }

    // Joiner path: the owner is the host. Optionally gate on protocol version.
    if (g_getData) {
        const char* hp = g_getData(g_mm, lobby, "kc_protocol");
        if (hp && hp[0]) {
            unsigned hv = (unsigned)atoi(hp);
            if (hv != (unsigned)coop::PROTOCOL_VERSION) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "protocol mismatch: host v%u, you v%u - update the mod on both sides",
                          hv, (unsigned)coop::PROTOCOL_VERSION);
                b[sizeof(b) - 1] = '\0';
                steamLog(b);
                setStatus("Version mismatch with host - update both sides");
                return;
            }
        }
    }

    coop::steamp2p::accept(owner); // pre-accept the host's P2P session
    char b[96];
    _snprintf(b, sizeof(b) - 1, "joined host %llu via lobby - connecting as CLIENT", owner);
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    setStatus("Connecting to host...");
    if (g_onConnect) g_onConnect(false, true, owner);
}

void onP2PSessionRequest(P2PSessionRequest_t* r) {
    if (!r) return;
    // Accept any inbound session from within an invite flow so the tunnel opens
    // regardless of which side sends first.
    coop::steamp2p::accept(r->m_steamIDRemote);
}

void dispatch(int iCallback, void* param, bool ioFailure) {
    switch (iCallback) {
        case cb_LobbyCreated:           onLobbyCreated((LobbyCreated_t*)param, ioFailure); break;
        case cb_LobbyEnter:             onLobbyEnter((LobbyEnter_t*)param); break;
        case cb_GameLobbyJoinRequested: onGameLobbyJoinRequested((GameLobbyJoinRequested_t*)param); break;
        case cb_P2PSessionRequest:      onP2PSessionRequest((P2PSessionRequest_t*)param); break;
        default: break;
    }
}

void* getIface(void* client, GetHSteamFn getUser, GetHSteamFn getPipe,
               ClientGetIfaceFn get, const char* const* versions, int n, const char** used) {
    void* p = 0;
    for (int i = 0; i < n && p == 0; ++i) {
        p = get(client, getUser(), getPipe(), versions[i]);
        if (p != 0 && used) *used = versions[i];
    }
    return p;
}

} // namespace

// ---- Public API --------------------------------------------------------------

bool init(ConnectFn onConnect) {
    if (g_ready) { if (onConnect) g_onConnect = onConnect; return true; }
    g_onConnect = onConnect;

    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (mod == 0) { steamLog("init deferred: steam_api64.dll not loaded"); return false; }

    GetHSteamFn      getUser   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamUser");
    GetHSteamFn      getPipe   = (GetHSteamFn)     GetProcAddress(mod, "SteamAPI_GetHSteamPipe");
    SteamClientFn    getClient = (SteamClientFn)   GetProcAddress(mod, "SteamClient");
    ClientGetIfaceFn getMmIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    ClientGetIfaceFn getFrIf   = (ClientGetIfaceFn)GetProcAddress(mod, "SteamAPI_ISteamClient_GetISteamFriends");

    g_createLobby = (MmCreateLobbyFn)      GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_CreateLobby");
    g_joinLobby   = (MmJoinLobbyFn)        GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_JoinLobby");
    g_leaveLobby  = (MmLeaveLobbyFn)       GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    g_inviteToLobby = (MmInviteToLobbyFn)  GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    g_setData     = (MmSetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    g_getData     = (MmGetLobbyDataFn)     GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    g_getOwner    = (MmGetLobbyOwnerFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    g_getNum      = (MmGetNumMembersFn)    GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    g_getMember   = (MmGetMemberByIndexFn) GetProcAddress(mod, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    g_inviteDlg   = (FriendsInviteDialogFn)GetProcAddress(mod, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    g_frCount     = (FrGetCountFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendCount");
    g_frByIndex   = (FrGetByIndexFn)       GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendByIndex");
    g_frName      = (FrGetNameFn)          GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_frState     = (FrGetStateFn)         GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    g_frGame      = (FrGetGamePlayedFn)    GetProcAddress(mod, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    g_regCb       = (RegisterCbFn)         GetProcAddress(mod, "SteamAPI_RegisterCallback");
    g_regCr       = (RegisterCrFn)         GetProcAddress(mod, "SteamAPI_RegisterCallResult");
    g_unregCb     = (UnregisterCbFn)       GetProcAddress(mod, "SteamAPI_UnregisterCallback");
    g_runCb       = (RunCallbacksFn)       GetProcAddress(mod, "SteamAPI_RunCallbacks");

    if (!getUser || !getPipe || !getClient || !getMmIf || !getFrIf || !g_createLobby ||
        !g_joinLobby || !g_setData || !g_getData || !g_getOwner || !g_getNum ||
        !g_getMember || !g_inviteToLobby || !g_frCount || !g_frByIndex || !g_frName ||
        !g_regCb || !g_regCr || !g_runCb) {
        steamLog("init FAILED: invite/lobby flat exports missing");
        return false;
    }

    void* client = getClient();
    if (client == 0) { steamLog("init FAILED: SteamClient() null (Steam not running?)"); return false; }
    if (getUser() == 0 || getPipe() == 0) { steamLog("init FAILED: no Steam user/pipe"); return false; }

    static const char* const mmVers[] = { "SteamMatchMaking009", "SteamMatchMaking008", "SteamMatchMaking010" };
    static const char* const frVers[] = { "SteamFriends015", "SteamFriends014", "SteamFriends017", "SteamFriends016" };
    const char* usedMm = 0; const char* usedFr = 0;
    g_mm      = getIface(client, getUser, getPipe, getMmIf, mmVers, 3, &usedMm);
    g_friends = getIface(client, getUser, getPipe, getFrIf, frVers, 4, &usedFr);
    if (g_mm == 0)      { steamLog("init FAILED: no ISteamMatchmaking interface"); return false; }
    if (g_friends == 0) { steamLog("init FAILED: no ISteamFriends interface"); return false; }

    // Register the persistent (non-call-result) callbacks. LobbyCreated is a call
    // result, registered per-CreateLobby in hostInvite().
    g_cbLobbyEnter.arm(cb_LobbyEnter, sizeof(LobbyEnter_t));
    g_cbJoinRequested.arm(cb_GameLobbyJoinRequested, sizeof(GameLobbyJoinRequested_t));
    g_cbP2PRequest.arm(cb_P2PSessionRequest, sizeof(P2PSessionRequest_t));
    g_regCb(&g_cbLobbyEnter, cb_LobbyEnter);
    g_regCb(&g_cbJoinRequested, cb_GameLobbyJoinRequested);
    g_regCb(&g_cbP2PRequest, cb_P2PSessionRequest);

    g_ready = true;
    char b[160];
    _snprintf(b, sizeof(b) - 1, "ready: mm=%s friends=%s (in-panel friend picker armed)",
              usedMm ? usedMm : "?", usedFr ? usedFr : "?");
    b[sizeof(b) - 1] = '\0';
    steamLog(b);
    return true;
}

bool ready() { return g_ready; }

namespace {
// Kick off the async friends-only 2-player lobby if we don't have one yet.
void createLobbyIfNeeded() {
    if (g_lobby != 0 || g_creating) return;
    g_creating = true;
    steamLog("creating friends-only 2-player lobby...");
    SteamAPICall_t call = g_createLobby(g_mm, k_ELobbyTypeFriendsOnly, 2);
    if (call == 0) {
        g_creating = false;
        steamLog("CreateLobby returned no call handle");
        setStatus("Lobby creation failed - type the ID instead");
        return;
    }
    // Re-arm the LobbyCreated call-result object for this specific async call.
    if (g_unregCb) g_unregCb(&g_cbLobbyCreated);
    g_cbLobbyCreated.arm(cb_LobbyCreated, sizeof(LobbyCreated_t));
    g_regCr(&g_cbLobbyCreated, call, cb_LobbyCreated);
}
} // namespace

void beginInvite() {
    if (!g_ready) { steamLog("beginInvite: Steam invite layer not ready"); setStatus("Steam unavailable - type the ID instead"); return; }
    g_picker = true;
    refreshFriends();
    createLobbyIfNeeded();
    if (g_status[0] == '\0') setStatus("Pick a friend to invite");
    steamLog("friend picker opened");
}

void inviteFriend(SteamId id) {
    if (!g_ready || id == 0) return;
    createLobbyIfNeeded(); // no-op if already have one / creating
    // Look up the name for a friendlier status line.
    const char* nm = 0;
    for (int i = 0; i < g_friendN; ++i) if (g_friendRows[i].id == id) { nm = g_friendRows[i].name; break; }
    if (g_lobby != 0 && g_inviteToLobby) {
        g_inviteToLobby(g_mm, g_lobby, id);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "sent lobby invite to %llu (%s)", id, nm ? nm : "?");
        b[sizeof(b) - 1] = '\0';
        steamLog(b);
    } else {
        // Lobby still being created; send the invite the moment it exists.
        g_pendingInvitee = id;
        steamLog("lobby not ready yet - invite queued");
    }
    char s[96];
    _snprintf(s, sizeof(s) - 1, "Invited %s - waiting for them to accept...", nm ? nm : "friend");
    s[sizeof(s) - 1] = '\0';
    setStatus(s);
}

bool pickerActive()            { return g_picker; }
int  friendCount()             { return g_friendN; }
SteamId friendId(int i)        { return (i >= 0 && i < g_friendN) ? g_friendRows[i].id : 0; }
const char* friendName(int i)  { return (i >= 0 && i < g_friendN) ? g_friendRows[i].name : ""; }
int  friendState(int i)        { return (i >= 0 && i < g_friendN) ? g_friendRows[i].state : 0; }

void tick() {
    if (!g_ready) return;
    // Drive the Steam callback pump ourselves: Kenshi does not call
    // SteamAPI_RunCallbacks (its async CreateLobby result never dispatched until
    // we added this), so our invite/lobby/join/P2P callbacks + call-results only
    // fire from here. Safe even if the game did pump - RunCallbacks just drains
    // the message queue, so sequential main-thread calls can't double-deliver a
    // single posted callback. Main thread only (this runs inside mainLoop_hook).
    if (g_runCb) g_runCb();

    // Refresh the picker's friend list every ~3s while it's open (online / in-game
    // status changes over time). Skip once the host has already connected.
    if (g_picker && !g_hostFired) {
        unsigned int now = GetTickCount();
        if (g_lastFriendRefresh == 0 || now - g_lastFriendRefresh >= 3000) {
            g_lastFriendRefresh = now;
            refreshFriends();
        }
    }

    // Host membership poll: once the invited friend enters the lobby, learn their
    // SteamID and connect. (Avoids needing the LobbyChatUpdate callback.)
    if (g_weAreHost && g_lobby != 0 && !g_hostFired && g_getNum) {
        int n = g_getNum(g_mm, g_lobby);
        if (n >= 2) {
            SteamId self = (SteamId)coop::steamp2p::selfId();
            for (int i = 0; i < n; ++i) {
                unsigned long long m = g_getMember(g_mm, g_lobby, i);
                if (m != 0 && m != self) {
                    g_hostFired = true;
                    coop::steamp2p::accept(m);
                    char b[96];
                    _snprintf(b, sizeof(b) - 1, "friend %llu joined the lobby - hosting for them", m);
                    b[sizeof(b) - 1] = '\0';
                    steamLog(b);
                    setStatus("Friend joined - hosting...");
                    if (g_onConnect) g_onConnect(true, true, m);
                    break;
                }
            }
        }
    }
}

const char* status() { return g_status; }

void reset() {
    if (g_ready && g_lobby != 0 && g_leaveLobby) g_leaveLobby(g_mm, g_lobby);
    g_lobby     = 0;
    g_weAreHost = false;
    g_hostFired = false;
    g_creating  = false;
    g_picker    = false;
    g_pendingInvitee = 0;
    g_friendN   = 0;
    g_status[0] = '\0';
    if (g_ready && g_unregCb) g_unregCb(&g_cbLobbyCreated);
}

} // namespace steaminvite
} // namespace coop
