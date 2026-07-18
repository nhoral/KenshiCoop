// Plugin - RE_Kenshi / KenshiLib plugin entry point (clean rebuild).
//
// Responsibilities (kept deliberately thin - everything substantive lives in the
// core/, net/, game/, sync/ modules):
//   * startPlugin(): read config, open the log, install the main-loop + title
//     hooks, start networking.
//   * mainLoop_hook(): the single main-thread safe point. Detects "gameplay
//     live", drains net events, drives per-stage sync, and (in test mode)
//     self-exits after the configured duration.
//   * titleUpdate_hook(): auto-loads the configured save once the menu settles.
//
// Stage 0 scope: modular skeleton + ENet handshake (version-checked) + lifecycle.
// Sync of entities is added from Stage 1 onward.

#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include <Debug.h>                  // DebugLog / ErrorLog
#include <core/Functions.h>         // KenshiLib::AddHook / GetRealAddress / SUCCESS
#include <kenshi/GameWorld.h>       // GameWorld (main-loop hook signature)
#include <kenshi/gui/TitleScreen.h> // TitleScreen::_NV_update (auto-load trigger)

#include <windows.h>
#include <cstdio>
#include <string>
#include <deque>

#include "CoopLog.h"
#include "core/Config.h"
#include "core/OwnRanks.h"
#include "core/Inbound.h"
#include "net/NetLink.h"
#include "net/SteamP2P.h"
#include "net/SteamInvite.h"
#include "game/Engine.h"
#include "sync/Replicator.h"
#include "sync/SaveXfer.h"
#include "test/Scenario.h"

namespace {

coop::Config     g_cfg;
coop::NetLink    g_net;
coop::Inbound    g_inbound;
coop::Replicator g_repl;
coop::u32        g_tick = 0;

// Last GameWorld seen by the main-loop hook. The F2-panel UI callbacks
// (coopUiConnect/coopUiDisconnect) run without a GameWorld argument, but the
// world is live when the user hits Connect/Disconnect, so we despawn our
// minted proxies (NPC + world-item, Phase 3) through this cached pointer to
// avoid leaking duplicate bodies into the save. Only touched on the main thread.
GameWorld*       g_lastGw = 0;

// Cross-owner trade veto owner classifier (engine InvOwnerClassFn): forwards a
// save-stable owner hand to the Replicator's squad-ownership sets. Free function
// so the engine layer (which must not know about the Replicator) can call it.
static int coopInvOwnerClass(const unsigned int h[5]) {
    return g_repl.ownerClassForHand(h);
}

bool  g_gameStarted   = false;
DWORD g_gameStartTick = 0;

// Auto-load state (title-screen settle gate).
bool  g_autoLoadDone   = false;
DWORD g_titleFirstTick = 0;

// Scenario harness state.
coop::Scenario* g_scenario        = 0;
bool            g_scenarioStarted = false;
DWORD           g_scenarioStartTick = 0;
unsigned int    g_scenarioTick    = 0;
DWORD           g_scenarioDoneTick = 0; // !=0 once RESULT logged; begins capture hold
const DWORD     SCENARIO_HOLD_MS  = 4000; // hold synced state on screen for capture

// Test-scene setup (host-only): one-shot world spawn the user then saves.
bool            g_setupDone     = false;
const DWORD     SETUP_DELAY_MS  = 4000; // let the world settle before spawning
DWORD           g_lastCraftRearmTick = 0; // throttle host craft re-arm
DWORD           g_bakeSaveTick  = 0;     // != 0: auto-bake save armed at this tick
const DWORD     CRAFT_REARM_MS  = 3000; // re-issue the work goal at most this often

// Coordinated save (protocol 31) state.
bool         g_peerPresent   = false; // a peer is connected right now
std::string  g_savePending;           // host: save name awaiting quiescence
coop::u32    g_saveReqId     = 0;     // join: monotonic PKT_SAVE_REQ counter

// Push-save-on-connect bootstrap (host). When a peer connects the host bakes a
// fresh save of its live world and, once that folder quiesces, sends the join a
// LOAD_GO for it (NOT a blind stream): the join loads it if it already has an
// identical copy, otherwise NACKs and the existing fallback-transfer path (see
// driveLoadSync) streams the folder before the join loads. This lets a joiner
// enter the host's world from the main menu with no pre-shared save.
bool         g_bootstrapArmed = false; // host: a connect-triggered save is baking
std::string  g_bootstrapName;          // host: that save's name (matches g_savePending)

// Coordinated load (protocol 32) state. World-swap edge detection: after
// gameplay has started once, gameplayLive dropping means the engine is
// swapping worlds (a load); live again = the reload edge (session reset
// point). Sub-second dips are logged as FLICKER and do NOT count - a real
// load screen lasts seconds, and a spurious reset would wipe live session
// state mid-game.
DWORD        g_swapStartTick = 0;     // != 0: gameplay non-live, swap running
coop::u32    g_swapHookTicks = 0;     // mainLoop ticks observed during the swap
const DWORD  SWAP_MIN_MS     = 400;   // shorter dips are flicker, not a reload

// Protocol 32 coordination state.
bool         g_loadSuppressOn   = false; // join: current suppression lever state
coop::u32    g_loadIdOut        = 0;     // host: monotonic LOAD_GO id
coop::u32    g_loadIdSeen       = 0;     // join: newest GO loadId handled
coop::u32    g_loadReqId        = 0;     // join: monotonic PKT_LOAD_REQ counter
std::string  g_loadXferPending;          // host: save awaiting post-reload transfer (NACK)
std::string  g_loadAfterCommit;          // join: save to load once its transfer commits
coop::u32    g_loadCommitBase   = 0;     // join: savexfer::commitSeq() at NACK time
// Deferred-signal backstop: SaveManager::load only SETS the LOADGAME signal;
// load_probe run 1 saw it sit unconsumed mid-session (run 2's engine consumed
// it in ~0.5 s on its own). Arm on every coordinated load issue; if the swap
// hasn't started after the grace window, pump execute() manually once.
DWORD        g_loadPumpArmTick  = 0;
const DWORD  LOAD_PUMP_GRACE_MS = 2000;

// Original function pointers, filled by KenshiLib::AddHook.
void (*g_mainLoop_orig)(GameWorld*, float) = 0;
void (*g_titleUpdate_orig)(TitleScreen*)   = 0;

// In-game co-op panel (F2) connect/disconnect handlers. Defined after
// startNetworking() (which coopUiConnect reuses); forward-declared here so
// mainLoop_hook can hand their addresses to coopPanelTick.
void startNetworking();
void coopUiConnect(bool isHost, bool useSteam, unsigned long long peerId);
void coopUiDisconnect();

// Log to BOTH our dedicated per-line-flushed file (what the test runner reads)
// and the engine's kenshi.log (handy when attached live).
void coopLog(const char* msg) { coop::logLine(msg);    DebugLog(msg); }
void coopErr(const char* msg) { coop::logErrLine(msg); ErrorLog(msg); }

// Blank-portraits diagnostic (protocol 36): loading a save folder without its
// portrait atlas blanks the squad-tab avatars until the next portrait rebuild.
// Warn at every coordinated-load issue point so a session log pinpoints WHICH
// load introduced the blank avatars (host reload, join GO, or a transferred
// copy that shipped without the atlas).
void warnIfNoPortraits(const std::string& name) {
    std::string p = coop::savexfer::saveFolderFor(name) + "\\portraits_texture.png";
    if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[load] WARN save '%s' has no portraits_texture.png "
                  "(squad avatars will render blank)", name.c_str());
        b[sizeof(b) - 1] = '\0'; coopErr(b);
    }
}

// Push-save-on-connect (host): bake a fresh save of the live world and arm the
// bootstrap so driveSaveSync announces it to the join with a LOAD_GO once the
// folder quiesces. Called for either connect ordering: a peer connecting while
// the host is already in-game (processNetEvents), or the host's gameplay
// starting with a peer already connected (mainLoop_hook gameplay-start edge).
// Host + saveSync + in-game are the caller's responsibility.
void armConnectPush() {
    char cur[64];
    cur[0] = '\0';
    coop::engine::saveInfo(cur, sizeof(cur), 0, 0);
    std::string name = cur[0] ? cur : "coopresume";
    g_bootstrapArmed = true;
    g_bootstrapName  = name;
    char b[144];
    _snprintf(b, sizeof(b) - 1,
              "[boot] baking save '%s' to push to join on connect", name.c_str());
    b[sizeof(b) - 1] = '\0'; coopLog(b);
    if (!coop::engine::saveGameAs(name))
        coopErr("[boot] connect-push save FAILED to issue");
}

// Drain peer connect/leave events and surface a single game-thread confirmation
// per event. The net thread already logs the handshake; this proves the event
// reached the game thread cleanly (and is where later stages spawn/sweep).
void processNetEvents(GameWorld* gw) {
    std::deque<coop::u32> conns, leaves;
    g_inbound.drainConnects(conns);
    g_inbound.drainLeaves(leaves);
    for (std::deque<coop::u32>::iterator it = conns.begin(); it != conns.end(); ++it) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "handshake: peer present id=%u (local id=%u)",
                  (unsigned)*it, (unsigned)g_net.localId());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        // Connect-edge resync (protocol 30): re-announce placed buildings and
        // force an immediate resend pass across all change-gated channels, so
        // a late joiner / reconnector converges now instead of waiting out
        // per-channel safety resends (or never minting a pre-connect build).
        if (g_cfg.latejoinSync) g_repl.onPeerConnected(g_net, g_net.localId());
        else coopLog("[latejoin] connect edge seen, resync OFF (gate)");
        g_peerPresent = true;
        // Coordinated save (protocol 31): while connected under save-sync,
        // the JOIN never writes a save locally - the host's save is
        // authoritative and a local save press forwards as PKT_SAVE_REQ.
        if (!g_cfg.isHost && g_cfg.saveSync) {
            coop::engine::setSaveSuppress(true);
            coopLog("[save] JOIN save suppression ON (host save is authoritative)");
        }
        // Push-save-on-connect (host): if already in a game, bake+announce the
        // live world so the join can enter it with no pre-shared save. If the
        // host is NOT yet in-game (title/loading), the gameplay-start edge in
        // mainLoop_hook arms this instead - covers either connect ordering.
        if (g_cfg.isHost && g_cfg.saveSync && g_gameStarted)
            armConnectPush();
    }
    for (std::deque<coop::u32>::iterator it = leaves.begin(); it != leaves.end(); ++it) {
        char b[64];
        _snprintf(b, sizeof(b) - 1, "handshake: peer left id=%u", (unsigned)*it);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        // Carried-body sync (protocol 18) + furniture occupancy (protocol 19):
        // the departed peer's stream will never author its drop/exit edges -
        // release any carry or occupancy its driven copies still hold.
        if (gw && (g_cfg.carrySync || g_cfg.furnSync)) g_repl.sweepCarries(gw);
        g_peerPresent = false;
        // Coordinated save: disconnected = solo again; local saves must work.
        if (!g_cfg.isHost && g_cfg.saveSync) {
            coop::engine::setSaveSuppress(false);
            coopLog("[save] JOIN save suppression OFF (peer left)");
        }
    }
    // Phase 2 crash hardening: a peer drop leaves this side's minted proxies
    // standing AND its drive maps pointing at bodies with no fresh authority
    // (the engine will eventually reap them, and the next drive touches a freed
    // pointer - the "join crash -> host follow-on crash" chain). Despawn the
    // minted proxies and clear the peer maps, mirroring coopUiDisconnect(). Runs
    // once per leave batch (we support a single peer).
    if (!leaves.empty()) {
        g_repl.clearPeerReplicationState(gw);
        g_inbound.flushWorldState();
    }
}

// Coordinated save + session resume (protocol 31), both roles, once per tick.
//
// HOST: any local save edge (menu/quicksave/autosave/programmatic - the
// SaveManager::save detour catches them all) arms the folder-quiescence
// watch; when the folder settles, the whole save folder streams to the join
// (SaveXfer sender, paced chunks); the join's ACK closes the loop. A join
// PKT_SAVE_REQ funnels into the same path via engine::saveGameAs.
//
// JOIN: local saves are suppressed while connected (the detour skips the
// engine write); a suppressed MANUAL edge forwards to the host as
// PKT_SAVE_REQ (autosave edges are dropped - the host's own autosave already
// drives a coordinated save). Received BEGIN/FILE/DONE stage + verify +
// commit the host's folder; the ACK reports the outcome.
void driveSaveSync() {
    // Local save edges from the detour (max 8 queued per tick).
    coop::engine::SaveEdge edges[8];
    unsigned int nEdges = coop::engine::drainSaveEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        std::string name = edges[i].name[0] ? edges[i].name : "coopresume";
        if (g_cfg.isHost) {
            g_savePending = name;
            coop::savexfer::armWatch(name);
        } else if (edges[i].suppressed && !edges[i].autosave) {
            coop::SaveReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_SAVE_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_saveReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueSaveReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[save] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    if (g_cfg.isHost) {
        // Join save requests -> the ONE authoritative save (the saveGameAs
        // below re-enters the detour, whose edge arms the watch above).
        std::deque<coop::InboundSaveReq> reqs;
        g_inbound.drainSaveReqs(reqs);
        for (std::deque<coop::InboundSaveReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[save] REQ from join id=%u name='%s' -> saving",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::saveGameAs(name[0] ? name : "coopresume"))
                coopErr("[save] join-requested save FAILED to issue");
        }

        // Quiescence watch -> start the transfer once the save is on disk.
        if (coop::savexfer::watching()) {
            unsigned int files = 0;
            unsigned __int64 bytes = 0;
            unsigned long waited = 0;
            int rc = coop::savexfer::tickWatch(&files, &bytes, &waited);
            if (rc == 1 || rc == 2) {
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "[save] QUIESCED kind=%s name='%s' files=%u bytes=%I64u waitMs=%lu",
                          rc == 1 ? "settled" : "timeout", g_savePending.c_str(),
                          files, bytes, waited);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                if (g_bootstrapArmed && g_savePending == g_bootstrapName) {
                    // Connect-push: announce the freshly-baked save with a
                    // LOAD_GO instead of a blind stream. The join loads it
                    // directly if its on-disk copy matches the fingerprint;
                    // otherwise it NACKs and driveLoadSync's fallback transfer
                    // streams the folder before the join loads. Reuses the
                    // whole existing LOAD_GO/NACK/transfer/commit machinery.
                    coop::LoadGoPacket go;
                    memset(&go, 0, sizeof(go));
                    go.type        = (coop::u8)coop::PKT_LOAD_GO;
                    go.ownerId     = g_net.localId();
                    go.loadId      = ++g_loadIdOut;
                    go.fingerprint = coop::savexfer::folderFingerprint(g_bootstrapName);
                    strncpy(go.name, g_bootstrapName.c_str(), sizeof(go.name) - 1);
                    g_net.queueLoadGo(go);
                    g_loadPumpArmTick = GetTickCount();
                    char b2[192];
                    _snprintf(b2, sizeof(b2) - 1,
                              "[boot] GO->join id=%u name='%s' fp=%08x (push on connect)",
                              go.loadId, g_bootstrapName.c_str(), go.fingerprint);
                    b2[sizeof(b2) - 1] = '\0'; coopLog(b2);
                    warnIfNoPortraits(g_bootstrapName);
                    g_bootstrapArmed = false;
                    g_bootstrapName.clear();
                    g_savePending.clear();
                } else if (g_peerPresent)
                    coop::savexfer::beginSend(g_net, g_net.localId(), g_savePending);
                else
                    coopLog("[save] no peer connected; transfer skipped");
            }
        }
        // Paced chunk pump for an in-flight transfer.
        if (coop::savexfer::sending())
            coop::savexfer::tickSend(g_net, g_net.localId());

        // Join commit acknowledgements.
        std::deque<coop::InboundSaveAck> acks;
        g_inbound.drainSaveAcks(acks);
        for (std::deque<coop::InboundSaveAck>::iterator it = acks.begin();
             it != acks.end(); ++it) {
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[save] XFER-ACK id=%u ok=%u files=%u bytes=%I64u",
                      it->pkt.xferId, (unsigned)it->pkt.ok,
                      (unsigned)it->pkt.files, it->pkt.bytes);
            b[sizeof(b) - 1] = '\0';
            if (it->pkt.ok) coopLog(b); else coopErr(b);
            coop::savexfer::noteAck(it->pkt.xferId, it->pkt.ok ? 1 : 0);
        }
    } else {
        // Receiver half: stage, verify, commit, acknowledge.
        std::deque<coop::InboundSaveBegin> begins;
        g_inbound.drainSaveBegins(begins);
        for (std::deque<coop::InboundSaveBegin>::iterator it = begins.begin();
             it != begins.end(); ++it)
            coop::savexfer::onSaveBegin(it->pkt);

        std::deque<coop::InboundSaveFile> chunks;
        g_inbound.drainSaveFiles(chunks);
        for (std::deque<coop::InboundSaveFile>::iterator it = chunks.begin();
             it != chunks.end(); ++it)
            coop::savexfer::onSaveFile(it->hdr, it->path.c_str(),
                                       it->data.empty() ? 0 : &it->data[0]);

        std::deque<coop::InboundSaveDone> dones;
        g_inbound.drainSaveDones(dones);
        for (std::deque<coop::InboundSaveDone>::iterator it = dones.begin();
             it != dones.end(); ++it) {
            coop::u16 files = 0;
            unsigned __int64 bytes = 0;
            int ok = coop::savexfer::onSaveDone(it->hdr,
                                                it->crcs.empty() ? 0 : &it->crcs[0],
                                                &files, &bytes);
            coop::SaveAckPacket ack;
            memset(&ack, 0, sizeof(ack));
            ack.type    = (coop::u8)coop::PKT_SAVE_ACK;
            ack.ownerId = g_net.localId();
            ack.xferId  = it->hdr.xferId;
            ack.ok      = ok ? 1 : 0;
            ack.files   = files;
            ack.bytes   = bytes;
            g_net.queueSaveAck(ack);
        }
    }
}

// Coordinated load (protocol 32), both roles, once per tick.
//
// HOST (load-authoritative, mirroring the save arbitration): any local load
// edge (menu or programmatic - the SaveManager::load detour catches both)
// broadcasts PKT_LOAD_GO with the save's folder fingerprint, then the host
// loads natively (never delayed). A join PKT_LOAD_REQ funnels into the same
// path via engine::loadSave. A join PKT_LOAD_NACK (copy missing/diverged)
// queues a protocol-31 SaveXfer of the folder, fired once the host's own
// reload has completed; the join loads after the verified commit.
//
// JOIN: local loads are suppressed while connected under the gate (the
// detour swallows the engine call); a suppressed manual edge forwards as
// PKT_LOAD_REQ. On LOAD_GO the join fingerprints its local copy: match =
// load now (bypass-once through the suppressed detour); mismatch/missing =
// NACK + a pending latch that loads after the matching transfer commits.
void driveLoadSync(GameWorld* gw) {
    // Local load edges from the detour (max 8 queued per tick).
    coop::engine::LoadEdge edges[8];
    unsigned int nEdges = coop::engine::drainLoadEdges(edges, 8);
    for (unsigned int i = 0; i < nEdges; ++i) {
        if (!edges[i].name[0]) continue;
        std::string name = edges[i].name;
        if (g_cfg.isHost) {
            // The title-screen auto-load precedes gameplay (and any peer) -
            // only a MID-SESSION load edge coordinates.
            if (!g_gameStarted) continue;
            // A load supersedes any in-flight save coordination.
            coop::savexfer::abortAll();
            g_savePending.clear();
            coop::LoadGoPacket go;
            memset(&go, 0, sizeof(go));
            go.type        = (coop::u8)coop::PKT_LOAD_GO;
            go.ownerId     = g_net.localId();
            go.loadId      = ++g_loadIdOut;
            go.fingerprint = coop::savexfer::folderFingerprint(name);
            strncpy(go.name, name.c_str(), sizeof(go.name) - 1);
            g_net.queueLoadGo(go);
            g_loadPumpArmTick = GetTickCount();
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[load] GO->join id=%u name='%s' fp=%08x",
                      go.loadId, name.c_str(), go.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            warnIfNoPortraits(name);
        } else if (edges[i].suppressed) {
            // Forward the swallowed manual load for host arbitration.
            coop::LoadReqPacket rq;
            memset(&rq, 0, sizeof(rq));
            rq.type    = (coop::u8)coop::PKT_LOAD_REQ;
            rq.ownerId = g_net.localId();
            rq.reqId   = ++g_loadReqId;
            strncpy(rq.name, name.c_str(), sizeof(rq.name) - 1);
            g_net.queueLoadReq(rq);
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[load] REQ->host id=%u name='%s'",
                      rq.reqId, name.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        } else if (g_gameStarted) {
            // The join's own coordinated (bypass-once) load went through the
            // engine - arm the stalled-signal backstop for it too.
            g_loadPumpArmTick = GetTickCount();
        }
    }

    if (g_cfg.isHost) {
        // Join load requests -> the ONE authoritative load (the loadSave
        // below re-enters the detour, whose edge broadcasts the GO above).
        std::deque<coop::InboundLoadReq> reqs;
        g_inbound.drainLoadReqs(reqs);
        for (std::deque<coop::InboundLoadReq>::iterator it = reqs.begin();
             it != reqs.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            if (coop::savexfer::folderFingerprint(name) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] REQ from join id=%u name='%s' REFUSED (no such save here)",
                          it->pkt.reqId, name);
                b[sizeof(b) - 1] = '\0'; coopErr(b);
                continue;
            }
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[load] REQ from join id=%u name='%s' -> loading",
                      it->pkt.reqId, name);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (!coop::engine::loadSave(name))
                coopErr("[load] join-requested load FAILED to issue");
        }

        // NACKs: the join can't load our save - stream it the folder once
        // OUR OWN reload has completed (never mid-swap).
        std::deque<coop::InboundLoadNack> nacks;
        g_inbound.drainLoadNacks(nacks);
        for (std::deque<coop::InboundLoadNack>::iterator it = nacks.begin();
             it != nacks.end(); ++it) {
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            char b[176];
            if (it->pkt.loadId != g_loadIdOut) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] stale NACK id=%u (current %u) ignored",
                          it->pkt.loadId, g_loadIdOut);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                continue;
            }
            _snprintf(b, sizeof(b) - 1,
                      "[load] NACK id=%u name='%s' joinFp=%08x -> transfer after reload",
                      it->pkt.loadId, name, it->pkt.fingerprint);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            g_loadXferPending = name;
        }
        if (!g_loadXferPending.empty() && coop::engine::gameplayLive(gw) &&
            !coop::savexfer::sending()) {
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "[load] starting fallback transfer name='%s'",
                      g_loadXferPending.c_str());
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            coop::savexfer::beginSend(g_net, g_net.localId(), g_loadXferPending);
            g_loadXferPending.clear();
        }
        // The chunk pump normally lives in driveSaveSync; keep the fallback
        // transfer moving even when saveSync is gated off.
        if (!g_cfg.saveSync && coop::savexfer::sending())
            coop::savexfer::tickSend(g_net, g_net.localId());
    } else {
        // LOAD_GOs: verify our on-disk copy and follow the host.
        std::deque<coop::InboundLoadGo> gos;
        g_inbound.drainLoadGos(gos);
        for (std::deque<coop::InboundLoadGo>::iterator it = gos.begin();
             it != gos.end(); ++it) {
            if (it->pkt.loadId <= g_loadIdSeen) continue; // stale/duplicate
            g_loadIdSeen = it->pkt.loadId;
            char name[sizeof(it->pkt.name) + 1];
            memcpy(name, it->pkt.name, sizeof(it->pkt.name));
            name[sizeof(it->pkt.name)] = '\0';
            if (!name[0]) continue;
            coop::u32 fp = coop::savexfer::folderFingerprint(name);
            char b[192];
            if (fp != 0 && fp == it->pkt.fingerprint) {
                // Already in this exact save? A connect-triggered push (host
                // bakes its current save and announces it) would otherwise
                // reload the join into the world it is already in - a pointless
                // load-screen hitch for the classic "both pre-loaded the same
                // save" flow. Skip only when in-game AND the loaded save name
                // matches; a title-screen join (not yet in-game) must still
                // load to actually enter the world.
                char curp[64]; curp[0] = '\0';
                bool alreadyIn = false;
                if (g_gameStarted) {
                    coop::engine::saveInfo(curp, sizeof(curp), 0, 0);
                    alreadyIn = (curp[0] && _stricmp(curp, name) == 0);
                }
                if (alreadyIn) {
                    _snprintf(b, sizeof(b) - 1,
                              "[load] GO id=%u name='%s' fp=%08x MATCH - already loaded, skip",
                              it->pkt.loadId, name, fp);
                    b[sizeof(b) - 1] = '\0'; coopLog(b);
                    g_loadAfterCommit.clear();
                } else {
                    _snprintf(b, sizeof(b) - 1,
                              "[load] GO id=%u name='%s' fp=%08x MATCH -> loading",
                              it->pkt.loadId, name, fp);
                    b[sizeof(b) - 1] = '\0'; coopLog(b);
                    warnIfNoPortraits(name);
                    g_loadAfterCommit.clear();
                    coop::engine::setLoadBypassOnce();
                    if (!coop::engine::loadSave(name))
                        coopErr("[load] coordinated load FAILED to issue");
                }
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] GO id=%u name='%s' hostFp=%08x localFp=%08x %s -> NACK (transfer)",
                          it->pkt.loadId, name, it->pkt.fingerprint, fp,
                          fp == 0 ? "MISSING" : "DIVERGED");
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                coop::LoadNackPacket nk;
                memset(&nk, 0, sizeof(nk));
                nk.type        = (coop::u8)coop::PKT_LOAD_NACK;
                nk.ownerId     = g_net.localId();
                nk.loadId      = it->pkt.loadId;
                nk.fingerprint = fp;
                strncpy(nk.name, name, sizeof(nk.name) - 1);
                g_net.queueLoadNack(nk);
                g_loadAfterCommit = name;
                g_loadCommitBase  = coop::savexfer::commitSeq();
            }
        }

        // The transfer's receive half (BEGIN/FILE/DONE -> stage/verify/
        // commit/ACK) normally lives in driveSaveSync; run it here when
        // saveSync is gated off so the fallback transfer still lands.
        if (!g_cfg.saveSync) {
            std::deque<coop::InboundSaveBegin> begins;
            g_inbound.drainSaveBegins(begins);
            for (std::deque<coop::InboundSaveBegin>::iterator it = begins.begin();
                 it != begins.end(); ++it)
                coop::savexfer::onSaveBegin(it->pkt);
            std::deque<coop::InboundSaveFile> chunks;
            g_inbound.drainSaveFiles(chunks);
            for (std::deque<coop::InboundSaveFile>::iterator it = chunks.begin();
                 it != chunks.end(); ++it)
                coop::savexfer::onSaveFile(it->hdr, it->path.c_str(),
                                           it->data.empty() ? 0 : &it->data[0]);
            std::deque<coop::InboundSaveDone> dones;
            g_inbound.drainSaveDones(dones);
            for (std::deque<coop::InboundSaveDone>::iterator it = dones.begin();
                 it != dones.end(); ++it) {
                coop::u16 files = 0;
                unsigned __int64 bytes = 0;
                int ok = coop::savexfer::onSaveDone(it->hdr,
                                                    it->crcs.empty() ? 0 : &it->crcs[0],
                                                    &files, &bytes);
                coop::SaveAckPacket ack;
                memset(&ack, 0, sizeof(ack));
                ack.type    = (coop::u8)coop::PKT_SAVE_ACK;
                ack.ownerId = g_net.localId();
                ack.xferId  = it->hdr.xferId;
                ack.ok      = ok ? 1 : 0;
                ack.files   = files;
                ack.bytes   = bytes;
                g_net.queueSaveAck(ack);
            }
        }

        // Pending latch: load once the transfer for OUR save committed.
        if (!g_loadAfterCommit.empty() &&
            coop::savexfer::commitSeq() > g_loadCommitBase) {
            if (coop::savexfer::lastCommitResult() == 1 &&
                _stricmp(coop::savexfer::lastCommitName().c_str(),
                         g_loadAfterCommit.c_str()) == 0) {
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] transfer committed -> loading '%s'",
                          g_loadAfterCommit.c_str());
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                warnIfNoPortraits(g_loadAfterCommit);
                coop::engine::setLoadBypassOnce();
                if (!coop::engine::loadSave(g_loadAfterCommit))
                    coopErr("[load] post-transfer load FAILED to issue");
                g_loadAfterCommit.clear();
            } else {
                // Some other/failed commit landed; re-base and keep waiting.
                g_loadCommitBase = coop::savexfer::commitSeq();
            }
        }
    }
}

// Co-op session panel (F2) + status overlay. Interactive sessions only - the
// unattended harness (scenario / self-exit timer) never touches the panel, and
// keeping the GUI stack out of those runs avoids perturbing the scenario
// oracles. Both calls are SEH-guarded internally and touch only GUI + input +
// (guarded) leader read, so they are safe wherever the GUI stack is up. gw may
// be null (title screen): coopOverlayTick then finds no leader and hides the
// banner, while the panel itself needs no world. Driven from BOTH the in-game
// mainLoop_hook and the title-screen titleUpdate_hook so a join can go ONLINE
// (and copy/paste Steam IDs) straight from the main menu.
void coopPanelDrive(GameWorld* gw) {
    if (!(g_cfg.scenario.empty() && g_cfg.testSeconds == 0)) return;
    coop::engine::CoopPanelState ps;
    ps.selfSteamId  = (unsigned long long)coop::steamp2p::selfId();
    ps.peerSteamId  = g_cfg.steamPeer;
    ps.running      = g_net.isRunning();
    ps.peerPresent  = g_peerPresent;
    ps.isHost       = g_cfg.isHost;
    ps.transportSel = (g_cfg.transport == "steam") ? 0 : 1;
    std::string detail;
    int ostate;
    if (g_peerPresent) {
        detail = g_cfg.isHost ? "Connected - peer joined" : "Connected to host";
        ostate = 2;
    } else if (g_net.isRunning()) {
        detail = g_cfg.isHost ? "Hosting - waiting for peer..." : "Connecting...";
        ostate = 1;
    } else {
        detail = "Offline - press F2, then set Connection to ONLINE";
        ostate = 0;
    }
    ps.detail = detail.c_str();
    // Still pump Steam callbacks so an inbound "Join Game" (a friend inviting
    // US) can fire coopUiConnect; the outbound invite/picker UI is gone.
    coop::steaminvite::tick();

    coop::engine::coopPanelTick(&ps, &coopUiConnect, &coopUiDisconnect);
    coop::engine::coopOverlayTick(gw, detail.c_str(), ostate, g_net.isRunning());
}

// Main-thread tick hook: the one safe point where we touch game state.
void mainLoop_hook(GameWorld* gw, float dt) {
    ++g_tick;
    g_lastGw = gw; // cache for the argument-less F2 UI callbacks

    coopPanelDrive(gw);

    // Protocol 32: world-swap edge detection. Runs FIRST so the reload edge
    // (and, under load-sync, the session reset) lands before any sync code
    // touches pointers from the torn-down world this tick.
    if (g_gameStarted) {
        bool live = coop::engine::gameplayLive(gw);
        if (!live && g_swapStartTick == 0) {
            g_swapStartTick = GetTickCount();
            g_swapHookTicks = 0;
            coopLog("[load] WORLD-SWAP begin (gameplay went non-live)");
        } else if (!live) {
            ++g_swapHookTicks; // probe evidence: does the hook tick during the load screen?
        } else if (g_swapStartTick != 0) {
            DWORD swapMs = GetTickCount() - g_swapStartTick;
            g_swapStartTick = 0;
            char b[160];
            if (swapMs < SWAP_MIN_MS) {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-FLICKER ms=%lu (ignored, below reload threshold)",
                          (unsigned long)swapMs);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            } else {
                _snprintf(b, sizeof(b) - 1,
                          "[load] WORLD-RELOAD swapMs=%lu hookTicksDuringSwap=%u",
                          (unsigned long)swapMs, (unsigned)g_swapHookTicks);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // Session reset (protocol 32): the old world is gone - every
                // pointer cache and session map describes it. Both sides run
                // this on their OWN reload edge; peer presence and the
                // suppression levers survive (the connection never dropped).
                if (g_cfg.loadSync) {
                    g_repl.resetSession();
                    g_inbound.flushWorldState();
                    coopLog("[load] inbound world-state queues flushed");
                }
            }
        }
    }

    if (!g_gameStarted && coop::engine::gameplayLive(gw)) {
        g_gameStarted   = true;
        g_gameStartTick = GetTickCount();
        coopLog("KenshiCoop: gameplay started");
        // Coordinated load (protocol 32): the title-screen auto-load fired the
        // load detour BEFORE gameplay - discard its queued edge here, or the
        // first driveLoadSync tick (g_gameStarted now true) would mistake it
        // for a mid-session host load and broadcast a spurious PKT_LOAD_GO.
        {
            coop::engine::LoadEdge stale[8];
            unsigned int n = coop::engine::drainLoadEdges(stale, 8);
            if (n > 0) {
                char b[112];
                _snprintf(b, sizeof(b) - 1,
                          "[load] %u pre-gameplay load edge(s) discarded (title-screen auto-load)",
                          n);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
            }
        }
        // Speed-intent capture (vote/effective decoupling): detour the engine's
        // speed setters so every USER action (button, keyboard pause, simulated
        // click) registers as a vote, while our own quiet applies stay invisible.
        // Installed at gameplay start so the seed reads the save's live speed.
        if (coop::engine::installSpeedIntentHooks(gw))
            coopLog("[speed] intent hooks installed (setGameSpeed/userPause/togglePause)");
        else
            coopLog("[speed] FAILED to install intent hooks (vote capture degraded)");
        // Push-save-on-connect ordering: if a peer connected while we were still
        // at the menu / loading, its connect edge could not bake a save (no live
        // world yet). Now that gameplay is live, arm the connect-push so the
        // waiting join gets pulled into this world.
        if (g_cfg.isHost && g_cfg.saveSync && g_peerPresent)
            armConnectPush();
    }

    // Manual-validation helper (host only): KENSHICOOP_AUTORECRUIT=N seconds -
    // ONCE, N s after gameplay settles, programmatically recruit the nearest
    // non-player world NPC. probeRecruit -> recruitNpc -> g_recruitFn is the
    // SAME PlayerInterface::recruit the dialog "join me" outcome hits, so
    // recruit_hook authors the edge and the whole recruit-sync path runs. Lets
    // us exercise recruit on any populated save (camp/squad1) with no dialog-
    // hireable NPC. OFF by default (0 = no-op, zero cost).
    if (g_cfg.isHost && g_gameStarted) {
        static int  autoRecruitS    = -1;
        static bool autoRecruitDone = false;
        if (autoRecruitS < 0) {
            const char* e = std::getenv("KENSHICOOP_AUTORECRUIT");
            autoRecruitS = e ? std::atoi(e) : 0;
        }
        if (autoRecruitS > 0 && !autoRecruitDone &&
            (GetTickCount() - g_gameStartTick) >= (DWORD)autoRecruitS * 1000u) {
            autoRecruitDone = true;
            unsigned int hb[5], ha[5];
            // Wide reach (600u): a caged/imprisoned player squad (the camp save)
            // sits well beyond the scenario's 60u nearest-NPC probe from the
            // guards/prisoners it should recruit.
            int res = coop::engine::probeRecruit(gw, false, hb, ha, 600.0f);
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                "[recruit] AUTORECRUIT res=%d before=%u,%u,%u,%u,%u "
                "after=%u,%u,%u,%u,%u", res, hb[0], hb[1], hb[2], hb[3], hb[4],
                ha[0], ha[1], ha[2], ha[3], ha[4]);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Test-runner self-exit: quit cleanly after the configured duration so
    // unattended runs terminate on their own and flush their logs. Also a hard
    // backstop for a scenario that never reports completion.
    if (g_cfg.testSeconds > 0 && g_gameStarted &&
        (GetTickCount() - g_gameStartTick) >= (DWORD)g_cfg.testSeconds * 1000u) {
        coopLog("KenshiCoop: test duration elapsed; exiting");
        coop::logClose();
        // TerminateProcess (not ExitProcess): we're inside the live game loop
        // with GPU/audio/net threads running; orderly teardown deadlocks on the
        // loader lock. Our log is already flushed/closed above.
        TerminateProcess(GetCurrentProcess(), 0);
    }

    processNetEvents(gw);

    // Coordinated load (protocol 32): the JOIN never loads locally while a
    // peer is connected under the gate - the host arbitrates; a manual load
    // press forwards as PKT_LOAD_REQ. Evaluated per tick (not just on the
    // connect edge) so the title-screen auto-load - which precedes gameplay -
    // is never swallowed.
    {
        bool want = !g_cfg.isHost && g_cfg.loadSync && g_peerPresent && g_gameStarted;
        if (want != g_loadSuppressOn) {
            g_loadSuppressOn = want;
            coop::engine::setLoadSuppress(want);
            coopLog(want ? "[load] JOIN load suppression ON (host arbitrates loads)"
                         : "[load] JOIN load suppression OFF");
        }
    }

    // Test-scene setup: host spawns the controlled scene ONCE, a few seconds after
    // gameplay starts, then leaves the game running so the user can arrange the
    // pose (e.g. seat the character) and SAVE. Baking it into a save gives the
    // chair/NPC save-stable hands that resolve on both clients.
    if (!g_setupDone && !g_cfg.setupScene.empty() && g_cfg.isHost && gw &&
        g_gameStarted && (GetTickCount() - g_gameStartTick) >= SETUP_DELAY_MS) {
        g_setupDone = true;
        if (g_cfg.setupScene == "craft") {
            // Crafting/gathering (Stage 3a): spawn a work fixture + an NPC and force
            // the NPC to work it, so the host streams the work task. Both clients
            // load the baked save, so the fixture has save-stable hands on both.
            coop::engine::setupCraftScene(gw);
        } else if (g_cfg.setupScene == "down") {
            // Body-state (Stage 2) BAKE: spawn a non-squad world NPC and ragdoll it,
            // so the user can SAVE a 'down1' that both clients then load.
            coop::engine::setupDownScene(gw);
        } else if (g_cfg.setupScene == "downhold") {
            // Body-state VALIDATION: the subject is already baked into the save - do
            // NOT spawn a duplicate. The periodic re-arm below keeps it down.
            coopLog("SETUP(downhold): no spawn - re-arm keeps baked down bodies down");
        } else if (g_cfg.setupScene == "duel") {
            // Combat (Stage 3c) BAKE: spawn two PEACEFUL non-squad NPCs (same faction)
            // so the user can SAVE a neutral 'duel1' that both clients load. The fight
            // is triggered live later (combat_order) so the join sees the transition.
            bool ok = coop::engine::setupDuelScene(gw);
            coopLog(ok ? "SETUP(duel): peaceful duelists spawned - SAVE 'duel1' now"
                       : "SETUP(duel): duelist spawn FAILED");
        } else if (g_cfg.setupScene == "squad") {
            // Bidirectional presence (Phase 3.5) BAKE: build a SECOND player squad tab
            // so host (tab 0) and join (tab 1) each own a tab. User SAVEs e.g. 'squad1'.
            bool ok = coop::engine::setupSquadScene(gw);
            coopLog(ok ? "SETUP(squad): second squad tab built - SAVE your two-tab save now"
                       : "SETUP(squad): squad-tab build FAILED");
        } else if (g_cfg.setupScene == "bedcage") {
            // Bed+cage occupancy (protocol 19) BAKE: spawn a bed and a prison
            // cage near the leader so both clients load save-stable furniture
            // hands. With KENSHICOOP_BAKESAVE set the save is written
            // automatically a few seconds later (no manual menu round-trip).
            bool ok = coop::engine::setupBedCageScene(gw);
            coopLog(ok ? "SETUP(bedcage): bed + cage spawned - SAVE 'bedcage1' now"
                       : "SETUP(bedcage): spawn FAILED");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "pole") {
            // Prisoner-pole occupancy (protocol 19 kind=4) BAKE: spawn one
            // standing prisoner POLE in front of the leader so both clients load
            // a save-stable pole hand. The pole_put controlled test then KOs a PC
            // and setPrisonMode's it onto the pole (visibly a body ON A POLE, not
            // in a cage). With KENSHICOOP_BAKESAVE the save writes automatically.
            bool ok = coop::engine::setupPoleScene(gw);
            coopLog(ok ? "SETUP(pole): prisoner pole spawned - SAVE 'pole1' now"
                       : "SETUP(pole): spawn FAILED (no pole template? see candidates)");
            if (ok && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 8000; // let physics/grounding settle
        } else if (g_cfg.setupScene == "buffpc") {
            // Stat buff BAKE (single client): raise EVERY player-squad PC to 120 in
            // all stats, then leave the game running so the user can SAVE manually
            // (or auto-bake if KENSHICOOP_BAKESAVE is set). No coop peer required.
            unsigned int nb = coop::engine::buffAllPlayerStats(gw, 120.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SETUP(buffpc): buffed %u PC(s) to 120 in every stat - SAVE now", nb);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
            if (nb > 0 && !g_cfg.bakeSave.empty())
                g_bakeSaveTick = GetTickCount() + 4000; // let the recalc settle
        } else if (g_cfg.setupScene == "inventory") {
            // Inventory (Phase 4a) BAKE: spawn a save-stable storage container in front
            // of the leader + seed it with items so both clients load an identical
            // container with a resolvable hand. User SAVEs e.g. 'inv1'.
            unsigned int ch[5] = { 0, 0, 0, 0, 0 };
            bool ok = coop::engine::setupInventoryScene(gw, ch);
            if (ok) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "SETUP(inventory): container hand=%u,%u,%u,%u,%u - SAVE 'inv1' now",
                ch[0], ch[1], ch[2], ch[3], ch[4]); b[sizeof(b) - 1] = '\0'; coopLog(b); }
            else coopLog("SETUP(inventory): container prep FAILED");
        } else {
            RootObject* seat = 0;
            bool ok = coop::engine::spawnSeatInFront(gw, 7.0f, 0.0f, &seat);
            if (ok && seat) {
                unsigned int h[5];
                if (coop::engine::readObjectHand(seat, h))
                    { char b[160]; _snprintf(b, sizeof(b)-1,
                        "SETUP: spawned seat hand=%u,%u,%u,%u,%u",
                        h[3], h[4], h[0], h[1], h[2]); b[sizeof(b)-1]='\0'; coopLog(b); }
                else coopLog("SETUP: spawned seat (hand unread)");
            } else {
                coopLog("SETUP: seat spawn FAILED (no seat template or createBuilding faulted)");
            }
            if (g_cfg.setupScene == "npc") {
                Character* npc = coop::engine::spawnNpcInFront(gw, 2.5f, 1.0f);
                coopLog(npc ? "SETUP: spawned world NPC" : "SETUP: NPC spawn FAILED");
            }
        }
        coopLog("SETUP: scene ready - arrange the pose and SAVE the game now");
    }

    // Deferred auto-bake: write the fixture save once the armed settle window
    // elapses. One-shot; the self-exit timer (KENSHICOOP_TEST_SECONDS) then
    // ends the bake run.
    if (g_bakeSaveTick != 0 && GetTickCount() >= g_bakeSaveTick) {
        g_bakeSaveTick = 0;
        bool ok = coop::engine::saveGameAs(g_cfg.bakeSave);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SETUP: auto-bake save '%s' %s",
                  g_cfg.bakeSave.c_str(), ok ? "ISSUED" : "FAILED");
        b[sizeof(b) - 1] = '\0'; coopLog(b);
    }

    // Craft re-arm (host): a baked work goal does not persist across save/load, and a
    // world worker's own AI can drift off-station. Re-issue the work goal on an
    // interval so the host keeps streaming the work task throughout the scene. The
    // call is throttled and no-ops when the worker is already on task, so it never
    // thrashes the worker's pathing. Only the 'craft' scene arms this.
    if (g_cfg.setupScene == "craft" && g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmCraftScene(gw);
    }

    // Down re-arm (host): a healthy ragdolled body recovers and stands back up, and
    // ragdoll state does not survive save/load. Re-knock the nearby non-squad bodies
    // on an interval so they stay on the ground throughout the scene. Both the bake
    // ('down') and the spawn-free validation ('downhold') arm this.
    if ((g_cfg.setupScene == "down" || g_cfg.setupScene == "downhold") &&
        g_cfg.isHost && gw && g_gameStarted &&
        (GetTickCount() - g_lastCraftRearmTick) >= CRAFT_REARM_MS) {
        g_lastCraftRearmTick = GetTickCount();
        coop::engine::rearmDownScene(gw);
    }

    // Inventory owner registration (host, Phase 4a): when inventory sync is active,
    // (re)resolve the baked storage container nearest the leader (else the leader's own
    // inventory) and register it as the owned container so publishInventories streams
    // its contents. Re-resolved on an interval because the container hand only resolves
    // once the world block has loaded. The join never registers (it reconciles).
    if (g_cfg.invSync && g_cfg.isHost && gw && g_gameStarted) {
        static DWORD lastInvReg = 0;
        if (GetTickCount() - lastInvReg >= (DWORD)CRAFT_REARM_MS) {
            lastInvReg = GetTickCount();
            unsigned int ch[5];
            if (coop::engine::pickInventoryContainer(gw, ch))
                g_repl.setOwnedContainerHand(ch);
        }
    }

    // Scenario completion hold: once a verdict is logged, keep driving the synced
    // bodies on screen for the capture window, then self-exit cleanly.
    if (g_scenario && g_scenarioDoneTick != 0) {
        if (GetTickCount() - g_scenarioDoneTick >= SCENARIO_HOLD_MS) {
            coop::logClose();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }

    // Coordinated load (protocol 32): while a world swap is in progress the
    // OLD world is being torn down under us - every cached Character*/
    // RootObject* is dangling, and mainLoop_hook keeps ticking throughout
    // (~500 ticks across a ~4 s load). Touching those pointers mid-swap is a
    // pure-virtual-call crash (a virtual on a half-destroyed object). Skip
    // ALL replication until the reload edge (gameplay live again) runs the
    // session reset at the top of the hook, which clears the stale caches -
    // the next live tick then resumes replication with a fresh world.
    // Sampled ONCE (pre-engine): if the engine tick below completes the
    // reload, the reset still runs first NEXT tick, so we stay hands-off this
    // tick either way. Before first gameplay this is simply false (no caches
    // yet). driveLoadSync itself is NOT gated on this - it only touches net
    // packets + file IO + loadSave, never cached world pointers.
    const bool worldLive = g_gameStarted && coop::engine::gameplayLive(gw);

    // --- Replication: BIDIRECTIONAL presence. Both clients stream their OWNED squad
    // subset (partitioned by hand-rank) and drive the peer's; the host additionally
    // streams world NPCs. Ingest received targets BEFORE the engine tick so apply
    // targets are current.
    if (worldLive) {
        g_repl.ingest(g_inbound);
        // Phase 4a: drain received container-contents snapshots into the per-container
        // cache (reconciled after the engine tick by applyInventories).
        g_repl.ingestInv(g_inbound);
        // Both clients latch reliable transition events (KO/death/revive) for the bodies
        // they drive, before apply (a side can emit an event for its own owned body and
        // the peer that drives that body must honour it).
        g_repl.applyEvents(gw, g_inbound);
    }
    if (worldLive) {
        g_repl.publishOwned(gw, g_net, g_net.localId());
        // Both clients stream the contents of every squad member they OWN (host tab 0,
        // join tab 1) on content-change - bidirectional, disjoint by the same tab
        // partition as positional sync. Gated on invSync so ordinary co-op sessions add
        // no inventory traffic; the peer reconciles via applyInventories (skips own).
        if (g_cfg.invSync)
            g_repl.publishInventories(gw, g_net, g_net.localId());
        // Protocol 37: BOTH clients diff every tracked container (own + received)
        // against its baseline to catch a completed cross-owner UI drag - the one
        // inventory write the single-writer snapshots cannot represent - and author
        // a reliable PKT_INV_XFER so the peer relocates its own copy (conservation).
        // RETIRED by the trade veto (blockXfer): a refused drag can never complete,
        // so there is nothing to detect/replicate - Config forces xferSync off when
        // blockXfer is on, making this (and applyTransfers below) a no-op. The
        // xferLatch_/xferDefer_ reconcile-race machinery then stays dormant (never
        // populated). KENSHICOOP_BLOCK_XFER=0 restores this replicate-the-trade path.
        if (g_cfg.xferSync)
            g_repl.detectAndPublishTransfers(gw, g_net, g_net.localId());
        // Phase W1 (bidirectional): BOTH clients stream the free ground items they
        // author in their interest sphere - owner-scoped netId spaces, peer items
        // filtered by the proxy echo guard - so a join-side drop of materials/food
        // finally appears on the host. Proxies reconcile after the engine tick
        // (applyWorldItems, also both sides now).
        if (g_cfg.worldSync)
            g_repl.publishWorldItems(gw, g_net, g_net.localId());
        // Phase W2: BOTH clients watch their OWNED characters for a WEAPON drop and author a
        // reliable conservation intent so the peer relocates its own copy of that weapon (a
        // weapon can't be rebuilt via the W1 proxy path). Bidirectional; gated on worldSync.
        if (g_cfg.worldSync)
            g_repl.detectAndPublishWeaponDrops(gw, g_net, g_net.localId());
        // Phase 2 (player combat + medical): owner-authoritative vitals sync for
        // player-squad members, both directions. publishMedical streams OUR
        // members' medical model (change-gated, reliable); applyMedical writes
        // received snapshots onto the peer copies we drive AND forwards any
        // local first aid administered on those copies back to their owner;
        // applyTreatments lands forwarded first aid on the bodies we own.
        // Ordered after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.medSync) {
            g_repl.publishMedical(gw, g_net, g_net.localId());
            g_repl.applyMedical(gw, g_inbound, g_net, g_net.localId());
            g_repl.applyTreatments(gw, g_inbound);
        }
        // Character stats sync (protocol 17): owner-authoritative CharStats
        // stream for player-squad members, both directions. publishStats
        // streams OUR members' stats (change-gated, reliable); applyStats
        // writes received snapshots onto the peer copies we drive. Ordered
        // after publishOwned (they use the ownHands_ set it refreshes).
        if (g_cfg.statsSync) {
            g_repl.publishStats(gw, g_net, g_net.localId());
            g_repl.applyStats(gw, g_inbound);
        }
        // Per-tab wallet sync (protocol 22): each client streams the money of
        // the squad tabs it OWNS (change-gated reliable, keyed by tab rank);
        // received snapshots land on the peer tabs via Ownerships::setMoney.
        // Ordered after publishOwned (ownership ranks are the partition rule).
        if (g_cfg.moneySync) {
            g_repl.publishMoney(gw, g_net, g_net.localId());
            g_repl.applyMoney(gw, g_inbound);
        }
        // Recruitment sync (protocol 23): drain the recruit detour's edge queue
        // into reliable EVT_RECRUIT events (subject = old hand, actor = new
        // hand) and pin recruited hands to their recruiter's ownership. The
        // receive half (re-key) lives in applyEvents above. Ordered after
        // publishOwned so this tick's recruits pin BEFORE next tick's census.
    if (g_cfg.recruitSync)
        g_repl.publishRecruits(gw, g_net, g_net.localId());

    // Squad management sync (protocol 35): poll the roster's pointer->hand
    // baseline (~2 Hz; a squad-tab move re-containers the body but the
    // Character* survives) and author reliable EVT_SQUAD_MOVE re-key edges,
    // pinning moved hands to the mover's ownership. The receive half (shared
    // EVT_RECRUIT re-key path) lives in applyEvents above.
    if (g_cfg.squadSync)
        g_repl.publishSquadMoves(gw, g_net, g_net.localId());

    // Faction-relation sync (protocol 24): stream player-faction relation rows
    // that moved locally (change-gated reliable, sampled ~1 Hz or immediately
    // on a detoured affectRelations mutation) and apply received rows onto
    // both local table directions. Both clients run the same detector; the
    // applied-row baseline update keeps the channel echo-free.
    if (g_cfg.factionSync) {
        g_repl.publishFactions(gw, g_net, g_net.localId());
        g_repl.applyFactions(gw, g_inbound);
    }
    // Door-state sync (protocol 26): stream baked-door rows whose (open,
    // locked) moved locally (change-gated reliable, ~1 Hz sample) and apply
    // received rows through the engine's own door actions. Both clients run
    // the same detector; the applied-row baseline update keeps it echo-free.
    if (g_cfg.doorSync) {
        g_repl.publishDoors(gw, g_net, g_net.localId());
        g_repl.applyDoors(gw, g_inbound);
    }
    // Placed-building sync (protocol 27): announce local placements (UI
    // detour edges + programmatic scenario places) as describe/mint keys,
    // stream change-gated construction-progress rows for buildings WE placed,
    // and mint + progress-apply the peer's. Placer-authoritative; a mint
    // never re-announces (echo-free by construction).
    if (g_cfg.buildSync) {
        g_repl.publishBuilds(gw, g_net, g_net.localId());
        g_repl.applyBuilds(gw, g_inbound);
    }
    // Placed-building doors (protocol 28): sample the session build maps'
    // doors and stream change-gated rows on the translated (placer key +
    // door index) identity; apply received rows through the same maps. The
    // removal half lives inside publishBuilds/applyBuilds (it shares the
    // build-edge drain); everything is gated by KENSHICOOP_BDOOR_SYNC.
    if (g_cfg.buildSync && g_cfg.bdoorSync) {
        g_repl.publishBuildDoors(gw, g_net, g_net.localId());
        g_repl.applyBuildDoors(gw, g_inbound);
    }
    // Production machine sync (protocol 33): the HOST is the machine
    // authority - it samples machine-class buildings in the interest
    // spheres (~1 Hz) and streams change-gated PKT_PROD rows; the join
    // applies received rows through the engine's own levers. Host-only
    // direction (world-simulation precedent), so there is no echo path.
    if (g_cfg.prodSync) {
        if (g_cfg.isHost)
            g_repl.publishProd(gw, g_net, g_net.localId());
        else
            g_repl.applyProd(gw, g_inbound);
    }
    // Research tech-tree sync (protocol 38): the HOST is the tech-tree
    // authority - it samples its Research store's known set ~1 Hz and streams
    // one reliable PKT_RESEARCH row per known sid; the join applies via
    // Research::startResearch (idempotent). Host-only direction, no echo path.
    if (g_cfg.researchSync) {
        if (g_cfg.isHost)
            g_repl.publishResearch(gw, g_net, g_net.localId());
        else
            g_repl.applyResearch(gw, g_inbound);
    }
        // Stealth sync (protocol 20): the HOST is the world-detection authority
        // - it streams each DRIVEN sneaker's whoSeesMeSneaking back to the
        // sneaker's owner; every client replays received snapshots onto the
        // bodies it OWNS (the indicators render on the owner's screen). The
        // posture half lives inside applyTargets (continuous BODY_SNEAK apply).
        if (g_cfg.stealthSync) {
            if (g_cfg.isHost)
                g_repl.publishStealth(gw, g_net, g_net.localId());
            g_repl.applyStealthFeedback(gw, g_inbound);
        }
        // Consensus game-speed sync: detect local speed clicks as REQUESTS,
        // host arbitrates effective = min(requests) (capped at 1x while either
        // player squad fights) and broadcasts; the join applies the SET. Runs
        // after publishOwned (the combat flag samples the ownHands_ set).
        if (g_cfg.speedSync)
            g_repl.syncSpeed(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
        // Phase 6 (6a evidence spike): env-gated ([shackledbg]) per-character
        // shackle/lock trace. No-op unless KENSHICOOP_DEBUG_SHACKLE=1, so it is
        // free to leave in the tick for manual-session characterization.
        coop::engine::shackleDbgTick(gw, g_cfg.isHost);
        // Game-clock sync (protocol 25): the host broadcasts its absolute
        // in-game clock ~1 Hz; the join measures the offset and SLEWS - a
        // multiplier the speed layer's quiet writes fold in on top of the
        // arbitrated consensus effective. AFTER syncSpeed so a slew change
        // applies against this tick's consensus state.
        if (g_cfg.timeSync)
            g_repl.syncTime(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
        // Runtime-spawn proxy replication (protocol 21): the join asks about
        // streamed hands it couldn't resolve last tick (host RUNTIME spawns)
        // and mints local proxy bodies from the host's replies; the host
        // answers requests. BEFORE the engine tick so a proxy bound this
        // frame is driven by this frame's applyTargets.
        if (g_cfg.spawnSync)
            g_repl.syncSpawns(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
    }

    // Coordinated save + load (protocols 31/32) run under g_gameStarted, NOT
    // worldLive: they must keep pumping DURING a world swap (the join drains
    // the host's LOAD_GO and issues its own load; the host's fallback
    // transfer and the deferred-signal state advance) and they only touch net
    // packets, file IO and the load/save ENTRY points - never a cached world
    // pointer that the swap invalidates.
    if (g_gameStarted) {
        // Coordinated save + session resume (protocol 31): host-arbitrated
        // save edges, folder-quiescence completion, paced in-band folder
        // transfer, staged+verified commit on the join.
        if (g_cfg.saveSync)
            driveSaveSync();
        // Coordinated load (protocol 32): host-arbitrated load edges,
        // fingerprint-verified join follow, SaveXfer fallback on divergence.
        if (g_cfg.loadSync)
            driveLoadSync(gw);
    }

    // Scenario onStart fires once, BEFORE the engine tick, so a host-issued move
    // order takes effect this frame.
    //
    // PEER-READY ARMING: the scenario clock does NOT start at gameplay start - it
    // starts when this client first receives a peer's owned-entity batch
    // (Inbound::sawRemoteEntity). On the HOST that flips true exactly when the
    // JOIN is loaded + streaming, so every scripted host action (orders, kills,
    // item adds - all timed off ctx.elapsedMs) happens with the join watching,
    // instead of racing its load screen. On the JOIN the host's stream is already
    // arriving by the time it reaches gameplay, so it arms ~immediately - which
    // also aligns both clients' scenario clocks to roughly the same wall moment.
    // Fallback: arm anyway after scenarioArmTimeoutMs of gameplay (peer never
    // connected / host-only diagnostics); 0 = arm immediately (spike runs).
    if (g_scenario && g_gameStarted && gw && !g_scenarioStarted) {
        bool  peerReady = g_inbound.sawRemoteEntity();
        DWORD waitedMs  = GetTickCount() - g_gameStartTick;
        bool  fallback  = (g_cfg.scenarioArmTimeoutMs == 0) ||
                          (waitedMs >= (DWORD)g_cfg.scenarioArmTimeoutMs);
        // Pre-arm phase: every tick until arming, let the scenario pin/hold its
        // subjects while the freshly-loaded world is still in its baked pose
        // (waiting a join-load before pinning loses wandering NPCs).
        {
            coop::ScenarioContext pctx;
            pctx.gw = gw; pctx.isHost = g_cfg.isHost; pctx.localId = g_net.localId();
            pctx.elapsedMs = waitedMs; pctx.tick = g_scenarioTick;
            pctx.peerReady = peerReady;
            g_scenario->onGameplay(pctx);
        }
        if (peerReady || fallback) {
            g_scenarioStarted   = true;
            g_scenarioStartTick = GetTickCount();
            coop::ScenarioContext ctx;
            ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
            ctx.elapsedMs = 0; ctx.tick = g_scenarioTick;
            ctx.peerReady = peerReady;
            char m[200];
            _snprintf(m, sizeof(m) - 1, "SCENARIO arm trigger=%s waitedMs=%lu",
                      peerReady ? "peer-ready" : "timeout", (unsigned long)waitedMs);
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            _snprintf(m, sizeof(m) - 1, "SCENARIO %s start", g_scenario->name());
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenario->onStart(ctx);
        }
    }

    g_mainLoop_orig(gw, dt); // run the engine (incl. local AI)

    // Apply AFTER the engine so our transform is the last word the renderer samples
    // (the local AI re-decides at the start of the next tick). Both clients drive the
    // PEER's owned bodies: the host drives the join's squad, the join drives the
    // host's squad + world NPCs. applyTargets skips our OWN owned hands.
    // Re-sample live: the engine tick above may have STARTED a swap (gameplay
    // just went non-live), in which case the caches are now stale - skip apply
    // this tick too (the reset runs next tick's top on the reload edge).
    if (worldLive && coop::engine::gameplayLive(gw)) {
        g_repl.applyTargets(gw);
        // Phase W2: relocate our own copy of any DROPPED weapon to the ground BEFORE the
        // inventory reconcile runs, so the conservation move beats the (debounced) removal
        // reconcile that would otherwise DESTROY the weapon we cannot refabricate.
        if (g_cfg.worldSync) {
            g_repl.applyWeaponDrops(gw, g_inbound);
            // Phase W3: re-home tracked ground copies into the picking character's bag, also
            // before the inventory reconcile (which can't refabricate a weapon into the proxy).
            g_repl.applyWeaponPickups(gw, g_inbound);
        }
        // Protocol 37: relocate our copy of any cross-owner TRADED item between the
        // two containers BEFORE the inventory reconcile, so the conservation move
        // beats the stale-snapshot dupe/wipe (and traded gear survives - no
        // fabrication on this path).
        if (g_cfg.xferSync)
            g_repl.applyTransfers(gw, g_inbound, g_net.localId());
        // Phase 4a: reconcile any peer-owned container we received a fresh snapshot
        // for (the join applies the host's container; the host skips its own).
        g_repl.applyInventories(gw);
        // Phase W1 (bidirectional): BOTH clients spawn/update/cull local proxies for
        // the peer's streamed ground items, keyed (ownerId, netId) so the per-sender
        // netId spaces never collide.
        if (g_cfg.worldSync)
            g_repl.applyWorldItems(gw, g_inbound);
        // Host-authoritative world: only the JOIN hides/freezes any local NPC the
        // host isn't streaming (so the join can't run a divergent copy). The host IS
        // the world authority, so it never suppresses.
        // NPC existence census (protocol 36): the host broadcasts its 1 Hz
        // wide-radius hand list; the join drains it and lets the wide-radius
        // pass in enforceHostAuthority cull local-only ghosts at render range.
        if (!g_cfg.isHost) {
            g_repl.applyNpcCensus(g_inbound);
            g_repl.enforceHostAuthority(gw);
        } else {
            g_repl.publishNpcCensus(gw, g_net, g_net.localId());
        }
        // Camera-anchored interest (protocol 43): both sides publish their
        // LOCAL camera to the engine's anchor store; the join additionally
        // ships its center to the host at ~1 Hz, and the host folds a fresh
        // peer hint into interestCenters. Runs even with CAM_INTEREST off
        // (the engine-side knob makes interestCenters ignore the anchors)
        // so an A/B toggle needs no session restart logic.
        g_repl.syncCamHint(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
    }

    // Coordinated load (protocol 32): deferred-signal backstop. The engine
    // usually consumes the LOADGAME signal on its own within ~0.5 s
    // (load_probe run 2), but run 1 saw it sit forever - if the swap hasn't
    // begun after the grace window, pump SaveManager::execute() once from
    // this end-of-tick context (the engine tick above is done with the world).
    if (g_loadPumpArmTick != 0 && g_cfg.loadSync) {
        if (g_swapStartTick != 0) {
            g_loadPumpArmTick = 0; // the swap started on its own
        } else if (GetTickCount() - g_loadPumpArmTick >= LOAD_PUMP_GRACE_MS) {
            g_loadPumpArmTick = 0;
            int delay = -1;
            int sig = coop::engine::saveMgrSignal(&delay);
            if (sig == 2 /*LOADGAME*/) {
                bool ok = coop::engine::saveMgrExecute();
                int sigAfter = coop::engine::saveMgrSignal(0);
                char b[144];
                _snprintf(b, sizeof(b) - 1,
                          "[load] EXEC pumped (deferred LOADGAME stalled) ok=%d sigAfter=%d",
                          ok ? 1 : 0, sigAfter);
                b[sizeof(b) - 1] = '\0'; coopLog(b);
                // If execute() swapped the world SYNCHRONOUSLY (signal consumed
                // and we're still live), the reload-edge detector never saw a
                // non-live frame, so it won't run the session reset - force it
                // here. The caches from the OLD world are dangling right now.
                if (ok && sigAfter != 2 && g_swapStartTick == 0 &&
                    coop::engine::gameplayLive(gw)) {
                    coopLog("[load] synchronous execute swap - forcing session reset");
                    g_repl.resetSession();
                    g_inbound.flushWorldState();
                    coopLog("[load] inbound world-state queues flushed");
                }
            }
        }
    } else if (g_loadPumpArmTick != 0) {
        g_loadPumpArmTick = 0;
    }

    // Scenario onTick AFTER apply, so a join's RECV line reflects the applied pos.
    if (g_scenario && g_gameStarted && gw && g_scenarioStarted && g_scenarioDoneTick == 0) {
        coop::ScenarioContext ctx;
        ctx.gw = gw; ctx.isHost = g_cfg.isHost; ctx.localId = g_net.localId();
        ctx.elapsedMs = GetTickCount() - g_scenarioStartTick;
        ctx.tick = ++g_scenarioTick;
        ctx.peerReady = g_inbound.sawRemoteEntity();
        if (g_scenario->onTick(ctx)) {
            // Stage 2: the receiver emits its interpolation smoothness summary
            // alongside the verdict so the runner can assert per-frame gliding.
            if (!g_cfg.isHost) g_repl.logSmoothSummary();
            bool ok = g_scenario->passed();
            char m[48];
            _snprintf(m, sizeof(m) - 1, "SCENARIO RESULT %s", ok ? "PASS" : "FAIL");
            m[sizeof(m) - 1] = '\0';
            coopLog(m);
            g_scenarioDoneTick = GetTickCount(); // begin the capture hold
        }
    }
}

// Title-screen update hook: a safe main-thread point every frame the menu is up.
// Wait g_cfg.autoLoadDelayMs after the first title frame AND until the save
// subsystem reports ready, then issue the deferred load once.
// SEH-guarded so a fault in the (title-screen-untested) GUI stack can never
// abort titleUpdate_hook before the bootstrap pump runs. No C++ unwind objects
// live in titleUpdate_hook, but coopPanelDrive uses std::string internally, so
// the guarded call lives in its own function (C2712).
void coopPanelDriveSeh(GameWorld* gw) {
    __try { coopPanelDrive(gw); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true;
            coopErr("[coop-ui] panel tick FAULTED at title screen (guarded)"); }
    }
}

void titleUpdate_hook(TitleScreen* self) {
    g_titleUpdate_orig(self);

    // Bring-up trace: log once on the first title tick, then only when the
    // online state flips (the moment the user toggles ONLINE via F2), so the log
    // shows the menu hook is live and pinpoints the connect edge without spam.
    {
        static int s_lastRunning = -1;
        int running = g_net.isRunning() ? 1 : 0;
        if (running != s_lastRunning) {
            s_lastRunning = running;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "[boot] title-tick running=%d host=%d started=%d saveEmpty=%d",
                      running, g_cfg.isHost ? 1 : 0, g_gameStarted ? 1 : 0,
                      g_cfg.save.empty() ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coopLog(b);
        }
    }

    // Push-save-on-connect (join, main menu). A join that has gone ONLINE must
    // drain the connect edge, receive the host's pushed save, and load it while
    // still at the title screen - work that normally only runs in-game from
    // mainLoop_hook (gated on g_gameStarted). Pump the join half FIRST (before
    // the panel) so a GUI fault can never block it. gw is null: processNetEvents
    // only touches it under a gw&& guard, and the JOIN branches of driveSaveSync/
    // driveLoadSync never deref it. The load path is gated on savesReady():
    // before then the host's LOAD_GO simply waits in the inbound queue (no NACK
    // -> no stream yet), and the save-receiver half still commits chunks to disk.
    if (g_net.isRunning() && !g_cfg.isHost && !g_gameStarted) {
        processNetEvents(0);
        if (g_cfg.saveSync) driveSaveSync();
        if (g_cfg.loadSync && coop::engine::savesReady()) driveLoadSync(0);
        // F2 panel while the join waits at the menu (guarded; the host's world is
        // the destination, so we skip the config auto-load for a join session).
        coopPanelDriveSeh(0);
        return;
    }

    // Co-op panel (F2) at the main menu for the HOST / offline case: lets a user
    // toggle ONLINE and paste a Steam ID before any save is loaded. Guarded
    // because the title-screen GUI stack is otherwise unexercised.
    coopPanelDriveSeh(0);

    if (g_autoLoadDone || g_cfg.save.empty()) return;

    DWORD now = GetTickCount();
    if (g_titleFirstTick == 0) { g_titleFirstTick = now; return; }
    if ((now - g_titleFirstTick) < g_cfg.autoLoadDelayMs) return;
    if (!coop::engine::savesReady()) return;

    if (coop::engine::loadSave(g_cfg.save)) {
        g_autoLoadDone = true;
        char m[128];
        _snprintf(m, sizeof(m) - 1,
                  "KenshiCoop: auto-load issued for save '%s' (after %lu ms settle)",
                  g_cfg.save.c_str(), (unsigned long)(now - g_titleFirstTick));
        m[sizeof(m) - 1] = '\0';
        coopLog(m);
    }
}

void startNetworking() {
    // Debug WAN simulation: when configured, hold/drop inbound entity batches so the
    // loopback harness exercises the real-latency path (interp + local enforcement)
    // instead of the ~0 ms same-frame delivery we'd otherwise validate against.
    if (g_cfg.netSimDelayMs || g_cfg.netSimJitterMs || g_cfg.netSimLossPct) {
        g_net.setNetSim(g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: NET SIM on - delay=%ums jitter=+/-%ums loss=%u%%",
                  g_cfg.netSimDelayMs, g_cfg.netSimJitterMs, g_cfg.netSimLossPct);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Steam reachability spike (channel 1): independent of the transport, so a
    // UDP session can still prove Steam P2P punch-vs-relay against a peer.
    if (g_cfg.steamPing != 0) {
        if (coop::steamp2p::init()) coop::steamp2p::setPingPeer(g_cfg.steamPing);
        else coopErr("[steam] KENSHICOOP_STEAM_PING set but Steam init failed");
    }

    // Steam P2P transport: connect by SteamID (NAT punch + Valve relay) with the
    // ENet protocol unchanged. Requires the partner's steamid64; falls back to
    // UDP loudly when Steam is unavailable so a misconfigured session still
    // behaves like the stock build instead of silently doing nothing.
    if (g_cfg.transport == "steam") {
        if (g_cfg.steamPeer == 0) {
            coopErr("[steam] KENSHICOOP_TRANSPORT=steam requires KENSHICOOP_STEAM_PEER=<partner steamid64>; falling back to UDP");
        } else if (!coop::steamp2p::init()) {
            coopErr("[steam] init failed (Steam not running / offline?); falling back to UDP");
        } else {
            coop::steamp2p::setPeer(g_cfg.steamPeer);
            g_net.setSteamTransport(g_cfg.steamPeer);
            coopLog("[steam] transport=steam armed (connect by SteamID; no port forwarding)");
        }
    }

    bool ok;
    if (g_cfg.isHost) {
        coopLog("KenshiCoop: starting as HOST");
        ok = g_net.startHost(g_cfg.port, &g_inbound);
    } else {
        coopLog("KenshiCoop: starting as CLIENT");
        ok = g_net.startClient(g_cfg.ip, g_cfg.port, &g_inbound);
    }
    if (!ok) coopErr("KenshiCoop: networking failed to start");
}

// In-game panel handlers. coopUiConnect tears down any live session, re-arms the
// config from the panel's choices, and restarts via the shared startNetworking()
// path (NetLink cleanly supports stop() then start again; Steam is re-armed and
// the Replicator/Inbound session state is reset for a clean handshake).
void coopUiConnect(bool isHost, bool useSteam, unsigned long long peerId) {
    if (g_net.isRunning()) g_net.stop();
    coop::steamp2p::shutdown();
    g_peerPresent = false;
    // World is live here (reconnect from within a running game): despawn minted
    // NPC + world-item proxies before clearing the maps so a re-connect doesn't
    // leave orphaned duplicates (and doesn't bake them into a save). Falls back
    // to a plain map reset if no world has ticked yet.
    if (g_lastGw) g_repl.clearPeerReplicationState(g_lastGw);
    else          g_repl.resetSession();
    g_inbound.flushWorldState();

    g_cfg.isHost    = isHost;
    g_cfg.transport = useSteam ? "steam" : "udp";
    // Re-read the UDP endpoint (ip/port) from coop_config.json so editing it then
    // hitting Connect works without restarting the game. (It also picks up a
    // steamPeer if one is set for advanced/back-compat use.)
    coop::reloadPeerFromFile(g_cfg);
    // A Steam ID pasted in the F2 panel this session wins over the config: the
    // normal flow is Copy my Steam ID -> friend Pastes it -> Connect, with no file
    // editing. peerId is 0 when nothing was pasted, so the config value stands.
    if (peerId != 0) g_cfg.steamPeer = peerId;
    g_repl.setStreamNpcs(isHost);            // host streams world NPCs; join drives
    // Ownership ranks must follow the role chosen in the panel. Only an explicit
    // KENSHICOOP_OWN_SQUAD override is preserved; otherwise recompute the default
    // (host owns {0}, join owns {1}). Without this, a session launched as HOST
    // that switches to JOIN keeps rank {0} and wrongly claims the host's player
    // squad, so that unit never moves on the client (unowned NPCs still sync).
    coop::resolveOwnRanks(g_cfg.ownRanks, isHost, g_cfg.ownRanksFromEnv);
    g_repl.setOwnRanks(g_cfg.ownRanks);

    std::string ranks;
    for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
         it != g_cfg.ownRanks.end(); ++it) {
        char n[16]; _snprintf(n, sizeof(n) - 1, "%u", *it); n[sizeof(n) - 1] = '\0';
        if (!ranks.empty()) ranks += ",";
        ranks += n;
    }
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[coop-ui] connect: role=%s transport=%s peer=%llu ownRanks={%s} src=%s",
              isHost ? "HOST" : "JOIN", g_cfg.transport.c_str(),
              (unsigned long long)g_cfg.steamPeer, ranks.c_str(),
              g_cfg.ownRanksFromEnv ? "env" : "role");
    b[sizeof(b) - 1] = '\0';
    coopLog(b);
    startNetworking();
}

void coopUiDisconnect() {
    coopLog("[coop-ui] disconnect");
    if (g_net.isRunning()) g_net.stop();
    coop::steaminvite::reset(); // leave any Steam lobby
    coop::steamp2p::shutdown();
    g_peerPresent = false;
    // World stays live on a manual disconnect: despawn minted NPC + world-item
    // proxies before clearing maps so nothing lingers as a duplicate or gets
    // baked into the next save.
    if (g_lastGw) g_repl.clearPeerReplicationState(g_lastGw);
    else          g_repl.resetSession();
    g_inbound.flushWorldState();
}

} // namespace

// RE_Kenshi resolves the entry by its C++-mangled name (?startPlugin@@YAXXZ), so
// this must NOT be extern "C".
__declspec(dllexport) void startPlugin() {
    coop::loadConfig(g_cfg);
    // The fake clock skew must be armed BEFORE the first log line so every
    // timestamp in this run (and every time-sync packet) shares the skewed clock.
    coop::logSetFakeSkewMs(g_cfg.fakeClockSkewMs);
    coop::logInit(g_cfg.logPath.c_str(), g_cfg.isHost ? "HOST" : "JOIN");

    coopLog("KenshiCoop loaded! (clean rebuild)");
    if (g_cfg.fakeClockSkewMs != 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: FAKE clock skew injected: %+ld ms",
                  g_cfg.fakeClockSkewMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    // Build stamp: changes every compile, so the test runner can confirm a fresh
    // DLL is actually deployed (anti-stale guard) rather than an old cached copy.
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "KenshiCoop: build %s %s", __DATE__, __TIME__);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
    {
        char b[128];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: role=%s proto=v%u port=%d save='%s'",
                  g_cfg.isHost ? "HOST" : "JOIN", (unsigned)coop::PROTOCOL_VERSION,
                  g_cfg.port, g_cfg.save.c_str());
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Hook the main-thread tick. GameWorld exposes both the virtual and the
    // non-virtual _NV_mainLoop_GPUSensitiveStuff (same RVA); GetRealAddress only
    // works on the _NV_ variant.
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &g_mainLoop_orig)) {
        coopErr("KenshiCoop: could not install main-loop hook!");
        return;
    }

    coop::engine::resolve();

    // Stage 4: the host streams nearby world NPCs (host-authoritative) in addition
    // to its squad; the join resolves each by hand and drives it like a squad body.
    if (g_cfg.isHost) g_repl.setStreamNpcs(true);

    // Bidirectional presence: this client owns (controls + streams) a disjoint set of
    // the shared squad chosen by save-stable hand-rank; it drives the peer's owned
    // members from their stream. Default leader-first: host {0}, join {1}.
    g_repl.setOwnRanks(g_cfg.ownRanks);
    {
        std::string ranks;
        for (std::set<unsigned int>::const_iterator it = g_cfg.ownRanks.begin();
             it != g_cfg.ownRanks.end(); ++it) {
            char num[16]; _snprintf(num, sizeof(num) - 1, "%u", *it); num[sizeof(num)-1] = '\0';
            if (!ranks.empty()) ranks += ",";
            ranks += num;
        }
        std::string m = "KenshiCoop: ownership ranks = {" + ranks + "} (bidirectional presence)";
        coopLog(m.c_str());
    }

    // Protocol 36 movement-smoothness knobs (KENSHICOOP_INTERP_* / _CATCHUP_K /
    // _SNAP_DIST): live-tune the interp window and the walk-drive gains for WAN
    // A/B runs. Defaults reproduce the historical constants exactly.
    {
        coop::InterpConfig ic;
        ic.minDelayMs  = g_cfg.interpMinDelayMs;
        ic.maxDelayMs  = g_cfg.interpMaxDelayMs;
        ic.maxExtrapMs = g_cfg.interpMaxExtrapMs;
        ic.staleMs     = g_cfg.interpStaleMs;
        ic.snapDistSq  = g_cfg.interpSnapDist * g_cfg.interpSnapDist;
        g_repl.setInterpConfig(ic);
        g_repl.setDriveTuning(g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds);
        g_repl.setCombatTuning(g_cfg.combatSoftDist, g_cfg.combatSnapDist,
                               g_cfg.combatBigSnapDist, g_cfg.combatSlideMax,
                               g_cfg.combatConvergeMs);
        g_repl.setSendStamp(g_cfg.sendStamp);
        g_repl.setCensusRadius(g_cfg.censusRadius);
        g_repl.setSpawnMintRadius(g_cfg.spawnMintRadius);
        g_repl.setCensusParkDist(g_cfg.censusParkDist);
        g_repl.setCensusFreezeAi(g_cfg.censusFreezeAi);
        g_repl.setStarveHold(g_cfg.starveHoldMs);
        // Camera-anchored interest (protocol 43): engine-side master enable
        // for the camera anchors in interestCenters.
        coop::engine::setCamInterest(g_cfg.camInterest);
        // travel_parity needs the 5 s SCENARIO WORLD/WNPC worldstate rows on
        // both sides; npc_sync feeds the same rows to the anti-zombie oracle
        // (Phase 2 mid-band tier: a populated town run has moving census-band
        // NPCs, which the hop corridor mostly lacks). Every other scenario
        // stays quiet (log volume).
        g_repl.setAuditRows(g_cfg.scenario == "travel_parity" ||
                            g_cfg.scenario == "npc_sync" ||
                            g_cfg.scenario == "world_parity" ||
                            g_cfg.scenario == "jail_probe");
        char b[260];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: interp delay=%u-%ums extrap=%ums stale=%ums snap=%.0fu "
                  "drive catchupK=%.2f snapDist=%.1fu snapSec=%.2f sendStamp=%d "
                  "census=%.0fu mint=%.0fu park=%.0fu starveHold=%ums",
                  g_cfg.interpMinDelayMs, g_cfg.interpMaxDelayMs,
                  g_cfg.interpMaxExtrapMs, g_cfg.interpStaleMs, g_cfg.interpSnapDist,
                  g_cfg.catchupK, g_cfg.snapDist, g_cfg.snapSeconds,
                  g_cfg.sendStamp ? 1 : 0,
                  g_cfg.censusRadius, g_cfg.spawnMintRadius, g_cfg.censusParkDist,
                  g_cfg.starveHoldMs);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state. KENSHICOOP_CARRY_SYNC=0 is the A/B escape hatch.
    g_repl.setCarrySync(g_cfg.carrySync);

    // Furniture occupancy sync (protocol 19, default ON): reliable bed/cage
    // enter/exit edges + self-healing occupancy state. KENSHICOOP_FURN_SYNC=0
    // is the A/B escape hatch.
    g_repl.setFurnSync(g_cfg.furnSync);
    g_repl.setChainSync(g_cfg.chainSync);
    g_repl.setStealthSync(g_cfg.stealthSync);

    // Per-tab wallet sync (protocol 22, default ON): owner-authoritative money
    // per squad tab. KENSHICOOP_MONEY_SYNC=0 is the A/B escape hatch (and the
    // shop_probe baseline).
    g_repl.setMoneySync(g_cfg.moneySync);

    // Runtime-spawn proxy replication (protocol 21, default ON): the join mints
    // local proxy bodies for host runtime spawns it cannot resolve by hand.
    // KENSHICOOP_SPAWN_SYNC=0 is the A/B escape hatch (spawn_probe forces off).
    g_repl.setSpawnSync(g_cfg.spawnSync);

    // AI-gating probe (join side): recruit diverged NPCs to test the inhabit lever.
    if (!g_cfg.isHost && g_cfg.probeRecruit) g_repl.setProbeRecruit(true);

    // AI-suspend (BOTH roles, DEFAULT ON): detour Character::periodicUpdate so
    // any body a client DRIVES from the peer's stream stops self-tasking (decision
    // layer off) while still animating. Faction is untouched - we hold the body's
    // current action instead of letting the AI re-decide and wander/thrash. This
    // is the universal QUIETING layer (doctrine 15 amendment); per-class APPLY
    // levers sit on top. KENSHICOOP_AI_SUSPEND=0 disables (A/B escape hatch).
    //
    // Phase 1b: enabled on the HOST too (was join-only). The host drives nothing
    // in the classic host-authoritative single-direction case (it owns every
    // NPC), so this is a no-op there. But once a recruit is TRANSFERRED into the
    // join's tab, control flips to the join and the HOST must now drive that
    // peer-owned squad member - without suspend it self-follows the host's leader
    // and shows the same slow-walk/snap artifact the join had. applyTargets only
    // ever suspends bodies we drive (peer-owned), so a host-owned NPC/leader is
    // never quieted by this.
    if (g_cfg.aiSuspend) {
        if (coop::engine::installAiSuspendHook()) {
            g_repl.setAiSuspend(true);
            coopLog("[ai] periodicUpdate detour installed; AI-suspend ON (default)");
        } else {
            coopLog("[ai] FAILED to install periodicUpdate detour; AI-suspend disabled");
        }
    }
    // Task-selection observation spike (KENSHICOOP_TASK_SPIKE, off by default):
    // passive detour on CharBody::setCurrentAction to prove the selection seam is
    // hookable in isolation and log the chosen task tuple per body. No behavior
    // change - a diagnostic for the "stream selection, not motion" direction.
    if (g_cfg.taskSelectSpike) {
        if (coop::engine::installTaskSelectSpikeHook()) {
            coop::engine::setTaskSelectSpike(true);
            coopLog("[spike] setCurrentAction detour installed; task-select observation ON (KENSHICOOP_TASK_SPIKE)");
        } else {
            coopLog("[spike] FAILED to install setCurrentAction detour (seam unresolved)");
        }
    }
    // Jail put-to-work desync spike: correlated [jail] STATE traces (read-only).
    g_repl.setJailProbe(g_cfg.jailProbe);
    if (g_cfg.jailProbe)
        coopLog("[jail] KENSHICOOP_JAIL_PROBE=1: captive-state [jail] STATE tracing ON");
    // Jail put-to-work observation spike (Phase A): host runs the captive
    // unopposed and logs its trajectory ([jail] OBSERVE) to classify intent.
    g_repl.setJailObserve(g_cfg.jailObserve);
    if (g_cfg.jailObserve)
        coopLog("[jail] KENSHICOOP_JAIL_OBSERVE=1: captive drive/suspend/self-heal OFF; [jail] OBSERVE tracing ON");
    // Step-2 experiment: sitter no-detach A/B (manual runs only; off by default).
    if (!g_cfg.isHost && g_cfg.noDetach) {
        g_repl.setNoDetach(true);
        coopLog("[quiet] KENSHICOOP_NO_DETACH=1: sitter detachFromTownAI SKIPPED (experiment)");
    }

    // Divergence-gated authority (join side, DEFAULT ON since the step-4 A/B):
    // trust world NPCs whose local AI sustainedly agrees with the host; drive
    // only divergence. KENSHICOOP_GATE_AUTHORITY=0 disables (A/B escape hatch).
    if (!g_cfg.isHost && g_cfg.gateAuthority) {
        g_repl.setGateAuthority(true);
        coopLog("[trust] divergence-gated authority ON (default; KENSHICOOP_GATE_AUTHORITY=0 disables)");
    }

    // Damage guard (BOTH sides, DEFAULT ON): locally-simulated melee hits on
    // driven bodies are suppressed (HIT_MISSED) so cosmetic fights cannot diverge
    // the local-only medical model. The guard set is "every body this client
    // drives": the peer's world-NPC copies on the join, the peer's SQUAD-member
    // copies on the host (host-side extension 2026-07-06 - phase-1 player_combat
    // measured the host's copy of a join victim bleeding 40+ blood while the
    // join's guarded copies stayed at 0). KENSHICOOP_DAMAGE_GUARD=0 disables.
    if (g_cfg.damageGuard) {
        if (coop::engine::installDamageGuardHook()) {
            g_repl.setDamageGuard(true);
            coopLog(g_cfg.isHost
                ? "[dmg] hitByMeleeAttack detour installed; damage guard ON (host, driven peer-squad bodies)"
                : "[dmg] hitByMeleeAttack detour installed; damage guard ON (default)");
        } else {
            coopLog("[dmg] FAILED to install hitByMeleeAttack detour; damage guard disabled");
        }
    }

    // Purchase observability (protocol 22, 1c groundwork): log every real
    // trade-UI purchase ("[shop] BUY-LOCAL"), the field evidence for the
    // vendor-stock mirror design. Rides the money-sync gate.
    if (g_cfg.moneySync) {
        if (coop::engine::installShopHook())
            coopLog("[shop] buyItem detour installed; purchase logging ON");
        else
            coopLog("[shop] FAILED to install buyItem detour; purchase logging off");
    }

    // Cross-owner trade veto (KENSHICOOP_BLOCK_XFER, default ON in real sessions):
    // refuse a UI inventory drag whose source + destination squad characters are
    // owned by different clients (item stays in the source bag) so ground drops
    // are the only cross-client transfer path. Retires Protocol 37 (Config forces
    // xferSync off when this is on). The classifier is always registered (cheap);
    // the detours install only when the veto is on or the xfer_block test runs.
    coop::engine::setInvOwnerClassifier(&coopInvOwnerClass);
    coop::engine::setBlockXfer(g_cfg.blockXfer);
    if (g_cfg.blockXfer || g_cfg.scenario == "xfer_block") {
        if (coop::engine::installXferBlockHook())
            coopLog(g_cfg.blockXfer
                ? "[xfer] drag detours installed; cross-owner trade veto ON (drop to transfer)"
                : "[xfer] drag detours installed; drag logging ON");
        else
            coopLog("[xfer] FAILED to install drag detours; cross-owner trade veto degraded");
    }

    // Phase W1b: query-free ground-drop capture (town reliability). Detour
    // Inventory::dropItem so every drop is discovered without the spatial sphere
    // query that fails in towns; publishWorldItems seeds its tracker from these
    // edges and culls by handle liveness. Rides the world-sync gate (the channel
    // it feeds), so real sessions and the world_item_* / drop scenarios get it.
    if (g_cfg.worldSync) {
        if (coop::engine::installItemDropHook())
            coopLog("[wi] dropItem detour installed; query-free drop capture ON");
        else
            coopLog("[wi] FAILED to install dropItem detour; drop capture falls back to the spatial scan");
    }

    // Recruitment sync (protocol 23, default ON): detour PlayerInterface::
    // recruit so every successful local recruit (dialog or programmatic)
    // authors a reliable EVT_RECRUIT; the peer re-keys its local copy of the
    // recruited body to the new stream key. KENSHICOOP_RECRUIT_SYNC=0 is the
    // A/B escape hatch (recruit_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setRecruitSync(g_cfg.recruitSync);
    // Squad management sync (protocol 35, default ON): pointer-diff move
    // edges -> EVT_SQUAD_MOVE re-keys + the container-rank latch (mid-session
    // tabs append instead of reshuffling ownership). KENSHICOOP_SQUAD_SYNC=0
    // is the A/B escape hatch (squad_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setSquadSync(g_cfg.squadSync);
    g_repl.setFactionSync(g_cfg.factionSync);
    g_repl.setTimeSync(g_cfg.timeSync);
    g_repl.setDoorSync(g_cfg.doorSync);
    g_repl.setBuildSync(g_cfg.buildSync);
    g_repl.setBdoorSync(g_cfg.bdoorSync);
    g_repl.setHungerSync(g_cfg.hungerSync);
    g_repl.setProdSync(g_cfg.prodSync);
    g_repl.setResearchSync(g_cfg.researchSync);
    // Protocol 34: the HOST authors every storage/machine container near the
    // interest centers (the ~1 Hz census inside publishInventories); the join
    // reconciles via the translated key. Host-only flag - the join must never
    // census-author (host-authoritative world containers). Layered on invSync
    // (publishInventories is the carrier and is gated on it below).
    g_repl.setStoreSync(g_cfg.storeSync && g_cfg.isHost);
    if (g_cfg.recruitSync) {
        if (coop::engine::installRecruitHook())
            coopLog("[recruit] recruit detour installed; recruitment sync ON");
        else
            coopLog("[recruit] FAILED to install recruit detour; recruitment sync degraded (no local-edge detection)");
    }

    // Build-placement observability (protocol 27): detour PreviewBuilding::
    // placeFinalPreviewBuilding so every REAL build-mode commit logs its
    // template + transform + runtime hand ("[build] LOCAL-PLACE") and queues
    // an edge for the sync layer. Installed for the sync AND for build_probe
    // (which measures the unsynced baseline but wants the UI-edge evidence).
    if (g_cfg.buildSync || g_cfg.scenario == "build_probe") {
        if (coop::engine::installBuildHook())
            coopLog("[build] placeFinalPreviewBuilding detour installed; placement logging ON");
        else
            coopLog("[build] FAILED to install placement detour; placement logging degraded");
    }

    // Dismantle observability (protocol 28): detour Building::
    // notifyConstructionDismantling so every UI dismantle logs its hand
    // ("[build] LOCAL-DISMANTLE") and queues a removal edge for the sync
    // layer. Installed for the sync AND for bdoor_probe (unsynced baseline,
    // but the edge evidence is the finding).
    if (g_cfg.bdoorSync || g_cfg.scenario == "bdoor_probe") {
        if (coop::engine::installDismantleHook())
            coopLog("[build] dismantle detour installed; removal logging ON");
        else
            coopLog("[build] FAILED to install dismantle detour; removal logging degraded");
    }

    // Faction-relation observability (protocol 24): detour both affectRelations
    // overloads so every REAL relation mutation logs its cause ("[fac] AFFECT")
    // and records a delta for the sync layer. Installed for the sync AND for
    // faction_probe (which measures the unsynced baseline but needs the
    // cause-attribution evidence).
    if (g_cfg.factionSync || g_cfg.scenario == "faction_probe") {
        if (coop::engine::installFactionHooks())
            coopLog("[fac] affectRelations detours installed; relation logging ON");
        else
            coopLog("[fac] FAILED to install affectRelations detours; relation logging degraded");
    }

    // Coordinated save (protocol 31): detour SaveManager::save so every local
    // save (menu, quicksave, autosave timer, programmatic bake) logs its
    // "[save] LOCAL-SAVE" edge and queues a SaveEdge - the trigger for the
    // host-authoritative save transfer (and the join's REQ forwarding).
    // save_probe installs it too but measures the raw behaviour alone
    // (saveSync forced off - no coordination on top).
    if (g_cfg.saveSync || g_cfg.scenario == "save_probe") {
        if (coop::engine::installSaveHook())
            coopLog(g_cfg.saveSync
                ? "[save] save detour installed; coordinated save ON"
                : "[save] save detour installed; save-edge logging ON");
        else
            coopLog("[save] FAILED to install save detour; coordinated save degraded");
    }

    // Coordinated load (protocol 32): detour SaveManager::load so every local
    // load (menu, title screen, programmatic) logs its "[load] LOCAL-LOAD"
    // edge and queues a LoadEdge - the trigger for the host-authoritative
    // load broadcast (and the join's REQ forwarding / suppression).
    // load_probe installs it too but measures the raw world swap alone
    // (loadSync forced off - no coordination on top).
    if (g_cfg.loadSync || g_cfg.scenario == "load_probe") {
        if (coop::engine::installLoadHook())
            coopLog(g_cfg.loadSync
                ? "[load] load detour installed; coordinated load ON"
                : "[load] load detour installed; load-edge logging ON");
        else
            coopLog("[load] FAILED to install load detour; coordinated load degraded");
    }

    // Title-screen hook. It drives THREE things at the main menu: the config
    // auto-load (only when a save is set), the F2 co-op panel (go ONLINE / paste
    // a Steam ID before loading), and the push-save-on-connect bootstrap (a join
    // with NO save receives + loads the host's world). So install it whenever a
    // save is configured OR this is an interactive session - NOT only when a save
    // is present, which was the old behavior that left a join-from-menu with no
    // title hook (the panel and bootstrap never ran). The scenario/test harness
    // always sets a save, so its behavior is unchanged. titleUpdate_hook itself
    // self-gates each of the three concerns.
    bool interactive = g_cfg.scenario.empty() && g_cfg.testSeconds == 0;
    if (!g_cfg.save.empty() || interactive) {
        if (KenshiLib::SUCCESS !=
            KenshiLib::AddHook(
                KenshiLib::GetRealAddress(&TitleScreen::_NV_update),
                &titleUpdate_hook, &g_titleUpdate_orig)) {
            coopErr("KenshiCoop: could not install title-screen hook (F2 panel + bootstrap + auto-load disabled)");
        } else if (!g_cfg.save.empty()) {
            std::string m = "KenshiCoop: title hook armed (auto-load save '" + g_cfg.save + "')";
            coopLog(m.c_str());
        } else {
            coopLog("KenshiCoop: title hook armed (F2 panel + push-on-connect at menu; no auto-load save)");
        }
    }

    if (g_cfg.testSeconds > 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: test mode - self-exit %d s after gameplay starts",
                  g_cfg.testSeconds);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Scenario harness: build the selected scenario (both host & join run it; it
    // branches on host/join internally). Unknown names are logged and ignored.
    if (!g_cfg.scenario.empty()) {
        g_scenario = coop::makeScenario(g_cfg.scenario);
        if (g_scenario) {
            std::string m = "KenshiCoop: scenario armed '" + g_cfg.scenario + "'";
            coopLog(m.c_str());
        } else {
            std::string m = "KenshiCoop: unknown scenario '" + g_cfg.scenario + "'";
            coopErr(m.c_str());
        }
    }

    // Session start policy (in-game panel, 2026-07-14): the unattended harness
    // (a scenario or a self-exit timer) ALWAYS auto-starts - it never touches the
    // F2 panel. An interactive install DEFERS the session to the panel's Connect
    // unless KENSHICOOP_AUTOCONNECT=1 restores load-time auto-start from the
    // env/config role+transport+peer (the legacy launcher behaviour).
    bool autoStart = g_cfg.autoConnect || !g_cfg.scenario.empty() || g_cfg.testSeconds > 0;

    // Bring Steam up now (when the panel may be used, or Steam is configured) so
    // the panel can show THIS player's SteamID for the two-code exchange before
    // any connection is made. init() is idempotent; startNetworking re-uses it.
    // When Steam is up, also arm the invite layer so this player can RECEIVE a
    // Steam "Join Game"/invite (GameLobbyJoinRequested) even before touching the
    // panel - the joiner never has to open it or type an ID.
    if (!autoStart || g_cfg.transport == "steam" || g_cfg.steamPing != 0) {
        if (coop::steamp2p::init())
            coop::steaminvite::init(&coopUiConnect);
    }

    if (autoStart) {
        startNetworking();
    } else {
        coopLog("KenshiCoop: session DEFERRED - press F2 in-game to pick role/transport, "
                "enter the friend's Steam ID, and check CONNECTED");
    }
}
