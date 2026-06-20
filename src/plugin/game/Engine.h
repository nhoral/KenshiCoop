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

// SEH-guarded readiness probe: does the save subsystem report it is up? Returns
// true when the probe symbol is unavailable (so it never blocks auto-load).
bool savesReady();

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

// SEH-guarded: mirror the source's locomotion state onto the movement controller
// (drives the AnimationClass walk/idle/run selection). Written as the last word.
bool applyMotion(Character* c, bool moving, float speed, float mx, float my, float mz);

// SEH-guarded: read the body's live locomotion truth (the float-bug oracle):
// currentlyMoving + currentSpeed. Returns false on fault / missing controller.
bool readMotion(Character* c, bool* moving, float* speed);

// SEH-guarded: fetch the local player's squad leader (playerCharacters[0]) or 0.
Character* leader(GameWorld* gw);

// ---- Stage 4 NPC replication primitives ------------------------------------

// SEH-guarded: enumerate characters near the local player and capture every one
// that is NOT a member of the local player squad (i.e. host-authoritative world
// NPCs) into 'out' (up to maxOut), filling hand + transform + locomotion. Returns
// the number written. The same EntityState shape used for squad members, so the
// receiver drives them through the identical resolve/walk-drive path.
unsigned int captureNpcs(GameWorld* gw, EntityState* out, unsigned int maxOut);

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

// SEH-guarded: enumerate nearby world NPCs (excluding the local player squad),
// writing each live Character* into outChars and its hand-bearing snapshot into
// outStates. Returns the count. Used by the join to find NPCs the host is NOT
// streaming so it can suppress them (host-authoritative world).
unsigned int listNpcs(GameWorld* gw, Character** outChars, EntityState* outStates,
                      unsigned int maxOut);

// ---- Deterministic test-scene setup (host-side; baked into a save) ---------
// These spawn objects into the live world so the user can SAVE the result. Once
// saved, the spawned chair/NPC become ordinary save entities with save-stable
// hands that resolve identically on both clients - the only way a controlled
// actor can sync (a runtime spawn alone gets a host-only hand the join can't see).

// SEH-guarded: place a seat (stool/chair/bench) furniture building 'fwd' metres in
// front of the local leader (and 'side' metres to its right), facing the leader.
// Returns true if a seat template was found and createBuilding did not fault.
// 'spawned' (optional) receives the new object for hand logging.
bool spawnSeatInFront(GameWorld* gw, float fwd, float side, RootObject** spawned);

// SEH-guarded: spawn a loose world character (player faction, NOT enlisted in the
// squad, so it travels the host-authoritative NPC path) at 'fwd'/'side' from the
// leader. Returns the new Character* (or 0).
Character* spawnNpcInFront(GameWorld* gw, float fwd, float side);

// SEH-guarded: place an OPERABLE work-fixture building (research bench, training
// dummy, machine...) 'fwd'/'side' from the leader, facing the leader, owned by
// 'owner' (pass a non-player faction so the fixture is a world-owned station, or 0
// for unowned). Used by the 'craft' setup scene to bake a save-stable work station
// both clients resolve. 'spawned' (optional) receives the new object for hand
// logging. Returns true if a machine template was found and createBuilding did not
// fault.
bool spawnMachineInFront(GameWorld* gw, float fwd, float side, Faction* owner,
                         RootObject** spawned);

// SEH-guarded: issue 'c' a player-style work order/job to perform 'task' (e.g.
// OPERATE_MACHINERY / USE_TRAINING_DUMMY) AT 'fixture', at the fixture's position.
// Clears prior goals first. Used by the setup scene to force the host NPC into the
// work pose so the captured task streams to the join. Returns true if issued.
bool orderWorkAt(Character* c, RootObject* fixture, int task);

// 'craft' setup scene (host-side): spawn a save-stable work fixture + a world NPC
// and force the NPC into the matching work pose (OPERATE_MACHINERY / training), so
// the host captures the work task and the join reproduces it once the save is
// baked. All task-enum selection is internal. Returns true if a fixture spawned.
bool setupCraftScene(GameWorld* gw);

// Craft RE-ARM (host-side): a worker's addGoal intent does NOT serialise, so a baked
// craft scene reloads with an idle worker. Re-find the baked fixture + nearest
// non-squad worker by search and re-issue the work goal, so the host resumes
// streaming the work task. Idempotent (no-ops when the worker is already on task);
// safe to call once on load and then periodically. Returns true if a goal is active.
bool rearmCraftScene(GameWorld* gw);

// Stage 2 body-state. knockDown drops a Character into (on=true) / out of (on=false)
// full-body ragdoll. setupDownScene spawns a non-squad world NPC and ragdolls it (a
// controllable "down" subject); rearmDownScene re-applies ragdoll to that subject on
// an interval (a healthy body recovers and stands). Host-only.
bool knockDown(Character* c, bool on);
// Maintain an already-down body each tick by topping the KO timer (no re-collapse).
// Prevents the get-up/flop flicker without re-triggering the ragdoll fall. Join-side.
bool holdDown(Character* c);
bool setupDownScene(GameWorld* gw);
// Re-knock every non-squad NPC near the leader so down bodies stay down (a healthy
// ragdoll recovers; ragdoll does not persist across save/load). Returns count, or -1.
int  rearmDownScene(GameWorld* gw);

// Phase 3.5 bake: build a SECOND player squad tab (recruit two bodies, separate one
// into its own player platoon). Gives the bidirectional ownership partition two tabs
// to split (host owns tab 0, join owns tab 1). Host-only; user SAVEs the result.
bool setupSquadScene(GameWorld* gw);

// down_order LIVE-transition helpers (Stage 2). pickDownSubject pins the non-squad
// NPC nearest the leader (its hand, readObjectHand layout); holdSubjectUpright keeps
// it idle/in-range during the baseline; orderDownSubject knocks THAT subject out at
// the order point so the join must transition upright -> down.
bool pickDownSubject(GameWorld* gw, unsigned int subjHand[5]);
bool holdSubjectUpright(GameWorld* gw, const unsigned int subjHand[5]);
bool orderDownSubject(GameWorld* gw, const unsigned int subjHand[5]);
// death_order: KILL the pinned subject (scaffold) so Character::isDead() flips and
// the host emits a reliable EVT_DEATH. Idempotent; re-assertable on a throttle.
bool killSubject(GameWorld* gw, const unsigned int subjHand[5]);
// combat_kill: WEAKEN the subject (lower blood only, never heal/collapse) so an
// ongoing real melee downs it decisively within the window - the down edge comes from
// genuine combat, not a scaffold. Returns true if applied. subjHand readObjectHand layout.
bool woundSubject(GameWorld* gw, const unsigned int subjHand[5], float blood);

// ---- Combat (Phase 3c, L5) -------------------------------------------------

// Read-only snapshot of a Character's combat state (the L5 probe). POD so it can be
// filled inside an SEH frame. target[] is the attack target's hand in readObjectHand
// layout [type,container,containerSerial,index,serial]; all-zero if no target.
struct CombatRead {
    bool         valid;       // at least one field read succeeded
    bool         inCombat;    // isInCombatMode(melee=true, ranged=true)
    bool         ranged;      // isInRangedCombatMode()
    bool         underMelee;  // isLiterallyUnderMeleeAttackRightNowForSure()
    bool         fleeing;     // isFleeing()
    bool         hasTarget;   // attack target is non-null
    unsigned int target[5];   // attack target hand (or zeros)
};
// SEH-guarded read of c's combat state into *out. Returns out->valid.
bool readCombat(Character* c, CombatRead* out);

// Join-side combat apply: e.task == TASK_COMBAT_MELEE and the subject hand is the
// attack target. Resolve it locally and order THIS body to focus-melee it, so the
// join's own engine animates the fight (replicate the cause, not the animation).
// Returns 2 ordered / 1 target not loaded / 0 no-op (not a combat intent) / -1 fault.
int applyCombat(Character* c, const EntityState& e);

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

// DIAGNOSTIC: probe whether the engine factory can instantiate WEAPON base templates at
// all (createItem returns null for the save weapon even with manufacturer+material). Tries
// the first `maxTry` weapon templates with no/man/man+mat and logs [wpndiag] success
// counts. Trials are added to cHand's inventory and immediately destroyed.
void diagWeaponCreate(GameWorld* gw, const unsigned int cHand[5], int maxTry);

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

// AI decision-layer suspension (the faction-safe alternative to recruit): detour
// Character::periodicUpdate so that, for NPCs in the suspended set, the AI "think"
// tick is skipped (no autonomous re-tasking) while the body keeps animating and
// executing its current/injected action. installAiSuspendHook() wires the detour
// once; the set is rebuilt each tick via clearAiSuspend()/addAiSuspend().
bool         installAiSuspendHook();
void         clearAiSuspend();
void         addAiSuspend(Character* c);
unsigned int aiSuspendCount();

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_H
