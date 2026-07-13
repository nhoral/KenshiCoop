# Spike 4 - Faction creation/assignment + hostility control

- Type: STATIC (headers) + code
- Status: DONE
- Source: `Faction.h`, `FactionRelations.h`, `Platoon.h`, `RootObjectFactory.h`

## Goal

How do we create factions, set them hostile, and assign characters to them - so we
can stage battles (two hostile sides that auto-fight) without manual attack orders.

## Findings

1. **Create/obtain factions (`FactionManager`):**
   `getOrCreateFaction(id, name)`, `getOrCreateFaction(GameData*)`,
   `getFactionByName`, `getFactionByStringID`, `getEmptyFaction`,
   `getAllFactions()`. Direct `Faction(name)` + `setup(GameData*)` also exists.
2. **Hostility (`FactionRelations`, `faction->relations`):**
   `setEnemy(Faction*)`, `setNoLongerEnemies`, `declareWar(Faction*)`,
   `setRelation(who, float)`, `isEnemy/isAlly/isCoexisting`, `getFactionRelation`,
   `affectRelations(p, FactionEvent, mult)`. `FactionEvent` enum includes
   `KILLED_ONE_OF_US_DIRECTLY`, `DEFEATED_AN_ENEMY`, `FIRST_AIDED_US`, etc.
3. **Assign characters:** via platoon membership, not a per-Character setter -
   `Platoon::setFaction(Faction*)`, `Faction::addActiveObject(RootObject*,
   ActivePlatoon*)`, `ActivePlatoon::addActiveObject` / `addCharacterAt`.
   `createNewEmptyActivePlatoon(squadTemplate, permanent, pos)` makes a fresh squad.
4. **Spawn directly into a faction:** `createRandomCharacter(faction, pos, owner,
   template, home, age)` and `createRandomSquad(faction, ...)` take the faction.
5. **Player detection:** `Faction::isThePlayer()` / `isPlayer` (PlayerInterface*).

## Implications for co-op

- **Battle staging is straightforward:** `getOrCreateFaction("spikeRed")` +
  `getOrCreateFaction("spikeBlue")`, `red->relations->setEnemy(blue)`, then
  `createRandomSquad(red, posA, ...)` and `createRandomSquad(blue, posB, ...)` near
  each other. The engine AI will start the fight on its own (no per-NPC attack
  order needed) - the missing primitive that spike 8 flagged for combat-load tests.
- Faction relation changes are global game state; in co-op they are host-authored
  and would need to replicate (today they don't) if the join's AI reads them. For a
  host-authoritative battle test that's fine (host owns the fight).
- `FactionEvent` (KILLED_ONE_OF_US, etc.) is how reputation shifts - relevant if we
  ever sync faction standing.

## Recommended follow-ups

- Add `engine::spawnHostileBattle(perSide, redTemplate, blueTemplate)` using the
  above; feed spikes 8-13 (combat-load scale, sync fidelity, attribution).
- Decide whether faction-relation changes need replication (likely host-only world
  state, but verify the join's AI doesn't make divergent hostility decisions).
