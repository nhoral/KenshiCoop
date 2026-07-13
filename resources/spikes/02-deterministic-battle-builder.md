# Spike 2 - Deterministic battle-scene builder reproducibility

- Type: DUMP (derived from spike 1) + code
- Status: DONE
- Save: c

## Goal

Can we build a battle scene by spawning that is REPEATABLE across runs and identical
across clients (so a battle test is deterministic)?

## Evidence

From spike 1 (`01/raw/host.log`): six sequential spawns took hands
`index = 2,3,4,5,6,7` (continuing the host object table after the leader's index 1),
each with a RANDOM serial, `type=1`, `container=1` (player faction),
`containerSerial=3079467776`. `spawnNpcInFront` is `createRandomCharacter`-based
([Engine.cpp](../../src/plugin/game/Engine.cpp) include note line 24).

## Findings

1. **Index is deterministic per session, serial is not.** On a fresh load the same
   spawn order yields the same low indices (2,3,4,...) because the object table
   starts from the same baseline, but the SERIAL is randomized per spawn, so the
   full hand differs run to run and **client to client**.
2. **`createRandomCharacter` randomizes appearance/stats/gear**, so even the bodies
   themselves differ between two spawns - not battle-deterministic.
3. **Cross-client identical scenes are impossible via live spawning** (spike 1):
   two clients' factories assign colliding indices to DIFFERENT bodies. The only
   route to an identical, repeatable scene on both clients is **bake-then-load**: an
   existing baked save (`duel1`, `down1`) loads with save-stable hands that resolve
   identically everywhere.

## Implications for co-op

- For repeatable battle TESTS, the pattern is: spawn the desired scene ONCE on the
  host, `save()` it to a named save (spike 5), then load that save on both clients.
  The duel1/down1 saves already prove this works for 2-NPC scenes.
- For a host-only perf/scale probe (spike 8) live spawning is fine because we only
  measure the host and don't need cross-client identity.
- A deterministic spawner would need: a fixed template id (not "random"), a fixed
  spawn order, and a seed - and even then cross-client identity still requires
  baking, because hands are assigned locally.

## Recommended follow-ups

- Implement `bakeBattleScene(perSide, templateId)` = spawn deterministic templates
  by id (spike 3) + `save()` (spike 5). That single helper makes every battle/medical
  test repeatable and cross-client-identical.
