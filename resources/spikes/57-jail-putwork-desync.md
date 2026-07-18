# Spike 57 - Jail PC "put to work" desync

- Type: RUN
- Status: DONE
- Save: jailed
- Branch commit: <filled at commit>

## Goal

Pin the exact mechanism behind the reported twitch: a **join-owned** PC is jailed
in the camp, a **host** guard comes to "put the prisoner to work," the PC briefly
exits the cage and then teleports back in. Decide the real fix scope (PEER-EXIT
authority vs prisoner/slave-flag replication vs consistency-only suppression)
before writing any fix.

The forking question the spike had to answer (see the plan): which side initiates
the brief exit, what re-cages it, and whether the join's PC prisoner/slave flags
are even set.

## Method

Read-only diagnostics, all gated OFF by default:

- `KENSHICOOP_JAIL_PROBE` -> `[jail] STATE` per-tick observer (throttled ~4/s per
  body) at the two places furniture is already read:
  - `publishOwned` ([ReplicatorPublish.cpp](../../src/plugin/sync/ReplicatorPublish.cpp)) - `side=own`: the join's real, authoritative PC.
  - `applyTargets` ([ReplicatorDrive.cpp](../../src/plugin/sync/ReplicatorDrive.cpp)) - `side=drv`: the host's driven copy of that PC.
  Fields: `kind` (0 none / 1 bed / 2 cage / 3 chained-pole), `chained`,
  `slaveOwner`, `isSlave` (new `Character::isSlave` read lever, RVA 0x5C8320),
  `task`/`raw`, `pos`, `mv`, plus `streamKind`/`localKind` on the drv side.
- `KENSHICOOP_TASK_SPIKE` -> `[spike] SELECT`: what task the local AI actually
  selects (per body), to see whether the join runs any "work" task for its PC.

Reproduce (automated, partitioned by role so the join owns tab 1):

```
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario jail_probe
```

Run analyzed: `tools/test-runs/20260718_093355/` (host.log, join.log). ~180 s
passive soak on the `jailed` save; the run auto-armed `KENSHICOOP_JAIL_PROBE=1`
and `KENSHICOOP_TASK_SPIKE=1` and `auditRows`.

## Raw evidence

Join PC hand = `1,2128417920` (join-owned). Its host driven copy carries the same hand.

Join's authoritative PC (`side=own`), all 645 samples identical:

```
[jail] STATE side=own hand=1,2128417920 kind=2 chained=1 slaveOwner=2,3733154304 isSlave=1 task=65535 raw=65535 pos=-47458.4,929.8,-54772.6 mv=0
```

Host's driven copy (`side=drv`) - steady, then a single 2->3->2 twitch:

```
[09:35:03.400] [furn] HEAL ENTER occ=1,2128417920 kind=2 ok=1
[09:35:03.400] [furn] cage-quiet occ=1,2128417920 kind=2
[09:35:03.732] [jail] STATE side=drv hand=1,2128417920 streamKind=2 localKind=3 chained=1 isSlave=1 task=65535 raw=65535 mv=0
... (localKind=3 held ~1.3 s, 5 samples) ...
[09:35:04.906] [furn] HEAL ENTER occ=1,2128417920 kind=2 ok=1
[09:35:04.907] [furn] cage-quiet occ=1,2128417920 kind=2
```

Join guard (driven Holy Sentinel copy) picks a grab/order task targeting the PC:

```
[09:34:42.050] [spike] SELECT body='Holy Sentinel' task=1 subj=1,2128417920 ...
```

Cage entry edge (host received the join's ENTER once, then only self-heals):

```
[09:34:42.335] [furn] RECV ENTER id=1 occ=1,2128417920 furn=2346,1500610304 kind=2 ok=1
```

## Findings

1. **The join's PC is correctly flagged as a caged slave, but has no task.** On the
   join (authoritative side) it is `kind=2` (cage) + `chained=1` + `isSlave=1`
   with `task=raw=65535` (TASK_NONE) for the entire run. The join knows it is a
   jailed slave; it just never runs any obedient-work behavior for it (it is a
   player character - PCs don't execute the town/slave job AI).
2. **The join's authoritative PC never moves.** All 645 `side=own` samples are the
   same position with `mv=0`; it never leaves the cage. There is no join-side
   exit at all in a properly partitioned run.
3. **The twitch is entirely on the HOST's driven copy.** Exactly once (~09:35:03-04)
   the host's copy went `localKind` 2 -> 3 (cage -> chained pole) for ~1.3 s while
   `streamKind` stayed 2, then snapped back to 2.
4. **Initiator = host-local world sim; re-cage = host furniture self-heal.** The
   host guard's local AI dragged the peer-owned driven copy out of the cage onto
   a work pole (kind 3); ~1.5 s later the host's own `streamKind=2` cage self-heal
   (`[furn] HEAL ENTER`/`cage-quiet`, ReplicatorDrive.cpp) re-cages it. Nothing
   crosses the wire: the join keeps streaming `BODY_IN_CAGE`, so the host must undo
   its own guard's action. There is no PEER-EXIT edge.
5. **The task never changes to "work" on either side.** `task=65535` on both
   `side=own` and `side=drv` throughout; the only "put to work" signal is the host
   guard's *local* order task, which acts on a body the host does not own.

## Validation

- Finding 1: `tools/test-runs/20260718_093355/join.log` - 645 `side=own hand=1,2128417920`
  lines, all `kind=2 chained=1 isSlave=1 task=65535 raw=65535` (only 1 outlier, differing
  only in `slaveOwner`, still `kind=2`). The `isSlave=1` read is the new
  `readSlaveState` lever ([EngineCharState.cpp](../../src/plugin/game/EngineCharState.cpp) `readSlaveState`; `Character::isSlave` resolved in [EngineInternal.cpp](../../src/plugin/game/EngineInternal.cpp)).
- Finding 2: same 645 lines - one distinct `pos=-47458.4,929.8` and `mv=0` for all.
- Finding 3: `host.log` - `side=drv hand=1,2128417920` timeline is `localKind=2`
  every second from 09:34:42 to 09:35:02, `localKind=3` only at 09:35:03-04 (5
  samples), then `localKind=2` again from 09:35:05 on. `streamKind=2` throughout.
- Finding 4: `host.log` lines 1722-1748 - `[furn] HEAL ENTER occ=1,2128417920 kind=2`
  + `cage-quiet` immediately before (09:35:03.400) and after (09:35:04.906) the
  `localKind=3` window bracket it; the self-heal is the kind=2 re-assert in
  ReplicatorDrive.cpp (~407-454). No `RECV EXIT`/`PEER-EXIT`/`SEND EXIT` for this
  hand exists in either log around the twitch (only the initial 09:34:42 `RECV ENTER`).
- Finding 5: both logs - every `[jail] STATE` line for the hand has `task=65535 raw=65535`;
  the only PC-targeting order is `[spike] SELECT body='Holy Sentinel' task=1
  subj=1,2128417920` in join.log (a guard body, not the PC).

## Open questions / hypotheses (UNVALIDATED)

- **Why the user saw the twitch on the JOIN screen.** In this partitioned automated
  run the join body is stone-still; the visible twitch is host-side. The manual
  report likely came from an OWN-ALL manual session (both clients drive the same
  PC) or from watching the host window. Would validate by re-running the
  `manual_session -Save jailed -Inhabit` case and watching the join window with
  `KENSHICOOP_JAIL_PROBE=1`; if the join `side=own` ever shows `kind!=2`/`mv=1`,
  there is a real join-side exit to explain. (Not reproduced here.)
- Whether the guard's driven-copy grab on the join can momentarily seize the join
  PC before authority corrects it (sub-250 ms, below the probe throttle).

## Implications for co-op

- The bug is **not** a missing prisoner/slave flag on the join - those are already
  set (`isSlave=1`, `kind=2`, `chained=1`). So "prisoner-flag replication so the
  join runs the job locally" is the wrong framing: even fully flagged, the join PC
  is a *player character* and will not run the obedient-work job AI.
- The concrete defect is **cross-owner furniture contention with no PEER-EXIT**:
  the host's local guard AI mutates furniture state on a body it does not own, and
  the only thing that undoes it is the host's own cage self-heal - a pure host-side
  churn loop. The authoritative join is never involved.
- This matches the existing `SYNC_GAPS`/baseline note that `EVT_EXIT_FURNITURE` is
  a no-op for `ownHands_` in [ReplicatorSpawn.cpp](../../src/plugin/sync/ReplicatorSpawn.cpp): there is a PEER-ENTER path but no PEER-EXIT.

## Recommended follow-ups

Fix scope, smallest -> largest:

1. **Consistency-only suppression (recommended first).** Stop the host's local AI
   from dragging a peer-owned caged body out of its furniture: when a driven copy
   has `streamKind != 0` (owner says it is in furniture), keep the AI-suspend /
   don't let the local guard task re-seat it, so there is no exit to re-cage. This
   removes the visible twitch with no wire change and matches how census-band
   divergence is already handled (freeze, don't fight the owner).
2. **PEER-EXIT authority (full parity).** Give the host a real edge to tell the
   join "your prisoner is being put to work": replicate the furniture EXIT/relocate
   for peer-owned bodies (a symmetric partner to PEER-ENTER), so the owner moves
   its authoritative body to the work pole and both sides agree. Larger: needs a
   new reliable edge + owner-side apply + task/job intent so the moved PC actually
   does the work (doctrine-31 "purchase-as-transfer"-style intent for jobs).
3. Not recommended: prisoner/slave-flag replication alone - already set; does not
   address the PC-has-no-job-AI reality.

Spike code (KENSHICOOP_JAIL_PROBE observer, `readSlaveState` lever, `jail_probe`
scenario) is knob-gated OFF by default and left in place as a reusable probe;
no behavior changes shipped.
