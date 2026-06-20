# Spike 3 - Enumerate spawnable character/squad templates

- Type: STATIC (headers) + code
- Status: DONE
- Source: `RootObjectFactory.h`, `GameData.h`, `GameDataManager.h`, `RaceData.h`

## Goal

What templates can we spawn from, and how do we look them up by id / enumerate them?
This is the basis for DETERMINISTIC spawning (spike 2) and battle/faction scenes.

## Findings

1. **Template lookup (`GameDataContainer`, inherited by `GameDataManager`):**
   - `GameData* getData(const std::string& sid)` and
     `getData(const std::string& sid, itemType category)` - by string id.
   - `GameData* getData(int id)` / `getDataByName(name, category)`.
   - `void getDataOfType(lektor<GameData*>& list, itemType type)` - **enumerate all
     templates of a type** (the key API for "list every race / squad / character").
   - `createNewData(itemType, forceID, name)` - mint a new template at runtime.
2. **`itemType` categories** (from `Enums.h`) include `RACE`, `HUMAN_CHARACTER`,
   `ANIMAL_CHARACTER`, `SQUAD_TEMPLATE`, `FACTION`, `FACTION_TEMPLATE`,
   `VENDOR_LIST`, `ITEM`, `WEAPON`, `ARMOUR`, `CONTAINER`, `SHOP_TRADER_CLASS`.
3. **Races:** `RaceData::AllRaces` (static `unordered_map<string,RaceData*>`),
   `RaceData::getRaceData("greenlander")`. So races are trivially enumerable.
4. **GameData identity:** `int id` (0x1C), `std::string name` (0x28),
   `itemType type` (0x50), `std::string stringID` (0x58).
5. **Spawn entry points** (RootObjectFactory) all take a `GameData*` template:
   `createRandomCharacter(faction, pos, owner, characterTemplate, home, age)`,
   `createRandomSquad(faction, pos, ..., squadTemplate, ...)`, `createItem(gd,...)`,
   `createBuilding(data,...)`.

## Implications for co-op

- We can build a **deterministic** spawner by id: `getData("greenlander", RACE)` (or
  a specific HUMAN_CHARACTER template) + `createRandomCharacter`, instead of the
  current `spawnNpcInFront`'s random pick (spike 2). Same id on both clients → same
  body type (still needs baking for identical HANDS, spike 1/2).
- A one-time DUMP probe could log `getDataOfType(HUMAN_CHARACTER)` /
  `SQUAD_TEMPLATE` to produce a catalog of usable ids for future test scenes.

## Recommended follow-ups

- `SpikeScenario` id 3: resolve the GameDataManager (no singleton in headers - find
  it via `ou->savedata` / a global), `getDataOfType` for RACE + HUMAN_CHARACTER +
  SQUAD_TEMPLATE, log the first N stringIDs. Commit the catalog as a reference.
- Add `engine::spawnTemplateById(sid)` for deterministic test scenes.
