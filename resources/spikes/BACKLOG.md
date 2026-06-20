# Kenshi Co-op: Spike Backlog (living queue)

This is the live TODO queue for the autonomous spike loop. The loop pulls the
**lowest-numbered non-DONE id** that is not yet committed, runs it, writes
`NN-slug.md` + `NN/raw/`, commits on the `spikes` branch, reverts experimental
code, and moves on. Completed spikes are recorded in [README.md](README.md).

**Validation rule (required for every item):** each findings doc must include a
`## Validation` section stating, per finding, exactly how it was confirmed (a quoted
`SPIKE NN ...` runtime log line on host/join, or a `path:line` / `header:line`
citation). Unvalidated ideas are NOT findings - they go under
`## Open questions / hypotheses (UNVALIDATED)` with the test that would confirm
them. Status is DONE only when every finding is validated. See [_TEMPLATE.md](_TEMPLATE.md).

Type tags: **RUN** (networked sync test), **DUMP** (host-only engine enumeration),
**STATIC** (SDK-header / code / binary analysis), **WORKFLOW** (harness/tooling).

---

## Carried-over pending originals (run first)

These 23 were queued in the original 50 and not yet completed; they keep their ids.

### Battle RUN (9-13)

| # | Title | Type |
|---|-------|------|
| 9 | Battle sync fidelity vs combatant count | RUN | *(DONE - see [09](09-battle-sync-fidelity.md), README)* |
| 10 | Combat event storm (reliable channel) | RUN | *(DONE - see [10](10-combat-event-storm.md), README)* |
| 11 | Attribution correctness at scale | RUN | *(PARTIAL - see [11](11-attribution-correctness-at-scale.md), README)* |
| 12 | Battle bandwidth profile | RUN | *(DONE - see [12](12-battle-bandwidth-profile.md), README)* |
| 13 | Ragdoll/corpse-pile consistency | RUN |

### Group F - DLL/engine capabilities (33-39)

| # | Title | Type |
|---|-------|------|
| 33 | Unused hookable vtable methods | STATIC |
| 34 | Game time / speed / pause control | DUMP |
| 35 | Camera / free-cam control | DUMP |
| 36 | In-game messages/notifications/dialog | DUMP |
| 37 | Weather/environment reads & control | DUMP |
| 38 | Programmatic orders/input surface | STATIC |
| 39 | Persisting custom coop state into the save | STATIC |

### Group G - Ogre / Kenshi internals (40-45)

| # | Title | Type |
|---|-------|------|
| 40 | Ogre version + scene graph access | STATIC |
| 41 | Ogre overlay rendering feasibility | DUMP |
| 42 | Animation system internals | STATIC |
| 43 | Worldspace -> zone/cell mapping | DUMP |
| 44 | Frame/tick model + main-thread guarantees | STATIC |
| 45 | Resource/mesh/name reads for UI | DUMP |

### Group H - Coop UI (46-50)

| # | Title | Type |
|---|-------|------|
| 46 | Ogre overlay HUD proof | RUN |
| 47 | Peer-squad nameplates/markers | RUN |
| 48 | Connection/status + ping overlay | RUN |
| 49 | Minimap markers for peer squad | STATIC |
| 50 | MyGUI native panel integration | STATIC |

---

## New spikes (51-450)

Seeded from the follow-ups recorded in the completed findings (spawnHostileBattle,
bakeScene, MedicalDelta + EVT_LIMB/EVT_REVIVE, dual-interest, hysteresis band,
rate-tiering, per-hand classification, BUY_INTENT, showPlayerAMessage HUD, overlay
nameplates, follow-peer cam, GameData custom-save blob, etc.).

### Theme 1 - Combat / battle load & sync (51-80)

| # | Title | Type |
|---|-------|------|
| 51 | spawnHostileBattle(perSide,red,blue) helper + 10v10 baseline sync | RUN |
| 52 | Combat-load FPS curve (host): 5v5/10v10/20v20/40v40 | RUN |
| 53 | Combat-load FPS curve (join) while host fights | RUN |
| 54 | Attribution correctness with 2 simultaneous fights | RUN |
| 55 | Attribution correctness with 4+ simultaneous fights | RUN |
| 56 | KO event storm: reliable-channel backpressure at 20v20 | RUN |
| 57 | Death event storm ordering/loss under load | RUN |
| 58 | Per-count bandwidth: bytes/s vs combatant count | RUN |
| 59 | Corpse-pile consistency host vs join after mass death | RUN |
| 60 | Ragdoll settle-position agreement across clients | RUN |
| 61 | Weapon-swing animation sync during melee | RUN |
| 62 | Projectile (crossbow bolt) flight sync | RUN |
| 63 | Archery hit/miss agreement across clients | RUN |
| 64 | Knockback impulse/displacement sync | RUN |
| 65 | Blood-FX / hit-spark replication | RUN |
| 66 | Flee/retreat decision agreement under morale break | RUN |
| 67 | Target-selection agreement (who attacks whom) | RUN |
| 68 | Friendly-fire incidence + attribution | RUN |
| 69 | Animal/beast battle (bonedogs) sync | RUN |
| 70 | Large-beast single-target load | RUN |
| 71 | Mixed melee+ranged blob sync fidelity | RUN |
| 72 | Stagger/stun state replication | RUN |
| 73 | Block/parry/dodge state sync | RUN |
| 74 | Weapon drawn/sheathed state sync in combat | RUN |
| 75 | Combat start/stop edge-detection latency | RUN |
| 76 | Multi-wave reinforcement spawn sync | RUN |
| 77 | Combat near interest boundary (combatants at 200u) | RUN |
| 78 | Host-vs-join FPS divergence under identical combat | RUN |
| 79 | Combat with prisoners/downed mixed in | RUN |
| 80 | Sustained 5-min battle drift/desync accumulation | RUN |

### Theme 2 - Spawning & scene-bake tooling (81-102)

| # | Title | Type |
|---|-------|------|
| 81 | spawnHostileBattle engine helper primitive | DUMP |
| 82 | bakeScene(name) auto-save + reload helper | DUMP |
| 83 | Deterministic spawnTemplateById exactness check | DUMP |
| 84 | equipByStringId loadout helper | DUMP |
| 85 | Squad spawn via createRandomSquad parameters | DUMP |
| 86 | Animal squad spawn templates enumeration | DUMP |
| 87 | Building/structure spawn feasibility | DUMP |
| 88 | Baked test-save library: battleN saves | WORKFLOW |
| 89 | Baked test-save library: medN saves | WORKFLOW |
| 90 | Baked test-save library: shopN saves | WORKFLOW |
| 91 | Seeded vs random spawn reproducibility | DUMP |
| 92 | Spawn at explicit world coords vs near-leader | DUMP |
| 93 | Spawn faction assignment at creation | DUMP |
| 94 | Spawn with specific stats/skills | DUMP |
| 95 | Spawn count ceiling before instability | DUMP |
| 96 | Despawn/cleanup of spawned NPCs | DUMP |
| 97 | Spawn item stacks of given quality/material | DUMP |
| 98 | Spawn container/storage with contents | DUMP |
| 99 | Scene-bake manifest format (what's in each save) | WORKFLOW |
| 100 | Spawn caravan/trader squad | DUMP |
| 101 | Spawn unique/named character templates | DUMP |
| 102 | One-command rebuild of full baked-save library | WORKFLOW |

### Theme 3 - Interest management & roaming (103-130)

| # | Title | Type |
|---|-------|------|
| 103 | Dual-interest prototype: 2nd captureNpcs on join leader | RUN |
| 104 | Dual-interest merge/dedupe correctness | RUN |
| 105 | Enter/exit hysteresis band prototype | RUN |
| 106 | Hysteresis dwell-time tuning sweep | RUN |
| 107 | Distance rate-tiering scheduler prototype | RUN |
| 108 | Rate-tier cadence vs interpolation smoothness | RUN |
| 109 | Per-hand streamed/local/host classification (spike 17 v2) | DUMP |
| 110 | Suppression-correctness audit for non-streamed NPCs | RUN |
| 111 | Zone-based interest vs radius-based comparison | RUN |
| 112 | Item-radius (60u) vs NPC-radius (200u) parity fix test | RUN |
| 113 | Teleport handoff: interest recompute after jump | RUN |
| 114 | Fast-travel handoff interest correctness | RUN |
| 115 | Join-leader-led roaming away from host | RUN |
| 116 | Both leaders roaming opposite directions | RUN |
| 117 | Interest with 3+ peers (scaling) | RUN |
| 118 | Interest center policy (host-only vs union) | DUMP |
| 119 | Moving interest-sphere churn measurement | RUN |
| 120 | NPC re-stream on re-entry identity stability | RUN |
| 121 | Suppressed-NPC state on re-entry (snap vs drift) | RUN |
| 122 | Item interest expansion to match NPC radius | RUN |
| 123 | MAX_PUBLISH=160 saturation under dense roam | RUN |
| 124 | Cap-overflow priority (combatants first) prototype | RUN |
| 125 | Interest during vehicle/mount movement | RUN |
| 126 | Zone-boundary crossing event hooks | DUMP |
| 127 | Sleeping/idle NPC suppression at distance | RUN |
| 128 | Interest radius auto-tuning by density | RUN |
| 129 | Cross-zone peer visibility | RUN |
| 130 | Interest persistence across save/load | RUN |

### Theme 4 - Medical sync (131-154)

| # | Title | Type |
|---|-------|------|
| 131 | MedicalDelta wire-struct design | STATIC |
| 132 | MedicalDelta blood-only replication prototype | RUN |
| 133 | EVT_LIMB limb-loss event replication | RUN |
| 134 | EVT_REVIVE recovery event replication | RUN |
| 135 | Collapse (KO) EDGE replication + set join unconcious | RUN |
| 136 | Death EDGE replication + set join medical.dead | RUN |
| 137 | Real addWound bleed-rate replication | RUN |
| 138 | First-aid (bandage) event replication | RUN |
| 139 | Doctoring-skill heal event replication | RUN |
| 140 | Rigging/prosthetic attach event replication | RUN |
| 141 | Robot-limb state replication | RUN |
| 142 | Hunger/starvation value sync | RUN |
| 143 | KO-timer countdown sync | RUN |
| 144 | Bleed-out -> death pipeline cross-client | RUN |
| 145 | Blood-regeneration policy (freeze on join?) | RUN |
| 146 | Multiple-wound aggregation sync | RUN |
| 147 | Healing-over-time (rest) sync policy | RUN |
| 148 | Crippled-flag replication | RUN |
| 149 | Bloodloss-trauma flag replication | RUN |
| 150 | Medical-delta bandwidth budget | STATIC |
| 151 | Medical sync for peer-squad vs world NPC | RUN |
| 152 | Reattach/replace limb (prosthetic) sync | RUN |
| 153 | Poison/toxin/acid medical-effect sync | RUN |
| 154 | Medical state on re-stream after suppression | RUN |

### Theme 5 - Economy & trading (155-178)

| # | Title | Type |
|---|-------|------|
| 155 | BUY_INTENT join->host message prototype | RUN |
| 156 | Sell path: confirm absent sellItem, design sell intent | STATIC |
| 157 | Sell-path implementation prototype | RUN |
| 158 | Vendor-stock sync host->join | RUN |
| 159 | Per-platoon ownerships.money delta replication | RUN |
| 160 | Money-item conservation across buy | RUN |
| 161 | Stolen/illegal-goods purchase handling | RUN |
| 162 | Bar/food purchase (consumable services) | RUN |
| 163 | Healing-service purchase | RUN |
| 164 | Training-service purchase | RUN |
| 165 | Bounty payout at police/faction | RUN |
| 166 | Double-buy race (both peers buy last stock) | RUN |
| 167 | Price agreement host vs join (TradeCulture mult) | RUN |
| 168 | Haggle/trade-skill price-effect sync | RUN |
| 169 | Vendor restock-timer sync | RUN |
| 170 | Selling looted gear money delta | RUN |
| 171 | Buying from multiple vendors concurrently | RUN |
| 172 | Shop UI opened by join routed to host | RUN |
| 173 | Money-split policy across squads | RUN |
| 174 | Global vs per-squad wallet UX decision | STATIC |
| 175 | Inventory-money pickup/drop conservation | RUN |
| 176 | Purchase while inventory full handling | RUN |
| 177 | Buy stack partial quantity | RUN |
| 178 | Vendor money depletion (can't afford your goods) | RUN |

### Theme 6 - Inventory & items (179-200)

| # | Title | Type |
|---|-------|------|
| 179 | Item stacking sync | RUN |
| 180 | Item quality/material attribute sync | RUN |
| 181 | Equip-slot change sync | RUN |
| 182 | Weapon-swap sync | RUN |
| 183 | Backpack/container open + transfer sync | RUN |
| 184 | Corpse looting sync + conservation | RUN |
| 185 | Dropped-bag conservation edge cases | RUN |
| 186 | Crafting-output item sync | RUN |
| 187 | Item repair-state sync | RUN |
| 188 | Unique-item identity sync | RUN |
| 189 | Drag-drop inventory move replication | RUN |
| 190 | Cross-squad item transfer | RUN |
| 191 | Pickup ground-item conservation | RUN |
| 192 | Container ownership/locking | RUN |
| 193 | Equip-armor visual sync on peer | RUN |
| 194 | Weapon visual sync on peer | RUN |
| 195 | Consumable use (food) sync | RUN |
| 196 | Item weight/encumbrance sync | RUN |
| 197 | Stack-split race condition | RUN |
| 198 | Simultaneous-loot same corpse race | RUN |
| 199 | Item durability-decay sync | RUN |
| 200 | Inventory delta-encoding design | STATIC |

### Theme 7 - Factions & diplomacy (201-218)

| # | Title | Type |
|---|-------|------|
| 201 | Faction relation-value sync | RUN |
| 202 | Declare-war propagation | RUN |
| 203 | Reputation-change sync | RUN |
| 204 | FactionEvent propagation surface | STATIC |
| 205 | Bounty-system sync | RUN |
| 206 | Faction-owned property ownership sync | RUN |
| 207 | Town-ownership sync | RUN |
| 208 | Recruit dialog -> add to squad sync | RUN |
| 209 | Prisoner/captive state sync | RUN |
| 210 | Free-prisoner event sync | RUN |
| 211 | Faction ally/coalition sync | RUN |
| 212 | Hostility trigger (attack -> aggro) sync | RUN |
| 213 | Faction-wide reputation from member action | RUN |
| 214 | Join actions affecting host faction standing | RUN |
| 215 | Faction list enumeration + ids | DUMP |
| 216 | Faction join/leave sync | RUN |
| 217 | Price/access changes from reputation sync | RUN |
| 218 | Faction patrol spawns sync | RUN |

### Theme 8 - AI, orders & jobs (219-244)

| # | Title | Type |
|---|-------|------|
| 219 | PlayerInterface::addOrderSelectedCharacters paths | STATIC |
| 220 | Move-order replication | RUN |
| 221 | Attack-order replication | RUN |
| 222 | Job-queue replication | RUN |
| 223 | Permajob replication | RUN |
| 224 | Formation sync | RUN |
| 225 | Follow-order sync | RUN |
| 226 | Guard-order sync | RUN |
| 227 | Patrol-order sync | RUN |
| 228 | Hold-position order sync | RUN |
| 229 | Stance (passive) sync | RUN |
| 230 | Stance (aggressive) sync | RUN |
| 231 | Sneak/stealth stance sync | RUN |
| 232 | Block/taunt order sync | RUN |
| 233 | Automated mining job sync | RUN |
| 234 | Automated farming job sync | RUN |
| 235 | Hauling job sync | RUN |
| 236 | Order-queue ordering preservation | RUN |
| 237 | Order cancel/override sync | RUN |
| 238 | AI auto-target vs player-order conflict | RUN |
| 239 | Pick-up-item order sync | RUN |
| 240 | Open/close door order sync | RUN |
| 241 | Flee/run-away AI sync | RUN |
| 242 | Squad-wide order broadcast | RUN |
| 243 | Job-assignment persistence across save | RUN |
| 244 | Order-latency measurement | RUN |

### Theme 9 - Ogre rendering & engine internals (245-264)

| # | Title | Type |
|---|-------|------|
| 245 | Scene-graph node access | STATIC |
| 246 | Mesh enumeration/read | DUMP |
| 247 | Material read/enumeration | DUMP |
| 248 | Particle-system enumeration | DUMP |
| 249 | Lighting-state read | DUMP |
| 250 | Time-of-day render-parameter read | DUMP |
| 251 | Render-target/screenshot capture API | DUMP |
| 252 | Debug-draw line/shape feasibility | DUMP |
| 253 | Multi-viewport feasibility | STATIC |
| 254 | AnimationState access (spike 42 deep-dive) | STATIC |
| 255 | Skeleton/bone access (spike 42 deep-dive) | STATIC |
| 256 | Scene-manager enumeration | DUMP |
| 257 | Camera list / active-camera read | DUMP |
| 258 | Texture/resource-manager read | DUMP |
| 259 | Overlay/2D element creation | DUMP |
| 260 | Frame-listener hook feasibility | DUMP |
| 261 | RTSS / shader-generator presence | DUMP |
| 262 | Ogre 2.0 Tindalos compositor pipeline | STATIC |
| 263 | Mesh bounding-box reads for markers | DUMP |
| 264 | Entity-to-scenenode mapping for a Character | DUMP |

### Theme 10 - Coop UI / HUD / MyGUI (265-294)

| # | Title | Type |
|---|-------|------|
| 265 | Ogre overlay HUD proof (text on screen) | RUN |
| 266 | Peer-squad nameplate over peer characters | RUN |
| 267 | Peer-squad marker (offscreen indicator) | RUN |
| 268 | Minimap marker for peer squad | RUN |
| 269 | Connection/status indicator | RUN |
| 270 | Ping/latency display | RUN |
| 271 | Peer health bars | RUN |
| 272 | Peer squad-list panel | RUN |
| 273 | Chat input box | RUN |
| 274 | Chat message display | RUN |
| 275 | showPlayerAMessage event wiring | STATIC |
| 276 | showPlayerAMessage HUD notification | RUN |
| 277 | MyGUI::Gui::getInstance entry + widget creation | STATIC |
| 278 | Native MyGUI panel proof | RUN |
| 279 | Lobby/ready UI | RUN |
| 280 | Join-status toast (connected/disconnected) | RUN |
| 281 | Peer cursor/selection indicator | RUN |
| 282 | Damage numbers over targets | RUN |
| 283 | Objective/quest shared panel | RUN |
| 284 | Settings panel for coop options | RUN |
| 285 | Nameplate occlusion/depth handling | RUN |
| 286 | Nameplate scaling with distance | RUN |
| 287 | HUD toggle hotkey | RUN |
| 288 | MyGUI skin/theme reuse from Kenshi | STATIC |
| 289 | Peer money/resources display | RUN |
| 290 | Event log/feed panel | RUN |
| 291 | Host/join role badge | RUN |
| 292 | Reconnect prompt UI | RUN |
| 293 | Map-screen peer markers | RUN |
| 294 | UI scaling/DPI handling | STATIC |

### Theme 11 - Networking & protocol (295-320)

| # | Title | Type |
|---|-------|------|
| 295 | Bandwidth-profiling harness | WORKFLOW |
| 296 | Packet-loss resilience sweep (-NetSimLoss) | RUN |
| 297 | Jitter resilience sweep (-NetSimJitter) | RUN |
| 298 | Latency sweep (-NetSimDelay) | RUN |
| 299 | Reliable vs unreliable channel audit | STATIC |
| 300 | Compression/delta-encoding opportunities | STATIC |
| 301 | Interpolation-buffer tuning | RUN |
| 302 | Extrapolation/dead-reckoning test | RUN |
| 303 | Reconnection after drop | RUN |
| 304 | Late-join mid-session | RUN |
| 305 | Clock sync / time-offset measurement | RUN |
| 306 | Host-migration feasibility | STATIC |
| 307 | Snapshot-rate vs smoothness | RUN |
| 308 | Message-ordering guarantees audit | RUN |
| 309 | Large-payload fragmentation handling | RUN |
| 310 | Out-of-order packet handling | RUN |
| 311 | Duplicate-packet handling | RUN |
| 312 | Keepalive/heartbeat tuning | RUN |
| 313 | Bandwidth-cap enforcement under load | RUN |
| 314 | Priority queue for critical events | RUN |
| 315 | NAT/connection-setup robustness | RUN |
| 316 | Per-message-type byte accounting | WORKFLOW |
| 317 | Delta vs full-state snapshot comparison | RUN |
| 318 | Encryption/overhead measurement | RUN |
| 319 | 3+ peer mesh vs star topology | RUN |
| 320 | Protocol versioning/handshake | RUN |

### Theme 12 - World sim & environment (321-340)

| # | Title | Type |
|---|-------|------|
| 321 | Weather sync (WeatherAffecting) | RUN |
| 322 | Day/night time sync | RUN |
| 323 | Time-speed/pause policy in coop | STATIC |
| 324 | World event (raid) sync | RUN |
| 325 | Invasion/siege sync | RUN |
| 326 | Environmental hazard: acid rain sync | RUN |
| 327 | Environmental hazard: gas/toxic sync | RUN |
| 328 | Environmental hazard: burn/fire sync | RUN |
| 329 | Foliage/vegetation state | DUMP |
| 330 | Zone-activation sync | RUN |
| 331 | Fog-of-war/exploration sync | RUN |
| 332 | Wildlife-spawn sync | RUN |
| 333 | Dust/sand-storm visibility sync | RUN |
| 334 | Temperature/biome-effect sync | RUN |
| 335 | World-state timer-events sync | RUN |
| 336 | Weather-control API | DUMP |
| 337 | Nest/animal-spawner sync | RUN |
| 338 | Patrol/caravan world-actor sync | RUN |
| 339 | Time-of-day driven NPC schedules sync | RUN |
| 340 | Ambient world sound/event triggers | RUN |

### Theme 13 - Save / load / persistence (341-356)

| # | Title | Type |
|---|-------|------|
| 341 | DLL-triggered save() coordination across clients | RUN |
| 342 | Custom coop blob via GameData typed maps | STATIC |
| 343 | GameSaveState extension feasibility | STATIC |
| 344 | Save-compat & migration across versions | RUN |
| 345 | Autosave coordination (avoid both saving) | RUN |
| 346 | Baked-save library management | WORKFLOW |
| 347 | Load determinism (same save -> same state) | RUN |
| 348 | instanceID handling across save/load | STATIC |
| 349 | Peer-squad persistence in host save | RUN |
| 350 | Coop-session resume from save | RUN |
| 351 | Save during active combat | RUN |
| 352 | Save/load round-trip identity stability | RUN |
| 353 | Save-file format inspection | STATIC |
| 354 | Quicksave/quickload coop behavior | RUN |
| 355 | Save naming/slot coordination | RUN |
| 356 | Corrupted-save recovery handling | RUN |

### Theme 14 - Camera & spectating (357-368)

| # | Title | Type |
|---|-------|------|
| 357 | Free-cam control API | DUMP |
| 358 | Follow-peer camera | RUN |
| 359 | Jump-to-event camera | RUN |
| 360 | Cinematic battle cam | RUN |
| 361 | Screenshot tooling for tests | WORKFLOW |
| 362 | Picture-in-picture feasibility | STATIC |
| 363 | Spectate peer squad | RUN |
| 364 | Camera position/orientation read+write | DUMP |
| 365 | Smooth camera transitions | RUN |
| 366 | Camera bounds/clamp behavior | RUN |
| 367 | Multi-target framing cam | RUN |
| 368 | Camera zoom/FOV control | DUMP |

### Theme 15 - Dialogue & social (369-380)

| # | Title | Type |
|---|-------|------|
| 369 | Speech bubble over peer characters | RUN |
| 370 | sayALine API surface | STATIC |
| 371 | sayALine sync | RUN |
| 372 | makeAnnouncement sync | RUN |
| 373 | Dialog/event trigger sync | RUN |
| 374 | Cutscene sync | RUN |
| 375 | Tutorial-popup handling in coop | RUN |
| 376 | Emote/gesture sync | RUN |
| 377 | Conversation between peer characters | RUN |
| 378 | Dialogue event-hook surface | STATIC |
| 379 | NPC dialog triggered by join | RUN |
| 380 | Shared dialog-choice resolution | RUN |

### Theme 16 - Characters, stats & progression (381-398)

| # | Title | Type |
|---|-------|------|
| 381 | XP/skill-gain sync | RUN |
| 382 | Stat-change sync | RUN |
| 383 | Race-ability sync | RUN |
| 384 | Hunger-need sync | RUN |
| 385 | Sleep/rest-need sync | RUN |
| 386 | Aging sync | RUN |
| 387 | Skill-use animation sync | RUN |
| 388 | Level-up event sync | RUN |
| 389 | Attribute-training sync | RUN |
| 390 | Limb-based stat-penalty sync | RUN |
| 391 | Strength/encumbrance progression sync | RUN |
| 392 | Stealth/perception skill sync | RUN |
| 393 | Combat-skill gain attribution | RUN |
| 394 | Stats/skills field map | STATIC |
| 395 | Recruit stat-initialization sync | RUN |
| 396 | Skill-cap/learning-rate sync | RUN |
| 397 | Status effects (drugged/drunk) sync | RUN |
| 398 | Character rename/customization sync | RUN |

### Theme 17 - Buildings & base-building (399-418)

| # | Title | Type |
|---|-------|------|
| 399 | Building construction-placement sync | RUN |
| 400 | Construction-progress sync | RUN |
| 401 | Research-bench progress sync | RUN |
| 402 | Production-machine output sync | RUN |
| 403 | Building-ownership sync | RUN |
| 404 | Door/gate open-close sync | RUN |
| 405 | Storage-container shared access | RUN |
| 406 | Turret/defense placement sync | RUN |
| 407 | Turret-firing sync | RUN |
| 408 | Power/generator state sync | RUN |
| 409 | Farming-plot growth sync | RUN |
| 410 | Building deconstruction sync | RUN |
| 411 | Building-upgrade sync | RUN |
| 412 | Wall/gate breach sync | RUN |
| 413 | Base-raid trigger sync | RUN |
| 414 | Resource-node (mining) depletion sync | RUN |
| 415 | Water/well production sync | RUN |
| 416 | Building damage/repair sync | RUN |
| 417 | Building data-model field map | STATIC |
| 418 | Multi-peer shared-base ownership | RUN |

### Theme 18 - Test harness & workflow tooling (419-436)

| # | Title | Type |
|---|-------|------|
| 419 | KENSHICOOP_SPIKE_ARG numeric-sweep pass-through | WORKFLOW |
| 420 | Regression-matrix runner | WORKFLOW |
| 421 | Log-oracle assertion framework | WORKFLOW |
| 422 | Screenshot diffing | WORKFLOW |
| 423 | Crash-triage automation | WORKFLOW |
| 424 | Perf-telemetry capture | WORKFLOW |
| 425 | Multi-save matrix runner | WORKFLOW |
| 426 | Replay capture/playback | WORKFLOW |
| 427 | Deterministic seed control | WORKFLOW |
| 428 | CI-style nightly spike loop | WORKFLOW |
| 429 | Log parser -> structured findings | WORKFLOW |
| 430 | Flaky-test detection | WORKFLOW |
| 431 | Two-client orchestration improvements | WORKFLOW |
| 432 | Headless/fast-forward mode feasibility | WORKFLOW |
| 433 | Spike-result dashboard rollup | WORKFLOW |
| 434 | Automatic raw-log archival/retention | WORKFLOW |
| 435 | Build-cache/incremental-build speedup | WORKFLOW |
| 436 | Assertion on bandwidth/perf thresholds | WORKFLOW |

### Theme 19 - Robustness & desync recovery (437-450)

| # | Title | Type |
|---|-------|------|
| 437 | Desync detection (state-hash compare) | RUN |
| 438 | Drift correction / resync | RUN |
| 439 | Rapid save/load stress | RUN |
| 440 | Client-crash recovery | RUN |
| 441 | Hostile/abrupt disconnect handling | RUN |
| 442 | Duplicate-hand collision handling | RUN |
| 443 | Serial-overflow collision handling | RUN |
| 444 | Large-distance teleport desync | RUN |
| 445 | Host pause/resume desync | RUN |
| 446 | Packet-flood / DoS resilience | RUN |
| 447 | State-divergence auto-heal | RUN |
| 448 | Reconnect state reconciliation | RUN |
| 449 | Time-skip (alt-tab/freeze) recovery | RUN |
| 450 | Memory/handle leak over long session | RUN |
