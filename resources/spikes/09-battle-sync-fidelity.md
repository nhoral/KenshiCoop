# Spike 9 - Battle sync fidelity vs combatant count

- Type: RUN
- Status: PARTIAL
- Save: c -> baked battle10 / battle20 / battle40
- Branch commit: <filled at commit>

## Goal

When a real melee runs on the host, how faithfully does the join reflect it, and
does fidelity degrade as the number of combatants grows? This sizes the headroom
for the co-op battle experience and tells us where the first failure mode is.

## Method

Runtime spawns get host-local hands the join can't resolve (spike 1), so a battle
must be BAKED into a save both clients load. Two passes, driven by a new
`KENSHICOOP_SPIKE_ARG`:

- bake (host-only): `spawnBattle(perSide)` spawns `2*perSide` peaceful non-squad
  NPCs (nearby faction) in two facing rows in front of the leader, each detached for
  a save-stable hand, then `saveGame("battle<2*perSide>")`. Run at perSide 5/10/20.
- measure (both clients load the baked save): the HOST pairs the baked bodies into a
  melee (`driveBattleMelee`, idempotent re-arm) while BOTH clients log every baked
  combatant each ~1.5s: `hand`, world `pos`, `bodyState`, `inCombat`.

Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 9 -Save c       -SkipBuild -SpikeArg bake5  -Seconds 30   # bake battle10
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 9 -Save battle10 -SkipBuild            -Seconds 45   # measure
# repeat bake10->battle20, bake20->battle40, then measure each
python resources/spikes/9/analyze.py resources/spikes/9/raw/b40/host.log resources/spikes/9/raw/b40/join.log
```

Raw logs preserved per size under `9/raw/b10`, `9/raw/b20`, `9/raw/b40`.

## Raw evidence

Bake (host): `SPIKE 9 bake perSide=5 spawned=10` / `bake save=battle10 ok=1` (also
perSide=10->battle20 spawned=20, perSide=20->battle40 spawned=40). All three saves
exist on disk (`%LOCALAPPDATA%\kenshi\save\battle{10,20,40}`).

`analyze.py` over the matched (host,join) unit samples:

```
battle10 (5v5):   host hands=21 join hands=22 common=16
                  t-aligned (|dt|<=2s) divergence: med=18.5 mean=24.6 max=231.7 (260 pairs)
                  last-sample body agree 14/16  combat agree 13/16
battle20 (10v10): host hands=.. join hands=.. common=27
                  t-aligned divergence: med=20.3 mean=33.5 max=258.8 (429 pairs)
                  body agree 27/27  combat agree 23/27
battle40 (20v20): common=47
                  t-aligned divergence: med=19.5 mean=22.0 max=92.8 (852 pairs)
                  body agree 47/47  combat agree 43/47
```

Representative matched bodies (battle40, final sample t=28516 both sides):

```
hand=1,598754880  Hpos=(-51234,2898) Jpos=(-51237,2893) dist= 5.7 body H0/J0 combat H1/J1
hand=3,3090698240 Hpos=(-51179,2661) Jpos=(-51208,2649) dist=32.1 body H0/J0 combat H0/J0 (Ht=1516 Jt=28516)
battle10 hand=1,753417984  body H0/J3 combat H1/J0   # host upright+fighting, join down+idle
```

## Findings

1. **A baked battle replicates as host-authoritative WORLD NPCs, and every baked
   combatant resolves cross-client by its save-stable hand.** Matched ("common")
   bodies scale with the baked count: 16 (battle10) -> 27 (battle20) -> 47
   (battle40). (Counts exceed the spawned 10/20/40 because the `c` town's own NPCs
   are also in the interest sphere and likewise resolve.)

2. **Aggregate positional divergence does NOT grow with combatant count over the
   10->40 range.** The per-sample median divergence is essentially flat: 18.5u /
   20.3u / 19.5u. Mean is noisier (24.6 / 33.5 / 22.0), moved by a few outliers, not
   by count. At these scales (<= 40 combatants, under the 96/96 interest and
   MAX_PUBLISH=160 caps measured in spike 14), count is not the bottleneck;
   per-body tracking is.

3. **Body-state (down/up) agreement is high and stays high with scale: 14/16, 27/27,
   47/47.** Combat-flag agreement is lower (13/16, 23/27, 43/47 ~= 81-95%): a
   minority of bodies are `inCombat` on the host but idle on the join (e.g. battle10
   hand 1,753417984: host body=0 combat=1, join body=3 combat=0).

4. **The dominant divergence mechanism is interest-scatter, not load.** In every run
   a few combatants flee/scatter beyond the host leader's 200u interest sphere; the
   host stops reporting them (their last host sample is at an early `t`, e.g.
   battle40 hand 3,3090698240 at Ht=1516) while the join keeps simulating its local
   copy to the end (Jt=28516). These produce the large max-distance outliers and are
   the spike-17 un-streamed-NPC divergence surfacing under combat-induced scatter.

## Validation

- Finding 1: `analyze.py` `common=` counts (16/27/47) come from intersecting
  the host and join `SPIKE 9 unit ... hand=I,S` sets per run; the per-hand table
  prints both clients' positions for the SAME hand (e.g. battle40
  `hand=1,598754880 Hpos=(-51234,2898) Jpos=(-51237,2893)`), proving the same
  save-stable hand resolves on both. Bake counts validated by host lines
  `bake ... spawned=10/20/40` and the three save folders existing on disk.
- Finding 2: the three `t-aligned ... med=` values (18.5/20.3/19.5) are computed
  over 260/429/852 matched-by-hand, nearest-elapsed-`t` (|dt|<=2s) sample pairs from
  the committed `b10/b20/b40` logs; re-run `analyze.py` to reproduce.
- Finding 3: the agreement ratios are direct counts of last-sample `body`/`combat`
  equality over common hands; the quoted mismatching line
  (`body H0/J3 combat H1/J0`) is a literal `SPIKE 9 unit` pair from `b10`.
- Finding 4: validated by host-vs-join last-sample timestamps in the per-hand table:
  outlier bodies have `Ht` in the first few seconds while `Jt=28516`, i.e. the host
  stopped enumerating them (left its 200u `listNpcs` sphere) while the join did not.

## Open questions / hypotheses (UNVALIDATED)

- **Absolute divergence magnitude (~18-20u) is an UPPER BOUND, not a clean
  measurement.** The join enters gameplay ~8s after the host, and both log elapsed
  `t` from their OWN gameplay start, so "same `t`" samples are offset by load-skew in
  wall-clock; during motion that skew alone inflates the measured distance. A clean
  per-body replication-lag number needs a shared wall-clock anchor (e.g. stamp host
  send-time into the stream and compare on receipt). Until then, do NOT claim a
  specific steady-state tracking error.
- **Whether divergence grows beyond 40 combatants (approaching/over the 96/160
  caps)** is untested here; battle40 stays under the caps. Needs a battle96+ bake.
- **Why a minority of bodies disagree on the combat flag** (host fighting, join idle)
  is unconfirmed: likely the combat TASK/target is not in the streamed EntityState,
  so the join renders position but not combat intent. Needs a probe that logs
  `rawTask`/attack-target on both sides for the mismatching hands.

## Implications for co-op

- Battles are viable to *watch* on the join up to at least ~40 combatants without a
  count-driven collapse: identity and down/up state are solid and scale cleanly.
- The first real fidelity problem is **interest-scatter**, not raw count: fleeing or
  pushed combatants leave the host's single 200u sphere and the join's local AI takes
  over, diverging. This is concrete motivation for the dual-interest (spike 19) and
  hysteresis (spike 18) work, and for forcing active combatants to a high-priority
  tier (spike 20) so they are never dropped.
- Combat *intent* (the attack/target) appears not to ride the wire; the join shows
  bodies in roughly the right place but not reliably "fighting". A `combat`-bearing
  delta (target hand + task) is likely needed for a convincing battle.

## Recommended follow-ups

- Keeper primitives (reverted from the tree with the rest of the probe; re-add when
  building the battle feature): `spawnBattle(perSide)`, `driveBattleMelee()`,
  `saveGame(name)` in `Engine`, and the `KENSHICOOP_SPIKE_ARG` bake/measure split.
  The baked `battle10/20/40` saves are a reusable battle-sync test library.
- Add a shared wall-clock anchor to the measure probe to convert the upper-bound
  divergence into a true replication-lag curve (resolves open question 1).
- Bake `battle96`+ to test fidelity at/over the interest & publish caps.
- Probe the combat-flag mismatch: log `readTaskKey`/attack-target for mismatching
  hands on both clients (feeds the medical/combat-intent delta work, spikes 51-80).
