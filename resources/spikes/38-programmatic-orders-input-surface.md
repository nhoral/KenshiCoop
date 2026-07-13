# Spike 38 - Programmatic orders / input surface

- Type: STATIC
- Status: DONE
- Save: n/a (static SDK + cross-reference of already-resolved mod calls)
- Branch commit: <filled at commit>

## Goal

Catalog every way the mod can drive a character/player *programmatically* - move
orders, combat orders, jobs, body state - and the raw input surface (keyboard/mouse).
This tells us which co-op features (remote-issued orders, "command peer's squad",
hotkeys) can be built on existing engine entry points vs. need new reversing.

## Method

Static read of the Kenshi SDK headers (`CharMovement.h`, `Character.h` via the mod's
own typedefs, `InputHandler.h`, `Globals.h`, `Enums.h`) for named order/input entry
points with concrete RVAs. Cross-referenced against `src/plugin/game/Engine.cpp`, which
already **resolves and calls** most of the order layer at runtime - that prior committed
use is the runtime validation for those RVAs (see Validation). No new code; nothing run.

## The order surface (three layers)

**1. Low-level movement - `CharMovement` (`CharMovement.h`, derives AbstractMovementBase):**
- `setDestination(Vector3&, UpdatePriority, bool notVertical)` RVA 0x660AF0 - path to a point
- `setDestination(RootObjectBase*/Character*/Building*, UpdatePriority)` - path to an object
- `halt()` 0x65F4F0 - stop + clear path; `manualMovement(Vector3&)` 0x65D740 - raw motion
- `setDesiredSpeedOrders(MoveSpeed|float)`, `setMovementMode(MOVE_NORMAL/COMBAT/DIRECTION)` 0x65E990
- `faceDirection` / `lookatPosition` / `setPatrolInput` / `playerMoveOrderWhileInCombatMode`
- `combatMovementOffensive(hand target, min, max, circle, power, speedLimit)` 0x2AE450
- NOTE: a bare `CharMovement::setDestination` is *ignored* for a player-controlled char
  (the AI overwrites it next frame) - see the mod's own comment at Engine.cpp:733.

**2. Player/AI order layer - `Character` (mod typedefs in Engine.cpp, runtime-resolved):**
- `Character::setDestination(Vector3* pos, bool shift)` - the **click-to-move** path; unlike
  the CharMovement version it actually moves a player char (Engine.cpp:65-68, resolved 307).
- `Character::addOrder(Building* dest, TaskType, RootObject* subject, shift, clear, Vector3* loc)`
  and `Character::addJob(TaskType, subject, shift, addDontClear, Vector3* loc)` - the
  **right-click order/job queue** with an explicit world location (Engine.cpp:108-119).
- `Character::addGoal(TaskType, RootObjectBase* subject)` - a **persistent AI goal**
  (e.g. `UNPROVOKED_FOCUSED_MELEE_ATTACK`); `clearAllAIGoals(Character*)` wipes them.
  This is the proven attack-order path (`orderMeleeAttack`, Engine.cpp:3116-3120).
- Squad/control: `PlayerInterface::recruit(Character*, bool)` (inhabit), `separateIntoMyOwnSquad`.

**3. Body-state forcing (not "orders" but programmatic control):**
- `Character::ragdollMode(bool, RagdollPart)`, `MedicalSystem::knockout/knockoutForceTimer`,
  `CharBody::setCurrentAction(TaskType,target)`, `CharBody::endAction()` - all resolved+used.

**Order vocabulary - `TaskType` enum (`Enums.h:277`):** large; relevant verbs include
`MELEE_ATTACK`, `FOCUSED_MELEE_ATTACK`, `UNPROVOKED_FOCUSED_MELEE_ATTACK`,
`CHOOSE_ENEMY_AND_ATTACK`, `RANGED_ATTACK[_FOCUSED[_UNPROVOKED]]`, `ATTACK_*` (town/
enemies/etc.), `SIT_AROUND`, `STAND_AT_NODE`, `MELEE_ATTACK_ANIMAL`. These are the task
codes passed to addOrder/addJob/addGoal.

## The input surface - `InputHandler` (`InputHandler.h`), instance = `Globals::key`

- Singleton: `__declspec(dllimport) InputHandler* key;` (`Globals.h:28`). The per-frame
  consumer is `PlayerInterface::playerControl(InputHandler&)` (0x7FFCA0) - it reads this
  object to produce player orders.
- **Read live input state** (members): `up/down/left/right/space`, `mLeft/mRight` +
  edge flags `mLDown/mRDown/mLUp/mRUp`, `ctrl/shift/alt`, `mPos`/`mPosAbs` (Vector2),
  `mSpeed`, `mWheel`, plus `isKeyState(command)` 0x361C10 and `controlEnabled` (0xD0).
- **Bindings (read/modify):** `bind(name,key)`, `unbind`, `unbindAll`, `isBound`,
  `getBoundKeys(command)`, `getBoundCommand(key,mode)`, `keyString`, `addCommand`/`addKey`.
- **Event injection:** `sendEvent(name)` 0x361B30 - fires a named command's event handler
  (the closest thing to "press this bound action" programmatically).
- Masks: `SHIFT/CTRL/ALT_MASK`; modes `GLOBAL`/`EDITOR`.

## Findings

1. **The order layer is fully reachable and the high-value verbs are already
   runtime-proven.** Movement (`Character::setDestination` / `CharMovement::setDestination`/
   `halt`), jobs (`addOrder`/`addJob` with a world location), and AI goals (`addGoal` /
   `clearAllAIGoals`, incl. attack goals) all have concrete RVAs and are resolved+called
   in committed mod code. No new reversing is needed to issue moves/attacks/jobs.
2. **There are two distinct "move" entry points with different semantics.** A bare
   `CharMovement::setDestination` is overwritten by the AI for a player char; the real
   player-order path is `Character::setDestination(pos, shift)` (and addOrder/addJob for
   queued/located tasks). This is documented from the mod's own Stage-1 finding.
3. **Orders are addressed by a `TaskType` enum + a subject + an optional world location.**
   The same triplet (task, subject, loc) covers move, sit, stand, and the full `ATTACK_*`
   / `*MELEE*` / `RANGED_ATTACK*` family - so a generic "issue order" RPC is feasible:
   serialise (TaskType, subject-hand, Vector3) and replay via addOrder/addJob/addGoal.
4. **A first-class input object exists (`Globals::key`, `InputHandler`) exposing both
   live state and binding control, but no clean public "inject a keypress".** State is
   set by private OIS callbacks (`keyDownEvent`/`keyUpEvent`); the public injection
   primitive is `sendEvent(name)` (fire a bound command) and `controlEnabled` to gate
   the local player's input. (Header-only - see Validation; not yet exercised.)

## Validation

- Findings 1-3 (order layer): every cited entry point is **already resolved via
  `KenshiLib::GetRealAddress` and invoked at runtime** in `src/plugin/game/Engine.cpp`
  (`g_charSetDestFn` :307, `g_addOrderFn`/`g_addJobFn` :330/:108-119, `g_addGoalFn`/
  `g_clearGoalsFn` :103-107, `orderMeleeAttack` :3116). Prior committed spikes (e.g. the
  driven-melee battles in spikes 9-13) drove characters through exactly these calls and
  produced observed combat/movement - so the RVAs and signatures are correct and
  callable, not just header guesses. Finding 2's "bare CharMovement ignored" is the
  mod's own recorded Stage-1 result (Engine.cpp:733).
- TaskType verbs (Finding 3): quoted directly from `Enums.h:277+` (line numbers cited).
- Finding 4 (input layer): symbols + RVAs + `Globals::key` declaration read from
  `InputHandler.h` / `Globals.h`. This layer is **NOT runtime-validated** - the mod does
  not currently touch `Globals::key`; that the singleton is live and `sendEvent` behaves
  as assumed is UNVALIDATED (see below).

## Open questions / hypotheses (UNVALIDATED)

- **`Globals::key` liveness and `sendEvent` behaviour are untested.** Need a host-only
  probe: read `Globals::key`, log a few state members across a frame, and try
  `sendEvent("<a known bound command>")` to confirm it triggers the action.
- **Whether `sendEvent` can drive *gameplay* orders (vs only camera/UI commands)** is
  unknown - the command map's contents weren't enumerated. Probe: iterate `commands`.
- **`addOrder` vs `addJob` exact difference** (clear-queue vs append; the `shift`
  semantics) is inferred from arg names, not differentially tested.
- **Ranged/attack-of-object orders** (`RANGED_ATTACK_FOCUSED`, `ATTACK_TOWN`, etc.) were
  catalogued but only the melee goal path is runtime-proven; the others are unexercised.

## Implications for co-op

- A remote "command my peer's squad" / "order this character" feature is buildable now
  on the proven order layer: send (TaskType, subject-hand, Vector3) and apply with
  `addOrder`/`addJob`/`addGoal` on the resolved local Character - the same calls the mod
  already makes to script battles.
- Local hotkeys/UI can read `InputHandler` (`Globals::key`) for chords and use
  `controlEnabled` to suppress player input during cutscene/spectator modes - but treat
  the input layer as unproven until a probe confirms `Globals::key` and `sendEvent`.

## Recommended follow-ups

- Probe `Globals::key`: dump live input members + enumerate the `commands` map, then test
  `sendEvent` on a benign bound command (validates Finding 4).
- Build a single `engine::issueOrder(Character*, TaskType, subjectHand, Vector3)` wrapper
  over addOrder/addJob/addGoal as the canonical order primitive for co-op RPCs.
- Differential test addOrder vs addJob (queue clear vs append; shift) to pin semantics.
