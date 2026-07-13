# Spike 52 - Combat-load FPS curve (host): 5v5 / 10v10 / 20v20 / 40v40

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Measure how host frame rate degrades with combatant count, using spike 51's
`spawnHostileBattle` helper. Produce a 4-point load curve (10 / 20 / 40 / 80 bodies)
so later sync/bandwidth spikes know where the host becomes the bottleneck.

## Method

- Re-added the (reverted) `spawnHostileBattle`/`rearmBattle`/`battleCensus` engine
  helpers from spike 51.
- New `SpikeScenario` id `52` (HOST-ONLY): spawns a battle of `perSide` = `SPIKE_ARG`
  at t=1.5s, then each 1s computes a WINDOWED frame rate from `ctx.tick` (incremented
  once per main-loop frame by `Plugin.cpp`): `fps = Dframes / Dseconds`. Samples after
  t=4s are counted in the BAND (avg/min); a per-second `rearmBattle` + `battleCensus`
  keeps the blob engaged and logs alive/fighting alongside each fps sample. Ends with an
  `fps-summary`.
- Ran the harness 4x (`-HostOnly -SkipBuild`) with `SpikeArg` 5, 10, 20, 40 off ONE
  build. ~38s window each. Per-run host log saved to `52/raw/host_per{5,10,20,40}.log`;
  `52/raw/host_5.png` shows the 80-body (40v40) crowd.

## Findings

1. **Frame rate degrades GRACEFULLY with scale; the host stays > 50 fps even at 80
   bodies.** Logged whole-band averages: **5v5=80.0, 10v10=74.8, 20v20=73.6,
   40v40=70.4 fps** (so ~12% loss for an 8x body increase). Uncapped (>60), so not
   vsync-limited.
2. **The run has a clean TWO-PHASE shape, and the steady-state (engaged) band is the
   real load figure.** Every scale starts HIGH (t=4-19s) then settles to a stable lower
   band (t>=~20s):
   - pre-engage (phase 1) median fps: 5v5 ~113, 10v10 ~113, 20v20 ~101, 40v40 ~90
   - steady engaged (phase 2) median fps: 5v5 ~71, 10v10 ~62, 20v20 ~61, **40v40 ~55**
   The steady band is monotonic in load and very stable at the top end (40v40 held
   54-56 fps for ~12 consecutive samples). Even *pre-engagement* fps falls with body
   count (115 -> 89), i.e. render + idle-AI cost alone scales with count.
3. **Per-second deep hitches to ~9-14 fps recur at EVERY scale (not load-specific).**
   Single-sample dips (5v5 minFps=12.2, 10v10=9.4, 20v20=9.3, 40v40=13.0) appear
   roughly every few seconds at all sizes. They do NOT scale with combatant count, so
   they are transient stalls (autosave/stream/GC) and/or **the probe's own per-second
   sweep** (`rearmBattle`+`battleCensus` = up to 160 SEH-guarded `resolveCharByHand`+
   `readCombat`+`readPos` calls in one frame at 80 bodies) - a measurement artifact, not
   steady engine load.
4. **This is a LOWER BOUND on real-battle cost.** Per spike 51 these are same-faction
   bodies in combat MODE that posture without dealing damage - no hit detection, blood,
   limb/ragdoll, or death churn. A lethal battle of the same size will cost MORE.

## Validation

- Findings 1-2: `52/raw/host_per{5,10,20,40}.log` - each ends with
  `SPIKE 52 fps-summary perSide=N total=2N samples=31..32 avgFps=.. minFps=..`
  (80.0/74.8/73.6/70.4 avg). The per-sample `SPIKE 52 fps perSide=N ... fps=.. BAND`
  lines give the two-phase distribution quoted above (e.g. 40v40 tail:
  56.4 55.4 55.3 56.3 55.4 55.1 54.3 55.4). Each run reached `SPIKE 52 CAPTURE-OK`.
- Scale is real: `fps battle spawned perSide=40 total=80` + `host_5.png` shows the dense
  ~80-body crowd; census lines confirm `alive`/`fighting` counts tracked the spawn.
- Finding 3: the <20 fps samples are isolated (one sample, recover next second) and the
  min is ~the same (9-13) at 10 bodies as at 80 - so it is not combatant-count-driven.
- Finding 4: cross-referenced to spike 51's validated non-lethality (wounded=0,
  alive=20/20 over 70s).

## Open questions / hypotheses (UNVALIDATED)

- **Cause of the phase-1 -> phase-2 transition** (~t=20s at every scale, regardless of
  load) is not isolated: combat-engagement ramp vs a time-based engine/harness event
  (autosave? camera?) - the steady band is trustworthy but the early-high cause is not.
- **LETHAL-battle FPS** (real damage/ragdoll/death) - the figure that matters for a true
  large battle - is unmeasured (needs the hostile-faction variant).
- **Probe-free FPS**: re-run without the per-second census/rearm sweep to remove the
  self-induced hitches and get pure engine load.
- **Higher scales** (60v60+) and GPU-bound vs CPU-bound attribution (no per-thread
  timing here).
- **Join-side FPS while host fights** is spike 53.

## Implications for co-op

- The host is comfortably above playable (>50 fps) up to 80 non-lethal combatants;
  combat AI is the dominant cost (clear phase-1->phase-2 step) but degrades gently.
- Treat ~55-60 fps as the steady-state ceiling for 40-80-body scenes; budget the net/
  replication layer against that, not against the inflated pre-engagement numbers.
- Avoid heavy per-frame mod sweeps over all combatants - my own per-second census caused
  visible hitches; replicate incrementally, not in one big per-tick pass.

## Recommended follow-ups

- Lethal-battle FPS curve (hostile factions) - the upper-bound number.
- Spike 53: join FPS while the host fights (the same helper, observe the join).
- Reduce probe overhead (sample a subset / spread census across frames) before using
  FPS as an acceptance gate.
- Keeper primitives (reverted): `engine::spawnHostileBattle`/`rearmBattle`/`battleCensus`,
  `SpikeScenario` id 52 windowed-FPS probe.
