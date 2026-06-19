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
// getObjectsWithinSphere fills a lektor with nearby objects of a given itemType
// (BUILDING etc.). Used by craft re-arm to find a baked work fixture by SEARCH,
// so a reloaded scene needs no sidecar to relocate the dummy/machine.
typedef void (__fastcall* GetObjsInSphereFn)(
    GameWorld* self, lektor<RootObject*>* results, const Ogre::Vector3* pos,
    float radius, itemType type, int maxNumber, RootObject* skip);
typedef void (__fastcall* ClearGoalsFn)(Character* self);
typedef void (__fastcall* UpdateListFn)(GameWorld* self, Character* c);

GetCharsInSphereFn g_getCharsFn    = 0;
GetObjsInSphereFn  g_getObjsFn     = 0;
ClearGoalsFn       g_clearGoalsFn  = 0;
UpdateListFn       g_removeUpdateFn = 0;
UpdateListFn       g_addUpdateFn    = 0;

// Stage 5 rest-pose reproduction. Tasker::key() reads the current TaskType;
// hand::getRootObject resolves a task's subject hand to its world fixture;
// CharBody::_NV_setCurrentAction(TaskType,target) commits the body to that task.
typedef int         (__fastcall* TaskerKeyFn)(const void* self);
// Tasker::getDescription() -> const std::string&. A member returning a reference
// puts `this` in RCX and returns the referent's address in RAX, so model it as a
// pointer-returning fn. Used purely as a diagnostic to self-document task keys.
typedef const std::string* (__fastcall* TaskerDescFn)(const void* self);
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
// Character::ragdollMode(bool on, RagdollPart::Enum part): drop the body into (or
// out of) ragdoll. Used to manufacture/reproduce a "down" body for Stage 2.
typedef void      (__fastcall* RagdollModeFn)(Character* self, bool on, int part);
// MedicalSystem::knockout(skill01) / knockoutForceTimer(seconds): a REAL knockout.
// Unlike a bare ragdoll (which a healthy loaded NPC instantly fights out of), the KO
// timer collapses the body AND holds it down, so it stays cleanly on the ground.
typedef void      (__fastcall* MedFloatFn)(MedicalSystem* self, float v);

TaskerKeyFn   g_taskerKeyFn   = 0;
TaskerDescFn  g_taskerDescFn  = 0;
HandGetRootFn g_handGetRootFn = 0;
SetActionFn   g_setActionFn   = 0;
AddGoalFn     g_addGoalFn     = 0;
AddOrderFn      g_addOrderFn      = 0;
AddJobFn        g_addJobFn        = 0;
SeparateSquadFn g_separateSquadFn = 0;
EndActionFn     g_endActionFn     = 0;
RagdollModeFn   g_ragdollModeFn   = 0;
MedFloatFn      g_knockoutFn      = 0;
MedFloatFn      g_knockoutForceFn = 0;

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
// Body-state reads (Stage 2): direct non-virtual bool getters on Character, same
// thiscall ABI as the CharBody getters (this=RCX, returns bool), so we reuse the
// CharBodyBoolFn type and pass the Character* as self.
CharBodyBoolFn   g_isDownFn        = 0;
CharBodyBoolFn   g_isRagdollFn     = 0;
CharBodyBoolFn   g_isDeadCharFn    = 0;
CharBodyBoolFn   g_isCrawlFn       = 0;

// Combat reads (L5 probe, Phase 3c). getAttackTarget returns a `hand` BY VALUE, so
// it uses the same member-struct-return ABI as getBoneWorldPosition: this=RCX,
// hidden return-buffer pointer=RDX (model it as `hand* fn(Character*, hand*)`).
// isInCombatMode(melee, ranged) takes two bools; the remaining flags are bare bool
// getters that reuse CharBodyBoolFn (this=RCX, returns bool).
typedef hand* (__fastcall* GetAttackTargetFn)(Character* self, hand* ret);
typedef bool  (__fastcall* InCombatModeFn)(Character* self, bool melee, bool ranged);
GetAttackTargetFn g_getAttackTargetFn = 0;
InCombatModeFn    g_inCombatModeFn    = 0;
CharBodyBoolFn    g_inRangedModeFn    = 0;
CharBodyBoolFn    g_underMeleeFn      = 0;
CharBodyBoolFn    g_fleeingFn         = 0;

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
    g_getObjsFn = (GetObjsInSphereFn)KenshiLib::GetRealAddress(
        &GameWorld::getObjectsWithinSphere);
    g_clearGoalsFn = (ClearGoalsFn)KenshiLib::GetRealAddress(&Character::clearAllAIGoals);
    g_removeUpdateFn = (UpdateListFn)KenshiLib::GetRealAddress(&GameWorld::removeFromUpdateListMain);
    g_addUpdateFn    = (UpdateListFn)KenshiLib::GetRealAddress(&GameWorld::addToUpdateListMain);
    if (!g_getCharsFn)
        coop::logErrLine("engine: could not resolve getCharactersWithinSphere (NPC stream off)");

    // Stage 5 pose reproduction. Non-fatal: if unresolved, rest NPCs idle-park.
    g_taskerKeyFn = (TaskerKeyFn)KenshiLib::GetRealAddress(&Tasker::key);
    g_taskerDescFn = (TaskerDescFn)KenshiLib::GetRealAddress(&Tasker::getDescription);
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
    g_ragdollModeFn = (RagdollModeFn)KenshiLib::GetRealAddress(&Character::ragdollMode);
    g_knockoutFn      = (MedFloatFn)KenshiLib::GetRealAddress(&MedicalSystem::knockout);
    g_knockoutForceFn = (MedFloatFn)KenshiLib::GetRealAddress(&MedicalSystem::knockoutForceTimer);

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

    // Body-state reads (non-fatal: unresolved -> bodyState stays 0 = upright).
    g_isDownFn     = (CharBodyBoolFn)KenshiLib::GetRealAddress(&Character::isDown);
    g_isRagdollFn  = (CharBodyBoolFn)KenshiLib::GetRealAddress(&Character::isRagdoll);
    g_isDeadCharFn = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        static_cast<bool (Character::*)() const>(&Character::isDead));
    g_isCrawlFn    = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        &Character::isStealthModeOrCrawling);

    // Combat reads (Phase 3c probe; non-fatal: unresolved -> zeros).
    g_getAttackTargetFn = (GetAttackTargetFn)KenshiLib::GetRealAddress(
        &Character::getAttackTarget);
    g_inCombatModeFn = (InCombatModeFn)KenshiLib::GetRealAddress(&Character::isInCombatMode);
    g_inRangedModeFn = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        &Character::isInRangedCombatMode);
    g_underMeleeFn   = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        &Character::isLiterallyUnderMeleeAttackRightNowForSure);
    g_fleeingFn      = (CharBodyBoolFn)KenshiLib::GetRealAddress(&Character::isFleeing);
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
        // Crafting / gathering / work poses (Stage 3a). All of these pin the body
        // AT a work fixture whose subject hand resolves cross-client, exactly like
        // sitting, so the player-order path (applyTaskOrder) reproduces them in
        // place. Mining drills, farm plots, research benches and smithies all run
        // through OPERATE_MACHINERY; the others cover automatic machines, training
        // dummies and the ambient "pretend to work" town pose.
        case OPERATE_MACHINERY:
        case OPERATE_AUTOMATIC_MACHINERY:
        case USE_TRAINING_DUMMY:
        case PRETEND_TO_OPERATE_MACHINERY:
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
void logTaskKeyOnce(int k, bool hasSubject, const char* desc) {
    static std::set<int> seen;
    if (seen.insert(k).second) {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[taskkey] key=%d desc='%s' repro=%d subject=%d",
                  k, (desc && desc[0]) ? desc : "?",
                  isReproduciblePose(k) ? 1 : 0, hasSubject ? 1 : 0);
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

// DIAGNOSTIC (host-side): once per (npc,fixture) pair, log where THIS client
// resolves the subject handle (seat, machine, dummy, bed...) vs where the NPC
// actually is. Comparing the host's and join's "[seatres]" lines for the same
// handle answers whether the fixture identity correlates across clients (same
// pos) or not. The task= field distinguishes seated poses from crafting/gathering
// work stations (OPERATE_MACHINERY etc.), so the same line doubles as the
// craft-subject ([craftres]) diagnostic for Stage 3a.
void logSeatResolveOnce(const char* side, int task, u32 npcIdx, u32 npcSer,
                        u32 sIdx, u32 sSer, u32 sType, u32 sCont, u32 sContSer,
                        float npx, float npy, float npz) {
    static std::set<std::pair<u32, u32> > seen;
    if (!seen.insert(std::make_pair(npcIdx, sIdx)).second) return;
    float sx = 0, sy = 0, sz = 0;
    bool ok = resolveSubjectPos(sIdx, sSer, sType, sCont, sContSer, &sx, &sy, &sz);
    float dx = sx - npx, dz = sz - npz;
    float d = ok ? (float)sqrt((double)(dx * dx + dz * dz)) : -1.0f;
    char b[240];
    _snprintf(b, sizeof(b) - 1,
              "[seatres] %s task=%d npc=%u,%u subj=%u,%u ok=%d npcpos=%.1f,%.1f subjpos=%.1f,%.1f d=%.1f",
              side, task, npcIdx, npcSer, sIdx, sSer, ok ? 1 : 0, npx, npz, sx, sz, d);
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
                const char* desc = 0;
                if (g_taskerDescFn) {
                    const std::string* ds = g_taskerDescFn(t);
                    if (ds) desc = ds->c_str();
                }
                logTaskKeyOnce(k, t->subject.index != 0 || t->subject.serial != 0, desc);
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
                    // DIAGNOSTIC: where does THIS (host) client resolve the fixture?
                    logSeatResolveOnce("HOST", k, e->hIndex, e->hSerial,
                                       e->sIndex, e->sSerial, e->sType,
                                       e->sContainer, e->sContainerSerial,
                                       e->x, e->y, e->z);
                }
            }
        }
        // Stage 2: body-state flags (down/KO/ragdoll/dead/crawl). 0 = upright. Read
        // last so a fault here can't lose the transform we already captured.
        e->bodyState = readBodyState(c);
        // Stage 3c combat: if the body is fighting a resolvable target, OVERRIDE the
        // pose task with the synthetic combat intent and stash the target's hand in the
        // subject fields. Combat outranks any rest pose (you can't sit and fight), so
        // this clobbers a sit/work task set above. The join reproduces the cause by
        // ordering its local copy to melee the same target. (Read inside this __try so
        // a combat-read fault can't lose the transform/body-state already captured.)
        {
            CombatRead cr;
            if (readCombat(c, &cr) && cr.inCombat && cr.hasTarget &&
                (cr.target[3] != 0 || cr.target[4] != 0)) {
                e->task = TASK_COMBAT_MELEE;
                e->sType            = cr.target[0];
                e->sContainer       = cr.target[1];
                e->sContainerSerial = cr.target[2];
                e->sIndex           = cr.target[3];
                e->sSerial          = cr.target[4];
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

// Resolve a Character* from raw hand fields (same path as resolve(EntityState)).
Character* resolveCharByHand(unsigned int idx, unsigned int ser, unsigned int type,
                             unsigned int cont, unsigned int contSer) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, idx, ser, (itemType)type, cont, contSer);
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

// Read a live NON-player Faction* off a nearby world NPC (the first non-squad
// character within the interest radius whose faction differs from the player's).
// This avoids FactionManager (no header): spawning into this faction yields a true
// world NPC that is NOT in the player squad, and owning the work fixture with the
// same faction gives that NPC a legitimate reason to operate it. Caller holds SEH.
// Returns 0 if no non-player NPC is nearby (e.g. an empty/blank save).
Faction* findNearbyNonPlayerFaction(GameWorld* gw) {
    if (!gw || !g_getCharsFn || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    Faction* playerFac = gw->player->getFaction();
    Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
    // Wide radius: we only need ANY loaded world NPC to read a faction pointer off,
    // not a close one. A blank-start save can sit just outside a town, so the bar
    // crowd is well beyond the 200u capture radius - reach the whole loaded block.
    g_npcQuery.clear();
    g_getCharsFn(gw, &g_npcQuery, &center, 6000.0f, 6000.0f, 6000.0f, 512, 512, 0);
    unsigned int total = g_npcQuery.size();
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SETUP: faction scan found %u loaded NPC(s) within 6000u", total);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    for (unsigned int i = 0; i < total; ++i) {
        RootObject* obj = g_npcQuery[i];
        if (!obj || isPlayerSquad(gw, obj)) continue;
        Faction* f = static_cast<Character*>(obj)->getFaction();
        if (f && f != playerFac) return f;
    }
    return 0;
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

// Find a furniture BUILDING template that is an OPERABLE work fixture an NPC can
// stand at and work (crafting/gathering class). Caller holds SEH. Ordered keyword
// preference: a training dummy is the most deterministic (no inputs/power/recipe -
// the user can just order "train" and the work pose plays), then common crafting
// machines. Mining/farming need terrain resources, so they're not spawned here.
GameData* findMachineTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* prefs[] = {
        "training dummy", "combat dummy", "punching bag", "research bench",
        "engineering bench", "weapon smithy", "spinning wheel", "loom"
    };
    const unsigned int nprefs = sizeof(prefs) / sizeof(prefs[0]);
    for (unsigned int k = 0; k < nprefs; ++k) {
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

// Read a character's CURRENT task key (TASK_NONE if idle / unreadable). Mirrors the
// capture path; used by re-arm to avoid re-issuing a goal a worker is already doing
// (clearAllAIGoals + addGoal every tick would thrash pathing and never animate).
int readCharTaskKey(Character* c) {
    if (!c || !g_taskerKeyFn) return TASK_NONE;
    __try {
        CharBody* b = c->body;
        Tasker* t = b ? b->currentAction : 0;
        if (!t) return TASK_NONE;
        return g_taskerKeyFn(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return TASK_NONE;
    }
}

// Find a BAKED work fixture (training dummy / crafting machine) near the leader by
// scanning loaded BUILDING objects (NOT templates). Returns the live fixture and the
// task to issue at it. This is how craft re-arm relocates the dummy after a reload
// without any sidecar - the save-stable building is simply searched for by name.
RootObject* findWorkFixtureNear(GameWorld* gw, int* outTask) {
    if (!gw || !g_getObjsFn || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, 60.0f, BUILDING, 256, 0);
        unsigned int total = g_npcQuery.size();
        const char* prefs[] = {
            "training dummy", "combat dummy", "punching bag", "research bench",
            "engineering bench", "weapon smithy", "spinning wheel", "loom"
        };
        const unsigned int nprefs = sizeof(prefs) / sizeof(prefs[0]);
        for (unsigned int k = 0; k < nprefs; ++k) {
            for (unsigned int i = 0; i < total; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                GameData* gd = o->getGameData();
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    if (outTask)
                        *outTask = (ciContains(gd->name.c_str(), "dummy") ||
                                    ciContains(gd->name.c_str(), "bag"))
                                       ? USE_TRAINING_DUMMY : OPERATE_MACHINERY;
                    return o;
                }
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Nearest NON-squad character to a fixture (the worker that should operate it).
Character* findWorkerNear(GameWorld* gw, RootObject* fixture) {
    if (!gw || !g_getCharsFn || !fixture || !gw->player) return 0;
    __try {
        Ogre::Vector3 at = fixture->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &at, 40.0f, 30.0f, 10.0f, 64, 64, 0);
        unsigned int total = g_npcQuery.size();
        Character* best = 0; float bestD2 = 1e18f;
        for (unsigned int i = 0; i < total; ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - at.x, dz = p.z - at.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; best = static_cast<Character*>(o); }
        }
        return best;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
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

// Spawn a world character in a GIVEN faction (not the player's), so it is NOT a
// squad member and flows through the host-authoritative world-NPC path. Body uses
// the leader's race template (a valid mesh); only the faction differs. SEH-guarded.
Character* spawnCharInFaction(GameWorld* gw, float fwd, float side, Faction* fac) {
    if (!gw || !gw->theFactory || !g_createCharFn || !fac) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        GameData* tmpl = ld->getGameData();
        if (!tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool spawnMachineInFront(GameWorld* gw, float fwd, float side, Faction* owner,
                         RootObject** spawned) {
    if (spawned) *spawned = 0;
    if (!gw || !gw->theFactory || !g_createBldgFn) {
        coop::logLine("SETUP: machine spawn skipped (no factory / createBuilding fn)");
        return false;
    }
    __try {
        GameData* tmpl = findMachineTemplate(gw);
        {
            char d[200];
            _snprintf(d, sizeof(d) - 1, "SETUP: machineTemplate='%s'",
                      tmpl ? tmpl->name.c_str() : "(none)");
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (!tmpl) return false;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return false;
        // Face the work face toward the leader so the operating body is visible.
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z); // y=0: createBuilding re-grounds
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/owner, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) { coop::logLine("SETUP: machine createBuilding returned null"); return false; }
        RootObject* ro = reinterpret_cast<RootObject*>(b); // Building's first base
        Ogre::Vector3 ap = ro->getPosition();
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: machine actualPos=%.2f,%.2f,%.2f visible=%d",
                      ap.x, ap.y, ap.z, ro->getVisible() ? 1 : 0);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (spawned) *spawned = ro;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP: machine createBuilding FAULTED");
        return false;
    }
}

bool orderWorkAt(Character* c, RootObject* fixture, int task) {
    if (!c || !fixture) return false;
    if (!g_addOrderFn && !g_addJobFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        Ogre::Vector3 loc = fixture->getPosition(); // virtual: safe direct call
        if (g_addOrderFn) {
            Building* dest = reinterpret_cast<Building*>(fixture);
            g_addOrderFn(c, dest, task, fixture, /*shift*/false,
                         /*clear*/true, &loc);
        } else if (g_addJobFn) {
            g_addJobFn(c, task, fixture, /*shift*/false,
                       /*addDontClear*/false, &loc);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Give 'c' a PERSISTENT AI GOAL (not a player order) to work 'task' at 'fixture'.
// addGoal hands the intent to the NPC's OWN AI - it is NOT a squad/player-order
// mechanism, so it does not recruit the NPC into the player squad. The NPC then
// walks to the fixture and operates it autonomously, exactly the natural-task path
// the host captures (the way the bar NPCs naturally sat). Returns true if issued.
bool goalWorkAt(Character* c, RootObject* fixture, int task) {
    if (!c || !fixture || !g_addGoalFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        g_addGoalFn(c, task, fixture);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Craft RE-ARM: a worker's addGoal intent does NOT serialise, so a baked craft
// scene (craft1) reloads with an idle worker. Re-find the baked fixture + nearest
// non-squad worker by SEARCH and re-issue the work goal, so the HOST resumes
// streaming the work task each session. Cheap + idempotent: it no-ops when the
// worker is already on task (re-issuing every tick would thrash pathing). Meant to
// be called once on load and then periodically by the host tick. Returns true if a
// goal is active/issued.
bool rearmCraftScene(GameWorld* gw) {
    int task = USE_TRAINING_DUMMY;
    RootObject* fixture = findWorkFixtureNear(gw, &task);
    if (!fixture) return false; // nothing baked nearby - silent (called on a timer)
    Character* worker = findWorkerNear(gw, fixture);
    if (!worker) return false;
    int cur = readCharTaskKey(worker);
    if (cur == task) return true; // already operating - leave it alone (no thrash)
    bool issued = goalWorkAt(worker, fixture, task);
    {
        unsigned int h[5];
        readObjectHand(static_cast<RootObject*>(worker), h);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "REARM: re-issued work goal task=%d worker=%u,%u curTask=%d ok=%d",
                  task, h[3], h[4], cur, issued ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    return issued;
}

// Identify the craft worker to drive for a LIVE-order test: the non-squad NPC
// nearest the baked work fixture, plus the task. Returns the worker's hand (in
// readObjectHand layout: [type,container,containerSerial,index,serial]) so a
// scenario can PIN this exact NPC for the whole run - vs re-picking "nearest now",
// which drifts as other world NPCs wander past the prop and orders the wrong body.
bool pickCraftWorker(GameWorld* gw, unsigned int workerHand[5], int* outTask) {
    int task = USE_TRAINING_DUMMY;
    RootObject* fixture = findWorkFixtureNear(gw, &task);
    if (!fixture) return false;
    Character* w = findWorkerNear(gw, fixture);
    if (!w) return false;
    if (!readObjectHand(static_cast<RootObject*>(w), workerHand)) return false;
    if (outTask) *outTask = task;
    return true;
}

// Hold the pinned worker UNTASKED at the prop during a craft_order baseline: clear
// its faction patrol goal and PARK it at the fixture each tick. An idle world NPC
// otherwise patrols out of the host's capture range (observed: only a handful of
// MEMBER samples, then it is too far to reach the prop when ordered). Parking it at
// the prop is the faithful "untasked NPC standing by a prop" staging. Returns true
// if held.
bool holdWorkerAtFixture(GameWorld* gw, const unsigned int workerHand[5]) {
    Character* w = resolveCharByHand(workerHand[3], workerHand[4], workerHand[0],
                                     workerHand[1], workerHand[2]);
    if (!w) return false;
    RootObject* fixture = findWorkFixtureNear(gw, 0);
    if (!fixture) return false;
    float fxx = 0, fxy = 0, fxz = 0;
    __try {
        Ogre::Vector3 p = fixture->getPosition();
        fxx = p.x; fxy = p.y; fxz = p.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    clearGoals(w);
    return park(w, fxx, fxy, fxz + 2.5f, 0.0f); // ~2.5u in front, not inside the prop
}

// Order a SPECIFIC worker (pinned by hand) to work the baked fixture (re-found by
// search - there is only one). This is the runtime EVENT the craft_order scenario
// fires: the host hands the pinned NPC a work goal mid-run so the join's driven copy
// transitions idle -> operating. workerHand is in readObjectHand layout. Guarded:
// if the worker is ALREADY operating, do nothing (re-issuing goalWorkAt every tick
// clears + re-adds the goal, thrashing pathing so it never settles into the pose -
// the same lesson the periodic re-arm encodes).
bool orderCraftWorker(GameWorld* gw, const unsigned int workerHand[5], int task) {
    Character* w = resolveCharByHand(workerHand[3], workerHand[4], workerHand[0],
                                     workerHand[1], workerHand[2]);
    if (!w) return false;
    if (readCharTaskKey(w) == task) return true; // already operating - don't thrash
    RootObject* fixture = findWorkFixtureNear(gw, 0);
    if (!fixture) return false;
    return goalWorkAt(w, fixture, task);
}

// --- down_order (Stage 2, LIVE knockout transition) ------------------------------
// A FIXED world anchor captured once at pin time. The baseline hold parks the subject
// here every tick. It must NOT be recomputed from the live leader each tick: a parked
// body that collides with the leader shoves it, the leader-relative anchor then chases
// the shoved leader, and the feedback flings the leader across the map (observed).
static float g_downAnchor[4]   = { 0, 0, 0, 0 }; // x,y,z,yaw
static bool  g_haveDownAnchor  = false;

// Identify the subject to knock out for a LIVE down-order test: the non-squad NPC
// nearest the LOCAL leader (in down1 that is the baked subject standing in front).
// Returns its hand in readObjectHand layout so the scenario PINS this exact NPC for
// the whole run, so host + join drive the SAME identity across the upright->down
// transition. Also latches a FIXED leader-front anchor (a few metres clear of the
// leader) for the baseline hold. Mirrors pickCraftWorker but anchors on the leader.
bool pickDownSubject(GameWorld* gw, unsigned int subjHand[5]) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return false;
    Character* best = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 30.0f, 30.0f, 30.0f, 64, 64, 0);
        float bestD = 1e18f;
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - center.x, dz = p.z - center.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestD) { bestD = d2; best = static_cast<Character*>(o); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!best) return false;
    if (!readObjectHand(static_cast<RootObject*>(best), subjHand)) return false;
    // Latch the hold anchor ONCE: 6 m in front of the leader (well clear of its body
    // so the parked subject never collides with / pushes the leader).
    Ogre::Vector3 pos; float yaw = 0.0f;
    if (leaderAnchor(gw, 6.0f, 0.0f, &pos, &yaw)) {
        g_downAnchor[0] = pos.x; g_downAnchor[1] = pos.y; g_downAnchor[2] = pos.z;
        g_downAnchor[3] = yaw; g_haveDownAnchor = true;
    }
    return true;
}

// Keep the pinned subject UPRIGHT and in capture range during a down_order baseline.
// A baked world NPC's AI paths it AWAY on reload (observed: ~89 u/s off-screen within
// 2 s, so by order-time it is far outside capture and the host streams no down body),
// and clearGoals alone does not hold it - so we PARK it at the FIXED anchor latched at
// pin time (teleport wins over its AI), like holdWorkerAtFixture pins the craft worker.
// The anchor is fixed (not leader-relative) to avoid the parked-body-pushes-leader
// feedback loop. It stays upright (nothing downs it in the baseline) and in range, so
// the join gets a clean upright "before" series. Returns true if held.
bool holdSubjectUpright(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    if (!g_haveDownAnchor) return false;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    clearGoals(s);
    return park(s, g_downAnchor[0], g_downAnchor[1], g_downAnchor[2], g_downAnchor[3]);
}

// The runtime EVENT the down_order scenario fires: knock the PINNED subject out so
// the host streams bodyState down and the join's driven copy must transition
// upright -> down. NOT guarded against re-issue: re-applying the forced KO timer on
// a throttle just tops it up (the body is already collapsed, no re-collapse), which
// is how it stays down for the rest of the run. subjHand is readObjectHand layout.
bool orderDownSubject(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    return knockDown(s, true);
}

// death_order: the runtime EVENT that KILLS the pinned subject. Test scaffold (no
// combat yet): collapse the body (ragdoll + KO) so it lies down, then mark the
// medical system dead + drain blood so Character::isDead() flips true. That sets
// BODY_DEAD in the host's bodyState capture, which publishOwned turns into a reliable
// EVT_DEATH. Re-assertable on a throttle (idempotent: already-dead stays dead).
bool killSubject(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        knockDown(s, true); // collapse + hold the body on the ground
        MedicalSystem* med = &s->medical;
        med->blood     = 0.0f; // past the point of no return
        med->unconcious = true;
        med->dead      = true; // Character::isDead() reads this -> BODY_DEAD
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// combat_kill bias (NOT a kill): lower the subject's blood so an ongoing REAL melee
// downs it decisively within the test window, without collapsing it ourselves (no
// unconcious/dead set, no ragdoll) - the opponent's hits + bleeding do the takedown,
// so the down edge comes from genuine combat. Idempotent-ish (clamps to >=0). Returns
// true if applied. subjHand is readObjectHand layout.
bool woundSubject(GameWorld* gw, const unsigned int subjHand[5], float blood) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        MedicalSystem* med = &s->medical;
        if (blood < 0.0f) blood = 0.0f;
        if (med->blood > blood) med->blood = blood; // only weaken, never heal
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Combat (Phase 3c, L5) -------------------------------------------------

bool readCombat(Character* c, CombatRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!c) return false;
    __try {
        if (g_inCombatModeFn) out->inCombat   = g_inCombatModeFn(c, true, true);
        if (g_inRangedModeFn) out->ranged     = g_inRangedModeFn(c) ? true : false;
        if (g_underMeleeFn)   out->underMelee  = g_underMeleeFn(c) ? true : false;
        if (g_fleeingFn)      out->fleeing     = g_fleeingFn(c) ? true : false;
        if (g_getAttackTargetFn) {
            // getAttackTarget returns a hand by value into our buffer (this=RCX,
            // retbuf=RDX). Read its POD fields into readObjectHand layout.
            char hbuf[sizeof(hand) + 16];
            memset(hbuf, 0, sizeof(hbuf));
            hand* th = reinterpret_cast<hand*>(hbuf);
            g_getAttackTargetFn(c, th);
            out->target[0] = (unsigned int)th->type;
            out->target[1] = th->container;
            out->target[2] = th->containerSerial;
            out->target[3] = th->index;
            out->target[4] = th->serial;
            out->hasTarget = (th->index != 0 || th->serial != 0);
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- duel scene (Phase 3c probe) ------------------------------------------------
static unsigned int g_duelHandA[5] = { 0, 0, 0, 0, 0 };
static unsigned int g_duelHandB[5] = { 0, 0, 0, 0, 0 };
static bool         g_haveDuel     = false;
// Fixed baseline anchors (x,y,z,yaw) for the two pinned duelists, latched once at pin
// time so combat_order can hold them peaceful + in range before the live attack order
// (same fixed-anchor reasoning as g_downAnchor: leader-relative parking causes a
// parked-body-pushes-leader feedback loop).
static float g_duelAnchorA[4] = { 0, 0, 0, 0 };
static float g_duelAnchorB[4] = { 0, 0, 0, 0 };
static bool  g_haveDuelAnchors = false;

// Order 'attacker' to focus-melee 'target' (UNPROVOKED so it engages regardless of
// faction relations). addGoal hands the intent to the NPC's own AI, like the work
// goal, so it is NOT a squad/player-order recruit. SEH-guarded.
static bool orderMeleeAttack(Character* attacker, Character* target) {
    if (!attacker || !target || !g_addGoalFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(attacker);
        g_addGoalFn(attacker, (int)UNPROVOKED_FOCUSED_MELEE_ATTACK,
                    static_cast<RootObject*>(target));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spawn two non-squad NPCs in front of the leader from the SAME nearby faction (so
// they are PEACEFUL on spawn - mutually non-hostile) and detach each into its own
// platoon for a stable, save-survivable hand. NO attack is issued here: this is the
// neutral baseline both a bake ('duel1') and the live combat_order test start from.
// startDuel()/rearmDuelScene() trigger the fight at runtime so the join sees a live
// peaceful->fighting transition (not just a baked load state). Returns true if both
// spawned and their hands were read.
bool setupDuelScene(GameWorld* gw) {
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP(duel): using nearby non-player faction (peaceful world NPCs)"
                      : "SETUP(duel): NO nearby non-player faction - falling back to "
                        "player faction (duelists WILL be squad members; load near a town)");
    Character* a = fac ? spawnCharInFaction(gw, 5.0f, -2.0f, fac)
                       : spawnNpcInFront(gw, 5.0f, -2.0f);
    Character* b = fac ? spawnCharInFaction(gw, 5.0f,  2.0f, fac)
                       : spawnNpcInFront(gw, 5.0f,  2.0f);
    if (!a || !b) { coop::logLine("SETUP(duel): duelist spawn FAILED"); return false; }
    detachFromTownAI(a);
    detachFromTownAI(b);
    g_haveDuel = readObjectHand(static_cast<RootObject*>(a), g_duelHandA) &&
                 readObjectHand(static_cast<RootObject*>(b), g_duelHandB);
    {
        char m[200];
        _snprintf(m, sizeof(m) - 1,
                  "SETUP(duel): spawned PEACEFUL A=%u,%u B=%u,%u (no attack issued)",
                  g_duelHandA[3], g_duelHandA[4], g_duelHandB[3], g_duelHandB[4]);
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
    }
    return g_haveDuel;
}

// combat_order LIVE-transition pin (Stage 3c): re-find the two baked duelists after a
// reload (the spawn-time globals are gone in a fresh process) by picking the two
// non-squad NPCs nearest the leader, and stash their hands into the duel globals so
// startDuel/rearmDuelScene/holdDuelistsPeaceful all operate on them by hand. Also
// latches a fixed front-left / front-right anchor for each so the baseline hold keeps
// them peaceful + in capture range. Returns true if TWO distinct subjects were pinned.
bool pickDuelSubjects(GameWorld* gw, unsigned int outA[5], unsigned int outB[5]) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return false;
    Character* best1 = 0; Character* best2 = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 30.0f, 30.0f, 30.0f, 64, 64, 0);
        float d1 = 1e18f, d2 = 1e18f;
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - center.x, dz = p.z - center.z;
            float dd = dx * dx + dz * dz;
            if (dd < d1) { d2 = d1; best2 = best1; d1 = dd; best1 = static_cast<Character*>(o); }
            else if (dd < d2) { d2 = dd; best2 = static_cast<Character*>(o); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!best1 || !best2 || best1 == best2) return false;
    if (!readObjectHand(static_cast<RootObject*>(best1), g_duelHandA)) return false;
    if (!readObjectHand(static_cast<RootObject*>(best2), g_duelHandB)) return false;
    for (int i = 0; i < 5; ++i) { outA[i] = g_duelHandA[i]; outB[i] = g_duelHandB[i]; }
    g_haveDuel = true;
    Ogre::Vector3 pa, pb; float ya = 0.0f, yb = 0.0f;
    if (leaderAnchor(gw, 6.0f, -1.5f, &pa, &ya) &&
        leaderAnchor(gw, 6.0f,  1.5f, &pb, &yb)) {
        g_duelAnchorA[0] = pa.x; g_duelAnchorA[1] = pa.y; g_duelAnchorA[2] = pa.z; g_duelAnchorA[3] = ya;
        g_duelAnchorB[0] = pb.x; g_duelAnchorB[1] = pb.y; g_duelAnchorB[2] = pb.z; g_duelAnchorB[3] = yb;
        g_haveDuelAnchors = true;
    }
    return true;
}

// Hold both pinned duelists at their latched anchors + clear goals each baseline tick,
// so they stay peaceful and in capture range until the live attack order. Returns true
// if held. No-op (false) until pickDuelSubjects has latched the anchors.
bool holdDuelistsPeaceful(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel || !g_haveDuelAnchors) return false;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    bool any = false;
    if (a) { clearGoals(a); park(a, g_duelAnchorA[0], g_duelAnchorA[1], g_duelAnchorA[2], g_duelAnchorA[3]); any = true; }
    if (b) { clearGoals(b); park(b, g_duelAnchorB[0], g_duelAnchorB[1], g_duelAnchorB[2], g_duelAnchorB[3]); any = true; }
    return any;
}

// Trigger the fight between the two pinned duelists (order each to melee the other).
// Used by the live probe/test to start combat AFTER a peaceful baseline. Returns the
// number of attack orders issued.
int startDuel(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return 0;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    if (!a || !b) return 0;
    int n = 0;
    if (orderMeleeAttack(a, b)) ++n;
    if (orderMeleeAttack(b, a)) ++n;
    return n;
}

int rearmDuelScene(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return -1;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    if (!a || !b) return -1;
    int n = 0;
    // Only re-issue to a duelist that has DISENGAGED (no combat mode), so we don't
    // thrash the AI of one that is already actively fighting.
    CombatRead ca, cb;
    if (readCombat(a, &ca) && !ca.inCombat) { if (orderMeleeAttack(a, b)) ++n; }
    if (readCombat(b, &cb) && !cb.inCombat) { if (orderMeleeAttack(b, a)) ++n; }
    return n;
}

bool getDuelHands(unsigned int outA[5], unsigned int outB[5]) {
    if (!g_haveDuel) return false;
    for (int i = 0; i < 5; ++i) { outA[i] = g_duelHandA[i]; outB[i] = g_duelHandB[i]; }
    return true;
}

int logDuelCombat(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return 0;
    const unsigned int* hands[2] = { g_duelHandA, g_duelHandB };
    const char* names[2] = { "A", "B" };
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        Character* c = resolveCharByHand(hands[k][3], hands[k][4], hands[k][0],
                                         hands[k][1], hands[k][2]);
        if (!c) continue;
        CombatRead cr;
        if (!readCombat(c, &cr)) continue;
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "COMBAT %s hand=%u,%u inCombat=%d ranged=%d underMelee=%d fleeing=%d "
            "hasTarget=%d target=%u,%u",
            names[k], hands[k][3], hands[k][4],
            cr.inCombat ? 1 : 0, cr.ranged ? 1 : 0, cr.underMelee ? 1 : 0,
            cr.fleeing ? 1 : 0, cr.hasTarget ? 1 : 0, cr.target[3], cr.target[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++n;
    }
    return n;
}

// 'craft' setup scene: spawn a save-stable work fixture + a NON-squad world NPC,
// then give the NPC a persistent AI GOAL to work it (NOT a player order, which
// would recruit it into the squad and bypass the host-authoritative world-NPC
// path). Its own AI then operates the station, so the HOST captures the natural
// work task and the join reproduces it once the save is baked. Task selection
// lives here where the TaskType enum is in scope. Returns true if a fixture spawned.
bool setupCraftScene(GameWorld* gw) {
    // Reloading a baked scene (craft1): a work fixture already exists nearby. Don't
    // spawn a duplicate - just re-arm the worker's goal (the goal didn't persist).
    {
        int rt = USE_TRAINING_DUMMY;
        if (findWorkFixtureNear(gw, &rt)) {
            coop::logLine("SETUP: existing work fixture found - re-arming (no spawn)");
            return rearmCraftScene(gw);
        }
    }

    // Pick the task from the chosen fixture name: a dummy is "used", everything
    // else is "operated". (findMachineTemplate prioritises a training dummy.)
    int task = OPERATE_MACHINERY;
    {
        GameData* tmpl = findMachineTemplate(gw);
        if (tmpl && (ciContains(tmpl->name.c_str(), "dummy") ||
                     ciContains(tmpl->name.c_str(), "bag")))
            task = USE_TRAINING_DUMMY;
    }
    // Borrow a live non-player faction from a nearby NPC so the worker is NOT a
    // squad member and the fixture has a legitimate owner (an ownerless fixture in
    // the player faction enlists the worker into the squad - the bug we observed).
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP: using nearby non-player faction (world NPC owner)"
                      : "SETUP: NO nearby non-player faction - falling back to player "
                        "faction (worker WILL be a squad member; load near a town)");

    RootObject* mach = 0;
    bool ok = spawnMachineInFront(gw, 6.0f, 0.0f, fac, &mach);
    if (ok && mach) {
        unsigned int h[5];
        if (readObjectHand(mach, h)) {
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SETUP: spawned machine hand=%u,%u,%u,%u,%u task=%d owned=%d",
                      h[3], h[4], h[0], h[1], h[2], task, fac ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else coop::logLine("SETUP: spawned machine (hand unread)");
    } else {
        coop::logLine("SETUP: machine spawn FAILED (no machine template or createBuilding faulted)");
    }

    // Spawn the worker in the borrowed faction (non-squad) when we have one; else
    // fall back to the player-faction spawn so the scene still produces something.
    Character* npc = fac ? spawnCharInFaction(gw, 4.0f, 1.0f, fac)
                         : spawnNpcInFront(gw, 4.0f, 1.0f);
    if (npc && mach) {
        // Detach the worker into its OWN platoon BEFORE baking, so its hand is fixed
        // by the save and is IDENTICAL on both clients. The join's runtime quieting
        // also detaches reproducible-pose NPCs (separateIntoMyOwnSquad); a worker
        // that SHARES a faction platoon gets re-containered there (hand 14->1,..),
        // breaking the host<->join hand pairing the pose oracle relies on. Baking it
        // pre-separated makes that runtime detach a no-op, so the hand stays stable.
        bool det = detachFromTownAI(npc);
        coop::logLine(det ? "SETUP: worker detached into own platoon (stable hand)"
                          : "SETUP: worker detach skipped/failed");
        // GOAL, not player order: addGoal hands intent to the NPC's own AI without
        // recruiting it (a player order would pull it into the squad).
        bool issued = goalWorkAt(npc, mach, task);
        coop::logLine(issued ? "SETUP: gave NPC a work GOAL at machine"
                             : "SETUP: work goal FAILED (no addGoal fn)");
        // Diagnostic: confirm the worker is NOT in the player squad (the screenshot
        // gate's machine-readable counterpart). Enlistment can be deferred a tick,
        // so this is an early indicator; the host-log [taskkey] world-capture + the
        // squad-bar screenshot are the authoritative checks.
        coop::logLine(isPlayerSquad(gw, static_cast<RootObject*>(npc))
                          ? "SETUP: WARN worker IS in player squad (enlisted)"
                          : "SETUP: worker is NON-squad (good)");
    } else {
        coop::logLine(npc ? "SETUP: no machine to assign NPC onto"
                          : "SETUP: NPC spawn FAILED");
    }
    return ok && mach != 0;
}

// Drop a Character into (on=true) or out of (on=false) full-body ragdoll. The host
// uses this to manufacture a "down" subject; the join uses it to reproduce one.
// SEH-guarded; no-op if the engine fn didn't resolve.
bool knockDown(Character* c, bool on) {
    if (!c) return false;
    __try {
        if (on) {
            // Prefer a real knockout: it collapses the body AND the medical KO timer
            // suppresses the get-up AI, so a loaded world NPC stays cleanly down (a
            // bare ragdoll it just fights out of - the twitch we saw). Top the timer
            // well past the re-arm interval. Fall back to ragdoll if unresolved.
            if (g_knockoutFn || g_knockoutForceFn) {
                MedicalSystem* med = &c->medical;
                if (g_knockoutFn)      g_knockoutFn(med, 1.0f);
                if (g_knockoutForceFn) g_knockoutForceFn(med, 8.0f);
                return true;
            }
            if (g_ragdollModeFn) { g_ragdollModeFn(c, true, RagdollPart::WHOLE); return true; }
            return false;
        }
        // Wake/stand: clear the forced KO timer and release any ragdoll.
        if (g_knockoutForceFn) g_knockoutForceFn(&c->medical, 0.0f);
        if (g_ragdollModeFn)   g_ragdollModeFn(c, false, RagdollPart::WHOLE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Maintain an already-down body WITHOUT re-triggering the collapse. knockDown()
// calls knockout(1.0), which re-initiates the ragdoll fall every time - calling
// that each tick produces the visible get-up/flop/ragdoll-spike flicker. The
// real cause of the get-up is the medical KO timer lapsing: once it hits 0 the
// wake AI stands the body, the replicator notices (too late), and re-collapses
// it. So instead top the force timer EVERY tick: the timer never reaches 0, the
// wake AI never fires, and the body stays cleanly down with no re-collapse.
bool holdDown(Character* c) {
    if (!c) return false;
    __try {
        if (g_knockoutForceFn) { g_knockoutForceFn(&c->medical, 8.0f); return true; }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The down subject pinned at bake time (readObjectHand layout), so the host can keep
// re-applying ragdoll to THAT exact NPC (a healthy body fights to stand back up).
static unsigned int g_downHand[5] = { 0, 0, 0, 0, 0 };
static bool         g_haveDownHand = false;

// 'down' setup scene: spawn a NON-squad world NPC and drop it into ragdoll, so the
// host streams bodyState != 0 and (later) the join reproduces a body on the ground.
// Mirrors the craft scene's faction borrow + pre-detach so the subject's hand is
// stable across save/reload and identical on both clients. Returns true if spawned.
bool setupDownScene(GameWorld* gw) {
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP(down): using nearby non-player faction (world NPC)"
                      : "SETUP(down): NO nearby non-player faction - falling back to "
                        "player faction (subject WILL be a squad member; load near a town)");
    Character* npc = fac ? spawnCharInFaction(gw, 4.0f, 0.0f, fac)
                         : spawnNpcInFront(gw, 4.0f, 0.0f);
    if (!npc) { coop::logLine("SETUP(down): NPC spawn FAILED"); return false; }
    bool det = detachFromTownAI(npc);
    coop::logLine(det ? "SETUP(down): subject detached into own platoon (stable hand)"
                      : "SETUP(down): subject detach skipped/failed");
    g_haveDownHand = readObjectHand(static_cast<RootObject*>(npc), g_downHand);
    if (g_haveDownHand) {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "SETUP(down): subject hand=%u,%u,%u,%u,%u",
                  g_downHand[3], g_downHand[4], g_downHand[0], g_downHand[1], g_downHand[2]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    bool kd = knockDown(npc, true);
    coop::logLine(kd ? "SETUP(down): subject knocked into ragdoll"
                     : "SETUP(down): knockDown FAILED (no ragdollMode fn)");
    return true;
}

// 'squad' setup scene (Phase 3.5 bake): build a SECOND player squad tab so the
// bidirectional ownership partition (host owns tab 0, join owns tab 1) has two tabs
// to split. A Kenshi squad tab is a Platoon, and a player member's tab identity is
// its hand CONTAINER. We recruit two world bodies into the player squad, then
// separateIntoMyOwnSquad ONE of them into its OWN player platoon (a distinct
// container = a distinct tab). The faction stays the player's, so the separated
// body is still a controllable squad member - just in tab 2. The user then SAVEs
// (e.g. 'squad1') and both clients load it. The member dump makes the bake
// machine-verifiable: 2+ distinct containers == 2+ squad tabs. Returns true if at
// least one recruit took.
bool setupSquadScene(GameWorld* gw) {
    if (!gw || !gw->player) { coop::logLine("SETUP(squad): no player interface"); return false; }
    PlayerInterface* pl = gw->player;

    Character* a = spawnNpcInFront(gw, 4.0f, -1.5f);
    Character* b = spawnNpcInFront(gw, 4.0f,  1.5f);
    bool ra = a && recruitNpc(gw, a);
    bool rb = b && recruitNpc(gw, b);
    coop::logLine(ra ? "SETUP(squad): recruited A into player squad"
                     : "SETUP(squad): recruit A FAILED");
    coop::logLine(rb ? "SETUP(squad): recruited B into player squad"
                     : "SETUP(squad): recruit B FAILED");
    // Separate B into its OWN player platoon -> a SECOND squad tab (distinct container).
    bool sep = b && detachFromTownAI(b);
    coop::logLine(sep ? "SETUP(squad): separated B into its own platoon (tab 2)"
                      : "SETUP(squad): separate B FAILED");

    // Dump the tab partition (distinct hand-containers across player chars) so the
    // bake is verifiable from the host log: 2+ distinct containers == 2+ squad tabs.
    __try {
        unsigned int n = pl->playerCharacters.size();
        char hdr[96]; _snprintf(hdr, sizeof(hdr) - 1, "SETUP(squad): playerChars=%u", n);
        hdr[sizeof(hdr) - 1] = '\0'; coop::logLine(hdr);
        for (unsigned int i = 0; i < n; ++i) {
            Character* c = pl->playerCharacters[i]; if (!c) continue;
            unsigned int h[5];
            if (readObjectHand(static_cast<RootObject*>(c), h)) {
                char b2[160];
                _snprintf(b2, sizeof(b2) - 1,
                    "SETUP(squad): member[%u] idx=%u,%u container(tab)=%u,%u",
                    i, h[3], h[4], h[1], h[2]);
                b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP(squad): member dump faulted");
    }
    return ra || rb;
}

// Keep down bodies down. A healthy ragdolled body recovers and stands back up, and
// ragdoll state does not survive save/load, so the host re-applies ragdoll on an
// interval. Rather than guess WHICH nearby NPC is "the subject" (the pin is empty
// after a reload, and the nearest-NPC heuristic mis-fired when the donor/a re-spawn
// sat at the same spot), we simply re-knock EVERY non-squad NPC within a modest
// radius of the leader. The baked subject is always covered, and any neighbour that
// goes down is a world NPC present in the save on BOTH clients, so the join
// reproduces it too. Re-applying to an already-ragdolled body is harmless. Returns
// the number of bodies (re-)knocked, or -1 if the query is unavailable.
int rearmDownScene(GameWorld* gw) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return -1;
    const float R = 30.0f; // horizontal reach for "down bodies in this scene"
    int n = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, R, R, R, 64, 64, 0);
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            if (knockDown(static_cast<Character*>(o), true)) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
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

// Read the host's body-state bit-flags off a rendered Character (BODY_* in Wire.h).
// SEH-guarded, POD-only: a fault returns 0 (treated as upright). 0 = normal/upright.
unsigned short readBodyState(Character* c) {
    if (!c) return 0;
    unsigned short s = 0;
    __try {
        if (g_isDownFn    && g_isDownFn(c))    s |= BODY_DOWN;
        if (g_isRagdollFn && g_isRagdollFn(c)) s |= BODY_RAGDOLL;
        if (g_isDeadCharFn && g_isDeadCharFn(c)) s |= BODY_DEAD;
        if (g_isCrawlFn   && g_isCrawlFn(c))   s |= BODY_CRAWL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return s;
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
        logSeatResolveOnce("JOIN", (int)e.task, e.hIndex, e.hSerial, e.sIndex, e.sSerial,
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

// Stage 3c (join-side): reproduce a streamed combat intent. e.task == TASK_COMBAT_MELEE
// and the subject hand is the attack target; resolve it to a local Character and order
// THIS body to focus-melee it (UNPROVOKED so it engages regardless of faction). The
// join's own engine then animates the fight (draw/swing/footwork) - we replicate the
// CAUSE, not the animation. Returns 2 ordered / 1 target not loaded here / 0 no-op /
// -1 fault. orderMeleeAttack clears prior goals so a re-issue cleanly re-targets.
int applyCombat(Character* c, const EntityState& e) {
    if (!c || e.task != TASK_COMBAT_MELEE) return 0;
    Character* target = resolveCharByHand(e.sIndex, e.sSerial, e.sType,
                                          e.sContainer, e.sContainerSerial);
    if (!target) return 1; // opponent not loaded on this client -> caller just holds
    return orderMeleeAttack(c, target) ? 2 : -1;
}

} // namespace engine
} // namespace coop
