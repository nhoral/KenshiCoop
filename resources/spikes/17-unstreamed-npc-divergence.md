# Spike 17 - Un-streamed NPC behavior on the join

- Type: RUN (partial evidence from spikes 1/8) + code
- Status: PARTIAL
- Save: c

## Goal

What do the join's local NPCs do when the host is NOT streaming them - does the
join's own AI run a divergent local world, and how far does it drift?

## Evidence

- Host streams world NPCs (`setStreamNpcs(true)` on host, Plugin.cpp:420) within
  200u (spike 15); the join publishes only its own squad and runs
  `enforceHostAuthority` every tick (Plugin.cpp ~306) to suppress/restore world NPCs.
- Yet in spikes 1 and 8, at the SAME leader position, the host's `listNpcs` near
  count was ~3-7 while the join's was ~9-15 and stable - a persistent population
  divergence the suppression did NOT close.

## Findings (preliminary)

1. **The two clients have genuinely different local NPC populations** at the same
   place - Kenshi populates town residents/patrols per client, so the sets differ
   before any sync.
2. **`enforceHostAuthority` did not collapse the join's count to the host's
   streamed set** in these runs. Either it suppresses only NPCs that ALSO exist in
   the host's stream (leaving join-only locals running), or the extra bodies are a
   category it doesn't touch. This is the crux to pin down.
3. Consequence: a join player standing next to the host still sees ~2-4x as many
   wandering NPCs as the host does, each running the join's local AI independently
   (divergent paths, divergent combat outcomes if any fight).

## Why PARTIAL

The `listNpcs` COUNT shows divergence but not WHICH bodies diverge or whether
they're being suppressed-but-recounted. A dedicated probe is needed:

- Log each near NPC's hand on both clients; classify into (a) host-streamed +
  join-driven, (b) join-only-local (not in host stream), (c) host-only.
- Confirm whether `enforceHostAuthority` actually suppresses category (b) (it
  should make them ABSENT) or leaves them live.

## Implications for co-op

- Background-NPC divergence is mostly cosmetic UNTIL it isn't: a join-only NPC that
  attacks the join squad has no host-authoritative outcome, so a fight could resolve
  differently on each client. The suppression path must be verified to actually
  remove join-only locals, or combat near towns will desync.

## Recommended follow-ups

- `SpikeScenario` id 17 v2: per-hand census + classification on both clients (the
  probe above). Quantify category (b) size and whether suppression empties it.
- If suppression is incomplete, extend `enforceHostAuthority` to suppress ALL
  non-streamed world NPCs near the join leader (not just host-known ones).
