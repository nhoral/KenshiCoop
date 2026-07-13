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

## Replication plane (`src/plugin/sync/`)

ONE `Replicator` class (Plugin.cpp drives ~35 entry points on `g_repl`);
`Replicator.h` unchanged by the split. The implementation is partial-class
across seven TUs; `ReplicatorUtil.h` is the shared private prelude (includes,
`nowMs()`, tuning constants, `dist3`/`isGearType`) - its helpers are
anonymous-namespace ON PURPOSE (per-TU internal copies, monolith semantics).
Never add file-scope mutable state there: cross-TU state is a class member.

### File inventory

| File | Responsibility |
|---|---|
| `ReplicatorCore.cpp` | ctor defaults, Phase 3 lifecycle audit (`lifeName`/`lifeSet`/`lifeSweep`), `resetSession` (the ONE reset point), `ingest`/`ingestInv`, tab latch/rank, `logSmoothSummary` |
| `ReplicatorPublish.cpp` | `publishOwned` (owned squad + near band + 2 Hz mid band + combat-intent capture incl. canonical-hand `CAP xlate`), `publishNpcCensus` |
| `ReplicatorDrive.cpp` | `applyTargets` (per-frame drive: walk/park/combat/carry/furniture/stealth + interp consumption + snap gates), `applyRest`, `sweepCarries`, `logHardSnap` |
| `ReplicatorAuthority.cpp` | `applyNpcCensus`, `enforceHostAuthority` (suppress/park/cull + far-mint requests), `parkDivergedCopy`, debug markers |
| `ReplicatorSpawn.cpp` | `syncSpawns` (protocol 21 mint incl. far mint + age/size), `applyEvents` (EVT_* reliable edges), `rekeyPeerBody` |
| `ReplicatorItems.cpp` | inventories (4a), world items (W1), weapon drops/pickups (W2), cross-owner transfers (protocol 37) |
| `ReplicatorChannels.cpp` | medical(+treatments)/stats/money/factions/doors/prod/research/builds(+doors)/recruits/squad-moves/stealth/speed/time channels, `onPeerConnected`; owns `medicalHash`/`statsHash`/`fillMedicalPacket` |

### Shared-hub contracts (Replicator members; authoritative comments in Replicator.h)

| Hub | Writer | Readers | Invariant / reset rule |
|---|---|---|---|
| `targets_` | net intake (`ingest`, Core) | drive tick (`applyTargets`) | Keyed by wire Key; entries carry interp buffers. Cleared in `resetSession`; rekeyed in place by `rekeyPeerBody`. |
| `ownHands_` / `tabRank_` | `publishOwned` (per tick) / `latchTabs` (on container list) | Items + Channels TUs (ownership scoping) | `ownHands_` is a PER-TICK snapshot - never carry it across ticks. Tab rank is deterministic (both clients sort the same container list). |
| `proxyByKey_` | `syncSpawns` (mint/cull) | Authority (census parks), Drive | A proxy Character* is valid only while its Key stays in the map; culls remove both. Cleared in `resetSession`. |
| `suppressed_` | `enforceHostAuthority`, Drive | Drive (skip), Authority (restore) | Suppression is reversible (restore on census corroboration); cleared in `resetSession`. |
| `censusHands_` | `applyNpcCensus` | `enforceHostAuthority` | Host-authored existence set with receive stamp; entries age out (stale census must not cull). Cleared in `resetSession`. |
| `life_` | `lifeSet` ONLY (called from every TU) | `lifeSweep`, lifecycle oracle (via `[life]` lines) | Audit layer over authority/mint/drive - never drives behavior. Swept on a timer; cleared in `resetSession`. |
| `drivenChars_` / `drivenSeen_` / `canonicalOf_` | drive tick (`applyTargets`) + starve-hold paths | `publishOwned` (CAP xlate), guards | Stamped only in the drive tick; pruned on `drivenSeen_`'s 30 s horizon; Character* compared, NEVER dereferenced after prune. Cleared in `resetSession`. |
| per-channel `*SeqOut_`/`*SampleMs_` | owning channel (Channels TU) | same channel | Seq monotonic per channel; sample throttles are per-channel cadence gates. Reset in `resetSession`. |

### Log-tag index (replication plane emitters)

| Tag family | Emitter TU | Consumer oracle (scripts/CoopOracles.psm1) |
|---|---|---|
| `[life]` | Core (`lifeSet`) | Test-EntityLifecycle |
| `[combat]` (order/CAP/CAP xlate), `[victim]` | Publish (capture) + Drive (order/apply) | Test-PlayerCombat / Test-AssaultTown / Test-CombatOrder |
| `[snap]`, `[interp]`, `[pose]`, `[ai]`, `[gate]`, `[trust]`, `[oi]` | Drive | Test-Smoothness / Test-AntiZombie / gate diagnostics |
| `[census]`, `[audit]`, `[authority]`, `[ck]` | Authority (+Publish send half) | Test-NpcParity / authority gates |
| `[spawn]`, `[limb]`, `[event]`, `[med]` (apply half) | Spawn | Test-SpawnParity / Test-LimbLoss / event oracles |
| `[inv]`, `[wi]`, `[wd]`, `[dk]`/`[pk]`/`[sk]`, `[xfer]`, `[key]` | Items | Test-InventorySync / Test-WorldItems / Test-WeaponConservation / Test-Transfers |
| `[stats]`, `[money]`, `[fac]`, `[door]`, `[bdoor]`, `[build]`, `[prod]`, `[research]`, `[recruit]`, `[squad]`, `[sneak]`, `[speed]`, `[time]`, `[latejoin]`, `[rank]` | Channels | matching per-channel oracles (Test-StatsSync, Test-MoneySync, ...) |
| `[carry]`, `[furn]` | Publish (SEND) + Drive (drive/sweep) + Spawn (RECV edges) | Test-CarrySync / Test-FurnitureSync |

## Scenario plane (`src/plugin/test/`)

`Scenario.h` is unchanged: `makeScenario(name)` is still the only public
entry point. `Scenario.cpp` is now just that factory, chaining the ten
per-domain makers declared in `ScenarioSupport.h` (each returns 0 when the
name is not its own). Scenario classes are TU-PRIVATE (anonymous namespace)
by design - to add a scenario, write the class in its domain TU and register
it in that TU's maker; nothing else changes. Shared SCENARIO-line emitters
(`logScenarioLine`/`logScenarioEntity`/`logVitalsLine`) and squad-tab
classification (`tabRankOf`/`tabLeaderIdx`/`handFromEntity`) live in
`ScenarioSupport.cpp` with external linkage.

### File inventory (which scenario lives where)

| File | Scenarios |
|---|---|
| `ScenarioSupport.cpp` | (helpers only - no scenarios) |
| `ScenarioMovement.cpp` | leader_move, fast_march, coop_presence, travel_parity, split_interest |
| `ScenarioNpc.cpp` | npc_sync, craft_order, down_order, death_order, spawn_probe, spawn_sync, npc_census, spawn_far |
| `ScenarioCombat.cpp` | combat_probe, combat_order, combat_kill, player_combat, assault_town, player_ko, combat_crowd |
| `ScenarioMedical.cpp` | medic_order, limb_loss, stats_sync |
| `ScenarioInventory.cpp` | inv_order, inv_bidir, trade_probe, trade_peer, inv_equip, inv_reequip, inv_wpnseq, inv_addequip, wpn_relocate |
| `ScenarioWorldItems.cpp` | drop_probe, world_item_sync, world_item_join, world_weapon_drop, world_armor_drop, weapon_loot |
| `ScenarioCharState.cpp` | carry_order, npc_carry, bed_pose, bed_put, cage_put, cage_peer_sync, sneak_probe, sneak_pose, sneak_detect, speed_sync, speed_probe |
| `ScenarioProbes.cpp` | spike (numbered-probe dispatcher), shop_probe, money_sync, vendor_trade, recruit_probe/sync, squad_probe/sync, faction_probe/sync, time_probe/sync, hunger_probe/sync |
| `ScenarioBuildings.cpp` | door_probe/sync, build_probe/sync, bdoor_probe/sync, prod_probe/sync, research_probe/sync, store_probe/sync |
| `ScenarioSession.cpp` | latejoin_probe/sync, save_probe, save_sync, save_stage1, resume_check, load_probe, load_sync |

### SCENARIO marker families (emitter domain -> consuming oracle)

`SCENARIO <MARKER> ...` lines are the scenario-to-oracle wire; phrasing and
field order are frozen. Emitters below are the domain TUs; consumers are the
oracle fragments (`scripts/oracles/*.ps1`, Stage 4).

| Marker family | Emitter TU(s) | Consumer oracle |
|---|---|---|
| `MEMBER` / `RECV` (via logScenarioLine/Entity) | every domain TU | Test-Crosscheck, Measure-NpcSync, Test-CoopPresence, Test-NpcPose* (Npc.ps1) |
| `VITALS` (via logVitalsLine) | Combat, Medical, Session | Get-VitalsSeries family (Medical.ps1) |
| `PCOMBAT`, `ASSAULT`, `PKO`, `CROWD` | Combat | Test-PlayerCombat / Test-AssaultTown / Test-PlayerKo / Test-CombatCrowd (Combat.ps1) |
| `CARRY`, `FURN`, `SNEAK*`, `SPEED` | CharState | Get-CarrySeries/Test-CarryOrder, Get-FurnSeries/Test-FurnPut, Test-Sneak*, Test-Speed* (Npc.ps1) |
| `INV`, `TINV`, `TRADE`, `EQUIP` | Inventory | Test-Inventory*/Test-Trade* (Inventory.ps1) |
| `DROP`, `GEAR`, `WITEM` | WorldItems | Test-DropProbe / Test-WorldItemSync / Test-WeaponDrop / Test-WeaponLoot (Inventory.ps1) |
| `VENDOR`, `WALLET`, `SHOPBUY`, `RECRUIT*`, `SQMOVE`/`SQEDGE`/`SQTABS`, `FACREL`/`FACWRITE`, `GTIME`, `HUNGER*` | Probes | Get-WalletSeries, Test-ShopProbe/MoneySync/VendorTrade, Test-Recruit*, Test-Squad*, Test-Faction*, Test-Time*, Test-Hunger* (World.ps1) |
| `DOOR*`, `BUILD*`, `BDOOR*`/`BDESTROY`, `PROD*`, `RESEARCH*`, `CONT*` | Buildings | Test-Door*/Build*/Bdoor*/Prod*/Research*/Store* (World.ps1) |
| `LJ*` (latejoin censuses), `SAVE*`, `LOAD*` | Session | Get-Latejoin*, Test-Latejoin*, Test-Save*/Load* (Session.ps1) |
| `SPAWN`, `WNPC`, `WORLD` (census rows) | Npc, Movement | Get-SpawnHands/Test-Spawn* (Npc.ps1), Get-WnpcRows/Test-TravelParity (Motion.ps1) |
| `SPIKE <id>` | Probes (SpikeScenario) | run_spike.ps1 evidence collection (diagnostic, ungated) |
| `RESULT pass=` | every domain TU (base-class exit) | Test-ScenarioResultPass (CoreChecks.ps1) |

## Oracle plane (`scripts/`)

`CoopOracles.psm1` is the module root: the gate infrastructure
(`Reset-/Add-/Get-GateResults`, `Merge-Status`), the manifest loader
(`Get-ScenarioManifest`), the central dispatch (`Invoke-OneOracle`,
`Invoke-RunAnalysis`) and the single `Export-ModuleMember`. The oracle
bodies live in `scripts/oracles/*.ps1`, DOT-SOURCED into the module scope,
so `$script:` state (Gates, ClockOffsetCache, the `*Regex` patterns) is
shared exactly as before the split. Importers (`run_test.ps1`,
`analyze_run.ps1`, `regress.ps1`) are unchanged.

### File inventory (which oracle lives where)

| Fragment | Contents |
|---|---|
| `oracles/Parsing.ps1` | clock offset + series parsers: Get-LogClockOffsetMs, Get-ClockSyncStats, Convert-StampToMs, Get-ScenarioLines, Get-ScenarioSeries, Get-MarkerTimeMs |
| `oracles/CoreChecks.ps1` | every-scenario gates: Test-LogHealth, Test-NoCheckFail, Test-ScenarioResultPass, Test-ClockSync |
| `oracles/Npc.ps1` | Test-Crosscheck, Measure-NpcSync, Test-NpcTrack, Test-CoopPresence, pose/body-state, craft/down/death orders, carry/furniture/cage, sneak, speed, split_interest, spawn probes, Test-SpawnSync, Test-NpcCensus |
| `oracles/Combat.ps1` | Test-CombatProbe/Order/Kill, Test-DamageGuard, Test-PlayerCombat, Test-AssaultTown, Test-PlayerKo, Test-CombatCrowd |
| `oracles/Medical.ps1` | Get-VitalsSeries, Test-NpcVitals, Test-LimbLoss, Get-StatsSeries, Test-StatsSync, Test-MedicOrder |
| `oracles/Inventory.ps1` | Test-Inventory*/Trade*, Test-DropProbe, Test-WorldItemSync, Test-WpnRelocate, Test-WeaponDrop, Test-WeaponLoot |
| `oracles/World.ps1` | wallet/shop/money/vendor, recruit, squad, faction, time (+Get-SlewSummary), door, build, bdoor, hunger, prod, research, store oracles |
| `oracles/Session.ps1` | latejoin census parsers + probes, save/load/resume oracles |
| `oracles/Motion.ps1` | Test-Smoothness, Test-AnimTruth, Test-MarchInPlace, Test-SnapRate, Test-SuppressChurn, Test-SpawnFarBind, Test-RestFlap, Test-ExistenceParity, travel_parity family, Test-AntiZombie, Test-Lifecycle, Test-MintDistance |

Rules: fragments define functions only (nothing runs at dot-source time
except `$script:*Regex` assignments); gate names and marker regexes are
frozen API; a new oracle goes in its domain fragment, its dispatch entry in
`Invoke-OneOracle`, its export in the root `Export-ModuleMember`, and its
gate wiring in `scenarios.psd1`.
