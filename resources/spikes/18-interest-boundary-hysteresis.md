# Spike 18 - Interest-boundary hysteresis / churn

- Type: code + RUN (recipe)
- Status: PARTIAL

## Goal

When the leader hovers near the 200u interest edge, do entities flicker in/out
(churn), and is there hysteresis to damp it?

## Code reasoning

`captureNpcs`/`listNpcs` use a **hard** `farRadius=200` (spike 15). There is NO
hysteresis band (no separate enter/exit radius) and NO dwell timer: an entity at
~200u is included or excluded purely on this frame's distance. The world-item
stream uses the same hard-radius pattern (RADIUS=60).

## Findings (predicted)

1. **Churn is expected at the boundary.** A leader (or an NPC) oscillating around
   200u will repeatedly enter/leave the streamed set frame-to-frame, causing the
   join's copy to spawn/despawn (or freeze/resume driving) - visible popping and
   wasted enter/exit work.
2. The conservation/world-item path already deduplicates by hand, which softens
   item churn somewhat, but NPC streaming has no such damping at the radius edge.

## Why PARTIAL

The hard-radius churn is clear from code; the visible severity needs a hover-at-edge
run to quantify (how many flips/second, how jarring).

## Implications for co-op

- Before any roaming/dual-interest work, add **hysteresis**: an outer ENTER radius
  (e.g. 200u) and a larger EXIT radius (e.g. 240u), plus a short dwell timer, so
  entities don't thrash at the boundary.

## Recommended follow-ups

- `SpikeScenario` id 18: park the host leader so a wandering NPC crosses 200u
  repeatedly; log per-tick streamed-set membership churn count. Then add the
  enter/exit hysteresis and re-measure.
