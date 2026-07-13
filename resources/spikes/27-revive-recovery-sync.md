# Spike 27 - Revive / recovery sync + EVT_REVIVE

- Type: RUN (derived from spike 22 probe) + code
- Status: DONE

## Goal

When a downed/KO'd character recovers (stands back up) on one client, does the peer
see it stand? Is there a revive event?

## Evidence

- Recovery is driven by the local `MedicalSystem`: the `knockoutTimer` lapses,
  `canGetUpWakeUp()` returns true, the wake AI stands the body. In spike 22 the host
  held the subject down by topping `knockoutForceTimer` (`holdDown`/`knockDown`),
  and the join's medical never even registered the KO (`unc=0` throughout).
- There is **no EVT_REVIVE** in the wire today; `knockDown(c,false)` exists
  host-side to wake a body (`engine::knockDown` off-path clears the KO timer +
  ragdoll), but nothing publishes a "got up" edge.

## Findings

1. **Recovery does NOT sync as medical state.** Each client's body wakes on its own
   medical timeline. If the host's body is held down but the join never saw it go
   down (spike 26), the join's body was never down to begin with - the two are
   simply running independent medical sims.
2. The down/KO POSE can be driven onto the join for an owned/pinned subject via the
   existing bodyState/EVT path, and clearing that pose is the de-facto "revive" for
   animation - but there is no symmetric, explicit revive EVENT, and it doesn't
   touch the join's medical flags.

## Implications for co-op

- For shared down/revive of player squads, the clean design is a pair of edges:
  publish `BODY_KO/BODY_DOWN` on collapse and a `BODY_UP` (EVT_REVIVE) on recovery,
  and have the join set BOTH the pose AND `medical.unconcious` so `isUnconcious()`
  agrees (ties into spike 26's fix).
- Without this, a teammate you "revived" may still appear down on your screen, or a
  body you think is KO'd is actually up and walking on the peer.

## Recommended follow-ups

- Add `EVT_REVIVE` (subject hand) as the recovery counterpart to the death/down
  events; apply via `engine::knockDown(c,false)` + clear join medical KO flags.
