# Spike 19 - Dual-interest feasibility

- Type: DUMP/code
- Status: DONE (feasibility) - design

## Goal

Can the host stream TWO interest spheres (one around each player's leader) so the
players can operate apart and each still has a consistent local world?

## Code reasoning

- Today there is a single host-leader-centered sphere (`captureNpcs` centers on
  `gw->player->playerCharacters[0]`, spike 15).
- `getCharactersWithinSphere(center, far, near, ...)` takes an arbitrary center, so
  calling it a SECOND time centered on the JOIN leader's last-known position is
  trivial. The host already KNOWS the join leader position (the join publishes its
  squad every tick).

## Findings

1. **Dual-interest is feasible and small.** Sketch:
   - Host keeps the join leader's position (from the join's published squad).
   - `captureNpcs` runs twice: once around the host leader, once around the join
     leader; results are MERGED + deduped by hand before publish.
   - `MAX_PUBLISH=160` (spike 14) must be split/raised, and the join must NOT
     suppress NPCs that fall in EITHER sphere.
2. Cost: a second sphere query (~the same as the first) + more published entities.
   At `c`'s densities (spike 8/14, ~4-9 NPCs) this is negligible; in a town it
   pushes against the 96/160 caps and wants priority ordering (spike 14).
3. The join's `enforceHostAuthority` suppression must become sphere-aware (don't
   suppress locals near the join leader that the host now streams), else the two
   collide.

## Implications for co-op

- This is the key unlock for "players roam apart". It's an incremental change to the
  existing capture/publish/suppress pipeline, not a rewrite - but it depends on
  fixing the caps (spike 14) and hysteresis (spike 18) first, and on the population
  divergence/suppression question (spike 17).

## Recommended follow-ups

- Prototype: add a second `captureNpcs` centered on the join leader's published
  position; merge+dedupe; raise/split MAX_PUBLISH; make suppression sphere-aware.
  Re-run spikes 16/17 to confirm the roaming join now has a consistent world.
