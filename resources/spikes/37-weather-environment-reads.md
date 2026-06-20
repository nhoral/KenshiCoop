# Spike 37 - Weather / environment reads & control

- Type: DUMP
- Status: PARTIAL
- Save: c
- Branch commit: <filled at commit>

## Goal

Can the mod read the local environment (wind, day/night light, active weather) and is
that state deterministic/consistent across clients? This informs whether weather needs
to enter the wire protocol (gameplay-affecting, must sync) or is a safe local read
(UI/ambience only). Secondary: locate a control entry point for later weather spikes.

## Method

Static: the named reads live on `GameWorld` - `getWindSpeed(const Vector3&)` and
`getLightLevel(const Vector3&, int floor, bool inside)`, both returning a float. The
weather actually *affecting* a character is per-character medical state:
`MedicalSystem.currentWeatherAffect` (a `WeatherAffecting` enum: `WA_NONE=0`,
`WA_DUSTSTORM`, `WA_ACID`, `WA_BURNING`, `WA_GAS`, `WA_RAIN`) plus
`currentWeatherAffectStrength` (float). A candidate control entry,
`PhysicsCollection::addGlobalEffect`, was located but NOT exercised.

Runtime: resolved `getWindSpeed`/`getLightLevel` by RVA and added `engine::readEnv`
(probe id 37). BOTH clients read wind / world-light / weather-affecting at their leader
every 1.5s and log it. Offline we compare host vs join at matched wall-time. Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 37 -Save c -Seconds 45
```

Probe code reverted; raw logs in `37/raw/`.

## Raw evidence

Both clients, every sample for the whole run (`37/raw/host.log`, `37/raw/join.log`):

```
host t=22516 wind=12.5687 light=1.0000 wa=0 wastr=0.000   (every host sample, identical)
join t=1515  wind=12.5687 light=1.0000 wa=0 wastr=0.000
join t=3015  wind=12.5687 light=1.0000 wa=0 wastr=0.000
...                                                        (19 join samples, all identical)
join t=28515 wind=12.5687 light=1.0000 wa=0 wastr=0.000
```

Both leaders resolve to the same character on save 'c' (same hand/pos as the smoke
anchor), so each client reads the same world position; every wind/light value matches
to 4 decimal places across both processes and the entire 30s window.

## Findings

1. **Wind speed is readable and stable.** `GameWorld::getWindSpeed(pos)` returned a
   plausible, constant `12.5687` for the leader's position throughout the run on both
   clients. The call resolves and executes without fault.
2. **World light level is readable.** `GameWorld::getLightLevel(pos,0,false)` returned
   `1.0000` (full daylight) for the whole window - consistent with a daytime save and
   no day/night transition occurring in 30s.
3. **Per-character weather affect is readable.** `MedicalSystem.currentWeatherAffect`
   read `0` (`WA_NONE`) and strength `0.0` for the leader - this save's region had no
   active hazard weather, which is the expected baseline (not a null read: the field is
   reached via the live character's medical block).
4. **Environment reads are consistent across clients for the same position.** Every
   wind/light value was bit-identical between host and join. Because both clients
   resolved the *same* leader at the *same* position, this confirms the reads are pure
   functions of (world position, world state) and not per-client random noise.

## Validation

- Findings 1-3: `37/raw/host.log` + `37/raw/join.log` (quoted above). Each value is a
  non-sentinel result - the probe initialises out-params to a distinct `-1.0` on
  resolve failure (see `engine::readEnv`); we got real numbers (12.5687 / 1.0 / 0),
  proving the RVAs resolved and the calls ran inside the SEH guard without faulting.
- Finding 4: side-by-side compare of the two log files - 20 host + 19 join samples, all
  `wind=12.5687 light=1.0000`. Zero divergence. Both processes produced their own
  independent `SPIKE 37 env role=...` lines (role=H vs role=J), so this is two real
  reads agreeing, not one process logged twice.

## Open questions / hypotheses (UNVALIDATED)

- **Non-zero weather was never observed.** Save 'c' / this region produced `WA_NONE`
  the whole run, so I could not validate that `currentWeatherAffect` reports
  `WA_DUSTSTORM`/`WA_RAIN`/etc. or that strength tracks intensity. Needs a probe that
  spawns the leader into an acid-rain / dust-storm region (or fast-forwards time) and
  re-reads.
- **Whether wind/light change with game time or weather is unconfirmed** - both were
  constant because no transition happened in 30s. Test: drive time forward (spike 34
  `setGameSpeedProbe`) across a dusk boundary and watch `light`.
- **Weather is assumed local/deterministic but NOT proven to be in-sync under real
  divergence.** Both clients read identical values only because they shared one
  position and (likely) near-identical world clocks. If per-client game clocks drift
  (spike 34 showed speed/pause is local), light level *could* diverge at a day/night
  boundary. Untested.
- **Control was not exercised.** `PhysicsCollection::addGlobalEffect` is a *candidate*
  entry for injecting a global effect; no call was made, so weather *control* (forcing
  rain, clearing a storm) remains entirely unvalidated. Marked PARTIAL for this reason.

## Implications for co-op

- Wind/light/weather reads are cheap, fault-free local calls usable for ambience and
  UI (e.g. a weather widget, light-based stealth hints) without touching the protocol.
- Whether weather must be synced is **still open**: it is read-consistent for a shared
  position, but day/night light is a function of the (local, unsynced) game clock, so a
  hazard-weather or dusk spike is needed before declaring it safe to leave out of sync.

## Recommended follow-ups

- Hazard-region probe: teleport leader into a known acid-rain/dust-storm zone, re-read
  `currentWeatherAffect`/strength on both clients, and check agreement.
- Day/night probe: combine with spike 34 time control to cross a dusk boundary and
  measure `getLightLevel` drift between clients with divergent clocks.
- Control probe: attempt `PhysicsCollection::addGlobalEffect` (carefully, SEH-guarded)
  to see if weather/effects can be injected, before relying on it for any feature.
- Keeper primitive (reverted): `engine::readEnv(gw, c, &wind, &light, &wa, &wstr)`.
