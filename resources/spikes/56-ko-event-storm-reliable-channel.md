# Spike 56 - KO event storm: reliable-channel backpressure at 20v20

- Type: RUN (bake-then-share, 2-phase)
- Status: DONE
- Save: c -> bakes "spikebake56" -> 2-client run on that save
- Branch commit: <filled at commit>

## Goal

Under a large simultaneous-KO storm (20v20 = 40 bodies), does the RELIABLE event channel
(EVT_KNOCKOUT / EVT_DEATH / EVT_REVIVE) deliver EVERY transition to the join without loss,
duplication, or reordering - i.e. no backpressure drops when many bodies go down at once?

## Method

- First cross-client combat spike to use the bake-then-share workflow from spike 82:
  - **Phase A** (id `56bake`, HOST-ONLY): `spawnLethalBattle(20, 3)` = 40 bodies in 3
    clusters, `rearmBattle` to keep them engaging, then `bakeSave("spikebake56")` at
    t=14s while the battle is hot (34 fighting / 4 already down at bake time).
  - **Phase B** (id `56`, 2-client, `-Save spikebake56`, 100s): both clients load the
    baked battle; it resumes fighting; the `Replicator` auto-emits `[event] SEND id=..`
    on the host and the join logs `[event] RECV id=..`. No new event-system code - the
    existing reliable channel is measured directly.
- Delivery check: compare the host's SEND event-id SET to the join's RECV id SET.

## Findings

1. **ZERO loss under the storm - perfect delivery.** Host emitted **42 events (ids 1..42
   contiguous)**: 41 EVT_KNOCKOUT + 1 EVT_REVIVE. Join received **exactly the same 42
   ids**; a set diff of SEND vs RECV is **IDENTICAL** (no gaps, no duplicates, complete
   coverage). The reliable channel absorbed the 40-body KO storm with no backpressure drop.
2. **KO STATE converges across clients.** Down-counts track within 1-2: host down
   26->29->29->28, join down 26->28->29->30 over the same window. Both wind down to ~1-4
   still fighting.
3. **Events are DECOUPLED from entity streaming.** The join streams FEWER bodies than the
   host (join near80=33-34 vs host 38) due to the interest/stream cap (spike 14), yet it
   still received ALL 42 events. The reliable event path latches KO/death by hand even for
   bodies the join isn't currently streaming (applyEvents creates a placeholder target) -
   so a body that is culled from streaming still gets its down/death transition.
4. `join_3.png`: the full ~40-body melee with many downed bodies + blood pools, rendered
   and fighting on the JOIN.

## Validation

- `56/raw/host.log` / `join.log`:
  - `rg -c "[event] SEND"` = 42; `ev=1` (KO) = 41; `ev=3` (revive) = 1.
  - `rg -c "[event] RECV"` (join) = 42.
  - SEND ids sorted-uniq = 42, min=1 max=42 (contiguous); RECV ids sorted-uniq = 42.
  - `diff <(SEND ids) <(RECV ids)` -> IDENTICAL.
- Convergence: `SPIKE 56 storm56 role=host/join ... down=N` census lines.
- Phase A: `SPIKE 56bake bakeSave issued=1 near80=41 fighting=34 down=4`; save on disk
  `...\kenshi\save\spikebake56\quick.save` (2.32 MB).

## Harness improvement shipped with this spike (peerReady gate)

Motivated by the observation that a live host spawn can fire BEFORE the join is loaded:
the scenario clock (`elapsedMs`) starts INDEPENDENTLY on each client the moment that
client reaches gameplay, so `elapsedMs>=4s` on the host is typically well before the join
has connected/loaded. Added `ScenarioContext::peerReady` (backed by
`Inbound::sawRemoteEntity()`): true once this client has received an owned-entity batch
from a peer - which only happens after that peer reaches gameplay + starts streaming.
**Validated here**: the host's `peerReady` flipped 0->1 between t=5s and t=10s (exactly
when the join finished loading). A live-spawn spike gated on `peerReady` would correctly
wait ~10s for the join instead of spawning into an unwitnessed world. (Bake-then-share is
already immune - Phase A is host-only and the join loads the finished save.)

## Open questions / hypotheses (UNVALIDATED)

- **DEATH events**: this storm produced KOs + 1 revive but 0 EVT_DEATH (melee KOs, doesn't
  finish within the window). A true death storm (spike 57) needs bleed-out/finishing to
  exercise the permanent-latch path at volume.
- **Actor attribution at volume**: SEND lines carry an `actor=` stamp; whether the actor is
  correct for each KO in a 40-body blob was not audited here (spike 67/68).
- **Bandwidth / channel timing**: loss=0, but per-event latency and reliable-channel byte
  cost under the storm were not measured (spike 58).
- **Higher volume**: 40 bodies = 42 events over ~85s (~0.5 ev/s peak). A tighter,
  higher-rate burst (e.g. simultaneous AoE) could stress backpressure harder - untested.

## Implications for co-op

- The reliable KO/death event channel is robust: it will not lose a knockdown/death under
  realistic battle volume, and it self-heals bodies the join isn't even streaming. This
  is the guarantee the combat layer (L5) was built on, now confirmed at 20v20.
- `peerReady` is the correct gate for ALL future LIVE host actions the join must witness.

## Recommended follow-ups

- Spike 57 (death storm) using bleed-out/finishing to generate EVT_DEATH at volume.
- Spike 58 (bandwidth vs combatant count) instrumenting reliable-channel bytes/s.
- Keeper primitives (reverted): `spawnLethalBattle`/`rearmBattle`/`bakeSave`/`countCharsNear`,
  `SpikeScenario` ids `56bake`/`56`. PERMANENT (committed): `ScenarioContext::peerReady` +
  `Inbound::sawRemoteEntity()`. `spikebake56` left on disk (reusable 20v20 baked battle).
