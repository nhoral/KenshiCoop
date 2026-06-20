# Spike 16 - Leader-separation: peer update cutoff distance

- Type: RUN (recipe) + code
- Status: PARTIAL

## Goal

As the two players' leaders move apart, at what separation does the shared world
stop updating for the roaming player?

## Code reasoning (grounded in spike 15)

- The host streams WORLD NPCs/items within **200u (NPCs) / 60u (items)** of the
  **host leader** only (single sphere, spike 15).
- The OWNED SQUAD is published unconditionally (no distance gate in `publishOwned`
  - it's the core of co-op), so **peer squad members always sync** regardless of
  separation.
- The join publishes only its own squad (`streamNpcs_=false`), so the host always
  sees the join's squad too.

## Findings (predicted)

1. **Two cutoffs, very different:**
   - Peer SQUAD positions: **no cutoff** - always synced (both directions).
   - Shared WORLD (NPCs/items around the roaming join): degrades once the join
     leader is **>200u from the HOST leader**, because the host streams nothing
     around the join. Beyond that the join falls back to its own divergent local
     world (spikes 1/17).
2. So players can SEE EACH OTHER anywhere, but only share a consistent WORLD while
   co-located within ~200u of the host leader.

## Why PARTIAL

The 200u prediction is from the capture radius, not a measured walk-apart run. A
direct probe is straightforward and worth doing.

## Recommended follow-ups

- `SpikeScenario` id 16: order the JOIN leader to walk away from the host leader in
  steps; both log (a) peer-squad-resolved? and (b) world-NPC census + how many
  resolve as host-streamed, vs separation distance. Confirm the squad-never-drops /
  world-drops-at-200u prediction and find any hysteresis (spike 18).
- This directly motivates dual-interest (spike 19).
