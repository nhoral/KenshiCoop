# Spike 44 - Frame/tick model + main-thread guarantees

- Type: STATIC
- Status: DONE
- Save: n/a (static SDK + cross-reference of the mod's runtime-proven hook)
- Branch commit: <filled at commit>

## Goal

Characterise Kenshi's frame/tick model and threading so co-op code knows: where it is
safe to touch game state, what timestep the simulation uses, what runs off the main
thread, and what the cross-thread hazards are. This governs every engine call the mod
makes (all the spike probes) and the net/replication threading.

## Method

Static read of `GameWorld.h` (the main-loop entry, update lists, thread members, time
stamper) cross-referenced with the mod's own `mainLoop_hook` (`KenshiCoop.cpp`), which is
runtime-proven: every spike's per-frame probe executes inside it. No new code.

## The model

**Main-loop entry (where the mod lives):**
- `GameWorld::mainLoop_GPUSensitiveStuff(float time)` - virtual, RVA 0x7877A0, with a
  non-virtual twin `_NV_mainLoop_GPUSensitiveStuff` (same RVA). The mod hooks the `_NV_`
  twin (`KenshiCoop.cpp:2451`). `time` is the per-frame delta (seconds). The "GPU
  sensitive" name marks it as the render/main thread tick.
- **Variable timestep:** the loop takes a float `dt` per call (not a fixed tick), scaled
  by `frameSpeedMult` (0x700) / `getFrameSpeedMultiplier` (spike 34). So simulation
  advance is `dt * frameSpeedMult`; pause is `paused` (0x8B9) or `frameSpeedMult==0`.

**Per-frame work the engine does on the main thread (from `GameWorld` members/methods):**
`processKeys()`, `processThreadMessages()` (drains worker->main messages),
`charsUpdate()`/`charsUpdateUT()`/`charsUpdatePaused()`/`charsUpdateDeathParade()` over
`charUpdateListMain` (0x750, guarded by `charUpdateListMain_inUse` 0x749),
`processSysMessages()`, and phased deferred destruction
(`killListPhase0/1/2`, `processKillList`, `processUpdateRemovalList`) - objects are
destroyed across phases, not mid-frame.

**Threads other than main (the hazard surface):**
- `_AINonRenderThread` (`RenderTimeBackthread*`, 0x790) + `AINonRenderThread()` returns a
  `ThreadWannabe*` - **AI runs on a separate non-render thread.**
- `audioThread` (`AudioSystemGlobal*`, 0x8C0) - separate audio thread.
- `threadSafeRagdollUpdates()` (0x7D1120) - ragdoll/physics updates are explicitly
  thread-safe-guarded, i.e. touched off the main thread.
- Cross-thread plumbing: `BackThreadMessagesToMainT<T>` / `MainthreadStateReaderT<T>`
  (in `ZoneManager.h`) with swap mutexes - the engine's back-thread->main-thread handoff.
- The mod adds its own **network thread** (NetLink `threadLoop`).

**Time model:** `SimpleTimeStamper timeStamper` (0x8A0), `getTimeStamp() -> double`,
`getTimeFromStamp_inGameHours`, `getLengthOfHourInRealSeconds` - wall/real time to in-game
hours conversion (the basis of spike 34's clock reads).

## Findings

1. **The simulation is a variable-timestep main-loop**, entered via
   `mainLoop_GPUSensitiveStuff(float dt)` on the render/main thread, with sim advance
   scaled by `frameSpeedMult`. There is no fixed-tick guarantee - co-op timing must not
   assume a constant step.
2. **The mod's per-frame code runs on the main thread and deliberately runs the engine
   first, then its own drive last.** `mainLoop_hook` calls `g_mainLoop_orig(gw,dt)` then
   issues its orders, so the mod always gets "the last word" each frame after the local
   AI has run.
3. **Kenshi is multi-threaded: at minimum a render/main thread, an AI non-render thread,
   and an audio thread, plus thread-safe ragdoll updates.** Engine state is therefore NOT
   freely touchable from arbitrary threads; the main loop is the safe point.
4. **There is a real loader-lock / worker-thread teardown hazard.** The mod must use
   `TerminateProcess` (not `ExitProcess`) to exit from the hook, because orderly teardown
   deadlocks against the live GPU/audio/net worker threads under the loader lock. This is
   concrete, experienced evidence that those worker threads are live concurrently with the
   main-loop hook.
5. **Destruction is phased/deferred** (`killListPhase0/1/2`) - the engine never deletes
   objects mid-frame, which is why the mod's despawn/kill helpers schedule rather than
   free immediately.

## Validation

- Findings 1-2: the hook signature `(GameWorld*, float dt)` and the
  `g_mainLoop_orig(gw,dt)`-then-drive ordering are in `KenshiCoop.cpp:2097/2226` and are
  **runtime-proven by every spike**: e.g. spike 43 emitted ~20 per-frame-gated `zone`
  samples and spike 37 ~20 `env` samples, all produced from `dispatchTick` running inside
  this hook, with clean self-exit. `frameSpeedMult` behaviour was measured in spike 34.
- Findings 3-4: thread members (`_AINonRenderThread` 0x790, `audioThread` 0x8C0,
  `threadSafeRagdollUpdates` 0x7D1120) quoted from `GameWorld.h:220-244`; the
  loader-lock/TerminateProcess hazard is documented and acted upon in
  `KenshiCoop.cpp:2118-2124` (the mod exits this way successfully every timed run).
- Finding 5: `killListPhase0/1/2` + `processKillList` members from `GameWorld.h:227-229`.

## Open questions / hypotheses (UNVALIDATED)

- **Which engine reads are safe from the mod's NetLink thread** is not characterised - the
  mod funnels engine work to the main-loop hook precisely to avoid this, but a explicit
  "what may the net thread read" boundary is undocumented.
- **The order of engine sub-phases within `mainLoop_GPUSensitiveStuff`** (when exactly
  `charsUpdate` vs physics vs render happen relative to our hook entry) is inferred from
  member names, not traced.
- **Whether `frameSpeedMult==0` fully freezes `charsUpdate`** (vs `charsUpdatePaused`
  taking over) is unconfirmed - relevant to whether a paused host still ticks NPCs.
- **AI-thread data races**: reading `Character`/AI state that the AI non-render thread is
  concurrently writing could tear; the mod reads in the main loop to avoid this but the
  exact fields owned by the AI thread are unmapped.

## Implications for co-op

- Do all engine reads/writes from the **main-loop hook** (the proven-safe point); keep the
  net thread to socket IO + queues, handing data across via the same main-loop drain the
  engine itself uses (`processThreadMessages` pattern).
- Never assume a fixed tick - drive replication off `dt`/wall-clock, and account for
  `frameSpeedMult` (and per-client speed/pause divergence from spike 34).
- Exit only via `TerminateProcess` from the hook (loader-lock hazard) - already adopted.
- Schedule destruction; the engine defers it in phases anyway.

## Recommended follow-ups

- Trace sub-phase ordering inside the main loop (lightweight timestamps around
  `g_mainLoop_orig`) if precise ordering ever matters for a feature.
- Map which `Character`/AI fields the AI non-render thread writes, to define safe
  net-thread reads (currently avoided by main-loop-only access).
