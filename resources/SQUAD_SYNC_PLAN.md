# Phase 2.5 - Squad Synchronization: Implementation Plan

> Status: Implementation plan (actionable). Detailed expansion of the "Phase 2.5"
> outline in `MASTER_PLAN.md`, grounded in the current code
> (`src/plugin/KenshiCoop.cpp`, `src/plugin/Scenario.cpp`, `src/netproto/Protocol.h`).
> Companion to `POSTMORTEM.md`. Test-driven: each milestone flips a concrete
> scenario or visual check from RED to GREEN.

## Goal

Both players see each other's **whole squad** in the shared, host-authoritative
world: correct identity, position, pose, and locomotion animation - reusing the
proven Phase 2 NPC pipeline (`hand` identity, transform-when-moving,
task-when-resting, v4 movement-state mirror) instead of the Phase 1 spawn-ghost
path.

### Success criteria

1. `squad_spawn_sync` scenario reports `SCENARIO RESULT PASS` on both clients and
   `CROSSCHECK [host->join] squad_positions_match True` (host streams its squad;
   join receives every member by `hand` within tolerance).
2. `squad_move_sync` scenario passes **bidirectionally**: join moves its squad and
   the host observes it (`CROSSCHECK [join->host] ... True`), and vice-versa.
3. Visually (burst frames + manual): each peer's squad members appear at the right
   place with the right pose on the other client - including members the peer
   spawned at runtime (which do not exist in the observer's save).
4. No regression: existing NPC sync metrics (7/9 at <=0.2 m, `mv=0/spd=0` at rest)
   are unchanged; no crashes over a play session.

## Why the scenarios are RED today

- `publishNpcStates` explicitly **skips the player's own squad** (the `isPlayer`
  guard that compares each picked object against `playerCharacters[j]`), and it is
  **host-only**. So neither peer ever transmits its own squad; the observer logs no
  `SCENARIO RECV` and the `CROSSCHECK` has nothing to match.
- Therefore `squad_spawn_sync` (host-authoritative) and `squad_move_sync`
  (join-authoritative, bidirectional) both fail at the wire layer.

## What `CROSSCHECK` actually proves (important)

`SCENARIO RECV` is logged in `receiveNpcStates` from the **received wire entry**
(`e.x,e.y,e.z`), for every entry, *before* any `hand`->`Character` resolution. So:

- **Wire layer** (capture + serialize + identity + transport): turning the
  scenarios GREEN proves this end-to-end. This is the riskiest part and the
  primary target of the scenario harness.
- **Visual layer** (resolve-or-spawn + drive the local body): NOT proven by
  `CROSSCHECK`. A peer-spawned unit has no local `Character`, so the data arrives
  and `CROSSCHECK` passes while nothing is rendered. The visual layer is validated
  separately by burst frames / manual inspection and by the `resolvedOk` vs
  `resolveFail` counters already in `receiveNpcStates`.

This split is the backbone of the milestone order: get the wire correct first
(automated GREEN), then make it visible (spawn lifecycle + drive).

## Authority model

> **Refoundation update (see addendum at end + `POSTMORTEM.md`):** on a *shared*
> save both clients load the SAME `playerCharacters`, so "each peer owns its own
> squad" became "each peer owns a **disjoint, hand-ranked subset** of the one
> shared squad" (the inhabit partition). The rest of this section still holds with
> "its own squad" read as "its owned subset".

- **Each peer owns and streams its own squad** (the members of its local
  `playerCharacters`). This is **bidirectional**, unlike the host-only NPC stream.
- **A peer never drives its own squad** - it simulates it locally (the host runs
  real AI; the join runs local control/prediction for its own units).
- **The host still owns all non-player NPCs.** Remote-owned squad hands must be
  **excluded from the host's NPC interest pick**, or the host would re-stream the
  join's units back as NPCs and fight the join for authority over them.
- Conflict rule: an entity is owned by exactly one source - the peer whose
  `playerCharacters` contains it. Ownership is by `hand`, registered on receipt.

## Wire protocol changes

Reuse the `NpcStateEntry` shape (already carries `hand` + transform + task +
subject + v4 locomotion state). Add a player-owned squad batch distinct from the
host NPC batch so the receiver knows the owner and applies the right authority
rule.

- New packet `PKT_SQUAD_STATE` (or reuse `PKT_NPC_STATE` with an `ownerId` header
  field). Prefer a dedicated header: `[u8 type][u32 ownerId][u8 count][entries...]`.
- `ownerId` = the streaming peer's network player id (host or join). The receiver
  uses it to (a) tag those hands as remote-owned and (b) exclude them from any NPC
  re-stream.
- Bump `PROTOCOL_VERSION` (currently 4 -> 5) and document the squad batch.
- Keep batch size within one datagram (same `NPC_BATCH_MAX` math; a squad is small,
  so one batch is plenty).
- Decision: keep the lightweight `PKT_PLAYER_STATE` heartbeat for presence /
  join-leave only, OR fold presence into the squad batch (leader = member 0).
  Recommended: keep a minimal heartbeat for liveness; squad batch carries visuals.

## Baseline (RED) results - 2026-06-16

Ran `dev_cycle.ps1 -Scenario squad_spawn_sync`. Confirmed RED, and surfaced a
prerequisite blocker:

- **Wire layer RED as expected.** The join's `SCENARIO RECV` lines are all regular
  host NPCs; no squad members are streamed (the host's own squad of 2 never
  appears). Both clients log `SCENARIO RESULT FAIL`.
- **Blocker: `spawnIntoPlayerSquad` returns 0.** Host logs
  `CHECK host_spawned_count FAIL expected=3 actual=0` - all three spawns failed, so
  the scenario has no members to stream and logs no `SCENARIO MEMBER`. This must be
  fixed first (see M0); the spawn-into-squad facade is the scenario's precondition.
- **Harness robustness issue.** When a scenario fails fast and self-exits, the game
  windows close before the screenshot anchor (`No SCENARIO MEMBER within 90 s`),
  and `run_test.ps1` then hangs trying to screenshot dead windows. Fix: anchor the
  capture on `SCENARIO RESULT` (or detect process exit) instead of a fixed 90 s
  wait. Tooling-only, but it blocks unattended runs.

## Implementation milestones

Each milestone is independently testable. Do them in order.

### M0 - Fix `spawnIntoPlayerSquad` (scenario precondition)

- Investigate why `spawnIntoPlayerSquad` (ScenarioApi facade in `KenshiCoop.cpp`,
  backed by `g_spawnFn` / `g_recruitFn`) returns 0. Likely candidates: the spawn
  function pointer didn't resolve, the recruit overload pick is wrong, or
  spawn-then-recruit ordering. Add a one-line failure reason to the facade so the
  scenario log says *why* (no fn, spawn null, recruit failed).
- Also harden `run_test.ps1` to anchor the screenshot on `SCENARIO RESULT` and to
  not hang when windows have already exited.
- Validate: `squad_spawn_sync` logs `CHECK host_spawned_count PASS` (members exist),
  even though the `CROSSCHECK` stays RED until M1.

### M1 - Capture & stream own squad (wire layer; turns `squad_spawn_sync` GREEN)

- Add `publishSquadState(gw)`: enumerate the **entire** `playerCharacters` lektor
  (not just `[0]`), fill each via the existing `guardedReadNpc`, and send as a
  `PKT_SQUAD_STATE` batch tagged with our `localId`. Run it on **both** host and
  join (bidirectional), at the same ~20 Hz cadence as `publishNpcStates`.
- Receiver: extend `receiveNpcStates` (or a sibling `receiveSquadState`) to drain
  the squad batch. The existing `SCENARIO RECV` logging already covers it, so
  `CROSSCHECK [host->join]` should pass once the host streams.
- Validate: `run_test.ps1 -Scenario squad_spawn_sync`. Expect host+join
  `SCENARIO RESULT PASS` and `CROSSCHECK ... True`.

### M2 - Bidirectional authority de-confliction (turns `squad_move_sync` GREEN)

- Maintain `g_remoteOwnedHands` (set of `HandKey`), populated from received
  `PKT_SQUAD_STATE` entries keyed by `ownerId`.
- In `publishNpcStates`, exclude any object whose `hand` is in
  `g_remoteOwnedHands` (in addition to the existing own-squad `isPlayer` skip), so
  the host never re-streams the join's squad as an NPC.
- Ensure each peer streams its squad regardless of host/join role (M1 already runs
  on both). Validate: `run_test.ps1 -Scenario squad_move_sync` - expect
  `CROSSCHECK [join->host] squad_positions_match True`.

### M3 - Apply to resolvable members (visual layer for shared-save units)

- For each received squad entry, resolve `hand`->local `Character` via
  `guardedHandToChar`. If it resolves (shared save member), drive it with the
  existing `updateNpcs` regimes (kinematic when moving, task at rest, v4 movement
  mirror). This is a near-zero-cost reuse: squad members become "remote-owned
  NPCs" in `g_npcs` with an `ownerId` tag.
- Guard: never enroll our **own** squad hands as driven ghosts (we simulate them).
- Validate: burst frames - a peer that moves a *pre-existing save* squad member is
  seen moving on the other client; `resolvedOk` counter increments.

### M4 - Spawn lifecycle for unresolved members (visual layer for spawned units)

- Problem: units a peer spawned at runtime (e.g. the scenario's
  `spawnIntoPlayerSquad`, or in-game recruits) have **no local `Character`** on the
  observer, so `guardedHandToChar` returns 0 (`resolveFail`). `CROSSCHECK` already
  passes (wire), but nothing renders.
- Fix: when a remote-owned hand fails to resolve for N consecutive frames, **spawn
  a local proxy** (reuse `guardedSpawn` / the Phase 1 ghost machinery), key it by
  the remote `hand`, and drive it via `updateNpcs`. On owner timeout / despawn,
  destroy the proxy (this is the "NPC spawn/despawn lifecycle" gap from the
  post-mortem, scoped to player squads first).
- Open question: appearance fidelity of the proxy (random vs. streamed appearance)
  - acceptable to start with a generic body; stream appearance later.
- Validate: `squad_spawn_sync` visually - the host's 3 spawned units appear on the
  join (as proxies) at the logged offsets; burst frames confirm pose/idle.

### M5 - Retire the player spawn-ghost path / cleanup

- Once squads ride the NPC pipeline (M3 + M4), remove the Phase 1 remote-player
  spawn-ghost special-case for *players* (`g_ghosts` driven from
  `PKT_PLAYER_STATE`), keeping only the minimal presence heartbeat.
- Reconcile `g_ghosts` vs `g_npcs`: a single driven-entity table tagged by owner
  is cleaner than two.
- Validate: full regression - both scenarios GREEN, NPC metrics unchanged, manual
  two-client session stable.

## Risks & open questions

- **Spawned-unit identity churn.** A runtime-spawned unit's `hand` exists only on
  the spawner; the observer must proxy it (M4). Confirm spawned-character hands are
  stable across the session (they should be once created).
- **Authority races.** If both peers ever touch the same hand (shouldn't happen
  with the ownership rule), define a deterministic winner (owner peer). The
  exclusion set (M2) is what prevents this.
- **Own-squad exclusion correctness.** The current `isPlayer` check compares
  pointers in `playerCharacters`; make sure remote-owned exclusion uses `hand`
  identity (pointers aren't comparable across the squad-vs-NPC pick reliably).
- **Bidirectional rate / bandwidth.** Two squads + NPCs at 20 Hz; squads are small,
  but verify batch counts and datagram sizes after adding `ownerId`.
- **Interpolation.** Still localhost-tested; real-latency interpolation is a
  Phase 6 concern but squad streaming should not preclude it.
- **Proxy appearance.** Generic body first; streaming appearance (race, gear) is a
  later fidelity pass.

## Test matrix (what flips GREEN, when)

| Check | RED now | GREEN after |
| --- | --- | --- |
| `squad_spawn_sync` `CHECK host_spawned_count` | yes | M0 |
| `squad_spawn_sync` `CROSSCHECK [host->join]` | yes | M1 |
| `squad_move_sync` `CROSSCHECK [join->host]` | yes | M2 |
| Resolvable squad member visible/moving on peer | yes | M3 |
| Peer-spawned unit visible on observer | yes | M4 |
| Single unified driven-entity model | n/a | M5 |

## Addendum: Co-op Drive Refoundation outcome (validated on `sync`)

M1-M5 proved the wire + the visual drive on a shared save. The refoundation closed
the remaining authority/animation gaps and corrected two assumptions:

- **Shared save is mandatory; distinct saves are a dead-end.** Identity is
  resolve-by-`hand`, which only matches when both clients load the identical save.
  `manual_session.ps1` now flags `-JoinSave != Save` as unsupported and steers to
  `-Inhabit`. `CROSSCHECK` proves the wire; the `sync` bar save proves the visual
  layer (sit/idle poses + tight `gap` across clients).
- **Ownership partition fixes "frozen leaders" (the M2 exclusion taken further).**
  On a shared save each client claimed the *whole* squad, so the receive-side
  own-guard (`ownHands`) skipped the peer's members and nobody drove the other's
  leader. Fix: each client owns only a **disjoint subset** and lets the peer's
  subset fall through to resolve-and-drive. Result on `sync`: host streams
  `[rank 0]`, join streams `[rank 1]`, each drives the other's member at `gap≈0`.
- **Ownership key = stable hand-derived rank, not `squadMemberID`.** The engine
  reports `squadMemberID == 0` for every player-squad member (`[memberID: 0,0]` in
  the logs), so it cannot disambiguate. We sort members by save-stable `hand` and
  use the ordinal (0 = leader); identical cross-client and reorder-stable.
  `KENSHICOOP_OWN_INDICES` ("0" / "~0") now selects on this rank (test override).
  This supersedes the "Own-squad exclusion correctness" risk: exclusion is by
  `hand`, and ownership is by hand-rank, not pointer or `isPlayer`.
- **Suppression (`removeFromUpdateListMain`) is NOT the drive foundation.** It
  freezes the movement controller (body renders but stops moving; teleport no
  longer flushes). The driven body must stay on the update list; we keep the
  proven per-frame quiet-the-AI + v4 locomotion-mirror + teleport path
  (`KENSHICOOP_SUPPRESS_AI=0`, default). The suppression branch is retained off by
  default for a future `manualMovement`-based experiment.
- **Tooling lesson:** `-SkipBuild` used to skip deploy too, silently testing a
  stale DLL. Build/deploy are now separate (`-SkipDeploy` explicit) and the plugin
  logs `inhabit ownership = ...; suppressAI=N` at load as a deploy-freshness gate.
