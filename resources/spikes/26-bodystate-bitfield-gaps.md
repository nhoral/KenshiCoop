# Spike 26 - bodyState bitfield gaps

- Type: RUN + code
- Status: DONE
- Save: down1

## Goal

The wire carries a coarse `bodyState` bitfield (down/KO/ragdoll/dead/crawl). What
medical reality does it MISS, and does even death reach the join's medical flags?

## Raw evidence (spike 22 run)

Host drove the subject all the way to death:
```
HOST t=19015 knockDown -> ko=8.0 unc=1
HOST t=24015 killSubject -> blood=0 dead=1 unc=1   (stays dead+KO to end)
JOIN  (same body) -> unc=0 dead=0 blood=75.8       (entire run, never changed)
```

## Findings

1. **The join's MedicalSystem flags never moved** - not blood, not `unconcious`,
   not `dead`, even after the host's subject was knocked out AND killed. The
   replication path (when active) drives the join's POSE/animation/ragdoll via
   `readBodyState`/`applyRaw`, but does **not** write the join's
   `medical.unconcious`/`dead`. So `Character::isDead()` on the join stays false.
2. `bodyState` (`readBodyState`, Engine.h ~500) only encodes
   down/KO/ragdoll/dead/crawl as POSE. It carries nothing about: blood, bleed rate,
   per-limb HP, severed limbs, bandaging, crippled, hunger, KO TIMER remaining.
3. Note: the dedicated `down_order`/`death_order` scenarios DO get a downed/dead
   POSE onto the join for an OWNED/pinned subject via the EVT path - but that drives
   the body's animation, not its medical flags. This spike's WORLD-NPC medical
   changes used no such event, so nothing crossed at all.

## Implications for co-op

- Any join-side logic that reads `Character::isDead()`/`isUnconcious()` (loot
  permission, "is this body a corpse", squad morale, revive eligibility) will be
  WRONG for host-driven bodies. The pose can look dead while the medical state says
  alive.
- The gap list above is the spec for what a medical sync must add beyond bodyState.
  Highest value: a `dead`/`unconcious` authoritative flag (so corpses/KO read
  correctly), then limb-loss events, then optional blood for HUD.

## Recommended follow-ups

- When the host publishes `BODY_DEAD`/`BODY_KO`, also set the join's
  `medical.dead`/`unconcious` (not just the pose) so `isDead()` agrees.
- Define the medical delta (spike 21) for the rest.
