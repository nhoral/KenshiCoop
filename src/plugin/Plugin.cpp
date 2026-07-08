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
#include "core/Inbound.h"
#include "net/NetLink.h"
#include "net/SteamP2P.h"
#include "game/Engine.h"
#include "sync/Replicator.h"
#include "test/Scenario.h"

namespace {

coop::Config     g_cfg;
coop::NetLink    g_net;
coop::Inbound    g_inbound;
coop::Replicator g_repl;
coop::u32        g_tick = 0;

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

// Original function pointers, filled by KenshiLib::AddHook.
void (*g_mainLoop_orig)(GameWorld*, float) = 0;
void (*g_titleUpdate_orig)(TitleScreen*)   = 0;

// Log to BOTH our dedicated per-line-flushed file (what the test runner reads)
// and the engine's kenshi.log (handy when attached live).
void coopLog(const char* msg) { coop::logLine(msg);    DebugLog(msg); }
void coopErr(const char* msg) { coop::logErrLine(msg); ErrorLog(msg); }

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
    }
}

// Main-thread tick hook: the one safe point where we touch game state.
void mainLoop_hook(GameWorld* gw, float dt) {
    ++g_tick;

    if (!g_gameStarted && coop::engine::gameplayLive(gw)) {
        g_gameStarted   = true;
        g_gameStartTick = GetTickCount();
        coopLog("KenshiCoop: gameplay started");
        // Speed-intent capture (vote/effective decoupling): detour the engine's
        // speed setters so every USER action (button, keyboard pause, simulated
        // click) registers as a vote, while our own quiet applies stay invisible.
        // Installed at gameplay start so the seed reads the save's live speed.
        if (coop::engine::installSpeedIntentHooks(gw))
            coopLog("[speed] intent hooks installed (setGameSpeed/userPause/togglePause)");
        else
            coopLog("[speed] FAILED to install intent hooks (vote capture degraded)");
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

    // --- Replication: BIDIRECTIONAL presence. Both clients stream their OWNED squad
    // subset (partitioned by hand-rank) and drive the peer's; the host additionally
    // streams world NPCs. Ingest received targets BEFORE the engine tick so apply
    // targets are current.
    g_repl.ingest(g_inbound);
    // Phase 4a: drain received container-contents snapshots into the per-container
    // cache (reconciled after the engine tick by applyInventories).
    g_repl.ingestInv(g_inbound);
    // Both clients latch reliable transition events (KO/death/revive) for the bodies
    // they drive, before apply (a side can emit an event for its own owned body and
    // the peer that drives that body must honour it).
    g_repl.applyEvents(gw, g_inbound);
    if (g_gameStarted) {
        g_repl.publishOwned(gw, g_net, g_net.localId());
        // Both clients stream the contents of every squad member they OWN (host tab 0,
        // join tab 1) on content-change - bidirectional, disjoint by the same tab
        // partition as positional sync. Gated on invSync so ordinary co-op sessions add
        // no inventory traffic; the peer reconciles via applyInventories (skips own).
        if (g_cfg.invSync)
            g_repl.publishInventories(gw, g_net, g_net.localId());
        // Phase W1: the HOST streams free ground items in the interest sphere (host-
        // authoritative world). The join never publishes world items (it sends intents
        // later, W2); it reconciles proxies after the engine tick (applyWorldItems).
        if (g_cfg.worldSync && g_cfg.isHost)
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
        // Runtime-spawn proxy replication (protocol 21): the join asks about
        // streamed hands it couldn't resolve last tick (host RUNTIME spawns)
        // and mints local proxy bodies from the host's replies; the host
        // answers requests. BEFORE the engine tick so a proxy bound this
        // frame is driven by this frame's applyTargets.
        if (g_cfg.spawnSync)
            g_repl.syncSpawns(gw, g_inbound, g_net, g_net.localId(), g_cfg.isHost);
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
    if (g_gameStarted) {
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
        // Phase 4a: reconcile any peer-owned container we received a fresh snapshot
        // for (the join applies the host's container; the host skips its own).
        g_repl.applyInventories(gw);
        // Phase W1: the JOIN spawns/updates/culls local proxies for the host's streamed
        // ground items (the host authors them, so it skips this).
        if (g_cfg.worldSync && !g_cfg.isHost)
            g_repl.applyWorldItems(gw, g_inbound);
        // Host-authoritative world: only the JOIN hides/freezes any local NPC the
        // host isn't streaming (so the join can't run a divergent copy). The host IS
        // the world authority, so it never suppresses.
        if (!g_cfg.isHost) g_repl.enforceHostAuthority(gw);
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
void titleUpdate_hook(TitleScreen* self) {
    g_titleUpdate_orig(self);

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

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state. KENSHICOOP_CARRY_SYNC=0 is the A/B escape hatch.
    g_repl.setCarrySync(g_cfg.carrySync);

    // Furniture occupancy sync (protocol 19, default ON): reliable bed/cage
    // enter/exit edges + self-healing occupancy state. KENSHICOOP_FURN_SYNC=0
    // is the A/B escape hatch.
    g_repl.setFurnSync(g_cfg.furnSync);
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

    // AI-suspend (join side, DEFAULT ON): detour Character::periodicUpdate so
    // host-driven NPCs stop self-tasking (decision layer off) while still
    // animating. Faction is untouched - we hold the body's current action instead
    // of letting the AI re-decide and wander/thrash. This is the universal
    // QUIETING layer (doctrine 15 amendment); per-class APPLY levers sit on top.
    // KENSHICOOP_AI_SUSPEND=0 disables (A/B escape hatch).
    if (!g_cfg.isHost && g_cfg.aiSuspend) {
        if (coop::engine::installAiSuspendHook()) {
            g_repl.setAiSuspend(true);
            coopLog("[ai] periodicUpdate detour installed; AI-suspend ON (default)");
        } else {
            coopLog("[ai] FAILED to install periodicUpdate detour; AI-suspend disabled");
        }
    }
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

    // Recruitment sync (protocol 23, default ON): detour PlayerInterface::
    // recruit so every successful local recruit (dialog or programmatic)
    // authors a reliable EVT_RECRUIT; the peer re-keys its local copy of the
    // recruited body to the new stream key. KENSHICOOP_RECRUIT_SYNC=0 is the
    // A/B escape hatch (recruit_probe forces it off to keep the unsynced
    // baseline measurable).
    g_repl.setRecruitSync(g_cfg.recruitSync);
    if (g_cfg.recruitSync) {
        if (coop::engine::installRecruitHook())
            coopLog("[recruit] recruit detour installed; recruitment sync ON");
        else
            coopLog("[recruit] FAILED to install recruit detour; recruitment sync degraded (no local-edge detection)");
    }

    // Auto-load: only hook the title screen when a save name was provided.
    if (!g_cfg.save.empty()) {
        if (KenshiLib::SUCCESS !=
            KenshiLib::AddHook(
                KenshiLib::GetRealAddress(&TitleScreen::_NV_update),
                &titleUpdate_hook, &g_titleUpdate_orig)) {
            coopErr("KenshiCoop: could not install title-screen hook (auto-load disabled)");
        } else {
            std::string m = "KenshiCoop: auto-load armed for save '" + g_cfg.save + "'";
            coopLog(m.c_str());
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

    startNetworking();
}
