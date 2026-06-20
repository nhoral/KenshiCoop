# Spike 13 - Ragdoll / corpse-pile consistency

- Type: RUN
- Status: DONE
- Save: battle40 (baked in Spike 9)
- Branch commit: <filled at commit>

## Goal

When a battle ends in a heap of corpses, does the corpse pile look the same on both
clients - do dead bodies settle to the same positions, stay put, and read as "dead" -
or does each client's local ragdoll physics drift them apart? This decides whether a
post-battle scene (looting, screenshots, body-recovery) is consistent in co-op.

## Method

Loaded `battle40`. The HOST drives a brief real melee (t<7s, bodies move/scatter),
then at t=8s kills ALL nearby bodies at once (`engine::killBattle` -> knockDown +
`medical.dead=true` -> `Character::isDead()` -> BODY_DEAD). BOTH clients log every
body's hand / position / bodyState each 1s as `SPIKE 13 unit` lines. Offline
(`13/analyze.py`) we take each client's LAST sample per hand and, for the bodies the
host reports dead, compare (a) what bodyState the join shows for the same hand and
(b) the host-vs-join corpse position; we also measure post-kill intra-client drift.
Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 13 -Save battle40 -Seconds 30
python resources/spikes/13/analyze.py resources/spikes/13/raw/host.log resources/spikes/13/raw/join.log
```

bodyState bits (Wire.h:173-176): BODY_DOWN=1, BODY_RAGDOLL=2, BODY_DEAD=4. Probe
helpers (`driveBattleMelee`, `killBattle`) + scenario case 13 reverted; raw + analyzer
kept in `13/`.

## Raw evidence

```
SPIKE 13 kill t=8015 count=46
```

Analyzer (`13/analyze.py`):

```
host hands=63 join hands=60 common=60
host-dead common bodies (last sample): 43
  join shows for those same hands: DEAD=0 RAGDOLL=43 DOWN=0 ALIVE/other=0
  corpse pos divergence (host-dead, last sample): min=1.2 med=4.3 mean=5.2 max=23.3  (n=43)
host corpse drift after t=12s: med=0.0 max=0.5
join corpse drift after t=12s: med=0.0 max=0.0
```

Same body, both clients, last samples (host=BODY_DOWN+RAGDOLL+DEAD=7, join=DOWN+RAGDOLL=3):

```
HOST t=29015 hand=1,3677246208 pos=-51204.8,1551.7,2881.4 body=7
JOIN t=29016 hand=1,3677246208 pos=-51202.5,1551.4,2884.6 body=3   (dist ~3.9u)
```

## Findings

1. **Corpse positions are highly consistent across clients - far tighter than live
   combatants.** Over 43 host-dead bodies the host-vs-join rest-position divergence
   was median 4.3u / mean 5.2u / max 23.3u, ~4x tighter than the ~18-20u median for
   moving combatants in Spike 9.
2. **Corpses are stationary on both clients after death; there is no independent
   ragdoll drift.** Post-kill intra-client displacement was ~0u (host med 0.0/max
   0.5u; join med 0.0/max 0.0u) - the join holds the last streamed position rather
   than running its own ragdoll sim that wanders off.
3. **The convergence is real, not a clock-skew artifact.** Because the bodies stop
   moving, the position comparison has no lag/time-alignment confound (unlike Spike
   9's moving targets), so the 4.3u median is a genuine residual rest-position
   disagreement, not interpolation lag.
4. **The corpse POSE replicates but the DEAD flag does not.** Every host-dead body
   (43/43) showed on the join as BODY_DOWN+BODY_RAGDOLL (body=3) and NEVER BODY_DEAD
   (body=4); the host showed body=7 (DOWN+RAGDOLL+DEAD). Bodies visibly collapse into
   a pile on the join, but the join's local `isDead()` stays false.

## Validation

- Finding 1: `13/analyze.py` line `corpse pos divergence ... med=4.3 mean=5.2
  max=23.3 (n=43)`, computed from the last `SPIKE 13 unit` sample per common hand in
  `13/raw/{host,join}.log`; compared against Spike 9's documented ~18-20u live median.
- Finding 2: analyzer lines `host corpse drift after t=12s: med=0.0 max=0.5` and
  `join ... med=0.0 max=0.0`, the max displacement of each dead body across all
  post-kill (t>=12s) samples on each client.
- Finding 3: follows from Finding 2 - zero post-kill motion means the targets are
  static, so the position diff has no time-alignment dependence (the Spike 9 caveat
  does not apply here); stated as the reason the residual is a true disagreement.
- Finding 4: analyzer `join shows for those same hands: DEAD=0 RAGDOLL=43`, plus the
  paired sample above (host `body=7` vs join `body=3` for the same hand) - the join
  carries DOWN+RAGDOLL but the DEAD bit (4) is absent. Consistent with the medical/
  dead-flag non-replication found in Spikes 22/25/26.

## Open questions / hypotheses (UNVALIDATED)

- **Death ANIMATION / ragdoll tumble path is not compared** - we only measured the
  settled rest position, not whether the fall looked the same frame-to-frame. The
  ~4u residual likely comes from each client freezing at a slightly different point
  of the fall; a per-frame capture would confirm.
- **Whether EVT_DEATH should latch BODY_DEAD on the join** is a design choice, not
  measured here. Today the join shows a ragdoll pile that never reads as "dead"
  locally - fine visually, but looting/permadeath logic that checks `isDead()` on the
  join would be wrong.
- **Very large piles (>96 bodies) and overlap/stacking** (do corpses interpenetrate
  identically?) untested - capped by the 96-entry interest query here.

## Implications for co-op

- A post-battle corpse pile is visually consistent for free: positions agree to a few
  units and bodies stay put, so screenshots / "walk through the aftermath" work
  without extra sync. Dead bodies are actually the BEST-synced case (static target).
- Any gameplay that asks "is this body dead?" on the join must NOT rely on the local
  bodyState DEAD bit - it never gets set. Use the latched EVT_DEATH (Spike 10/27) or
  add explicit dead-flag application on the join.
- Looting a corpse on the join needs the host-authoritative inventory path; the join's
  body isn't truly "dead" locally even though it lies in the pile.

## Recommended follow-ups

- Apply BODY_DEAD (and/or set the join's `medical.dead`) when EVT_DEATH latches, so
  `isDead()` is correct on the join for looting/permadeath; re-run this spike to
  confirm join shows body=7.
- Per-frame ragdoll-fall capture to characterize the death animation divergence (vs
  just rest position).
- Keeper primitives (reverted): `engine::killBattle` (instant corpse pile) and
  `engine::driveBattleMelee` - reusable for aftermath / looting / large-pile spikes.
