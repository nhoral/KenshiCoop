# Spike 42 - Animation system internals

- Type: STATIC
- Status: DONE
- Save: c (runtime anchors reused from spikes 37/41 harness checks)
- Branch commit: <filled at commit>

## Goal

Map how Kenshi drives character animation: what is the entry point, what selects a clip,
and how much of the system is reachable/reversed. This decides how co-op replicates a
remote character's pose - by streaming a clip name, or (as the mod does today) by
streaming the locomotion state and letting the engine pick the clip.

## Method

Static read of `Character.h`, `CharBody.h`, `CharMovement.h` (the animation entry points
+ the `AnimationClass` type), cross-referenced with the mod's existing animation handling
(`KenshiCoop.cpp` comments + the `runSlaveAnim` discovery hook), and the test harness's
animation-quality checks as runtime anchors. No new probe shipped.

## The animation architecture (three layers)

**1. Name-driven clip entry (reversed, hookable):**
- `Character::runSlaveAnim(const std::string& anim, float speed, float sync)` RVA 0x5B0930
  - plays a named clip at a speed with a sync phase. The mod already installs a discovery
  hook here (`runSlaveAnim_hook`) to harvest `(TaskType, animName, speed, sync)` tuples.
- `CharBody::getUpFromRagdoll(const std::string& animationName)` RVA 0x5C6270 - a second
  name-driven entry (stand-up clip after ragdoll).

**2. Selection inputs (reversed, the layer the mod actually uses):**
- The engine picks walk/idle/run from `CharMovement` state - `currentlyMoving`,
  `currentSpeed`/`desiredSpeed`, `movementMode` (`MOVE_NORMAL`/`COMBAT`/`DIRECTION`),
  `swordStateEnum` - plus the `CharBody::currentAction` (`Tasker`, a `TaskType`). So
  animation is largely a *derived* function of movement + task state, not an independent
  channel.

**3. Low-level `AnimationClass` (NOT reversed):**
- Reachable as a pointer: `Character::getAnimationClass()` RVA 0x645E0,
  `Character::animation` (0x448), `CharBody::animation` (0x10) - but the **class itself is
  obfuscated/stripped** (`AnimationClass` resolves to an empty token in all 8 headers
  that reference it: Character/CharBody/CharMovement/CombatClass/MedicalSystem/Appearance).
  So per-bone / per-track / blend-weight manipulation is unavailable; we have the pointer
  but no layout.

## Findings

1. **Animation has a clean name-driven entry (`runSlaveAnim(name, speed, sync)`) that is
   reversed and already hooked by the mod.** This is the lowest-level *usable* control:
   force a named clip with a speed and sync phase. RVA 0x5B0930.
2. **But the production replication model is state-driven, not clip-driven, and it works.**
   The mod streams the host's `CharMovement` locomotion state and lets the *receiver's*
   engine select and advance the clip - so ghosts play real, advancing walk/run/idle with
   no animation data on the wire. This is validated by the harness (see Validation).
3. **`AnimationClass` internals are a hard wall.** The pointer is reachable but the type is
   stripped, so direct timeline/bone control is not available without reversing it. Any
   "play exact clip X at frame Y" feature must go through `runSlaveAnim` (name+sync), not
   through `AnimationClass`.
4. **Pose-by-clip replication is feasible in principle** (via `runSlaveAnim`) and the mod
   built a discovery hook to harvest the clip vocabulary for it - but that vocabulary was
   not captured in this spike (see Open questions), so clip-by-name replication remains
   designed-but-unproven.

## Validation

- Finding 2 (state-driven animation works): the test harness's animation-quality gates
  PASSED on the recent runs (spike 37 capture, `tools/test-runs/20260620_*`):
  `anim-truth PASS - floatFrac=0.002 (<=0.3), translateFrames=4737`,
  `march-in-place PASS - marchFrac=0.009 (<=0.2), restSamples=21268`,
  `smoothness PASS - zeroFrac=0.011, maxStep=137.959`. `translateFrames=4737` means the
  ghost's skeleton genuinely advanced a locomotion clip driven only by streamed movement
  state - i.e. the engine selected+played the clip from CharMovement, exactly as Finding 2
  claims, with no clip name sent.
- Findings 1/3: RVAs + the `AnimationClass` opacity quoted directly from the headers
  (runSlaveAnim 0x5B0930 `Character.h:641`; getAnimationClass 0x645E0 `:663`; `animation`
  member 0x448 `:737`; `CharBody::animation` 0x10; obfuscated `AnimationClass` token in 8
  headers via `rg`). The mod's `runSlaveAnim` hook install (`KenshiCoop.cpp:2463`) confirms
  the entry point is real and hookable.

## Open questions / hypotheses (UNVALIDATED)

- **The clip-name vocabulary was NOT harvested.** The `runSlaveAnim` discovery hook
  produced **no** `SLAVEANIM` lines in any available run log - either it failed to install
  (it is non-fatal), or `runSlaveAnim` is not called for the (player/idle) characters in
  these scenarios (the name "slave" hints it may be for follower/secondary anim only).
  So the actual clip names per TaskType are unknown, and Finding 4 (clip-by-name
  replication) is unproven. Needs a dedicated run that exercises varied tasks (combat,
  sit, KO) with the hook confirmed installed.
- **Whether `runSlaveAnim` can be *driven* (not just observed)** to force an arbitrary
  clip on a ghost is untested - we have only hooked it for reading.
- **`AnimationClass` layout** (frame/track/blend state) is entirely unreversed; reversing
  it is the only path to fine-grained animation control.
- **`speed`/`sync` semantics** of `runSlaveAnim` are inferred from arg names, not measured.

## Implications for co-op

- Keep the **state-driven** locomotion replication as the primary mechanism - it is proven
  and needs zero animation bandwidth.
- For non-locomotion poses that the AI won't reproduce from state alone (emotes, scripted
  actions), `runSlaveAnim(name, speed, sync)` is the entry to drive - but first harvest the
  clip vocabulary and confirm the call can be issued (not just hooked).
- Do not plan any feature that needs `AnimationClass` internals until it is reversed.

## Recommended follow-ups

- Re-run the `runSlaveAnim` discovery hook in a task-rich scenario (driven melee + sit +
  KO) and confirm `SLAVEANIM` lines appear; catalog the (TaskType -> clip name) map.
- Probe *driving* `runSlaveAnim` on a ghost to force a named clip; screenshot-verify.
- Defer any `AnimationClass`-level work to a dedicated reversing spike.
