# Spike 5 - DLL-triggered save() to bake spawned scenes

- Type: STATIC (headers) + RUN (recipe)
- Status: DONE (mechanism) / runtime bake deferred
- Source: `SaveManager.h`, `SaveFileSystem.h`, `GameWorld.h`

## Goal

Can the DLL trigger a save programmatically, so a spawned scene becomes a baked,
cross-client-resolvable save (the route spikes 1/2 said is required for repeatable
multi-actor scenes)?

## Findings

1. **Yes - two entry points:**
   - `SaveManager::getSingleton()->save(const std::string& name, bool autosave)`
     (UI-facing) and the lower `int saveGame(location, name)`.
   - `SaveFileSystem::getSingleton()->saveGame(savePath)` (background I/O) + `sync()`.
2. The save captures the live world, so a `spawn N + save("battle1")` sequence bakes
   the spawned actors with **save-stable hands** that resolve identically on both
   clients (proven by the medical spike: baked `down1` subject had the same hand on
   host and join).
3. Save is host-side world state; the join doesn't save. The workflow is: host
   bakes once, then BOTH clients load that named save.

## The reusable pattern this unlocks

```
host: spawn deterministic scene (spike 3/4 templates)  ->  SaveManager::save("battleN")
both: run_test ... -Save battleN   (now both resolve the same actors)
```
This is the missing piece for repeatable battle/medical/combat tests (spikes
2,8-13,22-27 all want it).

## Why not fully DONE at runtime

The API is unambiguous and the bake->load->resolve chain is already proven by the
existing `duel1`/`down1`/`craft1` baked saves (which were made this exact way). A
new auto-bake helper just wires `SaveManager::save` after a spawn; not run here only
to keep this batch's scope contained.

## Recommended follow-ups

- Add `engine::bakeScene(name)` = resolve SaveManager singleton + `save(name,false)`,
  and a `KENSHICOOP_BAKE=name` setup mode so a probe can spawn+bake unattended.
- Use it to generate `battleN` saves for the Group B combat-load runs.
