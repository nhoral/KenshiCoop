# Kenshi Co-op Handoff: NPC, Body-State & Combat Sync (Layers L0–L5)

> Status: Working / committed. Written as context for future sessions. Grounded
> in (and cross-checked against) the actual code in
> `src/plugin/sync/Replicator.{h,cpp}`, `src/plugin/game/Engine.{h,cpp}`,
> `src/netproto/Wire.h`, and the scenarios in `src/plugin/test/Scenario.cpp`.
> Companion to `POSTMORTEM.md` (doctrines 1–27), `INTENT_REPLICATION.md` (the
> layered L0–L5 framework), and `WORLD_ITEM_SYNC_HANDOFF.md` (the item arc that
> followed). Where this doc and an older note disagree, **this doc was
> reconciled with the code on the date of writing — trust the code symbols cited
> here.**

## TL;DR for the next session

- The whole NPC stack is built on ONE principle: **replicate causes, not
  effects.** We never stream an animation clip. We stream identity + transform +
  locomotion scalars + AI intent + body state, and let the **join's own engine**
  produce the matching animation. Per-entity wire cost stays ~constant as
  animation variety grows, and every continuous field is **idempotent** (re-sent
  at 20 Hz, so a dropped packet self-heals).
- Identity is the Kenshi **`hand`** (5 fields: index, serial, type, container,
  containerSerial). It is save-stable and identical across machines that load the
  same save, so a streamed entity resolves to the *real local object*. Everything
  hangs off this. **Shared save is mandatory.**
- Behaviors are a **taxonomy of (class → lever-set)**, NOT one universal code
  path. Each class picks three levers: an **apply** lever, a **quieting** lever
  (stop local AI overriding), and an **authority/drift guard**. The same lever on
  the wrong class backfires — this is the hardest-won lesson (the sit/stand
  asymmetry).
- **State rides the unreliable 20 Hz batch; one-shot transitions ride the
  reliable `PKT_EVENT` channel.** A dropped "death" frame on the unreliable
  channel would leave a body alive on the join forever, so KO/death/revive are
  reliable + latched.
- Combat is **host-authoritative**: the host owns the outcome (who went down,
  who died, who gets the kill), the join renders reactions and never resolves a
  hit locally.

## The layered model (what each layer streams)

`EntityState` in `Wire.h` carries the per-entity state; `Replicator::applyTargets`
→ `applyRest` selects the apply regime. Layers, low to high:

| Layer | Streams | Apply lever | Status |
| --- | --- | --- | --- |
| **L0 Identity** | `hand` (5 fields) | resolve to real local object | DONE |
| **L1 Transform** | position + heading | `applyRaw` (teleport), `park` | DONE |
| **L2 Locomotion** | `cMoving`/`cSpeed`/`cMotion` (the "v4" scalars) | `walkTo` lead-point drive; engine picks gait | DONE |
| **L3 Intent/Task** | `task` (engine `TaskType`) + subject `hand` | `detachFromTownAI` + `applyTaskOrder` | DONE (sit/operate/craft) |
| **L4 Body state** | `bodyState` u16 bitfield | set body state directly, NO pathing | DONE |
| **L5 Combat** | `task==TASK_COMBAT_MELEE` + target `hand` + reliable events | `applyCombat` (order local melee) | DONE |

## What ships today, layer by layer

### L0–L2 — presence + locomotion

- The host captures owned characters (`engine::captureSquad`), streams an
  `EntityState` per body; the join resolves each by `hand` and drives the **real
  local object**.
- **Moving bodies**: lead-point walk-drive (`walkTo`) + a catch-up speed; we let
  the local engine animate the gait itself from the locomotion scalars rather
  than mirroring a clip. Guard: hard teleport when the gap exceeds `SNAP_DIST`
  (8.0 units, `Replicator.cpp`).
- **At-rest bodies**: mirror the v4 locomotion scalars as the last write of the
  frame so the engine settles to the right idle.

### L3 — fixture-bound task poses (sit / operate / craft)

A body posed at a fixture (stool, bench, machine) is a **task + subject hand**.
The join reproduces it by detaching from town AI then issuing a **player order**
pinned to the *same* fixture:

- Apply: `engine::detachFromTownAI(c)` then `engine::applyTaskOrder(c, out)`.
- Quieting: the order itself re-anchors the body, so detach is safe here.
- Guard: `TASK_DRIFT_MAX` (4.0 units) — abandon-to-park if the posed body drifts.
- Crafting/gathering reuses this lever almost verbatim (it is the same
  class). Validated via the `craft_order` scenario (live idle→operating order).

### L4 — body state (down / KO / dead / ragdoll)

Poses that are NOT a task at a fixture (knocked out, dead, ragdolling) cannot be
expressed as task+subject. They get a compact `bodyState` field and an apply path
that sets the body state directly with **no pathing** (you must never walk-drive a
downed body).

- Capture (`engine::readBodyState`, `Engine.cpp`): bitfield built from real engine
  reads — `Character::isDown` → `BODY_DOWN`, `isRagdoll` → `BODY_RAGDOLL`,
  `isDead` → `BODY_DEAD`. Unresolved reads leave `bodyState = 0` (upright), so a
  missing symbol degrades safely.
- `coop::bodyIsDown(s)` (`Wire.h`) = any of `BODY_DOWN | BODY_RAGDOLL | BODY_DEAD`.
- Apply: when the host streams a down body (or a latched event, below), the join
  holds the transform and keeps the body down; it does not path or walk-drive.
- Validated via `down_order` (live upright→down) and `death_order` (host KILLs the
  subject via the medical system so `Character::isDead()` flips → `BODY_DEAD`).

### Reliable event channel (`PKT_EVENT`)

`bodyState` is continuous/unreliable and self-heals — but the *instant* of a
transition must not be lost. So transitions ride a separate **reliable, sequenced**
channel:

- Wire: `PKT_EVENT = 5` carrying `EventPacket`; event types `EVT_KNOCKOUT = 1`,
  `EVT_DEATH = 2`, `EVT_REVIVE = 3` (`Wire.h`).
- Emission (`Replicator.cpp`, host publish): the host watches `bodyState` **edges**
  per hand (`hostBody_` map of prev state) and emits the matching reliable event
  on a transition — dead-edge → `EVT_DEATH`, down-edge → `EVT_KNOCKOUT`,
  rise-edge → `EVT_REVIVE`.
- Application (`Replicator.cpp`, join apply): events **latch** on the driven
  record — `EVT_DEATH` sets `deathLatched` (permanent) + `koLatched`,
  `EVT_KNOCKOUT` sets `koLatched`, `EVT_REVIVE` clears both. A latch FORCES the
  body down regardless of whether the next unreliable batch frame was dropped, so
  a death survives 30% packet loss. Validated via `death_order` under induced
  loss + latency.
- **The latch must survive a hand RE-KEY (2026-07-15).** The latch lives on the
  per-hand `Driven` record, and a dying body frequently RE-CONTAINERS (host
  un-squads a corpse / squad-tab move → `EVT_SQUAD_MOVE`). `rekeyPeerBody` erases
  the old key's record, so it now snapshots `deathLatched`/`koLatched`/
  `downApplied` and OR-merges them onto the new key (pure `rekeyCarryLatch` in
  `core/DeathLatch.h`, `[event] REKEY-LATCH` log). Without this a corpse that
  re-containers loses its pin and the join's local AI stands it back up — the
  "dead on one game, alive on the other" desync (bone-dog fight, serial
  3332275456). A container change breaking identity is the #1 desync lesson
  (below); the death latch was a casualty of it.
- **Owner-authoritative local-death veto (2026-07-15).** The `hitByMelee` damage
  guard blocks new melee wounds on a driven copy, but a non-melee/bleed path in
  an unguarded window can still flip local `medical.dead` with no owner event to
  reconcile it. `driveTargets` now vetoes it: a driven body the owner reports
  ALIVE (`!(bodyState & BODY_DEAD) && !deathLatched`) that went `isDead()` locally
  is un-killed via `engine::vetoLocalDeath` (`[death] veto` log, gated behind
  `dmgGuard_`). Death only takes hold on the peer via the owner's `EVT_DEATH`.
  Validated via `Test-DeathParity` (`scripts/oracles/Combat.ps1`) + prototest
  `testDeathRekey`.

### L5 — combat (melee intent + host-authoritative outcome)

The host streams a combat **intent**, not blow-by-blow: capture sets
`e.task = TASK_COMBAT_MELEE` with the attack target carried in the subject `hand`
fields (`Engine.cpp`). The join reproduces the *cause*:

- Apply (`Replicator.cpp`, Stage 3c, `coop::taskIsCombat(out.task)`):
  `engine::detachFromTownAI` once, then `engine::applyCombat(c, out)` orders the
  local copy to melee the same resolved target and lets the **join's own engine**
  run the fight (draw, swing, footwork).
- Re-arm: the order is issued once and re-armed on a throttle
  (`COMBAT_REISSUE_MS = 1500`) only if the local copy disengaged while the host
  still reports combat (checked via `engine::readCombat` → `inCombat`).
- We **deliberately do NOT walk-drive or park a combatant** — that fights the
  local combat movement and would freeze/slide the body. Position is only
  soft-corrected on large drift (`COMBAT_SNAP_DIST = 6.0`); combatants skip the
  AI-suspend path (their AI must run to animate).
- Disarm: when the host stops reporting combat, `combatArmed` clears and
  `clearGoals` drops the stale attack goal before re-parking.

### Damage / kill attribution

Damage itself is not streamed as a number per tick — the **outcome** is
(host-authoritative): the host's body-state edges produce the KO/death events, and
those drive the join's body down. Attribution (who gets the kill) is reconstructed
on the host from combat intent:

- `attackerOf_` map (`Replicator.cpp`): each tick, every entity whose
  `task == TASK_COMBAT_MELEE` stamps `attackerOf_[victim] = (attacker, now)`.
- The map is time-windowed: entries older than `ATTR_WINDOW_MS` (3000 ms) are
  pruned. When a victim's death/KO event fires, the still-fresh attacker is stamped
  as the event's ACTOR (`haveActor`). This "sticky" window means a body that dies a
  beat after the killing blow still attributes correctly.
- Validated via `combat_kill` (deterministic KO with time-windowed attribution),
  `combat_order` (live melee intent so a fight starting *after* the join loads
  still renders), and `combat_probe` (host combat-state read).

### Bidirectional per-tab ownership (the presence keystone)

Ownership is partitioned by Kenshi **squad tab** = the member's `hand` CONTAINER
rank. `publishOwned` + `applyTargets` run on BOTH clients; each side drives only
what it owns and excludes its own published hands from being driven by the peer
(the echo guard). This is what gives the guest real agency over its own squad
(host tab 0, join the rest). Validated via `coop_presence` (bidirectional
cross-check at 0 ms and WAN).

## What works (keep doing this)

- **Replicate causes, not effects (Doctrine 14).** Stream identity + transform +
  locomotion + intent + body state; never a clip/phase. Keeps the wire
  constant-cost and idempotent. Every win in this arc is an instance of this.
- **`hand`-based identity for every layer**, including combat targets (a target is
  a `hand`, never a pointer or a network id). Resolve-by-hand is the cornerstone
  and only works on a shared save.
- **(class → lever-set), not one code path (Doctrine 15).** Pick apply + quieting
  + guard per class. Don't reuse the last class's lever blindly.
- **State unreliable, transitions reliable (Doctrine 16).** Continuous fields at
  20 Hz self-heal; KO/death/revive go on `PKT_EVENT` and **latch** so a dropped
  frame can't un-kill a body.
- **Host owns combat outcome.** The join renders reactions; it never resolves a
  hit locally. Divergence is bounded by the host's authoritative body-state +
  events.
- **Every class ships a conformance oracle (Doctrine 17).** Host ground-truth read
  + a *rendered-body* read (not the field we wrote, so it can't self-confirm) + a
  tolerance + a deterministic baked scenario. The pose oracle reads the
  `Bip01 Pelvis` world height off the animated skeleton precisely so a written
  `task` flag can't fake a PASS.
- **Don't AI-suspend a combatant or a downed body inappropriately.** Combatants
  need their local AI running to animate the fight (they skip suspend); downed
  bodies must not be pathed/walk-driven at all.

## What does NOT work (don't re-learn the hard way)

- **Streaming a transform for a resting NPC is wrong.** A seated host produced a
  body STANDING where the host sat on the join — the pose did not follow. You must
  stream the *cause* (locomotion scalars + task), not the position.
- **Streaming a clip directly is not viable.** Idle/sit/stand are not slave
  animations (`runSlaveAnim` logged zero calls) and `AnimationClass` is opaque.
  This is why the whole framework streams causes instead.
- **The sit/stand asymmetry — the same lever backfires on the wrong class.**
  - *Sitters* need `detachFromTownAI` + a persistent location-bound ORDER. Detach
    is safe *only because* the order immediately re-anchors them.
  - *Standers* must **NOT** be detached. `separateIntoMyOwnSquad` changes the
    body's container, which changes its cross-client `hand` key, so the host can no
    longer match it and `enforceHostAuthority` suppresses it (the body goes
    ABSENT). Standers get `endAction` only.
  - Lesson: anything that changes a body's CONTAINER breaks its `hand` identity and
    desyncs it. Never re-squad / re-parent a driven body.
- **Don't walk-drive or park a combatant or a downed body.** It fights the local
  combat/ragdoll movement and freezes or slides the body. Position is soft-correct
  only on large drift.
- **A death on the unreliable channel can be lost.** If KO/death were only the
  `bodyState` batch, a dropped frame leaves a corpse alive on the join. That's the
  entire reason `PKT_EVENT` + latching exists.

## How to run / validate

Oracle/scenario tests (self-exit + verdict, via `scripts/run_test.ps1`):

```
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario npc_sync
```

```
powershell -ExecutionPolicy Bypass -File scripts/run_test.ps1 -Scenario combat_kill
```

Scenarios that exist today (`src/plugin/test/Scenario.cpp`): `npc_sync`,
`craft_order`, `down_order`, `death_order`, `combat_probe`, `combat_order`,
`combat_kill`, `coop_presence`. Combat/death scenarios are run under induced
packet loss + latency to prove the reliable-event latch.

- Build: `cmd /c scripts\build_plugin.cmd` (VS2010 / v100 x64).
- **Bumping `PROTOCOL_VERSION` (in `Wire.h`) means BOTH installs need the new
  DLL** or the handshake rejects the session — which is the desired behavior (no
  half-upgraded sessions).

## Tuning constants (current values, `Replicator.cpp`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `SNAP_DIST` | 8.0 | gap beyond which a moving body hard-teleports |
| `TASK_DRIFT_MAX` | 4.0 | posed-body drift beyond which we abandon-to-park |
| `COMBAT_SNAP_DIST` | 6.0 | combat drift beyond which we teleport-correct |
| `COMBAT_REISSUE_MS` | 1500 | re-arm the melee order at most this often |
| `ATTR_WINDOW_MS` | 3000 | how long a combatant's victim is remembered for kill attribution |

## Doctrine cross-reference

The framework doctrines this work established live in `INTENT_REPLICATION.md`
(14–18), extending the `POSTMORTEM.md` catalogue (1–13, 19–27). The item arc that
followed proposed 28–30 in `WORLD_ITEM_SYNC_HANDOFF.md`. The four load-bearing
ones for NPC/combat:

- **14 — Replicate causes, not effects.**
- **15 — A new behavior is a new (class → lever-set) entry, not new netcode.**
- **16 — State on the unreliable channel; transitions on the reliable channel.**
- **17 — Every class ships its conformance oracle** (host truth + rendered-body
  read + tolerance + baked scenario).
- **18 — Prefer divergence-gated authority where the local AI already agrees**
  (the `[gate]` metric is logged-not-yet-acted-on; promote it when the driven set
  needs to shrink).

## File map (where things live)

| Concern | File / symbols |
| --- | --- |
| Wire formats + version | `src/netproto/Wire.h` — `EntityState`, `bodyState` + `BODY_DOWN/RAGDOLL/DEAD`, `TASK_COMBAT_MELEE`, `PKT_EVENT`, `EVT_KNOCKOUT/DEATH/REVIVE`, `EventPacket`, helpers `taskIsCombat`/`bodyIsDown`, `PROTOCOL_VERSION` |
| Capture (host) | `src/plugin/game/Engine.cpp` — `captureSquad`, `readBodyState` (isDown/isRagdoll/isDead), combat-intent tagging (`task = TASK_COMBAT_MELEE`) |
| Apply primitives (join) | `src/plugin/game/Engine.{h,cpp}` — `applyRaw`, `walkTo`, `park`, `readMotion`, `detachFromTownAI`, `applyTaskOrder`, `applyCombat`, `readCombat`, `addAiSuspend` |
| Regime selection / drive | `src/plugin/sync/Replicator.cpp` — `applyTargets` → `applyRest`, Stage 3c combat override, `enforceHostAuthority`, `publishOwned` |
| Events + attribution | `src/plugin/sync/Replicator.{h,cpp}` — `hostBody_` (edge detect), `attackerOf_` (windowed attribution), `koLatched`/`deathLatched` |
| Tuning constants | `src/plugin/sync/Replicator.cpp` — `SNAP_DIST`, `TASK_DRIFT_MAX`, `COMBAT_SNAP_DIST`, `COMBAT_REISSUE_MS`, `ATTR_WINDOW_MS` |
| Tests | `src/plugin/test/Scenario.cpp` + `scripts/run_test.ps1` — `npc_sync`, `craft_order`, `down_order`, `death_order`, `combat_probe`, `combat_order`, `combat_kill`, `coop_presence` |
