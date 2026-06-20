# Spike 55 - Attribution correctness with 4+ simultaneous fights

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Scale spike 54 from 2 to 4 simultaneous fights and check two things: (a) does FACTION
attribution stay correct (every fighter targets an ENEMY, never an ally), and (b) do the
4 fights stay SPATIALLY separate, or do combatants cross-target between fights so the
"fights" merge into one brawl?

## Method

- `spawnLethalBattle(perSide, clusters)` (spike 54 mechanism: two EXISTING factions set
  mutually ENEMY via `FactionRelations::_NV_setEnemy`) with `clusters=4`. Each side's
  bodies are split into 4 spatial knots ~8u apart in depth (cluster depths ~6/14/22/30u).
- `lethalCensus` now also reports `maxTargetDist` = the MAX fighter->target XZ distance
  across all fighting combatants. If fights stay local, this stays near the within-cluster
  size (a few u); if combatants target enemies in OTHER clusters, it grows past the
  cluster spacing.
- `SpikeScenario` id `55`, HOST-ONLY, 12v12 (24 bodies, 4 clusters of 3v3), 60s, save `c`.

## Findings

1. **Faction attribution scales perfectly to 4 fights.** `targetFriend=0` on EVERY census
   tick for the full 60s; `targetEnemy` peaked at **24** (all 24 fighters targeting the
   enemy faction). No mis-attribution at 4x concurrency.
2. **Casualties scale with fight count.** `peakDown=10` simultaneously KO'd (blue side ran
   down to 7 KO'd); progressive down-counts across the window. `totalDead=0` (KO, not
   death, again - matches spike 54).
3. **The 4 fights do NOT stay spatially separate - they merge into one brawl.**
   `maxTargetDist` reached **72u**, far beyond the ~24u total cluster span and the ~26u
   geometric max at spawn. Combatants acquire targets in other clusters and roam, so 4
   clusters spaced 8u apart collapse into a single mobile melee (confirmed visually in
   `host_4.png` - one ~24-body mass, not 4 knots). `local=0`.

## Validation

- All numbers from `55/raw/host.log` `SPIKE 55 many ...` census lines + final
  `SPIKE 55 many-summary total=24 peakDown=10 totalDead=0 peakTgtE=24 sawTgtF=0
  maxDist=72.0 casualties=1 attribOk=1 local=0 -> PASS`.
- Finding 1: `tgtF=0` on every line; `tgtE` up to 24.
- Finding 2: `d` (down) counts rise across timestamped lines (b: d2->d3->...->d7).
- Finding 3: `maxDist` ranges 31-72u (lines), >> the 24u cluster span; `host_4.png` shows a
  single merged mass.

## Open questions / hypotheses (UNVALIDATED)

- **Cross-client attribution** still untested (runtime spawns don't replicate - spikes
  51/53). The host-local attribution is correct; the JOIN's view needs a baked save (82).
- **What spacing keeps fights separate?** `maxTargetDist` 72u suggests clusters must be
  spaced >~80-100u (beyond combat target-acquisition + roam radius) to stay distinct.
  Untested - would need a spacing sweep.
- **Killer/kill credit** and **down->death timing** unmeasured (same as 54).
- **maxTargetDist is post-movement** (units roam during 60s); the pure target-acquisition
  radius at t=0 was not isolated.

## Implications for co-op

- Faction-based attribution (who is hostile to whom) is robust under load and concurrency
  - good news for replicating combat intent.
- "Multiple separate fights" is NOT a thing the engine preserves spatially when units are
  close: expect them to coalesce. A co-op test that needs N genuinely independent fights
  must space them far apart (>~100u) or they become one combat for sync purposes.
- Reinforces 54: prioritise baked-save tooling (82) - it is the only path to the
  cross-client attribution question that actually matters.

## Recommended follow-ups

- Spacing sweep to find the separation distance that keeps fights independent.
- Pull spike 82 (bakeScene) forward for cross-client attribution.
- Keeper primitives (reverted): `engine::spawnLethalBattle(gw,perSide,clusters)`,
  `lethalCensus` (+`maxTargetDist`), `SpikeScenario` id 55.
