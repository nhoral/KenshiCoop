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
5. **CROSS-CLIENT CONFIRMED (sub-spike 82b).** A 2-client run on `-Save spikebake82`
   (id `82b`, both roles census near-leader): the JOIN sees `near60=14` (vs vanilla save
   'c' baseline of 2) and `fighting=9` - it loads and SEES the baked battle, and the
   battle RESUMES combat (factions stayed hostile across the save). Host/join near-counts
   agree (host 14/17, join 14/12 at t=6s/24s). `82b/raw/join_2.png` shows the join's
   character amid the active melee with downed bodies + a blood pool. Note: fighting-count
   differs host(11-12) vs join(6-9) - a combat-MODE-state cross-client divergence that
   spikes 56-80 will now quantify properly.

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

- **Combat-mode cross-client divergence**: 82b showed host fighting=11-12 vs join
  fighting=6-9 on the SAME baked battle. Whether this is sampling skew (~95ms apart +
  bodies dropping) or a real combat-state desync is the FIRST thing 56-80 should pin down.
- **Save cost / timing** under larger battles (40v40) and whether the SAVEGAME signal can
  stall a networked session were not measured (here it was host-only, 12 bodies).
- One spawned body did not persist (14->13): wandered out of 60u, died, or the count
  includes the 2 natives - not pinned down.

## Resolved by sub-spike 82b

- Direct cross-client visibility: CONFIRMED (join near60=14 vs baseline 2; join_2.png).
- Hostility persistence / battle resumes on load: CONFIRMED (join fighting=9 then 6).

## Implications for co-op (UNBLOCKS the cluster)

- **The bake-then-share workflow is fully automatable**: host `spawnLethalBattle` ->
  `bakeSave(name)` -> distribute/launch both clients on `name`. This is the prerequisite
  for EVERY spike 56-80 (they all need the join to see the battle).
- Recommended harness pattern: a TWO-PHASE spike - phase A (host-only) bakes the scene;
  phase B runs `-Save <baked>` with both clients and measures the cross-client property.
- `spikebake82` is a reusable baked 12-body battle save (left on disk intentionally).

## Recommended follow-ups (now unblocked)

- DONE: 2-client confirmation (sub-spike 82b) - join sees + fights the baked battle.
- Drain 56-80 using baked battles instead of live runtime spawns: phase A host-only bake
  -> phase B 2-client `-Save <baked>` measuring the cross-client property.
- FIRST target: the host-vs-join combat-mode count divergence 82b surfaced.
- Keeper primitives (reverted): `engine::bakeSave`, `savedExists`, `countCharsNear`
  (+fighting overload), plus the spike 51-55 battle helpers; `SpikeScenario` ids 82/82b.
