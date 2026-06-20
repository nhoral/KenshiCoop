# Spike 7 - Env-parameterized scenarios (workflow)

- Type: WORKFLOW (implemented)
- Status: DONE

## Goal

Make one scenario cover many test cases via runtime parameters, so we don't compile
a new `Scenario` class per variation.

## What was built

The spike harness itself is the answer:

- `SpikeScenario` ([Scenario.cpp](../../src/plugin/test/Scenario.cpp)) dispatches on
  the `KENSHICOOP_SPIKE` env var at runtime - one compiled class, many probes.
- `scripts/run_spike.ps1` sets `KENSHICOOP_SPIKE` (which `run_test.ps1`'s
  `Set-CoopEnv` deliberately does NOT overwrite, so it inherits into the launched
  game), plus passes `-Save`, `-Seconds`, `-Setup`, and the `-NetSim*` WAN knobs.
- `scripts/run_spikes.ps1` runs a LIST of ids off a single build (skip-on-error).

## Findings

1. **Env-var runtime selection works cleanly** - confirmed end to end (spikes
   0/1/8 all selected by `KENSHICOOP_SPIKE` off one build).
2. The existing harness already exposes rich per-run parameters via `run_test.ps1`:
   `-Save`, `-Seconds`, `-Setup`, `-NetSimDelayMs/-JitterMs/-LossPct`, `-Tolerance`.
   Adding a probe parameter is just reading another env var in the probe.
3. **One build serves every compiled probe** - `run_spikes.ps1 -Ids 1,8,14`
   amortizes the (~13 s incremental) build across all of them. This is the key
   workflow win: batch probe code, build once, sweep many ids.

## Implications for co-op

- New investigations should be added as `SpikeScenario` id branches, not new
  scenario classes - far less boilerplate and no `makeScenario`/run_test plumbing.
- For numeric sweeps (e.g. battle size N, separation distance D), read a second env
  var (e.g. `KENSHICOOP_SPIKE_ARG`) in the probe and pass `-Seconds`/a new
  `run_spike.ps1` param. Pattern is proven; just add the knob when needed.

## Recommended follow-ups

- Add a generic `KENSHICOOP_SPIKE_ARG` pass-through to `run_spike.ps1` for numeric
  sweeps so distance/scale probes don't need rebuilds between values.
