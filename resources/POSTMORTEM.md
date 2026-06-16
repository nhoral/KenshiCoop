# Kenshi Co-op Post-Mortem: Phases 1 & 2

> Status: Retrospective. Written after NPC presence replication (including
> pose/task replication) reached a working state. Grounded in the actual code in
> `src/plugin/KenshiCoop.cpp`, `src/plugin/NetClient.cpp`, and
> `src/netproto/Protocol.h`, not the original charter. Companion to
> `MASTER_PLAN.md` (the charter/vision).

## Current status snapshot

- **Phase 0 - Foundation: DONE.** Plugin loads under RE_Kenshi; deferred
  main-thread command queue (`MainThreadQueue`); ENet host/join handshake with a
  protocol-version check.
- **Phase 1 - Player Presence: MVP DONE, narrower than charter.** Two players see
  each other move as spawned ghost characters over a 20 Hz unreliable
  `PKT_PLAYER_STATE`. Squad (plural), interpolation buffer, own-squad prediction,
  and player pose/animation were not delivered.
- **Phase 2 - NPC Presence: DONE (the NPC half).** Interest-managed (~200u, up to
  128 NPCs), transform + task/pose streamed at 20 Hz; the client drives NPCs
  kinematically when moving and reproduces the host's task (sit/operate at the
  same fixture) at rest, with a drift guard. Roughly 7/9 NPCs land at 0.0-0.2m
  with correct poses. The charter's "combat" half of Phase 2 is untouched.

## Phase 1 retrospective (player presence)

### What worked

- **Ghost = a real spawned `Character`.** Representing a remote player with an
  actual Kenshi `Character` (via `guardedSpawn` -> `createRandomCharacter`)
  rather than a bespoke renderable means the engine animates and renders it for
  free.
- **SEH-guarded engine calls.** Every spawn/teleport/move runs inside
  `__try/__except`; a bad call disables the feature and logs instead of crashing
  the game. This stability-first posture paid off repeatedly.
- **Dual-regime mover.** `guardedSetDestination` at RUN speed when moving (so real
  walk/run animation plays) and `guardedPark` (halt + teleport) when stopped,
  with hard-snap past a large gap. Cheap and plausible at LAN latency.
- **20 Hz unreliable transport** for `PKT_PLAYER_STATE` is sufficient for
  co-located play.

### Where we diverged from the charter

`MASTER_PLAN.md` Phase 1 promised "two player-**squad** transforms + **animation**
state with **interpolation**; **local prediction** for own squad." In reality:

- **Single character, not squad.** `publishLocalState` sends only
  `playerCharacters[0]`; the rest of the squad is never replicated.
- **No interpolation buffer.** `processInbound` collapses to the latest packet
  per player; smoothing is per-frame ease/extrapolation (`LEAD=1.5`), not a
  timestamped ~100ms render-delay buffer. Fine on LAN; will show artifacts under
  real jitter.
- **No client-side prediction** for the own squad.
- **No player pose/animation on the wire.** Only heading + incidental
  locomotion; a stationary remote player has no rest-pose sync.

### Lesson

The charter's "interpolation + prediction for own squad" language assumed a
generic netcode model. In **our** model each client locally simulates its own
squad and the host is authoritative for everything else, so prediction and
reconciliation of the *own* squad is essentially moot. The important realization:
**the other player's squad is "remote NPC-like" state** and should ride the same
machinery we later built for NPCs - not a separate spawn-ghost path.

## Phase 2 retrospective (NPC presence)

### What worked

- **`hand`-based cross-process identity.** A Kenshi `hand` (its five fields) is
  save-stable and identical across machines that load the same save, so the
  client resolves a streamed `hand` back to its own local `Character`. This is the
  cornerstone of the entire approach.
- **Drive the real local character, don't puppet.** The client suppresses/moves
  the actual world NPC (`guardedHandToChar` -> drive), and on timeout simply hands
  it back to local AI rather than destroying it. NPCs are real inhabitants, not
  spawned puppets.
- **Reproduce intent, not transforms, at rest.** Streaming the host NPC's
  `TaskType` + subject `hand` and calling `setCurrentAction` at the host's fixture
  is what finally produced correct sit/operate poses *and* tight position
  together. This was the breakthrough after transform-only approaches failed.
- **Adaptive drift guard.** `GRACE_MS=4000`, `DRIFT_MAX=4m`, `taskBad`: a
  reproduced task that the local AI re-routes to a different object gets abandoned
  and falls back to position-hold. Bounds worst-case divergence without
  hand-tuning per task.
- **Automated host+join test harness.** Build -> deploy -> launch both clients ->
  screenshot -> parse logs, driven by `KENSHICOOP_SAVE/_TEST_SECONDS/_LOG`. Made
  empirical, screenshot-and-metrics iteration fast.

### What it cost

The rest-pose problem churned through roughly five movement-based regimes
(rest-release, soft position lock, lock-detection heuristic, fully hands-off, and
suppress+hold) before the right fix emerged. **We were trying to solve a pose
problem with movement tools.** The fix was to replicate the AI *task*, then let
the local engine animate it. Recognizing "this is an intent problem, not a
transform problem" earlier would have saved several iterations.

### Open gaps

- **Seated NPCs ignore teleport.** When a host-static seated machine-operator's
  client copy seats itself at a *different* fixture, the drift-guard fallback
  can't pull it back because the engine ignores teleport while seated (the 2
  remaining outliers).
- **No combat / KO / death / lying / ragdoll** replication.
- **No NPC spawn/despawn lifecycle.** Host-only NPCs simply fail to resolve
  (counted as `resolveFail`); nothing is spawned or destroyed.
- **No exact animation-frame streaming.** We reproduce the task and trust the
  local engine to animate; we do not stream the host's precise animation state.

## Doctrine (reusable design rules)

1. **Identity via `hand`** for every replicated entity. It is the stable key that
   makes "drive the real local object" possible across processes.
2. **Reproduce intent (task) for resting/interacting entities; drive transforms
   only for in-motion entities.** Transforms are right for locomotion; tasks are
   right for poses and interactions (sit, operate, and likely combat and jobs).
3. **There is no determinism, so always keep an authority drift-guard / snap.**
   Local AI will diverge; detect it and correct or abandon.
4. **The test harness is a force multiplier.** Keep investing in it - especially
   automated gap metrics and multi-scenario coverage - because visual + numeric
   feedback is what made the pose work tractable.
