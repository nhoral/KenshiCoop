# Spike 20 - Distance rate-tiering vs divergence

- Type: code + RUN (recipe)
- Status: PARTIAL - design

## Goal

Could we update far entities less often than near ones (LOD by distance) to save
bandwidth, and how much divergence would that introduce?

## Code reasoning

- Today every streamed entity is published at the SAME cadence (once per publish
  tick, up to MAX_PUBLISH=160), regardless of distance. There is no per-entity rate
  tiering and no dirty/priority queue.
- The join interpolates between updates (Interp.cpp smoothness path), so a slower
  update rate for far entities would mostly show as coarser interpolation, not pops
  - acceptable for distant background NPCs.

## Findings (design)

1. **Rate-tiering is feasible** as a publish-side scheduler: e.g. tier 0 (<60u)
   every tick, tier 1 (60-120u) every 3rd tick, tier 2 (120-200u) every 6th tick.
   The wire/format need not change - only WHICH entities are included each tick.
2. **Divergence is bounded by interpolation**: a far NPC moving at run speed updated
   every 6th tick (~100ms at 60fps) drifts at most a fraction of a body length
   between updates - negligible for background, visible for a fast far COMBATANT
   (so combatants should be force-promoted to tier 0, tying into spike 14 priority).
3. Saves the most where it matters least (distant idle crowd), which is exactly the
   bandwidth that scales with battle size (spike 12).

## Why PARTIAL

The scheme is clear from the publish code; the actual bandwidth saving + visible
divergence need a town/battle run to quantify (couldn't exercise density at `c`).

## Implications for co-op

- Rate-tiering + the spike 14 priority ordering together make large battles and
  dual-interest affordable on the wire. Recommended order: caps/priority (14) ->
  hysteresis (18) -> rate-tiering (20) -> dual-interest (19).

## Recommended follow-ups

- Add a distance-tier scheduler to `publishOwned`/NPC stream; force combatants to
  tier 0; measure bytes/s vs entity count (spike 12) in a dense scene.
