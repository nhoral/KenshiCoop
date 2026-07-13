# Spike 15 - Current host interest radius

- Type: DUMP (runtime) + code
- Status: DONE
- Save: c

## Goal

Document the exact interest radius the host uses to decide which world NPCs (and
items) it streams - the baseline for every distance/handoff question (spikes 16-20).

## Raw evidence (code)

NPC interest query (`captureNpcs` / `listNpcs`,
[Engine.cpp](../../src/plugin/game/Engine.cpp) ~896/~965):
```
g_getCharsFn(gw, &q, &center=leaderPos, farRadius=200, nearRadius=120,
             always=30, maxFar=96, maxNear=96, skip=0);
```
World-item interest ([Replicator.cpp](../../src/plugin/sync/Replicator.cpp) ~329):
```
const float RADIUS = 60.0f;   // ground-item interest scope (v1)
```
Other tuned radii: down/duel subject picks use 30u; craft worker search 40u;
`findNearbyNonPlayerFaction` reaches 6000u (setup only, not streaming).

## Findings

1. **World NPCs stream within a 200u far / 120u near sphere** centered on the
   **host leader's position** (not the camera, not the join). `always=30` forces
   inclusion within 30u. Caps: 96 far + 96 near (spike 14).
2. **Ground items stream within only 60u** - much tighter than NPCs.
3. The center is the host leader; there is **no second interest sphere** around the
   join's own leader for the host stream (the join does run its OWN `listNpcs` for
   local suppression, mirroring the same 200u, but the authoritative stream is
   host-leader-centered).
4. Units note: Kenshi world units are large; 200u is roughly a town-block scale.
   Runtime at `c` showed only ~4-9 world NPCs inside 200u (low-density area).

## Implications for co-op

- Today both players effectively must stay within ~200u of the HOST leader to see
  a consistent shared world around them. Beyond that, the host streams nothing and
  the join falls back to its own (divergent, spike 1) local simulation.
- This single host-centered sphere is the core limiter for "how far can the join
  roam" (spike 16) and motivates dual-interest (spike 19): each client streaming
  its own neighborhood so two players can operate apart.
- The 60u item radius vs 200u NPC radius asymmetry means dropped items can be
  invisible to a peer who can still see the NPCs around them.

## Recommended follow-ups

- Make the radius a named, logged constant (it's currently a literal in two query
  call sites) so it's tunable and visible.
- Spikes 16-20 build directly on these numbers.
