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

// down_order LIVE-transition helpers (Stage 2). pickDownSubject pins the non-squad
// NPC nearest the leader (its hand, readObjectHand layout); holdSubjectUpright keeps
// it idle/in-range during the baseline; orderDownSubject knocks THAT subject out at
// the order point so the join must transition upright -> down.
bool pickDownSubject(GameWorld* gw, unsigned int subjHand[5]);
bool holdSubjectUpright(GameWorld* gw, const unsigned int subjHand[5]);
bool orderDownSubject(GameWorld* gw, const unsigned int subjHand[5]);

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
