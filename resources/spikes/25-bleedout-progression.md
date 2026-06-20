# Spike 25 - Bleed-out progression sync

- Type: RUN
- Status: DONE
- Save: down1

## Goal

Does blood level / bleed-out progress sync, so a character downed by bleeding on the
host also goes down on the join?

## Raw evidence (spike 22 run, same probe)

```
HOST: blood 75.8 -> 40 (t=4s) -> 5 (t=14s) -> 0 (t=24s, killSubject)
      (also note: host blood slowly REGENERATES, 40.0->40.4 over 10s - Kenshi
       heals blood over time when not actively bleeding)
JOIN: blood = 75.8 for the ENTIRE run (never moved)
```
`currentBleedRate` read 0.00 on both throughout (the probe set `blood` directly via
`woundSubject` rather than opening a bleeding wound, so no active bleed rate - a
follow-up should use `addWound` to get a non-zero `currentBleedRate`).

## Findings

1. **Blood level does NOT sync.** The host's subject bled to 0 and the join's copy
   stayed at 75.8. No wire field carries blood.
2. **Blood regenerates locally** on each client independently - even if we synced a
   snapshot, both sims would then drift apart again without continuous updates.
3. The probe forced blood directly, so `currentBleedRate` stayed 0; to study true
   bleed-out timing use `MedicalSystem::addWound` (sets a real bleed rate) - noted
   as follow-up.

## Implications for co-op

- A bleed-out down/death on the host is invisible to the join unless the resulting
  DOWN/DEAD pose is separately replicated (the bodyState/EVT path, spike 26). Blood
  as a continuous quantity is the wrong thing to stream (it regenerates locally and
  drifts); stream the OUTCOME EDGES (collapsed, died) instead.
- If a blood BAR is ever shown for peer squads, it must be fed by a throttled
  host->join medical delta, accepting it's approximate between updates.

## Recommended follow-ups

- Re-run with `addWound` to capture real `currentBleedRate` and the host-side
  collapse timing (`pointOfCollapseBloodloss`), then verify only the collapse EDGE
  needs to cross the wire.
