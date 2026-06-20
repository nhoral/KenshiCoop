# Spike 22 - Limb-loss replication

- Type: RUN
- Status: DONE
- Save: down1 (baked subject resolves identically on both clients)

## Goal

When a character loses a limb on the host, does the join see it?

## Method

`run_spike.ps1 -Id 22 -Save down1`. Both clients pin the nearest baked non-squad
NPC (`pickDownSubject`); the host severs its LEFT_ARM (`engine::severLimb`: flesh=0)
at t=9s and both log `engine::readMedical` each tick. Full logs in `22/raw/`.

## Raw evidence

Both clients pinned the **same** baked body: `hand=1,19,3600952064,1,2897704960`
(identical host & join - confirms baked-save cross-client resolution).

```
HOST t=8515  ... LA=100/-1 ...          (before)
HOST induce t=9015 sever LEFT_ARM->STUMP
HOST t=9015  ... LA=0/-1 ...            (host left arm flesh -> 0)
JOIN t=10515 ... LA=100/-1 ...          (join: unchanged)
JOIN t=22515 ... LA=100/-1 ...          (join: still 100, whole run)
```

## Findings

1. **Limb loss does NOT replicate.** The host's subject lost its left arm
   (`flesh 100 -> 0`); the join's copy of the *same* body kept `LA=100` for the
   entire run. There is no wire field for limb health/sever, so the join never
   learns.
2. Confirms baked subjects resolve identically (same hand both sides) - the test
   itself is valid; the divergence is real, not a pinning mismatch.

## Implications for co-op

- Visible desync: a one-armed fighter on the host appears intact and fights
  normally on the join. Combat outcomes that depend on `hasAnArmToFightWith()` /
  `getMissingArmPenaltyMult()` will differ between clients.
- Fixing this needs a limb-state field in a medical delta (spike 21) OR a one-shot
  "limb severed" event (limb id + state) the join applies via `MedicalSystem::
  amputate`/`setLimb`. The event form is cheap and matches the existing EVT model.

## Recommended follow-ups

- Add `EVT_LIMB` (subject hand + RobotLimbs::Limb + LimbState) host->join, applied
  with `amputate(limb, createSeveredItem=false, force=0)` so the join shows the stump.
