// Engine - SEH-guarded access to Kenshi engine state.
//
// Every call here either reads or pokes live game memory, so it is the ONLY
// place allowed to touch the engine, and only ever from the main (game) thread.
// Each guarded call wraps the engine call in __try/__except so a transient bad
// pointer (e.g. mid-load) degrades to a no-op instead of crashing the game.
//
// Stage 0 surface is intentionally tiny: detect "gameplay is live" and drive the
// save auto-load. Later stages extend this module with entity resolve/apply.

#ifndef KENSHICOOP_ENGINE_H
#define KENSHICOOP_ENGINE_H

#include <string>
#include "../../netproto/Wire.h"

class GameWorld;
class Character;
class RootObject;
class Faction;

namespace coop {
namespace engine {

// Resolve engine function addresses used by the plugin. Call once at load.
void resolve();

// SEH-guarded: is live gameplay running (the local player squad exists)?
bool gameplayLive(GameWorld* gw);

// SEH-guarded: issue a deferred save load by name. Returns false if the save
// subsystem isn't ready yet (caller retries next frame) or the call faulted.
bool loadSave(const std::string& name);

// SEH-guarded: overwrite SaveManager::currentGame in place. The engine's
// load(name) path does NOT refresh currentGame, so loadSave() calls this to keep
// getCurrentGame()/saveInfo() truthful - otherwise the host connect-push saves
// the loaded world over a STALE save name (the fixture-clobber bug).
bool setCurrentGameName(const std::string& name);

// SEH-guarded readiness probe: does the save subsystem report it is up? Returns
// true when the probe symbol is unavailable (so it never blocks auto-load).
bool savesReady();

// SEH-guarded: save the running game under 'name' via SaveManager::save (the
// same path the in-game save menu takes; autosave=false). Used to BAKE fixture
// saves without a manual menu round-trip. Main-thread only.
bool saveGameAs(const std::string& name);

// ---- Protocol 31: coordinated save + session resume -------------------------

// SEH-guarded: read SaveManager::getCurrentGame() and getSavePath() (spike 39
// RVAs, runtime-validated by save_probe) into the caller's buffers. Either
// output may be null. Returns true if at least one string was read; on false
// the caller falls back to the %LOCALAPPDATA%\kenshi\save convention.
bool saveInfo(char* curGame, unsigned int curLen,
              char* savePath, unsigned int pathLen);

// One local save edge captured by the SaveManager::save detour (menu save,
// quicksave, autosave timer, or the mod's own saveGameAs).
struct SaveEdge { char name[48]; int autosave; int suppressed; };
// Detour SaveManager::save: every local save (any path) logs a "[save]
// LOCAL-SAVE" line and queues a SaveEdge the Plugin drains once per tick -
// the coordination trigger for the host-authoritative save transfer.
bool installSaveHook();
// JOIN under save-sync: suppress the LOCAL write entirely (the host's save is
// authoritative; a manual edge is forwarded as PKT_SAVE_REQ instead).
void setSaveSuppress(bool on);
unsigned int drainSaveEdges(SaveEdge* out, unsigned int maxOut);

// ---- Protocol 32: coordinated load ------------------------------------------
// One local load edge captured by the SaveManager::load detour (in-game load
// menu, title-screen load, or the mod's own loadSave).
struct LoadEdge { char name[48]; int suppressed; };
// Detour SaveManager::load: every local load (any path) logs a "[load]
// LOCAL-LOAD" line and queues a LoadEdge the Plugin drains once per tick -
// the coordination trigger for the host-authoritative load broadcast.
bool installLoadHook();
// JOIN under load-sync: swallow the LOCAL load entirely (the host arbitrates;
// a manual edge is forwarded as PKT_LOAD_REQ instead). The bypass lever lets
// the mod's own coordinated loadSave pass through while suppression is on.
void setLoadSuppress(bool on);
// One-shot: the NEXT load passes through the detour even under suppression
// (the join's coordinated load issued on PKT_LOAD_GO / post-transfer).
void setLoadBypassOnce();
unsigned int drainLoadEdges(LoadEdge* out, unsigned int maxOut);

// Protocol 32 probe: the deferred-signal mechanism's state. SaveManager::load
// sets signal=LOADGAME(2) + a frame delay; SOMETHING must then call
// execute() to perform the swap. load_probe found the title-screen loop is
// that something - mid-session the signal just sits there. Returns signal
// (0=idle, 1=SAVEGAME, 2=LOADGAME, 3=IMPORT, 4=NEWGAME; -1 unknown) and
// optionally the remaining frame delay.
int saveMgrSignal(int* outDelay);
// Call SaveManager::execute() once (SEH-guarded) - the engine's own deferred-
// action consumer, driven manually to fire a pending mid-session load. Call
// from the END of the main-loop tick (post engine tick): execute() tears the
// whole world down and rebuilds it synchronously.
bool saveMgrExecute();

// ---- Entity capture / resolve / apply (Stage 1+) ---------------------------

// SEH-guarded: capture the local player's squad into 'out' (up to maxOut),
// filling hand + transform + locomotion. When leaderOnly is true, only
// playerCharacters[0] is captured. Returns the number written.
unsigned int captureSquad(GameWorld* gw, bool leaderOnly,
                          EntityState* out, unsigned int maxOut);

// SEH-guarded: resolve a received entity's hand to this machine's local
// Character*, or 0 if it isn't loaded here / doesn't resolve.
Character* resolve(const EntityState& e);

// SEH-guarded: resolve a Character* from raw hand fields (same path as resolve()).
Character* resolveCharByHand(unsigned int idx, unsigned int ser, unsigned int type,
                             unsigned int cont, unsigned int contSer);

// ---- Canonical typed hand capture/resolve (Phase 5b: ObjectHand) ------------
// Typed replacements for the raw-array read*/resolve*ByHand helpers. They own
// the engine's hand-ctor arg order in ONE place, so no caller hand-indexes a
// u32[5]. The raw-array helpers (readObjectHand/readHand/resolveObjectByHand/
// resolveCharByHand) are thin adapters over these, using ObjectHand's from*/to*
// converters for the two legacy array orders. Migrate call sites to these to
// drop the [3]/[4]/[0]... remaps entirely.
//
// SEH-guarded: read obj/char ->handle into a typed ObjectHand. Returns false on
// null/fault (out is left untouched); the id/serial name the object.
bool handOf(RootObject* obj, ObjectHand& out);
bool charHandOf(Character* c, ObjectHand& out);
// SEH-guarded: resolve a typed hand to this machine's local Character*/RootObject*
// (same engine hand path as resolve(EntityState)). 0 if unloaded here /
// unresolvable / faulted.
Character* resolveChar(const ObjectHand& h);
RootObject* resolveObject(const ObjectHand& h);

// SEH-guarded RAW apply: teleport the body to the entity transform via the
// movement controller (no interpolation). Stage 1 apply path.
bool applyRaw(Character* c, const EntityState& e);

// SEH-guarded: read a character's current world position (for SCENARIO logging).
bool readPos(Character* c, float* x, float* y, float* z);

// SEH-guarded: read a character's hand into out[5] = {index, serial, type,
// container, containerSerial} (the SCENARIO hand-key field order).
bool readHand(Character* c, unsigned int out[5]);

// SEH-guarded: order a character to walk to an absolute destination (host-side
// scenario motion). Routes through the engine's normal locomotion.
bool orderMoveTo(Character* c, float x, float y, float z);

// ---- Stage 3 walk-drive primitives -----------------------------------------

// SEH-guarded: walk the body toward an absolute destination at 'speed' through
// the engine's locomotion (player move-order path, so a player-controlled body
// obeys), so the engine grounds it and plays a real walk/run clip. Re-issued each
// frame toward the moving target; the engine acts on the next tick.
bool walkTo(Character* c, float x, float y, float z, float speed);

// SEH-guarded: halt + teleport to an exact transform (a clean stop at rest).
bool park(Character* c, float x, float y, float z, float heading);
// Halt an in-flight movement goal without teleporting (census-freeze upkeep:
// the AI-suspend hook blocks new decisions, not a committed destination).
bool haltMovement(Character* c);

// SEH-guarded: mirror the source's locomotion state onto the movement controller
// (drives the AnimationClass walk/idle/run selection). Written as the last word.
bool applyMotion(Character* c, bool moving, float speed, float mx, float my, float mz);

// SEH-guarded: read the body's live locomotion truth (the float-bug oracle):
// currentlyMoving + currentSpeed. Returns false on fault / missing controller.
bool readMotion(Character* c, bool* moving, float* speed);

// SEH-guarded: fetch the local player's squad leader (playerCharacters[0]) or 0.
Character* leader(GameWorld* gw);

// SEH-guarded: read the LOCAL camera's world center into out[3] (x,y,z).
// Returns false when the camera is absent or not yet initialised (pre-load).
// Camera-anchored interest lever (spike 35): purely local read; the join
// forwards its value to the host at ~1Hz via PKT_CAM_HINT.
bool cameraCenter(GameWorld* gw, float out[3]);

// Camera-anchored interest anchor stores (protocol 43). The sync layer
// publishes the LOCAL camera center and the peer's (fresh) camera hint each
// tick; interestCenters folds them in as extra anchors, deduped against the
// squad-tab leader spheres. valid=false clears the anchor (camera not up /
// hint stale). Main-thread only.
void setLocalCamAnchor(bool valid, float x, float y, float z);
void setPeerCamHint(bool valid, float x, float y, float z);
// KENSHICOOP_CAM_INTEREST master enable: when off, interestCenters ignores
// the camera anchors (squad-tab leaders only - the pre-43 behavior).
void setCamInterest(bool on);

// SEH-guarded: expose the current interest anchors (up to 4 x,y,z triples
// into out[12]) to the sync layer - the mid-band nearest-first ordering
// prioritizes by distance to the closest ANCHOR (tab leaders + cameras), so
// camera-watched NPCs get mid-band drive slots too. Returns the anchor count.
unsigned int interestAnchors(GameWorld* gw, float out[12]);

// ---- Stage 4 NPC replication primitives ------------------------------------

// SEH-guarded: enumerate characters near the local player and capture every one
// that is NOT a member of the local player squad (i.e. host-authoritative world
// NPCs) into 'out' (up to maxOut), filling hand + transform + locomotion. Returns
// the number written. The same EntityState shape used for squad members, so the
// receiver drives them through the identical resolve/walk-drive path.
unsigned int captureNpcs(GameWorld* gw, EntityState* out, unsigned int maxOut);

// SEH-guarded (Phase 2 mid-band tier): resolve a hand (i,s,t,c,cs order like
// resolveCharByHand) and capture it into an EntityState. Returns false when
// the hand no longer resolves (despawned since the census walk that listed
// it) or the body is a player-squad member (never streamed from the NPC
// tiers). The resolve round-trip is the pointer-liveness proof.
bool captureNpcByHand(GameWorld* gw, unsigned int hIndex, unsigned int hSerial,
                      unsigned int hType, unsigned int hContainer,
                      unsigned int hContainerSerial, EntityState* out);

// SEH-guarded: clear a character's autonomous AI goals so it stops wandering /
// re-pathing on its own. The body is kept IN the engine update list (removing it
// freezes the movement controller and makes our walk-drive/teleport no-op), so
// we quiet it here and let the network transform drive it instead.
void clearGoals(Character* c);

// SEH-guarded: is 'c' a member of the LOCAL player squad (shared-save squad)?
// The join drives a squad member through the player move-order walk-drive (the
// body is inert when uncontrolled), but a world NPC is fully AI-simulated locally
// and must instead be driven kinematically (teleport wins over the local AI).
bool isLocalPlayerChar(GameWorld* gw, Character* c);

// SEH-guarded: pull an NPC OUT of the engine's main AI update list (and clear its
// goals) so the engine stops simulating it autonomously - its local AI would
// otherwise run its own schedule (sit/patrol/jobs) and diverge from the host.
// We then own its transform via kinematic teleport. Returns true on success.
bool suppressNpc(GameWorld* gw, Character* c);

// SEH-guarded: hand a previously-suppressed NPC back to the engine's local AI
// (when the host stops streaming it), so the world keeps living rather than
// leaving a frozen body behind. Also makes the body visible again.
void restoreNpc(GameWorld* gw, Character* c);

// True if 'obj' is one of THIS client's player-squad members. Caller holds the
// SEH frame. Exposed to the sync layer for the recruit membership audit (a
// re-keyed recruit binds + drives as a proxy but is NOT in the join's squad).
bool isPlayerSquad(GameWorld* gw, RootObject* obj);

// SEH-guarded: enumerate nearby world NPCs (excluding the local player squad),
// writing each live Character* into outChars and its hand-bearing snapshot into
// outStates. Returns the count. Used by the join to find NPCs the host is NOT
// streaming so it can suppress them (host-authoritative world).
unsigned int listNpcs(GameWorld* gw, Character** outChars, EntityState* outStates,
                      unsigned int maxOut);

// SEH-guarded: WIDE-radius world-NPC enumeration (protocol 36 census). Same
// exclusions as listNpcs (never the local player squad) but the query reaches
// 'radius' units around every interest center instead of the ~200 u stream
// bubble - the host builds its 1 Hz existence census from this, and the join
// scans the same radius to find local-only ghosts to cull. States are hand +
// position only in spirit (captureOne fills everything; callers use the hand).
unsigned int listNpcsWide(GameWorld* gw, float radius, Character** outChars,
                          EntityState* outStates, unsigned int maxOut);

// SEH-guarded: copy a character's display name into 'out' (always NUL-
// terminated; empty string on any fault). Diagnostics only - cull/suppress
// logs need a human-readable identity to classify pop-out reports.
void charName(Character* c, char* out, unsigned int cap);

// Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate): mint
// a colored text label pinned to a character; the engine's own projection
// tracks the body every frame. colorId: 0 = green (host-driven), 1 = red
// (hidden/suppressed), 2 = yellow (local-only ghost). Returns an opaque
// ScreenLabel handle (null on fault / GUI not up). Main-thread only.
void* markerCreate(Character* c, const char* text, int colorId);
bool  markerUpdate(void* label, const char* text, int colorId);
void  markerDestroy(void* label);

// ---- In-game co-op session panel ---------------------------------------------
// Moved to EngineUi.h (Phase 5a domain split): CoopPanelState, CoopConnectFn,
// CoopDisconnectFn, coopPanelTick, coopOverlayTick. The UI root (Plugin.cpp)
// includes EngineUi.h directly; the adapter (EngineInternal.h) re-includes it.

// ---- Deterministic test-scene setup (host-side; baked into a save) ---------
// Moved to EngineScenario.h (Phase 5a domain split): spawnSeatInFront,
// spawnNpcInFront, spawnMachineInFront, orderWorkAt, findFurnitureNear,
// orderUseBed, setupCraftScene, rearmCraftScene, setupDownScene, rearmDownScene,
// setupSquadScene, pickDownSubject, holdSubjectUpright, orderDownSubject,
// killSubject, woundSubject. The scenario harness (ScenarioSupport.h) + auto-bake
// (Plugin.cpp) include EngineScenario.h; the adapter (EngineInternal.h) re-includes it.

// ---- Down-state drive primitives (SYNC; used by the Replicator down path) ---
// knockDown drops a Character into (on=true) / out of (on=false) full-body
// ragdoll - the join calls it to reproduce a host-authoritative down edge.
bool knockDown(Character* c, bool on);
// Maintain an already-down body each tick by topping the KO timer (no re-collapse).
// Prevents the get-up/flop flicker without re-triggering the ragdoll fall. Join-side.
bool holdDown(Character* c);

// ---- Protocol 21: runtime-spawn proxy replication ---------------------------
// NPC sync resolves bodies by save-stable hand, so a squad the host's spawn
// manager mints at RUNTIME (roaming bandits, dialog ambushes) has a host-only
// hand the join can never resolve. The join asks (PKT_SPAWN_REQ), the host
// describes (these reads), and the join mints a LOCAL proxy body (this spawn)
// that the ordinary world-NPC drive path then owns.

// SEH-guarded (host): describe the runtime spawn at 'c' - template GameData
// stringID, faction stringID (via Faction::getData; "" when unreadable),
// world transform, dead flag, and age (Character::getAge; animals derive body
// SCALE from it - protocol 39 creature-size sync; 0 when unreadable). Returns
// false when the template stringID is unreadable (nothing to describe ->
// negative reply).
bool describeCharacter(Character* c, char* charSid, unsigned int charSidLen,
                       char* facSid, unsigned int facSidLen,
                       float* x, float* y, float* z, float* heading, bool* dead,
                       float* age);

// SEH-guarded (join): mint a LOCAL proxy body from a host description: template
// by CHARACTER stringID, faction by FACTION stringID (FactionManager::
// getFactionByStringID; falls back to a nearby non-player faction when the sid
// doesn't resolve), parked at the host transform, created at the host's 'age'
// (animals scale body size by age; <= 0 or non-finite falls back to the adult
// default). Appearance/equipment are the template's (randomized gear) -
// cosmetic; combat outcomes stay host-authoritative + damage-guarded. Returns
// the proxy Character* or 0.
Character* spawnProxyNpc(GameWorld* gw, const char* charSid, const char* facSid,
                         float x, float y, float z, float heading, float age);

// SEH-guarded (Phase 1 spawn parity, game/ZoneQuery.cpp): is the world block at
// (x,y,z) fully LOADED locally (loaded and not mid-load)? Within a loaded block
// every baked shared-save NPC resolves by hand, so an unresolvable census hand
// positioned there is a genuine host RUNTIME spawn - safe to proxy-mint at any
// distance. resolveZoneQuery() is called from engine::resolve().
bool isZoneLoadedAt(GameWorld* gw, float x, float y, float z);
void resolveZoneQuery();

// SEH-guarded (Phase 1 spawn parity): destroy a previously-minted proxy body
// (GameWorld::destroy, true destruction). Used when the proxy's original hand
// later resolves to a REAL engine body (baked block finished loading) - the
// proxy is a duplicate standing next to the authoritative original. The
// pointer is DEAD after this returns true.
bool despawnProxyNpc(GameWorld* gw, Character* proxy);

// SEH-guarded (join, mint duplicate guard): return a world NPC with the SAME
// template stringID within 'radius' of (x,y,z), skipping any pointer in
// 'excl' (bound proxies / suppressed culls the caller already accounts for),
// or 0. A hit means the census-missing hand is probably that body under a
// hand we cannot correlate - minting would double it.
Character* sameTemplateNear(GameWorld* gw, const char* charSid,
                            float x, float y, float z, float radius,
                            Character* const* excl, unsigned int exclCount);

// spawn_probe / spawn_sync scenario scaffold (SEH-guarded): reproduce a runtime
// squad spawn locally - 'count' world characters in a nearby NON-PLAYER faction
// (findNearbyNonPlayerFaction; leader-faction fallback on a blank save), spread
// in front of the leader, each detached from town-AI so the squad doesn't
// immediately disband to jobs. Fills outHands (readObjectHand layout) up to
// 'count'; returns the number spawned. Their hands are RUNTIME hands - exactly
// the unresolvable-cross-client identity the probe/proxy path exercises.
unsigned int spawnRuntimeSquad(GameWorld* gw, unsigned int count,
                               unsigned int (*outHands)[5]);

// ---- Combat (Phase 3c, L5) -------------------------------------------------

// Read-only snapshot of a Character's combat state (the L5 probe). POD so it can be
// filled inside an SEH frame. target[] is the attack target's hand in readObjectHand
// layout [type,container,containerSerial,index,serial]; all-zero if no target.
struct CombatRead {
    bool         valid;       // at least one field read succeeded
    bool         inCombat;    // isInCombatMode(melee=true, ranged=true)
    bool         modeActive;  // CombatClass::combatModeActive - STABLE across the
                              // slot rotations/combo gaps that flicker inCombat
    bool         ranged;      // isInRangedCombatMode()
    bool         underMelee;  // isLiterallyUnderMeleeAttackRightNowForSure()
    bool         fleeing;     // isFleeing()
    bool         hasTarget;   // attack target is non-null
    bool         waiting;     // engaged but QUEUED (no attack slot): sword state is
                              // CIRCLE_MENACINGLY / WAIT_MENACINGLY / HESITATE
    int          swordState;  // raw swordStateEnum (-1 = unreadable)
    unsigned int target[5];   // attack target hand (or zeros)
};
// SEH-guarded read of c's combat state into *out. Returns out->valid.
bool readCombat(Character* c, CombatRead* out);

// readCombat via a hand (readObjectHand layout). Scenario diagnostics.
bool readCombatByHand(const unsigned int hand[5], CombatRead* out);

// Join-side combat apply: e.task is a combat stance (TASK_COMBAT_MELEE or
// TASK_COMBAT_WAIT) and the subject hand is the attack target. Resolve it locally
// and order THIS body to focus-melee it, so the join's own engine animates the
// fight (replicate the cause, not the animation).
// breakOrder=true additionally routes the attack through the player-ORDER path
// (addOrder clear=true) FIRST: a seat-injected copy holds a player order at its
// stool which outranks the AI goal, so the goal alone never starts the fight.
// Returns 2 ordered / 1 target not loaded / 0 no-op (not a combat intent) / -1 fault.
int applyCombat(Character* c, const EntityState& e, bool breakOrder);

// Engagement escalation (world_parity camp): both the goal and order attack
// paths are silently dropped by the running local AI when the target is a
// locally player-owned body of a non-hostile faction (escaped-prisoner
// recapture: host guards fight, join guards idle). Character::attackTarget is
// the AI's own commit-an-attack entry and bypasses that validation. Returns
// 2 forced / 1 target not loaded / 0 no-op / -1 fault.
int forceAttack(Character* c, const EntityState& e);

// duel test scene: spawn two mutually-hostile non-squad NPCs in front of the leader
// from the SAME nearby faction so they are PEACEFUL on spawn (no attack issued here).
// Hands are stashed so startDuel/rearmDuelScene can trigger/re-issue the fight at
// runtime. Used both to BAKE a neutral 'duel1' save and as the baseline for the live
// combat_order transition test (peaceful->fighting after the join has loaded).
bool setupDuelScene(GameWorld* gw);
// combat_order LIVE-transition pin: after loading a baked 'duel1', re-find the two
// baked duelists (the two non-squad NPCs nearest the leader) and stash their hands so
// startDuel/rearmDuelScene/holdDuelistsPeaceful operate on them. Latches a fixed
// per-duelist anchor for the baseline hold. Returns true if two distinct subjects pin.
bool pickDuelSubjects(GameWorld* gw, unsigned int outA[5], unsigned int outB[5]);
// Hold both pinned duelists peaceful + in range at their latched anchors (clearGoals +
// park) during the combat_order baseline. Returns true if at least one was held.
bool holdDuelistsPeaceful(GameWorld* gw);
// Trigger the fight: order each pinned duelist to melee the other. Returns the number
// of attack orders issued (0 if the duelists aren't known/resolvable).
int  startDuel(GameWorld* gw);
// Re-issue the attack goals on the two pinned duelists if they've disengaged. Returns
// the number of (re-)issued orders, or -1 if the duelists aren't resolvable.
int  rearmDuelScene(GameWorld* gw);
// Fill outA/outB (each [5], readObjectHand layout) with the pinned duelists' hands.
// Returns true if both are known (setupDuelScene ran this session).
bool getDuelHands(unsigned int outA[5], unsigned int outB[5]);
// Host diagnostic: resolve the two pinned duelists and log a "COMBAT ..." line for
// each (inCombat/ranged/underMelee/fleeing/target). Returns the number logged.
int  logDuelCombat(GameWorld* gw);

// LIVE-order test support. pickCraftWorker identifies the worker to drive (non-squad
// NPC nearest the baked fixture) and returns its hand so a scenario can PIN it for
// the whole run; orderCraftWorker then hands THAT pinned worker a work goal mid-run.
// workerHand is in readObjectHand layout: [type,container,containerSerial,index,serial].
bool pickCraftWorker(GameWorld* gw, unsigned int workerHand[5], int* outTask);
bool orderCraftWorker(GameWorld* gw, const unsigned int workerHand[5], int task);
// Hold the pinned worker untasked + parked at the prop during the baseline (an idle
// world NPC patrols out of capture range otherwise). Call each baseline tick.
bool holdWorkerAtFixture(GameWorld* gw, const unsigned int workerHand[5]);

// SEH-guarded: read a RootObject's save-stable hand into out[5] (type, container,
// containerSerial, index, serial). Used to log spawned objects.
bool readObjectHand(RootObject* obj, unsigned int out[5]);

// ---- Phase 4a: container-contents (inventory) replication ------------------
// World objects carry the same save-stable hand as Characters, so a container that
// EXISTS in the shared save resolves cross-client. But runtime-minted items (craft
// output, loot) have host-only hands, so we do NOT drive item objects by hand:
// instead the host streams a container's CONTENTS (template stringID + itemType +
// quantity + quality) keyed by the CONTAINER's hand, and the join reconstructs items
// locally to match (createItem/addItem, removeItemAutoDestroy). Host-authoritative,
// idempotent, loss-tolerant; rides the reliable channel on content change.

// SEH-guarded: resolve a save-stable object hand (cHand layout = type, container,
// containerSerial, index, serial) to this machine's local RootObject*, or 0.
RootObject* resolveObjectByHand(const unsigned int cHand[5]);

// SEH-guarded: capture the LOOSE contents of the container identified by cHand into
// out[] (up to maxOut), filling template stringID + itemType + quantity + quality
// per item (equipped gear skipped in v1). *outHash receives an order-independent
// content fingerprint (0 == empty) so the caller only re-sends on real change.
// Returns the number of item entries written.
unsigned int captureContainerContents(GameWorld* gw, const unsigned int cHand[5],
                                      InvItemEntry* out, unsigned int maxOut,
                                      unsigned int* outHash);

// SEH-guarded: reconcile the local container (cHand) to the desired item multiset:
// add any shortfall (createItem of the template + tryAddItem) and remove any excess
// (removeItemAutoDestroy), per (stringID, itemType) key. count==0 empties it.
// Returns true if anything changed.
bool applyContainerContents(GameWorld* gw, const unsigned int cHand[5],
                            const InvItemEntry* items, unsigned int count);

// 'inventory' setup scene (Phase 4a bake): spawn a save-stable storage container in
// front of the leader and seed it with a couple of items, so both clients load an
// identical container with a resolvable hand. Falls back to seeding the leader's own
// inventory if no storage-building template is found. Host-only; user SAVEs (inv1).
// On success, fills outHand[5] with the anchored container's hand. Returns true if a
// container (or the leader fallback) was prepared.
bool setupInventoryScene(GameWorld* gw, unsigned int outHand[5]);

// Bed+cage scene (bed/cage occupancy sync, protocol 19): spawn one BED and one
// PRISON CAGE near the leader so the user (or the auto-bake) can SAVE a fixture
// save ('bedcage1') both clients load - save-stable furniture hands are what
// let the occupancy events resolve the same bed/cage on both machines.
// Templates are found by keyword over the BUILDING GameData set (camp bed /
// prisoner cage preferred). Returns true when both spawned; hands logged.
bool setupBedCageScene(GameWorld* gw);

// Prisoner-pole scene (protocol 19 kind=4 / engine IN_PRISON): spawn one standing
// prisoner POLE in front of the leader so both clients load a save-stable pole
// hand. A pole is the SAME containment system as a cage (setPrisonMode), just a
// different model - the pole_put controlled test uses it to visibly show a body
// tied to a pole. Auto-baked into 'pole1'. Returns true when the pole spawned.
bool setupPoleScene(GameWorld* gw);

// Resolve the baked inventory container again after load (host + join): v1 anchors on
// the leader's own inventory (a save-stable container that accepts arbitrary items).
// Fills outHand[5]; returns true if found. Used by the scenario + the replicator.
bool pickInventoryContainer(GameWorld* gw, unsigned int outHand[5]);

// SEH-guarded (host-side): add `qty` of a common, general-inventory-friendly item
// template to the container identified by cHand. Writes the chosen template stringID
// to outStringID (so the scenario/oracle knows what to look for). Returns the number
// added (0 on failure). Used both to seed the bake and to perform the live add.
int addTestItemsToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                            char* outStringID, unsigned int outLen);

// SEH-guarded: remove up to `qty` of the SAME common template addTestItemsToContainer
// adds, from the container identified by cHand. Returns the number removed (0 on
// failure). Used by the bidirectional inventory scenario to prove that REMOVALS (not
// just adds) propagate cross-client without loss.
int removeTestItemsFromContainer(GameWorld* gw, const unsigned int cHand[5], int qty);

// SEH-guarded: report the deterministic common test-item template (the one
// addTestItemsToContainer uses) WITHOUT adding anything, so both clients can track the
// same probe sid independently (same gamedata -> same template). Returns 1 on success.
int commonTestItemSid(GameWorld* gw, char* outSid, unsigned int outLen,
                      unsigned int* outType);

// SEH-guarded (protocol 37 fallback): create `qty` of the template (sid, typeCat) and
// add it to the container at cHand - the fabricate path for a transfer whose local
// source copy is missing (desync). Weapons need the manufacturer (and optionally the
// material) sid - the spike-451 recipe inside createItemAndAdd consumes them; other
// types ignore both. KENSHICOOP_WEAPON_FAB=0 disables the weapon branch entirely.
// Returns the number added.
int addItemsToContainerBySid(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, int qty,
                             int qualityBucket, const char* manufacturer,
                             const char* material);

// SEH-guarded (trade_probe / cross-owner transfers): move up to `qty` of (sid, typeCat)
// from the container at srcHand into the container at dstHand by RELOCATING the real
// Item* (removeItemDontDestroy + tryAddItem) - never fabricates or destroys, so it is
// exactly the engine-level mutation the co-op UI drag performs, and it works for gear
// the factory cannot rebuild (weapons). Loose stacks move first, then worn copies (the
// "drag out of an equip slot" case). If the destination refuses the item it is put BACK
// into src (never leaked). Returns the number moved.
// suspendVeto (default true): sanctioned sync relocations keep the trade veto
// suspended so it never refuses them. The xfer_block test passes false to drive
// the engine primitives EXACTLY like a UI drag (subject to the veto), so it can
// assert a cross-owner move is refused (returns 0, item conserved in src).
int moveItemBetweenContainers(GameWorld* gw, const unsigned int srcHand[5],
                              const unsigned int dstHand[5],
                              const char* sid, unsigned int typeCat, int qty,
                              bool suspendVeto = true);

// ---- Cross-owner trade veto (block direct squad-to-squad transfers) --------
// The engine has no single "drag" entry point (the squad-move problem), so a
// UI item drag is remove-then-add: Inventory::removeItemDontDestroy_returnsItem
// on the source, Inventory::tryAddItem on the destination. We detour both to
// REFUSE a move whose source and destination squad characters are owned by
// DIFFERENT clients - forcing players to transfer via ground drops. The veto is
// purely LOCAL (only the dragging client sees the drag), so no packet is needed.

// Owner-classification callback the Replicator supplies (ownHands_ + the full
// player-squad set). Given a save-stable owner hand (readObjectHand layout
// [type,container,containerSerial,index,serial]), returns:
//   0 = not a player-squad member (world NPC / container / vendor - never blocked)
//   1 = a squad member owned by THIS client
//   2 = a squad member owned by the PEER
// A move is cross-owner iff one end is 1 and the other is 2.
typedef int (*InvOwnerClassFn)(const unsigned int ownerHand[5]);
void setInvOwnerClassifier(InvOwnerClassFn fn);

// Enable/disable the cross-owner drag veto (KENSHICOOP_BLOCK_XFER). Off by
// default until set; the veto only fires when a classifier is also registered.
void setBlockXfer(bool on);

// Detour Inventory::tryAddItem + removeItemDontDestroy_returnsItem for the veto
// (and for diagnostic drag-sequence logging under KENSHICOOP_INV_DUMP=1).
// Returns true if both detours installed.
bool installXferBlockHook();

// SEH-guarded (store_probe): like addTestItemsToContainer, but walks the common
// stackable templates until the container ACCEPTS one - storage buildings are
// usually item-type-limited (a Fabric Chest refuses iron plates), which the fixed
// first-template add trips over. Writes the accepted template's stringID to
// outStringID. Returns the number added (0 = every candidate refused / no inv).
int probeAddAnyToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                           char* outStringID, unsigned int outLen);

// ---- Phase W0: world-item (ground drop) diagnostic hooks -------------------
// SEH-guarded: drop `qty` of the LOOSE (sid,type) the object at cHand holds onto the
// ground (Inventory::dropItem). Lets a scenario produce a real world item without UI.
// Returns the number dropped.
// outLastDropped (optional): receives the Item* of the last item dropped (the now-grounded
// object), so the conservation channel can track that real handle for a later pickup.
int dropItemFromInventory(GameWorld* gw, const unsigned int cHand[5],
                          const char* sid, unsigned int typeCat, int qty,
                          void** outLastDropped = 0);

// SEH-guarded DIAGNOSTIC: enumerate WEAPON/ARMOUR/ITEM/CONTAINER world objects near the
// local leader and log each (itemType / sid / hand / pos), so we can characterize what a
// dropped item becomes and whether the interest query enumerates it. The log IS the
// deliverable for the W0 drop_probe (no protocol changes).
void dumpWorldItems(GameWorld* gw);

// SEH-guarded: count loose world items (WEAPON+ARMOUR+ITEM) within `radius` of the
// leader. Oracle cross-check that a drop produced an enumerable ground item.
int countWorldItemsNear(GameWorld* gw, float radius);

// ---- Phase W1: world-item (ground drop) replication ------------------------
// A free ground item captured from the host's interest sphere. `hand` is the host's
// LOCAL engine hand (index/serial) - a stable per-item identity the host's tracker maps
// to a synthetic netId; it is NOT sent on the wire (the join can't resolve it). `hash`
// is a content+position fingerprint so the tracker only re-streams on real change.
struct WorldItemRaw {
    unsigned int   hand[5];     // host-local hand [type,container,containerSerial,index,serial]
    char           stringID[48];// template identity
    unsigned int   itemType;    // GameData::type category
    unsigned short quantity;
    unsigned short quality;     // quality*100 (0 if n/a)
    float          x, y, z;     // world position
    unsigned int   hash;        // content+pos fingerprint
};

// SEH-guarded (host): enumerate free GROUND items within `radius` of the local leader
// (interest scope) into out[] (up to maxOut), deduped by hand. Items inside inventories
// are NOT enumerated by the engine's sphere query (confirmed by the W0 drop_probe), so
// every result is a real world item. Returns the number written.
unsigned int captureWorldItems(GameWorld* gw, WorldItemRaw* out, unsigned int maxOut,
                               float radius);

// ---- Phase W1b: query-free ground-drop capture (town reliability) ----------
// The W1 stream above discovers ground items with getObjectsWithinSphere, which
// fails in towns (a dropped item then never appears / flickers on the peer). To
// discover a drop WITHOUT the spatial query we detour the engine drop primitive
// (Inventory::dropItem): every drop records the exact grounded Item* + its owner
// + description + world position at the drop frame. The Replicator seeds its
// world-item tracker from these edges (query-free) and culls by HANDLE liveness
// (below), so a town drop is streamed reliably.
struct ItemDropEdge {
    unsigned int   ownerHand[5]; // inventory owner that dropped it (readObjectHand layout)
    unsigned int   itemHand[5];  // the now-grounded item's local engine hand
    char           stringID[48];
    unsigned int   itemType;     // GameData::type category
    unsigned short quantity;
    unsigned short quality;      // quality*100 (0 if n/a)
    float          x, y, z;      // world position at the drop frame
};
// Detour Inventory::dropItem so every drop is recorded (drained once per tick).
// Returns true if installed.
bool installItemDropHook();
// Drain captured drop edges into out[] (up to maxOut); clears the queue. Returns
// the number written.
unsigned int drainItemDrops(ItemDropEdge* out, unsigned int maxOut);

// Handle-based liveness of a tracked ground item (query-free cull): resolve the
// item's local engine hand and return 1 if it is still a FREE ground item (it
// exists AND is not inside an inventory), filling outPos[3] with its current
// world position; return 0 if it is gone (destroyed) or has been picked up (now
// in an inventory). Replaces the "vanished from the spatial scan" cull test.
int groundItemLiveness(const unsigned int itemHand[5], float outPos[3]);

// SEH-guarded (join): spawn a LOCAL proxy ground item from the template (sid, typeCat) at
// world position (x,y,z), so the join renders a host-dropped item where the host sees it.
// Returns the spawned object (cull/update it later) or 0. The join owns this proxy; it is
// keyed to the host's netId by the caller (Replicator).
RootObject* spawnWorldItemProxy(GameWorld* gw, const char* sid, unsigned int typeCat,
                                int qty, float x, float y, float z);

// SEH-guarded (join): move an existing proxy to (x,y,z) (a settled world item rarely
// moves, but a re-drop/nudge must track). No-op on fault.
void updateWorldItemProxy(RootObject* proxy, float x, float y, float z);

// SEH-guarded (join): destroy a proxy ground item (engine removal). After this the
// pointer is DEAD; the caller must drop its netId->proxy entry. Returns true if destroyed.
bool removeWorldItemProxy(GameWorld* gw, RootObject* proxy);

// SEH-guarded (host, TEST hook): destroy every free ground item within `radius` of the
// leader (deduped). Used by the world_item_sync scenario to despawn a dropped item so the
// host's tracker culls it and the join removes its proxy (the cull half of the oracle).
// Returns the number destroyed.
int destroyWorldItemsNear(GameWorld* gw, float radius);

// SEH-guarded DIAGNOSTIC: dump every nearby item (ITEM/WEAPON/ARMOUR) with type/isInInventory/
// distance, to learn why a dropped weapon isn't being correlated to a free ground copy.
void diagGroundScan(GameWorld* gw, const unsigned int cHand[5], const char* sid, float radius);

// SEH-guarded: count FREE ground items (not the char's own worn/held copies) of (sid,type)
// within radius of cHand. Lets the relocation spike isolate the item lying on the ground.
int countFreeGroundItemsNear(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, float radius);

// SEH-guarded SPIKE: pick up a free ground item of (sid,type) near cHand by RELOCATING the
// real object into the character's inventory (no createItem) - the conservation primitive
// proving weapons can move bag<->ground without fabrication. Returns 1 on success.
int pickupWorldItemIntoInventory(GameWorld* gw, const unsigned int cHand[5],
                                 const char* sid, unsigned int typeCat, float radius);

// SEH-guarded: world position of the first free ground item of (sid,type) near cHand. Fills
// out[3], returns 1 if found. Used by the W2 drop detector to mirror the drop position.
int firstFreeGroundItemPos(GameWorld* gw, const unsigned int cHand[5],
                           const char* sid, unsigned int typeCat, float radius, float out[3]);

// SEH-guarded: world position of the object at hand. Fills out[3], returns 1 on success.
// Fallback drop position when the spatial item query can't find the dropped weapon.
int objectWorldPos(const unsigned int hand[5], float out[3]);

// SEH-guarded: world position of a tracked Item* (as void*). The drop detector reads the
// REAL dropped object's position to author the exact cursor-drop location. Returns 1 on ok.
int itemWorldPos(void* item, float out[3]);

// SEH-guarded CONSERVATION primitive (Phase W2): relocate the owner's OWN copy of a weapon
// from its bag to the ground at (x,y,z) - the peer-side mirror of a drop. Unequips a worn
// copy to loose first if needed. Never fabricates. Returns the number relocated.
// outDropped (optional): receives the now-grounded Item* so it can be tracked for pickup.
int relocateWeaponToGround(GameWorld* gw, const unsigned int ownerHand[5],
                           const char* sid, unsigned int typeCat,
                           float x, float y, float z, void** outDropped = 0);

// SEH-guarded (Phase W3): capture the WEAPON items the object at cHand holds, with their real
// Item* handles, so the drop detector can track the exact dropped object for a later pickup.
// Fills sids[i] (<48 chars) + items[i] (Item* as void*); returns the count.
unsigned int captureWeaponPtrs(GameWorld* gw, const unsigned int cHand[5],
                               char (*sids)[48], void** items, unsigned int maxOut);

// SEH-guarded (Phase W3): re-home a tracked ground Item* into the inventory at targetHand
// (Inventory::tryAddItem of the existing object - no fabrication). The pickup mirror of
// relocateWeaponToGround. Returns 1 on success. `item` must be a still-live tracked object.
int addItemPtrToInventory(GameWorld* gw, const unsigned int targetHand[5], void* item);

// ---- Equipped-gear (armour/weapon slot) test hooks (inv_equip scenario) ----
// SEH-guarded: report the first EQUIPPED item worn by the object at cHand (its
// template stringID + itemType) and the total count of worn items. Returns 1 if any
// worn item exists. Lets the scenario drive equip/unequip on a KNOWN, race-compatible
// template (gear the character already wears) without hunting for one.
int findEquippedItemKey(GameWorld* gw, const unsigned int cHand[5],
                        char* outSid, unsigned int outLen, unsigned int* outType,
                        int* outEquippedCount);

// SEH-guarded: remove `qty` of the EQUIPPED (sid, type) worn by the object at cHand
// (the "drop/unequip armour" action). Returns the number removed.
int removeEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty);

// SEH-guarded: create and EQUIP `qty` of (sid, type) onto the object at cHand (the
// "equip armour" action). Returns the number equipped.
int addEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                    const char* sid, unsigned int typeCat, int qty);

// SEH-guarded DIAGNOSTIC: log the full inventory (loose _allItems + every section with
// its slot/equip flags + items) of the object at cHand, so we can see where a worn
// weapon actually lives relative to the equip-section walk the snapshot uses.
void dumpInventory(GameWorld* gw, const unsigned int cHand[5]);

// SEH-guarded: UNEQUIP `qty` of the worn (sid,type) at cHand to LOOSE inventory WITHOUT
// destroying it (move slot -> loose, preserving the item for a later re-equip). Returns
// how many moved. The faithful "drag worn item into the bag" action for inv_reequip.
int unequipItemToLoose(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty);

// SEH-guarded: EQUIP `qty` of the LOOSE (sid,type) the object at cHand already holds
// (move loose -> slot). Equips a REAL item so it persists (fabricated equips are
// discarded, d25). Returns how many equipped. Drives the UP path for inv_reequip.
int reequipLooseItem(GameWorld* gw, const unsigned int cHand[5],
                     const char* sid, unsigned int typeCat, int qty);

// SEH-guarded: find a template the character at cHand can WEAR and equip it (so the
// scenario has a known worn item to cycle even when squad members start naked). Writes
// the winning stringID + itemType. Returns 1 on success, 0 if nothing could be worn.
int seedEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                     char* outSid, unsigned int outLen, unsigned int* outType);

// Moved to EngineProbe.h (Phase 5a domain split): the spike-451 weapon-mint
// fabrication probes (diagWeaponCreate, installCreateItemTraceHook,
// probeReplayWeaponMint, probeFabricateWeaponLoose, commonNovelWeaponSid) and the
// spike-402 native-snapshot round-trip (probeNativeSnapshot). The probe scenarios
// (ScenarioSupport.h) include EngineProbe.h; the adapter (EngineInternal.h)
// re-includes it. The spike-401 research block below STAYS here because three of
// its entry points are protocol-38 SYNC calls (interleaved sync+probe).

// ---- Protocol 38: research / tech-tree sync (+ spike 401 store probes) -------
// Spike 401 (research tech-tree store): the unlock store is
// PlayerInterface::technology - a `Research*` with NO KenshiLib header (forward
// declaration only), the documented "progress store unmapped" gap. The probe
// maps it dynamically from plugin context:
//   phase 0  - locate + baseline: log the pointer, hex-dump the first 0x100
//              bytes, classify every qword slot in the first 0x400 that points
//              at a readable GameData record (type + sid), and remember an
//              0x800-byte snapshot.
//   phase 1+ - re-snapshot and log every changed dword vs the previous snapshot
//              ("[r401] diff ..." with a float view). Run between operate()
//              bursts and across a tech completion: the moving dwords are the
//              progress scalar, the mutating region is the completed-set
//              container.
// Returns 1 ok / 0 store unresolved / -1 fault.
int probeResearchStore(GameWorld* gw, int phase);
// ManagementScreen::currentResearch - the tech UI's selected research GameData.
// Writes its sid ('' when none/unresolvable); returns 1 when a sid was written.
int probeCurrentResearchSid(char* outSid, unsigned int outLen);
// Query the live Research store for one RESEARCH GameData by sid, through the
// engine's own predicates (recovered 1.0.65 RVAs: isKnown 0x82f300, canResearch
// 0x832fa0 - the exact calls the research UI's click handler makes). Returns
// 1 ok / 0 unresolvable (no store, sid unknown) / -1 fault. SYNC (protocol 38).
int researchQueryBySid(GameWorld* gw, const char* sid, int* outKnown,
                       int* outCan);
// The engine's own selection lever: Research::startResearch(GameData*)
// (recovered RVA 0x834550) - what a research-UI click commits after its
// canResearch/isKnown gauntlet. The CALLER checks those first (this replays
// the click, not the gauntlet). Returns 1 called / 0 unresolvable / -1 fault.
// SYNC (protocol 38): the join applies a host-authoritative research start.
int researchStartBySid(GameWorld* gw, const char* sid);
// Deterministic subject pick: the FIRST RESEARCH record that is not known and
// canResearch (gamedata enumeration order is shared, so every client picks the
// same sid). Returns 1 picked / 0 none / -1 fault.
int researchPickSubject(GameWorld* gw, char* outSid, unsigned int outLen);
// Enumerate every RESEARCH GameData: log the first maxLog rows (sid, name,
// known, can) plus a total/known summary. Returns the total record count.
unsigned int probeResearchEnum(GameWorld* gw, unsigned int maxLog);
// Collect the sids of every KNOWN research (Research::isKnown over the shared
// RESEARCH enumeration) into outSids (each entry sidCap bytes, NUL-terminated).
// The protocol-38 publish sampler. Returns the number written (<= maxN);
// 0 also covers store-unresolved/fault. SYNC (protocol 38).
unsigned int researchEnumKnown(GameWorld* gw, char* outSids,
                               unsigned int sidCap, unsigned int maxN);
// Research-bench read beyond ProdRead: techLevel (getTechLevel), the
// UseableStuff progress-bar float (0x3A4 - candidate bench-local progress
// scalar) and the power bit. Returns 1 ok / 0 not a research bench / -1 fault.
int probeResearchBenchRead(const unsigned int mHand[5], int* outTech,
                           float* outProg, int* outPower);

// ---- Honest pose oracle (downstream of the actual body, not the task flag) --
// SEH-guarded: read pose signals that reflect the RENDERED body, so a pose check
// cannot self-confirm off the task field we may have written:
//   *pelvis  = pelvis (Bip01) height above the body's ground position, in metres
//              (seated ~0.4-0.6, standing ~0.9-1.1; <0 = unavailable)
//   *idle    = CharBody::isIdle()    (1/0/-1 unknown)
//   *crouched= CharBody::isCrouched()(1/0/-1 unknown)
//   *task    = current TaskType (TASK_NONE if none)
// Returns true if at least the pelvis height was read.
bool readPoseState(Character* c, float* pelvis, int* idle, int* crouched, int* task);

// SEH-guarded: read the body-state bit-flags (BODY_* in Wire.h) off a rendered
// Character - down/KO, ragdoll, dead, crawl. 0 = upright/normal (also on fault).
unsigned short readBodyState(Character* c);

// ---- Stage 5 rest-pose reproduction ----------------------------------------

// SEH-guarded: force 'c' into the task carried by 'e' (e.task) targeting the
// fixture named by e's subject hand, so the body adopts the SAME pose at the SAME
// object as the host (e.g. SIT_AROUND on a specific stool). Resolves the subject
// hand to a local RootObject; only commits if that fixture exists here.
// Deliberately never issues a task with a NULL target (the engine would auto-pick
// a nearby object and WALK the body to it). Returns:
//   2 = applied (fixture resolved at the host transform, pose committed)
//   3 = fixture resolved but it is the WRONG (far) one - a cross-client identity
//       mismatch; NOT committed (issuing it would walk the body to the far seat).
//       Caller should treat like a bad fixture and idle-park in place.
//   1 = subject not loaded here (caller should fall back to a held idle-park)
//   0 = no task in e / required fns unresolved
//  -1 = fault
int applyTask(Character* c, const EntityState& e);

// I9: reproduce a rest pose via the PLAYER-ORDER path (addOrder/addJob with an
// explicit location) instead of the autonomous SIT_AROUND goal. Pins the body to
// THIS fixture at THIS spot (no local seat re-search). Same return codes as
// applyTask (2 applied / 3 wrong-far fixture / 1 not loaded / 0 none / -1 fault).
int applyTaskOrder(Character* c, const EntityState& e);

// I9: detach an NPC from its town/faction (separateIntoMyOwnSquad) so the town-AI
// stops auto-assigning it tasks - an inert, squad-like puppet driven only by the
// host order. Call once per driven NPC. Returns true if the detach was issued.
bool detachFromTownAI(Character* c);

// I10: end the body's current action (CharBody::endAction) so a suspended
// node-stander stops executing its residual walk-to-node and drops to idle
// instead of marching in place. Returns true if the call was made.
bool endAction(Character* c);

// Read a body's RAW top-level Tasker::key (engine TaskType) - the same value the
// host streams as EntityState::rawTask. TASK_NONE if the body has no current
// task; -1 on fault / unresolved fn. Used by the AI-gating divergence check.
int readTaskKey(Character* c);

// True if 'taskKey' is a NODE-anchored rest pose (STAND_AT_NODE /
// STAND_AT_SHOPKEEPER_NODE). These sit/idle the body at an AI node whose subject
// is NOT a resolvable RootObject (applyTask cannot reproduce it), so the only way
// to reproduce the pose on the join is to let the body's OWN local AI execute the
// node behavior - i.e. do NOT suspend its AI and do NOT park it at rest.
bool isNodeAnchoredPose(int taskKey);

// AI-gating probe lever: recruit a world NPC into the local player's squad (the
// "inhabit" path) so it stops self-assigning town tasks and obeys our drive.
// Join-side only. Returns the engine's recruit() result (false if unresolved).
bool recruitNpc(GameWorld* gw, Character* c);

// Phase 1b (cross-game recruit membership): make 'c' a real member of THIS
// client's player squad, assigned to the tab named by newHand's container (the
// recruiter-reported target). The PEER calls this on a recruit/move edge so a
// recruited unit appears in the squad panel in BOTH games. Uses
// Character::setFaction into the resolved platoon (the header-documented
// re-platoon path) - NOT recruitNpc, whose PlayerInterface::recruit detour would
// echo a spurious recruit edge back. Control stays with the OWNER: the caller
// keeps the hand pinned peer-owned, so this side never streams/controls it and
// the host's kinematic drive wins. Idempotent; SEH-guarded. Returns true if the
// body is a player-squad member afterwards.
bool joinPlayerSquadAt(GameWorld* gw, Character* c, const unsigned int newHand[5]);

// AI decision-layer suspension (the faction-safe alternative to recruit): detour
// Character::periodicUpdate so that, for NPCs in the suspended set, the AI "think"
// tick is skipped (no autonomous re-tasking) while the body keeps animating and
// executing its current/injected action. installAiSuspendHook() wires the detour
// once; the set is rebuilt each tick via clearAiSuspend()/addAiSuspend().
bool         installAiSuspendHook();
void         clearAiSuspend();
void         addAiSuspend(Character* c);
unsigned int aiSuspendCount();

// Task-selection observation spike (KENSHICOOP_TASK_SPIKE, OFF by default):
// passively detours CharBody::setCurrentAction - the single seam every task
// SELECTION result (AI scorer or player order) flows through before the body
// executes it, separate from the periodicUpdate brain tick. Proves the seam is
// hookable in isolation and logs the chosen (task, subject, location) tuple per
// body (`[spike] SELECT` lines). Changes no behavior - the precondition probe
// for streaming selection instead of suppressing the whole AI.
bool         installTaskSelectSpikeHook();
void         setTaskSelectSpike(bool on);

// Join-side damage suppression (the "cosmetic fights are actually cosmetic"
// guard): detour Character::hitByMeleeAttack so that, for bodies in the guarded
// set, a locally-simulated melee hit applies NO damage (returns HIT_MISSED
// without running the engine's hit path). Kenshi's medical model is entirely
// LOCAL (blood/limbs/bleed never cross the wire), so without this the join's
// cosmetic copy of a host-authoritative fight accumulates real local damage that
// nothing reconciles - a body could bleed toward a death the host never had.
// Same pattern as the AI-suspend detour: pointer-compared set, rebuilt each tick
// on the main thread, hook installed once.
bool         installDamageGuardHook();
void         clearDamageGuard();
void         addDamageGuard(Character* c);
unsigned int damageGuardCount();
// Cumulative intercepted / passed-through melee hits since load (the damage_guard
// oracle's engagement signal: guarded>0 proves local swings really targeted driven
// bodies AND the hook stopped them).
void         damageGuardStats(unsigned long* outGuarded, unsigned long* outPassed);

// Read a character's current blood level by hand (medical.blood). The vitals
// ground-truth read for the damage_guard conformance oracle: the HOST's victim
// must lose blood in a real fight while the JOIN's driven copy must not.
bool readBloodByHand(const unsigned int hand[5], float* outBlood);

// ---- Player-combat / medical validation primitives (spike 21 field map) ----

// One anatomy part's damage model (protocol 16). Keyed by the part's index in
// MedicalSystem::anatomy - deterministic across clients loading the same save.
struct MedPartRead {
    bool  used;      // this slot carries a live part
    unsigned char partType; // HealthPartStatus::PartType
    unsigned char side;     // LeftRight
    float flesh;     // cut HP (-1 = unreadable)
    float fleshStun; // stun HP (-1 = unreadable)
    float bandaging; // bandage level (-1 = unreadable)
    float juryRig;   // robotics jury-rig level (-1 = unreadable)
    float maxHealth; // _maxHealth (local clamp; NOT sent on the wire)
};

// Full vitals snapshot of a rendered body's LOCAL medical model (the state that
// never crosses the wire by itself - spikes 21-23). Protocol 16: carries the
// FULL anatomy (parts[], anatomy order) plus limb-loss state; the legacy 4-limb
// arrays remain for the scenario scaffolds/oracles (order matches
// RobotLimbs::Limb: [0]=LEFT_ARM [1]=RIGHT_ARM [2]=LEFT_LEG [3]=RIGHT_LEG).
struct MedicalRead {
    bool  valid;
    float blood;
    float bleedRate;    // medical.currentBleedRate
    float limbFlesh[4]; // HealthPartStatus::flesh (current HP; -1 = part missing)
    float limbBand[4];  // HealthPartStatus::bandaging (-1 = part missing)
    float limbMax[4];   // HealthPartStatus::_maxHealth (-1 = part missing)
    bool  unconscious;  // medical.unconcious
    bool  dead;         // medical.dead
    // ---- protocol 16: full anatomy ----
    unsigned int nParts;      // filled parts[] slots (MedicalSystem::anatomy order)
    MedPartRead  parts[12];   // coop::MED_PARTS_MAX slots
    // ---- protocol 16: limb loss (RobotLimbs::Limb order) ----
    unsigned char limbState[4]; // LimbState (0xFF = unknown/no robotLimbs)
    char limbSid[4][48];        // robotic replacement template stringID ("" unless REPLACED)
    // ---- protocol 29: hunger ----
    float hunger; // MedicalSystem::hunger (-1 = unread/not carried)
    float fed;    // MedicalSystem::fed (-1 = unread/not carried)
    float dazed;  // MedicalSystem::dazedOrAlert (DIAGNOSTICS ONLY - never
                  // written; drunk/drug-evidence field for the deferred half)
};
// SEH-guarded read of c's medical model into *out. Returns out->valid.
bool readMedical(Character* c, MedicalRead* out);
// Same, resolving the body from a readObjectHand-layout hand first.
bool readMedicalByHand(const unsigned int hand[5], MedicalRead* out);
// SEH-guarded direct hunger/fed write (protocol 29 probe sentinel lever and
// the driven-copy apply primitive). Values < 0 leave the field untouched.
bool writeHungerByHand(const unsigned int hand[5], float hunger, float fed);

// Phase-2 vitals sync: SEH-guarded WRITE of a received owner-authoritative
// medical snapshot onto a DRIVEN copy (blood, bleedRate; full per-part
// flesh/fleshStun/bandaging/juryRig when in.nParts > 0, else the legacy 4-limb
// arrays; -1 fields skipped; a part is only written when the local anatomy slot
// agrees on partType+side). unconscious/dead are NOT written - the reliable
// event channel owns those transitions. Returns false on fault/null.
bool writeMedical(Character* c, const MedicalRead& in);

// Phase-2 treatment forwarding, owner side: raise-only per-limb bandaging apply
// (bandaging = max(local, band[i]), clamped to _maxHealth; -1 = skip). Raise-only
// makes a re-delivered delta idempotent. Returns the number of limbs raised.
int applyBandageLevels(Character* c, const float band[4]);

// ---- Protocol 17: character stats sync ----------------------------------
// Snapshot of a body's LOCAL CharStats model: raw stat/skill levels indexed by
// kenshi's StatsEnumerated (slot 0 = STAT_NONE, unused; -1 = unreadable), plus
// xp and freeAttributePoints. Like the medical model these never cross the
// wire by themselves - and unlike medical, the PEER's engine resolves real
// fights with its local copy of these numbers.
struct StatsRead {
    bool  valid;
    unsigned int nStats;    // filled slots (STAT_STRENGTH=1 .. STAT_END-1)
    float stats[40];        // coop::STATS_SLOT_MAX; by StatsEnumerated index
    float xp;               // CharStats::xp (-1 = unreadable)
    float freeAttribPts;    // CharStats::freeAttributePoints (-1 = unreadable)
};
// SEH-guarded read of c's CharStats into *out (via CharStats::getStatRef so no
// per-field offsets). Returns out->valid.
bool readStats(Character* c, StatsRead* out);
// Same, resolving the body from a readObjectHand-layout hand first.
bool readStatsByHand(const unsigned int hand[5], StatsRead* out);
// SEH-guarded WRITE of a received owner-authoritative stats snapshot onto a
// DRIVEN copy (-1 fields skipped), then CharStats::_recalculateStats() so
// derived values (attack/block speed, run speed, encumbrance) refresh
// immediately. Returns false on fault/null.
bool writeStats(Character* c, const StatsRead& in);

// Deterministic STAT-RAISE scaffold (stats_sync scenario): set one stat (by
// StatsEnumerated index) on the body at subjHand to 'value' (raise-only) and
// recalculate. Runs on the body's OWNER; the peer asserts the crossing.
bool raiseSubjectStat(GameWorld* gw, const unsigned int subjHand[5],
                      int statId, float value);
// Raise EVERY stat (STAT_STRENGTH..STAT_END-1) on the body at subjHand to at least
// 'value' (raise-only) and recalc once. Returns the count raised. Runs on the
// body's OWNER (combat_win buffs each side's own PC squad to a winning stat line).
unsigned int raiseAllStats(GameWorld* gw, const unsigned int subjHand[5], float value);
// Raise EVERY player-squad member (all tabs) to 'value' in every stat; returns the
// count of members buffed. The "buffpc" setup scene uses it to bake a maxed save.
unsigned int buffAllPlayerStats(GameWorld* gw, float value);

// ---- Protocol 18: carried-body sync --------------------------------------
// Snapshot of a body's LOCAL carry relationship: is it carrying someone
// (Character::isCarryingSomething + the carryingObject hand), and is it
// itself on someone's shoulder (Character::isBeingCarried)?
struct CarryRead {
    bool valid;
    bool carrying;           // this body carries something
    bool beingCarried;       // this body is on someone's shoulder
    unsigned int carried[5]; // carryingObject hand (readObjectHand layout;
                             // zeroed unless carrying)
};
bool readCarry(Character* c, CarryRead* out);
// Order the local CARRIER to pick up the local body at carriedHand via the
// engine's own pickupObject (full chain: carry-mode ragdoll, shoulder bone
// attach, carry animation, transform-follow). Idempotent: already carrying
// that body = success; carrying a different body = refused. SEH-guarded.
bool applyPickup(GameWorld* gw, Character* carrier, const unsigned int carriedHand[5]);
// Release the carrier's carried body (dropCarriedObject; ragdoll = limp
// ground drop). Idempotent: not carrying = success no-op. SEH-guarded.
bool applyDrop(Character* carrier, bool ragdoll);
// Scenario scaffolds (carry_order): resolve the carrier by hand, then
// applyPickup / applyDrop. Run on the CARRIER's owner; the peer asserts the
// crossing.
bool carrySubject(GameWorld* gw, const unsigned int carrierHand[5],
                  const unsigned int carriedHand[5]);
bool dropSubject(GameWorld* gw, const unsigned int carrierHand[5], bool ragdoll);

// ---- Protocol 19: furniture occupancy (beds + prison cages) ---------------
// Snapshot of a body's LOCAL furniture relationship: Character::inSomething
// (IN_NOTHING/IN_BED/IN_PRISON) + the furniture's save-stable hand (inWhat).
// Protocol 41 adds kind 3 (chained/pole): Character::isChained; for it the
// `furn` hand carries the OWNER (Character::slaveOwner), not a building.
struct FurnitureRead {
    bool valid;
    int  kind;             // 0 = none, 1 = bed, 2 = prison cage, 3 = chained/pole
    unsigned int furn[5];  // kind 1/2: inWhat hand; kind 3: slaveOwner hand
};
bool readFurniture(Character* c, FurnitureRead* out);
// ---- Phase 6: shackle (locked equipped item) read lever --------------------
// Read-only snapshot of a character's shackle/lock state for the 6a evidence
// spike. `chained` is Character::isChained (0x320); `hasShackleItem` is
// getChainedModeShackles() != null (an equipped LockedArmour); `lockPresent`
// is that LockedArmour's `lock` (0x2F0) != null (a live lock object). A
// desync fingerprint is when the owner reports chained/locked but the peer's
// driven copy reports it cleared. SEH-guarded; degrades to `chained` only if
// getChainedModeShackles is unresolved.
struct ShackleRead {
    bool valid;
    bool chained;          // Character::isChained (0x320)
    bool hasShackleItem;   // getChainedModeShackles() != null
    bool lockPresent;      // LockedArmour::lock (0x2F0) != null
    unsigned int owner[5]; // Character::slaveOwner (0x328) hand, all-zero if none
};
bool readShackle(Character* c, ShackleRead* out);
// Jail-probe read lever (KENSHICOOP_JAIL_PROBE): Character::isSlave() as int
// (0 NOT_SLAVE / 1 IS_SLAVE / 2 ESCAPING_SLAVE / 3 EX_SLAVE), -1 if unresolved
// or faulted. Read-only; answers whether the join marks its own PC a prisoner.
int readSlaveState(Character* c);
// Phase 6 (6a spike): env-gated ([shackledbg], KENSHICOOP_DEBUG_SHACKLE)
// per-character shackle/lock trace, throttled ~1 Hz. Enumerates nearby world
// NPCs (prisoners are not in the player squad) and logs every body that is
// chained or carries a shackle item, on BOTH clients, so a manual session
// captures the tick a peer's driven copy diverges from the owner. No-op when
// the env var is unset; SEH-guarded per body. `isHost` only tags the line.
void shackleDbgTick(GameWorld* gw, bool isHost);
bool shackleDbgOn();
// Place the local occupant into / remove it from the furniture at furnHand via
// the engine's own setBedMode/setPrisonMode (kind: 1 bed, 2 cage) - pose,
// attach and transform are engine-native. Idempotent: already in the desired
// state = success no-op. On exit (on=false) an unresolvable furnHand falls
// back to the occupant's own inWhat. SEH-guarded.
bool applyFurniture(GameWorld* gw, Character* occupant,
                    const unsigned int furnHand[5], int kind, bool on);
// Scenario scaffolds (bed_put / cage_put): resolve the occupant by hand, then
// applyFurniture against the baked fixture found via findFurnitureNear. Run on
// the OCCUPANT's owner; the peer asserts the crossing.
bool putSubjectInFurniture(GameWorld* gw, const unsigned int subjHand[5], int kind, bool on);
// True when the streamed task is a CONSCIOUS bed-use pose (USE_BED /
// USE_BED_ORDER / SLEEP_ON_FLOOR): those ride the validated L3 fixture-pose
// path (bed_pose), so the occupancy machinery must not fight it - the
// replicator scopes its edges/carve-out/self-heal away from them.
bool taskIsBedPose(int t);
// Self-heal helper (lost/late ENTER edge): find the nearest matching fixture
// (kind: 1 bed, 2 cage) within 'radius' of the streamed position and place
// the occupant in it (the continuous bodyState bit carries no furniture hand).
// Not used for kind 3 (chained/pole): a chain has no searchable building and
// needs the OWNER hand, so its self-heal re-applies via a remembered owner.
bool enterFurnitureNearPos(GameWorld* gw, Character* occupant, int kind,
                           float x, float y, float z, float radius);

// ---- Protocol 20: stealth mode + detection indicators ----------------------
// One seer entry of a sneaker's detection map: WHICH local character notices
// the sneaker, and how far along the notice is (Character::WhoSeesMe).
struct StealthSeer {
    unsigned int npc[5];   // seer hand (readObjectHand layout)
    unsigned char see;     // YesNoMaybe: 0 = NO, 1 = YES, 2 = MAYBE
    float         prog;    // progressOfMaybe (0..1, meaningful for MAYBE)
};
// Snapshot of a body's LOCAL stealth state: Character::stealthMode (the mode
// bool that drives the sneak-walk), stealthUnseen (overall HUD status), and a
// capped copy of whoSeesMeSneaking - the per-NPC detection map the stealth
// marker arrows render from.
struct StealthRead {
    bool valid;
    bool sneaking;          // Character::stealthMode (0xD4)
    unsigned char unseen;   // Character::stealthUnseen (YesNoMaybe key)
    unsigned int nSeers;    // entries copied into seers[]
    unsigned int mapSize;   // TOTAL entries in the engine map (may exceed cap)
    StealthSeer seers[16];
};
bool readStealth(Character* c, StealthRead* out);
// Cheap per-frame mode probe (no map iteration): -1 unreadable, 0 off, 1 on.
int readStealthMode(Character* c);
// Toggle the engine's own stealth mode on the LOCAL body (sneak-walk pose,
// stealthUpdate scanning, skill use - the full chain). Idempotent. SEH-guarded.
bool applyStealth(Character* c, bool on);
// Replay one detection entry between the LOCAL pair: resolve the seer by hand
// and run Character::notifyICanSeeYouSneaking on the local sneaker - exactly
// what the seer's own vision check would have done, so the marker arrows and
// unseen status render natively. SEH-guarded.
bool applyStealthSeer(GameWorld* gw, Character* sneaker,
                      const unsigned int npcHand[5], unsigned char see, float prog);
// Scenario scaffold (sneak_pose / sneak_probe): resolve the subject by hand,
// applyStealth. Run on the machine that owns the action being tested.
bool sneakSubject(GameWorld* gw, const unsigned int subjHand[5], bool on);

// Protocol-16 treatment forwarding: raise-only bandaging apply keyed by ANATOMY
// index (all parts, not just limbs). band[i] = level for anatomy part i, -1 =
// skip. Returns the number of parts raised.
int applyBandageParts(Character* c, const float band[12]);

// Limb-loss replication (Phase C/D): reconcile a driven copy's LimbStates with
// the owner's. states[] is LimbState per RobotLimbs::Limb (0xFF = skip); sid[]
// the robotic replacement template stringIDs ("" = none). ORIGINAL->STUMP/
// REPLACED applies MedicalSystem::amputate(limb, createSeveredItem, zero
// force); ORIGINAL->CRUSHED applies crushLimb. STUMP->REPLACED fabricates the
// prosthetic from sid (LIMB_REPLACEMENT template) and fits it via
// MedicalSystem::setRobotLimbItem. createSeveredItem: the HOST passes true
// (world authority - its real severed item then streams to everyone via the
// world-item channel); the JOIN passes false (the streamed copy is canonical).
// Returns a bitmask of limbs changed (bit i = limb i), 0 if nothing to do,
// -1 on fault.
int applyLimbStates(GameWorld* gw, Character* c, const unsigned char states[4],
                    const char sid[4][48], bool createSeveredItem);

// Join-side severed-item dedupe (Phase C): the join's own damage sim creates a
// REAL severed-limb ground item when an owned member loses a limb, but the
// HOST's copy (spawned by its event-apply) is the canonical one - it streams
// back as a world-item proxy. Destroy the local free severed-limb item(s)
// (Item::itemFunction == ITEM_SEVERED_LIMB) within 'radius' of the body at
// cHand. Returns the number destroyed.
int destroySeveredLimbsNear(GameWorld* gw, const unsigned int cHand[5], float radius);

// limb_loss oracle read: count FREE severed-limb ground items (itemFunction ==
// ITEM_SEVERED_LIMB, not in any inventory) within 'radius' of the body at cHand.
int countSeveredLimbsNear(GameWorld* gw, const unsigned int cHand[5], float radius);

// Deterministic LIMB-LOSS scaffold (limb_loss scenario): run the engine's own
// amputate on the body at subjHand (its OWNER calls this - authoritative damage,
// exactly what a real severing hit does, severed item included). limb =
// RobotLimbs::Limb (0..3). Returns true if the amputation ran.
bool amputateSubjectLimb(GameWorld* gw, const unsigned int subjHand[5], int limb);

// Deterministic LIMB wound scaffold (medic_order / player_ko): lower every limb's
// flesh to 'flesh' (only lower, never heal) and the blood to 'blood' on the body
// at subjHand. Produces a clean, oracle-visible medical delta on the body's OWNER
// without waiting for random combat limb rolls. Returns true if applied.
bool woundSubjectLimbs(GameWorld* gw, const unsigned int subjHand[5],
                       float flesh, float blood);

// Deterministic TREATMENT scaffold (medic_order): bandage every damaged limb on
// the body at subjHand (HealthPartStatus::bandaging -> _maxHealth, raise only) -
// the same medical fields a real first-aid pass raises incrementally
// (MedicalSystem::applyFirstAid, spike 23). Returns the number of limbs bandaged.
int healSubjectBandage(GameWorld* gw, const unsigned int subjHand[5]);

// player_ko revive scaffold: wake the body at subjHand (clear the forced KO +
// ragdoll, clear medical.unconcious, restore blood to a healthy floor) so its
// owner publishes the upright edge -> reliable EVT_REVIVE. Returns true on ok.
bool reviveSubject(GameWorld* gw, const unsigned int subjHand[5]);

// Owner-authoritative death veto (2026-07-15). A DRIVEN copy whose owner still
// reports it ALIVE must never latch DEAD from the local (cosmetic) fight - the
// melee damage guard blocks new wounds, but a lethal frame in an unguarded
// window or a non-melee source can still flip medical.dead, and Kenshi's
// medical model is local-only so nothing reconciles it back (the corpse stays
// dead here while the owner's copy lives). This clears a spuriously-set
// medical.dead + unconcious, releases the ragdoll, and floors blood so the
// body cannot immediately re-die. Death only takes hold on the peer via the
// owner's reliable EVT_DEATH. Returns true if it actually vetoed a local death.
bool vetoLocalDeath(Character* c);

// player_combat victim pick: the UPRIGHT non-squad world NPC nearest the body at
// refHand (excluding, optionally, up to two prior hands - pass 0 for none; the
// second exclude lets a window drop a DUD striker that never engages without
// re-picking it, run 033318). Fills outHand (readObjectHand layout). Returns
// true if one was found.
bool pickCombatVictim(GameWorld* gw, const unsigned int refHand[5],
                      const unsigned int excludeHand[5], unsigned int outHand[5],
                      const unsigned int excludeHand2[5] = 0);

// Order the body at atkHand to focus-melee the body at vicHand (UNPROVOKED, same
// goal path as startDuel). Both hands readObjectHand layout. Returns true if the
// order was issued. Guarded: no-op (true) when the attacker is already in combat,
// so a throttled re-issue doesn't thrash the AI (the combat_order lesson).
bool orderAttackByHand(GameWorld* gw, const unsigned int atkHand[5],
                       const unsigned int vicHand[5]);

// ---- Protocol 22 groundwork: money + vendor trading (shop_probe / money sync)
// Kenshi's wallet is PER-PLATOON (Ownerships::money via Platoon, spike 29): there
// is no global player wallet, so the sync unit is the squad TAB (the same
// partition positional/inventory sync own). Vendors are ShopTrader RootObjects
// with save-stable hands; a purchase is Inventory::buyItem mutating vendor stock
// + wallet LOCALLY on one client only - the divergence the probe measures.

// One nearby vendor (ShopTrader) summary for the shop_probe evidence log.
struct VendorRead {
    unsigned int hand[5]; // readObjectHand layout [type,container,containerSerial,index,serial]
    int          money;   // vendor cash register (RootObject::getMoney; -1 unreadable)
    int          stock;   // item ENTRIES in its inventory (-1 unreadable)
    int          qty;     // summed item quantity (stack-shrink shows here; -1 unreadable)
    int          src;     // which inventory answered: 0 none, 1 ShopTrader's own
                          // (the aggregated trade-UI view; LAZY - null until the
                          // shop opens), 2 the trader CHARACTER's inventory
    unsigned int traderHand[5]; // the trader Character's SAVE-STABLE hand (zeros
                          // if unread) - the cross-client vendor identity (the
                          // ShopTrader wrapper's own hand serial is runtime)
    char         sid[48]; // the wrapper's GameData stringID ("" if unread) -
                          // identifies WHAT these SHOP_TRADER_CLASS objects are
    float        x, y, z; // vendor world position
};

// SEH-guarded: enumerate SHOP_TRADER_CLASS objects within `radius` of the local
// leader into out[] (up to maxOut). Returns the number written.
unsigned int listVendorsNear(GameWorld* gw, VendorRead* out, unsigned int maxOut,
                             float radius);

// SEH-guarded: read the WALLET of the platoon containing the body at mHand
// (Character -> ActivePlatoon -> Platoon -> Ownerships::getMoney - engine
// accessors, never raw offsets). *outMoney = -1 on failure. Returns true on ok.
bool readWalletByHand(const unsigned int mHand[5], int* outMoney);

// SEH-guarded: write the wallet of the platoon containing the body at mHand via
// Ownerships::setMoney (the money-sync apply primitive). Returns true on ok.
bool writeWalletByHand(const unsigned int mHand[5], int money);

// Purchase observability detour (protocol 22, 1c groundwork): hook Inventory::
// buyItem so every REAL trade-UI purchase logs a "[shop] BUY-LOCAL" line
// (seller identity + money, item sid, buyer). Automation cannot reach a real
// purchase in the test save (lazy vendor inventories, no bound trader), so
// manual field sessions carry the evidence for the vendor-stock mirror design.
bool installShopHook();

// ---- Protocol 23: recruitment sync ------------------------------------------
// Hook PlayerInterface::recruit so every SUCCESSFUL local recruit (dialog or
// programmatic) records its before/after hand pair. The Replicator drains the
// queue once per tick into EVT_RECRUIT (subject = old hand, actor = new hand);
// the peer re-keys its local copy of the old hand to the new stream key.
struct RecruitEdge { unsigned int before[5]; unsigned int after[5]; };
bool installRecruitHook();
unsigned int drainRecruitEdges(RecruitEdge* out, unsigned int maxOut);

// ---- Protocol 24: faction-relation sync -------------------------------------
// One relation row between the PLAYER faction and a world faction, both
// directions (the probe must show which side guard aggro consults). sid is the
// faction's GameData stringID - the save-stable cross-client identity protocol
// 21 already round-trips for proxy spawns. enemy/ally are the derived flags
// the AI consults (-1 = accessor unresolved).
struct FactionRead {
    char  sid[48];
    char  name[48];
    float usToThem;   // player faction's relation table toward them
    float themToUs;   // their relation table toward the player (reciprocal row)
    int   enemy;      // player's table: isEnemy(them)
    int   enemyRecip; // their table: isEnemy(player)
    int   ally;       // player's table: isAlly(them)
};
// SEH-guarded enumeration of every REAL world faction with a readable sid.
unsigned int listPlayerRelations(GameWorld* gw, FactionRead* out, unsigned int maxOut);
// SEH-guarded single-row read: both table directions for one faction sid.
bool readRelationBySid(GameWorld* gw, const char* sid, float* outUs, float* outThem);
// SEH-guarded raw relation write (FactionRelations::setRelation) on the player
// faction's row toward sid; reciprocal also writes their row toward the player.
// outBefore/outAfter capture the player-side row around the write.
bool writeRelationBySid(GameWorld* gw, const char* sid, float value, bool reciprocal,
                        float* outBefore, float* outAfter);
// Detour BOTH FactionRelations::affectRelations overloads (the engine's own
// mutation path: EVENT = cause enum, AMT = raw amount). Every real mutation
// logs a "[fac] AFFECT-EV/AMT" line and records a delta the Replicator drains
// (the join forwards its local mutations to the host - the treatments pattern).
struct FactionDelta {
    char  meSid[48];
    char  whomSid[48];
    int   isEvent;
    int   ev;
    float amount;
    float mult;
    float after;
};
bool installFactionHooks();
unsigned int drainFactionDeltas(FactionDelta* out, unsigned int maxOut);

// ---- Protocol 26: door/gate state sync ---------------------------------------
// One row per BAKED door/gate near the interest centers. The wire identity is
// the door Building's save-stable hand - the exact key the furniture/bed sync
// already round-trips cross-client. `open` collapses the animated DoorState
// (OPEN/OPENING = 1, CLOSED/CLOSING = 0) so a door in motion publishes its
// DESTINATION, not the transient.
struct DoorRead {
    unsigned int hand[5]; // [type, container, containerSerial, index, serial]
    float x, y, z;        // world position (census diagnostics only)
    int   open;           // 1 = open/opening
    int   locked;         // DoorLock engaged (0 when no lock)
    int   hasLock;        // door carries a DoorLock at all
    int   state;          // raw DoorState (probe diagnostics)
    int   gate;           // isGate() != 0 (town gates ride the same channel)
    char  name[40];       // GameData name (diagnostics)
    // Protocol 28: the door-to-building link. A door on a PLACED building has
    // a runtime hand, so its wire identity is (parent building hand, index in
    // the parent's ordered `doors` list) translated through the build maps.
    unsigned int parentHand[5]; // owning Building's hand (all-zero = no parent)
    int   doorIndex;            // position in parent->doors (-1 = no parent)
};
// SEH-guarded enumeration of doors among BUILDING objects within radius of the
// interest centers (dual-interest, deduped). Returns the count written.
unsigned int enumDoorsNear(GameWorld* gw, float radius, DoorRead* out, unsigned int maxOut);
// SEH-guarded single-door read by hand. Returns false when the hand does not
// resolve locally or is not a door.
bool readDoorByHand(const unsigned int dHand[5], DoorRead* out);
// SEH-guarded door write through the engine's own action entries:
// openDoor/closeDoor (the polite path - animation, navmesh, sound), falling
// back to _forceDoorOpenUT/_forceDoorClosedUT when the polite call refuses;
// lockDoor/unlockDoor for the lock bit (skipped when the door has no lock).
// wantLocked < 0 leaves the lock untouched. Returns false on resolve failure
// or fault; *outAfter reports the post-write DoorRead when non-null.
bool writeDoorByHand(const unsigned int dHand[5], int wantOpen, int wantLocked,
                     DoorRead* outAfter);

// ---- Protocol 27: placed-building sync ---------------------------------------
// One placed-building/construction-site row. The template GameData stringID is
// the cross-client identity (protocol 21's sid precedent); the local hand is
// RUNTIME-minted for placements (the probe's central finding), so it is only a
// local key - the wire keys mints by the PLACER's hand.
struct BuildRead {
    unsigned int hand[5]; // local hand [type, container, containerSerial, index, serial]
    float x, y, z;        // world position
    float progress;       // ConstructionState::constructionProgress (0..1)
    int   complete;       // ConstructionState::isComplete
    char  sid[48];        // template GameData stringID (wire identity)
    char  name[40];       // template name (diagnostics)
};
// One REAL local placement edge (UI commit detour or programmatic placement).
struct BuildEdge {
    unsigned int hand[5]; // the placed building's local hand (the wire key)
    float x, y, z;        // placement transform (what the peer mints at)
    float yaw;
    int   floorNum;
    int   fromUi;         // 1 = the PreviewBuilding detour, 0 = programmatic
    char  sid[48];        // template sid
};
// Detour PreviewBuilding::placeFinalPreviewBuilding - the ONE engine path a
// player's build-mode commit lands on (justBeenBuilt carries the new Building).
// Every successful placement queues a BuildEdge the Replicator drains.
bool installBuildHook();
unsigned int drainBuildEdges(BuildEdge* out, unsigned int maxOut);
// SEH-guarded enumeration of INCOMPLETE construction sites among BUILDING
// objects within radius of the interest centers (complete/baked buildings are
// legion and never stream - only sites under construction are interesting).
unsigned int enumSitesNear(GameWorld* gw, float radius, BuildRead* out, unsigned int maxOut);
// SEH-guarded single-building read by local hand (works for complete ones too).
bool readBuildingByHand(const unsigned int bHand[5], BuildRead* out);
// SEH-guarded construction-progress write through the engine's own virtuals
// (setConstructionProgress; notifyConstructionComplete fires once at >= 1.0 so
// the site "finishes" natively - scaffold off, materials restored, navmesh).
// Returns false on resolve failure or fault; *outAfter when non-null.
bool writeBuildProgressByHand(const unsigned int bHand[5], float progress,
                              BuildRead* outAfter);
// SEH-guarded programmatic placement by template sid at explicit coordinates
// (the peer-side MINT primitive; completed=false mints a construction site).
// Returns 1 placed, 0 template-miss/factory-refused, -1 fault. outHand gets
// the minted building's local hand.
int placeBuildingAt(GameWorld* gw, const char* sid, float x, float y, float z,
                    float heading, bool completed, unsigned int outHand[5]);
// Probe convenience: pick a deterministic small BUILDING template, place it
// leader-relative (fwd/side) with completed=false, and report everything the
// probe logs. Returns 1 placed, 0 refused, -1 fault. wantDoor selects a
// template WITH a door (shack class) instead of the doorless fixture class
// (protocol 28: placed-door sync needs a door to toggle).
int probePlaceBuilding(GameWorld* gw, float fwd, float side, bool wantDoor,
                       unsigned int outHand[5], char* outSid, unsigned int sidLen,
                       float* outX, float* outY, float* outZ, float* outYaw);

// ---- Protocol 28: placed-building doors + dismantle ---------------------------
// SEH-guarded read of door #doorIndex of the building bHand resolves to
// (index into Building::doors, the engine's own ordered child list). Returns
// false when the building does not resolve, has no door list, or the index
// is out of range.
bool readDoorOfBuilding(const unsigned int bHand[5], unsigned int doorIndex,
                        DoorRead* out);
// SEH-guarded building removal through GameWorld::destroy (the world-item
// cull lever) - the peer-side apply for PKT_BUILD_REMOVE and the probe's
// local destroy lever. Returns false on resolve failure or fault.
bool destroyBuildingByHand(GameWorld* gw, const unsigned int bHand[5]);
// Detour Building::notifyConstructionDismantling - the engine's dismantle-
// complete notification - queueing the building's hand as a removal edge the
// Replicator drains into PKT_BUILD_REMOVE.
bool installDismantleHook();
unsigned int drainRemoveEdges(unsigned int (*out)[5], unsigned int maxOut);
// Queue a removal edge manually (the probe's programmatic destroy path uses
// this: GameWorld::destroy does not pass through the dismantle notification).
void queueRemoveEdge(const unsigned int bHand[5]);

// ---- Protocol 33: production machine sync ------------------------------------
// One production machine / power fixture / farm / research bench row. Identity
// is the Building's hand: save-stable for BAKED machines (the door/furniture
// precedent), runtime for session-placed ones (translated through the
// protocol-27 build maps on the wire). The continuous state is a handful of
// floats: the output buffer (StorageBuilding::productionItem), up to 2 input
// buffers (ProductionBuilding::consumptionItems), the power bit, and the farm
// growth floats. -1 sentinels mean "field not carried / not this class".
struct ProdRead {
    unsigned int hand[5]; // local hand [type, container, containerSerial, index, serial]
    float x, y, z;        // world position (census diagnostics)
    int   classType;      // BuildingClassType (BCTYPE_PRODUCTION/CRAFTING/FARM/...)
    int   complete;       // ConstructionState::isComplete (incomplete rides protocol 27)
    int   powerOn;        // isPowerOn() (0/1; -1 unreadable)
    float powerOutput;    // getPowerOutput() (generators; 0 for consumers)
    int   productionState;// ProductionBuilding::ProductionState (-1 = not production-class)
    float miningLevel;    // _resourceMiningLevel (ore drills / mines)
    // output buffer (StorageBuilding::productionItem; sid "" / -1 = no buffer)
    char  outSid[48];     // output item template GameData stringID
    float outAmount;      // productionItem->amount (stack + progress toward next)
    int   outCap;         // productionItem->maxCapacity
    // input buffers (ProductionBuilding::consumptionItems, first 2; -1 = absent)
    int   nInputs;
    float inAmount[2];
    char  inSid[2][48];
    // research bench evidence (BCTYPE_RESEARCH only; -1 otherwise)
    int   techLevel;      // ResearchBuilding::getTechLevel()
    // farm growth floats (BCTYPE_FARM only; -1 otherwise)
    float grown;          // FarmBuilding::grown
    float died;           // FarmBuilding::died
    float growStart;      // FarmBuilding::growStart
    int   harvested;      // FarmBuilding::harvested
    char  name[40];       // template name (diagnostics)
    char  sid[48];        // template GameData stringID
};
// SEH-guarded enumeration of COMPLETE machine-class buildings (production /
// crafting / furnace / farm / research) within radius of the interest centers
// (dual-interest, deduped). Returns the count written.
unsigned int enumMachinesNear(GameWorld* gw, float radius, ProdRead* out,
                              unsigned int maxOut);
// SEH-guarded single-machine read by local hand. Returns false when the hand
// does not resolve locally or is not a machine-class building.
bool readMachineByHand(const unsigned int mHand[5], ProdRead* out);
// SEH-guarded machine write through the engine's own levers. All fields are
// optional (sentinel = leave untouched):
//   wantPower  - -1 leave, 0/1 switchPowerOn (vtable, the engine's own toggle)
//   outAmount  - >= 0: set the output buffer. useSetItem=true routes through
//                the native virtual setProductionItem(item, stack, progress01)
//                (stack = floor, progress = frac); false writes
//                productionItem->amount directly (the clamp/fight probe leg).
//                The output ITEM template is the machine's current one (outSid
//                of the last read); no template swap in v1.
//   inAmount   - per-input direct ConsumptionItem::amount writes (< 0 = skip)
//   farm       - [grown, died, growStart, harvested] direct writes (< 0 = skip;
//                only applied on BCTYPE_FARM)
// Returns false on resolve failure or fault; *outAfter reports the post-write
// ProdRead when non-null.
bool writeMachineByHand(const unsigned int mHand[5], int wantPower,
                        float outAmount, bool useSetItem,
                        const float inAmount[2], const float farm[4],
                        ProdRead* outAfter);
// SEH-guarded: drive the machine's own operate(worker, amount) with the local
// leader as the worker - the engine's worker-production path, used by the
// probe as a deterministic "a worker is producing here" scaffold (a real
// ordered worker's output rides the same call). Returns false on resolve
// failure / fault.
bool operateMachineByHand(GameWorld* gw, const unsigned int mHand[5], float amount);
// Probe convenience (prod_probe / store_probe): place a COMPLETABLE building
// template leader-relative (fwd/side) as a construction site + queue the
// protocol-27 build edge (so the peer mints a proxy when buildSync is on); the
// caller ramps it complete via writeBuildProgressByHand. kind: 0 = power
// generator, 1 = crafting bench, 2 = storage container (BCTYPE_STORAGE -
// protocol 34's chest subject), 3 = research bench (BCTYPE_RESEARCH - spike
// 401's subject). Returns 1 placed, 0 template-miss/refused, -1 fault.
int probePlaceMachine(GameWorld* gw, float fwd, float side, int kind,
                      unsigned int outHand[5], char* outSid, unsigned int sidLen);

// ---- Protocol 34: storage/machine container sync -----------------------------
// One container-bearing building row for the census: a storage chest
// (BCTYPE_STORAGE) or a machine (the protocol-33 classes - their crafted whole
// ITEMS land in the same Building inventory). Identity is the Building's hand,
// exactly the ProdRead story: save-stable for BAKED buildings, runtime for
// session-placed ones (translated through the protocol-27 build maps on the
// wire). Contents are summarized as the captureContainerContents fingerprint:
// distinct (sid,type) entry count + total quantity + order-independent hash.
struct ContRead {
    unsigned int hand[5]; // local hand [type, container, containerSerial, index, serial]
    float x, y, z;        // world position (census diagnostics)
    int   classType;      // BuildingClassType (BCTYPE_STORAGE or a machine class)
    int   complete;       // ConstructionState::isComplete (incomplete rides protocol 27)
    int   hasInv;         // getInventory() != null (lazy-inventory evidence)
    int   nEntries;       // distinct (sid,type,equipped) entries captured
    int   qtyTotal;       // sum of entry quantities (whole items in the container)
    unsigned int hash;    // order-independent content fingerprint (0 = empty)
    char  firstSid[48];   // first captured entry's template sid (diagnostics)
    int   firstQty;       // first captured entry's quantity
    char  name[40];       // template name (diagnostics)
    char  sid[48];        // template GameData stringID
};
// SEH-guarded enumeration of COMPLETE container-bearing buildings (STORAGE +
// the machine classes) within radius of the interest centers (dual-interest,
// deduped). Per row the contents are captured (up to 64 entries) into the
// count/qty/hash summary - the capacity question's measuring stick. Returns
// the count written.
unsigned int enumContainersNear(GameWorld* gw, float radius, ContRead* out,
                                unsigned int maxOut);
// SEH-guarded single-container read by local hand. Returns false when the hand
// does not resolve locally or is not a container-bearing building class.
bool readContainerByHand(const unsigned int cHand[5], ContRead* out);

// ---- Protocol 35: squad management sync --------------------------------------
// A squad-tab MOVE re-containers the body exactly like a recruit (the hand's
// container fields change), but no single engine function owns the UI path -
// so instead of a detour, poll the roster: the Character* body pointer
// SURVIVES re-containering (the protocol-23 re-key evidence), so a per-poll
// pointer -> hand map diff catches EVERY move flavor (UI drag, createSquad,
// separate-into-own-squad) with one mechanism. A pointer that LEFT the roster
// reports a zeroed after-hand (dismissal/death - the stored hand is reported
// without dereferencing the possibly-freed pointer). Brand-NEW pointers are
// NOT edges here: recruit ENTRY is the protocol-23 detour's story.
struct SquadMoveEdge { unsigned int before[5]; unsigned int after[5]; };
// Poll the live roster against the pointer map, queueing edges (cap 64).
// Call ~1 Hz from the main thread. An EMPTY roster skips the exit sweep (a
// world swap mid-load must not report the whole squad as dismissed); the
// session-reset path clears the map via clearSquadRoster instead.
void pollSquadRoster(GameWorld* gw);
// Drop every pointer baseline (world swap: all Character* dangle).
void clearSquadRoster();
unsigned int drainSquadMoveEdges(SquadMoveEdge* out, unsigned int maxOut);

// SEH-guarded probe lever (squad_probe): programmatically MOVE the member at
// mHand between squad tabs, so the scenario can exercise the re-container
// path the UI drag takes. lever selects the candidate:
//   0 = Character::separateIntoMyOwnSquad(true)  - eject into a NEW tab
//       (tHand ignored; the proven detachFromTownAI lever on a squad body);
//   1 = Character::setFaction(playerFaction, targetPlatoon) - move into the
//       tab of the member at tHand (the header-documented re-platoon path);
//   2 = ActivePlatoon::addCharacterAt(target, member, index) - the container
//       insert taken directly (fallback if lever 1 does not re-container).
// outBefore/outAfter capture the member's hand around the call (wire layout).
// Returns 1 = hand CHANGED (the move landed), 0 = lever ran but the hand is
// unchanged (refused/no-op - a FINDING, not a fault), -1 = fault/unresolved.
int probeMoveSquadMember(GameWorld* gw, const unsigned int mHand[5],
                         const unsigned int tHand[5], int lever,
                         unsigned int outBefore[5], unsigned int outAfter[5]);

// ---- Protocol 23 phase 0: recruitment probe (recruit_probe) -----------------
// SEH-guarded: perform ONE programmatic recruitment via the engine's own
// PlayerInterface::recruit and report the identity evidence. runtimeSubject
// selects the leg: true = recruit a freshly runtime-spawned NPC (host-only
// hand - the spawn-sync regime), false = recruit the nearest BAKED world NPC
// (save-stable hand - the bar-recruit regime). outHandBefore/After capture the
// subject's hand around the call: recruit re-containers the body into a player
// platoon, so the CONTAINER fields change - whether index/serial survive is
// the finding that gates the protocol-23 design.
// Returns 1 recruited, 0 refused / no subject, -1 fault.
int probeRecruit(GameWorld* gw, bool runtimeSubject,
                 unsigned int outHandBefore[5], unsigned int outHandAfter[5],
                 float radius = 60.0f);

// SEH-guarded: force the vendor at vHand to BUILD its stock. A ShopTrader's
// Inventory is created lazily (null until the trade UI first opens - shop_probe
// run 101952: every vendor read stock=-1), so a programmatic purchase must first
// run the engine's own stock builder: the trader's ActivePlatoon::
// refreshInventory(firstTime=true), the same path the shop-open flow schedules.
// Returns 1 = inventory present (already or after refresh), 0 = still null,
// -1 = fault/unresolved.
int ensureVendorStock(GameWorld* gw, const unsigned int vHand[5]);

// SEH-guarded PROBE (shop_probe): drive ONE programmatic purchase - the first
// loose item of the vendor at vHand, bought by the character at buyerHand via
// the engine's own Inventory::buyItem (called on the VENDOR inventory,
// sendingTo = the buyer). Emits "[shop]" log lines with vendor money/stock and
// buyer wallet BEFORE/AFTER so the oracle can measure exactly what one local
// purchase mutates. Writes the bought template stringID to outSid.
// Returns 1 bought (buyItem returned an item), 0 nothing-to-buy/refused,
// -1 fault or unresolved prerequisites.
int probeVendorBuy(GameWorld* gw, const unsigned int vHand[5],
                   const unsigned int buyerHand[5],
                   char* outSid, unsigned int sidLen);

// ---- Consensus game-speed sync ----------------------------------------------

// SEH-guarded read of the world's speed state: *mult = frameSpeedMult,
// *paused = the user-pause flag. Returns false on fault/null.
bool readGameSpeed(GameWorld* gw, float* mult, bool* paused);

// ---- Protocol 25: game-clock sync --------------------------------------------
// SEH-guarded read of the world's game clock: *outHours = the engine's
// in-game-hours TimeOfDay (GameWorld::getTimeStamp_inGameHours - whether it is
// absolute campaign time or hours-since-load is a time_probe question),
// *outHourLenSec = real seconds per game hour. -1 sentinels on failure.
// There is NO clock writer: a timeStamper-base step was prototyped and
// rejected (the calendar does not derive from that timer - Engine.cpp note);
// the Replicator corrects offsets by slewing the sim speed instead.
bool readGameClock(GameWorld* gw, double* outHours, float* outHourLenSec);

// SEH-guarded write through the engine's OWN setters (GameWorld::userPause +
// GameWorld::setGameSpeed) so the UI speed buttons track the applied state
// exactly like a real click. mult <= 0 means paused (the wire convention);
// the pre-pause multiplier is left alone so unpausing restores it. Returns
// false on fault/null/unresolved setters. This IS the loud "user clicked"
// path: the speed-intent hooks see it, so scenarios use it as a simulated
// click and the replicator must NEVER use it for the arbitrated apply.
bool writeGameSpeed(GameWorld* gw, float mult, bool paused);

// The QUIET write (vote/effective decoupling): drive the actual sim speed via
// GameWorld::setFrameSpeedMultiplier + userPause WITHOUT updating the UI speed
// buttons and WITHOUT registering as user intent (reentrancy-guarded against
// the intent hooks). The buttons keep showing the player's last click (their
// VOTE); the replicator enforces the arbitrated min(host, join) underneath.
bool writeGameSpeedQuiet(GameWorld* gw, float mult, bool paused);

// Speed-intent capture (the vote source). Two complementary detectors,
// because the MainBar click handler writes the speed INLINE (2026-07-08
// manual-session finding: real UI clicks never reach a setGameSpeed detour):
//   * hooks on setGameSpeed/userPause/togglePause catch every call that DOES
//     route through the public setters (scenario writeGameSpeed clicks);
//   * a per-tick poll catches the rest: engine state deviating from our last
//     QUIET write = a real click / keyboard pause / RE_Kenshi; the button
//     highlight deviating from the vote snapshot = a click on the speed
//     EQUAL to the current effective (the stuck-vote case - engine state
//     doesn't move, but the UI highlight does).
// consumeSpeedIntent returns true once per new intent and fills the
// requested state (intent pause preserves the requested multiplier,
// mirroring the engine's model).
bool consumeSpeedIntent(GameWorld* gw, float* mult, bool* paused);

// Install the three intent detours (setGameSpeed / userPause / togglePause)
// and seed the intent state from the live engine so the first vote reflects
// the save's starting speed. Call once, from the main thread, after gameplay
// is live. Returns false when any target fails to resolve or hook.
bool installSpeedIntentHooks(GameWorld* gw);

// Spike probe (KENSHICOOP_SPEED_PROBE=1): read the MainBar speed-button
// selected states into out (one char per button, '0'/'1', NUL-terminated;
// cap n-1 buttons). Returns the button count read, -1 when the GUI isn't up.
int readSpeedButtons(char* out, int n);

// Phase 5: publish the combat-cap-active state (from syncSpeed) so the
// speed-path diagnostics can attribute an engine-forced cap vs a user click.
void setSpeedCombatHint(bool inCombat);

// Phase 5: force the MyGUI speed buttons back onto the captured vote when the
// live highlight has drifted (engine dialog auto-pause / combat cap / a
// click=false dehighlight). No-op when already matching. Gate the call on "no
// user acted this tick" so a genuine same-frame click is not fought.
void reconcileVoteButtons();

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_H
