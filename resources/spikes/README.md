# Kenshi Co-op: Autonomous Spike Investigation

This directory holds the findings of an autonomous spike investigation that
deepens our understanding of the Kenshi/Ogre environment and improves the co-op
workflow. Each spike has a `NN-slug.md` findings doc and a `NN/raw/` folder with
the captured logs/screenshots.

The investigation is a **living queue**, not a fixed count: 27 spikes are complete
(indexed below), and the active TODO queue (~423 items: 23 carried-over originals +
400 new spikes across 19 themes) lives in [BACKLOG.md](BACKLOG.md). The loop pulls
the lowest-numbered non-DONE id, runs it, commits its findings, reverts code, and
moves on.

## How spikes run

A single generic scenario, `SpikeScenario` in
[../../src/plugin/test/Scenario.cpp](../../src/plugin/test/Scenario.cpp), is
selected by the `KENSHICOOP_SPIKE=<id>` env var and dispatches to the per-spike
probe. Drivers:

- `scripts/run_spike.ps1 -Id <n> -Save c` - build+deploy+run+collect one spike.
- `scripts/run_spikes.ps1 -Ids 1,3,8 -Save c` - one build, many probes, skip-on-error.

Execution types: **RUN** (networked sync test), **DUMP** (host-only engine
enumeration), **STATIC** (SDK-header / code / binary analysis, no game run).

Workflow: implement a probe -> build -> run -> write `NN-slug.md` -> commit on the
`spikes` branch -> `git restore` the experimental code -> next. Findings persist;
experimental scaffolding does not (only the harness baseline stays).

## Validation rule (required)

Every findings doc MUST contain a `## Validation` section that says, per finding,
exactly how it was validated (runtime `SPIKE` log lines on host/join, or a
`path:line` / `header:line` citation). Do NOT state anything as a Finding unless it
is validated this way - unverified ideas go under
`## Open questions / hypotheses (UNVALIDATED)` with the test that would confirm them.
See [_TEMPLATE.md](_TEMPLATE.md).

## Status legend

DONE = every finding validated (per the Validation rule), question answered.
PARTIAL = some findings validated; the core question still has an open/unvalidated
hypothesis. BLOCKED = could not execute (reason recorded). PENDING = not yet run.

## Index (completed)

Pending/queued spikes are not listed here - see [BACKLOG.md](BACKLOG.md). This table
is the record of completed (DONE/PARTIAL) spikes.

| # | Title | Type | Status | One-line finding |
|---|-------|------|--------|------------------|
| 1 | Spawn-N helper + cross-client hand resolution | DUMP | DONE | Runtime spawns get host-local hands; never reach join; local NPC populations diverge (host 3 vs join 15) |
| 2 | Deterministic battle-scene builder reproducibility | DUMP | DONE | Index per-session-deterministic, serial random; cross-client identity needs bake-then-load (createRandomCharacter randomizes) |
| 3 | Enumerate spawnable character/squad templates | DUMP | DONE | getDataOfType(list,itemType) enumerates RACE/HUMAN_CHARACTER/SQUAD_TEMPLATE; RaceData::AllRaces; deterministic spawn by stringID |
| 4 | Faction creation/assignment + hostility control | DUMP | DONE | FactionManager::getOrCreateFaction + relations->setEnemy/declareWar + createRandomSquad -> auto-fighting battles, no per-NPC orders |
| 5 | DLL-triggered save() to bake spawned scenes | RUN | DONE | SaveManager::getSingleton()->save(name,autosave) bakes spawns into cross-client-resolvable saves (proven by duel1/down1) |
| 6 | Equip loadouts on spawned NPCs | DUMP | DONE | createItem + Inventory::equipItem / chooseMyClothing; baked loadouts resolve on both clients |
| 7 | Env-parameterized scenarios | WORKFLOW | DONE | Implemented: SpikeScenario dispatches on KENSHICOOP_SPIKE; one build serves all probes via run_spikes.ps1 |
| 8 | Battle scale ceiling (tick time/FPS) | DUMP | DONE | Host holds ~90 idle NPCs at ~74fps (no hard ceiling); join unaffected (spawns don't replicate); combat-load TBD |
| 9 | Battle sync fidelity vs combatant count | RUN | PARTIAL | Baked 5v5/10v10/20v20 melees; all combatants resolve+replicate to join, body-state agree 47/47 at 40-body; divergence flat (~18-20u) with count - bottleneck is interest-scatter, not load; absolute lag confounded by clock-skew (follow-up) |
| 10 | Combat event storm (reliable channel) | RUN | DONE | Flooded KO/REVIVE at 84/s (peak 103/s): 2798/2798 delivered, contiguous ids, 0 loss/dup - reliable channel never drops; backpressure = latency tail (med 34ms, p95 753ms, max 2.3s). Real combat ~0.09/s (1000x headroom). Loopback only; NetSim follow-up |
| 11 | Attribution correctness at scale | RUN | PARTIAL | Driven melee with 47 concurrent attacker->victim pairs (64 victims targeted); every emitted combat KO (4/4) attributed to a real attacker, 0 mis-attributions, 0 unattributed - recency map doesn't cross-contaminate. Death sample small (blood=12 not lethal); swarm/long-bleed + death-storm cases open |
| 12 | Battle bandwidth profile | RUN | DONE | Stream = full 20 Hz snapshot, no delta; fixed 79 B/body/tick = 1.58 KB/s per streamed body per peer. Host upload linear in body count (battle40 ~30 KB/s); chunks at 18 bodies/datagram; join uploads only its own squad (~1 KB/s flat). Worst case (160-cap) ~253 KB/s/peer. Bandwidth not the battle bottleneck |
| 13 | Ragdoll/corpse-pile consistency | RUN | DONE | Killed 46-body pile: corpse positions agree med 4.3u (~4x tighter than live, no clock-skew confound) and are stationary on both clients (drift ~0). Pose replicates (join shows DOWN+RAGDOLL body=3) but DEAD bit does NOT (host body=7); join isDead() stays false - looting/permadeath must use EVT_DEATH not local flag |
| 14 | Interest cap overflow behavior | DUMP | DONE | Caps: 96 far+96 near per query, MAX_PUBLISH=160/tick; overflow truncates silently (no crash, no priority) |
| 15 | Measure current host interest radius | DUMP | DONE | World NPCs stream within 200u far/120u near of HOST leader; ground items only 60u; single host-centered sphere |
| 16 | Leader-separation: peer update cutoff distance | RUN | PARTIAL | Peer SQUAD always syncs (no cutoff); shared WORLD degrades past 200u from host leader; runtime walk-apart recipe noted |
| 17 | Un-streamed NPC behavior on the join | RUN | PARTIAL | Persistent population divergence (host 3-7 vs join 9-15) despite enforceHostAuthority; needs per-hand classification probe |
| 18 | Interest-boundary hysteresis / churn | RUN | PARTIAL | Hard 200u radius, no hysteresis/dwell -> boundary churn expected; add enter/exit band |
| 19 | Dual-interest feasibility | DUMP | DONE | Feasible+small: 2nd captureNpcs centered on join leader, merge/dedupe, raise caps, sphere-aware suppression |
| 20 | Distance rate-tiering vs divergence | RUN | PARTIAL | No per-entity LOD today; tiered cadence feasible, bounded by interp; force combatants to tier 0 |
| 21 | Health/medical model field map | STATIC | DONE | MedicalSystem is a large local-only model (blood, bleed, 4 limb HealthPartStatus, RobotLimbs states, wounds); none on the wire |
| 22 | Limb-loss replication | RUN | DONE | Host severed left arm; join's same baked body stayed LA=100 all run - limb loss does NOT replicate |
| 23 | Bandage/first-aid detection + replication | RUN | DONE | First-aid heals local medical only; no wire field; effects don't replicate |
| 24 | Sleeping in a bed (task+subject pose) | RUN | PARTIAL | Sleep POSE should replicate via sit/craft node-AI path; rest benefit is local medical (no sync); pose run deferred |
| 25 | Bleed-out progression sync | RUN | DONE | Host bled 75->0, join stayed 75.8; blood not synced + regenerates locally - stream collapse EDGE not blood |
| 26 | bodyState bitfield gaps | DUMP | DONE | Join medical flags never moved even after host KO+kill; bodyState=pose only, misses blood/limbs/dead-flag |
| 27 | Revive/recovery sync + EVT_REVIVE | RUN | DONE | Recovery is local medical; no EVT_REVIVE; needs paired BODY_KO/BODY_UP edges + set join medical flags |
| 28 | Trading/shop API surface | STATIC | DONE | ShopTrader(getInventory/getMoney/takeMoney); Inventory::buyItem (no sellItem); price=getValueSingle x TradeCulture mult x Platoon mult |
| 29 | Player money (Cats): scope + syncability | DUMP | DONE | Money is PER-PLATOON (Ownerships.money) + inventory money-items; no global/per-faction wallet; per-squad host-auth sync is cheap |
| 30 | Vendor proximity probe at 'c' | RUN | PARTIAL | Find vendors via getObjectsWithinSphere filter SHOP_TRADER_CLASS; 'c' likely has no shop - needs a market save/bake |
| 31 | Purchase modeled as transfer + money delta | STATIC | DONE | Purchase = conserved item transfer + per-platoon money int delta; reuses inventory-conservation; host-authoritative |
| 32 | Shared-economy conflict model | STATIC | DONE | Per-squad wallets (SDK-native) + host-auth vendors + conservation avoids double-spend/dup without locks |
| 33 | Unused hookable vtable methods | STATIC | DONE | Mod hooks only 3 fns (mainLoop, title-update, periodicUpdate) via KenshiLib _NV_ detours, no vtable patching. High-value unhooked twins: Character::_NV_hitByMeleeAttack + declareDead (damage/death), Inventory::_NV_dropItem (drop), PlayerInterface::_NV_factoryObjectCreatedCallback (spawn), per-entity _NV_update/MedicalSystem. Thread-affinity unverified |
| 34 | Game time / speed / pause control | DUMP | DONE | Full named API on GameWorld (setGameSpeed/togglePause/userPause/isPaused/frameSpeedMult/clock). Levers work; paused bool & frameSpeedMult are 2 distinct knobs (effective pause = paused\|\|fsm==0). Speed/pause is PER-CLIENT LOCAL - 18/21 wall-aligned samples diverged (host x5 vs join x1; host running vs join paused). Co-op needs a speed/pause policy; unfocused-window auto-pause (fsm->0) is a hazard (hypothesis) |
| 35 | Camera / free-cam control | DUMP | DONE | Camera via player->getCamera() (CameraClass, fully named). All levers work at runtime: focusCameraOnObject (jumped cam onto leader), setFreeCameraMode (flag 0/1), teleport (moved cam ~+1560/axis). Camera is per-client local, absent from wire protocol - safe UI-only. Unblocks jump-to-peer / follow-peer / spectator cam |
| 36 | In-game messages / notifications | DUMP | DONE | GameWorld::showPlayerAMessage(text,queued) renders an arbitrary string in the on-screen message bar (verified by reading the screenshot - "KENSHICOOP SPIKE 36 NOTIFY" visible). Safe from main loop, ok=1 every call, local UI. Unblocks co-op notices (peer joined/downed); wire EVT_*->message for a kill feed. (SEH must avoid std::string in __try: C2712) |
| 37 | Weather/environment reads & control | DUMP | PARTIAL | GameWorld::getWindSpeed(pos)/getLightLevel(pos,floor,inside) + per-char MedicalSystem.currentWeatherAffect (WeatherAffecting: WA_NONE/DUSTSTORM/ACID/BURNING/GAS/RAIN) + strength all readable, fault-free. Host vs join bit-identical for shared position (wind 12.57, light 1.0, WA_NONE) - reads are pure fns of (pos,world). UNVALIDATED: non-zero weather never observed (no hazard region/no dusk in 30s), and control (PhysicsCollection::addGlobalEffect) NOT exercised. Day/night light may diverge under local clock drift (spike 34) |
| 38 | Programmatic orders / input surface | STATIC | DONE | Order layer (runtime-proven in Engine.cpp): Character::setDestination(pos,shift)=click-to-move; addOrder/addJob(TaskType,subject,loc)=right-click order/job queue; addGoal/clearAllAIGoals=persistent AI goal (attack via UNPROVOKED_FOCUSED_MELEE_ATTACK). Orders = (TaskType enum + subject + Vector3) -> generic issueOrder RPC feasible. Bare CharMovement::setDestination is overwritten by AI for player chars. Input: InputHandler singleton Globals::key exposes live state + bindings + sendEvent(name); no clean keypress-inject (state via private OIS cbs). Input layer UNVALIDATED (mod never touches Globals::key) |
| 39 | Persisting custom coop state into the save | STATIC | DONE | Three channels: (A) NATIVE in-band - GameDataManager::save/load + GameData::loadGameDataReturn carry a Serialisable* moreData extra-data slot (cleanest, atomic) but Serialisable is unreversed; (B) custom GameData record via saveToFile/loadFromFile/addANewInstancedObject (risky, typed container); (C) SIDECAR file keyed by SaveManager::getCurrentGame()/getSavePath(), load-timed off the existing LOADGAME deferred signal - only channel feasible TODAY, zero native-save risk. SaveManager singleton + load are runtime-proven (spike 5); getCurrentGame/getSavePath strings + Serialisable + save-completion hook all UNVALIDATED. Mod currently persists nothing (reconstructs on load) |
| 40 | Ogre version + scene graph access | STATIC | DONE | Ogre = 2.0.0 "Tindalos" (unstable) - early experimental 2.0 (SoA nodes, SCENE_DYNAMIC/STATIC, HLMS, compositor); NOT 1.x or stable 2.1+. Scene graph reachable via the spike-35 CameraClass (Ogre::Camera* camera @0x68, getCameraNode 0x100930, getCenterNode 0x166E30) -> getSceneManager/getRootSceneNode. CRITICAL: zero Ogre methods are RVA-annotated (vs ~1000s for kenshi::), so Ogre's own fns are NOT GetRealAddress-able - must use header-inline methods or raw member offsets; object CREATION (nodes/overlays) is the blocked part. Mod uses no Ogre today. UNVALIDATED: never dereffed an Ogre obj; recommend MyGUI/ForgottenGUI for HUD over raw Ogre |
| 41 | Ogre overlay rendering feasibility | DUMP | PARTIAL | Raw Ogre Overlay AND raw MyGUI both have ZERO RVA-annotated symbols -> NOT GetRealAddress-able, infeasible. Feasible HUD layer = Kenshi's ForgottenGUI (120 RVAs): createDatapanel->DatapanelGUI* (0x73EE10), messageBox->MyGUI::Window* (0x740F60), showMainbar/showNames, + GameWorld::showPlayerAMessage (0x723830, runtime-proven in spike 36 = only validated render path). BLOCKER/UNVALIDATED: no ForgottenGUI instance accessor found (no getSingleton; Globals::gui is dllimport w/ unproven linkability; no GameWorld getter) -> custom-widget render unproven, probe deferred until instance access solved. Spikes 46-48 should target ForgottenGUI, not raw Ogre |
| 47 | Peer-squad nameplates/markers (HUD render proof) | RUN | DONE | RENDER PROVEN (screenshot, both clients): ForgottenGUI::createScreenLabel(text,colour,size,rising) 0x73E920 mints a HUD text label via Globals::gui; ScreenLabel::_NV_setTracking(hand,offset) 0x6E1BB0 PINS it to a character so the engine projects+follows the body every frame = a nameplate (zero world->screen math by us); _NV_setRisingSpeed(RS_STOPPED) 0x6E1C10 keeps it persistent. Amber "PEER: leader" visible on the leader body on host_3.png + join_3.png, distinct from Kenshi's native white showNames plate. label ptr non-null both roles (H 0x0B273A60 / J 0x0AA931A0), persisted whole run, no fault, clean exit. SEH split (C2712) like spike 36. UNVALIDATED: tracking a PEER (remote) body specifically (tracked local leader for the shot; mechanism identical), lifecycle/destroy(ScreenLabelInterface*) 0x6E9080 on join/leave, live _NV_setCaption/_NV_setColor for downed/ping states, off-screen markers/minimap (spike 49 = different widget). Keeper: engine::hudNameplate |
| 46 | Ogre overlay HUD proof (ForgottenGUI instance) | RUN | PARTIAL | Unblocks spike 41's HUD blocker: Globals::gui (the __declspec(dllimport) ForgottenGUI* global) LINKS into the mod (kenshilib.lib exports it) and is non-null + stable for the whole session on BOTH clients (host gui=0x...61242750, join gui=0x...215F2750, identical across all 13 samples t=1516->19516; values differ by process = expected ASLR, never sent over wire). Instance access to ForgottenGUI is now SOLVED -> build HUD on createDatapanel/messageBox/createScreenLabel via gui. UNVALIDATED: no widget actually rendered here (only the instance ptr proven; render proof = spike 47/48), thread/lifetime safety of dereferencing gui across load/save untested, gui validity at boot/main-menu untested (null-guard still required) |
| 45 | Resource/mesh/name reads for UI | DUMP | PARTIAL | Race name VALIDATED: getRace()->RaceData::data(+0x40)->GameData::name(+0x28) read 'Greenlander' identically on both clients all run (safe plain virtual + 2 guarded hops). Generalises the mod's existing GameData::name/stringID reads (items/materials/manufacturers/templates, runtime-proven) to any GameData* record. UNVALIDATED: character DISPLAY name (no clean getName; CharBody 0x639930 std::string-by-value candidate untested), Faction::getName (0x286780; no public char->Faction getter), live-inventory item names. Mesh/Ogre-resource names BLOCKED (spike 40: no Ogre RVAs) |
| 44 | Frame/tick model + main-thread guarantees | STATIC | DONE | VARIABLE-timestep main loop via GameWorld::mainLoop_GPUSensitiveStuff(float dt) RVA 0x7877A0 (mod hooks _NV_ twin; runtime-proven by every spike's per-frame probe). Sim advance = dt*frameSpeedMult; pause = paused(0x8B9)\|\|fsm==0. Multi-threaded: render/main + AI non-render thread (_AINonRenderThread 0x790) + audio thread (0x8C0) + thread-safe ragdoll (0x7D1120) + mod's net thread. Main-loop is the SAFE point; mod runs engine first then drives last. Loader-lock hazard PROVEN: must TerminateProcess (ExitProcess deadlocks vs live GPU/audio/net threads). Destruction is phased/deferred (killListPhase0/1/2). UNVALIDATED: AI-thread-owned fields + sub-phase ordering unmapped |
| 43 | Worldspace -> zone/cell mapping | DUMP | PARTIAL | RootObject::getZoneMapLocation() (RVA 0x593B90, public virtual) returns the leader's ZoneMap* - non-null + bit-stable all run on BOTH clients (inzone=1, no churn while parked). GameWorld::zoneMgr @0x8B0 valid. Zone loading is DETERMINISTIC across clients: zmap-zmgr delta identical (0x735C8) on host & join despite different bases. Static surface mapped: ZoneManager::getZoneMap(pos)/mapFromResolutionCoord->iVector2 cell coords; ZoneSpacialGrid::getFullKey/getZoneKey (private, uint, content keys) + getObjects spatial query + cellSize. UNVALIDATED: no content NAME/KEY extracted (ZoneMap stripped; keys private) so cross-client zone IDENTITY only inferred from ptr-offset; cell-level + zone-crossing untested |
| 42 | Animation system internals | STATIC | DONE | 3 layers: (1) NAME-driven entry Character::runSlaveAnim(name,speed,sync) RVA 0x5B0930 (already hooked by mod) + CharBody::getUpFromRagdoll(name); (2) SELECTION inputs = CharMovement state (currentlyMoving/speed/movementMode/swordState) + Tasker action -> engine derives the clip; (3) AnimationClass itself is OBFUSCATED/stripped in all 8 headers (pointer reachable via getAnimationClass 0x645E0 but no layout -> no bone/track control). Production model is STATE-driven (validated: harness anim-truth PASS translateFrames=4737, march-in-place PASS - ghost plays real advancing clip from streamed movement, zero anim bytes). UNVALIDATED: clip-name vocab NOT harvested (runSlaveAnim hook emitted no SLAVEANIM lines - may be follower-only); driving runSlaveAnim untested |

Carried-over originals 33-50 are still pending and live in
[BACKLOG.md](BACKLOG.md) along with new spikes 51-450. As each completes it is
moved into this table.
