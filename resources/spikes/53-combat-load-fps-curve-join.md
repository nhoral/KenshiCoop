# Spike 53 - Combat-load FPS curve (join) while the host fights

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Measure the JOIN's frame rate while the HOST runs a large battle, to learn whether a
host-side fight costs the join anything. Tests spike 51's prediction directly: runtime-
spawned NPCs carry host-only hands and do not replicate, so the join should be load-
DECOUPLED from the host's battle.

## Method

- Re-added the (reverted) spike-51 battle helpers.
- New `SpikeScenario` id `53` (NETWORKED): the HOST spawns a `perSide`-sized battle and
  sustains it (`spawnHostileBattle` + per-second `rearmBattle`, logging an alive/fighting
  census); the JOIN samples its OWN windowed FPS (`ctx.tick` delta) each second plus how
  many non-squad combatants it actually SEES (`engine::captureNpcs`). Host window 50s
  (outlives join), join 40s.
- One networked run on save `c`, `SpikeArg 20` (20v20 = 40 bodies). Logs in
  `53/raw/{host,join}.log`.

## Findings

1. **The host fought a real 40-body battle.** Host census held `alive=40 fighting=38`
   for the whole window (`jfps HOST ... alive=40 fighting=38` through t=49250).
2. **The join was completely unaffected: avg 80.0 fps, indistinguishable from a
   light/idle baseline.** Join summary `samples=34 avgFps=80.0 minFps=13.7`. That 80 fps
   equals the host's own *5v5* average from spike 52 (80.0) - i.e. the join ran as if no
   battle existed. Its per-second band is high and noisy (42-142 fps) with NO sustained
   engaged-combat drop, in clear contrast to the host's own 20v20 steady band of ~60 fps
   (spike 52). The single low samples (min 13.7) are the same transient hitch seen at
   every scale, not load.
3. **The join never SAW the battle.** Join `seen` held steady at **8** and only crept to
   **10** late in the run - the shared-save baseline, not the host's 40 spawned bodies.
   The host's runtime spawns do not stream to the join (host-only hands, spike 51), so
   the join has nothing extra to simulate or render -> no cost.
4. **Therefore join load is DECOUPLED from a host's runtime-spawned battle.** This is a
   property of the runtime-spawn path specifically: in a BAKED-save battle (both clients
   load the same combatants) the join WOULD render/simulate them and its FPS would drop
   like the host's - that complementary case is NOT covered here.

## Validation

- Finding 1: `53/raw/host.log` - `jfps HOST battle spawned perSide=20 total=40` and
  repeated `jfps HOST ... alive=40 fighting=38`.
- Finding 2: `53/raw/join.log` - `jfps-summary JOIN samples=34 avgFps=80.0 minFps=13.7`;
  the BAND samples (42.6/89.5/67.9/.../81.5) show no sustained drop. Cross-referenced
  against spike 52's host 20v20 steady ~60 fps (same hardware, same build) to show the
  join sits ABOVE the host's engaged band.
- Finding 3: every join BAND line carries `seen=8` (later `seen=9`,`seen=10`); peakSeen=10
  vs the host's 40 - logged side by side.
- Finding 4: inference from 1-3 + spike 51's validated non-replication; the baked-save
  complement is explicitly flagged unproven.

## Open questions / hypotheses (UNVALIDATED)

- **Baked-save join FPS** (both clients load the battle, so the join DOES render/simulate
  it) - the case where join load is expected to match the host - is untested (spike 82/88).
- **Network-thread cost on the join** of processing the host's NPC stream (even for NPCs
  it can't resolve) was not isolated; FPS impact was nil but packet/CPU cost unmeasured.
- **Lethal battle** (real damage/ragdoll) on the host - whether any death/event traffic
  reaches and costs the join - not exercised.
- **Probe overhead**: the join's per-second `captureNpcs` adds a small sweep (like spike
  52); a probe-free baseline would be cleaner.

## Implications for co-op

- A host fighting a runtime-spawned battle imposes ~zero frame cost on the join today -
  but only because the join never sees that battle (a correctness gap, not a feature).
- For a SHARED battle both players experience, bake the scene; then expect the join to pay
  a combat-FPS cost similar to spike 52's host curve. Budget both clients accordingly.
- FPS is a poor liveness signal on the join during host combat - it stays high regardless;
  use the replicated-entity count, not frame rate, to detect "is the join seeing the fight".

## Recommended follow-ups

- Baked 20v20 save -> measure join FPS when it actually simulates the battle (spike 82/88).
- Instrument net-thread/packet cost on the join under a host battle (theme bandwidth: 58).
- Keeper primitives (reverted): `engine::spawnHostileBattle`/`rearmBattle`/`battleCensus`,
  `SpikeScenario` id 53 join-FPS probe.
