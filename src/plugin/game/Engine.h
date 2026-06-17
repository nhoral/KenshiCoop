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

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_H
