# Spike 24 - Sleeping in a bed (task + subject pose)

- Type: RUN (recipe) + code
- Status: PARTIAL
- Save: down1 used for the medical portion; sleep-pose portion deferred

## Goal

When a character sleeps in a bed on one client, does the peer see the sleep pose,
and does the rest benefit (medical `restedState`) sync?

## Evidence / reasoning

- Sleeping is a **task + animation** anchored on a bed BUILDING, exactly like
  sitting (the repo already has `sit1`/`sit2` baked saves) and the `craft` scene
  (operate-machinery pose). Those established the rule (Engine.h ~542): a
  node/furniture behavior is reproduced on the join by letting the body's OWN local
  AI execute the action at the resolvable fixture - NOT by streaming the animation.
- The rest BENEFIT is medical: `MedicalSystem::isFullyRested()`, `restedState`
  (0xB0), `canGetUpWakeUp()`. Spikes 21-26 proved medical state does not replicate.

## Findings (preliminary)

1. **Sleep POSE should replicate the same way sit/craft do**: if the bed is a baked
   (save-stable) building both clients resolve, and the host orders the body to use
   it, the join's local AI can reproduce the sleep pose. This reuses the proven
   craft/sit node-behavior path; no new wire field needed for the POSE.
2. **Rest benefit does NOT sync** - `restedState` is local medical, so each client
   restores rest on its own timeline (cosmetic divergence unless a player reads the
   peer's rest).
3. Untested at runtime here: confirm the sleep action specifically (some bed
   interactions may differ from chairs). The mechanism strongly predicts success.

## Why PARTIAL

The medical/rest half is settled (no sync, by spikes 21-26). The sleep-POSE half is
predicted from the sit/craft results but not directly run in this batch.

## Recommended follow-ups

- Bake a "leader + bed" scene (spike 5 `save()`), then `SpikeScenario` id 24:
  host orders the leader to sleep; both log task + pose; confirm the join reproduces
  the sleep animation via local AI (mirroring `setupCraftScene`/`rearmCraftScene`).
