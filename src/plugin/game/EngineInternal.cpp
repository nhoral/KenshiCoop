// EngineInternal.cpp (formerly Engine.cpp - monolith split, 2026-07-12). This
// file owns the shared engine-access substrate for every Engine*.cpp domain TU:
//   * the g_* resolved-function-pointer registry + hook trampolines/queues
//     (declared extern in EngineInternal.h - this is their ONE definition
//     point; the documentation for each lives here, with its definition);
//   * engine::resolve() - all KenshiLib::GetRealAddress resolution;
//   * hook detour bodies + install* entry points;
//   * Stage-0 runtime (gameplay-live gate, save/load coordination);
//   * shared internal helpers used by more than one domain TU.
// Domain operations live in the Engine*.cpp TUs (see resources/CODE_MAP.md).
// The include prelude (Boost guards first) rides EngineInternal.h.

#include "EngineInternal.h"

namespace coop {
namespace engine {

// ---- Phase 5c: typed, throttled SEH-fault accounting ------------------------
// Per-op counter + last-log timestamp. Main-thread only (engine invariant), so
// plain statics need no locking. Declarations live in EngineFaults.h.
namespace {
struct FaultState { unsigned int count; unsigned long lastMs; };
FaultState g_faults[FAULT_OP_COUNT] = { { 0, 0 } };
const char* const kFaultNames[FAULT_OP_COUNT] = {
    "resolve_char",   // FAULT_RESOLVE_CHAR
    "resolve_object", // FAULT_RESOLVE_OBJECT
    "hand_read",      // FAULT_HAND_READ
    "inv_of",         // FAULT_INV_OF
    "capture",        // FAULT_CAPTURE
    "apply",          // FAULT_APPLY
    "other"           // FAULT_OTHER
};
} // namespace

const char* faultOpName(FaultOp op) {
    if ((unsigned)op >= (unsigned)FAULT_OP_COUNT) return "unknown";
    return kFaultNames[op];
}

unsigned int faultCount(FaultOp op) {
    if ((unsigned)op >= (unsigned)FAULT_OP_COUNT) return 0;
    return g_faults[op].count;
}

void noteFault(FaultOp op) {
    if ((unsigned)op >= (unsigned)FAULT_OP_COUNT) return;
    FaultState& s = g_faults[op];
    ++s.count;
    unsigned long now = coop::wallClockMs();
    if (faultShouldLog(s.count, now, &s.lastMs, 1000)) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[engine] FAULT op=%s n=%u",
                  faultOpName(op), s.count);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// SaveManager entry points, resolved at load. getSingleton is a static member;
// load(name) and savesExist are __thiscall (passed via __fastcall self in RCX).
typedef SaveManager* (__fastcall* SaveMgrGetFn)();
typedef void         (__fastcall* SaveMgrLoadNameFn)(SaveManager* self, const std::string* name);
typedef bool         (__fastcall* SaveMgrSavesExistFn)(SaveManager* self);
typedef void         (__fastcall* SaveMgrSaveNameFn)(SaveManager* self, const std::string* name,
                                                     bool autosave);

SaveMgrGetFn        g_getFn        = 0;
SaveMgrLoadNameFn   g_loadFn       = 0;
SaveMgrSavesExistFn g_savesExistFn = 0;
SaveMgrSaveNameFn   g_saveFn       = 0;

// Protocol 31 (coordinated save): SaveManager::getCurrentGame / getSavePath.
// Both return `const std::string&` - a member returning a reference puts `this`
// in RCX and the referent's ADDRESS in RAX, so model them as pointer-returning
// (the TaskerDescFn precedent). Spike 39 quoted their RVAs from SaveManager.h
// but they were never called at runtime until now (save_probe validates them).
typedef const std::string* (__fastcall* SaveMgrStrFn)(SaveManager* self);
SaveMgrStrFn g_saveMgrCurGameFn = 0;
SaveMgrStrFn g_saveMgrPathFn    = 0;

// Protocol 32: SaveManager::execute - the deferred-signal consumer (performs
// the actual save/load/import/new-game named by the pending signal). The
// title-screen loop calls it; mid-session nothing does, so the coordinated
// load pumps it manually from the end of the main-loop tick.
typedef void (__fastcall* SaveMgrExecFn)(SaveManager* self);
SaveMgrExecFn g_saveMgrExecFn = 0;

// Protocol 31: SaveManager::save detour. save(name, autosave) is the ONE entry
// every save takes - the in-game save menu, quicksave, the autosave timer AND
// the mod's own saveGameAs (KenshiLib's inline patch means even the direct
// g_saveFn call re-enters the detour; the trampoline below is the real body).
// Every call queues a SaveEdgeRec the Plugin drains once per tick (engine tick
// and plugin tick share the main thread - no lock, the recruit-edge pattern).
// g_saveSuppressAll (JOIN under save-sync): the local write is SKIPPED - the
// host's save is authoritative and arrives via the in-band transfer; a manual
// edge is forwarded to the host as PKT_SAVE_REQ by the Plugin instead.
std::vector<SaveEdgeRec> g_saveEdges;
bool g_saveSuppressAll = false;
SaveMgrSaveNameFn g_saveHookOrig = 0;

void __fastcall saveMgrSave_hook(SaveManager* self, const std::string* name,
                                 bool autosave) {
    char nm[48];
    nm[0] = '\0';
    __try {
        if (name) {
            strncpy(nm, name->c_str(), sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = '\0';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '\0'; }
    const bool suppress = g_saveSuppressAll;
    if (!suppress) g_saveHookOrig(self, name, autosave);
    __try {
        SaveEdgeRec r;
        memset(&r, 0, sizeof(r));
        strncpy(r.name, nm, sizeof(r.name) - 1);
        r.autosave   = autosave ? 1 : 0;
        r.suppressed = suppress ? 1 : 0;
        if (g_saveEdges.size() < 8) g_saveEdges.push_back(r);
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "[save] LOCAL-SAVE name='%s' autosave=%d suppressed=%d",
                  r.name, r.autosave, r.suppressed);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Protocol 32: SaveManager::load detours. There are TWO public load entries
// and every trigger takes one of them: load(name) - the title screen's
// auto-load and the mod's own loadSave (KenshiLib's inline patch means even
// the direct g_loadFn call re-enters the detour; the trampoline below is the
// real body) - and load(SaveInfo&, resetPos) - the IN-GAME LOAD MENU (field
// evidence 2026-07-09: a menu load produced a world swap with NO edge from
// the name detour, so the menu never funnels through load(name)). BOTH are
// detoured into the same edge/suppression logic; a reentry latch keeps one
// overload calling the other (either direction) from double-counting the
// edge. The engine's load is DEFERRED (sets LOADGAME; the world swaps a few
// frames later), so passing it through is safe from the hook context - the
// probe validates the mid-session case. g_loadSuppressAll (JOIN under
// load-sync): the local load is SWALLOWED - the host arbitrates and the
// edge forwards as PKT_LOAD_REQ. g_loadBypassOnce lets the join's own
// coordinated load (issued on PKT_LOAD_GO) pass through while suppression
// is armed.
std::vector<LoadEdgeRec> g_loadEdges;
bool g_loadSuppressAll = false;
bool g_loadBypassOnce  = false;
bool g_inLoadDetour    = false; // reentry latch (outer hook owns the edge)
SaveMgrLoadNameFn g_loadHookOrig = 0;
typedef void (__fastcall* SaveMgrLoadInfoFn)(SaveManager* self, const SaveInfo* info,
                                             bool resetPos);
SaveMgrLoadInfoFn g_loadInfoHookOrig = 0;

// Shared edge/suppression body. Returns true when the original must run
// (i.e. the load was NOT suppressed). 'via' tags the log with the entry.
static bool loadDetourEdge(const char* nm, const char* via) {
    const bool bypass   = g_loadBypassOnce;
    const bool suppress = g_loadSuppressAll && !bypass;
    g_loadBypassOnce = false;
    __try {
        LoadEdgeRec r;
        memset(&r, 0, sizeof(r));
        strncpy(r.name, nm, sizeof(r.name) - 1);
        r.suppressed = suppress ? 1 : 0;
        if (g_loadEdges.size() < 8) g_loadEdges.push_back(r);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "[load] LOCAL-LOAD name='%s' suppressed=%d bypass=%d via=%s",
                  r.name, r.suppressed, bypass ? 1 : 0, via);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return !suppress;
}

void __fastcall saveMgrLoad_hook(SaveManager* self, const std::string* name) {
    if (g_inLoadDetour) { g_loadHookOrig(self, name); return; }
    char nm[48];
    nm[0] = '\0';
    __try {
        if (name) {
            strncpy(nm, name->c_str(), sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = '\0';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '\0'; }
    if (loadDetourEdge(nm, "name")) {
        g_inLoadDetour = true;
        g_loadHookOrig(self, name);
        g_inLoadDetour = false;
    }
}

// The in-game load menu's entry: load(SaveInfo&, resetPos).
void __fastcall saveMgrLoadInfo_hook(SaveManager* self, const SaveInfo* info,
                                     bool resetPos) {
    if (g_inLoadDetour) { g_loadInfoHookOrig(self, info, resetPos); return; }
    char nm[48];
    nm[0] = '\0';
    __try {
        if (info) {
            strncpy(nm, info->name.c_str(), sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = '\0';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = '\0'; }
    if (loadDetourEdge(nm, "menu")) {
        g_inLoadDetour = true;
        g_loadInfoHookOrig(self, info, resetPos);
        g_inLoadDetour = false;
    }
}

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
// Limb-loss replication (protocol 16): the engine's own limb-loss paths, so a
// replicated amputation runs the full effect chain (mesh detach, bleed, stats).
// amputate takes the force by const& (== a pointer at the ABI level).
typedef void      (__fastcall* MedAmputateFn)(MedicalSystem* self, int limb,
                                              bool createSeveredItem,
                                              const Ogre::Vector3* force);
typedef void      (__fastcall* MedCrushLimbFn)(MedicalSystem* self, int limb);
typedef void      (__fastcall* MedSetRobotLimbFn)(MedicalSystem* self, int limb,
                                                  Item* item, bool isLoadingASave);
// MedicalSystem::getLimbState: the engine's own accessor. MUST be used instead of
// robotLimbs->states[] - robotLimbs is allocated LAZILY (null on any character
// that never lost a limb), and reading through it made every healthy character
// report 0xFF "unknown" limb states (the limb_loss ok=0 bug).
typedef int       (__fastcall* MedGetLimbStateFn)(const MedicalSystem* self, int limb);
// Character stats sync (protocol 17): CharStats::getStatRef(StatsEnumerated)
// returns the RAW stat slot by enum (float& == float* at the ABI level), so we
// need no per-field offsets; CharStats::periodicUpdate (via the public _NV_
// wrapper) recalculates the derived caches (attack/block speed, run speed,
// encumbrance) after a write.
typedef float*    (__fastcall* StatsGetRefFn)(CharStats* self, int what);
typedef void      (__fastcall* StatsRecalcFn)(CharStats* self);
// Carried-body sync (protocol 18): pickupObject runs the engine's full pickup
// chain on the CARRIER (carry-mode ragdoll on `who`, shoulder bone attach,
// carry animation, transform-follow); dropCarriedObject is its inverse
// (ragdollHim = drop the body limp on the ground, removeOnly = detach without
// the physical drop - we always pass false).
typedef void      (__fastcall* PickupObjectFn)(Character* self, Character* who);
typedef void      (__fastcall* DropCarriedFn)(Character* self, bool ragdollHim,
                                              bool removeOnly);
// Furniture occupancy (protocol 19): setBedMode/setPrisonMode run the engine's
// full placement chain on the OCCUPANT (in-bed/in-cage pose, furniture attach,
// inSomething/inWhat bookkeeping). Same signature for both.
typedef void      (__fastcall* SetFurnModeFn)(Character* self, bool on,
                                              UseableStuff* h);
// Chained/pole prisoner (protocol 41): setChainedMode runs the engine's full
// shackle chain on the OCCUPANT (isChained/slaveOwner bookkeeping + shackle
// item + slave AI job). It is a Kenshi member `void setChainedMode(bool on,
// const hand& owner)`, so the x64 ABI passes the owner hand by hidden
// reference - the raw binding takes a `const hand*`. Reproduces a captive on
// a prisoner POLE (a different engine system from the cage's setPrisonMode).
typedef void      (__fastcall* SetChainedModeFn)(Character* self, bool on,
                                                 const hand* owner);
// Phase 6 shackle read lever: Character::getChainedModeShackles() (RVA
// 0x5C8290) returns the equipped LockedArmour* (the shackle item) or null. A
// non-null return with a non-null LockedArmour::lock (0x2F0) is a locked
// shackle. Read-only; used by readShackle() for the 6a evidence spike.
typedef LockedArmour* (__fastcall* GetShacklesFn)(Character* self);
// Stealth (protocol 20): setStealthMode runs the engine's full sneak chain on
// the LOCAL body (sneak-walk pose, stealthUpdate scanning, stealth skill use).
// notifyICanSeeYouSneaking updates the sneaker's whoSeesMeSneaking map + the
// marker arrows exactly as a seer's own vision check would. YesNoMaybe has
// user-declared constructors, so the x64 ABI passes it by hidden reference -
// the raw binding takes an int* to its ynm key.
typedef void      (__fastcall* SetStealthModeFn)(Character* self, bool on);
typedef void      (__fastcall* NotifySeeSneakFn)(Character* self, Character* who,
                                                 const int* seeingYnm, float prog01);

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
MedAmputateFn     g_medAmputateFn     = 0;
MedCrushLimbFn    g_medCrushLimbFn    = 0;
MedSetRobotLimbFn g_medSetRobotLimbFn = 0;
MedGetLimbStateFn g_medGetLimbStateFn = 0;
StatsGetRefFn     g_statsGetRefFn     = 0;
StatsRecalcFn     g_statsRecalcFn     = 0;
PickupObjectFn    g_pickupObjectFn    = 0;
DropCarriedFn     g_dropCarriedFn     = 0;
SetFurnModeFn     g_setBedModeFn      = 0;
SetFurnModeFn     g_setPrisonModeFn   = 0;
SetChainedModeFn  g_setChainedModeFn  = 0;
GetShacklesFn     g_getShacklesFn     = 0;
IsSlaveFn         g_isSlaveFn         = 0;
SetStealthModeFn  g_setStealthModeFn  = 0;
NotifySeeSneakFn  g_notifySeeSneakFn  = 0;
CamGetCenterFn    g_camGetCenterFn    = 0;
CamIsInitFn       g_camIsInitFn       = 0;

// Limb state with the engine's null policy: robotLimbs is lazily allocated, and
// a null robotLimbs means "no limb ever lost/replaced" == ORIGINAL on all four.
// Caller holds SEH.
unsigned char limbStateOf(MedicalSystem* med, int limb) {
    if (g_medGetLimbStateFn) return (unsigned char)g_medGetLimbStateFn(med, limb);
    RobotLimbs* rl = med ? med->robotLimbs : 0;
    return rl ? (unsigned char)rl->states[limb] : (unsigned char)LIMB_ORIGINAL;
}

// Consensus game-speed sync: the engine's own speed setters.
// GameWorld::setGameSpeed(speed, click) drives frameSpeedMult AND updates the
// UI speed buttons (the loud path a real click takes); userPause(p) drives the
// user-pause flag (0x8B9) independent of the multiplier; togglePause(on) is
// the keyboard-pause entry; setFrameSpeedMultiplier(m) is the bare multiplier
// setter (no UI notify) - the QUIET path the arbitrated effective rides so
// the buttons keep showing the player's VOTE.
typedef void (__fastcall* SetGameSpeedFn)(GameWorld* self, float speed, bool click);
typedef void (__fastcall* UserPauseFn)(GameWorld* self, bool p);
typedef void (__fastcall* SetFrameSpeedMultFn)(GameWorld* self, float m);
SetGameSpeedFn      g_setGameSpeedFn      = 0;
UserPauseFn         g_userPauseFn         = 0;
UserPauseFn         g_togglePauseFn       = 0;
SetFrameSpeedMultFn g_setFrameSpeedMultFn = 0;

// Door/gate state (protocol 26). DoorStuff : Building carries the whole door
// model: `state` (DoorState, animates through OPENING/CLOSING), a DoorLock,
// and the engine's own action entries - openDoor/closeDoor (the polite path:
// animation, navmesh, sound; returns false when refused) with
// _forceDoorOpenUT/_forceDoorClosedUT as the blunt fallback, and
// lockDoor/unlockDoor for the lock bit. Doors are found among BUILDING
// spatial-query results by the Building::imADoor member (0x1A0).
typedef bool (__fastcall* DoorBoolFn)(const DoorStuff* self);
typedef bool (__fastcall* DoorActFn)(DoorStuff* self);
typedef void (__fastcall* DoorVoidFn)(DoorStuff* self);
DoorBoolFn g_doorIsOpenFn      = 0;
DoorBoolFn g_doorIsLockedFn    = 0;
DoorActFn  g_doorOpenFn        = 0;
DoorActFn  g_doorCloseFn       = 0;
DoorActFn  g_doorForceOpenFn   = 0;
DoorActFn  g_doorForceCloseFn  = 0;
DoorVoidFn g_doorLockFn        = 0;
DoorVoidFn g_doorUnlockFn      = 0;

// Placed-building construction levers (protocol 27). Building carries a public
// ConstructionState `_buildState` at 0x160 (isComplete + constructionProgress
// float); the engine's own progress entries (used by builder labor) are the
// write levers - notifyConstructionComplete finishes a site natively
// (scaffold off, materials restored, pathfinding updated).
typedef void (__fastcall* BuildProgFn)(Building* self, float amount);
typedef void (__fastcall* BuildDoneFn)(Building* self);
BuildProgFn g_buildSetProgFn  = 0;
BuildProgFn g_buildAddProgFn  = 0;
BuildDoneFn g_buildNotifyDoneFn = 0;

// Production machine levers (protocol 33). All machine classes derive from
// UseableStuff (power bit @0x3B5, switchPowerOn = the engine's own toggle);
// production classes add productionState @0x468, the output ConsumptionItem*
// @0x448 and the consumptionItems lektor @0x478 (plain members - direct
// reads). setProductionItem is the native output-buffer write lever; operate()
// is the worker-production entry, resolved PER OVERRIDE because we call the
// non-virtual addresses (classType selects which one - the same dispatch the
// vtable would do). getPowerOutput has a generator override (base returns the
// consumer-side draw), picked via the engine's own isGenerator().
typedef void  (__fastcall* MachPowerSetFn)(UseableStuff* self, bool on);
typedef float (__fastcall* MachPowerOutFn)(const UseableStuff* self);
typedef bool  (__fastcall* MachIsGenFn)(const UseableStuff* self);
typedef void  (__fastcall* MachSetProdItemFn)(ProductionBuilding* self,
                                              GameData* item, int stack,
                                              float progress01);
typedef int   (__fastcall* MachTechLvlFn)(ResearchBuilding* self);
typedef void  (__fastcall* MachOperateFn)(Building* self, Character* who,
                                          float amount);
MachPowerSetFn    g_machPowerFn          = 0;
MachPowerOutFn    g_machPowerOutBaseFn   = 0;
MachPowerOutFn    g_machPowerOutGenFn    = 0;
MachIsGenFn       g_machIsGenFn          = 0;
MachSetProdItemFn g_machSetProdItemFn    = 0;
MachTechLvlFn     g_machTechLvlFn        = 0;
MachOperateFn     g_machOperateProdFn    = 0; // ProductionBuilding (generator/drill/base)
MachOperateFn     g_machOperateCraftFn   = 0; // CraftingBuilding override
MachOperateFn     g_machOperateFarmFn    = 0; // FarmBuilding override
MachOperateFn     g_machOperateResearchFn = 0; // ResearchBuilding override
// getProductionItemData: the TEMPLATE the machine produces (available even
// while productionItem is still null - the buffer only materializes on the
// first actual production tick, a prod_probe run-151948 finding). Per-class
// overrides resolved like operate().
typedef GameData* (__fastcall* MachProdItemDataFn)(Building* self);
MachProdItemDataFn g_machProdDataBaseFn  = 0; // StorageBuilding base
MachProdItemDataFn g_machProdDataCraftFn = 0; // CraftingBuilding override

// Game-clock read (protocol 25). GameWorld::getTimeStamp_inGameHours returns
// TimeOfDay BY VALUE, and TimeOfDay has user-declared constructors + copy
// assignment - NON-trivial for the MSVC x64 return ABI, so it comes back via
// a hidden retbuf pointer with `this` staying in RCX (the documented
// getPositionBip01 hazard: model as fn(self, retbuf), NEVER as a by-value
// return). TimeOfDay is one double (total in-game hours) at offset 0, so the
// retbuf is modeled as a raw double. getLengthOfHourInRealSeconds gives the
// real-seconds-per-game-hour conversion (a plain float return).
typedef double* (__fastcall* GetTimeHoursFn)(GameWorld* self, double* ret);
typedef float   (__fastcall* GetHourLenFn)(GameWorld* self);
GetTimeHoursFn g_getTimeHoursFn = 0;
GetHourLenFn   g_getHourLenFn   = 0;

// Speed-intent capture (the vote source). Detours on the three user-reachable
// speed entries record every call NOT made by our own quiet writer as user
// intent: UI button clicks, keyboard pause, RE_Kenshi speed controls and the
// scenario's simulated writeGameSpeed clicks all funnel through these -
// INCLUDING clicks equal to the current effective speed, which a state-diff
// detector can never see (the stuck-vote bug). Main-thread only (the engine
// calls these from its own tick; our writers run from the main-loop hook).
SetGameSpeedFn g_setGameSpeedOrig = 0;
UserPauseFn    g_userPauseOrig    = 0;
UserPauseFn    g_togglePauseOrig  = 0;
bool  g_speedGuardWrite   = false; // reentrancy guard: quiet writes are not intent
bool  g_speedIntentFresh  = false; // set by the hooks, cleared by consumeSpeedIntent
float g_speedIntentMult   = 1.0f;  // requested multiplier (pause preserves it)
bool  g_speedIntentPaused = false; // requested pause state
bool  g_speedIntentSeeded = false; // first consume seeds from live engine state

// Last QUIET write (the arbitrated effective we applied). The poll-based
// intent fallback compares the live engine against THIS to detect real UI
// clicks: the MainBar click handler writes the speed inline (manual-session
// finding 2026-07-08 - it does NOT call the hooked setGameSpeed), so an
// unexplained engine change can only be the user.
bool  g_quietHave   = false;
float g_quietMult   = 1.0f;
bool  g_quietPaused = false;

// Vote-highlight snapshot. The speed_probe spike proved setFrameSpeedMultiplier
// is silent BUT userPause re-highlights the MainBar buttons from the EFFECTIVE
// state (pause lights the pause button; unpause re-selects the current
// multiplier's button) - which would wipe the player's vote position on every
// enforced pause/resume. So: right after each REAL user action (the engine has
// just highlighted the clicked button), snapshot the button states; the quiet
// writer restores that snapshot after its guarded userPause. The buttons then
// always show the last thing the USER did, never the arbitrated effective.
char g_voteBtn[15]; int g_voteBtnN = 0;

// Phase 5 spike state: combat-cap hint (set by syncSpeed) + env gate.
bool g_speedCombatHint = false;

bool speedDbgOn() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("KENSHICOOP_DEBUG_SPEED"); on = (e && e[0] == '1') ? 1 : 0; }
    return on == 1;
}

void setSpeedCombatHint(bool inCombat) { g_speedCombatHint = inCombat; }

// One-liner used by the speed-setter diagnostics: dumps the current button
// highlight so the log shows whether the engine moved the indicator.
static void speedDbgLog(const char* what, float f, int i) {
    char btn[16]; if (readSpeedButtons(btn, sizeof(btn)) <= 0) btn[0] = '\0';
    char b[160];
    _snprintf(b, sizeof(b) - 1,
        "[speeddbg] %s v=%.2f i=%d guard=%d combat=%d btn=%s vote=%.*s",
        what, f, i, g_speedGuardWrite ? 1 : 0, g_speedCombatHint ? 1 : 0,
        btn, g_voteBtnN, g_voteBtn);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

// Capture the current speed-button highlight as the vote display. Guard: a
// BLANK read (no button selected) is never a valid vote - it is the transient
// dehighlight setGameSpeed leaves behind on a programmatic / denied write
// (spike 2026-07-17: the tier button is only reselected by the inline UI click
// handler, never by setGameSpeed). Storing that blank would wipe the indicator,
// so keep the last real selection instead.
void snapshotVoteButtons() {
    char buf[16];
    int n = readSpeedButtons(buf, sizeof(buf));
    if (n <= 0) return;
    bool anySel = false;
    for (int i = 0; i < n; ++i) if (buf[i] == '1') { anySel = true; break; }
    if (!anySel) return;
    memcpy(g_voteBtn, buf, n); g_voteBtnN = n;
}

void __fastcall setGameSpeed_hook(GameWorld* self, float speed, bool click) {
    if (speedDbgOn()) speedDbgLog("setGameSpeed", speed, click ? 1 : 0);
    if (!g_speedGuardWrite && speed > 0.0f) {
        g_speedIntentMult  = speed;
        g_speedIntentFresh = true;
    }
    g_setGameSpeedOrig(self, speed, click);
    // snapshotVoteButtons ignores the blank dehighlight setGameSpeed leaves
    // behind (spike 2026-07-17), so this keeps the last real tier selection.
    if (!g_speedGuardWrite) snapshotVoteButtons();
}

void __fastcall userPause_hook(GameWorld* self, bool p) {
    if (speedDbgOn()) speedDbgLog("userPause", 0.0f, p ? 1 : 0);
    if (!g_speedGuardWrite) {
        g_speedIntentPaused = p;
        g_speedIntentFresh  = true;
    }
    g_userPauseOrig(self, p);
    if (!g_speedGuardWrite) snapshotVoteButtons();
}

void __fastcall togglePause_hook(GameWorld* self, bool p) {
    if (speedDbgOn()) speedDbgLog("togglePause", 0.0f, p ? 1 : 0);
    if (!g_speedGuardWrite) {
        g_speedIntentPaused = p;
        g_speedIntentFresh  = true;
    }
    g_togglePauseOrig(self, p);
    if (!g_speedGuardWrite) snapshotVoteButtons();
}

// Re-apply the vote snapshot after a quiet write's userPause disturbed the
// highlight. SEH-guarded, pure UI (setStateSelected touches no engine state).
void restoreVoteButtons() {
    if (g_voteBtnN <= 0) return;
    __try {
        if (!gui || !gui->mainbar) return;
        Ogre::FastArray<MyGUI::Button*>& btns = gui->mainbar->speedButtons;
        int n = (int)btns.size();
        if (n > g_voteBtnN) n = g_voteBtnN;
        for (int i = 0; i < n; ++i)
            if (btns[i]) btns[i]->setStateSelected(g_voteBtn[i] == '1');
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Phase 5 continuous reconcile: force the MyGUI speed buttons back onto the
// captured vote whenever the live highlight has drifted off it (an engine
// dialog auto-pause re-highlight, a combat cap, or a click=false dehighlight).
// Only restores on a real mismatch so we don't hammer MyGUI every tick. The
// caller gates this on "no user acted this tick" so a genuine same-frame click
// is never fought.
void reconcileVoteButtons() {
    if (g_voteBtnN <= 0) return;
    char cur[16];
    int n = readSpeedButtons(cur, sizeof(cur));
    if (n <= 0 || n != g_voteBtnN) return;
    if (memcmp(cur, g_voteBtn, n) != 0) restoreVoteButtons();
}

// Money + vendor trading (protocol 22 groundwork). Character::getPlatoon gives
// the body's ActivePlatoon (its squad-tab container); ActivePlatoon::me is the
// persistent Platoon whose Ownerships block holds that tab's WALLET (spike 29 -
// there is no global player wallet). getMoney/setMoney are the engine's own
// accessors (never raw offsets); Inventory::buyItem is the real purchase path
// (wallet debit + vendor stock mutation + item transfer), called on the VENDOR
// inventory with sendingTo = the buyer.
typedef ActivePlatoon* (__fastcall* GetPlatoonFn)(const Character* self);
typedef int   (__fastcall* OwnGetMoneyFn)(const Ownerships* self);
typedef void  (__fastcall* OwnSetMoneyFn)(Ownerships* self, int amount);
typedef Item* (__fastcall* BuyItemFn)(Inventory* self, Item* itemToBuy,
                                      RootObject* sendingTo);
// ActivePlatoon::refreshInventory(firstTime): the engine's own trader-stock
// builder (scheduled off Platoon::traderInventoryRefreshTime). A ShopTrader's
// Inventory member is LAZY (null until the shop UI first opens - shop_probe run
// 101952 finding: every vendor read stock=-1), so the probe forces it via the
// TRADER's platoon before a programmatic purchase - what opening the trade
// window would have done. (ShopTrader::updateInventory itself is private.)
typedef void  (__fastcall* PlatoonRefreshInvFn)(ActivePlatoon* self, bool firstTime);
// ShopTrader::getTrader accessor - the raw `trader` member read null on every
// enumerated vendor (run 103547), so the engine's own accessor is the fallback
// (it may resolve the trader lazily / from a different field).
typedef Character* (__fastcall* ShopGetTraderFn)(const ShopTrader* self);
GetPlatoonFn  g_getPlatoonFn  = 0;
OwnGetMoneyFn g_ownGetMoneyFn = 0;
OwnSetMoneyFn g_ownSetMoneyFn = 0;
BuyItemFn     g_buyItemFn     = 0;
PlatoonRefreshInvFn g_platoonRefreshInvFn = 0;
ShopGetTraderFn     g_shopGetTraderFn     = 0;

// Purchase observability detour (protocol 22, 1c groundwork). Inventory::
// buyItem is the engine's ONE real purchase path (a trade-UI drag lands here),
// but automation cannot reach it in the test save (vendor inventories are lazy
// and the enumerated SHOP_TRADER_CLASS objects carry no bound trader - shop_
// probe runs 103018-104036). The detour makes every REAL purchase in a manual
// field session log a "[shop] BUY-LOCAL" line (seller identity + money, item
// sid, buyer) - the evidence that gates the eventual vendor-stock mirror. The
// buyer-side effects (wallet debit, item into the buyer's inventory) already
// ride the money + inventory channels; only the VENDOR-side mutation stays
// local, which this line makes measurable.
BuyItemFn g_buyItemOrig = 0;
Item* __fastcall buyItem_hook(Inventory* self, Item* itemToBuy, RootObject* sendingTo) {
    Item* got = g_buyItemOrig(self, itemToBuy, sendingTo);
    __try {
        char sid[48]; sid[0] = '\0';
        if (itemToBuy) {
            GameData* gd = itemToBuy->getGameData();
            if (gd) { strncpy(sid, gd->stringID.c_str(), sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0'; }
        }
        unsigned int sh[5] = { 0, 0, 0, 0, 0 };
        int sellerMoney = -1;
        RootObject* seller = self ? self->owner : 0;
        if (seller) {
            readObjectHand(seller, sh);
            __try { sellerMoney = seller->getMoney(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        unsigned int bh[5] = { 0, 0, 0, 0, 0 };
        if (sendingTo) readObjectHand(sendingTo, bh);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[shop] BUY-LOCAL ok=%d sid='%s' seller=%u,%u money=%d buyer=%u,%u",
                  got ? 1 : 0, sid, sh[3], sh[4], sellerMoney, bh[3], bh[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return got;
}

// ---- Cross-owner trade veto (block direct squad-to-squad transfers) --------
// A UI inventory drag has no single engine entry point; it is remove-then-add:
// Inventory::removeItemDontDestroy_returnsItem on the SOURCE (the item lands on
// the mouse cursor) then Inventory::tryAddItem on the DESTINATION. We detour the
// _NV_ twins of BOTH (hooking the real body catches virtual + direct calls) and
// REFUSE the add when the source and destination squad characters are owned by
// DIFFERENT clients - the item stays on the cursor / in the source bag, so a
// player can only hand items to the peer's squad by dropping them on the ground.
// Purely LOCAL: only the dragging client runs this, so no packet is involved.
//
// The source is identified two ways (belt + suspenders): the exact remove/add
// pairing captured here (g_pendRem*), and the item's own _whosInventoryWeAreIn
// hand (still the source until the destination add rewrites it) as the fallback
// for a cross-tick mouse-item drag. g_invVetoSuspend is the reentrancy guard the
// Replicator's own sanctioned relocations raise so they are never refused.
typedef Item* (__fastcall* RemoveDontDestroyFn)(Inventory* self, Item* it,
                                                int howmany, bool returnCopyIfSomeLeft);
typedef bool  (__fastcall* TryAddItemFn)(Inventory* self, Item* item, int quantity);
RemoveDontDestroyFn g_removeDontDestroyOrig = 0;
TryAddItemFn        g_tryAddItemOrig        = 0;

bool g_invVetoSuspend = false;            // Replicator's own move in progress (extern)
static bool           g_blockXfer   = false;
static InvOwnerClassFn g_invOwnerClass = 0;
static Inventory*     g_pendRemInv   = 0; // last remove's source inventory (main thread)
static Item*          g_pendRemItem  = 0; // last item removed onto the cursor
static unsigned int   g_pendRemOwnerHand[5] = { 0, 0, 0, 0, 0 }; // its owner-char hand
static bool           g_havePendRemOwner = false;                 // hand above is valid

// KENSHICOOP_INV_DUMP diagnostic gate (read once). When on, EVERY squad<->squad
// drag logs a "[xfer] DRAG" line (src/dst owner class + block decision) - the A1
// evidence for the drag call sequence, thread-affinity and refused-add behaviour.
static int xferDumpFlag() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}

// Read the owner (character/container) hand behind an Inventory into out[5]
// (readObjectHand layout). Caller holds SEH.
static bool ownerHandOfInventory(Inventory* inv, unsigned int out[5]) {
    if (!inv) return false;
    RootObject* o = inv->owner; // Inventory::owner (member 0x88)
    if (!o) return false;
    return readObjectHand(o, out);
}

// Read the hand of the inventory an item last belonged to (its drag source, which
// survives removeItemDontDestroy until the next add rewrites it). Caller holds SEH.
static bool sourceHandOfItem(Item* item, unsigned int out[5]) {
    if (!item) return false;
    const hand& h = item->_whosInventoryWeAreIn;
    out[0] = (unsigned int)h.type; out[1] = h.container; out[2] = h.containerSerial;
    out[3] = h.index; out[4] = h.serial;
    return (out[0] || out[1] || out[2] || out[3] || out[4]);
}

Item* __fastcall removeDontDestroy_hook(Inventory* self, Item* it, int howmany,
                                        bool returnCopyIfSomeLeft) {
    Item* r = g_removeDontDestroyOrig(self, it, howmany, returnCopyIfSomeLeft);
    if ((g_blockXfer || xferDumpFlag()) && !g_invVetoSuspend) {
        __try {
            g_pendRemItem = r ? r : it; g_pendRemInv = self;
            // Resolve + cache the SOURCE owner-character hand NOW, at remove time.
            // A UI drag is a strict remove->add on this frame; the add may receive a
            // COPY of the removed Item* (pointer != g_pendRemItem), so pairing on the
            // pointer alone let cross-squad drags slip through unclassified. The cached
            // hand makes the source reliable regardless of the pointer.
            g_havePendRemOwner = ownerHandOfInventory(self, g_pendRemOwnerHand);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_havePendRemOwner = false; }
    }
    return r;
}

bool __fastcall tryAddItem_hook(Inventory* self, Item* item, int quantity) {
    if ((g_blockXfer || xferDumpFlag()) && !g_invVetoSuspend && g_invOwnerClass &&
        self && item) {
        int decision = 0; // 1 = block
        int srcClass = 0, dstClass = 0;
        bool haveSrc = false, haveDst = false;
        unsigned int dstHand[5] = { 0, 0, 0, 0, 0 };
        unsigned int srcHand[5] = { 0, 0, 0, 0, 0 };
        __try {
            haveDst = ownerHandOfInventory(self, dstHand);
            // Source (in priority): the owner hand cached at the paired remove this
            // frame; else the exact remove/add pointer pairing; else the item's
            // last-inventory hand. The cached hand is the robust path - it survives
            // the engine handing us a COPY of the removed Item*.
            if (g_havePendRemOwner && g_pendRemInv && g_pendRemInv != self) {
                memcpy(srcHand, g_pendRemOwnerHand, sizeof(srcHand)); haveSrc = true;
            }
            if (!haveSrc && item == g_pendRemItem && g_pendRemInv && g_pendRemInv != self)
                haveSrc = ownerHandOfInventory(g_pendRemInv, srcHand);
            if (!haveSrc) haveSrc = sourceHandOfItem(item, srcHand);
            if (haveDst && haveSrc) {
                dstClass = g_invOwnerClass(dstHand);
                srcClass = g_invOwnerClass(srcHand);
                if ((srcClass == 1 && dstClass == 2) || (srcClass == 2 && dstClass == 1))
                    decision = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { decision = 0; }
        // Diagnostic (A1): under dump, log every add that touches a squad member on
        // EITHER end (srcClass||dstClass) - including the misses (class 0 / unresolved
        // hand), so a cross-squad drag that fails to block is no longer silent.
        if (xferDumpFlag() && (srcClass || dstClass || haveSrc || haveDst)) {
            __try {
                char sid[48]; sid[0] = '\0';
                GameData* gd = item->getGameData();
                if (gd) { strncpy(sid, gd->stringID.c_str(), sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0'; }
                char b[240];
                _snprintf(b, sizeof(b) - 1,
                          "[xfer] DRAG sid='%s' src=%d dst=%d block=%d haveSrc=%d haveDst=%d "
                          "srcH=%u,%u,%u,%u,%u dstH=%u,%u,%u,%u,%u", sid,
                          srcClass, dstClass, decision, haveSrc ? 1 : 0, haveDst ? 1 : 0,
                          srcHand[0], srcHand[1], srcHand[2], srcHand[3], srcHand[4],
                          dstHand[0], dstHand[1], dstHand[2], dstHand[3], dstHand[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (g_blockXfer && decision == 1) {
            __try {
                char sid[48]; sid[0] = '\0';
                GameData* gd = item->getGameData();
                if (gd) { strncpy(sid, gd->stringID.c_str(), sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0'; }
                char b[176];
                _snprintf(b, sizeof(b) - 1,
                          "[xfer] BLOCK cross-owner drag sid='%s' src=%d dst=%d (drop on the ground to transfer)",
                          sid, srcClass, dstClass);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            // Cancel the drag CLEANLY: put the item back into its SOURCE inventory
            // (veto suspended so our own re-add is not itself vetoed). Merely
            // returning false strands the item on the cursor with the source count
            // still decreased - which the gear-drop detector then mistakes for a
            // DROP and leaks the item to the peer as a ground item (observed bug).
            // Restoring the count in the same frame also keeps the detector quiet.
            bool restored = false;
            Inventory* src = g_pendRemInv;
            if (src && item) {
                bool sav = g_invVetoSuspend; g_invVetoSuspend = true;
                __try { restored = g_tryAddItemOrig(src, item, quantity); }
                __except (EXCEPTION_EXECUTE_HANDLER) { restored = false; }
                g_invVetoSuspend = sav;
            }
            g_pendRemItem = 0; g_pendRemInv = 0; g_havePendRemOwner = false;
            // If we returned it to the source, report the destination add as handled
            // so the engine clears the cursor (item safe in source, nothing crossed).
            // If the source was unresolved, refuse (item stays on cursor) rather than
            // risk a duplicate.
            return restored ? true : false;
        }
    }
    bool ok = g_tryAddItemOrig(self, item, quantity);
    g_pendRemItem = 0; g_pendRemInv = 0; g_havePendRemOwner = false;
    return ok;
}

// ---- Phase W1b: query-free ground-drop capture -----------------------------
// Detour Inventory::dropItem (the _NV_ twin: hooking the real body catches the
// virtual dispatch AND our own dropItemFromInventory / relocateWeaponToGround
// calls). Every drop records the grounded Item* + owner + description + position
// so the Replicator discovers town drops without the spatial sphere query. We
// call the original FIRST so the engine has already grounded + positioned the
// item when we read itemWorldPos. Same main-thread edge-queue pattern as the
// recruit/save detours (engine tick + plugin tick share the thread; no lock).
typedef void (__fastcall* DropItemFn)(Inventory* self, Item* it);
DropItemFn g_dropItemOrig = 0;
std::vector<ItemDropEdge> g_dropEdges;

void __fastcall dropItem_hook(Inventory* self, Item* it) {
    g_dropItemOrig(self, it);
    // A drop may internally remove the item (setting the veto's pending-remove
    // state) but is NOT a bag->bag transfer, so invalidate any pending remove here
    // - otherwise a later unrelated add could be mis-paired to this drop's source.
    g_pendRemItem = 0; g_pendRemInv = 0; g_havePendRemOwner = false;
    __try {
        if (it) {
            ItemDropEdge e;
            memset(&e, 0, sizeof(e));
            if (self && self->owner) readObjectHand(self->owner, e.ownerHand);
            readObjectHand(static_cast<RootObject*>(it), e.itemHand);
            GameData* gd = it->getGameData();
            if (gd) {
                strncpy(e.stringID, gd->stringID.c_str(), sizeof(e.stringID) - 1);
                e.stringID[sizeof(e.stringID) - 1] = '\0';
                e.itemType = (unsigned int)gd->type;
            }
            int qty = it->quantity; if (qty < 1) qty = 1;
            e.quantity = (unsigned short)(qty > 0xFFFF ? 0xFFFF : qty);
            float ql = it->quality;
            e.quality = (unsigned short)(ql > 0.0f ? (int)(ql * 100.0f) : 0);
            float p[3] = { 0, 0, 0 };
            bool haveP = itemWorldPos(it, p) &&
                         !(p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f);
            if (!haveP && self && self->owner) {
                // A town item often reports its transform as origin the frame it
                // grounds (the exact town-scan failure). Fall back to the dropping
                // character's feet so the peer proxy lands where the player is, not
                // at world (0,0,0) - which is off in the void and looks "missing".
                Ogre::Vector3 op = self->owner->getPosition();
                p[0] = op.x; p[1] = op.y; p[2] = op.z;
                haveP = !(p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f);
            }
            if (haveP) { e.x = p[0]; e.y = p[1]; e.z = p[2]; }
            if (g_dropEdges.size() < 128) g_dropEdges.push_back(e);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// AI-gating probe lever: PlayerInterface::recruit(Character*, bool) adds an NPC
// to the local player's squad (the "inhabit" path). A recruited body stops
// self-assigning town tasks (player chars idle until ordered) and obeys our
// drive, so we can mirror the host without replicating animation data.
typedef bool (__fastcall* RecruitFn)(PlayerInterface* self, Character* c, bool editor);
RecruitFn g_recruitFn = 0;

// Recruitment edge detour (protocol 23). PlayerInterface::recruit is the ONE
// engine path that turns a world NPC into a player-squad member (the dialog
// "join me" outcome and our own programmatic recruits both land here). The
// recruit RE-CONTAINERS the body - its hand changes (recruit_probe: container
// always changes; baked subjects keep their serial, runtime ones keep the
// whole tail) - so the peer can never resolve the NEW hand from its save. The
// detour captures the before/after hand pair of every SUCCESSFUL recruit into
// a small queue the Replicator drains once per tick into EVT_RECRUIT. Engine
// tick and plugin tick share the main thread, so the queue needs no lock.
std::vector<RecruitEdgeRec> g_recruitEdges;
RecruitFn g_recruitHookOrig = 0;
bool __fastcall recruit_hook(PlayerInterface* self, Character* c, bool editor) {
    unsigned int hb[5] = { 0, 0, 0, 0, 0 };
    bool haveBefore = false;
    __try {
        if (c) { readObjectHand(static_cast<RootObject*>(c), hb); haveBefore = true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool ok = g_recruitHookOrig(self, c, editor);
    if (ok && haveBefore) {
        __try {
            RecruitEdgeRec e;
            memcpy(e.before, hb, sizeof(hb));
            memset(e.after, 0, sizeof(e.after));
            readObjectHand(static_cast<RootObject*>(c), e.after);
            if (g_recruitEdges.size() < 64) g_recruitEdges.push_back(e);
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "[recruit] LOCAL before=%u,%u,%u,%u,%u after=%u,%u,%u,%u,%u",
                      e.before[0], e.before[1], e.before[2], e.before[3], e.before[4],
                      e.after[0], e.after[1], e.after[2], e.after[3], e.after[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return ok;
}

// Squad-move edge detection (protocol 35). A squad-tab MOVE re-containers the
// body exactly like a recruit, but there is no single engine function to
// detour for the UI drag - so the roster is POLLED instead: the Character*
// pointer survives re-containering (the protocol-23 re-key evidence), so a
// pointer -> hand baseline diff catches every move flavor. Exits report the
// STORED hand with a zeroed after (never dereferencing the possibly-freed
// pointer); brand-new pointers just seed the baseline (recruit ENTRY is the
// protocol-23 detour's story - double-reporting it here would race the
// EVT_RECRUIT re-key on the peer).
std::map<Character*, SquadHandRec> g_squadRoster;
std::vector<SquadMoveEdge> g_squadMoveEdges;

// Programmatic squad-move levers (probe tier). PlayerInterface::getFaction
// feeds Character::setFaction(faction, targetPlatoon) - the header-documented
// re-platoon path; ActivePlatoon::addCharacterAt is the raw container insert
// fallback. Both non-fatal: unresolved -> lever returns -1 and the probe logs
// the gap.
typedef Faction* (__fastcall* PlayerFactionFn)(const PlayerInterface* self);
typedef void     (__fastcall* AddCharacterAtFn)(ActivePlatoon* self,
                                                RootObject* c, int index);
PlayerFactionFn  g_playerFactionFn = 0;
AddCharacterAtFn g_addCharacterAtFn = 0;

// Build-placement edge detour (protocol 27). PreviewBuilding::
// placeFinalPreviewBuilding is the ONE engine path a player's build-mode
// commit lands on: it constructs the real Building and parks it in
// `justBeenBuilt`. A placed building is a RUNTIME object (host-only hand -
// the protocol-21 identity problem for structures), so the peer can never
// resolve it from its save; the detour captures every successful placement
// (template sid + transform + the placer's local hand as the wire key) into
// a small queue the Replicator drains once per tick into PKT_BUILD_PLACE.
// Engine tick and plugin tick share the main thread - no lock needed.
std::vector<BuildEdgeRec> g_buildEdges;
typedef void (__fastcall* PlaceFinalFn)(PreviewBuilding* self);
PlaceFinalFn g_placeFinalOrig = 0;
void __fastcall placeFinal_hook(PreviewBuilding* self) {
    g_placeFinalOrig(self);
    __try {
        if (!self) return;
        Building* b = self->justBeenBuilt;
        if (!b) return; // commit refused (placement rules) - nothing placed
        BuildEdgeRec e;
        memset(&e, 0, sizeof(e));
        RootObject* ro = static_cast<RootObject*>(b);
        if (!readObjectHand(ro, e.hand)) return;
        GameData* gd = ro->getGameData();
        if (gd) {
            strncpy(e.sid, gd->stringID.c_str(), sizeof(e.sid) - 1);
            e.sid[sizeof(e.sid) - 1] = '\0';
        }
        Ogre::Vector3 p = ro->getPosition();
        e.x = p.x; e.y = p.y; e.z = p.z;
        e.yaw = self->yaw;
        e.floorNum = self->floorNum;
        e.fromUi = 1;
        if (g_buildEdges.size() < 32) g_buildEdges.push_back(e);
        char lb[224];
        _snprintf(lb, sizeof(lb) - 1,
                  "[build] LOCAL-PLACE ui=1 sid='%s' hand=%u.%u.%u.%u.%u pos=%.1f,%.1f,%.1f yaw=%.2f floor=%d",
                  e.sid, e.hand[0], e.hand[1], e.hand[2], e.hand[3], e.hand[4],
                  e.x, e.y, e.z, e.yaw, e.floorNum);
        lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Dismantle edge detour (protocol 28). Building::notifyConstructionDismantling
// is the engine's dismantle-complete notification (the counterpart of
// notifyConstructionComplete). Capturing the hand here catches the UI
// dismantle path; the probe's programmatic destroy queues its edge manually
// (GameWorld::destroy never passes through this notification). The Replicator
// drains the queue into PKT_BUILD_REMOVE and only acts on hands it PLACED -
// baked-building dismantles log but never stream.
std::vector<unsigned int> g_removeEdges; // flat, 5 u32 per edge
typedef void (__fastcall* DismantleFn)(Building* self);
DismantleFn g_dismantleOrig = 0;
static void pushRemoveEdge(const unsigned int h[5]) {
    if (g_removeEdges.size() >= 5 * 32) return;
    for (int i = 0; i < 5; ++i) g_removeEdges.push_back(h[i]);
}
void __fastcall dismantle_hook(Building* self) {
    unsigned int h[5] = { 0, 0, 0, 0, 0 };
    bool haveHand = false;
    __try {
        // Read the hand BEFORE the original runs: the notification may tear
        // the building down and the hand is unreadable afterwards.
        if (self) haveHand = readObjectHand(static_cast<RootObject*>(self), h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_dismantleOrig(self);
    if (haveHand) {
        __try {
            pushRemoveEdge(h);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "[build] LOCAL-DISMANTLE hand=%u.%u.%u.%u.%u",
                      h[0], h[1], h[2], h[3], h[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

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

// ---- Task-selection observation spike (KENSHICOOP_TASK_SPIKE) --------------
// The theoretical AI cut point (design chat 2026-07-18): CharBody::setCurrentAction
// is the ONE seam every task-SELECTION result (AI scorer OR player order) flows
// through before the body EXECUTES it - virtual vtable 0x18 / RVA 0x5C6740, wholly
// separate from the whole-brain Character::_NV_periodicUpdate we suspend today. If
// this passive detour fires, it proves the seam is hookable INDEPENDENTLY of the
// periodic-update hook (the open spike question) and that "which task won" is
// interceptable in isolation - the precondition for streaming selection instead of
// suppressing the brain wholesale. It changes NOTHING: it reads the incoming
// Tasker's (task key, subject, location) tuple and calls the original. Logging is
// globally throttled; off by default (log volume). Members are read raw (no engine
// calls) under SEH so a torn Tasker/CharBody can never fault the engine tick.
typedef bool (__fastcall* SetCurrentActionFn)(CharBody* self, Tasker* t);
SetCurrentActionFn g_setActionOrig    = 0;
bool               g_taskSelectSpike  = false;
unsigned long      g_taskSelectFires  = 0; // total detour fires (reachability proof)

bool __fastcall setCurrentAction_hook(CharBody* self, Tasker* t) {
    if (g_taskSelectSpike && self && t) {
        __try {
            ++g_taskSelectFires;
            Character* c   = self->character;                 // CharBody 0x18
            TaskData*  td  = t->taskData;                     // Tasker  0x70
            int        key = td ? (int)td->key : -1;          // TaskData 0x44
            hand       subj = t->subject;                     // Tasker  0x10
            Ogre::Vector3 loc = t->location;                  // Tasker  0x58
            char nm[40]; nm[0] = '\0';
            if (c) charName(c, nm, sizeof(nm));
            static unsigned long logTick = 0; // ~4 lines/s across all bodies
            unsigned long now = GetTickCount();
            if ((now - logTick) >= 250) {
                logTick = now;
                char b[192];
                _snprintf(b, sizeof(b) - 1,
                    "[spike] SELECT body='%s' task=%d subj=%u,%u loc=%.0f,%.0f,%.0f fires=%lu",
                    nm, key, subj.index, subj.serial,
                    loc.x, loc.y, loc.z, g_taskSelectFires);
                b[sizeof(b) - 1] = '\0';
                coop::logLine(b);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // read fault: skip logging, still fall through to the original below
        }
    }
    return g_setActionOrig(self, t);
}

// Join-side damage guard. Character::hitByMeleeAttack is where a landed melee
// swing applies its Damages to the victim (wounds, blood loss, KO math). On the
// join, fights involving host-authoritative bodies are COSMETIC - intent
// replication makes the copies swing at each other so the fight renders, but the
// outcome (KO/death) arrives from the host as reliable events. Without this
// detour the cosmetic swings apply REAL local damage to the join's copies, and
// Kenshi's medical model is local-only (spikes 21-27: blood/limbs/bleed never
// cross the wire), so that damage silently diverges forever. For guarded victims
// we skip the engine's hit path entirely and report HIT_MISSED - no damage, no
// wound, no local KO; posture/outcome remain host-authoritative.
// Same safety shape as periodicUpdate_hook: 'self' is only compared as a key.
typedef HitMaterialType (__fastcall* HitByMeleeFn)(
    Character* self, CutDirection dir, Damages& damage, Character* who,
    CombatTechniqueData* attack, int comboID);
HitByMeleeFn          g_hitByMeleeOrig = 0;
std::set<Character*>  g_damageGuarded;
unsigned long         g_dmgGuardedHits = 0; // swings intercepted (conformance signal)
unsigned long         g_dmgPassedHits  = 0; // swings passed through to the engine

HitMaterialType __fastcall hitByMelee_hook(Character* self, CutDirection dir,
                                           Damages& damage, Character* who,
                                           CombatTechniqueData* attack, int comboID) {
    if (!g_damageGuarded.empty() &&
        g_damageGuarded.find(self) != g_damageGuarded.end()) {
        ++g_dmgGuardedHits;
        return HIT_MISSED; // cosmetic fight: the local swing never lands
    }
    ++g_dmgPassedHits;
    return g_hitByMeleeOrig(self, dir, damage, who, attack, comboID);
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
// RootObjectFactory::create - the GENERIC "spawn any RootObject into the world at a
// position" path (createBuilding/createRandomCharacter are specialized wrappers). Used
// (Phase W1) to spawn a JOIN-side ground-item proxy from an item template at a world
// position. Same by-value Vector3/Quaternion ABI as createBuilding (large structs go by
// hidden reference). Returns the spawned object as RootObjectBase* (== the Item address).
typedef RootObjectBase* (__fastcall* CreateObjFn)(
    RootObjectFactory* self, GameData* data, Ogre::Vector3 position,
    bool isFromActiveLevelMod, Faction* owner, Ogre::Quaternion rotation,
    FactoryCallbackInterface* cb, RootObjectContainer* certainContainer,
    GameSaveState* state, bool invisible, Building* homeBuilding, float age);
// GameWorld::destroy(RootObject*, justUnloaded, debugInfo) - engine removal of a dynamic
// object (overloaded, so resolve via an explicit member-pointer cast). justUnloaded=false
// = true destruction. Used to CULL a join-side world-item proxy on despawn. After this
// the pointer is DEAD.
typedef bool (__fastcall* DestroyObjFn)(
    GameWorld* self, RootObject* obj, bool justUnloaded, const char* debugInfo);
// RootObjectFactory::createItem (NON-virtual; resolve via GetRealAddress like
// createBuilding). The `const hand&` argument is passed by reference == a pointer.
typedef Item* (__fastcall* CreateItemFn)(
    RootObjectFactory* self, GameData* gd, const hand* handle, GameData* weaponMesh,
    GameData* matData, int levelOverride, Faction* flagUniform);
// Inventory::equipItem (NON-virtual) - move a loose item into its equipment slot.
typedef bool (__fastcall* EquipItemFn)(Inventory* self, Item* item);
// Inventory::getAllSections (NON-virtual): the canonical list of EVERY inventory
// section. Worn gear lives in the equip SECTIONS (one per slot: each weapon, each
// armour piece, belt, backpack, ...), NOT in the loose _allItems list. Walking all
// sections and keeping the ones flagged isAnEquippedItemSection captures every
// equippable slot uniformly - the old per-getter approach (getEquippedArmour/
// getEquippedWeapons) silently missed slots such as the weapon holster.
typedef lektor<InventorySection*>* (__fastcall* GetAllSectionsFn)(Inventory* self);
// Inventory::getPrimaryWeapon / getSecondaryWeapon (NON-virtual): worn weapons do NOT
// live in any getAllSections() section - the engine keeps them in these dedicated
// accessors. We must read them explicitly to capture equipped weapons. Weapon derives
// from Item via a single-inheritance chain (InventoryItemBase->Item->Gear->Weapon), so
// a Weapon* is numerically an Item* and can be used wherever an Item* is expected.
typedef Item* (__fastcall* GetWeaponFn)(Inventory* self);
// Protocol 21 runtime-spawn proxies: FactionManager::getFactionByStringID gives
// the join a LIVE Faction* from the wire's faction stringID; Faction::getData
// gives the host that stringID off a runtime spawn's live faction (both
// non-virtual, so resolved like every other engine call).
typedef Faction*  (__fastcall* FacBySidFn)(FactionManager* self, const std::string* sid);
typedef GameData* (__fastcall* FacGetDataFn)(const Faction* self);
// Protocol 24 faction-relation sync: read/write the relation table between the
// player faction and world factions. All non-virtual (or called via their _NV_
// RVA), resolved like every other engine call. setRelation writes the raw
// relation float on ONE side's table; isEnemy/isAlly are the derived flags the
// AI actually consults.
typedef float (__fastcall* RelGetFn)(FactionRelations* self, Faction* p);
typedef void  (__fastcall* RelSetFn)(FactionRelations* self, Faction* who, float setTo);
typedef bool  (__fastcall* RelBoolFn)(FactionRelations* self, Faction* c);

CreateCharFn     g_createCharFn   = 0;
CreateBuildingFn g_createBldgFn   = 0;
CreateObjFn      g_createObjFn    = 0; // Phase W1 world-item proxy spawn
DestroyObjFn     g_destroyObjFn   = 0; // Phase W1 world-item proxy cull
GetDataOfTypeFn  g_getDataOfTypeFn = 0;
CreateItemFn     g_createItemFn   = 0;
EquipItemFn      g_equipItemFn    = 0;
GetAllSectionsFn g_getSectionsFn  = 0;
GetWeaponFn      g_getPrimaryWeaponFn   = 0;
GetWeaponFn      g_getSecondaryWeaponFn = 0;
FacBySidFn       g_facBySidFn     = 0; // protocol 21 proxy spawn (join)
FacGetDataFn     g_facGetDataFn   = 0; // protocol 21 describe (host)
RelGetFn         g_relGetFn       = 0; // protocol 24 relation read
RelSetFn         g_relSetFn       = 0; // protocol 24 relation write
RelBoolFn        g_relIsEnemyFn   = 0; // protocol 24 derived hostility flag
RelBoolFn        g_relIsAllyFn    = 0; // protocol 24 derived alliance flag

// The faction's GameData stringID - the save-stable cross-client identity
// (protocol 21 already round-trips it for proxy spawns). "" when unreadable.
void facSidOf(Faction* f, char* out, unsigned int outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (!f || !g_facGetDataFn) return;
    __try {
        GameData* gd = g_facGetDataFn(f);
        if (gd) {
            strncpy(out, gd->stringID.c_str(), outLen - 1);
            out[outLen - 1] = '\0';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Faction-relation mutation detours (protocol 24). FactionRelations::
// affectRelations is the engine's own relation-change path and has TWO
// overloads: the EVENT one (cause enum -> computed amount) and the raw AMOUNT
// one (dialog effects, direct adjustments). Both are detoured: each fires a
// "[fac] AFFECT-EV/AMT" log line (cause-attribution evidence) and records a
// delta the Replicator can drain (the join forwards its local mutations to
// the host, the treatments pattern). If one overload calls the other
// internally the log exposes the call graph - the forwarder dedupes by only
// draining what the sync layer decides to forward.
std::vector<FactionDeltaRec> g_facDeltas;
typedef void (__fastcall* AffectRelEvFn)(FactionRelations* self, Faction* p, int e, float mult);
typedef void (__fastcall* AffectRelAmtFn)(FactionRelations* self, Faction* p, float amount, float mult);
AffectRelEvFn  g_affectEvOrig  = 0;
AffectRelAmtFn g_affectAmtOrig = 0;

// Per-pair log debounce (2026-07-11 field session: an NPC-vs-NPC wildlife war
// on the join fired 1,840 AFFECT lines in minutes - two factions re-asserting
// an already-clamped relation every engine tick). The DELTA RECORDING is
// untouched (the sync layer drains g_facDeltas); only the log line is gated:
// each (me, whom) pair logs at most once per 5 s, with the skipped count
// carried into the next emitted line. POD-only (SEH legal), oldest-slot reuse.
FacLogGate g_facLogGates[16];

void recordFactionDelta(FactionRelations* self, Faction* p, int isEvent,
                        int ev, float amount, float mult) {
    __try {
        FactionDeltaRec r;
        memset(&r, 0, sizeof(r));
        facSidOf(self ? self->me : 0, r.meSid, sizeof(r.meSid));
        facSidOf(p, r.whomSid, sizeof(r.whomSid));
        r.isEvent = isEvent; r.ev = ev; r.amount = amount; r.mult = mult;
        r.after = -999.0f;
        if (self && p && g_relGetFn) r.after = g_relGetFn(self, p);
        if (g_facDeltas.size() < 64) g_facDeltas.push_back(r);
        unsigned long now = GetTickCount();
        FacLogGate* gate = 0;
        FacLogGate* oldest = &g_facLogGates[0];
        for (int gi = 0; gi < 16; ++gi) {
            FacLogGate* g = &g_facLogGates[gi];
            if (g->lastMs != 0 && strcmp(g->meSid, r.meSid) == 0 &&
                strcmp(g->whomSid, r.whomSid) == 0) { gate = g; break; }
            if (g->lastMs < oldest->lastMs) oldest = g;
        }
        if (!gate) {
            gate = oldest;
            strncpy(gate->meSid, r.meSid, sizeof(gate->meSid) - 1);
            gate->meSid[sizeof(gate->meSid) - 1] = '\0';
            strncpy(gate->whomSid, r.whomSid, sizeof(gate->whomSid) - 1);
            gate->whomSid[sizeof(gate->whomSid) - 1] = '\0';
            gate->lastMs = 0; gate->skipped = 0;
        }
        if (gate->lastMs != 0 && (now - gate->lastMs) < 5000) {
            ++gate->skipped;
            return;
        }
        gate->lastMs = now ? now : 1;
        char b[224];
        if (isEvent) {
            _snprintf(b, sizeof(b) - 1,
                      "[fac] AFFECT-EV me='%s' whom='%s' event=%d mult=%.3f after=%.2f skipped=%lu",
                      r.meSid, r.whomSid, ev, mult, r.after, gate->skipped);
        } else {
            _snprintf(b, sizeof(b) - 1,
                      "[fac] AFFECT-AMT me='%s' whom='%s' amount=%.3f mult=%.3f after=%.2f skipped=%lu",
                      r.meSid, r.whomSid, amount, mult, r.after, gate->skipped);
        }
        gate->skipped = 0;
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall affectRelEv_hook(FactionRelations* self, Faction* p, int e, float mult) {
    g_affectEvOrig(self, p, e, mult);
    recordFactionDelta(self, p, 1, e, 0.0f, mult);
}

void __fastcall affectRelAmt_hook(FactionRelations* self, Faction* p, float amount, float mult) {
    g_affectAmtOrig(self, p, amount, mult);
    recordFactionDelta(self, p, 0, 0, amount, mult);
}

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
// Carried-body sync (protocol 18): Character::isBeingCarried, same bare bool
// getter ABI as the body-state reads above.
CharBodyBoolFn   g_isBeingCarriedFn = 0;

// Combat reads (L5 probe, Phase 3c). getAttackTarget returns a `hand` BY VALUE, so
// it uses the same member-struct-return ABI as getBoneWorldPosition: this=RCX,
// hidden return-buffer pointer=RDX (model it as `hand* fn(Character*, hand*)`).
// isInCombatMode(melee, ranged) takes two bools; the remaining flags are bare bool
// getters that reuse CharBodyBoolFn (this=RCX, returns bool).
typedef hand* (__fastcall* GetAttackTargetFn)(Character* self, hand* ret);
typedef bool  (__fastcall* InCombatModeFn)(Character* self, bool melee, bool ranged);
typedef CombatClass* (__fastcall* GetCombatClassFn)(Character* self);
GetAttackTargetFn g_getAttackTargetFn = 0;
InCombatModeFn    g_inCombatModeFn    = 0;
GetCombatClassFn  g_getCombatClassFn  = 0;
AttackTargetFn    g_attackTargetFn    = 0;
CharBodyBoolFn    g_inRangedModeFn    = 0;
CharBodyBoolFn    g_underMeleeFn      = 0;
CharBodyBoolFn    g_fleeingFn         = 0;

lektor<GameData*> g_dataScratch; // reused seat-template scan buffer (main thread)

// One reused scratch buffer for the interest query (main thread only).
lektor<RootObject*> g_npcQuery;

// ---- Phase 5d: owned capability registry ----------------------------------
// Per-capability availability, folded from the CapRow table at the END of
// resolve(). Fail-closed: all false until g_capsEvaluated flips, so capAvailable
// reports "off" for the whole window before resolution has actually happened.
static bool g_capAvail[CAP_COUNT] = { false };
static bool g_capsEvaluated = false;

bool capAvailable(Capability c) {
    if (!g_capsEvaluated || c < 0 || c >= CAP_COUNT) return false;
    return g_capAvail[c];
}

void resolve() {
    g_getFn  = (SaveMgrGetFn)KenshiLib::GetRealAddress(&SaveManager::getSingleton);
    g_loadFn = (SaveMgrLoadNameFn)KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const std::string&)>(&SaveManager::load));
    g_savesExistFn = (SaveMgrSavesExistFn)KenshiLib::GetRealAddress(&SaveManager::savesExist);
    g_saveFn = (SaveMgrSaveNameFn)KenshiLib::GetRealAddress(&SaveManager::save);
    g_saveMgrExecFn = (SaveMgrExecFn)KenshiLib::GetRealAddress(&SaveManager::execute);
    // Protocol 31: locate the active save on disk (spike 39 RVAs, runtime-
    // validated by save_probe). Non-fatal: saveInfo falls back to the
    // %LOCALAPPDATA%\kenshi\save convention the harness already assumes.
    g_saveMgrCurGameFn = (SaveMgrStrFn)KenshiLib::GetRealAddress(&SaveManager::getCurrentGame);
    g_saveMgrPathFn    = (SaveMgrStrFn)KenshiLib::GetRealAddress(&SaveManager::getSavePath);
    // Entity resolve path: hand->Character lookup and the 5-arg hand ctor
    // (overloaded, so disambiguate via an explicit member-pointer cast).
    g_handGetCharFn = (HandGetCharFn)KenshiLib::GetRealAddress(&hand::getCharacter);
    g_handCtorFn = (HandCtorFn)KenshiLib::GetRealAddress(
        static_cast<hand* (hand::*)(unsigned int, unsigned int, itemType,
                                    unsigned int, unsigned int)>(&hand::_CONSTRUCTOR));
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

    // Limb-loss replication (protocol 16; non-fatal: unresolved -> limb sync off).
    g_medAmputateFn  = (MedAmputateFn)KenshiLib::GetRealAddress(&MedicalSystem::amputate);
    g_medCrushLimbFn = (MedCrushLimbFn)KenshiLib::GetRealAddress(&MedicalSystem::crushLimb);
    g_medSetRobotLimbFn = (MedSetRobotLimbFn)KenshiLib::GetRealAddress(
        &MedicalSystem::setRobotLimbItem);
    g_medGetLimbStateFn = (MedGetLimbStateFn)KenshiLib::GetRealAddress(
        &MedicalSystem::getLimbState);
    // Character stats sync (protocol 17; non-fatal: unresolved -> stats sync off).
    g_statsGetRefFn = (StatsGetRefFn)KenshiLib::GetRealAddress(&CharStats::getStatRef);
    g_statsRecalcFn = (StatsRecalcFn)KenshiLib::GetRealAddress(&CharStats::_NV_periodicUpdate);
    // Carried-body sync (protocol 18; non-fatal: unresolved -> carry sync off).
    g_pickupObjectFn = (PickupObjectFn)KenshiLib::GetRealAddress(&Character::pickupObject);
    g_dropCarriedFn  = (DropCarriedFn)KenshiLib::GetRealAddress(&Character::dropCarriedObject);
    g_isBeingCarriedFn = (CharBodyBoolFn)KenshiLib::GetRealAddress(&Character::isBeingCarried);
    // Furniture occupancy (protocol 19; non-fatal: unresolved -> occupancy sync off).
    g_setBedModeFn    = (SetFurnModeFn)KenshiLib::GetRealAddress(&Character::setBedMode);
    g_setPrisonModeFn = (SetFurnModeFn)KenshiLib::GetRealAddress(&Character::setPrisonMode);
    // Chained/pole prisoner (protocol 41; non-fatal: unresolved -> chain sync off).
    g_setChainedModeFn = (SetChainedModeFn)KenshiLib::GetRealAddress(&Character::setChainedMode);
    // Phase 6 shackle read lever (non-fatal: unresolved -> readShackle reports
    // only Character::isChained, no shackle-item/lock discrimination).
    g_getShacklesFn = (GetShacklesFn)KenshiLib::GetRealAddress(&Character::getChainedModeShackles);    // Jail-probe slave-state read (non-fatal: unresolved -> readSlaveState = -1).
    g_isSlaveFn = (IsSlaveFn)KenshiLib::GetRealAddress(&Character::isSlave);
    // Stealth sync (protocol 20; non-fatal: unresolved -> stealth sync off).
    g_setStealthModeFn = (SetStealthModeFn)KenshiLib::GetRealAddress(&Character::setStealthMode);
    g_notifySeeSneakFn = (NotifySeeSneakFn)KenshiLib::GetRealAddress(&Character::notifyICanSeeYouSneaking);
    // Camera-anchored interest (spike 35; non-fatal: unresolved -> camera
    // anchor off, interest falls back to squad-tab leaders only).
    g_camGetCenterFn = (CamGetCenterFn)KenshiLib::GetRealAddress(&CameraClass::getCenter);
    g_camIsInitFn    = (CamIsInitFn)KenshiLib::GetRealAddress(&CameraClass::isInitialised);
    // Consensus game-speed sync (non-fatal: unresolved -> speed sync off).
    g_setGameSpeedFn = (SetGameSpeedFn)KenshiLib::GetRealAddress(&GameWorld::setGameSpeed);
    g_userPauseFn    = (UserPauseFn)KenshiLib::GetRealAddress(&GameWorld::userPause);    g_togglePauseFn = (UserPauseFn)KenshiLib::GetRealAddress(&GameWorld::togglePause);
    g_setFrameSpeedMultFn =
        (SetFrameSpeedMultFn)KenshiLib::GetRealAddress(&GameWorld::setFrameSpeedMultiplier);
    // Door/gate state (protocol 26; non-fatal: unresolved -> door sync off).
    g_doorIsOpenFn     = (DoorBoolFn)KenshiLib::GetRealAddress(&DoorStuff::isOpen);
    g_doorIsLockedFn   = (DoorBoolFn)KenshiLib::GetRealAddress(&DoorStuff::isLocked);
    g_doorOpenFn       = (DoorActFn)KenshiLib::GetRealAddress(&DoorStuff::openDoor);
    g_doorCloseFn      = (DoorActFn)KenshiLib::GetRealAddress(&DoorStuff::closeDoor);
    g_doorForceOpenFn  = (DoorActFn)KenshiLib::GetRealAddress(&DoorStuff::_forceDoorOpenUT);
    g_doorForceCloseFn = (DoorActFn)KenshiLib::GetRealAddress(&DoorStuff::_forceDoorClosedUT);
    g_doorLockFn       = (DoorVoidFn)KenshiLib::GetRealAddress(&DoorStuff::lockDoor);
    g_doorUnlockFn     = (DoorVoidFn)KenshiLib::GetRealAddress(&DoorStuff::unlockDoor);
    // Construction progress (protocol 27; non-fatal: unresolved -> build sync off).
    g_buildSetProgFn = (BuildProgFn)KenshiLib::GetRealAddress(
        &Building::_NV_setConstructionProgress);
    g_buildAddProgFn = (BuildProgFn)KenshiLib::GetRealAddress(
        &Building::_NV_addConstructionProgress);
    g_buildNotifyDoneFn = (BuildDoneFn)KenshiLib::GetRealAddress(
        &Building::_NV_notifyConstructionComplete);
    // Production machine levers (protocol 33; non-fatal: unresolved -> prod sync off).
    g_machPowerFn        = (MachPowerSetFn)KenshiLib::GetRealAddress(
        &UseableStuff::_NV_switchPowerOn);
    g_machPowerOutBaseFn = (MachPowerOutFn)KenshiLib::GetRealAddress(
        &UseableStuff::_NV_getPowerOutput);
    g_machPowerOutGenFn  = (MachPowerOutFn)KenshiLib::GetRealAddress(
        &GeneratorBuilding::_NV_getPowerOutput);
    g_machIsGenFn        = (MachIsGenFn)KenshiLib::GetRealAddress(
        &UseableStuff::isGenerator);
    g_machSetProdItemFn  = (MachSetProdItemFn)KenshiLib::GetRealAddress(
        &ProductionBuilding::_NV_setProductionItem);
    g_machTechLvlFn      = (MachTechLvlFn)KenshiLib::GetRealAddress(
        &ResearchBuilding::_NV_getTechLevel);
    g_machOperateProdFn  = (MachOperateFn)KenshiLib::GetRealAddress(
        &ProductionBuilding::_NV_operate);
    g_machOperateCraftFn = (MachOperateFn)KenshiLib::GetRealAddress(
        &CraftingBuilding::_NV_operate);
    g_machOperateFarmFn  = (MachOperateFn)KenshiLib::GetRealAddress(
        &FarmBuilding::_NV_operate);
    g_machOperateResearchFn = (MachOperateFn)KenshiLib::GetRealAddress(
        &ResearchBuilding::_NV_operate);
    g_machProdDataBaseFn  = (MachProdItemDataFn)KenshiLib::GetRealAddress(
        &StorageBuilding::_NV_getProductionItemData);
    g_machProdDataCraftFn = (MachProdItemDataFn)KenshiLib::GetRealAddress(
        &CraftingBuilding::_NV_getProductionItemData);
    // Game-clock reads (protocol 25; non-fatal: unresolved -> time sync off).
    g_getTimeHoursFn = (GetTimeHoursFn)KenshiLib::GetRealAddress(
        &GameWorld::getTimeStamp_inGameHours);
    g_getHourLenFn = (GetHourLenFn)KenshiLib::GetRealAddress(
        &GameWorld::getLengthOfHourInRealSeconds);
    // AI-gating probe lever (non-fatal: only used when the probe is enabled).
    g_recruitFn = (RecruitFn)KenshiLib::GetRealAddress(
        static_cast<bool (PlayerInterface::*)(Character*, bool)>(&PlayerInterface::recruit));

    // Squad-move probe levers (protocol 35; non-fatal: unresolved -> the
    // probeMoveSquadMember lever returns -1 and squad_probe logs the gap).
    g_playerFactionFn = (PlayerFactionFn)KenshiLib::GetRealAddress(
        &PlayerInterface::getFaction);
    g_addCharacterAtFn = (AddCharacterAtFn)KenshiLib::GetRealAddress(
        &ActivePlatoon::addCharacterAt);

    // Money + vendor trading (protocol 22 groundwork; non-fatal: unresolved ->
    // wallet reads return -1 and the shop_probe logs the gap).
    g_getPlatoonFn  = (GetPlatoonFn)KenshiLib::GetRealAddress(&Character::getPlatoon);
    g_ownGetMoneyFn = (OwnGetMoneyFn)KenshiLib::GetRealAddress(&Ownerships::getMoney);
    g_ownSetMoneyFn = (OwnSetMoneyFn)KenshiLib::GetRealAddress(&Ownerships::setMoney);
    g_buyItemFn     = (BuyItemFn)KenshiLib::GetRealAddress(&Inventory::buyItem);
    g_platoonRefreshInvFn =
        (PlatoonRefreshInvFn)KenshiLib::GetRealAddress(&ActivePlatoon::refreshInventory);
    g_shopGetTraderFn = (ShopGetTraderFn)KenshiLib::GetRealAddress(&ShopTrader::getTrader);
    // Test-scene spawn fns (non-fatal: only used by the host-side setup step).
    g_createCharFn = (CreateCharFn)KenshiLib::GetRealAddress(
        &RootObjectFactory::createRandomCharacter);
    g_createBldgFn = (CreateBuildingFn)KenshiLib::GetRealAddress(
        &RootObjectFactory::createBuilding);
    // Phase W1 world-item proxy spawn/cull (non-fatal: unresolved -> world-item sync off).
    g_createObjFn = (CreateObjFn)KenshiLib::GetRealAddress(&RootObjectFactory::create);
    g_destroyObjFn = (DestroyObjFn)KenshiLib::GetRealAddress(
        static_cast<bool (GameWorld::*)(RootObject*, bool, const char*)>(&GameWorld::destroy));
    // Phase 1 spawn parity (non-fatal: unresolved -> far minting stays
    // radius-gated). Lives in game/ZoneQuery.cpp (header-collision quarantine).
    resolveZoneQuery();
    g_getDataOfTypeFn = (GetDataOfTypeFn)KenshiLib::GetRealAddress(
        &GameDataContainer::getDataOfType);
    // Phase 4a inventory: createItem is overloaded, so disambiguate the 6-arg form.
    g_createItemFn = (CreateItemFn)KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*,
                                                 GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    // Equipped-gear sync: equipItem is non-virtual, single overload.
    g_equipItemFn = (EquipItemFn)KenshiLib::GetRealAddress(&Inventory::equipItem);
    // Worn gear lives in equip sections, not _allItems: enumerate ALL sections and
    // keep the equipped ones (covers every slot, incl. weapons + worn backpack).
    g_getSectionsFn = (GetAllSectionsFn)KenshiLib::GetRealAddress(&Inventory::getAllSections);
    // Worn weapons are NOT in any section: read the dedicated weapon accessors directly.
    g_getPrimaryWeaponFn   = (GetWeaponFn)KenshiLib::GetRealAddress(&Inventory::getPrimaryWeapon);
    g_getSecondaryWeaponFn = (GetWeaponFn)KenshiLib::GetRealAddress(&Inventory::getSecondaryWeapon);
    // Protocol 21 runtime-spawn proxies (non-fatal: unresolved -> spawn sync off).
    g_facBySidFn   = (FacBySidFn)KenshiLib::GetRealAddress(
        &FactionManager::getFactionByStringID);
    g_facGetDataFn = (FacGetDataFn)KenshiLib::GetRealAddress(&Faction::getData);

    // Faction-relation accessors (protocol 24 groundwork; non-fatal: unresolved
    // -> relation reads return sentinel -999 and the faction_probe logs the gap).
    g_relGetFn     = (RelGetFn)KenshiLib::GetRealAddress(&FactionRelations::getFactionRelation);
    g_relSetFn     = (RelSetFn)KenshiLib::GetRealAddress(&FactionRelations::setRelation);
    g_relIsEnemyFn = (RelBoolFn)KenshiLib::GetRealAddress(&FactionRelations::isEnemy);
    g_relIsAllyFn  = (RelBoolFn)KenshiLib::GetRealAddress(&FactionRelations::isAlly);
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
    g_getCombatClassFn = (GetCombatClassFn)KenshiLib::GetRealAddress(
        &Character::getCombatClass);
    g_inRangedModeFn = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        &Character::isInRangedCombatMode);
    g_underMeleeFn   = (CharBodyBoolFn)KenshiLib::GetRealAddress(
        &Character::isLiterallyUnderMeleeAttackRightNowForSure);
    g_fleeingFn      = (CharBodyBoolFn)KenshiLib::GetRealAddress(&Character::isFleeing);
    // Engagement escalation: the AI's own commit-an-attack entry (non-fatal:
    // unresolved -> escalation degrades to the goal/order paths).
    g_attackTargetFn = (AttackTargetFn)KenshiLib::GetRealAddress(&Character::attackTarget);

    // ---- Phase 5d: fold the resolved pointers into the capability registry ---
    // ONE table naming which resolved slots back each engine capability. The
    // typed GetRealAddress(&Class::method) assignments stay inline above (a
    // member-function pointer is a heterogeneous compile-time type that can't
    // live in a uniform C++03 array), but their success/failure is now judged
    // and REPORTED here in one place instead of scattered hand-written null
    // checks. A missing REQUIRED slot fails its whole capability and logs a
    // named "[engine] CAP-MISS op=<name> cap=<cap>" line the oracles can watch;
    // the "[engine] CAPS <name>=<0|1> ..." fingerprint records the full picture.
    {
        static const CapRow kCapRows[] = {
            { (void**)&g_getFn,             "SaveManager::get",              CAP_SAVELOAD,      true },
            { (void**)&g_loadFn,            "SaveManager::load",             CAP_SAVELOAD,      true },
            { (void**)&g_saveMgrCurGameFn,  "SaveManager::getCurrentGame",   CAP_SAVEPATH,      true },
            { (void**)&g_saveMgrPathFn,     "SaveManager::getSavePath",      CAP_SAVEPATH,      true },
            { (void**)&g_handGetCharFn,     "hand::getCharacter",            CAP_HAND_RESOLVE,  true },
            { (void**)&g_handCtorFn,        "hand::ctor5",                   CAP_HAND_RESOLVE,  true },
            { (void**)&g_getCharsFn,        "getCharactersWithinSphere",     CAP_NPC_STREAM,    true },
            { (void**)&g_medAmputateFn,     "MedicalSystem::amputate",       CAP_LIMB,          true },
            { (void**)&g_medCrushLimbFn,    "MedicalSystem::crushLimb",      CAP_LIMB,          true },
            { (void**)&g_statsGetRefFn,     "CharStats::getStatRef",         CAP_STATS,         true },
            { (void**)&g_pickupObjectFn,    "Character::pickupObject",       CAP_CARRY,         true },
            { (void**)&g_dropCarriedFn,     "Character::dropCarriedObject",  CAP_CARRY,         true },
            { (void**)&g_setBedModeFn,      "Character::setBedMode",         CAP_FURNITURE,     true },
            { (void**)&g_setPrisonModeFn,   "Character::setPrisonMode",      CAP_FURNITURE,     true },
            { (void**)&g_setChainedModeFn,  "Character::setChainedMode",     CAP_CHAIN,         true },
            { (void**)&g_getShacklesFn,     "Character::getChainedShackles", CAP_SHACKLE,       true },
            { (void**)&g_isSlaveFn,         "Character::isSlave",            CAP_SLAVE,         true },
            { (void**)&g_setStealthModeFn,  "Character::setStealthMode",     CAP_STEALTH,       true },
            { (void**)&g_notifySeeSneakFn,  "Character::notifySeeSneaking",  CAP_STEALTH,       true },
            { (void**)&g_camGetCenterFn,    "CameraClass::getCenter",        CAP_CAMERA,        true },
            { (void**)&g_camIsInitFn,       "CameraClass::isInitialised",    CAP_CAMERA,        true },
            { (void**)&g_setGameSpeedFn,    "GameWorld::setGameSpeed",       CAP_SPEED,         true },
            { (void**)&g_userPauseFn,       "GameWorld::userPause",          CAP_SPEED,         true },
            { (void**)&g_setFrameSpeedMultFn,"GameWorld::setFrameSpeedMult", CAP_QUIET_SPEED,   true },
            { (void**)&g_doorIsOpenFn,      "DoorStuff::isOpen",             CAP_DOOR,          true },
            { (void**)&g_doorOpenFn,        "DoorStuff::openDoor",           CAP_DOOR,          true },
            { (void**)&g_doorCloseFn,       "DoorStuff::closeDoor",          CAP_DOOR,          true },
            { (void**)&g_buildSetProgFn,    "Building::setConstructionProg", CAP_BUILD,         true },
            { (void**)&g_buildAddProgFn,    "Building::addConstructionProg", CAP_BUILD,         true },
            { (void**)&g_machPowerFn,       "UseableStuff::switchPowerOn",   CAP_MACHINE,       true },
            { (void**)&g_machSetProdItemFn, "ProductionBuilding::setProdItem",CAP_MACHINE,      true },
            { (void**)&g_machOperateProdFn, "ProductionBuilding::operate",   CAP_MACHINE,       true },
            { (void**)&g_getTimeHoursFn,    "GameWorld::getTimeStampHours",  CAP_TIME,          true },
            { (void**)&g_getHourLenFn,      "GameWorld::getHourLenSeconds",  CAP_TIME,          true },
            { (void**)&g_getPlatoonFn,      "Character::getPlatoon",         CAP_WALLET,        true },
            { (void**)&g_ownGetMoneyFn,     "Ownerships::getMoney",          CAP_WALLET,        true },
            { (void**)&g_ownSetMoneyFn,     "Ownerships::setMoney",          CAP_WALLET,        true },
            { (void**)&g_relGetFn,          "FactionRelations::getRelation", CAP_FACTION,       true },
            { (void**)&g_relSetFn,          "FactionRelations::setRelation", CAP_FACTION,       true },
            { (void**)&g_attackTargetFn,    "Character::attackTarget",       CAP_COMBAT_ESCALATE,true }
        };
        const int nRows = (int)(sizeof(kCapRows) / sizeof(kCapRows[0]));
        capEvaluate(kCapRows, nRows, g_capAvail);
        g_capsEvaluated = true;

        char capLine[192];
        for (int i = 0; i < nRows; ++i) {
            if (kCapRows[i].required && !capRowResolved(kCapRows[i])) {
                _snprintf(capLine, sizeof(capLine) - 1,
                          "[engine] CAP-MISS op=%s cap=%s",
                          kCapRows[i].name, capName(kCapRows[i].cap));
                capLine[sizeof(capLine) - 1] = '\0';
                coop::logLine(capLine);
            }
        }
        // One-line availability fingerprint for the oracle/diagnostic surface.
        std::string caps = "[engine] CAPS";
        for (int c = 0; c < CAP_COUNT; ++c) {
            caps += ' ';
            caps += capName((Capability)c);
            caps += g_capAvail[c] ? "=1" : "=0";
        }
        coop::logLine(caps.c_str());
        if (!capCoreOk(g_capAvail))
            coop::logLine("[engine] CAPS INCOMPATIBLE - core hand-resolve missing "
                          "(runtime image unsupported)");
    }

    // Spike 402: prove which executable is actually mapped and record the
    // KenshiLib-remapped entry points for the native save/object-serialisation
    // pipeline.  This is deliberately address-only: no serialiser is invoked.
    const char* spikeId = std::getenv("KENSHICOOP_SPIKE");
    if (spikeId && strcmp(spikeId, "402") == 0) {
        const unsigned __int64 base =
            (unsigned __int64)GetModuleHandleA(NULL);
        char modulePath[MAX_PATH];
        modulePath[0] = '\0';
        GetModuleFileNameA(NULL, modulePath, MAX_PATH);
        modulePath[MAX_PATH - 1] = '\0';

        typedef void (RootObjectContainer::*SerialiseThingsAllMemFn)(
            GameData*, GameDataContainer*, PosRotPair*, const std::string&);
        typedef void (RootObjectContainer::*SerialiseThingsSomeMemFn)(
            const lektor<RootObject*>&, GameData*, GameDataContainer*,
            PosRotPair*, const std::string&);

        struct AddrProbe {
            const char* name;
            unsigned __int64 address;
        };
        const AddrProbe probes[] = {
            { "RootObjectContainer::serialiseThings(all)",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  static_cast<SerialiseThingsAllMemFn>(
                      &RootObjectContainer::serialiseThings)) },
            { "RootObjectContainer::serialiseThings(list)",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  static_cast<SerialiseThingsSomeMemFn>(
                      &RootObjectContainer::serialiseThings)) },
            { "Character::serialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Character::_NV_serialise) },
            { "Character::loadFromSerialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Character::_NV_loadFromSerialise) },
            { "Building::serialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Building::_NV_serialise) },
            { "Building::loadFromSerialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Building::_NV_loadFromSerialise) },
            { "Item::serialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Item::_NV_serialise) },
            { "Item::loadFromSerialise",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &Item::_NV_loadFromSerialise) },
            { "GameDataContainer::save",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &GameDataContainer::save) },
            { "GameDataContainer::load",
              (unsigned __int64)KenshiLib::GetRealAddress(
                  &GameDataContainer::load) }
        };

        char line[480];
        _snprintf(line, sizeof(line) - 1,
                  "[r402] module='%s' base=%016llx", modulePath, base);
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
        for (unsigned int i = 0;
             i < sizeof(probes) / sizeof(probes[0]); ++i) {
            const unsigned __int64 rva =
                probes[i].address && probes[i].address >= base
                    ? probes[i].address - base : 0;
            _snprintf(line, sizeof(line) - 1,
                      "[r402] %-48s addr=%016llx rva=%08llx",
                      probes[i].name, probes[i].address, rva);
            line[sizeof(line) - 1] = '\0';
            coop::logLine(line);
        }
    }
}

bool gameplayLive(GameWorld* gw) {
    __try {
        return gw && gw->player && gw->player->playerCharacters.size() > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// probeNativeSnapshot (the spike-402 native-snapshot round-trip probe) moved to
// EngineProbe.cpp (Phase 5e, HARNESS-ONLY). It uses the external leader(); the
// spike-402 ADDRESS-only trace above stays here (it runs from installEngineDetours
// at startup, gated by KENSHICOOP_SPIKE=402).

// Overwrite SaveManager::currentGame in place. getCurrentGame() returns a
// reference to the member (modelled as a pointer via g_saveMgrCurGameFn), so we
// write straight through it. Field evidence: SaveManager::load(name) does NOT
// update currentGame, so after a programmatic load getCurrentGame()/saveInfo()
// keep reporting the STALE name (whatever the in-game menu / persisted config
// left there). Left unfixed, the host connect-push (armConnectPush ->
// saveGameAs(saveInfo)) writes the freshly-loaded world over an UNRELATED save
// folder - the fixture-clobber that corrupted pole1 mid-regression. Caller
// (loadSave) sets it to the name we just asked the engine to load so the
// connect-push and any coordinated save target the correct slot.
bool setCurrentGameName(const std::string& name) {
    if (!g_getFn || !g_saveMgrCurGameFn) return false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        const std::string* cur = g_saveMgrCurGameFn(mgr);
        if (!cur) return false;
        *const_cast<std::string*>(cur) = name;
        return true;
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
        // The engine's own load does NOT refresh currentGame, so set it here to
        // the save we just issued. Keeps getCurrentGame()/saveInfo() truthful so
        // the connect-push (armConnectPush) targets THIS save, never a stale one.
        setCurrentGameName(name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int saveMgrSignal(int* outDelay) {
    if (outDelay) *outDelay = -1;
    if (!g_getFn) return -1;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return -1;
        if (outDelay) *outDelay = mgr->delay;
        return mgr->signal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool saveMgrExecute() {
    if (!g_getFn || !g_saveMgrExecFn) return false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        g_saveMgrExecFn(mgr);
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

bool saveGameAs(const std::string& name) {
    if (!g_getFn || !g_saveFn) return false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        g_saveFn(mgr, &name, /*autosave*/false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool saveInfo(char* curGame, unsigned int curLen,
              char* savePath, unsigned int pathLen) {
    if (curGame && curLen)   curGame[0]  = '\0';
    if (savePath && pathLen) savePath[0] = '\0';
    if (!g_getFn) return false;
    bool any = false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        if (curGame && curLen && g_saveMgrCurGameFn) {
            const std::string* s = g_saveMgrCurGameFn(mgr);
            if (s) {
                strncpy(curGame, s->c_str(), curLen - 1);
                curGame[curLen - 1] = '\0';
                any = true;
            }
        }
        if (savePath && pathLen && g_saveMgrPathFn) {
            const std::string* s = g_saveMgrPathFn(mgr);
            if (s) {
                strncpy(savePath, s->c_str(), pathLen - 1);
                savePath[pathLen - 1] = '\0';
                any = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return any;
}

bool installSaveHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&SaveManager::save);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&saveMgrSave_hook,
                              (void**)&g_saveHookOrig) == KenshiLib::SUCCESS;
}

void setSaveSuppress(bool on) { g_saveSuppressAll = on; }

unsigned int drainSaveEdges(SaveEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_saveEdges.size() && n < maxOut; ++i, ++n) {
        memcpy(out[n].name, g_saveEdges[i].name, sizeof(out[n].name));
        out[n].autosave   = g_saveEdges[i].autosave;
        out[n].suppressed = g_saveEdges[i].suppressed;
    }
    g_saveEdges.clear();
    return n;
}

bool installLoadHook() {
    // Both public load entries (field evidence: the in-game load menu takes
    // the SaveInfo overload and never funnels through load(name)).
    intptr_t addrName = KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const std::string&)>(&SaveManager::load));
    intptr_t addrInfo = KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const SaveInfo&, bool)>(&SaveManager::load));
    if (!addrName || !addrInfo) return false;
    if (KenshiLib::AddHook(addrName, (void*)&saveMgrLoad_hook,
                           (void**)&g_loadHookOrig) != KenshiLib::SUCCESS)
        return false;
    return KenshiLib::AddHook(addrInfo, (void*)&saveMgrLoadInfo_hook,
                              (void**)&g_loadInfoHookOrig) == KenshiLib::SUCCESS;
}

void setLoadSuppress(bool on) { g_loadSuppressAll = on; }
void setLoadBypassOnce()      { g_loadBypassOnce = true; }

unsigned int drainLoadEdges(LoadEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_loadEdges.size() && n < maxOut; ++i, ++n) {
        memcpy(out[n].name, g_loadEdges[i].name, sizeof(out[n].name));
        out[n].suppressed = g_loadEdges[i].suppressed;
    }
    g_loadEdges.clear();
    return n;
}


bool installAiSuspendHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&Character::_NV_periodicUpdate);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&periodicUpdate_hook,
                              (void**)&g_periodicOrig) == KenshiLib::SUCCESS;
}

bool installTaskSelectSpikeHook() {
    // Disambiguate the two setCurrentAction overloads: hook the Tasker* twin
    // (the one every scorer/order result funnels through), not the
    // (TaskType, RootObject*) convenience overload.
    intptr_t addr = KenshiLib::GetRealAddress(
        static_cast<bool (CharBody::*)(Tasker*)>(&CharBody::_NV_setCurrentAction));
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&setCurrentAction_hook,
                              (void**)&g_setActionOrig) == KenshiLib::SUCCESS;
}
void setTaskSelectSpike(bool on) { g_taskSelectSpike = on; }

void clearAiSuspend()           { g_aiSuspended.clear(); }
void addAiSuspend(Character* c) { if (c) g_aiSuspended.insert(c); }
unsigned int aiSuspendCount()   { return (unsigned int)g_aiSuspended.size(); }

bool installDamageGuardHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&Character::_NV_hitByMeleeAttack);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&hitByMelee_hook,
                              (void**)&g_hitByMeleeOrig) == KenshiLib::SUCCESS;
}

bool installShopHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&Inventory::buyItem);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&buyItem_hook,
                              (void**)&g_buyItemOrig) == KenshiLib::SUCCESS;
}

// Cross-owner trade veto: detour the _NV_ twins of the drag primitives (hooking
// the real body catches virtual dispatch AND direct calls). Both must install or
// the pairing is incomplete, so a partial success is reported as failure.
bool installXferBlockHook() {
    intptr_t addRem = KenshiLib::GetRealAddress(
        &Inventory::_NV_removeItemDontDestroy_returnsItem);
    intptr_t addAdd = KenshiLib::GetRealAddress(&Inventory::_NV_tryAddItem);
    if (!addRem || !addAdd) return false;
    if (KenshiLib::AddHook(addRem, (void*)&removeDontDestroy_hook,
                           (void**)&g_removeDontDestroyOrig) != KenshiLib::SUCCESS)
        return false;
    return KenshiLib::AddHook(addAdd, (void*)&tryAddItem_hook,
                              (void**)&g_tryAddItemOrig) == KenshiLib::SUCCESS;
}

void setBlockXfer(bool on)                    { g_blockXfer = on; }
void setInvOwnerClassifier(InvOwnerClassFn fn) { g_invOwnerClass = fn; }

// Query-free ground-drop capture: detour the dropItem _NV_ twin.
bool installItemDropHook() {
    intptr_t addr = KenshiLib::GetRealAddress(&Inventory::_NV_dropItem);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&dropItem_hook,
                              (void**)&g_dropItemOrig) == KenshiLib::SUCCESS;
}

unsigned int drainItemDrops(ItemDropEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_dropEdges.size() && n < maxOut; ++i, ++n)
        out[n] = g_dropEdges[i];
    g_dropEdges.clear();
    return n;
}

bool installRecruitHook() {
    intptr_t addr = KenshiLib::GetRealAddress(
        static_cast<bool (PlayerInterface::*)(Character*, bool)>(&PlayerInterface::recruit));
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&recruit_hook,
                              (void**)&g_recruitHookOrig) == KenshiLib::SUCCESS;
}

bool installBuildHook() {
    intptr_t addr = KenshiLib::GetRealAddress(
        &PreviewBuilding::_NV_placeFinalPreviewBuilding);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&placeFinal_hook,
                              (void**)&g_placeFinalOrig) == KenshiLib::SUCCESS;
}

bool installDismantleHook() {
    intptr_t addr = KenshiLib::GetRealAddress(
        &Building::_NV_notifyConstructionDismantling);
    if (!addr) return false;
    return KenshiLib::AddHook(addr, (void*)&dismantle_hook,
                              (void**)&g_dismantleOrig) == KenshiLib::SUCCESS;
}

unsigned int drainRemoveEdges(unsigned int (*out)[5], unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i + 5 <= g_removeEdges.size() && n < maxOut; i += 5, ++n)
        for (int j = 0; j < 5; ++j) out[n][j] = g_removeEdges[i + j];
    g_removeEdges.clear();
    return n;
}

void queueRemoveEdge(const unsigned int bHand[5]) {
    if (bHand) pushRemoveEdge(bHand);
}

unsigned int drainBuildEdges(BuildEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_buildEdges.size() && n < maxOut; ++i, ++n) {
        memcpy(out[n].hand, g_buildEdges[i].hand, sizeof(out[n].hand));
        out[n].x = g_buildEdges[i].x;
        out[n].y = g_buildEdges[i].y;
        out[n].z = g_buildEdges[i].z;
        out[n].yaw = g_buildEdges[i].yaw;
        out[n].floorNum = g_buildEdges[i].floorNum;
        out[n].fromUi = g_buildEdges[i].fromUi;
        memcpy(out[n].sid, g_buildEdges[i].sid, sizeof(out[n].sid));
    }
    g_buildEdges.clear();
    return n;
}

unsigned int drainRecruitEdges(RecruitEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_recruitEdges.size() && n < maxOut; ++i, ++n) {
        memcpy(out[n].before, g_recruitEdges[i].before, sizeof(out[n].before));
        memcpy(out[n].after,  g_recruitEdges[i].after,  sizeof(out[n].after));
    }
    g_recruitEdges.clear();
    return n;
}

// SEH-guarded roster snapshot into POD arrays (the map diff below must live
// OUTSIDE the SEH frame - C2712). readObjectHand carries its own SEH.
static unsigned int snapshotRoster(GameWorld* gw, Character** outC,
                                   unsigned int (*outH)[5], unsigned int maxOut) {
    unsigned int n = 0;
    __try {
        if (!gw->player) return 0;
        unsigned int size = (unsigned int)gw->player->playerCharacters.size();
        for (unsigned int i = 0; i < size && n < maxOut; ++i) {
            Character* c = gw->player->playerCharacters[i];
            if (!c) continue;
            if (!readObjectHand(static_cast<RootObject*>(c), outH[n])) continue;
            outC[n] = c;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

void pollSquadRoster(GameWorld* gw) {
    if (!gw) return;
    const unsigned int MAXR = 160; // matches publishOwned's MAX_PUBLISH bound
    static Character*   cs[MAXR];    // main-thread only
    static unsigned int hs[MAXR][5];
    unsigned int n = snapshotRoster(gw, cs, hs, MAXR);
    if (n == 0) return; // mid-load/world-swap: never sweep a whole squad out
    // Mark: diff each surviving pointer's hand against the baseline.
    for (unsigned int i = 0; i < n; ++i) {
        std::map<Character*, SquadHandRec>::iterator it = g_squadRoster.find(cs[i]);
        if (it == g_squadRoster.end()) {
            // New pointer: seed only. A recruit ENTRY is the protocol-23
            // detour's edge; a session-start census is not an edge at all.
            SquadHandRec r;
            memcpy(r.h, hs[i], sizeof(r.h));
            g_squadRoster[cs[i]] = r;
            continue;
        }
        if (memcmp(it->second.h, hs[i], sizeof(hs[i])) != 0) {
            if (g_squadMoveEdges.size() < 64) {
                SquadMoveEdge e;
                memcpy(e.before, it->second.h, sizeof(e.before));
                memcpy(e.after, hs[i], sizeof(e.after));
                g_squadMoveEdges.push_back(e);
            }
            memcpy(it->second.h, hs[i], sizeof(it->second.h));
        }
    }
    // Sweep: pointers gone from the roster (dismissed/dead). Report the STORED
    // hand only - the pointer may already be freed. Linear membership scan is
    // fine (a squad is tens of bodies at 1 Hz).
    for (std::map<Character*, SquadHandRec>::iterator it = g_squadRoster.begin();
         it != g_squadRoster.end(); ) {
        bool present = false;
        for (unsigned int i = 0; i < n; ++i)
            if (cs[i] == it->first) { present = true; break; }
        if (present) { ++it; continue; }
        if (g_squadMoveEdges.size() < 64) {
            SquadMoveEdge e;
            memcpy(e.before, it->second.h, sizeof(e.before));
            memset(e.after, 0, sizeof(e.after));
            g_squadMoveEdges.push_back(e);
        }
        g_squadRoster.erase(it++);
    }
}

void clearSquadRoster() {
    g_squadRoster.clear();
    g_squadMoveEdges.clear();
}

unsigned int drainSquadMoveEdges(SquadMoveEdge* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_squadMoveEdges.size() && n < maxOut; ++i, ++n) {
        memcpy(out[n].before, g_squadMoveEdges[i].before, sizeof(out[n].before));
        memcpy(out[n].after,  g_squadMoveEdges[i].after,  sizeof(out[n].after));
    }
    g_squadMoveEdges.clear();
    return n;
}

int probeMoveSquadMember(GameWorld* gw, const unsigned int mHand[5],
                         const unsigned int tHand[5], int lever,
                         unsigned int outBefore[5], unsigned int outAfter[5]) {
    if (outBefore) memset(outBefore, 0, 5 * sizeof(unsigned int));
    if (outAfter)  memset(outAfter, 0, 5 * sizeof(unsigned int));
    if (!gw || !mHand) return -1;
    // Wire layout [type, container, containerSerial, index, serial] ->
    // resolveCharByHand(index, serial, type, container, containerSerial).
    Character* c = resolveCharByHand(mHand[3], mHand[4], mHand[0], mHand[1], mHand[2]);
    if (!c) return -1;
    Character* t = 0;
    if (lever != 0) {
        if (!tHand) return -1;
        t = resolveCharByHand(tHand[3], tHand[4], tHand[0], tHand[1], tHand[2]);
        if (!t) return -1;
    }
    __try {
        unsigned int hb[5];
        if (!readObjectHand(static_cast<RootObject*>(c), hb)) return -1;
        if (outBefore) memcpy(outBefore, hb, sizeof(hb));
        if (lever == 0) {
            if (!g_separateSquadFn) return -1;
            g_separateSquadFn(c, true);
        } else {
            if (!g_getPlatoonFn || !gw->player) return -1;
            ActivePlatoon* ap = g_getPlatoonFn(t);
            if (!ap) return -1;
            if (lever == 1) {
                if (!g_playerFactionFn) return -1;
                Faction* f = g_playerFactionFn(gw->player);
                if (!f) return -1;
                // Virtual (vtable slot 0 per the header) - dispatch directly.
                c->setFaction(f, ap);
            } else {
                if (!g_addCharacterAtFn) return -1;
                // Index semantics are the probe's question; a large index is
                // the safest "append" guess (the engine clamps list inserts).
                g_addCharacterAtFn(ap, static_cast<RootObject*>(c), 999);
            }
        }
        unsigned int ha[5];
        if (!readObjectHand(static_cast<RootObject*>(c), ha)) return -1;
        if (outAfter) memcpy(outAfter, ha, sizeof(ha));
        return (memcmp(hb, ha, sizeof(hb)) != 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void clearDamageGuard()           { g_damageGuarded.clear(); }
void addDamageGuard(Character* c) { if (c) g_damageGuarded.insert(c); }
unsigned int damageGuardCount()   { return (unsigned int)g_damageGuarded.size(); }
void damageGuardStats(unsigned long* outGuarded, unsigned long* outPassed) {
    if (outGuarded) *outGuarded = g_dmgGuardedHits;
    if (outPassed)  *outPassed  = g_dmgPassedHits;
}

bool readBloodByHand(const unsigned int hand[5], float* outBlood) {
    if (!outBlood) return false;
    Character* c = resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
    if (!c) return false;
    __try {
        *outBlood = c->medical.blood;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace engine
} // namespace coop
