# Spike 1 - Spawn-N helper + cross-client hand resolution

- Type: DUMP (runtime) + code
- Status: DONE
- Save: c
- Probe: `SpikeScenario` id 1 (host spawns 6 runtime NPCs, both sides census `listNpcs`)

## Goal

Can we spawn NPCs at runtime for repeatable test scenes, and do host runtime-spawned
NPCs become visible/resolvable on the join? This underpins every spawn-based test
(battles, medical subjects) and the whole interest model.

## Method

`powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 1 -Save c -Seconds 30`

Host calls `engine::spawnNpcInFront` 6 times at offsets in front of the leader,
logs each new `Character`'s hand, then re-resolves its own first spawn. Both clients
log `listNpcs` count near their leader every second. Full logs in `01/raw/`.

## Raw evidence

Host:
```
SPIKE 1 baseline role=H near=3
SPIKE 1 spawned i=0 hand=2,1895874048,1,1,3079467776
SPIKE 1 spawned i=1 hand=3,3890014720,1,1,3079467776
SPIKE 1 spawned i=2 hand=4,3689819392,1,1,3079467776
SPIKE 1 spawned i=3 hand=5,2842529280,1,1,3079467776
SPIKE 1 spawned i=4 hand=6,1067382784,1,1,3079467776
SPIKE 1 spawned i=5 hand=7,3584472576,1,1,3079467776
SPIKE 1 spawnedN=6 newNear=9 (baseline=3)
SPIKE 1 host re-resolve firstSpawned=OK
SPIKE 1 census role=H ... near=3 spawnedN=6   (stable for the rest of the run)
```
Join (never spawns; reports its OWN local population):
```
SPIKE 1 baseline role=J near=3
SPIKE 1 census role=J t=1015 near=7 spawnedN=0
SPIKE 1 census role=J t=10015 near=13 spawnedN=0
SPIKE 1 census role=J ... near=15 spawnedN=0  (climbs to 15, stays)
```

## Findings

1. **Spawning works and is cheap.** `spawnNpcInFront` reliably creates a live
   `Character` in front of the leader; 6 spawned in <5 ms with no fault.
2. **Runtime spawns get HOST-LOCAL sequential hands.** The new bodies took index
   `2,3,4,5,6,7` (continuing the host's local object table; the leader was index 1)
   with a random serial, `type=1`, `container=1`, `containerSerial=3079467776`
   (the player faction container). The index is assigned by the LOCAL object
   factory and is **not coordinated across clients** - the join's factory would
   hand out the same low indices to *different* objects.
3. **They persist and resolve on the host** (`re-resolve firstSpawned=OK`), so a
   spawn is durable host-side - good enough for host-authoritative scenes.
4. **They do NOT reach the join.** The join's `spawnedN` stayed 0 and its census
   reflects an INDEPENDENT local population. This matches the existing engine note
   ([Engine.h](../../src/plugin/game/Engine.h) lines 131-135): "a runtime spawn
   alone gets a host-only hand the join can't see."
5. **`listNpcs` counts player-faction spawns only transiently** - `newNear` jumped
   3->9 on the spawn frame, then settled back to 3. `listNpcs` enumerates the
   host-authoritative WORLD-NPC set (non-player factions); the player-faction
   spawns are not part of that streamed set, so they fall out of the count after
   the spawn frame even though they still exist (re-resolve OK).
6. **Big surprise - local NPC populations DIVERGE at the same location.** Host saw
   3 world NPCs near the leader; the join independently saw up to 15. Both loaded
   the identical save at the identical leader position, so each client's local AI
   is spawning/admitting town NPCs on its own. (Feeds spike 17.)

## Implications for co-op

- **Repeatable test scenes cannot rely on runtime spawning being mirrored.** To get
  a controllable actor on BOTH clients you must either (a) BAKE the spawn into a
  save (then it has a save-stable hand both resolve - the existing duel1/down1
  pattern), or (b) stream spawns as host-authored CONTENT keyed by a synthetic id
  the way world items do (the `netId`/conservation model), not by the engine hand.
- For host-authoritative battle/medical scenes (host owns the outcome, join only
  renders), a runtime spawn on the host is fine as long as we ALSO give the join a
  way to render it - which today it cannot for arbitrary spawns. The cleanest path
  for controllable cross-client actors remains "bake then load".
- The host/join local-population divergence means any feature that assumes "the
  same NPCs are near both leaders" is wrong unless the host actively suppresses the
  join's local NPCs and streams its own (the `enforceHostAuthority` suppress path).

## Recommended follow-ups

- A reusable `bakeSpawnScene` helper (spawn N + immediately `save()`) would make
  repeatable multi-actor scenes trivial - see spike 5 (DLL-triggered save).
- Quantify the population divergence and the suppress/stream coverage (spike 17).
- If cross-client live spawns are ever needed, design a "spawn intent" stream
  (template id + spawn pos -> each client spawns locally + maps to a synthetic id),
  mirroring world-item conservation.
