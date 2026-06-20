# Spike 14 - Interest cap / overflow behavior

- Type: DUMP (runtime) + code
- Status: DONE
- Save: c
- Evidence: spike 8 run (256-slot `listNpcs` buffer under heavy spawn) + code constants

## Goal

What happens when the number of NPCs near the leader exceeds the capture/publish
caps? Does the stream truncate gracefully or break?

## Method

Code read of the capture path + the spike-8 runtime run (which spawned up to 90
bodies while sampling `listNpcs` into a 256-slot buffer).

## Raw evidence (code)

`engine::captureNpcs` and `engine::listNpcs`
([Engine.cpp](../../src/plugin/game/Engine.cpp) ~885, ~956) both issue:
```
g_getCharsFn(gw, &q, &center, /*far*/200.0f, /*near*/120.0f, /*always*/30.0f,
             /*maxFar*/96, /*maxNear*/96, /*skip*/0);
```
Publish cap ([Replicator.cpp](../../src/plugin/sync/Replicator.cpp) ~121):
```
const unsigned int MAX_PUBLISH = 160;   // squad + NPCs published per tick
```
`enforceHostAuthority` uses a 256-slot `listNpcs` buffer (MAX_NPCS=256).

Runtime (spike 8): the 256-slot `listNpcs` count stayed ~4-9 (transient spikes to
36 on spawn frames) - the natural world-NPC density at `c` is low, so no cap was hit.

## Findings

1. **Two hard caps stack:** the engine query truncates at **maxFar=96 + maxNear=96**
   characters per call, and the replicator publishes at most **MAX_PUBLISH=160**
   entities/tick (squad members consume some of that budget first; NPCs fill the
   remainder).
2. **Overflow is graceful truncation, not a crash** - both are simple `n < maxOut`
   loop bounds; excess characters are silently dropped from the stream. There is no
   prioritization (e.g. nearest-first beyond what the engine's far/near split gives).
3. The interest sphere is **200u far / 120u near** (see spike 15), so the cap only
   bites in a genuinely dense crowd (a big-city market or a 100+ battle) within 200u.

## Implications for co-op

- A large battle (>96 combatants within 200u of a leader) will SILENTLY drop the
  overflow on the join: some fighters simply won't be streamed/driven. For
  large-scale battle fidelity we'd need (a) a higher/prioritized cap, (b)
  nearest-first ordering, or (c) per-combatant interest LOD (spike 20).
- The 160 publish cap is shared with squad members; a large player squad shrinks
  the NPC budget. Worth making the cap explicit/configurable before battle work.

## Recommended follow-ups

- Add nearest-first sorting + a "must-include combatants" priority before the
  truncation, and raise MAX_PUBLISH behind a config gate.
- Re-test in a dense town (a save positioned in a market) to actually exercise the
  96/160 caps, which `c` could not.
