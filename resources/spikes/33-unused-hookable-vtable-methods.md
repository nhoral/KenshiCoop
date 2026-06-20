# Spike 33 - Unused hookable vtable methods

- Type: STATIC
- Status: DONE
- Save: n/a
- Branch commit: <filled at commit>

## Goal

We currently drive co-op from a single main-loop hook and poll engine state each
frame. Which Kenshi virtual methods could we hook instead to get engine-native event
edges (damage, death, spawn, drop, save) - cheaper and more precise than polling?
This spike inventories the current hooks and the highest-value UNHOOKED candidates.

## Method

Static analysis of the compiled plugin (`src/plugin/Plugin.cpp`, `game/Engine.cpp`;
note `KenshiCoop.cpp` is on-disk but excluded from the build per `KenshiCoop.vcxproj`)
and the RE'd SDK headers under `third_party/KenshiLib_deps/KenshiLib/Include/kenshi/`.
Verified every cited address/line by reading the headers and hook sites directly. No
game run (STATIC); no files modified.

Hooking mechanism (the only one in use): `KenshiLib::AddHook(addr, detour, &orig)` on
an address from `KenshiLib::GetRealAddress(&Class::_NV_<fn>)`. A `_NV_<fn>` twin shares
the virtual's RVA but is a non-virtual symbol, so `GetRealAddress` can resolve it (it
cannot resolve a pure-virtual member pointer). No direct vtable patching is used.

## Raw evidence

Currently installed hooks (3 sites, verified):

```
Plugin.cpp:409-411  AddHook(GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff), &mainLoop_hook,   &g_mainLoop_orig)
Plugin.cpp:457-459  AddHook(GetRealAddress(&TitleScreen::_NV_update),                    &titleUpdate_hook,&g_titleUpdate_orig)   [only if a save is configured]
Engine.cpp:820-822  AddHook(GetRealAddress(&Character::_NV_periodicUpdate),              &periodicUpdate_hook, ...)              [only if join + probeAiSuspend]
```

SDK scale: ~97 `kenshi/**/*.h` headers declare virtuals; ~1,277 `_NV_` hook twins
exist. Verified candidate twins (header:line, RVA from the header comment):

```
Character.h:264   _NV_update()                RVA 0x5CE9B0   per-character sim step
Character.h:266   _NV_postUpdate()            RVA 0x5CD2A0
Character.h:270   _NV_periodicUpdate()        RVA 0x5CC610   (the AI-suspend twin)
Character.h:536   _NV_hitByMeleeAttack(CutDirection, Damages&, Character* who, CombatTechniqueData*, int)  RVA 0x438D70
Character.h:439   declareDead()               RVA 0x7A4FA0   (non-virtual, still hookable)
MedicalSystem.h:117  _NV_periodicUpdate()     RVA 0x64D2F0   bleedout/KO progression
CombatClass.h:65  _NV_initCombatMode(const hand&, int, bool)  RVA 0x665230
CombatClass.h:67  _NV_go(float)               RVA 0x60C4D0   combat state advance
CombatClass.h:71  _NV_periodicUpdate(float)   RVA 0x60CCF0
Inventory.h:177   _NV_dropItem(Item*)         RVA 0x744B80
PlayerInterface.h:110  _NV_factoryObjectCreatedCallback(RootObject*)  RVA 0x4D3190
```

## Findings

1. **The mod hooks exactly three engine functions, all via KenshiLib detours on
   `_NV_` twins; it never patches a vtable.** One unconditional per-frame hook
   (`GameWorld::_NV_mainLoop_GPUSensitiveStuff`), one conditional auto-load hook
   (`TitleScreen::_NV_update`), one conditional AI-suspend probe
   (`Character::_NV_periodicUpdate`). Everything else (spawn, save, combat, drop) is
   either polled in the main loop or called through resolved non-virtual pointers.
2. **An engine-native DAMAGE/death edge is available and unhooked:
   `Character::_NV_hitByMeleeAttack` (Character.h:536) and the non-virtual
   `Character::declareDead` (Character.h:439).** Today combat KO/death is inferred by
   diffing `bodyState` each frame in the replicator; hooking these would give the
   exact hit (with attacker `who`, damage, technique) and the exact death moment.
3. **An engine-native ITEM-DROP edge is available and unhooked: `Inventory::
   _NV_dropItem` (Inventory.h:177).** The world-item/conservation path (Phase W2)
   currently polls weapon slots; this twin fires precisely when an item is dropped.
4. **A spawn/creation notification is available and unhooked:
   `PlayerInterface::_NV_factoryObjectCreatedCallback` (PlayerInterface.h:110).** New
   objects are currently discovered by re-scanning the interest sphere; this callback
   is the engine telling us "an object was just created."
5. **Finer-grained per-entity tick hooks exist beyond the world main loop:
   `Character::_NV_update`/`_NV_postUpdate` (Character.h:264/266), `CharBody::
   _NV_update(float)`, `CharMovement::_NV_update(float)`, `CombatClass::_NV_go(float)`,
   and `MedicalSystem::_NV_periodicUpdate` (MedicalSystem.h:117).** These run per
   owned entity and could host per-entity authority/suppression logic without scanning
   every frame.

## Validation

- Finding 1: hook sites read directly - `Plugin.cpp:409-411` (mainLoop), `:457-459`
  (title, inside the `g_cfg.save` branch), `Engine.cpp:820-822` (inside
  `installAiSuspendHook`, called at `Plugin.cpp` only when join + `probeAiSuspend`).
  Mechanism comments at `Plugin.cpp:406` and `core/Functions.h`; no `vtable`/
  patch-based hooking found anywhere in `src/plugin`.
- Finding 2: `Character.h:535-536` declares `hitByMeleeAttack(...)` + `_NV_` twin
  (RVA 0x438D70); `Character.h:439` `declareDead()` (RVA 0x7A4FA0). The current
  inference path is Replicator.cpp:184-223 (bodyState-edge -> EVT_KNOCKOUT/DEATH),
  confirming these edges are derived, not hooked.
- Finding 3: `Inventory.h:176-177` declares `dropItem(Item*)` + `_NV_dropItem`
  (RVA 0x744B80). (Cross-ref the Phase W2 drop spikes which poll slots.)
- Finding 4: `PlayerInterface.h:109-110` declares the override +
  `_NV_factoryObjectCreatedCallback(RootObject*)` (RVA 0x4D3190); base pure-virtual at
  `PlayerInterface.h:21`.
- Finding 5: verified twins `Character.h:264/266`, `MedicalSystem.h:117`,
  `CombatClass.h:65/67/71` all carry distinct RVAs in the header comments (quoted in
  Raw evidence), i.e. they are independently resolvable/hookable.

## Open questions / hypotheses (UNVALIDATED)

- **Hook safety/thread-affinity per candidate is NOT verified.** The design rule is
  that main-thread mutation only happens in `mainLoop_hook`; whether each of these
  twins is always called on the main thread (so a detour can safely touch game state)
  is unverified and must be checked per hook before use.
- **Reentrancy / recursion**: hooking high-frequency per-entity ticks (`_NV_update`)
  for many entities could be costly or reentrant; the per-call overhead is unmeasured.
- **`hitByMeleeAttack` argument layout** (`Damages&`, `CombatTechniqueData*`) is taken
  from the header; whether those structs are fully/correctly mapped for safe reads is
  unverified - a runtime probe reading the args would confirm.

## Implications for co-op

- Several features the spikes flagged as "inferred by polling" have a cleaner
  engine-native source: death/damage (`hitByMeleeAttack`/`declareDead`), item drop
  (`dropItem`), and spawn (`factoryObjectCreatedCallback`). Moving to these hooks
  would cut per-frame scanning and give exact, attributable edges (helps Spikes
  10/11/13 follow-ups: real attacker + real damage at the hit, not a recency guess).
- The plumbing already exists (`KenshiLib::AddHook` + `_NV_` resolve), so adding a
  hook is low-effort; the gating concern is thread-affinity, not capability.

## Recommended follow-ups

- DUMP probe: install a temporary logging detour on `Character::_NV_hitByMeleeAttack`
  and `Inventory::_NV_dropItem`, confirm they fire on the main thread, and dump the
  attacker/victim/damage and dropped-item identity - this would directly upgrade the
  attribution (Spike 11) and world-drop (Phase W2) paths.
- DUMP probe: hook `PlayerInterface::_NV_factoryObjectCreatedCallback` and log every
  created object to replace interest-sphere rescans for spawn detection.
- Document the verified candidate list in `resources/` as the hook backlog.
