# Spike 82 - bakeScene(name): autonomous save + reload helper (UNBLOCKS 56-80)

- Type: DUMP (capability) -> validated as RUN
- Status: DONE
- Save: c (bakes to a new save "spikebake82")
- Branch commit: <filled at commit>

## Goal

Spikes 51/53/55 proved runtime-spawned NPCs are HOST-LOCAL and never stream to a join,
so the entire combat-sync cluster (56-80: KO/death events, corpse/ragdoll/animation/
projectile agreement host-vs-join) is blocked unless the battle lives in a SHARED SAVE
both clients load. This spike validates the missing piece: can a runtime-spawned battle
be (a) saved AUTONOMOUSLY (no human "save" click) and (b) reloaded with the spawned
bodies PERSISTING as real save entities?

## Method

- New engine helpers:
  - `bakeSave(name)` -> `SaveManager::save(const std::string&, bool autosave)` (RVA
    0x47B1A0), the deferred high-level save (sets SAVEGAME signal; `execute()` writes a
    few frames later, exactly like the already-wired deferred `load`).
  - `savedExists(name)` -> `SaveManager::saveExists(location, name)` (0x36C0D0), diagnostic.
  - `countCharsNear(gw, radius)` -> `getCharactersWithinSphere` near the leader, to detect
    whether the spawned bodies survive a save/reload round-trip.
- `SpikeScenario` id `82`, HOST-ONLY, in-process state machine (no harness change):
  1. t=2s   baseline `countCharsNear(60u)` BEFORE spawn,
  2. t=4s   `spawnLethalBattle(6,1)` (12 bodies) + near-count after spawn,
  3. t=12s  `bakeSave("spikebake82")`,
  4. t=22s  `loadSave("spikebake82")` (deferred reload),
  5. t=42s  near-count again after the reload settles.
- Persistence proven if reload-count ~= spawn-count (>> baseline).

## Findings

1. **Autonomous save WORKS.** `bakeSave` issued=1 and a COMPLETE, valid Kenshi save was
   written to disk at `...\kenshi\save\spikebake82\` containing `quick.save` (2.26 MB),
   `platoon/`, `zone/`, `portraits_texture.png`. No human interaction.
2. **In-process reload WORKS.** `loadSave` issued at t=22s; the MEMBER anchor stream shows
   an ~8.4s gap (09:34:37 -> 09:34:45) = the world teardown + load screen. The leader
   resolves with the SAME save-stable hand afterwards (it was saved with that hand).
3. **Runtime-spawned battle bodies PERSIST through save+reload.** near60 went baseline=2
   -> 14 after spawning 12 -> **13 after reload** (delta +11 over baseline ~= the 12
   spawned). The spawned NPCs are now ORDINARY save entities, not transient runtime-only
   objects.
4. **`savedExists("spikebake82")` returned 0** despite the save existing - confirming its
   `location` argument needs the real save path, not "". Use the on-disk folder / a
   time-delay instead of this probe to confirm a write.

## Validation

- `82/raw/host.log`:
  `bake phase0 baseline near60=2`,
  `bake phase1 spawned=12 near60=14`,
  `bake phase2 bakeSave issued=1`,
  `bake phase3 savedExists=0 loadSave issued=1 t=22172`,
  `bake phase4 reload near60=13 (base=2 spawn=14 delta=11) persisted=1 -> PASS`.
- Save on disk: `ls ...\kenshi\save\spikebake82\` -> quick.save 2261118 bytes + platoon/ +
  zone/ (a complete save).
- Reload gap: ~8.4s discontinuity in `SCENARIO MEMBER` timestamps right after the
  loadSave at t=22s.

## Open questions / hypotheses (UNVALIDATED)

- **Direct cross-client confirmation** (a JOIN loads `spikebake82` and sees the SAME 13
  bodies with matching hands) was NOT run here - only host-local persistence + reload.
  It is strongly implied (the bodies are now shared-save entities, and the mod's whole
  model is that shared-save hands resolve identically on both clients), but it must be
  confirmed by a 2-client run on `-Save spikebake82`.
- **Hostility persistence**: whether the two factions stay mutually ENEMY after reload
  (so the baked battle RESUMES fighting on load) is unconfirmed - faction relations are
  saved, so likely yes, but unmeasured.
- **Save cost / timing** under larger battles (40v40) and whether the SAVEGAME signal can
  stall a networked session were not measured (here it was host-only, 12 bodies).
- One spawned body did not persist (14->13): wandered out of 60u, died, or the count
  includes the 2 natives - not pinned down.

## Implications for co-op (UNBLOCKS the cluster)

- **The bake-then-share workflow is fully automatable**: host `spawnLethalBattle` ->
  `bakeSave(name)` -> distribute/launch both clients on `name`. This is the prerequisite
  for EVERY spike 56-80 (they all need the join to see the battle).
- Recommended harness pattern: a TWO-PHASE spike - phase A (host-only) bakes the scene;
  phase B runs `-Save <baked>` with both clients and measures the cross-client property.
- `spikebake82` is a reusable baked 12-body battle save (left on disk intentionally).

## Recommended follow-ups (now unblocked)

- IMMEDIATE: 2-client run on `-Save spikebake82` to confirm the join sees the 13 bodies
  (closes the only open item) and whether the battle resumes hostile.
- Then drain 56-80 using baked battles instead of live runtime spawns.
- Keeper primitives (reverted): `engine::bakeSave`, `savedExists`, `countCharsNear`,
  plus the spike 51-55 battle helpers; `SpikeScenario` id 82.
