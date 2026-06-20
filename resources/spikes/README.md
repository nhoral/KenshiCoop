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

Carried-over originals 33-50 are still pending and live in
[BACKLOG.md](BACKLOG.md) along with new spikes 51-450. As each completes it is
moved into this table.
