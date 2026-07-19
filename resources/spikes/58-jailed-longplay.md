# Spike 58 - Jailed long-play diagnostics (jailed / slaves save / cage2)

- Type: RUN
- Status: DONE
- Saves: jailed, slaves save, cage2
- Branch commit: <filled at commit>

## Goal

A remote (internet) manual test surfaced three interlocking jail/slave desyncs
that spike 57's 220 s passive soak was too short to characterize:

1. A jailed **host** PC that "exits the cage to run, then teleports back in", on a
   loop, as seen on the **join**.
2. Host NPCs "desyncing" while the two PCs were caged slaves in the same town.
3. The host being unable to clear the "Obedient slave" job (it re-applies).

This spike measures the real interactions over ~10 min soaks on three dense
captive saves, quantifies the twitch magnitude, and sizes the fix BEFORE any
behavior change. All diagnostics are knob-gated OFF by default.

## Method

Read-only, knob-gated (see [CODE_MAP.md](../CODE_MAP.md)):

- `KENSHICOOP_JAIL_PROBE` -> `[jail] STATE` (own vs drv captive state, ~4/s) +
  the new **`[jail] SNAP`** re-seat metric in
  [ReplicatorDrive.cpp](../../src/plugin/sync/ReplicatorDrive.cpp): at each
  furniture self-heal, `divergence=` (how far the driven copy had drifted from
  the owner's streamed pos = the visible teleport magnitude) and `localStep=`
  (drift since last tick while nominally seated; `>0` => the copy's OWN local AI
  is walking it - the "exit cage to run" half).
- `KENSHICOOP_TASK_SPIKE` -> `[spike] SELECT`: the task the local AI selects per
  body (TaskType ordinal; mapped from `kenshi/Enums.h`).
- `KENSHICOOP_JAIL_OBSERVE` -> `[jail] OBSERVE`: host runs a captive UNOPPOSED
  (no drive/suspend/self-heal) and logs its trajectory, to classify the guard
  put-to-work as fixture-relocate vs job walk-round.
- `auditRows` (SCENARIO WNPC/WORLD) for pos/context.

New long-play harness:

- `jail_soak` scenario ([scenarios.psd1](../../scripts/scenarios.psd1)): a ~9.5 min
  passive soak (WorldParityScenario with jail-specific duration in
  [ScenarioNpc.cpp](../../src/plugin/test/ScenarioNpc.cpp)); arms JAIL_PROBE +
  TASK_SPIKE (NOT observe - observe suppresses the self-heal the SNAP measures).
  `-Save` overrides the default `jailed`. Partitions by role (host=rank 0,
  join=rank 1), so each side owns one caged PC.
- `manual_session.ps1 -JailProbe` ([manual_session.ps1](../../scripts/manual_session.ps1)):
  arms JAIL_PROBE + TASK_SPIKE (self-heal ON = real behavior) on both clients and
  auto-collects BOTH logs on exit. `-JailObserve` is a separate opt-in that adds
  observe mode; do NOT use it for parity A-B (observe disables the self-heal, so
  captives exit furniture and walk off on the observing side - a diagnostic
  divergence, not a real desync; this was mis-observed once as a "join-only NPC
  exit" before `-JailProbe` stopped arming observe).

Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario jail_soak -Save jailed
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario jail_soak -Save "slaves save"
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario jail_soak -Save cage2
```

Runs analyzed:

- `tools/test-runs/20260718_162720/` - jailed, ~9.5 min (SNAP/normal).
- `tools/test-runs/jailed_observe_20260718_161255/` - jailed, observe trajectory.
- `tools/test-runs/20260718_163803/` - slaves save, ~9.5 min.
- `tools/test-runs/20260718_164845/` - cage2, ~10 min.
- `tools/manual-sessions/20260711_zoom/` and the remote host log
  (`Kenshi\KenshiCoop_host.log`, 15:52) - the original remote repro (this machine
  was JOIN id=1).

## Raw evidence

### The teleport (SNAP), per save (join side)

| save   | SNAPs | distinct bodies | median div | max div | div>50u | div>100u | localStep>2u |
|--------|------:|----------------:|-----------:|--------:|--------:|---------:|-------------:|
| jailed |    22 |              10 |      74.7u |  278.8u |      12 |        6 |            8 |
| slaves |    33 |              15 |      88.5u |  884.7u |      18 |       14 |            3 |
| cage2  |     5 |               5 |      50.8u |  488.8u |       3 |        1 |            0 |

The re-seat teleport is large (median 75-88u, tail to 885u) and hits 10-15
distinct captive bodies per run - the "many units twitching near the cages".

### The kind conflict that drives it

The owner streams a caged prisoner as BOTH `BODY_IN_CAGE` and `BODY_CHAINED`
(host `[jail] STATE side=own ... kind=2 chained=1 isSlave=1`). The driven copy's
reliable furniture ENTER edge and the continuous CHAINED bit disagree, so
`streamKind` (chained=3) != `localKind` (cage=2) and the heal breaks the cage and
re-chains, teleporting the body (jailed, hand `2,604125952`):

```
[16:28:02.048] [furn] RECV EXIT  occ=2,604125952 furn=1,3064575232 kind=3
[16:28:02.048] [furn] RECV ENTER occ=2,604125952 furn=1236,296997056 kind=2
[16:28:02.082] [jail] SNAP hand=2,604125952 kind=3 was=2 divergence=154.4 localStep=0.0 ok=1
```

### The "exit cage to run" half

8 of 22 jailed SNAPs carry `localStep>2u`: the AI-suspended driven copy still
drifted before the heal caught it (e.g. `localStep=28.6`), i.e. its own local AI
walked it out before the re-seat snapped it back.

### Guard put-to-work is a WALK-ROUND, not a relocate (OBSERVE run)

57 distinct captives observed. Busiest (`1,2512939520`) toggles `localKind` 3
(pole) <-> 2 (cage). Of 13,674 unopposed samples, **4,919 (36%) show movement,
max step 1,682u** - when the host runs the captive unopposed it drags it on a
job round hundreds of units, not to one fixed work spot.

### What the guards actually select ([spike] SELECT, host)

| save   | dominant selected tasks (count) |
|--------|---------------------------------|
| jailed | USE_CAGE 107 (1338), STAND_AT_NODE 51 (173), USE_BED 98 (22) |
| slaves | MOVE_ON_FREE_WILL 1 (524), STAND_AT_NODE 51 (315), OPERATE_MACHINERY 87 (102), SEND_DIALOGUE 159 (77) |
| cage2  | MOVE_ON_FREE_WILL 1 (244), **CHAIN_TARGET 166 (216)**, STAND_AT_NODE 51 (208), MOVE_ON_FREE_WILL_FAST 67 (143) |

cage2's guards spend a huge share re-issuing `CHAIN_TARGET` - the shackle
contention hotspot behind the "cage2 unlock desync" report.

### Census-band churn ("host NPCs desynced")

`[census] FREEZE` fired 2222 / 2228 / 1472 times across 19 / 33 / 27 distinct
world hands (jailed / slaves / cage2) - the divergence-gated AI freeze is
constantly firing on captive-town NPCs whose local AI diverges from the host.

### No PEER-EXIT edge (still)

Across all three ~10 min runs: **0** `PEER-ENTER` / `PEER-EXIT` events (1 stray
in cage2 join). Cross-owner furniture changes never cross the wire; the only
correction is the local self-heal - the pure host-side churn loop from spike 57,
now confirmed at scale.

## Findings

1. **The visible twitch is a furniture-kind conflict on the driven copy, not a
   real position desync.** A prisoner streamed as chained+caged has `streamKind`
   (chained=3) disagreeing with the reliably-authored `localKind` (cage=2); the
   self-heal resolves the disagreement by teleporting the body (median ~75-88u,
   tail to 885u), 10-15 bodies per run. This is the mechanism behind "exits the
   cage and teleports back", now measured.
2. **The driven copy's local AI still moves it between heals.** Even AI-suspended,
   ~36% of the time the copy walks (SNAP `localStep>2`; OBSERVE step>1u), so the
   twitch is exit-then-snap, not a pure teleport.
3. **Guard "put to work" is a job walk-round (up to ~1.7 k u), not a fixed
   relocate.** So a naive "relocate to work pole" replication would not reproduce
   what the host actually does; true parity needs job intent (large).
4. **The obedient-slave job is native Kenshi AI, re-applied locally.** The
   captive PC streams `task=65535` (TASK_NONE) on both sides throughout - the job
   the host player "can't clear" is the host's OWN local slave-town AI reissuing
   it (TARGET_IS_MY_SLAVE etc.), not something sync sends. Sync only aggravates
   the *visual* via the kind-conflict teleport. (Consistent with spike 57.)
5. **cage2's desync is dominated by CHAIN_TARGET contention.** Guards on both
   sides repeatedly try to (re)chain the same prisoners; with no shackle-authority
   handoff each side's guard AI fights the other's streamed chain state.
6. **The census-band freeze is load-bearing but noisy.** It is what keeps the
   captive-town NPCs from fully diverging, but it fires thousands of times - a
   sign the town AI and the stream disagree continuously around dense captive
   scenes.

## Validation

- Findings 1-2: `tools/test-runs/20260718_162720/join.log` SNAP lines (22, stats
  table above); the `2,604125952` timeline pairs a `RECV ENTER kind=2` with a
  `SNAP kind=3 was=2 divergence=154.4` 34 ms later.
- Finding 3: `tools/test-runs/jailed_observe_20260718_161255/join.log` -
  `[jail] OBSERVE` 13,674 samples, 4,919 with `step>1`, `max=1682.5`; busiest
  captive `localKind` 3/2 split 630/116.
- Finding 4: every `[jail] STATE` for the caged PCs reads `task=65535 raw=65535`
  on both `side=own` and `side=drv` in all three runs.
- Finding 5: `tools/test-runs/20260718_164845/host.log` - `CHAIN_TARGET` (166)
  is the 2nd-most-selected task (216).
- Finding 6: `[census] FREEZE` counts per run (2222 / 2228 / 1472).

## Recommended follow-ups (smallest -> largest)

1. **Kill the kind-conflict teleport (recommended first, no wire change).** When a
   driven copy is streamed as chained AND the reliable edge says cage/bed (or
   vice-versa), pick ONE authority for the transform instead of letting the
   `streamKind=3` fallthrough fight the `localKind=2` cage anchor every
   `FURN_HEAL_MS`. Concretely: while both `BODY_CHAINED` and `BODY_IN_CAGE/BED`
   are set, treat the cage/bed as the transform anchor (kind 2/1) and re-assert
   the shackle as an EQUIP state only (no transform), so there is no 75-885u
   re-seat. Directly removes findings 1-2's visible twitch.
2. **Hold the driven captive harder between heals.** For a streamed-captive body,
   also `haltMovement` per tick (not just `addAiSuspend`) so `localStep` stays 0
   and there is no exit-to-run before the heal. Cheap; complements #1.
3. **PEER-EXIT authority (full parity, larger).** A symmetric partner to
   PEER-ENTER so the owner moves its own captive to the work pole and both sides
   agree - but per finding 3 this only looks right if the job walk-round intent is
   also replicated (doctrine-31 purchase-as-transfer-style job intent). Big.
4. **Shackle-authority handoff for cage2.** Suppress a driven guard's
   `CHAIN_TARGET` against a body whose owner already streams the chain state, so
   the two guard AIs stop fighting over the same prisoner.

Spike code (the `[jail] SNAP` metric, `jail_soak` scenario, `manual_session
-JailProbe`) is knob-gated OFF by default and left in place as a reusable probe.
No behavior changes shipped.
