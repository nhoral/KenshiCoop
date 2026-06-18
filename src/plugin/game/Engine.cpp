// Engine implementation. See Engine.h. Built for the VS2010 (v100) toolchain.
//
// The Boost auto-link guards must precede any KenshiLib header that transitively
// pulls in <boost/thread/...> (e.g. RootObjectFactory.h), exactly as the legacy
// monolith does, or MSBuild tries to link Boost libs we neither ship nor use.

#define _CRT_SECURE_NO_WARNINGS 1
#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include "Engine.h"
#include "../CoopLog.h"

#include <core/Functions.h>         // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>       // GameWorld::player
#include <kenshi/PlayerInterface.h> // PlayerInterface::playerCharacters
#include <kenshi/SaveManager.h>     // SaveManager::getSingleton/load/savesExist
#include <kenshi/Character.h>       // Character (handle/getPosition/getOrientation/movement)
#include <kenshi/CharMovement.h>    // CharMovement::_setPositionDirectionAndTeleport/setDestination
#include <kenshi/Enums.h>           // itemType, MoveSpeed { RUN }
#include <kenshi/RootObject.h>      // RootObject base (getCharactersWithinSphere out type)
#include <kenshi/RootObjectBase.h>  // getGameData/getFaction (spawn template + owner)
#include <kenshi/RootObjectFactory.h> // createRandomCharacter / createBuilding
#include <kenshi/GameData.h>        // GameData::name (seat-template scan)
#include <kenshi/CharBody.h>        // CharBody::currentAction / _NV_setCurrentAction
#include <kenshi/Tasker.h>          // Tasker::key() -> TaskType, Tasker::subject (hand)
#include <kenshi/util/hand.h>       // hand (5-field identity, getRootObject)
#include <kenshi/util/lektor.h>     // lektor<T> (playerCharacters, interest query)
#include <ogre/OgreVector3.h>
#include <ogre/OgreQuaternion.h>
#include <cmath>
#include <set>
#include <utility>

namespace coop {
namespace engine {
namespace {

// SaveManager entry points, resolved at load. getSingleton is a static member;
// load(name) and savesExist are __thiscall (passed via __fastcall self in RCX).
typedef SaveManager* (__fastcall* SaveMgrGetFn)();
typedef void         (__fastcall* SaveMgrLoadNameFn)(SaveManager* self, const std::string* name);
typedef bool         (__fastcall* SaveMgrSavesExistFn)(SaveManager* self);

SaveMgrGetFn        g_getFn        = 0;
SaveMgrLoadNameFn   g_loadFn       = 0;
SaveMgrSavesExistFn g_savesExistFn = 0;

// hand::getCharacter resolves a save-stable handle back to the local Character*.
// The 5-arg hand constructor builds a proper hand (sets the vptr) from received
// fields so getCharacter() can be called on it.
typedef Character* (__fastcall* HandGetCharFn)(const hand* self);
typedef hand*      (__fastcall* HandCtorFn)(hand* self, unsigned int index,
                                            unsigned int serial, itemType type,
                                            unsigned int container,
                                            unsigned int containerSerial);

HandGetCharFn g_handGetCharFn = 0;
HandCtorFn    g_handCtorFn    = 0;

// Character::setDestination(pos, shift) is the PLAYER move-order path (what
// click-to-move issues); unlike CharMovement::setDestination it actually moves a
// player-controlled character. Non-virtual, so resolved at runtime.
typedef void (__fastcall* CharSetDestFn)(Character* self, const Ogre::Vector3* pos, bool shift);
CharSetDestFn g_charSetDestFn = 0;

// Stage 4 NPC replication: nearby-character interest query + AI quieting.
// getCharactersWithinSphere fills a lektor with nearby characters (as RootObject*
// base pointers); clearAllAIGoals stops a character pathing autonomously.
typedef void (__fastcall* GetCharsInSphereFn)(
    GameWorld* self, lektor<RootObject*>* results, const Ogre::Vector3* pos,
    float farRadius, float nearRadius, float always, int maxFar, int maxNear,
    RootObject* skip);
typedef void (__fastcall* ClearGoalsFn)(Character* self);
typedef void (__fastcall* UpdateListFn)(GameWorld* self, Character* c);

GetCharsInSphereFn g_getCharsFn    = 0;
ClearGoalsFn       g_clearGoalsFn  = 0;
UpdateListFn       g_removeUpdateFn = 0;
UpdateListFn       g_addUpdateFn    = 0;

// Stage 5 rest-pose reproduction. Tasker::key() reads the current TaskType;
// hand::getRootObject resolves a task's subject hand to its world fixture;
// CharBody::_NV_setCurrentAction(TaskType,target) commits the body to that task.
typedef int         (__fastcall* TaskerKeyFn)(const void* self);
typedef RootObject* (__fastcall* HandGetRootFn)(const hand* self);
typedef bool        (__fastcall* SetActionFn)(void* charBody, int taskType, RootObject* target);
// Character::addGoal(TaskType, RootObjectBase*): adds a PERSISTENT AI goal to
// perform the task at the subject. Unlike setCurrentAction (a transient body
// action the player-char AI overwrites within a frame), a goal is what actually
// holds a body seated/operating - it survives because the AI now owns the intent.
typedef void        (__fastcall* AddGoalFn)(Character* self, int taskType, RootObject* subject);
// I9: the PLAYER-ORDER path. addOrder/addJob take an explicit world LOCATION, so
// (unlike the autonomous SIT_AROUND goal, which re-runs a local seat search and
// wanders ~50 m) they pin the body to THIS fixture at THIS spot - the same path a
// player click-to-sit issues. separateIntoMyOwnSquad detaches the NPC from its
// town/faction so nothing auto-assigns it tasks (a "squad-like" inert puppet that
// only the host's order drives). location is a const-ref -> passed as a pointer.
typedef void      (__fastcall* AddOrderFn)(Character* self, Building* dest, int t,
                                           RootObject* subject, bool shift, bool clear,
                                           const Ogre::Vector3* location);
typedef void      (__fastcall* AddJobFn)(Character* self, int t, RootObject* subject,
                                         bool shift, bool addDontClear,
                                         const Ogre::Vector3* location);
typedef Platoon*  (__fastcall* SeparateSquadFn)(Character* self, bool permanent);
// CharBody::endAction() ends the body's CURRENT action. A node-anchored stander
// (STAND_AT_NODE) suspended mid-walk keeps EXECUTING its walk-to-node action, so
// held in place it marches; ending the action drops it to idle (the standing
// analog of pinning a seat).
typedef void      (__fastcall* EndActionFn)(void* charBody);

TaskerKeyFn   g_taskerKeyFn   = 0;
HandGetRootFn g_handGetRootFn = 0;
SetActionFn   g_setActionFn   = 0;
AddGoalFn     g_addGoalFn     = 0;
AddOrderFn      g_addOrderFn      = 0;
AddJobFn        g_addJobFn        = 0;
SeparateSquadFn g_separateSquadFn = 0;
EndActionFn     g_endActionFn     = 0;

// AI-gating probe lever: PlayerInterface::recruit(Character*, bool) adds an NPC
// to the local player's squad (the "inhabit" path). A recruited body stops
// self-assigning town tasks (player chars idle until ordered) and obeys our
// drive, so we can mirror the host without replicating animation data.
typedef bool (__fastcall* RecruitFn)(PlayerInterface* self, Character* c, bool editor);
RecruitFn g_recruitFn = 0;

// AI decision-layer detour. Character::periodicUpdate() is the per-character AI
// "think" tick that re-scores/re-assigns autonomous town tasks (and thus keeps
// overwriting whatever pose we set). Detouring it lets us SUSPEND just the
// decision layer for host-driven NPCs while CharBody::update (animation + action
// execution) keeps running - the body still animates, it just stops self-tasking.
// The suspended set is keyed by Character* and only ever touched on the main
// thread (the detour fires during the engine tick; the set is rebuilt after it).
typedef void (__fastcall* PeriodicUpdateFn)(Character* self);
PeriodicUpdateFn      g_periodicOrig = 0;
std::set<Character*>  g_aiSuspended;

// The detour never dereferences 'self' (it is only compared as a key), so a
// stale/odd pointer cannot fault here; no SEH frame needed.
void __fastcall periodicUpdate_hook(Character* self) {
    if (!g_aiSuspended.empty() &&
        g_aiSuspended.find(self) != g_aiSuspended.end()) {
        return; // decision layer suspended: host drives the task, AI stays quiet
    }
    g_periodicOrig(self);
}

// Test-scene spawning. createRandomCharacter / createBuilding take Vector3 (and
// Quaternion) BY VALUE; declaring the typedef with the exact by-value signature
// lets the compiler emit the correct x64 ABI (large structs passed by hidden
// reference), mirroring the legacy monolith's proven spawn typedef.
typedef RootObject* (__fastcall* CreateCharFn)(
    RootObjectFactory* self, Faction* faction, Ogre::Vector3 position,
    RootObjectContainer* owner, GameData* characterTemplate, Building* home, float age);
typedef Building* (__fastcall* CreateBuildingFn)(
    RootObjectFactory* self, GameData* data, Ogre::Vector3 position, TownBase* t,
    Faction* owner, Ogre::Quaternion rotation, FactoryCallbackInterface* cb,
    Layout* furnitureOf, Building* isDoorOf, GameSaveState* saveState,
    Building* isIndoorsOf, bool invisible, bool completed, bool isFoliage,
    int floorNumber, bool isOutsideFurniture);
typedef void (__fastcall* GetDataOfTypeFn)(
    GameDataContainer* self, lektor<GameData*>* list, itemType type);

CreateCharFn     g_createCharFn   = 0;
CreateBuildingFn g_createBldgFn   = 0;
GetDataOfTypeFn  g_getDataOfTypeFn = 0;

// Honest pose oracle. getPositionBip01 is non-virtual (no vtable), so it must be
// resolved + called through a pointer (returns Ogre::Vector3 BY VALUE). isIdle /
// isCrouched are non-virtual bool reads on CharBody.
//
// CRITICAL ABI: for an MSVC x64 MEMBER function returning a struct by value, the
// `this` pointer stays in RCX and the hidden return-buffer pointer is passed in
// RDX (this-first, NOT retbuf-first as for free functions). So we must model it as
// `Vector3* fn(Character* this, Vector3* retbuf)` and call fn(c, &out) - declaring
// it `Vector3 fn(Character*)` makes the compiler put the retbuf in RCX and `this`
// in RDX, which writes through the wrong pointer and FAULTS.
typedef Ogre::Vector3* (__fastcall* GetBip01Fn)(Character* self, Ogre::Vector3* ret);
typedef bool           (__fastcall* CharBodyBoolFn)(const void* self);
// getBoneWorldPosition(const std::string&) - animated skeleton bone in WORLD space
// (this is the one that actually drops when seated). Same member-struct-return ABI
// (this=RCX, retbuf=RDX) plus the string-by-pointer arg in R8.
typedef Ogre::Vector3* (__fastcall* GetBoneWorldPosFn)(Character* self, Ogre::Vector3* ret,
                                                       const std::string* name);

GetBip01Fn       g_getBip01Fn      = 0;
GetBoneWorldPosFn g_getBoneWorldFn = 0;
CharBodyBoolFn   g_isIdleFn        = 0;
CharBodyBoolFn   g_isCrouchedFn    = 0;

lektor<GameData*> g_dataScratch; // reused seat-template scan buffer (main thread)

// One reused scratch buffer for the interest query (main thread only).
lektor<RootObject*> g_npcQuery;

} // namespace

void resolve() {
    g_getFn  = (SaveMgrGetFn)KenshiLib::GetRealAddress(&SaveManager::getSingleton);
    g_loadFn = (SaveMgrLoadNameFn)KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const std::string&)>(&SaveManager::load));
    g_savesExistFn = (SaveMgrSavesExistFn)KenshiLib::GetRealAddress(&SaveManager::savesExist);
    if (!g_getFn || !g_loadFn)
        coop::logErrLine("engine: could not resolve SaveManager load functions");

    // Entity resolve path: hand->Character lookup and the 5-arg hand ctor
    // (overloaded, so disambiguate via an explicit member-pointer cast).
    g_handGetCharFn = (HandGetCharFn)KenshiLib::GetRealAddress(&hand::getCharacter);
    g_handCtorFn = (HandCtorFn)KenshiLib::GetRealAddress(
        static_cast<hand* (hand::*)(unsigned int, unsigned int, itemType,
                                    unsigned int, unsigned int)>(&hand::_CONSTRUCTOR));
    if (!g_handGetCharFn || !g_handCtorFn)
        coop::logErrLine("engine: could not resolve hand resolve functions");

    g_charSetDestFn = (CharSetDestFn)KenshiLib::GetRealAddress(
        static_cast<void (Character::*)(const Ogre::Vector3&, bool)>(
            &Character::setDestination));

    // Stage 4 NPC replication. Non-fatal: if unresolved, NPC streaming/quieting
    // is simply skipped (squad sync still works).
    g_getCharsFn = (GetCharsInSphereFn)KenshiLib::GetRealAddress(
        &GameWorld::getCharactersWithinSphere);
    g_clearGoalsFn = (ClearGoalsFn)KenshiLib::GetRealAddress(&Character::clearAllAIGoals);
    g_removeUpdateFn = (UpdateListFn)KenshiLib::GetRealAddress(&GameWorld::removeFromUpdateListMain);
    g_addUpdateFn    = (UpdateListFn)KenshiLib::GetRealAddress(&GameWorld::addToUpdateListMain);
    if (!g_getCharsFn)
        coop::logErrLine("engine: could not resolve getCharactersWithinSphere (NPC stream off)");

    // Stage 5 pose reproduction. Non-fatal: if unresolved, rest NPCs idle-park.
    g_taskerKeyFn = (TaskerKeyFn)KenshiLib::GetRealAddress(&Tasker::key);
    g_handGetRootFn = (HandGetRootFn)KenshiLib::GetRealAddress(&hand::getRootObject);
    g_setActionFn = (SetActionFn)KenshiLib::GetRealAddress(
        static_cast<bool (CharBody::*)(TaskType, RootObject*)>(
            &CharBody::_NV_setCurrentAction));
    g_addGoalFn = (AddGoalFn)KenshiLib::GetRealAddress(&Character::addGoal);
    g_addOrderFn = (AddOrderFn)KenshiLib::GetRealAddress(&Character::addOrder);
    g_addJobFn   = (AddJobFn)KenshiLib::GetRealAddress(&Character::addJob);
    g_separateSquadFn = (SeparateSquadFn)KenshiLib::GetRealAddress(
        &Character::separateIntoMyOwnSquad);
    g_endActionFn = (EndActionFn)KenshiLib::GetRealAddress(&CharBody::endAction);

    // AI-gating probe lever (non-fatal: only used when the probe is enabled).
    g_recruitFn = (RecruitFn)KenshiLib::GetRealAddress(
        static_cast<bool (PlayerInterface::*)(Character*, bool)>(&PlayerInterface::recruit));

    // Test-scene spawn fns (non-fatal: only used by the host-side setup step).
    g_createCharFn = (CreateCharFn)KenshiLib::GetRealAddress(
        &RootObjectFactory::createRandomCharacter);
    g_createBldgFn = (CreateBuildingFn)KenshiLib::GetRealAddress(
        &RootObjectFactory::createBuilding);
    g_getDataOfTypeFn = (GetDataOfTypeFn)KenshiLib::GetRealAddress(
        &GameDataContainer::getDataOfType);

    // Honest pose oracle reads (non-fatal).
    g_getBip01Fn   = (GetBip01Fn)KenshiLib::GetRealAddress(&Character::getPositionBip01);
    g_getBoneWorldFn = (GetBoneWorldPosFn)KenshiLib::GetRealAddress(&Character::getBoneWorldPosition);
    g_isIdleFn     = (CharBodyBoolFn)KenshiLib::GetRealAddress(&CharBody::isIdle);
    g_isCrouchedFn = (CharBodyBoolFn)KenshiLib::GetRealAddress(&CharBody::isCrouched);
}

bool gameplayLive(GameWorld* gw) {
    __try {
        return gw && gw->player && gw->player->playerCharacters.size() > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool loadSave(const std::string& name) {
    if (!g_getFn || !g_loadFn) return false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        g_loadFn(mgr, &name); // deferred: sets LOADGAME, engine loads a few frames later
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool savesReady() {
    if (!g_getFn || !g_savesExistFn) return true; // unresolved: don't block auto-load
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        return g_savesExistFn(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Entity capture / resolve / apply --------------------------------------

namespace {
// Anchored rest poses worth reproducing: the body stays put AT a fixture (a stool,
// throne, bed, machine), so committing the same task on the join seats/poses it in
// place. We deliberately EXCLUDE movement tasks (WANDER_TOWN, GO_TO_THE_BAR...) and
// plain standing (STAND_STILL/IDLE): reproducing a wander task walks the body away
// (tens of metres of drift), and a standing pose is visually identical to a park.
bool isReproduciblePose(int t) {
    switch (t) {
        case SIT_AROUND:
        case SIT_ON_THRONE:
        case REST:
        case RELAX_IN_TOWN_PACKAGE:
        case USE_BED:
        case USE_BED_ORDER:
        case SLEEP_ON_FLOOR:
        case OPERATE_MACHINERY:
            return true;
        default:
            return false;
    }
}

// Node-anchored rest poses: the body sits/idles AT an AI node. The node subject is
// not a resolvable RootObject, so applyTask cannot reproduce it - only the body's
// own local AI can, by executing the node. Used to decide NOT to suspend/park these.
bool isNodeAnchoredPoseImpl(int t) {
    switch (t) {
        case STAND_AT_NODE:
        case STAND_AT_SHOPKEEPER_NODE:
            return true;
        default:
            return false;
    }
}

// DEBUG (host-side): log each distinct task key seen among captured bodies once,
// with whether we treat it as a reproducible rest pose. Reveals exactly which
// tasks the bar's seated NPCs use so the allowlist can be widened. Lives outside
// any __try so the std::set's allocations don't violate MSVC's SEH/unwind rule.
void logTaskKeyOnce(int k, bool hasSubject) {
    static std::set<int> seen;
    if (seen.insert(k).second) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[taskkey] key=%d repro=%d subject=%d",
                  k, isReproduciblePose(k) ? 1 : 0, hasSubject ? 1 : 0);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// DIAGNOSTIC: resolve a subject (seat) hand to its world position. POD-only locals
// in its own SEH frame (no C++ unwinding objects) so a bad handle degrades to false.
bool resolveSubjectPos(u32 idx, u32 ser, u32 type, u32 cont, u32 contSer,
                       float* x, float* y, float* z) {
    if (!g_handGetRootFn || !g_handCtorFn) return false;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, idx, ser, (itemType)type, cont, contSer);
        RootObject* r = g_handGetRootFn(h);
        if (!r) return false;
        Ogre::Vector3 p = r->getPosition();
        *x = p.x; *y = p.y; *z = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DIAGNOSTIC (host-side): once per (npc,seat) pair, log where THIS client resolves
// the seat handle vs where the NPC actually is. Comparing the host's and join's
// "[seatres]" lines for the same seat handle answers whether furniture identity
// correlates across clients (same seatpos) or not (different seatpos).
void logSeatResolveOnce(const char* side, u32 npcIdx, u32 npcSer,
                        u32 sIdx, u32 sSer, u32 sType, u32 sCont, u32 sContSer,
                        float npx, float npy, float npz) {
    static std::set<std::pair<u32, u32> > seen;
    if (!seen.insert(std::make_pair(npcIdx, sIdx)).second) return;
    float sx = 0, sy = 0, sz = 0;
    bool ok = resolveSubjectPos(sIdx, sSer, sType, sCont, sContSer, &sx, &sy, &sz);
    float dx = sx - npx, dz = sz - npz;
    float d = ok ? (float)sqrt((double)(dx * dx + dz * dz)) : -1.0f;
    char b[224];
    _snprintf(b, sizeof(b) - 1,
              "[seatres] %s npc=%u,%u seat=%u,%u ok=%d npcpos=%.1f,%.1f seatpos=%.1f,%.1f d=%.1f",
              side, npcIdx, npcSer, sIdx, sSer, ok ? 1 : 0, npx, npz, sx, sz, d);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// SEH-guarded capture of a single Character into an EntityState. Kept in its own
// __try frame (no C++ unwinding objects) so a bad pointer degrades to a skip.
bool captureOne(Character* c, EntityState* e) {
    __try {
        const hand& h = c->handle;
        e->hType            = (u32)h.type;
        e->hContainer       = h.container;
        e->hContainerSerial = h.containerSerial;
        e->hIndex           = h.index;
        e->hSerial          = h.serial;

        Ogre::Vector3 p = c->getPosition();
        e->x = p.x; e->y = p.y; e->z = p.z;
        e->heading = c->getOrientation().getYaw().valueRadians();

        CharMovement* mv = c->movement;
        if (mv) {
            e->cSpeed   = mv->currentSpeed;
            e->cMotionX = mv->currentMotion.x;
            e->cMotionY = mv->currentMotion.y;
            e->cMotionZ = mv->currentMotion.z;
            e->cMoving  = mv->currentlyMoving ? 1 : 0;
        } else {
            e->cSpeed = 0; e->cMotionX = e->cMotionY = e->cMotionZ = 0; e->cMoving = 0;
        }
        // Stage 5 pose: capture the current task + the object it targets, so the
        // receiver can adopt the same pose at the same fixture (sit/operate). No
        // current action -> TASK_NONE (the receiver idle-parks).
        e->task = TASK_NONE;
        e->rawTask = TASK_NONE;
        e->sType = e->sContainer = e->sContainerSerial = e->sIndex = e->sSerial = 0;
        if (g_taskerKeyFn) {
            CharBody* b = c->body;
            Tasker* t = b ? b->currentAction : 0;
            if (t) {
                int k = g_taskerKeyFn(t);
                e->rawTask = (u16)k; // diagnostic: stream the raw key for divergence checks
                logTaskKeyOnce(k, t->subject.index != 0 || t->subject.serial != 0);
                // Only stream anchored rest poses; everything else stays TASK_NONE
                // so the receiver parks instead of reproducing a moving task.
                if (isReproduciblePose(k)) {
                    e->task = (u16)k;
                    const hand& s = t->subject;
                    e->sType            = (u32)s.type;
                    e->sContainer       = s.container;
                    e->sContainerSerial = s.containerSerial;
                    e->sIndex           = s.index;
                    e->sSerial          = s.serial;
                    // DIAGNOSTIC: where does THIS (host) client resolve the seat?
                    logSeatResolveOnce("HOST", e->hIndex, e->hSerial,
                                       e->sIndex, e->sSerial, e->sType,
                                       e->sContainer, e->sContainerSerial,
                                       e->x, e->y, e->z);
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

unsigned int captureSquad(GameWorld* gw, bool leaderOnly,
                          EntityState* out, unsigned int maxOut) {
    if (!gw || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        if (!gw->player) return 0;
        unsigned int size = (unsigned int)gw->player->playerCharacters.size();
        for (unsigned int i = 0; i < size && n < maxOut; ++i) {
            Character* c = gw->player->playerCharacters[i];
            if (!c) continue;
            // captureOne has its own __try; calling it here is fine.
            if (captureOne(c, &out[n])) ++n;
            if (leaderOnly) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

Character* resolve(const EntityState& e) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.hIndex, e.hSerial, (itemType)e.hType,
                     e.hContainer, e.hContainerSerial);
        return g_handGetCharFn(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool applyRaw(Character* c, const EntityState& e) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        Ogre::Vector3 pos(e.x, e.y, e.z);
        Ogre::Quaternion rot(Ogre::Radian(e.heading), Ogre::Vector3::UNIT_Y);
        mv->_setPositionDirectionAndTeleport(pos, rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readPos(Character* c, float* x, float* y, float* z) {
    if (!c) return false;
    __try {
        Ogre::Vector3 p = c->getPosition();
        if (x) *x = p.x; if (y) *y = p.y; if (z) *z = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readHand(Character* c, unsigned int out[5]) {
    if (!c) return false;
    __try {
        const hand& h = c->handle;
        out[0] = h.index;
        out[1] = h.serial;
        out[2] = (unsigned int)h.type;
        out[3] = h.container;
        out[4] = h.containerSerial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool orderMoveTo(Character* c, float x, float y, float z) {
    if (!c) return false;
    __try {
        Ogre::Vector3 dest(x, y, z);
        // Prefer the player move-order path (moves a player-controlled leader).
        if (g_charSetDestFn) {
            g_charSetDestFn(c, &dest, false);
            return true;
        }
        // Fallback: drive the movement controller directly (AI/proxy bodies).
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->setDesiredSpeed(RUN);
        mv->setDestination(dest, HIGH_PRIORITY, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool walkTo(Character* c, float x, float y, float z, float speed) {
    if (!c) return false;
    __try {
        Ogre::Vector3 dest(x, y, z);
        // Dual-path so ONE call drives both kinds of body:
        //   * player-controlled (squad): the player move-order path; a player char
        //     ignores a bare CharMovement::setDestination (proved in Stage 1).
        //   * AI-controlled (NPC): a CharMovement HIGH_PRIORITY destination, which
        //     overrides the NPC's autonomous movement goals (proved in the monolith).
        // Issuing both is safe: each body obeys the one that applies to it.
        if (g_charSetDestFn) g_charSetDestFn(c, &dest, false);

        CharMovement* mv = c->movement;
        if (mv) {
            mv->setDestination(dest, HIGH_PRIORITY, false);
            float s = speed;
            if (s < 1.0f) s = (float)RUN; // unknown/tiny: default to a run pace
            mv->setDesiredSpeed(s);        // override the order's default speed (catch-up)
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool park(Character* c, float x, float y, float z, float heading) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot(Ogre::Radian(heading), Ogre::Vector3::UNIT_Y);
        mv->halt(); // clean stop (resets path AND clip phase - only at settle)
        mv->_setPositionDirectionAndTeleport(pos, rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyMotion(Character* c, bool moving, float speed, float mx, float my, float mz) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->currentlyMoving = moving;
        mv->currentSpeed    = speed;
        mv->desiredSpeed    = speed; // keep accel logic from re-deciding to idle
        mv->currentMotion   = Ogre::Vector3(mx, my, mz);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readMotion(Character* c, bool* moving, float* speed) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        if (moving) *moving = mv->currentlyMoving;
        if (speed)  *speed  = mv->currentSpeed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int readTaskKey(Character* c) {
    if (!c || !g_taskerKeyFn) return -1;
    __try {
        CharBody* b = c->body;
        Tasker* t = b ? b->currentAction : 0;
        if (!t) return (int)TASK_NONE;
        return g_taskerKeyFn(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool isNodeAnchoredPose(int taskKey) { return isNodeAnchoredPoseImpl(taskKey); }

bool recruitNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_recruitFn) return false;
    __try {
        if (!gw->player) return false;
        return g_recruitFn(gw->player, c, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool installAiSuspendHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&Character::_NV_periodicUpdate);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&periodicUpdate_hook,
                              (void**)&g_periodicOrig) == KenshiLib::SUCCESS;
}

void clearAiSuspend()           { g_aiSuspended.clear(); }
void addAiSuspend(Character* c) { if (c) g_aiSuspended.insert(c); }
unsigned int aiSuspendCount()   { return (unsigned int)g_aiSuspended.size(); }

Character* leader(GameWorld* gw) {
    if (!gw) return 0;
    __try {
        if (!gw->player || gw->player->playerCharacters.size() == 0) return 0;
        return gw->player->playerCharacters[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

namespace {
// True if 'obj' is one of the local player's squad members (we never stream our
// own controllable squad as a host NPC). Caller holds the SEH frame.
bool isPlayerSquad(GameWorld* gw, RootObject* obj) {
    PlayerInterface* pl = gw->player;
    if (!pl) return false;
    unsigned int pc = (unsigned int)pl->playerCharacters.size();
    for (unsigned int j = 0; j < pc; ++j) {
        if (static_cast<RootObject*>(pl->playerCharacters[j]) == obj) return true;
    }
    return false;
}
} // namespace

unsigned int captureNpcs(GameWorld* gw, EntityState* out, unsigned int maxOut) {
    if (!g_getCharsFn || !gw || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        PlayerInterface* pl = gw->player;
        if (!pl || pl->playerCharacters.size() == 0) return 0;

        // Interest center: the local player's leader. The query radii approximate
        // a town-block footprint (~200u far) so the bar's NPCs are all captured.
        Ogre::Vector3 center = pl->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 200.0f, 120.0f, 30.0f, 96, 96, 0);

        unsigned int total = g_npcQuery.size();
        for (unsigned int i = 0; i < total && n < maxOut; ++i) {
            RootObject* obj = g_npcQuery[i];
            if (!obj) continue;
            if (isPlayerSquad(gw, obj)) continue; // never stream our own squad here
            // getCharactersWithinSphere returns Characters as RootObject* bases.
            if (captureOne(static_cast<Character*>(obj), &out[n])) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

void clearGoals(Character* c) {
    if (!c || !g_clearGoalsFn) return;
    __try {
        g_clearGoalsFn(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool isLocalPlayerChar(GameWorld* gw, Character* c) {
    if (!gw || !c) return false;
    __try {
        return isPlayerSquad(gw, static_cast<RootObject*>(c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool suppressNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_removeUpdateFn) return false;
    __try {
        g_removeUpdateFn(gw, c);
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        // Freezing alone leaves the body standing/visible at its seat; hide it too
        // so a host-unstreamed NPC fully disappears (no standing-on-the-seat double).
        static_cast<RootObject*>(c)->setVisible(false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void restoreNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_addUpdateFn) return;
    __try {
        g_addUpdateFn(gw, c);
        static_cast<RootObject*>(c)->setVisible(true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Stage 6 host authority: enumerate nearby WORLD NPCs (excluding our own squad),
// yielding both the live Character* and its hand-bearing EntityState so the join
// can decide which are NOT in the host's streamed set and suppress them. Mirrors
// captureNpcs' interest query so the join's local set matches the host's.
unsigned int listNpcs(GameWorld* gw, Character** outChars, EntityState* outStates,
                      unsigned int maxOut) {
    if (!g_getCharsFn || !gw || !outChars || !outStates || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        PlayerInterface* pl = gw->player;
        if (!pl || pl->playerCharacters.size() == 0) return 0;
        Ogre::Vector3 center = pl->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 200.0f, 120.0f, 30.0f, 96, 96, 0);
        unsigned int total = g_npcQuery.size();
        for (unsigned int i = 0; i < total && n < maxOut; ++i) {
            RootObject* obj = g_npcQuery[i];
            if (!obj) continue;
            if (isPlayerSquad(gw, obj)) continue; // never suppress our own squad
            Character* ch = static_cast<Character*>(obj);
            if (captureOne(ch, &outStates[n])) { outChars[n] = ch; ++n; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

namespace {
// Case-insensitive substring test on raw C strings (no C++ temporaries -> SEH
// legal). Returns true if 'needle' appears anywhere in 'hay'.
bool ciContains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h; const char* b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// Find a furniture BUILDING template that is actually a SEAT. Caller holds SEH.
// Priority avoids matching crafting stations ("Engineering Bench" etc.): we want
// stools/chairs/thrones, NOT generic "bench"/"seat" substrings.
GameData* findSeatTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    // Ordered keyword preference; first present keyword that any template matches.
    const char* prefs[] = { "bar stool", "stool", "chair", "throne" };
    for (unsigned int k = 0; k < 4; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Compute a world point 'fwd' metres ahead of the leader's facing and 'side' to
// its right, plus the leader's yaw. Caller holds SEH.
bool leaderAnchor(GameWorld* gw, float fwd, float side,
                  Ogre::Vector3* outPos, float* outYaw) {
    if (!gw || !gw->player || gw->player->playerCharacters.size() == 0) return false;
    Character* ld = gw->player->playerCharacters[0];
    if (!ld) return false;
    Ogre::Vector3 p = ld->getPosition();
    float yaw = ld->getOrientation().getYaw().valueRadians();
    // Kenshi faces -Z at yaw 0; forward = (sin yaw, 0, cos yaw) is a good-enough
    // "ahead of the character" for placing a prop the user then fine-tunes.
    float fx = (float)sin((double)yaw), fz = (float)cos((double)yaw);
    float rx = fz, rz = -fx; // right = forward rotated -90deg about Y
    outPos->x = p.x + fx * fwd + rx * side;
    outPos->y = p.y; // character ground Y; building placement re-grounds via terrain
    outPos->z = p.z + fz * fwd + rz * side;
    if (outYaw) *outYaw = yaw;
    return true;
}
} // namespace

bool readObjectHand(RootObject* obj, unsigned int out[5]) {
    if (!obj) return false;
    __try {
        const hand& h = obj->handle;
        out[0] = (unsigned int)h.type;
        out[1] = h.container;
        out[2] = h.containerSerial;
        out[3] = h.index;
        out[4] = h.serial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool spawnSeatInFront(GameWorld* gw, float fwd, float side, RootObject** spawned) {
    if (spawned) *spawned = 0;
    if (!gw || !gw->theFactory || !g_createBldgFn) {
        coop::logLine("SETUP: seat spawn skipped (no factory / createBuilding fn)");
        return false;
    }
    __try {
        unsigned int nTemplates = 0;
        if (g_getDataOfTypeFn) {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
            nTemplates = g_dataScratch.size();
        }
        GameData* tmpl = findSeatTemplate(gw);
        {
            char d[200];
            _snprintf(d, sizeof(d) - 1, "SETUP: BUILDING templates=%u seatTemplate='%s'",
                      nTemplates, tmpl ? tmpl->name.c_str() : "(none)");
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (!tmpl) return false;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return false;
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat reqPos=%.2f,%.2f,%.2f yaw=%.2f",
                      pos.x, pos.y, pos.z, yaw);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        // Face the seat back toward the leader so a seated body looks outward.
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        // createBuilding adds the terrain height to position.y (a seat passed at the
        // absolute ground Y floats ~terrain-height up). Pass y=0 so it re-grounds.
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z);
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/0, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) { coop::logLine("SETUP: createBuilding returned null"); return false; }
        // Building's first base is RootObject (offset 0), so this reinterpret is a
        // no-op address-wise - lets us avoid pulling in the heavy Building.h.
        RootObject* ro = reinterpret_cast<RootObject*>(b);
        Ogre::Vector3 ap = ro->getPosition(); // virtual: where it actually landed
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat actualPos=%.2f,%.2f,%.2f visible=%d",
                      ap.x, ap.y, ap.z, ro->getVisible() ? 1 : 0);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (spawned) *spawned = ro;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP: createBuilding FAULTED");
        return false;
    }
}

Character* spawnNpcInFront(GameWorld* gw, float fwd, float side) {
    if (!gw || !gw->theFactory || !g_createCharFn) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        Faction* fac = ld->getFaction();
        GameData* tmpl = ld->getGameData();
        if (!fac || !tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        // owner=0: createRandomCharacter makes its own container; NOT enlisted in
        // the player squad, so the receiver drives it via the NPC path.
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// SEH-guarded bone read. MUST live in its own function with only POD locals: a
// __try cannot coexist with C++ objects that need unwinding (the std::string for
// the bone name is therefore owned by the caller and passed by pointer). Returns
// the pelvis-bone height above the logical root, or false on fault.
static bool readPelvisDelta(Character* c, const std::string* boneName, float* outH) {
    __try {
        Ogre::Vector3 ground = c->getPosition();
        Ogre::Vector3 bone(0, 0, 0);
        g_getBoneWorldFn(c, &bone, boneName);   // this=RCX, retbuf=RDX, name=R8
        *outH = bone.y - ground.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool readCharBodyFlags(Character* c, int* idle, int* crouched, int* task) {
    __try {
        CharBody* b = c->body;
        if (!b) return false;
        if (idle && g_isIdleFn)         *idle     = g_isIdleFn(b) ? 1 : 0;
        if (crouched && g_isCrouchedFn) *crouched = g_isCrouchedFn(b) ? 1 : 0;
        if (task && g_taskerKeyFn) {
            Tasker* t = b->currentAction;
            if (t) *task = g_taskerKeyFn(t);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readPoseState(Character* c, float* pelvis, int* idle, int* crouched, int* task) {
    if (pelvis) *pelvis = -1.0f;
    if (idle) *idle = -1; if (crouched) *crouched = -1; if (task) *task = TASK_NONE;
    if (!c) return false;
    bool any = false;
    // Safe reads (crouch/idle/task) - isolated so a pelvis fault cannot wipe them.
    if (readCharBodyFlags(c, idle, crouched, task)) any = true;
    // Pelvis bone height above root - the animated-skeleton signal that drops when
    // seated. The std::string lives HERE (outside the SEH frame in the helper).
    if (g_getBoneWorldFn && pelvis) {
        std::string boneName("Bip01 Pelvis");
        float h = 0.0f;
        if (readPelvisDelta(c, &boneName, &h)) {
            // pelvis-above-root, in Kenshi units. Magnitude varies a LOT by race
            // (Greenlander/Shek/Hiver/Skeleton heights differ), so the oracle
            // compares the SAME NPC host-vs-join rather than an absolute seat line.
            // Keep any plausibly-real value; -99 flags a clearly-bad read.
            *pelvis = (h > 0.5f && h < 25.0f) ? h : -99.0f;
            any = true;
        }
    }
    return any;
}

// Max horizontal gap (m) between the host's streamed transform and a resolved seat
// fixture for that seat to be accepted as the correct one. A correct seat is right
// under the NPC (<~4 m); a mis-resolved cross-client seat is tens of metres away.
static const float SEAT_MATCH_DIST = 6.0f;

int applyTask(Character* c, const EntityState& e) {
    if (e.task == TASK_NONE) return 0;
    if (!c || !g_handGetRootFn || !g_handCtorFn) return 0;
    if (!g_addGoalFn && !g_setActionFn) return 0;
    __try {
        // Build the subject hand and resolve it to a local fixture.
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.sIndex, e.sSerial, (itemType)e.sType,
                     e.sContainer, e.sContainerSerial);
        RootObject* target = g_handGetRootFn(h);
        // DIAGNOSTIC: where does THIS (join) client resolve the same seat handle?
        // Compare the "[seatres] JOIN" seatpos to the host's for the same seat to
        // see whether furniture identity correlates across clients.
        logSeatResolveOnce("JOIN", e.hIndex, e.hSerial, e.sIndex, e.sSerial,
                           e.sType, e.sContainer, e.sContainerSerial, e.x, e.y, e.z);
        if (!target) return 1; // fixture not loaded here -> caller idle-parks
        // PROXIMITY GATE: cross-client furniture identity is NOT reliable - the same
        // subject handle frequently resolves to a DIFFERENT stool tens of metres away
        // on the join. Issuing the seat goal then walks the body ~50 m to that wrong
        // stool while our position-drive teleports it back => the "walking in place,
        // repeatedly teleported" loop. So verify the resolved fixture is actually at
        // the host's streamed transform before committing; a far match is rejected so
        // the caller idle-parks in place (no walk, no loop) instead.
        {
            Ogre::Vector3 sp = target->getPosition(); // virtual: safe direct call
            float dx = sp.x - e.x, dz = sp.z - e.z;
            if ((dx * dx + dz * dz) > (SEAT_MATCH_DIST * SEAT_MATCH_DIST))
                return 3; // fixture resolved but it's the WRONG (far) one -> park
        }
        // Clear any local intent first, then issue a PERSISTENT AI goal to perform
        // the task AT the resolved fixture. addGoal is the most stable mechanism
        // tried: it sits ~88% of NPCs correctly. SIT_AROUND does not hard-pin our
        // target seat (it re-runs a local seat search), so a few NPCs still settle
        // on a different free stool - that residual is the open problem. (Tried and
        // rejected: setCurrentAction-primary made MORE NPCs wander; snap-then-pose
        // crashed and still wandered.)
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        if (g_addGoalFn) {
            g_addGoalFn(c, (int)e.task, target);
        } else if (g_setActionFn && c->body) {
            g_setActionFn(c->body, (int)e.task, target);
        }
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// I9: detach an NPC from its town/faction so the town-AI stops auto-assigning it
// tasks - a "squad-like" inert puppet whose only intent source is the host order.
// separateIntoMyOwnSquad(true) ejects it into its own platoon. SEH-guarded; the
// caller invokes this once per driven NPC. Returns true if the call was made.
bool detachFromTownAI(Character* c) {
    if (!c || !g_separateSquadFn) return false;
    __try {
        g_separateSquadFn(c, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// I10: end the body's current action so a suspended node-stander stops executing
// its residual walk-to-node (the "walk in place" when we hold it at the host
// transform) and drops to idle. SEH-guarded; returns true if the call was made.
bool endAction(Character* c) {
    if (!c || !g_endActionFn || !c->body) return false;
    __try {
        g_endActionFn(c->body);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// I9: reproduce a rest pose via the PLAYER-ORDER path instead of the autonomous
// SIT_AROUND goal. addOrder/addJob carry an explicit world LOCATION, so the body
// is ordered to THIS fixture at THIS spot (what a player click-to-sit issues) -
// the engine does not re-run its own seat search and wander to a different stool.
// Same resolution + proximity gate + return codes as applyTask.
int applyTaskOrder(Character* c, const EntityState& e) {
    if (e.task == TASK_NONE) return 0;
    if (!c || !g_handGetRootFn || !g_handCtorFn) return 0;
    if (!g_addOrderFn && !g_addJobFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.sIndex, e.sSerial, (itemType)e.sType,
                     e.sContainer, e.sContainerSerial);
        RootObject* target = g_handGetRootFn(h);
        if (!target) return 1; // fixture not loaded here -> caller idle-parks
        Ogre::Vector3 loc = target->getPosition(); // virtual: safe direct call
        {
            float dx = loc.x - e.x, dz = loc.z - e.z;
            if ((dx * dx + dz * dz) > (SEAT_MATCH_DIST * SEAT_MATCH_DIST))
                return 3; // resolved the WRONG (far) fixture -> caller parks in place
        }
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        // Player-order to the EXACT seat fixture + location. A stool is a Building,
        // so it doubles as the order destination. clear=true drops prior orders.
        if (g_addOrderFn) {
            Building* dest = reinterpret_cast<Building*>(target);
            g_addOrderFn(c, dest, (int)e.task, target, /*shift*/false,
                         /*clear*/true, &loc);
        } else if (g_addJobFn) {
            g_addJobFn(c, (int)e.task, target, /*shift*/false,
                       /*addDontClear*/false, &loc);
        }
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace engine
} // namespace coop
