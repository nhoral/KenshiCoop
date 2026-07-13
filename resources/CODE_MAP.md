# CODE_MAP - module inventory, shared-state contracts, log-tag index

Built incrementally during the 2026-07-12 monolith split (see the plan "Break
Up the Monolith Files"). This file INDEXES the inline comments - it does not
replace them; the authoritative documentation for any symbol stays at its
definition. Grep conventions are load-bearing: banner comments (`// ---- ...`),
`SCENARIO *` markers and every log string are stable API - the log format is
the contract between the C++ plugin and the PowerShell oracles.

## Engine plane (`src/plugin/game/`)

`Engine.h` is the ONLY public surface (Replicator/Scenario/Plugin include it;
it did not change in the split). `EngineInternal.h` is the private surface
shared by the `Engine*.cpp` TUs: the full include prelude, the resolved-
function-pointer typedefs, `extern` declarations for the `g_*` registry, and
declarations of cross-TU helpers.

### File inventory

| File | Responsibility | Shared hubs it touches |
|---|---|---|
| `EngineInternal.cpp` (was `Engine.cpp`) | g_* registry definitions, `resolve()` (all `GetRealAddress` resolution), hook detour bodies + `install*` entries, hook edge queues + drains, save/load coordination (Stage-0 runtime, protocols 31-32), squad-roster poll (protocol 35), AI-suspend/damage-guard set management, `limbStateOf`/`facSidOf`/vote-button helpers | owns ALL of them |
| `EngineEntity.cpp` | EntityState capture (`captureOne`/`captureSquad`/`captureNpcs`), hand resolve (`resolve`/`resolveCharByHand`/`readObjectHand`), motion apply/orders, NPC suppression, interest centers, debug marker HUD, seat/bed/cage/machine template + work-fixture finders | `g_npcQuery`, `g_dataScratch`, hand/locomotion `g_*Fn` |
| `EngineInventory.cpp` | Phase 4a container capture/reconcile (`readInvItems`, `createItemAndAdd`, equip levers), Phase W0/W1 world items, spike-451 weapon-mint trace (`[mkspy]`), spike-401 research store probe (r401) | `g_dataScratch`, inventory/factory `g_*Fn` |
| `EngineSpawnCombat.cpp` | Protocol 21 runtime-spawn proxies (`spawnProxyNpc`/`describeCharacter`), scenario scene setup (seat/bed/cage/machine/duel/down), combat reads + melee orders, limb-loss/revive/bandage medical primitives | `g_npcQuery`, `g_dataScratch`, spawn/combat/medical `g_*Fn`; owns duel/down anchor statics |
| `EngineCharState.cpp` | Protocol 17 stats, 18 carry, 19 furniture, 20 stealth; consensus game-speed plane (read/write/quiet write, intent consume, vote buttons) | stats/carry/furniture/stealth/speed `g_*Fn`, speed-intent flags |
| `EngineWorld.cpp` | Protocol 22 money/vendors, 23 recruit probe, 24 faction relations, 26 doors, 27/28 placed buildings, 33 machines, 34 containers | wallet/door/build/machine/faction `g_*Fn`, `g_facDeltas` (read via drain) |
| `ZoneQuery.cpp` | Zone-loaded query, quarantined TU (ZoneManager.h vs CombatClass.h `ParticlePool` clash) | none (private `g_zone*Fn`) |

Rules (enforced by EngineInternal.h banner): domain TUs never define `g_*`
pointers or install hooks; a helper needed by a second domain TU moves to
`EngineInternal.cpp` (or gains external linkage) and is declared in
`EngineInternal.h` - never duplicated.

### Shared-state contracts (engine plane)

| Hub | Writer | Readers | Invariant / reset rule |
|---|---|---|---|
| `g_*Fn` registry (~100 fn ptrs) | `resolve()` + `install*` (EngineInternal.cpp), once at plugin load | every Engine TU | Null until `resolve()`; every caller null-checks. Never reassigned after load; hook `*Orig` pointers are written only by `KenshiLib::AddHook`. |
| `g_npcQuery` (`lektor<RootObject*>`) | any Engine TU (`.clear()` then engine fill) | same call only | Reused scratch, MAIN THREAD ONLY, no reentrancy: contents valid only until the next call that clears it. |
| `g_dataScratch` (`lektor<GameData*>`) | any Engine TU (template scans) | same call only | Same contract as `g_npcQuery`. |
| Edge queues: `g_saveEdges`/`g_loadEdges` (cap 8), `g_recruitEdges` (64), `g_buildEdges` (32), `g_removeEdges` (5x32 u32), `g_facDeltas` (64), `g_squadMoveEdges` | hook detours (EngineInternal.cpp) | `drain*` entries, called once per tick by Plugin/Replicator | Engine tick and plugin tick share the main thread - NO LOCK. Queues are capped (drops beyond cap); drain clears. Not reset on session swap - drained continuously. |
| Suppression flags: `g_saveSuppressAll`, `g_loadSuppressAll`, `g_loadBypassOnce`, `g_inLoadDetour` | `setSaveSuppress`/`setLoadSuppress`/`setLoadBypassOnce` (Plugin-driven), detours (latch) | save/load detours | `g_loadBypassOnce` self-clears on first detour hit; `g_inLoadDetour` is a same-thread reentry latch (two load overloads call each other). |
| `g_aiSuspended` / `g_damageGuarded` (`std::set<Character*>`) | `addAiSuspend`/`clearAiSuspend`, `addDamageGuard`/`clearDamageGuard` (Replicator drive tick) | `periodicUpdate_hook` / `hitByMelee_hook` | Pointers are compared as KEYS, never dereferenced in the hooks (stale-safe). Rebuilt by the Replicator after each drive pass; cleared on session reset. |
| Speed-intent state (`g_speedIntent*`, `g_quiet*`, `g_voteBtn*`) | speed hooks (intent), `writeGameSpeedQuiet` (quiet snapshot) | `consumeSpeedIntent` (EngineCharState.cpp) | `g_speedGuardWrite` marks our own quiet writes so they never count as user intent; vote-button snapshot restored after every guarded userPause. |
| `g_squadRoster` (`map<Character*, hand>`) | `pollSquadRoster` (EngineInternal.cpp) | same function (baseline diff) | Pointer keys survive re-containering; exits report stored hand with zeroed after, never dereferencing. `clearSquadRoster` on session swap. |

### Log-tag index (engine plane emitters)

Consumer column = oracle function in `scripts/CoopOracles.psm1` (completed in
the Stage 4 oracle split; "diag" = human/log-diff diagnostic only).

| Tag | Emitter | Meaning | Consumer |
|---|---|---|---|
| `[save] LOCAL-SAVE` | EngineInternal.cpp `saveMgrSave_hook` | every engine save entry (name, autosave, suppressed) | Test-SaveSync |
| `[load] LOCAL-LOAD` | EngineInternal.cpp `loadDetourEdge` | every engine load entry (name, suppressed, via=name/menu) | Test-LoadSync |
| `[recruit] LOCAL` | EngineInternal.cpp `recruit_hook` | successful recruit before/after hand pair | Test-RecruitSync |
| `[build] LOCAL-PLACE` / `LOCAL-DISMANTLE` | EngineInternal.cpp place/dismantle hooks | build-mode commit / dismantle edge (hand, sid, pos) | Test-BuildSync |
| `[fac] AFFECT-EV` / `AFFECT-AMT` | EngineInternal.cpp `recordFactionDelta` | engine relation mutation (5 s per-pair debounce; deltas still queue) | Test-FactionSync |
| `[shop] BUY-LOCAL` | EngineInternal.cpp `buyItem_hook` | real purchase through Inventory::buyItem | diag (protocol 22 groundwork) |
| `[shop] BUY-BEFORE/AFTER`, `ensure-stock`, `probe-buy` | EngineWorld.cpp `probeVendorBuy` | programmatic vendor-buy probe | diag |
| `[spawn] proxy` / `runtime` | EngineSpawnCombat.cpp spawn levers | proxy mint / runtime squad spawn results | Test-SpawnParity family |
| `[seatres]` (side=HOST/JOIN) | EngineEntity.cpp `logSeatResolveOnce` (also emitted from spawn/combat TU) | once-per-pair subject-hand resolve position check | diag (Stage 3a craft/seat) |
| `[med] LIMB-FIT` | EngineSpawnCombat.cpp | robot-limb fit path evidence | Test-LimbLoss |
| `[mk] *` / `[mkspy] *` / `[wpndiag] *` / `[tmpl] MISS` / `[recon] grp` / `[wd] scan` | EngineInventory.cpp | item mint/reconcile/weapon-fab diagnostics (spike 451) | diag; `[recon]`/inventory hashes feed Test-InventorySync |
| `[door] write`, `[build] mint/destroy/progress-write/probe-place`, `[prod] *` | EngineWorld.cpp | door/building/production write levers + probes | Test-DoorSync / Test-BuildSync / Test-ProdSync |
| `[taskkey] key` | EngineEntity.cpp `logTaskKeyOnce` | first sighting of each task key (self-documenting enum) | diag |
| `SETUP:` | EngineEntity/SpawnCombat scene helpers | scenario scene-setup progress | scenario oracles (setup evidence) |

<!-- Stage 2 (replication plane), Stage 3 (scenario plane) and Stage 4
     (oracle plane) sections are appended by their stages. -->
