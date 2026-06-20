# Kenshi Co-op: Autonomous Spike Investigation

This directory holds the findings of 50 investigative spikes run autonomously to
deepen our understanding of the Kenshi/Ogre environment and improve the co-op
workflow. Each spike has a `NN-slug.md` findings doc and a `NN/raw/` folder with
the captured logs/screenshots.

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

## Status legend

DONE = question answered with evidence. PARTIAL = partial evidence / some blocked.
BLOCKED = could not execute (reason recorded). PENDING = not yet run.

## Index

| # | Title | Type | Status | One-line finding |
|---|-------|------|--------|------------------|
| 1 | Spawn-N helper + cross-client hand resolution | DUMP | DONE | Runtime spawns get host-local hands; never reach join; local NPC populations diverge (host 3 vs join 15) |
| 2 | Deterministic battle-scene builder reproducibility | DUMP | DONE | Index per-session-deterministic, serial random; cross-client identity needs bake-then-load (createRandomCharacter randomizes) |
| 3 | Enumerate spawnable character/squad templates | DUMP | PENDING | |
| 4 | Faction creation/assignment + hostility control | DUMP | PENDING | |
| 5 | DLL-triggered save() to bake spawned scenes | RUN | PENDING | |
| 6 | Equip loadouts on spawned NPCs | DUMP | PENDING | |
| 7 | Env-parameterized scenarios | WORKFLOW | DONE | Implemented: SpikeScenario dispatches on KENSHICOOP_SPIKE; one build serves all probes via run_spikes.ps1 |
| 8 | Battle scale ceiling (tick time/FPS) | DUMP | DONE | Host holds ~90 idle NPCs at ~74fps (no hard ceiling); join unaffected (spawns don't replicate); combat-load TBD |
| 9 | Battle sync fidelity vs combatant count | RUN | PENDING | |
| 10 | Combat event storm (reliable channel) | RUN | PENDING | |
| 11 | Attribution correctness at scale | RUN | PENDING | |
| 12 | Battle bandwidth profile | RUN | PENDING | |
| 13 | Ragdoll/corpse-pile consistency | RUN | PENDING | |
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
| 28 | Trading/shop API surface | STATIC | PENDING | |
| 29 | Player money (Cats): scope + syncability | DUMP | PENDING | |
| 30 | Vendor proximity probe at 'c' | RUN | PENDING | |
| 31 | Purchase modeled as transfer + money delta | STATIC | PENDING | |
| 32 | Shared-economy conflict model | STATIC | PENDING | |
| 33 | Unused hookable vtable methods | STATIC | PENDING | |
| 34 | Game time / speed / pause control | DUMP | PENDING | |
| 35 | Camera / free-cam control | DUMP | PENDING | |
| 36 | In-game messages/notifications/dialog | DUMP | PENDING | |
| 37 | Weather/environment reads & control | DUMP | PENDING | |
| 38 | Programmatic orders/input surface | STATIC | PENDING | |
| 39 | Persisting custom coop state into the save | STATIC | PENDING | |
| 40 | Ogre version + scene graph access | STATIC | PENDING | |
| 41 | Ogre overlay rendering feasibility | DUMP | PENDING | |
| 42 | Animation system internals | STATIC | PENDING | |
| 43 | Worldspace -> zone/cell mapping | DUMP | PENDING | |
| 44 | Frame/tick model + main-thread guarantees | STATIC | PENDING | |
| 45 | Resource/mesh/name reads for UI | DUMP | PENDING | |
| 46 | Ogre overlay HUD proof | RUN | PENDING | |
| 47 | Peer-squad nameplates/markers | RUN | PENDING | |
| 48 | Connection/status + ping overlay | RUN | PENDING | |
| 49 | Minimap markers for peer squad | STATIC | PENDING | |
| 50 | MyGUI native panel integration | STATIC | PENDING | |
