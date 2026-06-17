// KenshiCoop - RE_Kenshi / KenshiLib plugin entry point.
//
// Milestone 1: load under RE_Kenshi and print to the debug log.
// Milestone 2: hook the main-thread tick and read the local player's position.
// Milestone 4: bridge a background net thread into the tick via MainThreadQueue.
// Milestone 5: send local player state each tick; log received remote states.
//
// Mode is chosen via environment variables read at load time:
//   KENSHICOOP_MODE = "host" (default) | "join"
//   KENSHICOOP_IP   = host IP when joining (default 127.0.0.1)
//   KENSHICOOP_PORT = UDP port (default 27800)

// Some KenshiLib headers (e.g. RootObjectFactory.h) include <boost/thread/...>,
// whose Boost auto-link pragma would force linking a compiled libboost_thread
// we neither ship nor use - we only reference reconstructed class layouts and
// call game functions resolved at runtime. Disable Boost auto-linking entirely.
#define BOOST_ALL_NO_LIB 1
// ...and make Boost.System (pulled in transitively by boost/thread) header-only
// so system_category()/generic_category() are defined inline rather than needing
// a compiled libboost_system.
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

// ---- KenshiLib / Kenshi headers --------------------------------------------
// Verified against KenshiLib (KenshiReclaimer/KenshiLib, branch RE_Kenshi_mods).
// Headers live under <KENSHILIB_DIR>/Include: Debug.h at the root, the hooking
// API under core/, and the reconstructed game structures under kenshi/.
#include <Debug.h>                  // DebugLog(const char*) / ErrorLog
#include <core/Functions.h>         // KenshiLib::AddHook / GetRealAddress / SUCCESS
#include <kenshi/GameWorld.h>       // GameWorld + _NV_mainLoop_GPUSensitiveStuff
#include <kenshi/PlayerInterface.h> // PlayerInterface::playerCharacters (lektor<Character*>)
#include <kenshi/Enums.h>           // MoveSpeed { WALK, RUN }
#include <kenshi/Character.h>       // Character (getPosition/getOrientation/teleport)
#include <kenshi/CharMovement.h>    // CharMovement::_setPositionDirectionAndTeleport/halt
#include <kenshi/CharBody.h>        // CharBody::currentAction (the NPC's active task)
#include <kenshi/Tasker.h>          // Tasker::key() -> TaskType, Tasker::subject (target)
#include <kenshi/RootObject.h>      // RootObjectContainer (Character::container type)
#include <kenshi/RootObjectFactory.h> // RootObjectFactory::createRandomCharacter
#include <kenshi/SaveManager.h>     // SaveManager::getSingleton()/load() - auto-load a save
#include <kenshi/gui/TitleScreen.h> // TitleScreen::_NV_update - safe point to trigger load
#include <kenshi/util/hand.h>       // hand: save-stable composite entity key
#include <kenshi/util/lektor.h>     // lektor<T>: engine vector (getCharactersWithinSphere out)

#include <cstdlib>   // getenv
#include <cstdio>    // _snprintf (VC10 has no C99 snprintf)
#include <cstring>   // memset (build a hand in a scratch buffer)
#include <string>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "../netproto/Protocol.h"
#include "MainThreadQueue.h"
#include "NetClient.h"
#include "CoopLog.h"
#include "Scenario.h"
#include "ScenarioApi.h"

namespace {

coop::NetClient      g_net;
coop::MainThreadQueue g_inbound;
coop::u32            g_tick = 0;

// Original main-loop pointer, filled by KenshiLib::AddHook.
void (*g_mainLoop_orig)(GameWorld*, float) = 0;

// Discovery hook trampoline: Character::runSlaveAnim(const std::string&, float,
// float). Used to learn which animation CLIP NAMES the engine plays for which
// TaskType, so we can replicate poses by clip rather than coaxing the local AI.
void (__fastcall* g_runSlaveAnim_orig)(void*, const std::string*, float, float) = 0;

// Read an env var with a fallback default.
std::string envOr(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string(fallback);
}

// ---- Logging ---------------------------------------------------------------
// Every KenshiCoop log call goes through these wrappers so events land BOTH in
// the dedicated, machine-readable coop log (CoopLog: timestamped, flushed,
// thread-safe) AND in the engine's kenshi.log via KenshiLib's helpers, exactly
// as before. Defined AFTER the global DebugLog->coopLog rename so these are the
// only call sites left referencing the real KenshiLib functions (no recursion).
void coopLog(const char* msg) { coop::logLine(msg);    DebugLog(msg); }
void coopErr(const char* msg) { coop::logErrLine(msg); ErrorLog(msg); }

// ---- Auto-load + test-runner config (read once at plugin load) -------------
std::string g_saveName;        // KENSHICOOP_SAVE: save to auto-load (empty = manual menu)
int         g_testSeconds = 0; // KENSHICOOP_TEST_SECONDS: self-exit after N s of gameplay (0 = off)

// KENSHICOOP_AUTOSPAWN: manual-validation helper (host only, no scenario). After
// gameplay is live, spawn N distinct-hand units into the host's squad ONCE, then
// stop. You drive them around and the join renders/follows them via the squad
// pipeline - exercising the cross-client squad-render path with no save juggling.
int         g_autoSpawnCount = 0;
bool        g_autoSpawnDone  = false;
const DWORD AUTOSPAWN_DELAY_MS = 4000; // let the world + peer settle before spawning

// Ownership partition for the "inhabit" co-op model: both clients load the SAME
// save (so NPC resolve-by-hand still works), but each OWNS a different subset of
// the shared squad. A client locally controls + streams the members it owns, and
// DRIVES the peer's owned members from their stream (the M3 resolve-and-drive
// path) instead of skipping them. Membership is keyed by the engine's STABLE
// Character::squadMemberID (save data, identical cross-client, survives squad
// reordering / recruits) - see weOwnSquadMember. Configured via the test-override
// knob KENSHICOOP_OWN_INDICES (values are squadMemberIDs, not list positions):
//   ""    -> own ALL members (default; legacy single-squad / distinct-save behavior)
//   "0"   -> own only the leader (squadMemberID 0)
//   "~0"  -> own ALL members EXCEPT the leader (e.g. join inhabits the rest)
//   "1,3" -> own squadMemberIDs 1 and 3
bool                   g_ownAll     = true;
bool                   g_ownExcept  = false;  // the index list is an EXCLUSION set
std::set<unsigned int> g_ownIndices;

// Drive authority (Refoundation P1): when true, a driven entity is pulled out of
// the engine's main AI update list ONCE on acquire (removeFromUpdateListMain) and
// then driven purely by streamed transforms + the v4 motion mirror, restored to
// local AI on release/timeout.
//
// EMPIRICAL RESULT (P1 validation on the `sync` save, 2026-06): suppression is
// DEFAULT OFF because removeFromUpdateListMain freezes the movement controller:
// the body still RENDERS, but _setPositionDirectionAndTeleport no longer flushes
// to the live transform (a moving NPC's `actual` position stayed byte-identical
// while its target moved 480m away, sup=1). The teleport-based drive REQUIRES the
// body to remain in the update list so the controller's per-tick step applies the
// write. We therefore keep the proven per-frame approach (quiet the AI via
// clearGoals/neutralize, mirror motion v4, teleport while ticked) as the live
// path, and retain the suppression branch behind KENSHICOOP_SUPPRESS_AI=1 only
// for future experiments (e.g. paired with manualMovement instead of teleport).
bool                   g_suppressAI = false;

// Moving-NPC drive style (fix for "floating"/teleport-slide during movement):
// when true, a moving driven body is WALKED to the host position via the engine's
// own locomotion (setDestination at the host's speed) so it plays a real, grounded
// walk cycle instead of being teleported every frame (which slides a static pose -
// the "float"). A large gap still hard-snaps (teleport) to catch up. Set 0 to fall
// back to the pure teleport-kinematic mover (tight position, but slides). Resting
// NPCs are unaffected (they pose via task + settle once). KENSHICOOP_WALK_DRIVE.
bool                   g_walkDrive  = true;

bool  g_autoLoadDone    = false; // load() already issued from the title screen?
DWORD g_titleFirstTick  = 0;     // when the title screen first updated (GetTickCount)
DWORD g_autoLoadDelayMs = 5000;  // settle this long before auto-loading
                                 // (override via KENSHICOOP_AUTOLOAD_DELAY_MS)
bool  g_gameStarted     = false; // first in-game tick observed (arms self-exit timer)
DWORD g_gameStartTick   = 0;     // when gameplay began (GetTickCount)

// ---- Scenario harness (KENSHICOOP_SCENARIO) --------------------------------
std::string      g_scenarioName;          // selected scenario (empty = none)
coop::Scenario*  g_scenario       = 0;    // live scenario object (host & join)
bool             g_scenarioStarted = false; // onStart() already called?
DWORD            g_scenarioStartTick = 0;  // when the scenario armed (GetTickCount)
unsigned         g_scenarioTick    = 0;    // onTick() counter
DWORD            g_scenarioDoneTick = 0;   // when onTick first returned true (0 = not done)
// After a scenario completes we HOLD the final synced frame on screen instead of
// self-exiting instantly: the observer client used to vanish in the ~1s between
// the host's first SCENARIO MEMBER (the runner's capture anchor) and the actual
// screenshot, so join.png came back blank. Holding keeps both bodies rendered
// and the windows alive long enough for a clean burst capture, then we exit.
const DWORD      SCENARIO_HOLD_MS  = 5000;

// TitleScreen::_NV_update hook trampoline (filled by KenshiLib::AddHook).
void (*g_titleUpdate_orig)(TitleScreen*) = 0;

// SaveManager entry points, resolved at load. getSingleton is a static member
// (plain function pointer); load(const std::string&) is a non-virtual member,
// so `this` is the first arg and the string reference is passed as a pointer.
typedef SaveManager* (__fastcall* SaveMgrGetFn)();
typedef void         (__fastcall* SaveMgrLoadNameFn)(SaveManager* self, const std::string* name);
typedef bool         (__fastcall* SaveMgrSavesExistFn)(SaveManager* self);
SaveMgrGetFn        g_saveMgrGetFn        = 0;
SaveMgrLoadNameFn   g_saveMgrLoadFn       = 0;
SaveMgrSavesExistFn g_saveMgrSavesExistFn = 0;

// ---- Phase 1: ghost characters --------------------------------------------
// Each remote player is represented by a real Kenshi Character ("ghost") that
// we spawn once and teleport to the latest received position every tick.
//
// Spawning/teleporting characters drives deep, version-specific engine code, so
// EVERY such call is wrapped in SEH (__try/__except). A bad call then disables
// the ghost feature and logs, instead of crashing the game - which keeps the
// build safe to iterate on (worst case: no ghost appears). This mirrors how
// Kenshi-Online guards its factory/teleport calls.
//
// All ghost state lives on and is touched only by the MAIN thread.

// Raw, ABI-correct call shapes. On x64 there is a single calling convention;
// `this` is the first argument. By-value Ogre::Vector3 (POD, trivial dtor) is
// passed exactly as the game's own ABI does, and references become pointers.
typedef RootObject* (__fastcall* SpawnFn)(
    RootObjectFactory* self, Faction* faction, Ogre::Vector3 position,
    RootObjectContainer* owner, GameData* characterTemplate, Building* home,
    float age);
// clearAllAIGoals: best-effort, used to quiet the ghost. We position the ghost
// through its CharMovement controller (virtual calls, no resolve needed), since
// that controller owns the authoritative Havok position - writing anywhere else
// gets snapped back each frame, which is what made the puppet "blink".
typedef void (__fastcall* ClearGoalsFn)(Character* self);
// separateIntoMyOwnSquad: spawning borrows the player's container, which enlists
// the ghost in the player's active squad. This pulls it back out into its own
// squad so it is not a controllable member of your party. Return value (a
// Platoon*) is ignored. Non-virtual, resolved by address.
typedef void* (__fastcall* SeparateSquadFn)(Character* self, bool permanent);
// GameWorld::destroy(obj, justUnloaded, debugInfo): the engine's own removal of
// a dynamic RootObject. Used to despawn a ghost when its owner disconnects or
// times out. Overloaded in the header, so we cast to the exact signature.
typedef bool (__fastcall* DestroyObjFn)(
    GameWorld* self, RootObject* obj, bool justUnloaded, const char* debugInfo);
// Phase 2 NPC replication: interest query + AI-tick membership control.
// getCharactersWithinSphere fills 'results' with nearby characters; the
// add/remove update-list calls toggle whether the engine AI-ticks a character.
typedef void (__fastcall* GetCharsInSphereFn)(
    GameWorld* self, lektor<RootObject*>* results, const Ogre::Vector3* pos,
    float farRadius, float nearRadius, float always, int maxFar, int maxNear,
    RootObject* skip);
typedef void (__fastcall* UpdateListFn)(GameWorld* self, Character* c);
// hand::getCharacter resolves a save-stable handle back to the local Character*
// (the client uses this to find its own copy of a host-driven NPC). The 5-arg
// hand constructor builds a proper hand (sets the vptr) from received fields.
typedef Character* (__fastcall* HandGetCharFn)(const hand* self);
typedef hand* (__fastcall* HandCtor5Fn)(
    hand* self, unsigned int index, unsigned int serial, itemType type,
    unsigned int container, unsigned int containerSerial);
// Pose-replication spike: Tasker::key() returns the current TaskType (e.g.
// SIT_AROUND / RELAX_IN_TOWN_PACKAGE for bar-sitting). Non-virtual, resolvable.
typedef int (__fastcall* TaskerKeyFn)(const void* self);
// hand::getRootObject resolves a (subject) handle to its world RootObject - the
// fixture/node an NPC's task targets. CharBody::setCurrentAction(TaskType,target)
// commits the NPC to a task on that object (the reproduction primitive). The
// _NV_ variant has the same RVA as the virtual and is directly callable.
typedef RootObject* (__fastcall* HandGetRootFn)(const hand* self);
typedef bool        (__fastcall* SetActionFn)(void* charBody, int taskType,
                                              RootObject* target);
// Scenario facade: recruit a Character into the player squad (overloaded, so we
// resolve the (Character*, bool) variant), and look up a GameData template by
// its string id (e.g. a character race) from the world's GameDataManager.
typedef bool      (__fastcall* RecruitFn)(PlayerInterface* self, Character* c, bool editor);
typedef GameData* (__fastcall* GetDataFn)(GameDataManager* self, const std::string* sid,
                                          itemType category);

SpawnFn            g_spawnFn        = 0;
ClearGoalsFn       g_clearGoalsFn   = 0;
SeparateSquadFn    g_separateFn     = 0;
DestroyObjFn       g_destroyFn      = 0;
GetCharsInSphereFn g_getCharsFn     = 0;
UpdateListFn       g_removeUpdateFn = 0;
UpdateListFn       g_addUpdateFn    = 0;
HandGetCharFn      g_handGetCharFn  = 0;
HandCtor5Fn        g_handCtorFn     = 0;
TaskerKeyFn        g_taskerKeyFn    = 0;
HandGetRootFn      g_handGetRootFn  = 0;
SetActionFn        g_setActionFn    = 0;
RecruitFn          g_recruitFn      = 0;
GetDataFn          g_getDataFn      = 0;
bool               g_ghostResolved  = false;
bool               g_ghostDisabled  = false;
bool               g_isHostMode     = true; // set in startNetworking
int                g_spawnFailures  = 0;
DWORD              g_lastSpawnExCode = 0;   // SEH code of the last guardedSpawn fault (0 = none)

// Reused across interest queries so the engine grows the buffer once instead of
// allocating every tick (lektor has no destructor of its own).
lektor<RootObject*> g_npcQuery;

// Per-ghost state. We can't force an animation clip directly (AnimationClass is
// private) and a teleport zeroes the engine's perceived speed (no animation), so
// we combine two regimes:
//   MOVING  -> drive the engine's locomotion toward an EXTRAPOLATED point a bit
//              ahead of travel, at RUN, so it animates AND keeps up (instead of
//              trailing the last-known position).
//   STOPPED -> teleport to the EXACT networked position and halt, so the two
//              clients' resting positions match precisely (no drift).
struct GhostState {
    Character*    chr;
    Ogre::Vector3 tgtPos;       // latest received position
    Ogre::Vector3 prevTgtPos;   // previous received position (for velocity)
    Ogre::Vector3 vel;          // per-packet displacement (extrapolation)
    float         tgtHeading;
    Ogre::Vector3 lastDest;     // last destination we actually issued
    DWORD         lastMoveTick; // when the target last moved meaningfully
    DWORD         lastSeenTick; // when we last received any state for this player
    bool          primed;
    bool          destIssued;
    bool          parked;       // currently snapped+halted at rest?
    // Pose replication: the host NPC's current task + the object it targets, plus
    // bookkeeping for re-issuing setCurrentAction on the local copy.
    coop::u16     hostTask;     // latest received TaskType (NPC_TASK_NONE if none)
    coop::u32     hostSubj[5];  // subject hand {type,container,containerSerial,index,serial}
    DWORD         taskTick;     // when we last issued the task (0 = never)
    bool          taskActive;   // last setCurrentAction resolved a target object
    bool          taskBad;      // reproduced task wandered the NPC off; hold instead
    bool          idleSet;      // STAND_STILL idle task already issued at rest
    bool          aiNeutralized;// town AI package stripped (clear goals + own squad)
    bool          suppressed;   // P1: removed from the engine AI update list (drive-authority)
    // Locomotion-animation mirror (Protocol v4): the host's CharMovement state.
    // The engine's AnimationClass picks walk/idle/run from these, so writing them
    // onto the client copy each frame makes its locomotion clip match the host
    // (kills "walk-in-place" without per-frame halt, which froze the phase).
    bool          hostMoving;   // host CharMovement.currentlyMoving
    float         hostSpeed;    // host CharMovement.currentSpeed
    Ogre::Vector3 hostMotion;   // host CharMovement.currentMotion (world-space)
    // Phase 2.5 (M4): true if WE spawned this body as a local stand-in for a
    // peer-owned squad member that has no Character in our save. A proxy must be
    // DESTROYED (not handed back to local AI) when its owner stops streaming.
    bool          isProxy;
    GhostState() : chr(0), tgtHeading(0.0f), lastMoveTick(0), lastSeenTick(0),
                   primed(false), destIssued(false), parked(false),
                   hostTask(coop::NPC_TASK_NONE), taskTick(0), taskActive(false),
                   taskBad(false), idleSet(false), aiNeutralized(false),
                   suppressed(false),
                   hostMoving(false), hostSpeed(0.0f),
                   hostMotion(Ogre::Vector3::ZERO), isProxy(false) {
        hostSubj[0] = hostSubj[1] = hostSubj[2] = hostSubj[3] = hostSubj[4] = 0;
    }
};

// M5: presence-only. A remote player no longer gets a spawned "ghost" body - the
// player's controlled character is squad member 0 and is rendered by the squad
// pipeline (M3/M4) like any other unit, so a separate ghost would double-render
// them. This map is now a lightweight PRESENCE record per remote player (latest
// position + last-seen tick), feeding remotePlayerCount() (scenario arming) and
// publishNpcStates() NPC interest centers. GhostState is reused; chr stays null.
std::map<coop::u32, GhostState> g_ghosts; // remote playerId -> presence record

// ---- Phase 2: replicated NPCs ---------------------------------------------
// The host streams nearby NPC transforms keyed by their save-stable `hand`.
// The client resolves each hand to its own local Character, suppresses that
// NPC's local AI, and drives it with the SAME dual-regime mover used for player
// ghosts. On timeout (host stopped sending) the NPC is handed back to local AI
// rather than destroyed - it is a real world inhabitant, not a spawned puppet.
struct HandKey {
    coop::u32 type, container, containerSerial, index, serial;
    bool operator<(const HandKey& o) const {
        if (type            != o.type)            return type            < o.type;
        if (container        != o.container)        return container        < o.container;
        if (containerSerial != o.containerSerial) return containerSerial < o.containerSerial;
        if (index            != o.index)            return index            < o.index;
        return serial < o.serial;
    }
};

HandKey makeKey(const coop::NpcStateEntry& e) {
    HandKey k;
    k.type            = e.htype;
    k.container        = e.hcontainer;
    k.containerSerial = e.hcontainerSerial;
    k.index            = e.hindex;
    k.serial            = e.hserial;
    return k;
}

std::map<HandKey, GhostState> g_npcs; // hand -> driven NPC state (client side)

// Phase 2.5 (M2) authority de-confliction: hands we have received in a peer's
// PKT_SQUAD_STATE (i.e. owned/simulated by another player), mapped to the last
// tick we saw them. The host excludes these from its NPC interest pick so it
// never re-streams a peer's squad members back as host NPCs (which would make
// that peer fight itself over its own units). Entries auto-expire so a peer that
// leaves stops suppressing those hands.
std::map<HandKey, DWORD> g_remoteOwnedHands;
const DWORD REMOTE_OWN_TTL_MS = 3000;

bool isRemoteOwnedHand(const HandKey& k, DWORD now) {
    std::map<HandKey, DWORD>::iterator it = g_remoteOwnedHands.find(k);
    if (it == g_remoteOwnedHands.end()) return false;
    if (now - it->second > REMOTE_OWN_TTL_MS) return false; // stale -> not owned
    return true;
}

// Phase 2.5 (M4) proxy lifecycle: a peer-owned squad member with no Character in
// our save (e.g. one the peer spawned at runtime) can't be resolved by hand, so
// we spawn a local stand-in ("proxy") to render it. Count consecutive unresolved
// sightings before spawning, so a hand that's merely loading doesn't get a proxy.
std::map<HandKey, int> g_proxyMiss;
const int   PROXY_SPAWN_AFTER_MISSES = 3;
int         g_squadProxyFailures     = 0;
bool        g_squadProxyDisabled     = false;

// Local hands of bodies WE spawned to REPRESENT a remote entity (squad proxies
// and remote-player ghosts). These must never be streamed back to the peer: a
// proxy/ghost is our private rendering of the peer's unit, and if we broadcast it
// the peer would proxy it in turn, each hop minting a new hand - an unbounded
// feedback explosion. Excluded from BOTH publishSquadState and publishNpcStates.
std::set<HandKey> g_syntheticHands;

// Convert a guardedGetHand() {index,serial,type,container,containerSerial} tuple
// into the HandKey used by the driven-entity maps.
HandKey makeKeyFromHand(const coop::u32 h[5]) {
    HandKey k;
    k.index           = h[0];
    k.serial          = h[1];
    k.type            = h[2];
    k.container       = h[3];
    k.containerSerial = h[4];
    return k;
}

// Resolve the non-virtual game functions to their real runtime addresses once.
// GetRealAddress maps KenshiLib's reconstructed symbol to the loaded exe.
void resolveGhostFns() {
    if (g_ghostResolved) return;
    g_ghostResolved = true;

    g_spawnFn = (SpawnFn)KenshiLib::GetRealAddress(
        &RootObjectFactory::createRandomCharacter);

    // Optional: used to quiet the ghost's autonomous behaviour. Non-fatal.
    g_clearGoalsFn = (ClearGoalsFn)KenshiLib::GetRealAddress(
        &Character::clearAllAIGoals);

    // Optional: eject the ghost from the player's squad. Non-fatal.
    g_separateFn = (SeparateSquadFn)KenshiLib::GetRealAddress(
        &Character::separateIntoMyOwnSquad);

    // Optional: despawn a ghost on disconnect/timeout. destroy() is overloaded,
    // so select the (RootObject*, bool, const char*) variant explicitly.
    g_destroyFn = (DestroyObjFn)KenshiLib::GetRealAddress(
        static_cast<bool (GameWorld::*)(RootObject*, bool, const char*)>(
            &GameWorld::destroy));

    // Phase 2 NPC replication. Non-fatal if any fail (feature stays off).
    g_getCharsFn = (GetCharsInSphereFn)KenshiLib::GetRealAddress(
        &GameWorld::getCharactersWithinSphere);
    g_removeUpdateFn = (UpdateListFn)KenshiLib::GetRealAddress(
        &GameWorld::removeFromUpdateListMain);
    g_addUpdateFn = (UpdateListFn)KenshiLib::GetRealAddress(
        &GameWorld::addToUpdateListMain);

    // Resolve hand->Character lookup and the 5-arg hand constructor (overloaded,
    // so disambiguate via an explicit member-pointer cast). Non-fatal.
    g_handGetCharFn = (HandGetCharFn)KenshiLib::GetRealAddress(&hand::getCharacter);
    g_handCtorFn = (HandCtor5Fn)KenshiLib::GetRealAddress(
        static_cast<hand* (hand::*)(unsigned int, unsigned int, itemType,
                                    unsigned int, unsigned int)>(&hand::_CONSTRUCTOR));

    // Pose-replication spike: current-task reader. Non-fatal if unresolved.
    g_taskerKeyFn = (TaskerKeyFn)KenshiLib::GetRealAddress(&Tasker::key);
    g_handGetRootFn = (HandGetRootFn)KenshiLib::GetRealAddress(&hand::getRootObject);
    g_setActionFn = (SetActionFn)KenshiLib::GetRealAddress(
        static_cast<bool (CharBody::*)(TaskType, RootObject*)>(
            &CharBody::_NV_setCurrentAction));

    // Scenario facade. recruit() is overloaded; pick the (Character*, bool)
    // variant. getData() is overloaded; pick the (string, itemType) variant.
    // Both non-fatal: a scenario that needs them just fails its own CHECKs.
    g_recruitFn = (RecruitFn)KenshiLib::GetRealAddress(
        static_cast<bool (PlayerInterface::*)(Character*, bool)>(
            &PlayerInterface::recruit));
    g_getDataFn = (GetDataFn)KenshiLib::GetRealAddress(
        static_cast<GameData* (GameDataManager::*)(const std::string&, itemType)>(
            &GameDataManager::getData));

    if (!g_spawnFn) {
        g_ghostDisabled = true;
        coopErr("KenshiCoop: could not resolve ghost spawn function");
    }

    {
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "KenshiCoop: resolved fns spawn=%p recruit=%p clearGoals=%p "
            "separate=%p getData=%p",
            (void*)g_spawnFn, (void*)g_recruitFn, (void*)g_clearGoalsFn,
            (void*)g_separateFn, (void*)g_getDataFn);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }
}

// SEH-isolated helpers. These contain NO objects requiring C++ unwinding
// (Ogre::Vector3/Quaternion have trivial destructors), so __try is legal here.
Character* guardedSpawn(RootObjectFactory* factory, Faction* faction,
                        const Ogre::Vector3* pos, RootObjectContainer* owner,
                        GameData* tmpl) {
    RootObject* r = 0;
    g_lastSpawnExCode = 0;
    __try {
        r = g_spawnFn(factory, faction, *pos, owner, tmpl, 0, 20.0f);
    } __except (g_lastSpawnExCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return static_cast<Character*>(r);
}

// Drive the puppet through its movement controller so the Havok position, the
// logical position and the rendered transform all move together (no snap-back).
// _setPositionDirectionAndTeleport and halt() are virtual calls on the real
// object, so no GetRealAddress is needed.
bool guardedMovePuppet(Character* ghost, const Ogre::Vector3* pos,
                       const Ogre::Quaternion* rot) {
    __try {
        CharMovement* mv = ghost->movement; // direct member (offset 0x640)
        if (!mv) return false;
        mv->_setPositionDirectionAndTeleport(*pos, *rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read the ghost's live world position (virtual getPosition()). SEH-guarded.
bool guardedGetPos(Character* ghost, Ogre::Vector3* out) {
    __try {
        *out = ghost->getPosition();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read the NPC's locomotion state straight from the reconstructed CharMovement
// members (no symbol resolve needed). This is how we MEASURE "walking in place"
// objectively: a body we are holding at rest whose controller still reports
// currentSpeed>0 / currentlyMoving=true is playing a walk clip without actually
// translating. A still screenshot can't see that; this number can. SEH-guarded.
bool guardedReadMotion(Character* c, bool* moving, float* speed, float* desired) {
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        if (moving)  *moving  = mv->currentlyMoving;
        if (speed)   *speed   = mv->currentSpeed;
        if (desired) *desired = mv->desiredSpeed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Permanently quiet a replicated NPC's autonomous AI, the same way we quiet a
// spawned player ghost: clear its current goals AND eject it from its town/job
// squad so the AI stops re-acquiring a package and re-issuing walk orders.
// This is the crux of the idle fix: a one-shot halt or per-frame force-stop only
// FROZE the body (the local AI kept re-deciding inside the engine's own update,
// so the idle clip never advanced). With the package gone, the NPC behaves like
// a player ghost - it stays put and plays a real, advancing idle animation when
// we settle it once. Idempotent via the caller's aiNeutralized flag. SEH-guarded.
bool guardedNeutralizeNpc(Character* c) {
    if (!c) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        if (g_separateFn)   g_separateFn(c, false); // own squad: drop the package
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Order the ghost to move to an absolute destination at RUN speed. This routes
// through the engine's normal locomotion, so it plays real walk/run animation
// and faces its travel direction. RUN keeps it from lagging behind a sprinting
// remote (the engine still auto-walks when it's close to the destination).
// setDestination / setDesiredSpeed are virtual on CharMovement -> plain calls.
bool guardedSetDestination(Character* ghost, const Ogre::Vector3* dest) {
    __try {
        CharMovement* mv = ghost->movement;
        if (!mv) return false;
        mv->setDesiredSpeed(RUN);
        mv->setDestination(*dest, HIGH_PRIORITY, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Walk the body to an absolute destination at a SPECIFIC speed (matching the
// host's currentSpeed) through the engine's own locomotion. Unlike guardedSetDestination
// (which forces RUN), this paces the copy to the host so it neither overshoots
// (then idles, stutter) nor lags. Routing through setDestination means the engine
// grounds the body and plays a real walk cycle - the fix for the teleport-slide
// "float". Re-issued each frame toward the host's current position; the engine
// acts on it on the next tick. setDestination / setDesiredSpeed are virtual ->
// plain calls. SEH-guarded.
bool guardedWalkTo(Character* ghost, const Ogre::Vector3* dest, float speed) {
    __try {
        CharMovement* mv = ghost->movement;
        if (!mv) return false;
        float s = speed;
        if (s < 1.0f) s = (float)RUN; // host speed unknown/tiny: default to RUN
        mv->setDesiredSpeed(s);
        mv->setDestination(*dest, HIGH_PRIORITY, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Snap the ghost to an exact transform and stop it (used at rest so the two
// clients agree precisely). teleport + halt are virtual -> plain calls.
bool guardedPark(Character* ghost, const Ogre::Vector3* pos,
                 const Ogre::Quaternion* rot) {
    __try {
        CharMovement* mv = ghost->movement;
        if (!mv) return false;
        mv->halt();
        mv->_setPositionDirectionAndTeleport(*pos, *rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Snap to a transform WITHOUT halting. halt() resets the locomotion path AND
// the animation phase, so calling it every frame freezes the body on frame 0 of
// the idle clip (the "frozen NPC" artifact). For held NPCs we settle the body
// once with guardedPark (a clean stop) and thereafter only re-place it with this
// no-halt teleport, so the engine's idle/walk clip keeps ADVANCING.
bool guardedTeleport(Character* ghost, const Ogre::Vector3* pos,
                     const Ogre::Quaternion* rot) {
    __try {
        CharMovement* mv = ghost->movement;
        if (!mv) return false;
        mv->_setPositionDirectionAndTeleport(*pos, *rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Mirror the host's locomotion state onto the local copy's movement controller.
// The engine's AnimationClass selects walk/idle/run from currentlyMoving /
// currentSpeed / currentMotion, so writing the host's values makes the clip
// match. We write these as the LAST thing each frame (after g_mainLoop_orig), so
// they are what the renderer samples - the local AI recomputes them at the START
// of the next tick, but our end-of-frame write wins for the displayed frame.
// Deliberately does NOT call halt() (that would freeze the phase). SEH-guarded.
bool guardedApplyMotion(Character* c, bool moving, float speed,
                        const Ogre::Vector3* motion) {
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->currentlyMoving = moving;
        mv->currentSpeed    = speed;
        mv->desiredSpeed    = speed; // keep accel logic from re-deciding to idle
        if (motion) mv->currentMotion = *motion;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Remove a ghost from the world via the engine's own object destruction. After
// this the Character pointer is dead and must not be touched again.
void guardedDespawn(GameWorld* gw, Character* ghost) {
    if (!gw || !ghost || !g_destroyFn) return;
    __try {
        g_destroyFn(gw, ghost, false, "KenshiCoop ghost");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Scenario facade guarded primitives ------------------------------------
// SEH-isolated leaves used by the ScenarioApi facade. Kept here (with the other
// guarded helpers and the resolved function pointers) so scenarios never touch
// the engine directly. No C++-unwinding objects live in these frames.

// Recruit a spawned character into the player squad. Best-effort.
bool guardedRecruit(PlayerInterface* pl, Character* c) {
    if (!pl || !c || !g_recruitFn) return false;
    __try {
        return g_recruitFn(pl, c, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read a character's STABLE squad-member id (Character::squadMemberID, 0x418).
// This is save data, so it is identical on both clients for the same character
// and - unlike the positional index into playerCharacters - it does NOT shift
// when the squad is reordered or a recruit is added. Used as the ownership-
// partition key (see weOwnSquadMember). SEH-guarded.
bool guardedSquadMemberId(Character* c, int* out) {
    if (!c) return false;
    __try {
        *out = c->squadMemberID;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read a character's save-stable hand into {index,serial,type,container,
// containerSerial}. SEH-guarded.
bool guardedGetHand(Character* c, coop::u32 out[5]) {
    if (!c) return false;
    __try {
        const hand& h = c->handle;
        out[0] = h.index;
        out[1] = h.serial;
        out[2] = (coop::u32)h.type;
        out[3] = h.container;
        out[4] = h.containerSerial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Tag (add) or untag (remove) a body WE spawned - a squad proxy or a remote-
// player ghost - by its LOCAL hand, so our streamers never re-broadcast it.
// See g_syntheticHands. Best-effort: a hand we can't read just won't be tagged.
void markSynthetic(Character* c, bool add) {
    if (!c) return;
    coop::u32 h[5];
    if (!guardedGetHand(c, h)) return;
    HandKey k = makeKeyFromHand(h);
    if (add) g_syntheticHands.insert(k);
    else     g_syntheticHands.erase(k);
}

// Look up a GameData template by string id from the world's GameDataManager.
GameData* guardedLookupTemplate(GameWorld* gw, const std::string* sid) {
    if (!gw || !g_getDataFn || !sid) return 0;
    __try {
        return g_getDataFn(&gw->gamedata, sid, CHARACTER);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Squad member count (reads the player's lektor under SEH). -1 on fault.
int guardedSquadSize(GameWorld* gw) {
    if (!gw || !gw->player) return -1;
    __try {
        return (int)gw->player->playerCharacters.size();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- Phase 2 guarded game calls (no C++ unwinding objects -> SEH legal) -----

// Resolve a received hand (5 fields) to this machine's local Character*, or 0.
// Builds a proper hand in a scratch buffer via the engine constructor (so the
// vptr is valid) then calls getCharacter(). Returns 0 if the NPC isn't loaded
// here or the handle doesn't resolve.
Character* guardedHandToChar(const coop::NpcStateEntry& e) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.hindex, e.hserial, (itemType)e.htype,
                     e.hcontainer, e.hcontainerSerial);
        return g_handGetCharFn(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Pull an NPC out of the engine's main AI update list and clear its goals so it
// stops simulating locally; the host's streamed transforms drive it instead.
bool guardedSuppressNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_removeUpdateFn) return false;
    __try {
        g_removeUpdateFn(gw, c);
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hand an NPC back to the engine's local AI (used when the host stops streaming
// it, so the world keeps living instead of leaving a frozen body).
void guardedRestoreNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_addUpdateFn) return;
    __try {
        g_addUpdateFn(gw, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Clear an NPC's autonomous AI goals so it stops wandering/pathing on its own.
// We keep the NPC IN the engine update list (unlike removeFromUpdateListMain,
// which freezes its movement controller and makes our teleport/setDestination
// silently no-op), and instead quiet it by clearing goals every frame, then
// drive it like a ghost. SEH-guarded.
void guardedClearGoals(Character* c) {
    if (!c || !g_clearGoalsFn) return;
    __try {
        g_clearGoalsFn(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Fill g_npcQuery with characters near 'center'. Reuses one buffer across calls.
bool guardedQueryNpcs(GameWorld* gw, const Ogre::Vector3* center) {
    __try {
        g_npcQuery.clear();
        // farRadius, nearRadius, always, maxFar, maxNear, skip(none).
        // ~200u far radius approximates Kenshi-Online's interest footprint (it
        // syncs a 3x3 grid of 128u zones, ~192u, around each player).
        g_getCharsFn(gw, &g_npcQuery, center, 200.0f, 120.0f, 30.0f, 96, 96, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read one query result into an NpcStateEntry. Returns 1 on success, 0 if the
// object isn't a Character, -1 on fault. All virtual reads are guarded here.
int guardedReadNpc(RootObject* obj, coop::NpcStateEntry* out) {
    __try {
        if (obj->getDataType() != CHARACTER) return 0;
        Character* c = static_cast<Character*>(obj);
        const hand& h = c->handle;
        Ogre::Vector3 p = c->getPosition();
        float heading = c->getOrientation().getYaw().valueRadians();
        out->htype            = (coop::u32)h.type;
        out->hcontainer       = h.container;
        out->hcontainerSerial = h.containerSerial;
        out->hindex           = h.index;
        out->hserial          = h.serial;
        out->x       = p.x;
        out->y       = p.y;
        out->z       = p.z;
        out->heading = heading;
        // Locomotion-animation state: stream the host's movement controller so the
        // client picks the SAME walk/idle/run clip. Defaults to idle if unreadable.
        out->cspeed   = 0.0f;
        out->cmotionX = 0.0f;
        out->cmotionY = 0.0f;
        out->cmotionZ = 0.0f;
        out->cmoving  = 0;
        {
            CharMovement* mv = c->movement;
            if (mv) {
                out->cspeed   = mv->currentSpeed;
                out->cmotionX = mv->currentMotion.x;
                out->cmotionY = mv->currentMotion.y;
                out->cmotionZ = mv->currentMotion.z;
                out->cmoving  = mv->currentlyMoving ? 1 : 0;
            }
        }
        // Pose replication: capture the NPC's current task + its subject object so
        // the client can reproduce the same action at the same fixture. Defaults
        // to "no task" if there's no current action or the reader faults.
        out->task             = coop::NPC_TASK_NONE;
        out->stype            = 0;
        out->scontainer       = 0;
        out->scontainerSerial = 0;
        out->sindex           = 0;
        out->sserial          = 0;
        if (g_taskerKeyFn) {
            CharBody* b = c->body;
            Tasker* t = b ? b->currentAction : 0;
            if (t) {
                out->task             = (coop::u16)g_taskerKeyFn(t);
                const hand& s         = t->subject;
                out->stype            = (coop::u32)s.type;
                out->scontainer       = s.container;
                out->scontainerSerial = s.containerSerial;
                out->sindex           = s.index;
                out->sserial          = s.serial;
            }
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Human-readable name for the rest-related TaskTypes we care about (uses the
// compiler's enum values, so it stays correct regardless of ordinals).
const char* taskName(int t) {
    switch (t) {
        case IDLE:                     return "IDLE";
        case HOLD_POSITION:            return "HOLD_POSITION";
        case STAND_STILL:              return "STAND_STILL";
        case STAND_AT_NODE:            return "STAND_AT_NODE";
        case STAND_AT_SHOPKEEPER_NODE: return "STAND_AT_SHOPKEEPER_NODE";
        case WANDER_TOWN:              return "WANDER_TOWN";
        case OPERATE_MACHINERY:        return "OPERATE_MACHINERY";
        case REST:                     return "REST";
        case SIT_AROUND:               return "SIT_AROUND";
        case RELAX_IN_TOWN_PACKAGE:    return "RELAX_IN_TOWN_PACKAGE";
        case SIT_ON_THRONE:            return "SIT_ON_THRONE";
        case GO_TO_THE_BAR_AND_DRINK:  return "GO_TO_THE_BAR_AND_DRINK";
        case USE_BED:                  return "USE_BED";
        case USE_BED_ORDER:            return "USE_BED_ORDER";
        case SLEEP_ON_FLOOR:           return "SLEEP_ON_FLOOR";
        default:                       return "other";
    }
}

// Pose replication (state-changing): force a Character into 'taskType' targeting
// the object named by the subject hand fields. Resolves the subject to a
// RootObject via hand::getRootObject; ONLY if that fixture actually resolves here
// do we commit setCurrentAction(TaskType, target) so the NPC adopts the matching
// pose (sit/operate) at the same object.
//
// We deliberately do NOT issue the task with a null target: a null-target task
// (e.g. OPERATE_MACHINERY with no specific machine) makes the engine AUTO-PICK a
// nearby machine and WALK the NPC to it - dragging the local copy tens of metres
// off the host position, and seated tasks then ignore our teleport. So an
// unresolved subject returns "not applied" and the caller falls back to a quiet
// idle-park instead. Returns: 2 applied (fixture resolved), 1 not applied
// (subject not loaded here), 0 missing fns/body, -1 fault. SEH-guarded.
int guardedSetTask(Character* c, int taskType, const coop::u32 subj[5]) {
    if (!c || !g_setActionFn || !g_handGetRootFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        // hand ctor order: (index, serial, type, container, containerSerial).
        g_handCtorFn(h, subj[3], subj[4], (itemType)subj[0], subj[1], subj[2]);
        RootObject* target = g_handGetRootFn(h);
        if (!target) return 1; // fixture not loaded here -> caller idle-parks
        CharBody* b = c->body;
        if (!b) return -1;
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        g_setActionFn(b, taskType, target);
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Put an NPC into a self-contained task that needs no target object (e.g.
// STAND_STILL / IDLE). We use this for at-rest NPCs that have no reproducible
// host task: clearing AI goals every frame lets the local AI re-decide, start a
// step, then get frozen mid-stride (the "frozen walk" artifact). Issuing one
// concrete STAND_STILL task instead gives a calm idle and stops the AI churning.
// Clears goals first, then setCurrentAction(taskType, null). SEH-guarded.
int guardedSetIdleTask(Character* c, int taskType) {
    if (!c || !g_setActionFn) return 0;
    __try {
        CharBody* b = c->body;
        if (!b) return -1;
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        g_setActionFn(b, taskType, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Best-effort: rename, clear autonomous AI goals (stops wandering/dialogue),
// and eject from the player's squad. We do NOT halt here - movement is driven
// by setDestination so the ghost walks toward the networked position. The
// std::string lives in the caller's frame (no unwinding object in this __try
// function), keeping SEH legal here.
void guardedQuiet(Character* ghost, const std::string& name) {
    __try {
        ghost->setName(name);            // virtual, relabels the body
        if (g_clearGoalsFn) g_clearGoalsFn(ghost);
        if (g_separateFn) g_separateFn(ghost, false); // leave the player's squad
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// M5: record a remote player's PKT_PLAYER_STATE as a lightweight PRESENCE
// heartbeat - latest position + last-seen tick. We no longer spawn or drive a
// "ghost" body: the player's controlled character arrives via PKT_SQUAD_STATE
// (squad member 0) and is rendered by the squad pipeline (M3/M4), so a separate
// ghost body would double-render them. Presence still backs remotePlayerCount()
// (scenario arming) and publishNpcStates() interest centers. Main thread only.
void receiveRemoteState(GameWorld* gw, const coop::PlayerStatePacket& p) {
    (void)gw;
    if (p.playerId == g_net.localId()) return; // never track ourselves

    Ogre::Vector3 target(p.x, p.y, p.z);
    DWORD now = GetTickCount();

    GhostState& gs = g_ghosts[p.playerId]; // inserts a default record if new
    if (!gs.primed) {
        gs.chr      = 0;        // presence only - never a body
        gs.tgtPos   = target;   // seed so the first delta below is zero
        char buf[128];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: remote player %u present at (%.1f, %.1f, %.1f)",
            p.playerId, p.x, p.y, p.z);
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }

    Ogre::Vector3 step = target - gs.tgtPos;
    const float MOVE_EPS = 0.15f;
    if (step.squaredLength() > MOVE_EPS * MOVE_EPS) {
        gs.vel          = step;
        gs.lastMoveTick = now;
    }
    gs.prevTgtPos   = gs.tgtPos;
    gs.tgtPos       = target;
    gs.tgtHeading   = p.heading;
    gs.lastSeenTick = now;
    gs.primed       = true;
}

// M5: drop a remote player's PRESENCE record (or all, when id == PLAYER_ID_ALL).
// No body to destroy anymore - the player's squad bodies time out and despawn on
// their own through the squad pipeline (updateNpcs).
void despawnGhost(GameWorld* gw, coop::u32 id) {
    (void)gw;
    if (id == coop::PLAYER_ID_ALL) {
        if (!g_ghosts.empty()) coopLog("KenshiCoop: cleared all remote presence");
        g_ghosts.clear();
        return;
    }
    if (g_ghosts.erase(id) > 0) {
        char buf[96];
        _snprintf(buf, sizeof(buf) - 1, "KenshiCoop: remote player %u left", id);
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// M5: presence is body-less, so there is nothing to drive each frame - we only
// expire records for players we've stopped hearing from (disconnect/crash/quit),
// which keeps remotePlayerCount() and the NPC interest centers honest. The
// player's actual squad bodies time out and despawn through updateNpcs.
void updateGhosts(GameWorld* gw, float dt) {
    (void)gw; (void)dt;
    if (g_ghosts.empty()) return;

    const DWORD TIMEOUT_MS = 3000; // no heartbeat for this long -> drop presence
    DWORD now = GetTickCount();

    std::map<coop::u32, GhostState>::iterator it = g_ghosts.begin();
    while (it != g_ghosts.end()) {
        if ((now - it->second.lastSeenTick) > TIMEOUT_MS) {
            char buf[96];
            _snprintf(buf, sizeof(buf) - 1,
                "KenshiCoop: remote player %u presence timed out", it->first);
            buf[sizeof(buf) - 1] = '\0';
            coopLog(buf);
            std::map<coop::u32, GhostState>::iterator dead = it++;
            g_ghosts.erase(dead);
            continue;
        }
        ++it;
    }
}

// Drain remote player states delivered by the net thread and update each peer's
// PRESENCE record (M5: no ghost body). Runs on the MAIN thread.
void processInbound(GameWorld* gw) {
    std::deque<coop::PlayerStatePacket> items;
    g_inbound.drain(items);
    if (items.empty()) return;

    // Collapse to the most-recent state per player. Several packets can arrive
    // between frames; only the newest matters since it just sets the
    // interpolation target that updateGhosts() eases toward each frame. Process
    // PKT_PLAYER_LEFT markers in order: a disconnect drops the ghost and clears
    // any earlier queued state, while a later state for the same id re-joins.
    std::map<coop::u32, coop::PlayerStatePacket> latest;
    for (size_t i = 0; i < items.size(); ++i) {
        const coop::PlayerStatePacket& it = items[i];
        if (it.type == coop::PKT_PLAYER_LEFT) {
            if (it.playerId == coop::PLAYER_ID_ALL) latest.clear();
            else                                    latest.erase(it.playerId);
            despawnGhost(gw, it.playerId);
        } else {
            latest[it.playerId] = it;
        }
    }

    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    bool doLog = (now - lastLog >= 1000);
    if (doLog) lastLog = now;

    std::map<coop::u32, coop::PlayerStatePacket>::iterator it;
    for (it = latest.begin(); it != latest.end(); ++it) {
        const coop::PlayerStatePacket& p = it->second;
        if (doLog) {
            char buf[160];
            _snprintf(buf, sizeof(buf) - 1,
                "KenshiCoop: remote player %u tick %u pos (%.1f, %.1f, %.1f)",
                p.playerId, p.tick, p.x, p.y, p.z);
            buf[sizeof(buf) - 1] = '\0';
            coopLog(buf);
        }
        receiveRemoteState(gw, p);
    }
}

// Publish the local player's transform for the net thread to transmit.
// Runs on the MAIN thread inside the tick hook.
void publishLocalState(GameWorld* gw) {
    if (!gw || !gw->player) return;
    if (gw->player->playerCharacters.size() == 0) return;

    Character* chr = gw->player->playerCharacters[0];
    if (!chr) return;

    // getPosition() is RootObjectBase::getPosition() (virtual, returns
    // Ogre::Vector3); getOrientation() is RootObject::getOrientation() (virtual,
    // returns Ogre::Quaternion). Both are normal virtual calls here - the
    // "GetRealAddress doesn't work on virtuals" caveat only applies to hooking.
    Ogre::Vector3 pos = chr->getPosition();
    float heading = chr->getOrientation().getYaw().valueRadians();

    coop::PlayerStatePacket p;
    p.type     = coop::PKT_PLAYER_STATE;
    p.playerId = g_net.localId();
    p.tick     = g_tick;
    p.x        = pos.x;
    p.y        = pos.y;
    p.z        = pos.z;
    p.heading  = heading;

    g_net.setLocalState(p);

    // Throttled visibility (~once/second): proves the tick hook is reading live
    // game state. Remove or lower frequency once two-client sync is verified.
    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    if (now - lastLog >= 1000) {
        lastLog = now;
        char buf[160];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: local player pos (%.1f, %.1f, %.1f) heading %.2f",
            pos.x, pos.y, pos.z, heading);
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// ---- Phase 2 NPC replication: host side ------------------------------------
// Gather the transforms of NPCs near any player (host + remote ghosts), dedup
// by hand, drop our own squad, and publish the batch for the net thread to
// broadcast. Plain C++ (vectors/maps) calling only the SEH-guarded helpers.
void publishNpcStates(GameWorld* gw) {
    if (!g_getCharsFn || !gw || !gw->player) return;
    PlayerInterface* pl = gw->player;
    if (pl->playerCharacters.size() == 0) return;

    // Interest centers: the host's own player, plus every remote player's last
    // known position (their ghost target) so NPCs near a joined client stream.
    std::vector<Ogre::Vector3> centers;
    {
        Ogre::Vector3 pp;
        if (guardedGetPos(pl->playerCharacters[0], &pp)) centers.push_back(pp);
    }
    for (std::map<coop::u32, GhostState>::iterator g = g_ghosts.begin();
         g != g_ghosts.end(); ++g) {
        if (g->second.primed) centers.push_back(g->second.tgtPos);
    }
    if (centers.empty()) return;

    const size_t MAX_NPCS = 128;
    std::map<HandKey, coop::NpcStateEntry> picked; // dedup across centers
    DWORD nowPick = GetTickCount(); // for remote-owned-hand freshness checks

    // Pose-replication spike: log every picked NPC's current task once per second.
    bool logTasks = false;
    {
        static DWORD lastTaskLog = 0;
        DWORD tnow = GetTickCount();
        if (tnow - lastTaskLog >= 1000) { lastTaskLog = tnow; logTasks = true; }
    }

    for (size_t ci = 0; ci < centers.size() && picked.size() < MAX_NPCS; ++ci) {
        if (!guardedQueryNpcs(gw, &centers[ci])) continue;
        unsigned int total = g_npcQuery.size();
        for (unsigned int i = 0; i < total && picked.size() < MAX_NPCS; ++i) {
            RootObject* obj = g_npcQuery[i];
            if (!obj) continue;

            // Never replicate the host's own controllable squad members.
            bool isPlayer = false;
            unsigned int pc = pl->playerCharacters.size();
            for (unsigned int j = 0; j < pc; ++j) {
                if (static_cast<RootObject*>(pl->playerCharacters[j]) == obj) {
                    isPlayer = true; break;
                }
            }
            if (isPlayer) continue;

            coop::NpcStateEntry e;
            if (guardedReadNpc(obj, &e) != 1) continue;
            // M2: never re-stream a peer's squad member as a host NPC. The peer
            // owns and simulates it (and streams it via PKT_SQUAD_STATE); if we
            // also streamed it as an NPC, the peer would receive contradictory
            // host-authoritative transforms for its own unit and fight itself.
            HandKey key = makeKey(e);
            if (isRemoteOwnedHand(key, nowPick)) continue;
            if (g_syntheticHands.count(key)) continue; // our own proxy/ghost body
            picked[key] = e;

            if (logTasks) {
                char tb[176];
                _snprintf(tb, sizeof(tb) - 1,
                    "KenshiCoop: task npc pos(%.0f,%.0f,%.0f) type=%d(%s) "
                    "subj{t=%u c=%u cs=%u i=%u s=%u}",
                    e.x, e.y, e.z, (int)e.task, taskName((int)e.task),
                    e.stype, e.scontainer, e.scontainerSerial, e.sindex, e.sserial);
                tb[sizeof(tb) - 1] = '\0';
                coopLog(tb);
            }
        }
    }

    std::vector<coop::NpcStateEntry> out;
    out.reserve(picked.size());
    for (std::map<HandKey, coop::NpcStateEntry>::iterator it = picked.begin();
         it != picked.end(); ++it) {
        out.push_back(it->second);
    }
    g_net.setNpcStates(out.empty() ? 0 : &out[0], (unsigned int)out.size());

    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    if (now - lastLog >= 1000) {
        lastLog = now;
        char buf[96];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: host streaming %u NPC(s)", (unsigned int)out.size());
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// ---- Phase 2.5 squad replication: publish our OWN squad (bidirectional) -----
// Enumerate the ENTIRE local playerCharacters lektor and read each via the same
// guardedReadNpc used for NPCs, then hand the batch to the net thread tagged with
// our network id. Unlike publishNpcStates this runs on BOTH host and join, and it
// deliberately does NOT skip player members - streaming them is the whole point.
// The net layer chunks the batch per datagram, so no cap is needed here.
// Stable, cross-client ordinal for a player-squad member: the number of (non-
// synthetic) player characters whose save-stable hand sorts before this one's.
// Both clients load the identical shared squad, so sorting by hand yields the
// SAME ordinal on each machine, and (unlike the raw playerCharacters list index)
// it does not depend on enumeration order. We would prefer Character::squadMemberID
// as the identity, but the engine reports 0 for every member of the player squad
// (verified empirically in the inhabit logs), so it cannot disambiguate members.
// Returns -1 on fault so the caller can fall back to the positional index.
int squadMemberRank(PlayerInterface* pl, Character* target) {
    if (!pl || !target) return -1;
    coop::u32 th[5];
    if (!guardedGetHand(target, th)) return -1;
    HandKey tk = makeKeyFromHand(th);
    unsigned int n = pl->playerCharacters.size();
    int rank = 0;
    for (unsigned int i = 0; i < n; ++i) {
        Character* c = pl->playerCharacters[i];
        if (!c || c == target) continue;
        coop::u32 h[5];
        if (!guardedGetHand(c, h)) continue;
        HandKey k = makeKeyFromHand(h);
        if (g_syntheticHands.count(k)) continue; // ignore proxies / remote ghosts
        if (k < tk) ++rank;
    }
    return rank;
}

// True if THIS client owns a given player-squad member under the configured
// ownership partition (see g_ownAll / KENSHICOOP_OWN_INDICES). An owned member
// is locally controlled + streamed; a non-owned member is driven from the peer's
// stream (the inhabit model).
//
// Identity is the hand-derived stable rank above (principled ownership: stable
// cross-client and under list reordering), NOT the raw positional index. The
// KENSHICOOP_OWN_INDICES knob is interpreted against this rank (so "0" = the
// leader, "~0" = everyone else), kept as a test override. Falls back to the
// positional index if the rank can't be computed, so a transient fault never
// makes BOTH clients disown the same member (which would refreeze it).
bool weOwnSquadMember(PlayerInterface* pl, Character* c, unsigned int fallbackIndex) {
    if (g_ownAll) return true;
    int rank = squadMemberRank(pl, c);
    unsigned int key = (rank >= 0) ? (unsigned int)rank : fallbackIndex;
    bool listed = g_ownIndices.count(key) != 0;
    return g_ownExcept ? !listed : listed;
}

void publishSquadState(GameWorld* gw) {
    if (!gw || !gw->player) return;
    PlayerInterface* pl = gw->player;
    unsigned int n = pl->playerCharacters.size();
    if (n == 0) { g_net.setSquadStates(g_net.localId(), 0, 0); return; }

    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    bool  doLog = (now - lastLog >= 1000);
    std::string ownedIds; // squadMemberIDs we streamed (diagnostic)

    std::vector<coop::NpcStateEntry> out;
    out.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
        Character* c = pl->playerCharacters[i];
        if (!c) continue;
        if (!weOwnSquadMember(pl, c, i)) continue; // peer owns this member; don't stream it
        coop::NpcStateEntry e;
        if (guardedReadNpc(static_cast<RootObject*>(c), &e) != 1) continue;
        // Skip synthetic bodies (proxies / remote-player ghosts) that the engine
        // enrolled in our playerCharacters: streaming them back would make the
        // peer proxy our proxy, in an unbounded feedback loop.
        if (g_syntheticHands.count(makeKey(e))) continue;
        out.push_back(e);
        if (doLog) {
            int rank = squadMemberRank(pl, c);
            char nb[16]; _snprintf(nb, sizeof(nb) - 1, "%d", rank); nb[15] = '\0';
            if (!ownedIds.empty()) ownedIds += ",";
            ownedIds += nb;
        }
    }
    g_net.setSquadStates(g_net.localId(),
                         out.empty() ? 0 : &out[0], (unsigned int)out.size());

    if (doLog) {
        lastLog = now;
        char buf[128];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: streaming %u own-squad member(s) [rank: %s]",
            (unsigned int)out.size(), ownedIds.empty() ? "-" : ownedIds.c_str());
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// Build a fresh GhostState (driven-entity record) for a resolved-or-proxy body
// from a received wire entry. Mirrors the NPC receiver's first-sight setup.
void initDrivenGhost(GhostState& gs, Character* c, const coop::NpcStateEntry& e,
                     DWORD now, bool isProxy) {
    Ogre::Vector3 target(e.x, e.y, e.z);
    gs.chr          = c;
    gs.tgtPos       = target;
    gs.prevTgtPos   = target;
    gs.vel          = Ogre::Vector3::ZERO;
    gs.tgtHeading   = e.heading;
    gs.lastDest     = target;
    gs.lastMoveTick = 0;       // start "at rest" so a static member is left calm
    gs.lastSeenTick = now;
    gs.primed       = true;
    gs.destIssued   = false;
    gs.parked       = false;
    gs.hostTask     = e.task;
    gs.hostSubj[0]  = e.stype;
    gs.hostSubj[1]  = e.scontainer;
    gs.hostSubj[2]  = e.scontainerSerial;
    gs.hostSubj[3]  = e.sindex;
    gs.hostSubj[4]  = e.sserial;
    gs.hostMoving   = (e.cmoving != 0);
    gs.hostSpeed    = e.cspeed;
    gs.hostMotion   = Ogre::Vector3(e.cmotionX, e.cmotionY, e.cmotionZ);
    gs.isProxy      = isProxy;
}

// Update an existing driven GhostState from a newer wire entry (same regime the
// NPC receiver uses for already-tracked entries: velocity, task re-arm, mirror).
void updateDrivenGhost(GhostState& gs, const coop::NpcStateEntry& e, DWORD now) {
    Ogre::Vector3 target(e.x, e.y, e.z);
    Ogre::Vector3 step = target - gs.tgtPos;
    const float MOVE_EPS = 0.15f;
    if (step.squaredLength() > MOVE_EPS * MOVE_EPS) {
        gs.vel          = step;
        gs.lastMoveTick = now;
    }
    gs.prevTgtPos   = gs.tgtPos;
    gs.tgtPos       = target;
    gs.tgtHeading   = e.heading;
    gs.lastSeenTick = now;
    if (gs.hostTask != e.task || gs.hostSubj[3] != e.sindex ||
        gs.hostSubj[4] != e.sserial) {
        gs.taskTick   = 0;
        gs.taskActive = false;
        gs.taskBad    = false;
    }
    gs.hostTask     = e.task;
    gs.hostSubj[0]  = e.stype;
    gs.hostSubj[1]  = e.scontainer;
    gs.hostSubj[2]  = e.scontainerSerial;
    gs.hostSubj[3]  = e.sindex;
    gs.hostSubj[4]  = e.sserial;
    gs.hostMoving   = (e.cmoving != 0);
    gs.hostSpeed    = e.cspeed;
    gs.hostMotion   = Ogre::Vector3(e.cmotionX, e.cmotionY, e.cmotionZ);
}

// Spawn a local stand-in for a peer-owned squad member that has no Character in
// our save (M4). Borrows the local player's faction/template/container (always
// valid loaded data, same as the player-ghost path), then quiets it (clear goals
// + leave our squad) so only our networked drive moves it. Returns 0 on failure.
Character* spawnSquadProxy(GameWorld* gw, const coop::NpcStateEntry& e) {
    if (!gw || !gw->player || !gw->theFactory) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    Character* local = gw->player->playerCharacters[0];
    if (!local) return 0;
    Faction*             faction = local->getFaction();
    GameData*            tmpl    = local->getGameData();
    // owner = NULL: a proxy renders the PEER's unit, so it must NOT join our
    // player squad (otherwise we'd be able to select/command the other player's
    // characters). createRandomCharacter tolerates a null container (proven by
    // the M0 spawn-into-squad work) - it makes a free world body we then drive.
    Ogre::Vector3 at(e.x, e.y, e.z);
    Character* proxy = guardedSpawn(gw->theFactory, faction, &at, /*owner=*/0, tmpl);
    if (!proxy) return 0;
    char namebuf[64];
    _snprintf(namebuf, sizeof(namebuf) - 1, "Remote Squad %u", e.hindex);
    namebuf[sizeof(namebuf) - 1] = '\0';
    std::string name(namebuf);
    guardedQuiet(proxy, name); // setName + clear goals + separate from our squad
    markSynthetic(proxy, true); // never stream our proxy back to the peer
    return proxy;
}

// ---- Phase 2.5 squad replication: receive peer squads -----------------------
// Drain owner-tagged squad members. During a scenario, log each as a SCENARIO
// RECV line (matches the host's SCENARIO MEMBER by hand for CROSSCHECK). Then
// render the peer's squad (M3/M4): resolve shared-save members to their local
// Character and drive them; proxy-spawn peer-only members that don't resolve.
// Members WE own (in our playerCharacters) are skipped - we simulate those.
void receiveSquadState(GameWorld* gw) {
    std::deque<coop::OwnedNpcState> items;
    g_inbound.drainSquad(items);
    if (items.empty() || !gw) return;

    // Collapse to the newest entry per hand (several batches may have arrived).
    std::map<HandKey, coop::OwnedNpcState> latest;
    for (size_t i = 0; i < items.size(); ++i) latest[makeKey(items[i].e)] = items[i];

    // M2 de-confliction: register every received squad hand as remote-owned so
    // our own NPC stream (publishNpcStates) excludes it. Refreshed each receipt;
    // entries expire via REMOTE_OWN_TTL_MS when the owner stops streaming.
    DWORD nowOwn = GetTickCount();
    for (std::map<HandKey, coop::OwnedNpcState>::iterator it = latest.begin();
         it != latest.end(); ++it) {
        g_remoteOwnedHands[it->first] = nowOwn;
    }

    if (!g_scenarioName.empty()) {
        static DWORD lastRecvLog = 0;
        DWORD now = GetTickCount();
        if (now - lastRecvLog >= 500) {
            lastRecvLog = now;
            for (std::map<HandKey, coop::OwnedNpcState>::iterator it = latest.begin();
                 it != latest.end(); ++it) {
                const coop::NpcStateEntry& e = it->second.e;
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RECV hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
                    e.hindex, e.hserial, e.htype, e.hcontainer, e.hcontainerSerial,
                    e.x, e.y, e.z);
                b[sizeof(b) - 1] = '\0';
                coopLog(b);
            }
        }
    }

    // M3/M4 visual layer: render the peer's squad. Driving and (especially)
    // proxy-spawning require a live local world: until our own player squad is
    // loaded, guardedSpawn faults and createRandomCharacter has no scene to spawn
    // into. Bail before then so we don't burn the proxy-failure budget while this
    // client is still on the loading screen (the squad keeps streaming in over the
    // net, so we just pick it up once we're in-game).
    if (!gw->player || gw->player->playerCharacters.size() == 0) return;

    // Build the set of hands WE own (simulated locally; never drive them). Under
    // the inhabit partition this is only OUR owned subset - members the PEER owns
    // are intentionally NOT here, so they fall through to the resolve-and-drive
    // path below (the shared-save body exists locally and is driven by the peer's
    // stream). With the default (own-all) partition this is every member, i.e.
    // the legacy behavior where nothing the peer sends collides with our squad.
    DWORD now = GetTickCount();
    std::set<HandKey> ownHands;
    {
        PlayerInterface* pl = gw->player;
        unsigned int n = pl ? pl->playerCharacters.size() : 0;
        for (unsigned int i = 0; i < n; ++i) {
            Character* c = pl->playerCharacters[i];
            if (!c) continue;
            if (!weOwnSquadMember(pl, c, i)) continue; // peer-owned: let it resolve-and-drive
            coop::u32 h[5];
            if (guardedGetHand(c, h)) ownHands.insert(makeKeyFromHand(h));
        }
    }

    for (std::map<HandKey, coop::OwnedNpcState>::iterator it = latest.begin();
         it != latest.end(); ++it) {
        const HandKey&             key = it->first;
        const coop::NpcStateEntry& e   = it->second.e;

        if (ownHands.count(key)) continue; // our own member - simulated locally

        std::map<HandKey, GhostState>::iterator f = g_npcs.find(key);
        if (f != g_npcs.end()) {            // already driven -> just update target
            updateDrivenGhost(f->second, e, now);
            continue;
        }

        // New remote member. M3: a shared-save member resolves to a local body.
        Character* c = guardedHandToChar(e);
        if (c) {
            GhostState gs;
            initDrivenGhost(gs, c, e, now, false);
            g_npcs[key] = gs;
            g_proxyMiss.erase(key);
            continue;
        }

        // M4: GENUINELY peer-spawned member with no local Character (e.g. a recruit
        // the peer hired at runtime, whose hand does not exist in the shared save).
        // This is the ONLY case that proxies in v1: on the shared-save inhabit path
        // every existing squad member resolves above and takes resolve-and-drive, so
        // we never proxy a body that already exists locally. Spawn the proxy only
        // after a few unresolved frames (debounce transient loads).
        if (g_squadProxyDisabled) continue;
        if (++g_proxyMiss[key] < PROXY_SPAWN_AFTER_MISSES) continue;
        Character* proxy = spawnSquadProxy(gw, e);
        if (!proxy) {
            if (++g_squadProxyFailures >= 5) {
                g_squadProxyDisabled = true;
                coopErr("KenshiCoop: squad proxy spawning failed; disabled");
            }
            continue;
        }
        GhostState gs;
        initDrivenGhost(gs, proxy, e, now, true);
        g_npcs[key] = gs;
        g_proxyMiss.erase(key);
        char buf[160];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: spawned squad proxy for hand %u,%u at (%.1f,%.1f,%.1f)",
            e.hindex, e.hserial, e.x, e.y, e.z);
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// ---- Phase 2 NPC replication: client side ----------------------------------
// Drain received NPC transforms, resolve each hand to the local Character,
// suppress its AI on first sight, and record the latest target. Main thread.
void receiveNpcStates(GameWorld* gw) {
    std::deque<coop::NpcStateEntry> items;
    g_inbound.drainNpc(items);
    if (items.empty() || !gw) return;

    // Collapse to the newest entry per hand (several batches may have arrived).
    std::map<HandKey, coop::NpcStateEntry> latest;
    for (size_t i = 0; i < items.size(); ++i) latest[makeKey(items[i])] = items[i];

    DWORD now = GetTickCount();

    // Scenario observation (join side): log every received NPC as a RECV line in
    // the same schema the host emits SCENARIO MEMBER, so run_test.ps1 can match
    // them by hand. Throttled to ~500ms to keep the log readable.
    if (!g_scenarioName.empty()) {
        static DWORD lastRecvLog = 0;
        if (now - lastRecvLog >= 500) {
            lastRecvLog = now;
            for (std::map<HandKey, coop::NpcStateEntry>::iterator rt = latest.begin();
                 rt != latest.end(); ++rt) {
                const coop::NpcStateEntry& e = rt->second;
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RECV hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
                    e.hindex, e.hserial, e.htype, e.hcontainer, e.hcontainerSerial,
                    e.x, e.y, e.z);
                b[sizeof(b) - 1] = '\0';
                coopLog(b);
            }
        }
    }

    int newlySuppressed = 0;
    int resolvedOk = 0, resolveFail = 0, tracked = 0;

    for (std::map<HandKey, coop::NpcStateEntry>::iterator it = latest.begin();
         it != latest.end(); ++it) {
        const coop::NpcStateEntry& e = it->second;
        Ogre::Vector3 target(e.x, e.y, e.z);

        std::map<HandKey, GhostState>::iterator f = g_npcs.find(it->first);
        if (f == g_npcs.end()) {
            Character* c = guardedHandToChar(e);
            if (!c) { ++resolveFail; continue; } // not loaded / handle didn't resolve
            ++resolvedOk;
            {
                Ogre::Vector3 ipos;
                bool iok = guardedGetPos(c, &ipos);
                char dbg[176];
                _snprintf(dbg, sizeof(dbg) - 1,
                    "KenshiCoop: resolved npc chr=%p getpos=%d pos(%.1f,%.1f,%.1f) "
                    "tgt(%.1f,%.1f,%.1f) hand{t=%u c=%u cs=%u i=%u s=%u}",
                    (void*)c, iok ? 1 : 0, ipos.x, ipos.y, ipos.z,
                    e.x, e.y, e.z, e.htype, e.hcontainer, e.hcontainerSerial,
                    e.hindex, e.hserial);
                dbg[sizeof(dbg) - 1] = '\0';
                coopLog(dbg);
            }
            // Do NOT clear goals on first sight: a resting NPC must keep its
            // local job so it can sit/idle in the same pose as the host. We only
            // suppress AI while we are actively driving a MOVING NPC (see below).
            ++newlySuppressed;

            GhostState gs;
            gs.chr          = c;
            gs.tgtPos       = target;
            gs.prevTgtPos   = target;
            gs.vel          = Ogre::Vector3::ZERO;
            gs.tgtHeading   = e.heading;
            gs.lastDest     = target;
            gs.lastMoveTick = 0; // start "at rest" so a static NPC is left alone
            gs.lastSeenTick = now;
            gs.primed       = true;
            gs.destIssued   = false;
            gs.parked       = false;
            gs.hostTask     = e.task;
            gs.hostSubj[0]  = e.stype;
            gs.hostSubj[1]  = e.scontainer;
            gs.hostSubj[2]  = e.scontainerSerial;
            gs.hostSubj[3]  = e.sindex;
            gs.hostSubj[4]  = e.sserial;
            gs.hostMoving   = (e.cmoving != 0);
            gs.hostSpeed    = e.cspeed;
            gs.hostMotion   = Ogre::Vector3(e.cmotionX, e.cmotionY, e.cmotionZ);
            g_npcs[it->first] = gs;
        } else {
            ++tracked;
            GhostState& gs = f->second;
            Ogre::Vector3 step = target - gs.tgtPos;
            const float MOVE_EPS = 0.15f;
            if (step.squaredLength() > MOVE_EPS * MOVE_EPS) {
                gs.vel          = step;
                gs.lastMoveTick = now;
            }
            gs.prevTgtPos   = gs.tgtPos;
            gs.tgtPos       = target;
            gs.tgtHeading   = e.heading;
            gs.lastSeenTick = now;
            // If the host changed this NPC's task or its subject, mark it for
            // re-issue so the local copy follows (e.g. it got up and sat elsewhere).
            if (gs.hostTask != e.task || gs.hostSubj[3] != e.sindex ||
                gs.hostSubj[4] != e.sserial) {
                gs.taskTick   = 0;     // force re-apply on the next rest frame
                gs.taskActive = false;
                gs.taskBad    = false; // give the new task a fresh chance
            }
            gs.hostTask     = e.task;
            gs.hostSubj[0]  = e.stype;
            gs.hostSubj[1]  = e.scontainer;
            gs.hostSubj[2]  = e.scontainerSerial;
            gs.hostSubj[3]  = e.sindex;
            gs.hostSubj[4]  = e.sserial;
            gs.hostMoving   = (e.cmoving != 0);
            gs.hostSpeed    = e.cspeed;
            gs.hostMotion   = Ogre::Vector3(e.cmotionX, e.cmotionY, e.cmotionZ);
        }
    }

    (void)newlySuppressed;
    static DWORD lastLog = 0;
    if (now - lastLog >= 1000) {
        lastLog = now;
        char buf[160];
        _snprintf(buf, sizeof(buf) - 1,
            "KenshiCoop: npc rx recv=%u resolved=%d tracked=%d unresolved=%d active=%u",
            (unsigned int)latest.size(), resolvedOk, tracked, resolveFail,
            (unsigned int)g_npcs.size());
        buf[sizeof(buf) - 1] = '\0';
        coopLog(buf);
    }
}

// Kinematic puppet drive for replicated NPCs. Unlike player ghosts (which we
// path via setDestination so they animate), NPCs are eased toward the networked
// target with a per-frame teleport: no engine pathfinding, so they never snag on
// furniture, and the halt()+teleport each frame overrides any residual local-AI
// drift. Large gaps snap hard; small gaps ease for a smooth slide. Returns false
// only on a guarded fault.
bool driveNpcKinematic(GhostState& gs) {
    Ogre::Vector3 actual;
    if (!guardedGetPos(gs.chr, &actual)) return true; // transient; keep it

    const float SNAP_DIST = 8.0f;            // beyond this, jump straight there
    const float SNAP_SQ   = SNAP_DIST * SNAP_DIST;
    const float EASE      = 0.35f;           // fraction of remaining gap per frame

    Ogre::Vector3 delta = gs.tgtPos - actual;
    Ogre::Vector3 place = (delta.squaredLength() > SNAP_SQ)
                              ? gs.tgtPos
                              : actual + delta * EASE;
    Ogre::Quaternion rot(Ogre::Radian(gs.tgtHeading), Ogre::Vector3::UNIT_Y);
    // No-halt teleport: halting every frame would freeze the walk clip on frame
    // 0. The host's locomotion state is mirrored separately (guardedApplyMotion)
    // so the body plays the matching walk/run cycle while it slides.
    return guardedTeleport(gs.chr, &place, &rot);
}

// Per frame (client): drive every replicated NPC toward its target. On timeout
// (host stopped streaming it) hand the body back to local AI - never destroy it.
void updateNpcs(GameWorld* gw) {
    if (g_npcs.empty()) return;
    const DWORD TIMEOUT_MS = 3000;
    DWORD now = GetTickCount();

    static DWORD lastDiag = 0;
    bool diag = (now - lastDiag >= 1000);
    if (diag) lastDiag = now;
    int diagIdx = 0;

    std::map<HandKey, GhostState>::iterator it = g_npcs.begin();
    while (it != g_npcs.end()) {
        GhostState& gs = it->second;
        if (!gs.primed || !gs.chr) { ++it; continue; }

        if ((now - gs.lastSeenTick) > TIMEOUT_MS) {
            if (gs.isProxy) {
                // A proxy has no business existing once its owner stops streaming
                // it (the peer-spawned unit it stood in for is gone): destroy it.
                coopLog("KenshiCoop: squad proxy despawned (timeout)");
                markSynthetic(gs.chr, false);
                guardedDespawn(gw, gs.chr);
            } else {
                // A resolved local body: hand it back to local AI. If we pulled
                // it out of the update list to drive it (P1), re-enroll it so the
                // world keeps living instead of leaving a frozen body behind.
                if (gs.suppressed) guardedRestoreNpc(gw, gs.chr);
                coopLog("KenshiCoop: npc released (timeout)");
            }
            std::map<HandKey, GhostState>::iterator dead = it++;
            g_npcs.erase(dead);
            continue;
        }

        // Position is host-authoritative (local AI diverges by 30-100m if left
        // alone), so we always own the body:
        //   MOVING (host target moved recently): slide kinematically with the host.
        //   AT REST (host target still): suppress the local AI (so it does not try
        //     to walk -- which, while we pin the body, looks like marching in
        //     place) and hold the NPC at the host transform with a small deadzone.
        //     This gives a calm standing idle at the correct position. (Matching
        //     the host's exact sit/idle pose would require streaming the host's
        //     animation state; tracked as a follow-up.)
        const DWORD STOP_MS = 500;
        bool moving = (now - gs.lastMoveTick) < STOP_MS;
        bool ok = true;

        if (g_suppressAI) {
            // ---- Drive-authority path (P1) -------------------------------------
            // Pull the body out of the engine's main AI update list ONCE, so the
            // engine stops simulating it autonomously and we own its transform.
            // This replaces the old per-frame "fight" (clearGoals every move,
            // neutralize + STAND_STILL at rest): with the AI gone there is no
            // residual walk intent to overwrite, so we just drive position, pose
            // the task once, and mirror the host's locomotion clip.
            if (!gs.suppressed) gs.suppressed = guardedSuppressNpc(gw, gs.chr);

            if (moving) {
                gs.parked     = false;
                gs.taskTick   = 0;      // re-pose the task next time it rests
                gs.taskActive = false;
                gs.taskBad    = false;
                ok = driveNpcKinematic(gs);
                guardedApplyMotion(gs.chr, gs.hostMoving, gs.hostSpeed, &gs.hostMotion);
            } else {
                // Reproduce the host's task at the host's fixture (same pose AND
                // place), ONCE on entering rest, with the same drift guard. A host
                // task change re-arms it via receiveNpcStates.
                const DWORD GRACE_MS     = 4000;
                const float DRIFT_MAX_SQ = 4.0f * 4.0f;
                bool haveTask = (gs.hostTask != coop::NPC_TASK_NONE) && !gs.taskBad;
                if (haveTask && gs.taskTick == 0) {
                    int r = guardedSetTask(gs.chr, (int)gs.hostTask, gs.hostSubj);
                    gs.taskTick   = now;
                    gs.taskActive = (r >= 2); // a real fixture target resolved
                }
                if (gs.taskActive && (now - gs.taskTick) > GRACE_MS) {
                    Ogre::Vector3 a;
                    if (guardedGetPos(gs.chr, &a) &&
                        (gs.tgtPos - a).squaredLength() > DRIFT_MAX_SQ) {
                        gs.taskActive = false;
                        gs.taskBad    = true;
                    }
                }
                if (!gs.taskActive) {
                    // No reproducible fixture: settle to the host transform ONCE
                    // (clean halt+teleport), then only re-place on drift WITHOUT
                    // halting so the idle clip keeps advancing. No neutralize /
                    // STAND_STILL needed - the AI is already off the update list.
                    const float REPARK_SQ = 1.0f * 1.0f;
                    Ogre::Vector3 a;
                    if (guardedGetPos(gs.chr, &a)) {
                        Ogre::Quaternion rot(Ogre::Radian(gs.tgtHeading),
                                             Ogre::Vector3::UNIT_Y);
                        if (!gs.parked) {
                            ok = guardedPark(gs.chr, &gs.tgtPos, &rot);
                            if (ok) gs.parked = true;
                        } else if ((gs.tgtPos - a).squaredLength() > REPARK_SQ) {
                            ok = guardedTeleport(gs.chr, &gs.tgtPos, &rot);
                        }
                    }
                    guardedApplyMotion(gs.chr, gs.hostMoving, gs.hostSpeed, &gs.hostMotion);
                }
            }
        } else if (moving) {
            gs.parked = false;
            gs.taskTick = 0;          // re-issue the task next time it rests
            gs.taskActive = false;
            gs.taskBad = false;
            gs.idleSet = false;       // re-issue STAND_STILL next time it rests
            gs.aiNeutralized = false; // re-neutralize if it stops again
            if (g_walkDrive) {
                // Engine-WALK the body to the host position so the engine grounds
                // it and plays a real walk cycle (no teleport-slide "float"). We
                // re-aim it at the host's current position each frame; the engine
                // acts on the next tick (this drive runs after g_mainLoop_orig).
                // No per-frame clearGoals: the HIGH_PRIORITY destination overrides
                // the AI's movement, and clearing would cancel the walk we issue.
                // No motion mirror either: the body is genuinely moving, so the
                // engine selects the grounded walk clip itself. A large gap (it
                // fell behind / host warped) still hard-snaps to catch up.
                Ogre::Vector3 actual;
                bool haveActual = guardedGetPos(gs.chr, &actual);
                const float SNAP_SQ = 8.0f * 8.0f;
                if (haveActual && (gs.tgtPos - actual).squaredLength() > SNAP_SQ) {
                    Ogre::Quaternion rot(Ogre::Radian(gs.tgtHeading),
                                         Ogre::Vector3::UNIT_Y);
                    ok = guardedTeleport(gs.chr, &gs.tgtPos, &rot);
                } else {
                    // Catch-up speed: walk at the host's pace, but faster the
                    // further we've fallen behind (we chase a moving destination,
                    // so a flat speed lags). Boost is gap-proportional and capped
                    // so it tightens position while still playing a walk/run clip.
                    float spd = gs.hostSpeed;
                    if (haveActual) {
                        float gap = (gs.tgtPos - actual).length();
                        spd += gap * 2.0f;
                        float cap = (gs.hostSpeed > 1.0f ? gs.hostSpeed : (float)RUN) * 2.5f;
                        if (spd > cap) spd = cap;
                    }
                    ok = guardedWalkTo(gs.chr, &gs.tgtPos, spd);
                }
            } else {
                // Legacy teleport-kinematic mover: tight position, but slides a
                // static pose (the "float"). Mirror the host locomotion so the
                // clip at least matches while it slides.
                guardedClearGoals(gs.chr);
                ok = driveNpcKinematic(gs);
                guardedApplyMotion(gs.chr, gs.hostMoving, gs.hostSpeed, &gs.hostMotion);
            }
        } else {
            // Reproduce the HOST's task at the HOST's fixture so the local copy
            // adopts the same pose AND position. Issue ONCE on entering rest (the
            // engine keeps the task running); a host task change re-arms it via
            // receiveNpcStates. DRIFT GUARD: some tasks (e.g. OPERATE_MACHINERY)
            // let the local AI re-pick a *different* nearby object, sending the NPC
            // tens of metres off. If a reproduced task wanders the NPC past
            // DRIFT_MAX from the host position (after a grace period to allow a
            // legitimate short walk), abandon it and hold the host position instead
            // (correct place, generic idle pose). Tasks whose subject doesn't
            // resolve here fall straight through to hold.
            const DWORD GRACE_MS    = 4000;
            const float DRIFT_MAX_SQ = 4.0f * 4.0f;
            bool haveTask = (gs.hostTask != coop::NPC_TASK_NONE) && !gs.taskBad;
            if (haveTask && gs.taskTick == 0) {
                int r = guardedSetTask(gs.chr, (int)gs.hostTask, gs.hostSubj);
                gs.taskTick   = now;
                gs.taskActive = (r >= 2); // a real fixture target resolved
            }
            if (gs.taskActive && (now - gs.taskTick) > GRACE_MS) {
                Ogre::Vector3 a;
                if (guardedGetPos(gs.chr, &a) &&
                    (gs.tgtPos - a).squaredLength() > DRIFT_MAX_SQ) {
                    gs.taskActive = false;
                    gs.taskBad    = true; // stop re-issuing this divergent task
                }
            }
            if (!gs.taskActive) {
                // No reproducible host fixture. Three things, each ONCE, make this
                // NPC behave like a quiet player ghost that still animates:
                //  1) neutralize its town AI package (clear goals + own squad) so
                //     it stops autonomously re-issuing walk orders;
                //  2) give it a concrete STAND_STILL task - a real, NON-walking
                //     idle. (Neutralize+park alone left ~1/3 of NPCs marching in
                //     place; a concrete idle task is what holds them. We use
                //     STAND_STILL rather than the host's task because tasks like
                //     OPERATE_MACHINERY with no specific fixture auto-walk the NPC
                //     to some machine and drift it tens of metres off.)
                //  3) settle to the host transform ONCE (halt+teleport) and then
                //     leave it alone, so the engine's idle clip actually ADVANCES
                //     rather than being reset to frame 0 every tick (= frozen).
                // Re-park only on a generous drift so we don't blip the animation.
                if (!gs.aiNeutralized) {
                    guardedNeutralizeNpc(gs.chr);
                    gs.aiNeutralized = true;
                    gs.parked        = false; // force one fresh settle below
                }
                if (!gs.idleSet) {
                    guardedSetIdleTask(gs.chr, STAND_STILL);
                    gs.idleSet = true;
                }
                const float REPARK_SQ = 1.0f * 1.0f;
                Ogre::Vector3 a;
                if (guardedGetPos(gs.chr, &a)) {
                    if (!gs.parked) {
                        // First settle: one clean stop (halt+teleport) to kill any
                        // in-progress step, then we never halt again so the idle
                        // clip advances.
                        Ogre::Quaternion rot(Ogre::Radian(gs.tgtHeading),
                                             Ogre::Vector3::UNIT_Y);
                        ok = guardedPark(gs.chr, &gs.tgtPos, &rot);
                        if (ok) gs.parked = true;
                    } else if ((gs.tgtPos - a).squaredLength() > REPARK_SQ) {
                        // Drifted: re-place WITHOUT halting (no phase reset).
                        Ogre::Quaternion rot(Ogre::Radian(gs.tgtHeading),
                                             Ogre::Vector3::UNIT_Y);
                        ok = guardedTeleport(gs.chr, &gs.tgtPos, &rot);
                    }
                }
                // Mirror the host's locomotion every frame so the AI's residual
                // "walk" intent (which marches a pinned body in place) is overwritten
                // by the host's true state - idle when the host is idle. No halt, so
                // the idle clip keeps advancing rather than freezing on frame 0.
                guardedApplyMotion(gs.chr, gs.hostMoving, gs.hostSpeed, &gs.hostMotion);
            }
        }

        if (!ok) {
            if (gs.isProxy) {
                coopLog("KenshiCoop: squad proxy despawned (drive fault)");
                markSynthetic(gs.chr, false);
                guardedDespawn(gw, gs.chr);
            } else {
                if (gs.suppressed) guardedRestoreNpc(gw, gs.chr);
                coopLog("KenshiCoop: npc released (drive fault)");
            }
            std::map<HandKey, GhostState>::iterator dead = it++;
            g_npcs.erase(dead);
            continue;
        }

        // Per-NPC diagnostic, logged AFTER our drive so the locomotion numbers
        // reflect what the renderer will sample this frame, not the AI's
        // pre-empted intent. For a PARK NPC, mv=1/spd>0 == walk-in-place.
        if (diag) {
            Ogre::Vector3 a;
            bool gp = guardedGetPos(gs.chr, &a);
            char buf[256];
            if (gp) {
                float d = (gs.tgtPos - a).length();
                bool moving = (now - gs.lastMoveTick) < STOP_MS;
                bool  mvMoving = false; float mvSpeed = 0.0f, mvDesired = 0.0f;
                guardedReadMotion(gs.chr, &mvMoving, &mvSpeed, &mvDesired);
                _snprintf(buf, sizeof(buf) - 1,
                    "KenshiCoop: npc[%d] chr=%p getpos=1 actual(%.1f,%.1f,%.1f) "
                    "tgt(%.1f,%.1f,%.1f) gap=%.1f regime=%s sup=%d task=%d(%s) act=%d "
                    "mv=%d spd=%.2f des=%.2f host{mv=%d spd=%.2f}",
                    diagIdx, (void*)gs.chr, a.x, a.y, a.z,
                    gs.tgtPos.x, gs.tgtPos.y, gs.tgtPos.z,
                    d, moving ? "MOVE" : "PARK", gs.suppressed ? 1 : 0,
                    (int)gs.hostTask, taskName((int)gs.hostTask),
                    gs.taskActive ? 1 : 0,
                    mvMoving ? 1 : 0, mvSpeed, mvDesired,
                    gs.hostMoving ? 1 : 0, gs.hostSpeed);
            } else {
                _snprintf(buf, sizeof(buf) - 1,
                    "KenshiCoop: npc[%d] chr=%p getpos=0 (FAULT - bad pointer)",
                    diagIdx, (void*)gs.chr);
            }
            buf[sizeof(buf) - 1] = '\0';
            coopLog(buf);
        }
        ++diagIdx;
        ++it;
    }
}

// ---- Auto-load a save ------------------------------------------------------
// Resolve SaveManager's singleton accessor and the load(name) overload. Both
// are non-virtual, so GetRealAddress works (it does not on virtuals). load() is
// overloaded, so we disambiguate with an explicit member-pointer cast.
void resolveSaveFns() {
    g_saveMgrGetFn = (SaveMgrGetFn)KenshiLib::GetRealAddress(&SaveManager::getSingleton);
    g_saveMgrLoadFn = (SaveMgrLoadNameFn)KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const std::string&)>(&SaveManager::load));
    // Readiness gate: only used to confirm the save subsystem is up before we
    // issue the (deferred) load. Non-fatal if it can't be resolved.
    g_saveMgrSavesExistFn = (SaveMgrSavesExistFn)KenshiLib::GetRealAddress(
        &SaveManager::savesExist);
    if (!g_saveMgrGetFn || !g_saveMgrLoadFn)
        coopErr("KenshiCoop: could not resolve SaveManager load functions");
}

// SEH-guarded: fetch the SaveManager singleton and ask it to load 'name'.
// Returns false if the singleton isn't ready yet (caller retries next frame) or
// a guarded call faulted. No C++-unwinding objects live in this frame ('name'
// is a reference param), so __try is legal here.
//
// load(name) is DEFERRED: it sets the manager's LOADGAME signal and returns
// immediately; the engine's own SaveManager::execute() performs the actual
// world load a few frames later (verified empirically - load() returned ~2.7s
// before the world finished loading). So this is safe to call from the title
// update; the only requirement is that the menu/save subsystem has settled
// first (see titleUpdate_hook), otherwise the deferred load crashes mid-load.
bool guardedLoadSave(const std::string& name) {
    if (!g_saveMgrGetFn || !g_saveMgrLoadFn) return false;
    __try {
        SaveManager* mgr = g_saveMgrGetFn();
        if (!mgr) return false;
        g_saveMgrLoadFn(mgr, &name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-guarded readiness probe: does the save subsystem report existing saves
// yet? Used only to confirm it is up before issuing the load. Returns true when
// unresolved so it never blocks auto-load on a machine where the symbol is
// missing (the time-based settle is the primary gate).
bool guardedSavesExist() {
    if (!g_saveMgrGetFn || !g_saveMgrSavesExistFn) return true;
    __try {
        SaveManager* mgr = g_saveMgrGetFn();
        if (!mgr) return false;
        return g_saveMgrSavesExistFn(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Title-screen update hook: a safe main-thread point that fires every frame the
// main menu is up. We must NOT trigger the load the instant the menu appears -
// the deferred world load crashes if issued before the menu/save subsystem has
// settled (issuing it ~64ms after "Main menu loaded" reliably crashed). So we
// wait g_autoLoadDelayMs of wall-clock time after the first title frame AND
// until savesExist() reports the subsystem is up, then issue the load once.
// Installed only when KENSHICOOP_SAVE is set.
void titleUpdate_hook(TitleScreen* self) {
    g_titleUpdate_orig(self); // let the menu update first

    if (g_autoLoadDone || g_saveName.empty()) return;

    DWORD now = GetTickCount();
    if (g_titleFirstTick == 0) { g_titleFirstTick = now; return; } // start the clock
    if ((now - g_titleFirstTick) < g_autoLoadDelayMs) return;      // let it settle

    if (!g_saveMgrGetFn || !g_saveMgrLoadFn) {
        g_autoLoadDone = true; // nothing to retry; don't spam
        coopErr("KenshiCoop: auto-load skipped (SaveManager unresolved)");
        return;
    }

    if (!guardedSavesExist()) return; // save subsystem not ready yet; retry next frame

    if (guardedLoadSave(g_saveName)) {
        g_autoLoadDone = true;
        char m[128];
        _snprintf(m, sizeof(m) - 1,
                  "KenshiCoop: auto-load issued for save '%s' (after %lu ms settle)",
                  g_saveName.c_str(), (unsigned long)(now - g_titleFirstTick));
        m[sizeof(m) - 1] = '\0';
        coopLog(m);
    }
    // else: singleton not ready yet - try again next frame.
}

// ---- Discovery: what clips does the engine play, and for which tasks? --------
// SEH-isolated reader: copy the animation name + the character's current task
// key into POD out-params (no C++-unwinding objects here, so __try is legal).
bool guardedReadAnimCall(Character* c, const std::string* anim,
                         char* nameOut, int nameCap, int* taskOut) {
    __try {
        const char* s = anim ? anim->c_str() : 0;
        int i = 0;
        if (s) { for (; i < nameCap - 1 && s[i]; ++i) nameOut[i] = s[i]; }
        nameOut[i] = '\0';
        *taskOut = -1;
        CharBody* b = c ? c->body : 0;
        Tasker* t = b ? b->currentAction : 0;
        if (t && g_taskerKeyFn) *taskOut = (int)g_taskerKeyFn(t);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hook on Character::runSlaveAnim. Logs each unique (taskType, animName) pair
// ONCE so the log stays small while we harvest the clip vocabulary. Always calls
// through to the real animation so gameplay is unaffected.
void __fastcall runSlaveAnim_hook(void* self, const std::string* anim,
                                  float speed, float sync) {
    char nm[96]; int tk = -1;
    if (guardedReadAnimCall(static_cast<Character*>(self), anim, nm, sizeof(nm), &tk)) {
        static std::set<std::string> seen; // dedupe by "task|anim"
        char key[160];
        _snprintf(key, sizeof(key) - 1, "%d|%s", tk, nm);
        key[sizeof(key) - 1] = '\0';
        if (seen.insert(key).second) {
            char buf[224];
            _snprintf(buf, sizeof(buf) - 1,
                "KenshiCoop: SLAVEANIM task=%d(%s) anim='%s' spd=%.2f sync=%.2f",
                tk, taskName(tk), nm, speed, sync);
            buf[sizeof(buf) - 1] = '\0';
            coopLog(buf);
        }
    }
    g_runSlaveAnim_orig(self, anim, speed, sync);
}

// Our main-thread tick hook. The single safe point where we touch game state.
void mainLoop_hook(GameWorld* gw, float dt) {
    ++g_tick;

    // Detect the first live gameplay frame (player squad exists). This single
    // gate arms BOTH the test-runner self-exit timer and the scenario harness,
    // so scenarios work whether or not KENSHICOOP_TEST_SECONDS is set.
    if ((g_testSeconds > 0 || g_scenario || g_autoSpawnCount > 0) && !g_gameStarted &&
        gw && gw->player && gw->player->playerCharacters.size() > 0) {
        g_gameStarted   = true;
        g_gameStartTick = GetTickCount();
        coopLog("KenshiCoop: gameplay started");
    }

    // Test-runner self-exit: quit cleanly after the configured duration so
    // unattended Cursor runs terminate on their own and flush their logs. Also
    // serves as a hard backstop for a scenario that never reports completion.
    // Disabled entirely when KENSHICOOP_TEST_SECONDS is 0 (normal co-op play).
    if (g_testSeconds > 0 && g_gameStarted &&
        (GetTickCount() - g_gameStartTick) >= (DWORD)g_testSeconds * 1000u) {
        coopLog("KenshiCoop: test duration elapsed; exiting");
        coop::logClose();
        // TerminateProcess (not ExitProcess): we're calling from the game's
        // main-loop hook while GPU/audio/net worker threads are live. ExitProcess
        // runs orderly thread teardown + DLL detach under the loader lock and
        // deadlocks against those threads, leaving a non-responding zombie that
        // keeps the mod DLL file-locked (breaks the next deploy). TerminateProcess
        // exits immediately; our log is already flushed/closed above.
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // Manual-validation auto-spawn (KENSHICOOP_AUTOSPAWN): host only, no scenario.
    // Once gameplay is live and settled, spawn N distinct-hand units into our squad
    // ONCE, fanned out near the leader so they're visible together. They get fresh
    // hands (not in the join's playerCharacters), so the join renders them as
    // proxies and follows as you move them - the cross-client squad-render path.
    if (g_autoSpawnCount > 0 && !g_autoSpawnDone && !g_scenario && g_isHostMode &&
        g_gameStarted && gw && gw->player && gw->player->playerCharacters.size() > 0 &&
        (GetTickCount() - g_gameStartTick) >= AUTOSPAWN_DELAY_MS) {
        g_autoSpawnDone = true; // one-shot regardless of per-unit success
        Ogre::Vector3 base;
        Character* leader = gw->player->playerCharacters[0];
        if (!leader || !coop::getCharPos(leader, &base)) base = Ogre::Vector3::ZERO;
        int made = 0;
        for (int i = 0; i < g_autoSpawnCount; ++i) {
            Ogre::Vector3 at = base;
            at.x += (float)((i + 1) * 2);  // simple fan-out so they don't stack
            at.z += (float)((i % 2) * 2);
            if (coop::spawnIntoPlayerSquad(gw, 0, at)) ++made;
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1,
            "KenshiCoop: AUTOSPAWN spawned %d/%d manual squad members",
            made, g_autoSpawnCount);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Scenario harness: once gameplay is live, drive the selected scenario's
    // state machine. onStart() fires once; onTick() runs each frame until it
    // reports completion, then we log the PASS/FAIL verdict and exit (flushing
    // the dedicated log first). Both host and join run the same object; the
    // scenario itself branches on ctx.isHost.
    if (g_scenario && g_gameStarted && gw) {
        if (g_scenarioDoneTick != 0) {
            // Scenario already finished: HOLD the final synced state on screen so
            // the runner can screenshot both clients, then self-exit. We keep
            // falling through to the publish/receive/updateNpcs below so the
            // replicated bodies stay driven (rendered) during the hold.
            if (GetTickCount() - g_scenarioDoneTick >= SCENARIO_HOLD_MS) {
                coop::logClose();
                // TerminateProcess (not ExitProcess): avoids the loader-lock
                // deadlock when quitting from inside the live game loop.
                TerminateProcess(GetCurrentProcess(), 0);
            }
        } else {
            coop::ScenarioContext ctx;
            ctx.gw      = gw;
            ctx.isHost  = g_isHostMode;
            ctx.localId = g_net.localId();
            if (!g_scenarioStarted) {
                g_scenarioStarted   = true;
                g_scenarioStartTick = GetTickCount();
                ctx.elapsedMs = 0;
                ctx.tick      = g_scenarioTick;
                char m[160];
                _snprintf(m, sizeof(m) - 1, "SCENARIO %s start", g_scenario->name());
                m[sizeof(m) - 1] = '\0';
                coopLog(m);
                g_scenario->onStart(ctx);
            }
            ctx.elapsedMs = GetTickCount() - g_scenarioStartTick;
            ctx.tick      = ++g_scenarioTick;
            if (g_scenario->onTick(ctx)) {
                bool ok = g_scenario->passed();
                char m[64];
                _snprintf(m, sizeof(m) - 1, "SCENARIO RESULT %s", ok ? "PASS" : "FAIL");
                m[sizeof(m) - 1] = '\0';
                coopLog(m);
                g_scenarioDoneTick = GetTickCount(); // begin the capture hold
            }
        }
    }

    publishLocalState(gw);   // send our presence heartbeat (PKT_PLAYER_STATE)
    processInbound(gw);      // update remote-player PRESENCE from heartbeats
    updateGhosts(gw, dt);    // M5: expire stale presence (no body to drive)

    // Phase 2: host streams nearby NPC transforms (~20 Hz); client applies them.
    // The CLIENT drains the latest transforms BEFORE the main loop (so targets
    // are current) but applies them (updateNpcs) AFTER it: the engine's AI runs
    // inside g_mainLoop_orig and re-issues walk orders to our held NPCs every
    // tick, so driving them beforehand let the AI overwrite us (the bodies then
    // marched in place). Driving AFTER the loop gives us the last word - our
    // force-idle/teleport is what the renderer samples this frame.
    // Phase 2.5: squad streaming is BIDIRECTIONAL - each peer publishes its OWN
    // squad (~20 Hz) and observes the other's. receiveSquadState renders the
    // peer's squad: shared-save members are resolved and driven (M3), peer-only
    // members are proxy-spawned and driven (M4); updateNpcs drives both.
    {
        static DWORD lastSquad = 0;
        DWORD now = GetTickCount();
        if (now - lastSquad >= 50) { lastSquad = now; publishSquadState(gw); }
    }

    if (g_isHostMode) {
        static DWORD lastGather = 0;
        DWORD now = GetTickCount();
        if (now - lastGather >= 50) { lastGather = now; publishNpcStates(gw); }
        receiveSquadState(gw);   // observe + render the peer's squad (proxies)
        g_mainLoop_orig(gw, dt); // run the engine (incl. local AI) first...
        updateNpcs(gw);          // ...then drive the peer's squad bodies last
    } else {
        receiveNpcStates(gw);
        receiveSquadState(gw);   // observe + render the host's squad (proxies)
        g_mainLoop_orig(gw, dt); // run the engine (incl. local AI) first...
        updateNpcs(gw);          // ...then impose host state as the last word
    }
}

void startNetworking() {
    std::string mode = envOr("KENSHICOOP_MODE", "host");
    std::string ip   = envOr("KENSHICOOP_IP",   "127.0.0.1");
    int port         = std::atoi(envOr("KENSHICOOP_PORT", "27800").c_str());

    bool ok;
    if (mode == "join") {
        g_isHostMode = false;
        coopLog("KenshiCoop: starting as CLIENT");
        ok = g_net.startClient(ip, port, &g_inbound);
    } else {
        g_isHostMode = true;
        coopLog("KenshiCoop: starting as HOST");
        ok = g_net.startHost(port, &g_inbound);
    }
    if (!ok) coopErr("KenshiCoop: failed to start networking");
}

} // namespace

// ---- Scenario action facade (declared in ScenarioApi.h) --------------------
// Defined here, where the resolved function pointers and SEH-guarded leaves
// live. These are the ONLY game-touching entry points a scenario uses. The
// anonymous-namespace helpers above are at global scope, so unqualified lookup
// from inside namespace coop finds them.
namespace coop {

void scenarioLog(const char* msg) { coopLog(msg); }

int remotePlayerCount() { return (int)g_ghosts.size(); }

Character* localPlayer(GameWorld* gw) {
    if (!gw || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    return gw->player->playerCharacters[0];
}

Faction* playerFaction(GameWorld* gw) {
    Character* p = localPlayer(gw);
    return p ? p->getFaction() : 0; // virtual, safe (same call ghosts use)
}

RootObjectContainer* playerSquadContainer(GameWorld* gw) {
    // The player character's container IS its active-squad container (this is
    // exactly what the ghost spawn borrows to enlist into the player squad).
    Character* p = localPlayer(gw);
    return p ? p->container : 0;
}

GameData* playerTemplate(GameWorld* gw) {
    Character* p = localPlayer(gw);
    return p ? p->getGameData() : 0; // virtual, safe
}

GameData* lookupTemplate(GameWorld* gw, const char* stringId) {
    if (!stringId || !stringId[0]) return playerTemplate(gw);
    std::string sid(stringId);
    GameData* d = guardedLookupTemplate(gw, &sid);
    return d ? d : playerTemplate(gw); // fall back to a guaranteed-valid template
}

int playerSquadSize(GameWorld* gw) { return guardedSquadSize(gw); }

Character* spawnIntoPlayerSquad(GameWorld* gw, GameData* tmpl,
                                const Ogre::Vector3& pos) {
    if (!gw || !gw->theFactory) {
        coopLog("KenshiCoop: spawnIntoPlayerSquad FAIL reason=no_world_or_factory");
        return 0;
    }
    Faction*             fac   = playerFaction(gw);
    RootObjectContainer* owner = playerSquadContainer(gw);
    GameData*            t     = tmpl ? tmpl : playerTemplate(gw);
    // owner (the player's squad container) is intentionally NOT required: the
    // player character's RootObject::container is frequently null right after a
    // load, and createRandomCharacter tolerates a null certainContainer (it
    // makes its own) - this is exactly what the working ghost spawn path relies
    // on. We pass owner through when we have it, and enlist via recruit below.
    // Only faction + template are genuine preconditions.
    if (!fac || !t) {
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "KenshiCoop: spawnIntoPlayerSquad FAIL reason=null_input "
            "factory=%p faction=%p owner=%p tmpl=%p spawnFn=%p",
            (void*)gw->theFactory, (void*)fac, (void*)owner, (void*)t,
            (void*)g_spawnFn);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        return 0;
    }

    Character* c = guardedSpawn(gw->theFactory, fac, &pos, owner, t);
    if (!c) {
        char b[160];
        if (g_lastSpawnExCode != 0) {
            _snprintf(b, sizeof(b) - 1,
                "KenshiCoop: spawnIntoPlayerSquad FAIL reason=spawn_threw "
                "code=0x%08lX spawnFn=%p", g_lastSpawnExCode, (void*)g_spawnFn);
        } else {
            _snprintf(b, sizeof(b) - 1,
                "KenshiCoop: spawnIntoPlayerSquad FAIL reason=spawn_null "
                "spawnFn=%p faction=%p owner=%p tmpl=%p",
                (void*)g_spawnFn, (void*)fac, (void*)owner, (void*)t);
        }
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
        return 0;
    }
    // Spawning into the player's container already enlists the character in the
    // active squad; recruit() is belt-and-suspenders (best-effort, guarded).
    guardedRecruit(gw->player, c);
    return c;
}

bool getCharPos(Character* c, Ogre::Vector3* out) {
    if (!c || !out) return false;
    return guardedGetPos(c, out);
}

bool getCharHand(Character* c, u32 out[5]) { return guardedGetHand(c, out); }

bool teleportChar(Character* c, const Ogre::Vector3& pos, float headingRad) {
    if (!c) return false;
    Ogre::Quaternion rot(Ogre::Radian(headingRad), Ogre::Vector3::UNIT_Y);
    return guardedPark(c, &pos, &rot); // halt + place: a clean, stopped pose
}

bool moveCharTo(Character* c, const Ogre::Vector3& dest) {
    if (!c) return false;
    return guardedSetDestination(c, &dest);
}

void clearCharGoals(Character* c) { guardedClearGoals(c); }

void despawnChar(GameWorld* gw, Character* c) { guardedDespawn(gw, c); }

} // namespace coop

// RE_Kenshi calls this once when the plugin loads. RE_Kenshi resolves the entry
// point by its C++-mangled name (?startPlugin@@YAXXZ), exactly as the HelloWorld
// example does - so this must NOT be extern "C" (that would export plain
// "startPlugin" and RE_Kenshi would fail with "Could not initialize plugin").
__declspec(dllexport) void startPlugin() {
    // Read config early so logging and auto-load are set up before any hook
    // fires. Mode also picks the default log filename so host/join (which run
    // from different working dirs) never write to the same file.
    std::string mode = envOr("KENSHICOOP_MODE", "host");
    g_saveName       = envOr("KENSHICOOP_SAVE", "");
    g_testSeconds    = std::atoi(envOr("KENSHICOOP_TEST_SECONDS", "0").c_str());
    g_autoSpawnCount = std::atoi(envOr("KENSHICOOP_AUTOSPAWN", "0").c_str());
    {
        // Parse the ownership partition: optional leading '~' = exclusion set,
        // then a comma-separated index list. Empty => own all (default).
        std::string own = envOr("KENSHICOOP_OWN_INDICES", "");
        if (!own.empty()) {
            g_ownAll = false;
            const char* p = own.c_str();
            if (*p == '~') { g_ownExcept = true; ++p; }
            unsigned int v = 0; bool have = false;
            for (;; ++p) {
                if (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); have = true; }
                else {
                    if (have) { g_ownIndices.insert(v); v = 0; have = false; }
                    if (*p == '\0') break;
                }
            }
        }
    }
    g_suppressAI     = std::atoi(envOr("KENSHICOOP_SUPPRESS_AI", "0").c_str()) != 0;
    g_walkDrive      = std::atoi(envOr("KENSHICOOP_WALK_DRIVE", "1").c_str()) != 0;
    g_scenarioName   = envOr("KENSHICOOP_SCENARIO", "");
    {
        int d = std::atoi(envOr("KENSHICOOP_AUTOLOAD_DELAY_MS", "5000").c_str());
        if (d > 0) g_autoLoadDelayMs = (DWORD)d; // settle before auto-load
    }

    bool isJoin = (mode == "join");
    std::string defLog  = isJoin ? "KenshiCoop_join.log" : "KenshiCoop_host.log";
    std::string logPath = envOr("KENSHICOOP_LOG", defLog.c_str());
    coop::logInit(logPath.c_str(), isJoin ? "JOIN" : "HOST");

    coopLog("KenshiCoop loaded!"); // Milestone 1

    // Always surface the ownership + drive-authority config (the runner watches
    // for "inhabit ownership" to prove a fresh build is actually deployed; see
    // manual_session.ps1 / the -SkipDeploy fix). Logged unconditionally so the
    // marker is present even in own-all (non-partitioned) runs.
    {
        std::string idx;
        for (std::set<unsigned int>::iterator it = g_ownIndices.begin();
             it != g_ownIndices.end(); ++it) {
            char nb[16]; _snprintf(nb, sizeof(nb) - 1, "%u", *it); nb[15] = '\0';
            if (!idx.empty()) idx += ",";
            idx += nb;
        }
        char b[160];
        if (g_ownAll) {
            _snprintf(b, sizeof(b) - 1,
                "KenshiCoop: inhabit ownership = ALL (own entire squad); suppressAI=%d walkDrive=%d",
                g_suppressAI ? 1 : 0, g_walkDrive ? 1 : 0);
        } else {
            _snprintf(b, sizeof(b) - 1,
                "KenshiCoop: inhabit ownership = %s indices [%s]; suppressAI=%d walkDrive=%d",
                g_ownExcept ? "ALL EXCEPT" : "ONLY", idx.c_str(),
                g_suppressAI ? 1 : 0, g_walkDrive ? 1 : 0);
        }
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Milestone 2/4/5: hook the main-thread tick. Verified in GameWorld.h: the
    // class exposes both virtual mainLoop_GPUSensitiveStuff and a non-virtual
    // _NV_mainLoop_GPUSensitiveStuff (same RVA 0x7877A0). We must use the _NV_
    // variant because GetRealAddress does not work on virtual functions.
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &g_mainLoop_orig)) {
        coopErr("KenshiCoop: could not install main-loop hook!");
        return;
    }

    resolveGhostFns(); // Phase 1: map spawn/teleport to real runtime addresses
    resolveSaveFns();  // SaveManager::getSingleton/load for auto-load

    // Discovery: hook runSlaveAnim to harvest which clip names the engine plays
    // per TaskType (informs the pose-by-clip replication). Non-fatal if it fails.
    if (KenshiLib::SUCCESS !=
        KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::runSlaveAnim),
            &runSlaveAnim_hook, &g_runSlaveAnim_orig)) {
        coopErr("KenshiCoop: could not install runSlaveAnim discovery hook");
    }

    // Auto-load: only hook the title screen when a save name was provided. The
    // hook triggers the load once the menu is up (see titleUpdate_hook).
    if (!g_saveName.empty()) {
        if (KenshiLib::SUCCESS !=
            KenshiLib::AddHook(
                KenshiLib::GetRealAddress(&TitleScreen::_NV_update),
                &titleUpdate_hook, &g_titleUpdate_orig)) {
            coopErr("KenshiCoop: could not install title-screen hook (auto-load disabled)");
        } else {
            std::string m = "KenshiCoop: auto-load armed for save '" + g_saveName + "'";
            coopLog(m.c_str());
        }
    }
    if (g_testSeconds > 0) {
        char b[96];
        _snprintf(b, sizeof(b) - 1,
                  "KenshiCoop: test mode - self-exit %d s after gameplay starts",
                  g_testSeconds);
        b[sizeof(b) - 1] = '\0';
        coopLog(b);
    }

    // Scenario harness: build the selected scenario object (host & join both
    // run it; it branches on host/join internally). Unknown names are logged
    // and ignored (normal co-op tick continues).
    if (!g_scenarioName.empty()) {
        g_scenario = coop::makeScenario(g_scenarioName);
        if (g_scenario) {
            std::string m = "KenshiCoop: scenario armed '" + g_scenarioName + "'";
            coopLog(m.c_str());
        } else {
            std::string m = "KenshiCoop: unknown scenario '" + g_scenarioName + "'";
            coopErr(m.c_str());
        }
    }

    startNetworking();
}
