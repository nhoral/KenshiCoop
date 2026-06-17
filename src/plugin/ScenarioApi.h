// ScenarioApi - the thin action facade scenarios use to touch the game.
//
// These functions are DECLARED here and IMPLEMENTED in KenshiCoop.cpp, where
// the resolved engine function pointers (g_spawnFn, g_recruitFn, ...) and the
// SEH-guarded primitives (guardedSpawn, guardedPark, ...) already live. Keeping
// the implementation there means scenarios get crash-safe, main-thread-only
// game mutation for free, and Scenario.cpp stays free of engine/Boost headers.
//
// Every function is defensive: it null-checks and returns a failure value (0 /
// false) rather than throwing, so a scenario can probe state without risking
// the process. All positions are Ogre world coordinates.

#ifndef KENSHICOOP_SCENARIOAPI_H
#define KENSHICOOP_SCENARIOAPI_H

#include <ogre/OgreVector3.h>

#include "../netproto/Protocol.h"

class GameWorld;
class Character;
class Faction;
class GameData;
class RootObjectContainer;

namespace coop {

// Emit one already-formatted scenario log line (routes through coopLog so it
// lands in both the coop log and kenshi.log). Use for CHECK / SCENARIO lines.
void scenarioLog(const char* msg);

// Number of remote players currently tracked (received at least one state for).
// > 0 means the peer is in-game and sending, so scenarios can use this as a
// connection handshake to align their clocks across the staggered launch.
int remotePlayerCount();

// ---- State accessors (all return 0 on failure) ----------------------------
Character*           localPlayer(GameWorld* gw);          // first player character
Faction*             playerFaction(GameWorld* gw);        // player's faction
RootObjectContainer* playerSquadContainer(GameWorld* gw); // active squad container
GameData*            playerTemplate(GameWorld* gw);        // player's own GameData
GameData*            lookupTemplate(GameWorld* gw, const char* stringId); // by string id
int                  playerSquadSize(GameWorld* gw);       // squad member count, -1 on fail

// ---- Actions ---------------------------------------------------------------
// Spawn a character from 'tmpl' (or the player's own template if tmpl==0) at
// 'pos' and enlist it into the player squad. Returns the new Character* or 0.
Character* spawnIntoPlayerSquad(GameWorld* gw, GameData* tmpl,
                                const Ogre::Vector3& pos);

// Read a character's live world position. Returns false on fault.
bool getCharPos(Character* c, Ogre::Vector3* out);

// Read a character's save-stable hand as {index, serial, type, container,
// containerSerial}. Returns false on fault.
bool getCharHand(Character* c, u32 out[5]);

// Snap a character to an exact transform and stop it (halt + teleport).
bool teleportChar(Character* c, const Ogre::Vector3& pos, float headingRad);

// Order a character to walk to an absolute destination (engine locomotion).
bool moveCharTo(Character* c, const Ogre::Vector3& dest);

// Clear a character's autonomous AI goals (best-effort).
void clearCharGoals(Character* c);

// Remove a spawned character from the world (best-effort). The pointer is dead
// afterward and must not be reused.
void despawnChar(GameWorld* gw, Character* c);

} // namespace coop

#endif // KENSHICOOP_SCENARIOAPI_H
