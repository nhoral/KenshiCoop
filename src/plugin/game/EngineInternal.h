// EngineInternal.h - the PRIVATE engine-plane surface shared by the Engine*.cpp
// translation units (monolith split, 2026-07-12). NOT part of the public API:
// Replicator/Scenario/Plugin code must keep using Engine.h only.
//
// What lives here:
//   * the full include prelude every Engine*.cpp needs (Boost guards first -
//     see the note in EngineInternal.cpp; ZoneManager.h stays quarantined in
//     ZoneQuery.cpp because it redefines ParticlePool vs CombatClass.h);
//   * the resolved-function-pointer typedefs (mirrors - the authoritative
//     documentation for each stays with its definition in EngineInternal.cpp);
//   * `extern` declarations for the g_* registry, hook edge queues and shared
//     scratch buffers DEFINED in EngineInternal.cpp (single definition point;
//     engine::resolve() populates the pointers at load);
//   * declarations of internal helpers used across domain TUs.
//
// Rules for the split TUs:
//   * never define a g_* here or in a domain TU - EngineInternal.cpp owns them all;
//   * section-private statics/anon-namespace helpers stay in their domain TU;
//   * a helper needed by a SECOND domain TU moves to EngineInternal.cpp and gets
//     declared here (do not duplicate it).

#ifndef KENSHICOOP_ENGINE_INTERNAL_H
#define KENSHICOOP_ENGINE_INTERNAL_H

#define _CRT_SECURE_NO_WARNINGS 1
#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include "Engine.h"
// Phase 5a domain split: the adapter re-includes the narrow public headers carved
// out of Engine.h so the domain TUs (which define these entry points) still see
// their declarations.
#include "EngineUi.h"
#include "EngineScenario.h"
#include "EngineProbe.h"
#include "EngineFaults.h" // Phase 5c: typed, throttled SEH-fault accounting
#include "EngineCaps.h"   // Phase 5d: owned capability registry over resolve()
#include "../CoopLog.h"
#include "../../netproto/ContentHash.h" // invEntryHash / sectionNameHash (shared with prototest)

#include <core/Functions.h>         // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>       // GameWorld::player
#include <kenshi/PlayerInterface.h> // PlayerInterface::playerCharacters
#include <kenshi/CameraClass.h>     // CameraClass (camera-anchored interest, spike 35)
#include <kenshi/SaveManager.h>     // SaveManager::getSingleton/load/savesExist
#include <kenshi/SaveInfo.h>        // SaveInfo (the in-game load menu's load(SaveInfo&) overload)
#include <kenshi/Character.h>       // Character (handle/getPosition/getOrientation/movement)
#include <kenshi/CharMovement.h>    // CharMovement::_setPositionDirectionAndTeleport/setDestination
#include <kenshi/Enums.h>           // itemType, MoveSpeed { RUN }
#include <kenshi/RootObject.h>      // RootObject base (getCharactersWithinSphere out type)
#include <kenshi/RootObjectBase.h>  // getGameData/getFaction (spawn template + owner)
#include <kenshi/RootObjectFactory.h> // createRandomCharacter / createBuilding / createItem
#include <kenshi/GameData.h>        // GameData::name / stringID / type (template scan + inv)
#include <kenshi/GameSaveState.h>   // native object snapshot round-trip probe
#include <kenshi/Inventory.h>       // Inventory (Phase 4a container contents)
#include <kenshi/Item.h>            // Item / InventoryItemBase (quantity/quality/equipped)
#include <kenshi/CharBody.h>        // CharBody::currentAction / _NV_setCurrentAction
#include <kenshi/CombatClass.h>     // CombatClass::combatState (active-vs-waiting stance)
#include <kenshi/CharStats.h>       // CharStats (protocol 17 stats sync)
#include <kenshi/Tasker.h>          // Tasker::key() -> TaskType, Tasker::subject (hand)
#include <kenshi/util/hand.h>       // hand (5-field identity, getRootObject)
#include <kenshi/util/lektor.h>     // lektor<T> (playerCharacters, interest query)
#include <kenshi/Gear.h>            // Sword (spike 451 weapon-mint ctor trace)
#include <kenshi/Faction.h>         // Faction::getData / FactionManager::getFactionByStringID (protocol 21)
#include <kenshi/FactionRelations.h> // FactionRelations (protocol 24 faction-relation sync)
#include <kenshi/Platoon.h>         // Platoon / ActivePlatoon / Ownerships (wallet, protocol 22)
#include <kenshi/Building/DoorStuff.h> // DoorStuff (door/gate state, protocol 26)
#include <kenshi/Building/FarmBuilding.h>      // FarmBuilding growth floats (protocol 33; pulls Production/Storage/UseableStuff)
#include <kenshi/Building/CraftingBuilding.h>  // CraftingBuilding::operate override (protocol 33)
#include <kenshi/Building/GeneratorBuilding.h> // GeneratorBuilding::getPowerOutput override (protocol 33)
#include <kenshi/Building/ResearchBuilding.h>  // ResearchBuilding tech-level evidence (protocol 33)
#include <kenshi/ShopTrader.h>      // ShopTrader (vendor stock, protocol 22)
#include <kenshi/Globals.h>         // gui (ForgottenGUI*, KenshiLib data export)
#include <kenshi/gui/ForgottenGUI.h> // ForgottenGUI::mainbar
#include <kenshi/gui/MainBarGUI.h>  // MainBarGUI::speedButtons (vote-button probe)
// NOTE: kenshi/ZoneManager.h cannot be included here - it redefines ParticlePool
// (also defined by CombatClass.h above). The zone-loaded query lives in its own
// TU (game/ZoneQuery.cpp); see engine::isZoneLoadedAt / engine::resolveZoneQuery.
#include <mygui/MyGUI_Button.h>     // MyGUI::Button::getStateSelected
#include <ogre/OgreVector3.h>
#include <ogre/OgreQuaternion.h>
#include <cmath>
#include <cstdlib> // getenv (KENSHICOOP_INV_DUMP reconcile-trace gate)
#include <intrin.h> // _ReturnAddress (spike 451 weapon-mint caller RVAs)
#pragma intrinsic(_ReturnAddress)
#include <map>     // squad roster pointer->hand baseline (protocol 35)
#include <set>
#include <utility>
#include <vector>

namespace coop {
namespace engine {

// ---- Resolved-function-pointer typedefs (documentation lives at the
// ---- definitions in EngineInternal.cpp; these are identical re-typedefs) -----------

// SaveManager (save/load, protocols 31-32)
typedef SaveManager* (__fastcall* SaveMgrGetFn)();
typedef void         (__fastcall* SaveMgrLoadNameFn)(SaveManager* self, const std::string* name);
typedef bool         (__fastcall* SaveMgrSavesExistFn)(SaveManager* self);
typedef void         (__fastcall* SaveMgrSaveNameFn)(SaveManager* self, const std::string* name,
                                                     bool autosave);
typedef const std::string* (__fastcall* SaveMgrStrFn)(SaveManager* self);
typedef void (__fastcall* SaveMgrExecFn)(SaveManager* self);
typedef void (__fastcall* SaveMgrLoadInfoFn)(SaveManager* self, const SaveInfo* info,
                                             bool resetPos);

// hand resolve
typedef Character* (__fastcall* HandGetCharFn)(const hand* self);
typedef hand*      (__fastcall* HandCtorFn)(hand* self, unsigned int index,
                                            unsigned int serial, itemType type,
                                            unsigned int container,
                                            unsigned int containerSerial);

// locomotion / spatial query / AI quieting
typedef void (__fastcall* CharSetDestFn)(Character* self, const Ogre::Vector3* pos, bool shift);
typedef void (__fastcall* GetCharsInSphereFn)(
    GameWorld* self, lektor<RootObject*>* results, const Ogre::Vector3* pos,
    float farRadius, float nearRadius, float always, int maxFar, int maxNear,
    RootObject* skip);
typedef void (__fastcall* GetObjsInSphereFn)(
    GameWorld* self, lektor<RootObject*>* results, const Ogre::Vector3* pos,
    float radius, itemType type, int maxNumber, RootObject* skip);
typedef void (__fastcall* ClearGoalsFn)(Character* self);
typedef void (__fastcall* UpdateListFn)(GameWorld* self, Character* c);

// tasks / pose / medical / stats / carry / furniture / stealth
typedef int         (__fastcall* TaskerKeyFn)(const void* self);
typedef const std::string* (__fastcall* TaskerDescFn)(const void* self);
typedef RootObject* (__fastcall* HandGetRootFn)(const hand* self);
typedef bool        (__fastcall* SetActionFn)(void* charBody, int taskType, RootObject* target);
typedef void        (__fastcall* AddGoalFn)(Character* self, int taskType, RootObject* subject);
typedef void      (__fastcall* AddOrderFn)(Character* self, Building* dest, int t,
                                           RootObject* subject, bool shift, bool clear,
                                           const Ogre::Vector3* location);
typedef void      (__fastcall* AddJobFn)(Character* self, int t, RootObject* subject,
                                         bool shift, bool addDontClear,
                                         const Ogre::Vector3* location);
typedef Platoon*  (__fastcall* SeparateSquadFn)(Character* self, bool permanent);
typedef void      (__fastcall* EndActionFn)(void* charBody);
typedef void      (__fastcall* RagdollModeFn)(Character* self, bool on, int part);
typedef void      (__fastcall* MedFloatFn)(MedicalSystem* self, float v);
typedef void      (__fastcall* MedAmputateFn)(MedicalSystem* self, int limb,
                                              bool createSeveredItem,
                                              const Ogre::Vector3* force);
typedef void      (__fastcall* MedCrushLimbFn)(MedicalSystem* self, int limb);
typedef void      (__fastcall* MedSetRobotLimbFn)(MedicalSystem* self, int limb,
                                                  Item* item, bool isLoadingASave);
typedef int       (__fastcall* MedGetLimbStateFn)(const MedicalSystem* self, int limb);
typedef float*    (__fastcall* StatsGetRefFn)(CharStats* self, int what);
typedef void      (__fastcall* StatsRecalcFn)(CharStats* self);
typedef void      (__fastcall* PickupObjectFn)(Character* self, Character* who);
typedef void      (__fastcall* DropCarriedFn)(Character* self, bool ragdollHim,
                                              bool removeOnly);
typedef void      (__fastcall* SetFurnModeFn)(Character* self, bool on,
                                              UseableStuff* h);
// Chained/pole prisoner (protocol 41): Character::setChainedMode(bool on,
// const hand& owner) - owner passed by hidden reference (const hand*).
typedef void      (__fastcall* SetChainedModeFn)(Character* self, bool on,
                                                 const hand* owner);
// Phase 6 shackle read lever: Character::getChainedModeShackles() -> equipped
// LockedArmour* (shackle item) or null; non-null LockedArmour::lock == locked.
typedef LockedArmour* (__fastcall* GetShacklesFn)(Character* self);
// Jail-probe read lever: Character::isSlave() -> SlaveStateEnum (0 NOT_SLAVE /
// 1 IS_SLAVE / 2 ESCAPING_SLAVE / 3 EX_SLAVE), returned in eax as int.
typedef int       (__fastcall* IsSlaveFn)(Character* self);
typedef void      (__fastcall* SetStealthModeFn)(Character* self, bool on);
typedef void      (__fastcall* NotifySeeSneakFn)(Character* self, Character* who,
                                                 const int* seeingYnm, float prog01);
// Engagement escalation (world_parity camp): Character::attackTarget is the
// engine's own AI commit-an-attack entry - it bypasses the goal/order
// validation that silently drops an attack on a non-hostile player-squad
// target (a driven guard beating a locally player-owned escaped prisoner).
typedef void (__fastcall* AttackTargetFn)(Character* self, Character* who);
// Camera-anchored interest (spike 35): CameraClass::getCenter() returns
// Ogre::Vector3 by value (12 bytes -> hidden return pointer on x64, the
// GetTimeHoursFn precedent). isInitialised guards a camera not yet set up.
typedef Ogre::Vector3* (__fastcall* CamGetCenterFn)(const CameraClass* self,
                                                    Ogre::Vector3* ret);
typedef bool           (__fastcall* CamIsInitFn)(const CameraClass* self);

// game speed / clock
typedef void (__fastcall* SetGameSpeedFn)(GameWorld* self, float speed, bool click);
typedef void (__fastcall* UserPauseFn)(GameWorld* self, bool p);
typedef void (__fastcall* SetFrameSpeedMultFn)(GameWorld* self, float m);
typedef double* (__fastcall* GetTimeHoursFn)(GameWorld* self, double* ret);
typedef float   (__fastcall* GetHourLenFn)(GameWorld* self);

// doors (protocol 26)
typedef bool (__fastcall* DoorBoolFn)(const DoorStuff* self);
typedef bool (__fastcall* DoorActFn)(DoorStuff* self);
typedef void (__fastcall* DoorVoidFn)(DoorStuff* self);

// placed buildings (protocol 27) + production machines (protocol 33)
typedef void (__fastcall* BuildProgFn)(Building* self, float amount);
typedef void (__fastcall* BuildDoneFn)(Building* self);
typedef void  (__fastcall* MachPowerSetFn)(UseableStuff* self, bool on);
typedef float (__fastcall* MachPowerOutFn)(const UseableStuff* self);
typedef bool  (__fastcall* MachIsGenFn)(const UseableStuff* self);
typedef void  (__fastcall* MachSetProdItemFn)(ProductionBuilding* self,
                                              GameData* item, int stack,
                                              float progress01);
typedef int   (__fastcall* MachTechLvlFn)(ResearchBuilding* self);
typedef void  (__fastcall* MachOperateFn)(Building* self, Character* who,
                                          float amount);
typedef GameData* (__fastcall* MachProdItemDataFn)(Building* self);

// money / vendor trading (protocol 22)
typedef ActivePlatoon* (__fastcall* GetPlatoonFn)(const Character* self);
typedef int   (__fastcall* OwnGetMoneyFn)(const Ownerships* self);
typedef void  (__fastcall* OwnSetMoneyFn)(Ownerships* self, int amount);
typedef Item* (__fastcall* BuyItemFn)(Inventory* self, Item* itemToBuy,
                                      RootObject* sendingTo);
typedef void  (__fastcall* PlatoonRefreshInvFn)(ActivePlatoon* self, bool firstTime);
typedef Character* (__fastcall* ShopGetTraderFn)(const ShopTrader* self);

// recruitment (protocol 23) + squad moves (protocol 35)
typedef bool (__fastcall* RecruitFn)(PlayerInterface* self, Character* c, bool editor);
typedef Faction* (__fastcall* PlayerFactionFn)(const PlayerInterface* self);
typedef void     (__fastcall* AddCharacterAtFn)(ActivePlatoon* self,
                                                RootObject* c, int index);

// build placement / dismantle edges (protocols 27-28)
typedef void (__fastcall* PlaceFinalFn)(PreviewBuilding* self);
typedef void (__fastcall* DismantleFn)(Building* self);

// AI suspend + damage guard detours
typedef void (__fastcall* PeriodicUpdateFn)(Character* self);
typedef HitMaterialType (__fastcall* HitByMeleeFn)(
    Character* self, CutDirection dir, Damages& damage, Character* who,
    CombatTechniqueData* attack, int comboID);

// factory / inventory / faction primitives
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
typedef RootObjectBase* (__fastcall* CreateObjFn)(
    RootObjectFactory* self, GameData* data, Ogre::Vector3 position,
    bool isFromActiveLevelMod, Faction* owner, Ogre::Quaternion rotation,
    FactoryCallbackInterface* cb, RootObjectContainer* certainContainer,
    GameSaveState* state, bool invisible, Building* homeBuilding, float age);
typedef bool (__fastcall* DestroyObjFn)(
    GameWorld* self, RootObject* obj, bool justUnloaded, const char* debugInfo);
typedef Item* (__fastcall* CreateItemFn)(
    RootObjectFactory* self, GameData* gd, const hand* handle, GameData* weaponMesh,
    GameData* matData, int levelOverride, Faction* flagUniform);
typedef bool (__fastcall* EquipItemFn)(Inventory* self, Item* item);
typedef lektor<InventorySection*>* (__fastcall* GetAllSectionsFn)(Inventory* self);
typedef Item* (__fastcall* GetWeaponFn)(Inventory* self);
typedef Faction*  (__fastcall* FacBySidFn)(FactionManager* self, const std::string* sid);
typedef GameData* (__fastcall* FacGetDataFn)(const Faction* self);
typedef float (__fastcall* RelGetFn)(FactionRelations* self, Faction* p);
typedef void  (__fastcall* RelSetFn)(FactionRelations* self, Faction* who, float setTo);
typedef bool  (__fastcall* RelBoolFn)(FactionRelations* self, Faction* c);
typedef void (__fastcall* AffectRelEvFn)(FactionRelations* self, Faction* p, int e, float mult);
typedef void (__fastcall* AffectRelAmtFn)(FactionRelations* self, Faction* p, float amount, float mult);
// Protocol 45 bounty/crime write levers (BountyManager, inline at Character+0xF0;
// non-virtual, resolved like every other engine call via GetRealAddress).
typedef void (__fastcall* BountyAddFn)(BountyManager* self, Faction* enforcer, int amount);
typedef void (__fastcall* BountyClearFn)(BountyManager* self, Faction* enforcer);
typedef int  (__fastcall* BountyGetFn)(BountyManager* self, Faction* whosLooking);

// honest-pose / body-state / combat reads
typedef Ogre::Vector3* (__fastcall* GetBip01Fn)(Character* self, Ogre::Vector3* ret);
typedef bool           (__fastcall* CharBodyBoolFn)(const void* self);
typedef Ogre::Vector3* (__fastcall* GetBoneWorldPosFn)(Character* self, Ogre::Vector3* ret,
                                                       const std::string* name);
typedef hand* (__fastcall* GetAttackTargetFn)(Character* self, hand* ret);
typedef bool  (__fastcall* InCombatModeFn)(Character* self, bool melee, bool ranged);
typedef CombatClass* (__fastcall* GetCombatClassFn)(Character* self);

// ---- Hook edge-queue records (defined here; the queue mechanics are
// ---- documented at the hook bodies in EngineInternal.cpp) ---------------------------

struct SaveEdgeRec { char name[48]; int autosave; int suppressed; };
struct LoadEdgeRec { char name[48]; int suppressed; };
struct RecruitEdgeRec { unsigned int before[5]; unsigned int after[5]; };
struct SquadHandRec { unsigned int h[5]; };
struct BuildEdgeRec {
    unsigned int hand[5];
    float x, y, z, yaw;
    int   floorNum;
    int   fromUi;
    char  sid[48];
};
struct FactionDeltaRec {
    char  meSid[48];   // whose relation table mutated (relations->me)
    char  whomSid[48]; // toward which faction
    int   isEvent;     // 1 = FactionEvent overload, 0 = raw-amount overload
    int   ev;          // FactionEvent (isEvent=1)
    float amount;      // raw amount (isEvent=0)
    float mult;
    float after;       // relation value after the engine ran its own math
};
struct FacLogGate {
    char meSid[48]; char whomSid[48];
    unsigned long lastMs; unsigned long skipped;
};

// ---- The g_* registry (defined in EngineInternal.cpp; engine::resolve() fills the
// ---- function pointers at plugin load) --------------------------------------

// SaveManager
extern SaveMgrGetFn        g_getFn;
extern SaveMgrLoadNameFn   g_loadFn;
extern SaveMgrSavesExistFn g_savesExistFn;
extern SaveMgrSaveNameFn   g_saveFn;
extern SaveMgrStrFn        g_saveMgrCurGameFn;
extern SaveMgrStrFn        g_saveMgrPathFn;
extern SaveMgrExecFn       g_saveMgrExecFn;

// save/load edge queues + suppression flags (protocols 31-32)
extern std::vector<SaveEdgeRec> g_saveEdges;
extern bool g_saveSuppressAll;
extern SaveMgrSaveNameFn g_saveHookOrig;
extern std::vector<LoadEdgeRec> g_loadEdges;
extern bool g_loadSuppressAll;
extern bool g_loadBypassOnce;
extern bool g_inLoadDetour;
extern SaveMgrLoadNameFn g_loadHookOrig;
extern SaveMgrLoadInfoFn g_loadInfoHookOrig;

// hand resolve
extern HandGetCharFn g_handGetCharFn;
extern HandCtorFn    g_handCtorFn;

// locomotion / spatial query / AI quieting
extern CharSetDestFn      g_charSetDestFn;
extern GetCharsInSphereFn g_getCharsFn;
extern GetObjsInSphereFn  g_getObjsFn;
extern ClearGoalsFn       g_clearGoalsFn;
extern UpdateListFn       g_removeUpdateFn;
extern UpdateListFn       g_addUpdateFn;

// tasks / pose / medical / stats / carry / furniture / stealth
extern TaskerKeyFn   g_taskerKeyFn;
extern TaskerDescFn  g_taskerDescFn;
extern HandGetRootFn g_handGetRootFn;
extern SetActionFn   g_setActionFn;
extern AddGoalFn     g_addGoalFn;
extern AddOrderFn      g_addOrderFn;
extern AddJobFn        g_addJobFn;
extern SeparateSquadFn g_separateSquadFn;
extern EndActionFn     g_endActionFn;
extern RagdollModeFn   g_ragdollModeFn;
extern MedFloatFn      g_knockoutFn;
extern MedFloatFn      g_knockoutForceFn;
extern MedAmputateFn     g_medAmputateFn;
extern MedCrushLimbFn    g_medCrushLimbFn;
extern MedSetRobotLimbFn g_medSetRobotLimbFn;
extern MedGetLimbStateFn g_medGetLimbStateFn;
extern StatsGetRefFn     g_statsGetRefFn;
extern StatsRecalcFn     g_statsRecalcFn;
extern PickupObjectFn    g_pickupObjectFn;
extern DropCarriedFn     g_dropCarriedFn;
extern SetFurnModeFn     g_setBedModeFn;
extern SetFurnModeFn     g_setPrisonModeFn;
extern SetChainedModeFn  g_setChainedModeFn;
extern GetShacklesFn     g_getShacklesFn;
extern IsSlaveFn         g_isSlaveFn;
extern SetStealthModeFn  g_setStealthModeFn;
extern NotifySeeSneakFn  g_notifySeeSneakFn;
extern CamGetCenterFn    g_camGetCenterFn;
extern CamIsInitFn       g_camIsInitFn;
extern AttackTargetFn    g_attackTargetFn;

// game speed / clock (+ intent hooks state)
extern SetGameSpeedFn      g_setGameSpeedFn;
extern UserPauseFn         g_userPauseFn;
extern UserPauseFn         g_togglePauseFn;
extern SetFrameSpeedMultFn g_setFrameSpeedMultFn;
extern GetTimeHoursFn g_getTimeHoursFn;
extern GetHourLenFn   g_getHourLenFn;
extern SetGameSpeedFn g_setGameSpeedOrig;
extern UserPauseFn    g_userPauseOrig;
extern UserPauseFn    g_togglePauseOrig;
extern bool  g_speedGuardWrite;
extern bool  g_speedIntentFresh;
extern float g_speedIntentMult;
extern bool  g_speedIntentPaused;
extern bool  g_speedIntentSeeded;
extern bool  g_quietHave;
extern float g_quietMult;
extern bool  g_quietPaused;
extern char g_voteBtn[15];
extern int  g_voteBtnN;
// Phase 5 spike (KENSHICOOP_DEBUG_SPEED): combat-cap-active hint, set by
// Replicator::syncSpeed each tick so the speed-setter diagnostics can tell an
// engine-forced (combat) change from a user click by context.
extern bool g_speedCombatHint;
// True when KENSHICOOP_DEBUG_SPEED=1 (cached). Gates the speed-path diagnostics.
bool speedDbgOn();

// doors
extern DoorBoolFn g_doorIsOpenFn;
extern DoorBoolFn g_doorIsLockedFn;
extern DoorActFn  g_doorOpenFn;
extern DoorActFn  g_doorCloseFn;
extern DoorActFn  g_doorForceOpenFn;
extern DoorActFn  g_doorForceCloseFn;
extern DoorVoidFn g_doorLockFn;
extern DoorVoidFn g_doorUnlockFn;

// placed buildings + production machines
extern BuildProgFn g_buildSetProgFn;
extern BuildProgFn g_buildAddProgFn;
extern BuildDoneFn g_buildNotifyDoneFn;
extern MachPowerSetFn    g_machPowerFn;
extern MachPowerOutFn    g_machPowerOutBaseFn;
extern MachPowerOutFn    g_machPowerOutGenFn;
extern MachIsGenFn       g_machIsGenFn;
extern MachSetProdItemFn g_machSetProdItemFn;
extern MachTechLvlFn     g_machTechLvlFn;
extern MachOperateFn     g_machOperateProdFn;
extern MachOperateFn     g_machOperateCraftFn;
extern MachOperateFn     g_machOperateFarmFn;
extern MachOperateFn     g_machOperateResearchFn;
extern MachProdItemDataFn g_machProdDataBaseFn;
extern MachProdItemDataFn g_machProdDataCraftFn;

// money / vendor trading
extern GetPlatoonFn  g_getPlatoonFn;
extern OwnGetMoneyFn g_ownGetMoneyFn;
extern OwnSetMoneyFn g_ownSetMoneyFn;
extern BuyItemFn     g_buyItemFn;
extern PlatoonRefreshInvFn g_platoonRefreshInvFn;
extern ShopGetTraderFn     g_shopGetTraderFn;
extern BuyItemFn g_buyItemOrig;

// Cross-owner trade veto (see Engine.h installXferBlockHook). g_invVetoSuspend
// is the reentrancy guard: the Replicator's OWN item relocations that re-home a
// REAL tracked item across the ownership boundary (W3 pickup addItemPtrToInventory,
// the retired Protocol 37 moveItemBetweenContainers) set it around their tryAddItem
// call so the veto never refuses a sanctioned sync move (only genuine UI drags).
extern bool g_invVetoSuspend;

// recruitment + squad moves
extern RecruitFn g_recruitFn;
extern std::vector<RecruitEdgeRec> g_recruitEdges;
extern RecruitFn g_recruitHookOrig;
extern std::map<Character*, SquadHandRec> g_squadRoster;
extern std::vector<SquadMoveEdge> g_squadMoveEdges;
extern PlayerFactionFn  g_playerFactionFn;
extern AddCharacterAtFn g_addCharacterAtFn;

// build placement / dismantle edges
extern std::vector<BuildEdgeRec> g_buildEdges;
extern PlaceFinalFn g_placeFinalOrig;
extern std::vector<unsigned int> g_removeEdges; // flat, 5 u32 per edge
extern DismantleFn g_dismantleOrig;

// AI suspend + damage guard
extern PeriodicUpdateFn     g_periodicOrig;
extern std::set<Character*> g_aiSuspended;
extern HitByMeleeFn         g_hitByMeleeOrig;
extern std::set<Character*> g_damageGuarded;
extern unsigned long        g_dmgGuardedHits;
extern unsigned long        g_dmgPassedHits;

// factory / inventory / faction
extern CreateCharFn     g_createCharFn;
extern CreateBuildingFn g_createBldgFn;
extern CreateObjFn      g_createObjFn;
extern DestroyObjFn     g_destroyObjFn;
extern GetDataOfTypeFn  g_getDataOfTypeFn;
extern CreateItemFn     g_createItemFn;
extern EquipItemFn      g_equipItemFn;
extern GetAllSectionsFn g_getSectionsFn;
extern GetWeaponFn      g_getPrimaryWeaponFn;
extern GetWeaponFn      g_getSecondaryWeaponFn;
extern FacBySidFn       g_facBySidFn;
extern FacGetDataFn     g_facGetDataFn;
extern RelGetFn         g_relGetFn;
extern RelSetFn         g_relSetFn;
extern RelBoolFn        g_relIsEnemyFn;
extern RelBoolFn        g_relIsAllyFn;
extern BountyAddFn      g_bountyAddFn;   // protocol 45: unfairAddToBounty
extern BountyClearFn    g_bountyClearFn; // protocol 45: clearBounty
extern BountyGetFn      g_bountyGetFn;   // protocol 45: getActualBounty
extern std::vector<FactionDeltaRec> g_facDeltas;
extern AffectRelEvFn  g_affectEvOrig;
extern AffectRelAmtFn g_affectAmtOrig;
extern FacLogGate g_facLogGates[16];

// honest-pose / body-state / combat reads
extern GetBip01Fn        g_getBip01Fn;
extern GetBoneWorldPosFn g_getBoneWorldFn;
extern CharBodyBoolFn    g_isIdleFn;
extern CharBodyBoolFn    g_isCrouchedFn;
extern CharBodyBoolFn    g_isDownFn;
extern CharBodyBoolFn    g_isRagdollFn;
extern CharBodyBoolFn    g_isDeadCharFn;
extern CharBodyBoolFn    g_isCrawlFn;
extern CharBodyBoolFn    g_isBeingCarriedFn;
extern GetAttackTargetFn g_getAttackTargetFn;
extern InCombatModeFn    g_inCombatModeFn;
extern GetCombatClassFn  g_getCombatClassFn;
extern CharBodyBoolFn    g_inRangedModeFn;
extern CharBodyBoolFn    g_underMeleeFn;
extern CharBodyBoolFn    g_fleeingFn;

// shared scratch buffers (main thread only)
extern lektor<GameData*>   g_dataScratch; // reused template-scan buffer
extern lektor<RootObject*> g_npcQuery;    // reused interest-query buffer

// ---- Hook detour bodies (defined in EngineInternal.cpp; passed to KenshiLib::AddHook
// ---- by the install* entry points, which may live in a domain TU) ----------

void __fastcall setGameSpeed_hook(GameWorld* self, float speed, bool click);
void __fastcall userPause_hook(GameWorld* self, bool p);
void __fastcall togglePause_hook(GameWorld* self, bool p);
void __fastcall affectRelEv_hook(FactionRelations* self, Faction* p, int e, float mult);
void __fastcall affectRelAmt_hook(FactionRelations* self, Faction* p, float amount, float mult);

// ---- Shared internal helpers (defined in the TU noted; used cross-TU) -------

// EngineInternal.cpp:

// Limb state with the engine's null policy (lazily-allocated robotLimbs);
// caller holds SEH.
unsigned char limbStateOf(MedicalSystem* med, int limb);

// The faction's GameData stringID - the save-stable cross-client identity.
// "" when unreadable.
void facSidOf(Faction* f, char* out, unsigned int outLen);

// Vote-highlight snapshot/restore (game-speed consensus UI).
void snapshotVoteButtons();
void restoreVoteButtons();

// EngineEntity.cpp:

// SEH-guarded RootObject::getInventory(): the ~24-times-copied
// __try{inv=ro->getInventory()}__except{0} pattern in one place (Phase 5c). 0 on
// null/fault; a fault bumps FAULT_INV_OF. Safe to call from inside another SEH
// frame (it is a leaf with its own frame).
Inventory* invOf(RootObject* ro);

// Once-per-(npc,fixture) "[seatres]" resolve diagnostic.
void logSeatResolveOnce(const char* side, int task, u32 npcIdx, u32 npcSer,
                        u32 sIdx, u32 sSer, u32 sType, u32 sCont, u32 sContSer,
                        float npx, float npy, float npz);
// True if 'obj' is a local player-squad member. Caller holds SEH.
bool isPlayerSquad(GameWorld* gw, RootObject* obj);
// Live NON-player Faction* read off a nearby world NPC. Caller holds SEH.
Faction* findNearbyNonPlayerFaction(GameWorld* gw);
// Dual-interest centers (one per squad tab leader, up to two). Caller holds SEH.
unsigned int interestCenters(GameWorld* gw, Ogre::Vector3 outC[4]);
// Case-insensitive substring test on raw C strings (SEH legal).
bool ciContains(const char* hay, const char* needle);
// Template scans (reused g_dataScratch; main thread only).
GameData* findSeatTemplate(GameWorld* gw);
GameData* findBedTemplate(GameWorld* gw);
GameData* findCageTemplate(GameWorld* gw);
GameData* findPoleTemplate(GameWorld* gw);
GameData* findMachineTemplate(GameWorld* gw);
// Position/yaw anchor in front of the local leader.
bool leaderAnchor(GameWorld* gw, float fwd, float side,
                  Ogre::Vector3* outPos, float* outYaw);
// Current task key of a character (TASK_NONE when unreadable).
int readCharTaskKey(Character* c);
// Nearest work fixture / its worker (craft re-arm search).
RootObject* findWorkFixtureNear(GameWorld* gw, int* outTask);
Character* findWorkerNear(GameWorld* gw, RootObject* fixture);

// EngineInventory.cpp:

// Container contents (loose + equipped) into out[] (see definition).
unsigned int readInvItems(Inventory* inv, InvItemEntry* out, Item** outItems,
                          unsigned int maxOut);
// Item template by stringID within its itemType category.
GameData* findItemTemplateImpl(GameWorld* gw, const char* sid, unsigned int typeCat);
// Spawn a character from a random template into the given faction.
Character* spawnCharInFaction(GameWorld* gw, float fwd, float side, Faction* fac);
// Generic WEAPON_MANUFACTURER GameData for weapon fabrication when the wire
// carried no manufacturer sid (spike 451). First enumerated record, cached per
// session. Externalised (Phase 5e) so the weapon-mint probes in EngineProbe.cpp
// can reuse it; the definition stays in EngineInventory.cpp.
GameData* fallbackWeaponManufacturer(GameWorld* gw);
// Create `qty` of the template (sid, typeCat) and add it to inv (spike-451 weapon
// recipe for WEAPONs; blank-handle fabricate for the rest). Externalised (Phase
// 5e) so probeFabricateWeaponLoose in EngineProbe.cpp can reuse it; the definition
// stays in EngineInventory.cpp.
bool createItemAndAdd(GameWorld* gw, Inventory* inv, const char* sid,
                      unsigned int typeCat, int qty, int qualityBucket, bool equip,
                      const char* manufacturer = 0, const char* material = 0);

// EngineSpawnCombat.cpp:

// SEH shim for FactionManager::getFactionByStringID.
Faction* factionBySidGuarded(GameWorld* gw, const std::string* sid);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_INTERNAL_H
