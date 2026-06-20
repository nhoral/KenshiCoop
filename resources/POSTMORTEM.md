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
  128 NPCs), transform + task/pose + **locomotion state** streamed at 20 Hz; the
  client drives NPCs kinematically when moving and reproduces the host's task
  (sit/operate at the same fixture) at rest, with a drift guard. Roughly 7/9 NPCs
  land at 0.0-0.2m with correct poses. Resting NPCs now animate a correct,
  advancing idle - "walk-in-place" and the "frozen/floating" artifact were fixed
  by **movement-state mirroring (Protocol v4)**, see the addendum below. The
  charter's "combat" half of Phase 2 is untouched.

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
  screenshot (incl. 5-frame bursts for animation) -> parse logs, driven by
  `KENSHICOOP_SAVE/_TEST_SECONDS/_LOG`. Made empirical, screenshot-and-metrics
  iteration fast.
- **Compiled scenario harness** (`Scenario.h/.cpp`, `ScenarioApi.h`). Deterministic,
  time-gated state machines that run in `mainLoop_hook` on BOTH clients once
  gameplay is live, selected by `KENSHICOOP_SCENARIO`. A scenario calls only the
  `ScenarioApi` facade (spawn/teleport/move/read), so all game mutation stays
  SEH-guarded on the main thread. The host emits `SCENARIO MEMBER hand= pos=`, the
  observer emits `SCENARIO RECV` (from `receiveNpcStates`), and `run_test.ps1`
  matches them by `hand` within a tolerance (`CROSSCHECK`). This turns "is squad X
  replicated?" into an automated RED/GREEN test instead of a visual judgement.

### Animation fidelity: from task-coaxing to movement-state mirroring (v4)

Once positions were tight, resting NPCs still "walked in place" (a walk clip that
never translates), and an aggressive fix left them frozen/floating. Two dead-ends
and the fix:

- **Dead-end A - coax the local AI into idle.** Clearing goals + issuing
  `STAND_STILL` + ejecting the town package, once per rest, plateaued at ~72-92%
  idle. The local AI kept re-deciding inside the engine's own update and re-issued
  a step every tick, so a body we pinned in place marched. Coaxing intent could
  not reliably win against the AI that re-runs every frame.
- **Dead-end B - `runSlaveAnim` clip replication.** Hypothesis: map `TaskType` ->
  animation clip and force the clip directly, ignoring local AI. A discovery hook
  on `Character::runSlaveAnim` logged **zero** calls across both clients over 45 s
  with 9 sitting/standing/operating NPCs. Idle/sit/stand/operate are **not** slave
  animations - `runSlaveAnim` is for scripted one-shots. And `AnimationClass` is
  opaque (forward-declared everywhere, no definition), so reading the host's exact
  clip+phase was not viable either.
- **The fix - mirror the movement state (Protocol v4).** `CharMovement.h` shows the
  engine's `AnimationClass` selects walk/idle/run from `currentlyMoving` /
  `currentSpeed` / `currentMotion`. The host now streams those three; the client
  writes them onto the local copy's `CharMovement` as the LAST thing each frame
  (after `g_mainLoop_orig`), so they are what the renderer samples - the local AI
  recomputes them next tick, but our end-of-frame write wins for the displayed
  frame. Crucially we stopped calling `halt()` every frame: `halt()` resets the
  clip to frame 0 (that was the "frozen" artifact) and teleport-after-physics
  caused the "floating". Now we settle once with a clean stop, then only no-halt
  teleport on >1 m drift, so the idle clip advances. Result: every resting NPC
  reads `mv=0/spd=0` (walk-in-place eliminated) at tight positions, no freeze/float.

### What it cost

The rest-pose problem churned through roughly five movement-based regimes
(rest-release, soft position lock, lock-detection heuristic, fully hands-off, and
suppress+hold) before the right fix emerged. **We were trying to solve a pose
problem with movement tools.** The fix was to replicate the AI *task*, then let
the local engine animate it. Recognizing "this is an intent problem, not a
transform problem" earlier would have saved several iterations.

### Open gaps

- **`OPERATE_MACHINERY` operators that don't resolve a fixture stand idle.** When
  the streamed subject `hand` (the specific machine) doesn't resolve to a local
  `RootObject`, the operator falls back to `STAND_STILL` - so it stands where it
  should be hunched/operating. With the v4 movement mirror these are now at the
  right place (`gap <= ~1 m`); it is a *pose* fallback, not the old 18 m drift.
  Roughly 2 of 4 operators hit this in the test save.
- **No combat / KO / death / lying / ragdoll** replication.
- **No NPC spawn/despawn lifecycle.** Host-only NPCs simply fail to resolve
  (counted as `resolveFail`); nothing is spawned or destroyed.
- **No precise animation-phase streaming.** We mirror the host's *locomotion
  state* (speed/motion/moving) so the engine picks the same walk/idle/run clip,
  but we do not sync the exact clip nor its phase - `AnimationClass` is opaque, so
  e.g. two idlers may be at different points in the same idle loop. Good enough for
  locomotion; insufficient for scripted one-shots (eating, gestures).

## Doctrine (reusable design rules)

> Forward pointer: these rules are generalized into the project's go-forward
> design in `INTENT_REPLICATION.md` (the "replicate causes, not effects" framework
> and the per-class lever-set + conformance-oracle model that Phase 3+ builds on).

1. **Identity via `hand`** for every replicated entity. It is the stable key that
   makes "drive the real local object" possible across processes.
2. **Reproduce intent (task) for resting/interacting entities; drive transforms
   only for in-motion entities.** Transforms are right for locomotion; tasks are
   right for poses and interactions (sit, operate, and likely combat and jobs).
3. **There is no determinism, so always keep an authority drift-guard / snap.**
   Local AI will diverge; detect it and correct or abandon.
4. **The test harness is a force multiplier.** Keep investing in it - especially
   automated gap metrics, burst-frame capture, and the compiled scenario harness
   (RED/GREEN `CROSSCHECK`) - because visual + numeric feedback is what made the
   pose work tractable.
5. **Locomotion animation is a function of movement state, not a paintable clip.**
   The engine's `AnimationClass` selects walk/idle/run from
   `currentlyMoving/currentSpeed/currentMotion`. To make a driven copy animate
   like its source, mirror those scalars (as the last write of the frame) and let
   the engine's own selector animate; never `halt()` per frame (it resets the clip
   phase). Reserve clip-level control for scripted one-shots, which is a separate,
   harder problem (`runSlaveAnim` + opaque `AnimationClass`).
6. **Validate replication with a deterministic in-game scenario, not just eyes.**
   A scripted host action + a cross-client `hand`->position match within tolerance
   makes "is this entity class replicated?" a repeatable pass/fail.

## Addendum: Co-op Drive Refoundation (shared-save inhabit + ownership partition)

Written after re-baselining squad sync on the master plan's shared-save model and
validating on the `sync` bar save (burst screenshots + `npc[...]` diag logs).

### What we confirmed

- **Shared save is mandatory for v1.** NPC (and squad) identity is resolve-by-
  `hand`; identical hands only exist when both clients load the *same* save. The
  earlier "distinct-save" experiment (each client its own save) mints different
  hands and is therefore a dead-end - it is now flagged unsupported in
  `manual_session.ps1`. On the shared save, NPC sync lands at `gap 0-1 m` with
  matching poses across clients (validated visually + in logs).
- **The "frozen leader" bug was an authority gap, not a sync regression.** On a
  shared save both clients load the *same* squad, so each was claiming every
  member and the receive-side own-guard skipped them -> neither drove the other's
  leader. The fix is an **ownership partition**: each player OWNS a disjoint
  subset of the shared squad (locally controls + streams it) and DRIVES the
  peer's subset from their stream via the existing resolve-and-drive path. With
  the partition, host streams its member and drives the guest's, and vice-versa -
  no frozen/missing leaders.
- **Ownership identity: hand-derived rank, NOT `squadMemberID`.** We intended to
  key ownership on `Character::squadMemberID` (resources §5.4), but the engine
  reports `squadMemberID == 0` for *every* member of the player squad (verified in
  the inhabit logs: `[memberID: 0,0]`), so it cannot disambiguate members. The
  working key is a **stable hand-derived rank**: sort the squad's members by their
  save-stable `hand` and use the ordinal (0 = leader). Both clients load the
  identical shared squad, so the ranks match cross-client and survive list
  reordering. The `KENSHICOOP_OWN_INDICES` knob ("0" / "~0") now selects on rank.

### What did NOT work: `removeFromUpdateListMain` as the drive foundation

The refoundation hypothesis was to suppress a driven entity by pulling it out of
the engine's main AI update list (`GameWorld::removeFromUpdateListMain`, resources
§5.1) instead of fighting the AI every frame. **Empirically it freezes the body.**
With suppression on, a moving NPC's `actual` position stayed *byte-identical*
while its target moved 480 m away (`sup=1`, `gap=479`): the body still *renders*,
but `_setPositionDirectionAndTeleport` no longer flushes to the live transform
because the controller's per-tick step is gone. **The teleport/kinematic drive
REQUIRES the body to remain in the update list** so the controller applies the
write. We therefore keep the proven per-frame approach (quiet the AI via
`clearGoals`/`neutralize`, mirror locomotion v4, teleport while ticked) as the
live path, behind `KENSHICOOP_SUPPRESS_AI` (default `0`); the suppression branch
is retained off by default for a future experiment that pairs it with
`CharMovement::manualMovement` (velocity drive) instead of teleport.

### Movement animation: walk the body, don't teleport-slide it

A second pass (after user feedback that driven NPCs *floated* during movement)
found the moving-NPC drive still teleported the body to the host position every
frame (`driveNpcKinematic` -> `_setPositionDirectionAndTeleport`) and only
*mirrored* the locomotion scalars. Because `updateNpcs` runs AFTER `g_mainLoop_orig`,
the engine had already computed this frame's animation from the AI's (idle)
locomotion state before our mirror write landed - so a teleported body that never
actually walks renders a static/idle pose sliding across the floor (the "float"),
and the mirror is always one tick too late to fix it.

Fix (`KENSHICOOP_WALK_DRIVE`, default on): for a MOVING body, issue
`setDestination(hostPos)` at a gap-proportional catch-up speed (`setDesiredSpeed`)
instead of teleporting. The engine then genuinely WALKS the body on its next tick -
grounded, real walk cycle, `currentlyMoving/currentSpeed` set by the engine itself
(verified: `mv=1 spd≈15` with NO mirror) - so the clip is correct without any
mirror hack. A large gap (>8 m, e.g. the body fell behind a fast runner) still
hard-snaps via teleport. Cost: position is looser while moving (~0-6 m chase lag
vs ~0-2 m for teleport) because it walks rather than warps; the catch-up speed
bounds it and the snap covers the worst case. Resting bodies are unchanged (pose
via task + settle once + no-halt). The old teleport-kinematic mover remains behind
`KENSHICOOP_WALK_DRIVE=0` for tight-position-over-animation needs.

### Doctrine added

7. **Drive replicated bodies while they stay ON the engine update list.** A direct
   position write (`_setPositionDirectionAndTeleport`) only reaches the renderer
   because the movement controller's per-tick step flushes it. Removing the body
   from the update list to "cleanly" suppress AI also removes that step, freezing
   the body (it still renders, so a single screenshot hides it - the log's
   per-entity `gap` over time is what exposes it). Quiet the AI in-place instead.
7b. **For MOVING bodies, drive locomotion (`setDestination`), don't teleport.**
   Teleporting a body the engine isn't actually walking slides a static pose
   ("float"), and mirroring locomotion scalars can't fix it because our post-tick
   write lands after the frame's animation was computed. Let the engine walk the
   body so it animates itself; reserve teleport for at-rest settle and large-gap
   snaps. Validate movement animation across BURST frames (leg/pose change between
   frames) + engine-real `mv/spd`, not a single still (which can't show a slide).
8. **Co-op v1 is shared-save + ownership partition.** Both clients load the same
   save; each owns a disjoint, hand-ranked subset of the shared squad, streams its
   own, and drives the peer's. Distinct saves break resolve-by-`hand`.
9. **Never validate an undeployed build.** `manual_session.ps1 -SkipBuild` used to
   skip *deploy* too, so tests ran the old DLL. Build and deploy are now separate
   (`-SkipDeploy` is explicit) and the plugin logs `inhabit ownership = ...;
   suppressAI=N` at load, which the runner watches for before trusting a result.

## Addendum: Seated & standing task-pose sync (the sit/stand breakthrough)

Written after the bar-scene sit/stand sync reached a user-confirmed "really good"
state on the `sync` save. This closes the largest remaining open gap from the
Phase 2 retrospective: NPCs that the host has *sitting on a specific stool* or
*standing at a specific node* now reproduce that pose at the right place on the
guest, instead of standing/marching-in-place. Grounded in `Replicator.cpp`
(`applyRest`), `Engine.cpp` (`applyTaskOrder` / `detachFromTownAI` / `endAction`)
and the iteration log in `tools/test-runs/POSE_IDEAS.md` (I0-I11).

### The two sub-problems (they are NOT the same fix)

The "interacting NPC" pose splits into two distinct cases that needed *opposite*
handling, and conflating them is what cost the most iterations:

- **Sitters** (`SIT_AROUND`, task 87) - need a *persistent, location-bound order*.
- **Standers** (`STAND_AT_NODE`, task 51) - need their *residual action ended*, and
  must be left attached to their squad/town-AI.

### What worked - sitters: detach from town-AI + a player-style ORDER

The winning path (I9) is, on first rest for a sitter:

1. **Detach the NPC from its town-AI platoon** via
   `Character::separateIntoMyOwnSquad` (`engine::detachFromTownAI`). This stops the
   town package from continuously re-assigning it a *new* seat search.
2. **Issue a player-style order**, not an AI goal: `Character::addOrder`
   (fallback `addJob`) with an explicit **destination building + location**
   (`engine::applyTaskOrder`). A player order *honors the specific fixture*; the
   body walks to and sits on the host's exact stool.

Result: every seated NPC sits on the correct stool, **0 drift-abandons**
(user-confirmed visually, then reconfirmed on `sync`).

**Why it worked:** the seat target was always available - the failure was that the
*goal* layer refused to use it (see "wander goal" below). Detaching removes the
competing town-AI intent; the player order is the one intent left, and it is the
one kind of intent that respects an explicit location.

### What worked - standers: end the residual action, do NOT detach

Standers are AI-suspended but keep *executing* a half-finished "walk-to-node"
action, so pinned in place they march. The fix (I10b + I11):

1. **`CharBody::endAction`** (`engine::endAction`) once on park - drops the
   suspended body from "walking to node" to idle.
2. **Re-quiet on relapse (I11).** `endAction` once is not enough for every stander:
   some *re-acquire* a walk action after settling and march again. So each tick, if
   a parked body actually reports a walk motion while we hold it stationary (the
   precise march signature, via `engine::readMotion`), re-issue `endAction`. A
   genuinely idle body is never touched, so its idle clip is never reset to frame 0.
3. **Crucially, do NOT detach standers.** (See the I10 failure below.)

Result: standers idle at the correct spot; the residual marchers are gone.

### What did NOT work (and the precise reason)

- **`addGoal(SIT_AROUND, seat)` - a "wander goal", not a seat assignment.**
  Empirically `SIT_AROUND` ignores the target fixture and re-searches for *a*
  nearby seat. On the guest it resolved to a *far* stool, the body walked 50+ m,
  and the drift guard abandoned it standing (the `753417984` holdout, I0-I6). This
  is why we moved off goals to player **orders** for sitters.
- **`_NV_setCurrentAction(SIT_AROUND)` (I7).** Same wander behaviour as `addGoal` -
  the action layer also re-searches for a seat and ignores the target. Made things
  *worse* (more wanderers). Reverted.
- **Snap-then-pose (I8).** Teleport the body onto the seat *before* committing the
  task. **Crashed the client.** Reverted.
- **Detaching STANDERS (I10).** Applying the sitter recipe (detach) to standers
  made them go **ABSENT** on the guest. Two compounding reasons: (a)
  `separateIntoMyOwnSquad` changes the NPC's **container**, and the cross-client
  identity key (`keyOf`) includes `container`/`containerSerial` - so the detached
  NPC no longer matches the host's streamed `hand`, and `enforceHostAuthority`
  suppresses it; (b) a stander gets no replacement intent (unlike a sitter's
  persistent order), so once in its own squad it just wanders off. Sitters survive
  detach *only because* the immediately-following persistent sit order re-anchors
  them. Fix: `endAction`-only for standers, no detach.
- **Releasing node-anchored NPCs to local AI (I4).** Freed AI wandered them off the
  host position (CROSSCHECK regression). Reverted.

### Doctrine added

10. **"Interacting" pose is two problems: seat-bound and node-bound. Treat them
    separately.** A persistent, location-honoring *order* fixes the first; *ending
    the residual action* fixes the second. The same lever applied to both
    backfires.
11. **Use player ORDERS (location-bound), not AI GOALS, when a specific fixture
    matters.** `addGoal(SIT_AROUND)` / `setCurrentAction(SIT_AROUND)` are
    *wander* intents that re-search for any nearby seat and ignore the target;
    `addOrder`/`addJob` with an explicit destination+location honor the exact one.
12. **Detaching from town-AI (`separateIntoMyOwnSquad`) changes the entity's
    container, which changes its cross-client `hand` identity.** Only detach an
    entity you immediately re-anchor with a persistent intent (a sitter's order),
    and only when its identity break is acceptable. Never detach an entity you
    still need to match host-side by `hand` (a stander), or it goes ABSENT
    (identity mismatch -> host-authority suppression) and/or wanders (no
    replacement intent).
13. **Quieting the AI is not the same as stopping the current action.** An
    AI-suspended body still *executes* a half-finished action (walk-to-node), so it
    marches in place when pinned. End the action explicitly, and re-end it when the
    body relapses into a walk motion (`readMotion` is the relapse detector); never
    blanket-`endAction` every frame or you reset the idle clip phase.

### Where this leads

The sit/stand result is the second proof (after the v4 locomotion mirror) of a
single principle - **replicate the causes of an animation, let the local engine
produce it, quiet the AI per-class, and guard drift**. That principle is the spine
of the go-forward plan: `INTENT_REPLICATION.md` generalizes it into a layered model
(L0 identity -> L5 combat), a `(behavior class -> lever-set)` taxonomy, the wire
implications (a body-state field + a reliable `PKT_EVENT` channel), and a reusable
per-class conformance oracle - with crafting/gathering as the next proof case. The
re-prioritized roadmap lives in `MASTER_PLAN.md` (Phase 3a/3b/3c).

## Addendum: Bidirectional per-tab ownership (the true-co-op keystone)

Written after the bidirectional ownership partition reached a user-confirmed
working state on the `squad1` save - validated by the automated `coop_presence`
oracle (0 ms + WAN sim) AND by manual control (each player drives only their own
squad tab; both sides mirror cleanly). This is the milestone that turns "host
streams a world; guest watches" into "two players, each authoritative over their
own squad."

### The progression

1. **Host-only authority** (Phase 2): the host streamed everything; the guest only
   rendered. No guest agency.
2. **Leader-only ownership partition** (Phase 2.5): each client OWNS a disjoint,
   hand-ranked subset and DRIVES the peer's, but the partition was keyed on a single
   hand-rank ("leader") - coarse, and not how Kenshi players actually organize.
3. **Per-tab bidirectional ownership** (Phase 3.5, this addendum): ownership is
   partitioned by **Kenshi squad tab**, which maps cleanly to how players think
   about "my squad" vs "your squad".

### What worked

- **A squad tab IS the member's `hand` CONTAINER.** A Kenshi squad tab is a
  `Platoon`; a player member's tab identity is its `hand` container/containerSerial.
  So "partition by tab" = "partition by distinct container", and container is
  already part of the save-stable `hand` - no new identity needed. `publishOwned`
  captures the whole squad, extracts the unique `(hContainer, hContainerSerial)`
  pairs, sorts them into a stable **container rank**, and keeps only members whose
  container rank is in this client's `ownRanks_`. The ranks match cross-client
  because both load the identical shared save.
- **Run the partition on BOTH clients; keep only world-NPC suppression one-sided.**
  `publishOwned` (capture+stream my tabs) and `applyTargets` (drive the peer's tabs)
  now run on both host and join. Only `enforceHostAuthority` (suppress/restore
  world NPCs the host is/ isn't streaming) stays join-only, because world NPCs
  remain host-authoritative. This symmetry is what gives the guest real agency
  without a second authority for the world.
- **A drive-exclusion guard is mandatory.** `applyTargets` (and
  `enforceHostAuthority`) skip any hand in `ownHands_` (the set this client just
  published). Without it, a client would try to DRIVE its own locally-controlled
  body from the peer's echo - fighting its own input. The own-guard is the same
  idea that fixed the "frozen leader" gap, now applied per-tab on both sides.
- **A programmatic bake makes the 2-tab setup repeatable.** `setupSquadScene`
  recruits two world bodies into the player squad, then `separateIntoMyOwnSquad`s
  ONE into its own platoon (a second tab), and dumps each member's container so the
  bake is verifiable from the log (2+ distinct containers = 2+ tabs). The user SAVEs
  it as `squad1`; both clients load it.

### What we had to get right (and a gotcha)

- **`squadMemberID` is still useless for ownership** (every player member reports
  `0`); container rank is the working key, consistent with doctrine #8.
- **`playerCharacters.size()` can lag a just-separated member.** Right after the
  bake the host logged `playerChars=2` even with two recruits + a separation; the
  authoritative signal is the **distinct-container count** in the member dump, not
  the list size snapshot at that instant. Validate the partition from container
  identity, not list cardinality.
- **WAN micro-slide on near-static bodies is cosmetic, not a presence failure.**
  Under the WAN sim the driven tab-leader advances in brief dead-reckoning snaps, so
  the locomotion-tuned `smoothness`/`anim-truth` fractions spike on the tiny
  translate sample of a mostly-stationary presence test. We made those **advisory**
  for `coop_presence`; the authoritative gates are the bidirectional `COOP-PRESENCE`
  cross-check + `MARCH`. (A future velocity-hysteresis pass can smooth the seam.)

### Doctrine added

19. **Co-op ownership is partitioned by squad TAB (container rank), bidirectionally.**
    Both clients run capture+stream (`publishOwned`) and drive (`applyTargets`); each
    owns a disjoint set of tabs (distinct `hand` containers, stably ranked). Only
    world-NPC suppression stays host-authoritative. This is the v1 "true co-op"
    presence model.
20. **Always exclude your own published hands from your own drive/suppress paths.**
    A bidirectional partition echoes your members back from the peer; without an
    `ownHands_` exclusion guard a client drives its own body from that echo and
    fights its own input.
21. **Verify a squad/tab bake by distinct-container identity, not list size.**
    `playerCharacters.size()` can momentarily lag a just-separated member; the count
    of distinct `(container, containerSerial)` pairs is the trustworthy tab count.
22. **Gate placement/state tests on placement metrics; make locomotion-quality
    metrics advisory there.** A mostly-static presence test shows latency micro-slide
    under WAN that a locomotion oracle reads as a failure - judge it on the
    cross-check, not on smoothness.
23. **Replicate runtime-created CONTENT by container snapshot + local reconstruction,
    not by item `hand`.** Items minted at runtime (craft output, loot) get host-only
    `hand`s that don't resolve on the peer - the same reason we bake saves. Stream the
    container's contents as a description (template `stringID` + type + qty + quality)
    keyed by the container's stable `hand`, gate the reliable send on a content hash,
    and let the peer reconstruct to match (idempotent, loss-tolerant). The client never
    reconciles a container it authors (host-authoritative). This is doctrine 14
    ("replicate causes, not effects") one layer up: the container is the cause.
24. **A move ACROSS an ownership boundary is a non-atomic two-authority transaction -
    the snapshot model cannot make it safe; route trades through a shared authority.**
    Per-container convergence is correct only when ONE client authors each container.
    A cross-squad item drag touches two containers owned by two different clients: the
    source removal (your owned container) propagates authoritatively, but the
    destination ADD lands in a container the OTHER client authors - its next snapshot
    lacks the item and DELETES it (net LOSS). The mirror case (dragging OUT of a
    peer-owned container) DUPLICATES. There is no per-container fix; the transaction has
    no single owner. The sanctioned design is to route transfers through a shared
    authority - the host-authoritative WORLD: DROP (remove from my owned inventory ->
    spawn a ground item, host-owned) then PICKUP (remove the ground item -> add to my
    owned inventory). Each leg is single-owner end-to-end, so the race disappears. This
    is the inventory analogue of doctrine 8 (disjoint ownership) and the
    host-authoritative-world invariant. The supported (owned-container) path is proven
    loss-free both ways by `inv_bidir`; cross-squad trade is deferred to 4b.

## Addendum: Inventory / container-contents (Phase 4a)

Written after `inv_order` reached a green automated state on `squad1` - host live-add,
host/join FINAL content-hash match, at 0 ms and WAN (80 ms +/-30 ms, 30% loss), with
`npc_sync`/`combat_kill`/`coop_presence` regressions still green.

### What worked

- **Content-snapshot/reconcile beats hand-driving for items.** Save-baked items carry
  a stable `hand`, but crafting/loot mint NEW items whose hands are host-only and
  unresolvable on the join. Driving item objects by hand was a dead end; streaming the
  CONTAINER's contents (keyed by the container's stable hand) and reconstructing items
  locally (`createItem`+`tryAddItem` for shortfalls, `removeItemAutoDestroy` for
  excess) is idempotent, loss-tolerant, and host-authoritative.
- **Reliable channel + hash gate keeps it cheap and correct.** `publishInventories`
  only enqueues a snapshot when the container's order-independent content hash changes
  (plus a 5 s safety resend); it rides the reliable channel, so a content change
  survives the 30% loss the WAN sim injects (proven: the add lands on the join under
  loss). `applyInventories` reconciles only on a freshly-received (dirty) snapshot.
- **Anchor on the leader's inventory, not a spawned chest (v1).** Many Kenshi storage
  buildings are resource-limited and would reject a seeded general item - a real bake
  risk. The leader's own inventory exists in every save, resolves cross-client by its
  character `hand`, and accepts arbitrary items, so `inv_order` validates against ANY
  squad save (no dedicated bake needed). A general-purpose storage-building anchor is
  the natural follow-up.

### What we had to get right (and two gotchas)

- **The verdict must be robust to the launch stagger.** The join loads ~10 s after the
  host, so it can start sampling AFTER the host's add already propagated - it then
  never witnesses the 0->1 edge itself. Gating on "join observed the change" or "join
  count grew" falsely fails. The stagger-robust proof is: host performed a live add +
  host content actually changed (first hash != last) + host/join FINAL hashes match +
  join ends non-empty (the save's leader baseline is empty, so a matching non-empty
  multiset proves cross-client reconstruction). "Join saw the transition" is advisory.
- **Oracle regexes must not anchor at line start.** Log lines carry a
  `[ts] [ROLE] INFO:` prefix; an `^SCENARIO ...` anchor silently matched zero lines.
- **Identical equality via a shared hash.** Host and join compute the SAME
  `invEntryHash` over `(stringID,type,qty,quality)`, summed order-independently, so
  "equal hash == equal multiset" is a sound cross-client content check.

## Addendum: Bidirectional inventory + the cross-squad-trade decision (Phase 4a -> 4b)

Written after `inv_bidir` reached green at 0 ms and WAN (120 ms +/-40 ms, 30% loss),
with `inv_order`/`coop_presence` regressions still green.

### What worked

- **Bidirectional fell out of the existing partition for free.** The replicator already
  computes `ownHands_` (the squad members we own, by tab-rank) every tick for the
  positional drive-exclusion guard. Publishing the inventory of every container in that
  set - instead of a host-only single registration - makes inventory sync symmetric and
  disjoint by construction (host authors tab 0, join authors tab 1). `applyInventories`
  skipping that same set means no client reconciles a container it authors, both ways.
  No new ownership concept, no new wire type.
- **Gate the publish behind `invSync`.** Once publishing keys off `ownHands_` (always
  populated), ordinary co-op would start streaming inventories for everyone; gating
  `publishInventories` on `invSync` keeps non-inventory sessions silent.
- **The bidirectional oracle needs DISTINCT net deltas per side.** `inv_bidir` has the
  host end +2 and the join end +1 on their owned containers, and checks per-rank
  convergence in BOTH directions. Distinct deltas make the cross-client match
  unambiguous (the two streams can't be coincidentally equal), and the ADD-then-REMOVE
  sequence forces the peer to converge DOWN as well as up - so a removal that failed to
  propagate would leave the peer stuck above the author's count and fail the check.

### The loss the user saw, explained

Manual cross-squad drag lost items "a few times." Root cause is doctrine 24: dragging an
item INTO a peer-owned container is a split transaction - the source removal is
authoritative but the destination add is local-only and gets deleted by the destination
owner's next snapshot. It is not a wire/reconcile bug; it is structural. Hence the 4b
decision to route trades through the world (drop -> pickup) rather than allow direct
cross-authority moves. The owned-container path itself is loss-free (proven by
`inv_bidir`), which is exactly what drop/pickup is built on.

## Addendum: Equipped gear (armour/weapon slots) - the loose-only blind spot (Phase 4a)

Written after `inv_equip` reached green at 0 ms and WAN (80 ms +/-20 ms, 1% loss), with
`inv_bidir`/`inv_order`/`coop_presence` regressions still green. Prompted by a user
observation: dropping a loose item synced, but dropping/unequipping ARMOUR removed it on
the acting client only - the other client kept wearing it.

### What worked

- **Read worn gear from the equipment SECTIONS, not `_allItems`.** The original snapshot
  iterated `Inventory::_allItems` and even had an `if (it->isEquipped) continue;` guard -
  but that guard was DEAD CODE: for a character, worn armour/weapons don't live in
  `_allItems` at all. They live in equipment sections. The snapshot now appends those,
  tagged `equipped=1` + `slot`. (How we found it: every geared character logged
  `count=0 eq=0` - the loose list really is empty for save-loaded gear.)
- **The up path is a MOVE, not a fabrication.** Re-equipping from inventory used to vanish
  because the reconcile destroyed the worn surplus and `create+equipItem`'d a replacement -
  and a fabricated blank-handle item is discarded the moment it enters a slot (d25). The
  fix keys the reconcile by TEMPLATE (not by `(sid,type,equipped)`) and, when only the
  worn/loose SPLIT changed at unchanged total, MOVES the real item: UP `equipItem`s an
  existing loose copy (a real item persists); DOWN detaches it and re-homes it into a loose
  section. The non-obvious trap: `Inventory::tryAddItem` auto-routes an equippable item
  straight back into its slot, so a removed item must be placed via a loose section's own
  `addItem` or it instantly re-equips (the first `inv_reequip` run showed `eq` never dipping
  - the unequip was silently undone). With the section-targeted add, `inv_reequip` shows a
  clean `peak2 dip1 ->2` on author AND observer, both directions.
- **Walk EVERY equip section, don't enumerate slots by getter.** The first cut read worn
  gear via `getEquippedArmour` + `getEquippedWeapons`. Armour replicated, but unequipping a
  WEAPON did nothing on the peer - because the weapon holster is its own equip section that
  `getEquippedWeapons` didn't report, so the weapon was never in the snapshot and unequipping
  it changed nothing to resend. Fix: iterate `Inventory::getAllSections()` and capture every
  section flagged `isAnEquippedItemSection` (tagging each item `equipped=1` + the section's
  `limitedSlot`). One uniform loop now covers ALL equippable slots - every armour piece,
  both weapon slots, belt, and a worn backpack - and any future slot for free, since the
  reconcile already keys on `(stringID,itemType,equipped)`. `inv_equip` confirmed it: the
  geared member's worn count rose from `eq1` to `eq2` (armour + weapon now both captured).
- **`equipped` is part of the reconcile key.** Keying on `(stringID,itemType,equipped)`
  (and folding the flag into the content hash) makes a worn item and a loose copy of the
  same template reconcile INDEPENDENTLY, so unequipping converges the slot, not just a
  count. The peer's down-reconcile (`removeItemAutoDestroy` on its real worn item) is what
  finally removes the armour the user saw stuck on.
- **Stagger-robust test from REAL gear.** Because both clients load the same save, the
  observer's own copy starts wearing the same gear; when the author unequips one item, the
  observer must actively remove its worn copy to converge to "one fewer." That holds no
  matter when the join finishes loading - no need to witness intermediate states (unlike
  the naked-baseline approach, which can't tell a synced-down observer from one that was
  never geared).

### Addendum 2: weapons aren't (only) in sections - the held-weapon blind spot

Walking every `isAnEquippedItemSection` made armour, belt, worn backpack, and the SHEATHED
weapon (it sits in a `hip`/`back` section) replicate. But the user reported that manually
unequipping AND re-equipping a WEAPON still didn't sync either direction, while pants did -
and the automated `inv_reequip` was passing the whole time. A per-member inventory dump
(`engine::dumpInventory`) explained the contradiction:

- The geared member's sections were `hip, back, boots, head, backpack_attach, shirt, armour,
  legs, belt, main` - **there is no dedicated weapon section.** The SHEATHED (secondary)
  weapon happened to live in `hip`, so the section walk captured it and `inv_reequip` was
  actually exercising that weapon (and passing). The test gave false confidence.
- A HELD (primary) weapon is tracked ONLY by `Inventory::getPrimaryWeapon()` /
  `getSecondaryWeapon()`, which are NOT part of `getAllSections()`. Re-equipping a weapon
  from inventory routes it to the held slot, so it left every section the snapshot reads -
  it vanished from the wire and the peer never saw it return. That is the manual failure.

Fix: after the section walk, also read `getPrimaryWeapon()`/`getSecondaryWeapon()` and append
any worn weapon, deduped by `(stringID, itemType)` against the already-captured equipped
entries (the sheathed weapon is exposed BOTH as a section item and via the pointer, and the
two are DISTINCT `Item*` objects, so pointer-identity dedup double-counted it - `inv_reequip`
jumped from `peak2` to `peak3` until the dedup keyed on template instead). With template
dedup, peak returned to `2` and both clients converge.

### Addendum 3: the mid-drag "weapon gone" transient + a publish settle-debounce

Even after the held-weapon capture fix, manually unequipping/re-equipping a weapon STILL
didn't sync, while the automated `inv_reequip` (which drives clean engine moves) kept
passing. Rather than guess, the reconcile was made fully observable - `engine::dumpInventory`
on every SEND/APPLY (env `KENSHICOOP_INV_DUMP=1`), a `[recon]` trace of each branch inside
`applyContainerContents`, and a single-instance diagnostic scenario `inv_wpnseq` that drives
`applyContainerContents` through a crafted snapshot sequence with NO UI. That nailed it:

- The clean two-step (weapon EQ -> weapon LOOSE -> weapon EQ) reconciles perfectly:
  `MOVE-DOWN ok=1` then `MOVE-UP ok=1`. The move logic is correct.
- The real manual capture (`KENSHICOOP_INV_DUMP`) showed the host briefly publishing the
  weapon as FULLY GONE (`_allItems n=0`, every section empty) for ~240 ms - the window where
  the inventory UI holds the dragged weapon on the CURSOR, out of the inventory entirely.
- Feeding THAT into the reconcile (`inv_wpnseq` S2 `[weapon GONE]`) shows the killer:
  `REMOVE-LOOSE got=1` DESTROYS the (real) weapon, and the follow-up re-equip can only
  `CREATE-EQ`, which returns `ok=0` for the weapon (createItemAndAdd fails for this weapon
  template; equipped fabrication is non-persistent anyway - d25). So the weapon is gone for
  good. Clothes survived because they are never reported "fully gone" mid-drag.

First fix attempt (host side): a flat **settle-debounce** in `publishInventories` - a changed
fingerprint had to stay STABLE for `INV_SETTLE_MS` (~350 ms) before being sent. This passed
the automated suite but STILL failed the manual weapon test, and the `KENSHICOOP_INV_DUMP`
trace from that run showed why: a *deliberate* human re-equip holds the weapon on the cursor
for far longer than 350 ms - the captured "weapon FULLY GONE" state persisted **961 ms**
(`12:35:46.571`). The flat window was below the human cursor-hold, so the transient settled
and was published, the peer destroyed its real weapon, and re-equip's `CREATE-EQ` was
unreliable. (Pants never hit this: cloth re-create succeeds, so even a destroy recovers.)

Final fix (the right layer): make the debounce **removal-aware**. The killer transient is
always a count DECREASE (the weapon leaves the inventory for the cursor), and so is a genuine
drop; an equip<->loose move keeps the count. So `publishInventories` tracks `lastSentN` (entry
count of the last SENT snapshot) and applies the long `INV_REMOVE_SETTLE_MS` (1800 ms) window
ONLY when `n < lastSentN`; additions and equip/unequip MOVES keep the fast 350 ms window. A
~1 s cursor-hold is now swallowed, so the peer keeps its real weapon and just MOVES it; only
genuine removals wait out the longer settle (a real drop lags ~1.8 s on the peer - acceptable).
Manually validated: host removals/unequips reflected on the join, weapon included, with the
join trace showing `MOVE-UP ok=1` and **zero** weapon REMOVE ops (the real item was never
destroyed). Automated `inv_reequip`/`inv_equip`/`inv_bidir` stay green.

### Addendum 4: weapon-SLOT fidelity (Weapon I vs Weapon II)

With unequip/re-equip finally syncing, the next manual finding: re-equipping a weapon into the
OTHER weapon slot (unequip from slot 2, re-equip to slot 1) persisted on the acting client, but
the peer put it back in slot 2. Two compounding causes, both rooted in how the engine models
weapon slots:

- A character has TWO weapon sections - `hip` (Weapon II) and `back` (Weapon I) - and BOTH
  carry `limitedSlot = ATTACH_WEAPON (0)`. Armour sections each have a unique `AttachSlot`
  (head=3, legs=6, ...), so `slot` identifies them; the two weapon slots are identical in
  `slot`. The snapshot's `slot` field therefore could not tell Weapon I from Weapon II.
- Consequently the content fingerprint (`invEntryHash`) was IDENTICAL for the same weapon in
  `hip` vs `back` (same sid/type/equipped/slot), so a pure slot move produced NO fingerprint
  change and was never published. The peer only ever saw the loose<->equipped transitions of
  the drag and re-equipped via `equipItem`, which auto-routes to the default slot (`hip`).

Fix (three coordinated parts): (1) repurpose the `InvItemEntry` pad as `section` = a 16-bit
hash of the equip section's NAME (`sectionNameHash`), built identically on both clients, so the
two weapon slots get distinct wire identities; (2) fold `section` into `invEntryHash` so a
Weapon I<->II move now changes the fingerprint and publishes; (3) a SLOT-FIDELITY pass in
`applyContainerContents` after the count reconcile: for each worn weapon, if it sits in a
different weapon section than the author's `section` hash, MOVE the real item between the two
weapon sections (`removeItemDontDestroy_returnsItem` + the target `InventorySection::addItem` -
the same direct placement `unequipToLoose` uses). Armour is untouched (its per-slot sections
never mismatch). Protocol bumped 6 -> 7. Manually validated both directions; the trace shows a
single `SLOT-MOVE` per side and **zero** weapon REMOVE ops (the real item is relocated, not
rebuilt). Doctrine 27.

### What did NOT work

- **Refabricating a removed worn weapon on the peer.** Once the transient destroyed the
  join's real weapon, the re-equip had to `createItemAndAdd(..., equip=true)`, which returns
  `ok=0` for the weapon template (and even when create succeeds for gear, equipped
  fabrication is discarded - d25). The peer therefore cannot reconstruct a worn weapon from
  scratch; it must PRESERVE and MOVE its own real copy. Hence the fix prevents the
  destroying transient rather than trying to rebuild afterwards.
- **Trusting "walk every equip section" as slot-complete for weapons.** It is NOT: worn
  weapons can live outside `getAllSections()` entirely (the held slot is only in
  `getPrimaryWeapon()`/`getSecondaryWeapon()`). The section walk caught the sheathed weapon
  by luck (it sat in `hip`), which masked the gap and let `inv_reequip` pass on a weapon it
  wasn't really testing end-to-end. Always read the weapon accessors in addition to sections.
- **Pointer-identity dedup across the section list and the weapon accessors.** The same
  logical weapon is two different `Item*` objects (one held by the section, one returned by
  the accessor), so it was counted twice. Dedup by `(stringID, itemType)` among equipped
  entries instead.
- **Synthesizing worn gear for the test.** `seedEquippedItem` (create item -> `tryAddItem`
  -> `equipItem`) reports success for one tick (`isEquipped` flips true), then the engine
  DISCARDS the fabricated, blank-handle item within a tick or two (next sample: gone, or
  fallen back to loose). So you cannot mint equipped gear from scratch; the test must use
  gear the save already wears. This is also why the equip (UP) reconcile is out of scope -
  a peer reconstructing "newly equipped" gear hits the same non-persistence. Only removal
  of real, save-loaded worn gear is reliably authoritative today.
- **Picking the tab's lowest-hand member blindly.** The lead isn't always the geared one;
  `inv_equip` now scans the tab for the lowest-hand member with `eq>=1` (deterministic, so
  both clients pick the same one) and caches it so sampling doesn't drift after the unequip
  drops that member's worn count.

### Doctrine added

- **Doctrine 25 - Equipped gear is a separate store; walk EVERY equip section, and MOVE
  real items between worn/loose instead of destroy+recreate.** Worn gear is NOT in
  `Inventory::_allItems`; enumerate `getAllSections()` and capture each section flagged
  `  isAnEquippedItemSection`, carrying an `equipped`+`slot` tag on the wire so worn vs loose
  reconcile independently. Walking all sections (rather than per-slot getters like
  `getEquippedArmour`/`getEquippedWeapons`) covers armour, belt, and a worn backpack - BUT
  IT IS NOT ENOUGH FOR WEAPONS. Worn weapons can live outside `getAllSections()` entirely:
  the SHEATHED weapon happens to sit in a `hip`/`back` section, but the HELD weapon is tracked
  only by `getPrimaryWeapon()`/`getSecondaryWeapon()`. So ALSO read those two accessors and
  append any worn weapon, deduped by `(stringID, itemType)` against the equipped entries
  already captured (the sheathed weapon shows up in BOTH the section and the accessor as two
  distinct `Item*`, so pointer-identity dedup double-counts - key on template). Without this,
  re-equipping a weapon (routed to the held slot) vanishes from the snapshot and never
  replicates. For an equipped<->loose split change on a template
  whose TOTAL count is unchanged, the reconcile does an in-place MOVE of the REAL item:
  UP = `equipItem` on an existing loose copy; DOWN = detach via `removeItemDontDestroy` then
  add into a LOOSE (non-equip) section's own `addItem`. Two engine gotchas this encodes:
  (a) fabricated blank-handle items still get discarded when equipped, so the up path MUST
  equip a real item (a synced loose copy), never `create+equipItem`; (b) the inventory-level
  `tryAddItem` AUTO-ROUTES an equippable item straight back into its slot (re-equipping what
  you just took off), so unequip-to-loose must target a loose section's `addItem` directly.
  A genuinely-new worn item with no loose copy to equip remains best-effort `create+equip`
  (may not persist). Validated by `inv_reequip` (unequip a real worn item to loose, then
  re-equip it: per-rank dip-then-restore + cross-client convergence, both directions) plus
  the `inv_equip` down-path drop test.
- **Doctrine 26 - Debounce inventory publishes; never replicate mid-drag transients.** The
  inventory UI holds a dragged item on the CURSOR, so for a fraction of a second the
  character's inventory reports the item FULLY GONE (not loose, not worn - absent from
  `_allItems` and every section). A peer that acts on that transient DESTROYS its real copy,
  and it cannot refabricate a worn weapon (createItemAndAdd fails for weapons; equipped
  fabrication is discarded - d25), so the item is lost permanently. `publishInventories`
  therefore requires a changed content fingerprint to stay STABLE before sending, and the
  window is **removal-aware**: additions and equip<->loose MOVES (entry count unchanged) use a
  fast `INV_SETTLE_MS` (350 ms), but a count DECREASE (`n < lastSentN` - the in-cursor "gone"
  flicker AND a genuine drop) must hold for `INV_REMOVE_SETTLE_MS` (1800 ms). A flat 350 ms is
  NOT enough: a human cursor-hold during re-equip was measured at ~960 ms, well past it. So
  only resting states replicate, and removals specifically ride out the human drag - the peer
  reconciles every weapon change as a MOVE of its real item, never a destroy. The tradeoff is
  a ~1.8 s lag before a genuine drop appears on the peer (accepted). Diagnose inventory
  desyncs with `KENSHICOOP_INV_DUMP=1` (full inventory dump on
  every SEND/APPLY + a `[recon]` per-branch trace in `applyContainerContents`) and the
  `inv_wpnseq` scenario, which drives the reconcile through a crafted snapshot sequence on a
  single instance - reproducing the bug deterministically without any manual play.
- **Doctrine 27 - Replicate the equip SECTION, not just the AttachSlot, for weapons.** A
  character's two weapon slots (`hip` = Weapon II, `back` = Weapon I) BOTH carry
  `limitedSlot = ATTACH_WEAPON`, so the `slot` field cannot tell them apart and `equipItem`
  auto-routes a re-equipped weapon to the default slot. Carry a 16-bit hash of the equip
  section NAME (`section`, identical on both clients since sections are built from the same
  race/inventory data) on each `InvItemEntry`, FOLD IT INTO `invEntryHash` (else a Weapon
  I<->II move has an unchanged fingerprint and never publishes), and add a SLOT-FIDELITY pass
  to `applyContainerContents` that MOVES the real worn weapon between weapon sections
  (`removeItemDontDestroy` + target `InventorySection::addItem`) to match the author's section.
  Armour needs none of this (unique per-slot sections). Validated manually both directions
  (one `SLOT-MOVE` per side, zero REMOVE).
