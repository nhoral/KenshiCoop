# Spike 23 - Bandage / first-aid detection + replication

- Type: RUN (derived from spike 22 probe) + code
- Status: DONE

## Goal

Can we detect a bandaging/first-aid action and does its effect replicate?

## Evidence

- `MedicalSystem::applyFirstAid(skill, equipment, frameTime, who)` /
  `applyDoctoring` / `applyRigging` heal the LOCAL medical (raise `HealthPartStatus::
  flesh`, set `bandaging` 0x48), gated by `scoreFirstAidNeed()`.
- Spike 22 established that NO medical field (`flesh`, `blood`, `bandaging`) crosses
  the wire: the host changed limb flesh and blood, the join saw none of it.

## Findings

1. **First-aid effects do NOT replicate.** Healing applied on one client raises
   that client's `flesh`/`bandaging` only; the peer's copy is unchanged (same
   mechanism as spike 22's limb result, inverse direction).
2. **Detection is possible host-side** via the `bandaging` float per limb and the
   "being treated" task state, but there is no event today that signals "X is
   bandaging Y" to the peer.
3. Because each client's medical heals independently, two un-synced sims will drift:
   a wound bandaged on the host still bleeds on the join (and vice-versa).

## Implications for co-op

- Medic gameplay (a player healing the other's squad) is currently cosmetic on the
  initiator's screen only. To make it shared we need EITHER the full medical delta
  (spike 21) OR a "treatment applied" event carrying the resulting limb HP so the
  peer can set it.
- Detection of the ACTION (someone is administering first aid) is easy to surface
  for a HUD; the harder part is the medical STATE the action produces.

## Recommended follow-ups

- Add the medical delta channel (spike 21); first-aid then "just works" as a state
  change. Until then, treat medic actions as host-authoritative on the body's owner.
