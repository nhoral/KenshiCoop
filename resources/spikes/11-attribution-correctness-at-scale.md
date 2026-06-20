# Spike 11 - Attribution correctness at scale (who downed whom)

- Type: RUN
- Status: PARTIAL
- Save: battle40 (baked in Spike 9)
- Branch commit: <filled at commit>

## Goal

KO/death events carry an `actor` (who downed the victim), stamped from the
replicator's `attackerOf_` recency map (Replicator.cpp:160-222). When many fights
overlap, does that map attribute each death to the body that was ACTUALLY attacking
the victim, or does it cross-contaminate (stamp a nearby unrelated attacker)?
Correct attribution is the foundation for any kill-feed, XP/credit, or aggro logic.

## Method

Loaded the 40-body baked battle (`battle40`). The HOST drives a real melee every
1.5s (`engine::driveBattleMelee`, pairs 0v1,2v3,... and orders each idle body to
focus-melee its partner -> real combat, so KO/death carries a true attacker). At
t=6s the host runs a one-time `engine::weakenBattle(gw, 12)` to push bodies toward
dropping under the ongoing melee. Independently, BOTH clients log every combatant's
CURRENT attack target each 750ms as ground truth:

```
SPIKE 11 ct role=H t=.. hand=<idx,ser> tgt=<idx,ser> inc=<0|1>
```

Offline (`11/analyze.py`), for each `[event] SEND` (victim `hand`, `actor`) we check
the stamped `actor` was observed targeting that victim in the ground-truth log, and
report mis-attributions (actor never seen targeting victim). Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 11 -Save battle40 -Seconds 60
python resources/spikes/11/analyze.py resources/spikes/11/raw/host.log
```

Probe helpers (`driveBattleMelee`, `weakenBattle`) and scenario case 11 were reverted
after capture; raw logs + analyzer kept in `11/`.

## Raw evidence

Host drove a busy, overlapping melee, then weakened 49 bodies:

```
SPIKE 11 drive t=1515 orders=46
SPIKE 11 weaken t=6015 blood<=12 count=49
```

The 4 KO edges the replicator emitted, each carrying a stamped actor:

```
[event] SEND id=1 ev=1 hand=1,6,1483154304,1,1723144576 actor=1,3996215808 bs 0->1
[event] SEND id=2 ev=1 hand=1,3,1144164992,1,3823120128 actor=3,3090698240 bs 0->1
[event] SEND id=3 ev=1 hand=1,41,1035400384,1,3676879872 actor=1,3808471552 bs 0->1
[event] SEND id=4 ev=1 hand=1,51,289184576,1,3677246208 actor=2,2703369216 bs 0->1
```

Analyzer over the ground-truth target log:

```
peak concurrent attacker->victim pairs at one host tick: 47
observed attacker->victim pairs: 80   victims targeted: 64
  ev=1(KO): total=4 attributed=4 actor-seen-targeting-victim=4 unattributed(actor=0)=0
MIS-ATTRIBUTIONS (stamped actor NEVER seen targeting victim): 0
```

## Findings

1. **Attribution was correct for every emitted combat KO under heavy overlap.** All
   4 KO events carried a non-zero actor, and in every case that actor was
   independently observed targeting exactly that victim - 0 mis-attributions.
2. **The combat field was genuinely "at scale" / overlapping, not a tidy set of
   isolated duels.** At peak, 47 distinct attacker->victim relationships were live in
   a single host tick, with 64 distinct victims targeted across the run, yet the
   recency map still resolved each death to its own attacker (no cross-contamination
   despite many simultaneous fights sharing the field).
3. **Combat KOs are reliably attributed; non-combat downs are correctly left
   unattributed.** Every KO here came from real melee and got an actor; the code
   path leaves `actor=0` for non-combat causes, and we saw 0 spurious actor=0 among
   combat KOs.

## Validation

- Finding 1: `11/raw/host.log` `[event] SEND id=1..4` (quoted above) each have a
  non-zero `actor`. `11/analyze.py` cross-references each `(actor,victim)` against the
  set of `(attacker,victim)` pairs built from the independent `SPIKE 11 ct ... hand=
  tgt=` ground-truth lines (a DIFFERENT data source than `attackerOf_` - it reads each
  body's live `getAttackTarget` via `engine::readCombat`). Result line:
  `attributed=4 actor-seen-targeting-victim=4 ... MIS-ATTRIBUTIONS: 0`.
- Finding 2: analyzer `peak concurrent attacker->victim pairs at one host tick: 47`
  and `observed attacker->victim pairs: 80 victims targeted: 64`, computed from the
  per-host-tick `SPIKE 11 ct ... inc=1` lines in `11/raw/host.log`. The correctness
  in Finding 1 was measured against this same busy field.
- Finding 3: code path Replicator.cpp:205-214 stamps the actor only when `attackerOf_`
  has the victim within `ATTR_WINDOW_MS` (=3000, line 60), else leaves it zeroed; the
  analyzer's `unattributed(actor=0)=0` confirms no combat KO fell through unattributed
  in this run.

## Open questions / hypotheses (UNVALIDATED)

- **Behaviour under a true simultaneous die-off (tens of deaths in one tick) is not
  yet measured.** Despite 47 concurrent fights, only 4 KO edges resolved in 60s -
  lowering `blood` to 12 was not reliably lethal (bodies kept fighting), so the death
  sample (N=4) is small. The correctness rate is 100% but on few events. To validate
  at death-storm scale we need a lethal primitive (e.g. an instant melee-attributed
  finishing hit, or `weakenBattle` to blood<=1) that drops many bodies in one window.
- **Mis-attribution when a victim is swarmed by multiple attackers** (3+ bodies all
  targeting one victim) is untested here - pairing kept it mostly 1v1. `attackerOf_`
  keeps only the LAST attacker seen (map overwrite, Replicator.cpp:172), so a swarm
  death would credit whichever attacker published most recently; whether that matches
  the "real" killer is undefined and worth a dedicated swarm probe.
- **Window expiry edge** (ATTR_WINDOW_MS=3000): a victim that disengages, survives >3s,
  then dies of bleed-out should emit `actor=0`. Not exercised here.

## Implications for co-op

- A kill-feed / "downed by" attribution feature is viable on the existing event
  stream for ordinary overlapping combat: the actor field is trustworthy for combat
  KOs and is correctly blank for non-combat downs. No extra ground-truth channel is
  needed for the common case.
- The recency-map design (last-attacker-wins within a 3s window) is the known limit:
  swarm and long-bleed deaths are the cases to harden before relying on attribution
  for anything scoring-sensitive (XP, bounty, faction blame).

## Recommended follow-ups

- Add a lethal/finishing primitive to the spike toolkit (instant melee-attributed
  kill, or `weakenBattle` to ~1 blood) and re-run to get a large death sample
  (target: >50 KO/death events, many in overlapping windows) for a statistically
  meaningful correctness rate.
- Dedicated swarm spike: 4-6 attackers on 1 victim; verify the stamped actor is "a"
  real attacker and decide whether last-wins is acceptable or should be first-hit /
  most-damage.
- Keeper primitives from this spike (reverted): `engine::driveBattleMelee` and
  `engine::weakenBattle` - reusable for any future battle-stress / lethality probe.
