# Spike 6 - Equip loadouts on spawned NPCs

- Type: STATIC (headers) + code
- Status: DONE
- Source: `RootObjectFactory.h`, `Inventory.h`, `Item.h`, `RaceData.h`

## Goal

Can we put weapons/armor on spawned NPCs so battle test bodies are realistically
equipped (and fight with weapons)?

## Findings

1. **Item creation:** `RootObjectFactory::createItem(GameData* gd, hand, weaponMesh,
   matData, levelOverride, flagUniform)` or `createItem(GameData* itemState)`.
   Templates resolved by id/type (`getData(sid, WEAPON|ARMOUR)`, spike 3).
2. **Equip:** `Inventory::equipItem(Item*)`; section add via
   `Inventory::addItem(Item*, qty, dropOnFail, destroyOnFail)`. A character's
   inventory is reachable from the Character/RootObject (`getInventory()`).
3. **Auto-clothing:** `RootObjectFactory::chooseMyClothing(gear, dataList, listName,
   RaceData*, noShoes)` populates a gear list for a race - `createRandomCharacter`
   already uses this internally (spike 1's spawns came clothed/armed by their
   template).
4. So two paths: (a) rely on the character template's own default loadout
   (automatic), or (b) explicitly `createItem` + `equipItem` for a controlled
   loadout.

## Implications for co-op

- Battle test NPCs can be armed either by choosing armed templates (spike 3) or by
  explicit equip. For deterministic tests, explicit equip by item stringID is best.
- Equipment is part of the character's inventory; once the scene is BAKED (spike 5)
  the equipped items get save-stable hands and resolve on both clients - same rule
  as world items (the inventory-sync work already handles equipped-item resolution).
- Live (un-baked) equip changes ride the existing inventory-sync channel for OWNED
  squad members; for world NPCs they follow the same not-replicated caveat as other
  world-NPC detail unless streamed.

## Recommended follow-ups

- Add `engine::equipByStringId(c, weaponSid, armourSids...)` for deterministic
  loadouts, used by the battle-bake helper (spike 5) so `battleN` saves have known,
  identical loadouts on both clients.
