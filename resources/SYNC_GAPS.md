# Kenshi Co-op: Remaining World-State Sync Gaps (living roadmap)

> Status: living document (2026-07-08). The prioritized queue of world-state
> synchronization that is NOT yet implemented + validated, so a session can
> still visibly diverge there. Companion to
> [INTENT_REPLICATION.md](INTENT_REPLICATION.md) (framework + doctrine, what IS
> done) and [VALIDATION_BASELINE.md](VALIDATION_BASELINE.md) (evidence for the
> done set). Update an entry's Status when work starts/lands; add new gaps as
> real sessions surface them.

## What is already covered (context, one line each)

Squad presence + per-tab ownership; world-NPC streaming with dual interest;
poses/rest/furniture/crafting; body state + reliable KO/death/revive events;
combat stance/attribution/damage-guard; full-anatomy medical + treatment
forwarding + limb loss; character stats; carried bodies; bed/cage occupancy;
stealth + detection feedback; inventory incl. equipped gear + weapon slots;
world ground items (drop/pickup conservation); consensus game speed/pause;
runtime NPC spawn proxies + suppression hardening (protocol 21); squad
management (tab moves re-keyed, latched rank partition - protocol 35); Steam
P2P transport. See the doctrine list (14-35) for the mechanism of each.

## Priority 1 - bites a normal session within minutes

### 1. Money + vendor trading  [MOSTLY DONE 2026-07-08 - protocol 22]

- **Symptom (was):** any purchase/sale/bounty/bar meal changed cats on ONE
  client only.
- **Shipped (protocol 22):** owner-authoritative per-tab wallet stream
  (`PKT_MONEY`, the `PKT_STATS` shape: change-gated reliable, ~1 Hz floor,
  5 s safety resend; apply via `Ownerships::setMoney`). Gates: `money_sync`
  (sentinel wallets cross both directions) and `vendor_trade` (the buyer-side
  purchase composite - wallet debit + bought item - converges over
  PKT_MONEY + the inventory channel). `KENSHICOOP_MONEY_SYNC=0` is the
  escape hatch; `shop_probe` forces it off to keep the baseline measurable.
- **Remaining (vendor-side stock/cash mirror) - reframed by the shop_probe
  findings:** the planned host-authoritative `ShopTrader`-stock snapshot has
  no automatable substrate: vendor inventories are LAZY (`ShopTrader::
  inventory` is null until the trade UI first opens; runs 103018-104036),
  the enumerated SHOP_TRADER_CLASS objects in the test save carry NO bound
  trader (`getTrader()` null), their wrapper hands are RUNTIME-minted (serials
  differ per client/run - a hand-keyed snapshot cannot match), and the engine
  REGENERATES stock per client anyway. So vendor stock is already divergent
  by engine design even in the areas the sync could reach. The `[shop]
  BUY-LOCAL` detour on `Inventory::buyItem` now logs every REAL purchase
  (seller identity/money, item sid, buyer) - field evidence from manual
  sessions gates any future mirror (likely a shop-open-scoped snapshot keyed
  by the trader Character's save-stable hand).
- **Spikes:** 28 (trading API), 29 (money scope), 30 (vendor proximity),
  31/32 (purchase-as-transfer, shared-economy conflicts - superseded by the
  shipped design).

### 2. Recruitment mid-session  [DONE 2026-07-08 - protocol 23]

- **Symptom (was):** a recruit picked up during a session existed on the
  recruiting client only; the peer saw nothing (join recruits - the describe
  channel was join-pull only) or a DUPLICATE proxy next to its still-standing
  baked copy (host recruits; recruit_probe run 114151).
- **Root cause:** recruiting re-containers the character into a player platoon
  (hand CONTAINER changes; baked subjects keep their serial, runtime ones the
  whole tail - recruit_probe), so the peer can never resolve the NEW hand.
- **Shipped (protocol 23):** a detour on `PlayerInterface::recruit` captures
  every successful local recruit's before/after hand pair; the Replicator
  authors a reliable `EVT_RECRUIT` (subject = old hand, actor = new hand) and
  the peer RE-KEYS its existing local copy to the new stream key via the
  proxy translation map (one body, no duplicate; suppressed copies restored
  first). Runtime-born recruits resolve over the describe/mint channel, now
  BIDIRECTIONAL (both sides answer `PKT_SPAWN_REQ` and author them). Recruited
  hands are owned by their RECRUITER regardless of local tab rank
  (the `pinOwned_`/`pinPeer_` ownership pins, generalized to cover squad
  moves too by protocol 35 - the probe showed a join recruit landing in the
  host-owned rank-0 container). `KENSHICOOP_RECRUIT_SYNC=0` is
  the escape hatch; `recruit_probe` forces it off to keep the baseline
  measurable. Gates: `recruit_sync` (all four legs converge with exactly one
  peer body each); regressions coop_presence, npc_sync, spawn_sync,
  split_interest green.
- **Remaining edge:** the recruit is visible/driven on the peer but not in the
  peer's SQUAD UI (peer-side platoon placement deferred - it would recruit the
  copy locally and re-partition ownership; revisit if field sessions want
  shared control of each other's recruits). Save/load coordination of recruits
  rides gap 8.
- **Spikes:** 38 (programmatic orders), backlog #208, #395; recruit_probe
  findings (runs 114151, 120738 - the double-recruit collision).

### 3. Faction relations / reputation / aggro  [DONE 2026-07-08 - protocol 24]

- **Symptom (was):** attacking a faction (or paying off a bounty) flips
  hostility on one client only - guards attack one player's view of the world.
- **Probe findings (faction_probe run 132239):** faction GameData stringIDs
  are cross-client stable (identical censuses on the shared save); the engine
  keeps the two per-side relation tables MIRRORED (player->them always equals
  them->player, in every enumerated row); the enemy/ally flags DERIVE from the
  value (sentinel -75 flipped enemy=1 both directions, +65 flipped ally=1); a
  sentinel `setRelation` sticks locally and NOTHING crosses without a channel.
- **Shipped (protocol 24):** one f32 per faction sid is the whole state.
  `PKT_FACTION` (sid + relation + per-sender seq) on CH_RELIABLE; BOTH clients
  run the same change detector (player-faction table sampled ~1 Hz, or
  immediately when the detoured `FactionRelations::affectRelations` - either
  overload - saw a real mutation), stream rows that moved vs the seeded
  baseline, and apply received rows onto both local table directions via
  `setRelation`. Applying updates the baseline first, which keeps the channel
  echo-free; per-sid safety resend covers a lost row. `KENSHICOOP_FACTION_SYNC`
  (default ON) is the A/B hatch; forced OFF inside `faction_probe`.
  `faction_sync` gates both sentinel crossings + final-table agreement (run
  133734: crossed 2/2, no diverged rows).
- **Remaining edges (accepted):** bounties / crime state are a separate engine
  system (the `[fac] AFFECT` detour keeps accumulating cause evidence);
  NPC faction-vs-faction wars are world-sim (gap 6) - only rows involving the
  PLAYER faction stream.

### 4. Game calendar / time-of-day [DONE 2026-07-08 - protocol 25]

- **Symptom (was):** speed and pause are consensus (doctrine 24), but the
  CLOCK itself drifted (different load/pause moments); day/night drives NPC
  schedules, shop hours, stealth vision.
- **Probe findings (`time_probe`, run 141509):** `getTimeStamp_inGameHours`
  (struct-return ABI - TimeOfDay has user-declared ctors, so it comes back
  via a hidden retbuf, the getPositionBip01 hazard) returns the ABSOLUTE
  campaign clock in total in-game hours; on the shared save the host/join
  offset was exactly the load-moment skew (~0.3 gh) and grew with every
  differential pause. Hour length identical on both clients (109.1 s/gh).
  The clock rate tracks `frameSpeedMult` EXACTLY (2x burst -> 2.00 measured
  rate ratio) - which makes a sim-speed SLEW a precise correction lever.
- **Mechanism (protocol 25):** host-authoritative `PKT_TIME` broadcast
  (absolute f64 gameHours, ~1 Hz reliable). The join measures
  offset = host - local each sample and SLEWS: a proportional multiplier
  (capped 2x / floored 0.25x, hysteresis deadband at ~7 game-seconds) that
  the speed layer's QUIET writes fold in ON TOP of the arbitrated consensus
  effective (`slewedEffective` - composes with `speed_sync` rather than
  fighting its continuous enforcement; UI buttons never move). A direct
  clock STEP was prototyped (write the `timeStamper` CPerfTimer base,
  self-verifying) and REJECTED: the calendar does not derive from that
  timer (run 150001) - there is no writable clock base, slew-only.
- **Validated:** `time_sync` (runs 142444/145800s): ~0.3 gh load skew closed
  in ~35 s, final offset 0.002 gh (tol 0.02), monotonic both sides,
  convergence held across a consensus 2x burst. Regressions green
  (`coop_presence`, `npc_sync`, `speed_sync`, `faction_sync`).
- **Accepted edges:** the catch-up is a visible session-start transient (the
  join's sim runs up to 2x for ~35 s per 0.3 gh of skew); `speed_sync`'s
  oracle gates RAW fsm equality, so that scenario forces timeSync off (the
  composition is gated by `time_sync` instead); differential pauses during
  play re-open small offsets that the slew re-closes within seconds.

## Priority 2 - structural, bites longer campaigns

### 5. Buildings and base-building [DOOR + PLACEMENT + PLACED-DOOR/DISMANTLE + PRODUCTION-MACHINE SLICES DONE 2026-07-09 - protocols 26/27/28/33]

- **Symptom:** player-placed buildings, construction progress, production
  machines, research, farming, and door/gate states exist per-client.
- **Root cause:** a placed building is a runtime object (host-only hand) -
  the protocol-21 identity problem for structures; continuous states
  (progress %, door open) have no channel.
- **Door slice (protocol 26, DONE):** door/gate open+lock state on BAKED
  buildings. Probe findings (`door_probe`, run 160041): baked-door hands
  are cross-client stable (census intersection on the shared save - the
  furniture/bed identity precedent holds for `DoorStuff : Building`); the
  engine's own `openDoor`/`closeDoor` write lever works and animates
  natively (state walks OPENING->OPEN, so the channel publishes the
  collapsed DESTINATION state, never the mid-swing transient); with
  doorSync off the sentinel toggles stayed local (nothing crosses - the
  gap was real). Mechanism: symmetric change-gated `PKT_DOOR` rows
  (hand + open + locked, CH_RELIABLE, ~1 Hz sample within ~100 m of the
  interest centers), the `publishFactions`/`applyFactions` shape - seeded
  per-hand baseline, baseline updated BEFORE the apply write (echo-free),
  per-sender seq drops stale rows, 10 s safety resend, unresolvable hands
  skipped silently (out-of-interest or runtime door). Validated:
  `door_sync` run 161116 (crossed 2/2, diverged 0); regressions green
  (`coop_presence`, `npc_sync`, `faction_sync`, `time_sync`).
  `KENSHICOOP_DOOR_SYNC` (default ON) is the A/B hatch; forced OFF inside
  `door_probe`.
- **Placement slice (protocol 27, DONE):** player-PLACED buildings +
  construction progress. Probe findings (`build_probe`, run 174550): the
  raw `createBuilding` factory call BYPASSES the UI's town-placement
  verification (both placements landed in the town-adjacent `sync` save -
  the mint path never needs the rules relitigated); minted-site hands are
  runtime (census intersection ZERO - the protocol-21 identity problem
  confirmed for structures, so the wire keys by the PLACER's hand); the
  engine's own `setConstructionProgress` is the progress lever (0..1
  scale) and SELF-COMPLETES at >= 1.0 (progress jumps to 4.0, isComplete
  flips, scaffold comes off natively). Mechanism: PLACER-AUTHORITATIVE
  describe/mint - a local placement (the `placeFinalPreviewBuilding` UI
  detour or a programmatic scenario place) announces `PKT_BUILD_PLACE`
  (template sid + transform, keyed by the placer's hand); the receiver
  mints an incomplete local site via the same factory and keeps a
  key -> local-hand translation map (refused mints remembered, resends
  deduped); progress streams as change-gated `PKT_BUILD_STATE` rows (~1 Hz
  sample, 10 s safety resend while incomplete, complete=1 latches the
  channel silent). Echo-free BY CONSTRUCTION: a factory mint never passes
  through the placement detour, so nothing re-announces. Validated:
  `build_sync` run 175747 (minted 2/2, progress crossed 2/2 up to the
  complete latch); regressions green (`coop_presence`, `npc_sync` (flaky:
  smoothness/npc_track machine-load signature, zero [build] packets in
  that save, both retry-passed), `door_sync`, `faction_sync`,
  `time_sync`); prototest 149/149. `KENSHICOOP_BUILD_SYNC` (default ON)
  is the A/B hatch; forced OFF inside `build_probe`.
- **Placed-door + dismantle slice (protocol 28, DONE):** doors on PLACED
  buildings + building removal. Probe findings (`bdoor_probe`, run 195513):
  a minted proxy mints its own DoorStuff children in the same template
  order (shack door at parent->doors index 0 on BOTH clients - the
  (placer building hand, door index) translation identity holds); the
  polite openDoor/closeDoor lever works on runtime doors; GameWorld::
  destroy cleanly removes a placed building locally; and the peer's proxy
  SURVIVED the placer's destroy (11 ghost census samples - the removal gap
  was real). Mechanism: `PKT_BUILD_DOOR` - the symmetric change-gated
  protocol-26 door shape on the TRANSLATED key (placer's building hand +
  door index, resolved through the protocol-27 build maps + a new reverse
  map local-mint-hand -> placer-key); the protocol-26 channel now SKIPS
  doors whose parent is session-placed (no double-streaming);
  `PKT_BUILD_REMOVE` - placer-authoritative removal (dismantle detour on
  `Building::notifyConstructionDismantling` or programmatic destroy -> the
  peer destroys its mapped proxy via GameWorld::destroy and tombstones the
  map entry). Validated: `bdoor_sync` run 200456 (toggles crossed 2/2 onto
  the proxies, host destroy removed the join's proxy - ghost exorcised);
  regressions green. `KENSHICOOP_BDOOR_SYNC` (default ON) is the A/B
  hatch; forced OFF inside `bdoor_probe`.
- **Production machine slice (protocol 33, wire 31 -> 32, DONE):**
  production machines, power fixtures and farm growth. Probe findings
  (`prod_probe`, run 152730): machine-class buildings (`Building::
  classType` in PRODUCTION/CRAFTING/FURNACE/FARM/RESEARCH) census with
  cross-client-stable hands for BAKED machines while session-placed ones
  translate through the protocol-27 build maps; the divergence is real
  (the host's operate()-driven bench output moved while the join's
  minted copy stayed flat, and a power toggle never crossed); the write
  levers stick - `UseableStuff::switchPowerOn` persists, the native
  `setProductionItem` lands AND MATERIALIZES a still-null output buffer
  (a fresh bench has no `productionItem` until the first production
  tick - the template comes from `getProductionItemData`), and a direct
  `ConsumptionItem::amount` write survives the next engine tick
  (update() does not clamp it). Mechanism: HOST-AUTHORITATIVE (the
  world-simulation precedent) - the host samples machines in the
  interest spheres ~1 Hz and streams `PKT_PROD` rows (power bit,
  production state, output item sid + amount, input amounts, farm
  growth floats; -1 = field not carried, the hunger fold-in trick),
  keyed by baked hand or placer key (`keyKind` disambiguates); FIRST
  sight sends (the host's state is the baseline - not the symmetric
  seed-silent door shape), then change-gated on quantized hundredths
  with the 10 s safety resend doubling as the join drift corrector; the
  join applies only fields that actually diverged, materializing null
  buffers via setProductionItem then landing exact amounts with the
  direct write. Session-reset clears the row cache (protocol 32);
  onPeerConnected ages lastSendMs (protocol 30). Research benches are
  census/evidence only (tech-unlock store still unmapped - follow-up
  spike). Validated: `prod_sync` run 154503 (bench output converged
  gap=0.000 after 30 ops, power OFF crossed in <= 3 join samples, final
  power agreed); regressions green. `KENSHICOOP_PROD_SYNC` (default ON)
  is the A/B hatch; forced OFF inside `prod_probe`.
- **Storage/machine container slice (protocol 34, wire 32 -> 33, DONE):**
  storage chests and machine INVENTORY contents - the container-inventory
  channel registered exactly ONE container (the nearest baked chest), so
  every other chest and every bench/drill/furnace inventory forked
  per-client. Probe findings (`store_probe`, run 171728): container-
  bearing buildings (`BCTYPE_STORAGE` + the machine classes) census with
  readable inventories on both clients; a template NAME does not reveal
  its class (the first "general storage" match placed as a non-STORAGE
  class - the probe places candidates and verifies the LIVE classType);
  storage buildings are item-type-LIMITED (a Fabric Chest refused the
  iron-plate sentinel - harmless for the real apply path, which only
  fabricates items the author's copy already holds); the phase-4a
  capture/reconcile levers work on BUILDING inventories (fabricate-in
  landed 0->5, reconcile removal 5->2 stuck); operate() does NOT land
  whole items in a machine's container (output rides the protocol-33
  buffer floats until a worker collects - so the reconcile-churn risk is
  moot: the force-emptied bench container stayed empty); and the join
  fabricated into its MINTED chest copy (the translated-key apply half).
  Mechanism: HOST-AUTHORITATIVE - the host's ~1 Hz census
  (`enumContainersNear`) registers every COMPLETE container-bearing
  building near the interest centers with the EXISTING container-
  inventory channel (replacing the single `pickInventoryContainer`
  registration), so contents stream through the proven per-container
  hash + settle-window + 5 s safety-resend gate as `PKT_INV_SNAPSHOT`;
  a `keyKind` byte on the wire says whether the container key is a raw
  save-stable hand or a protocol-27 placer key (session-placed buildings
  translate both ways through the build maps, the `PKT_PROD` identity
  approach); an unresolvable key is dropped and the safety resend
  re-delivers once the mint lands. Session reset re-censuses (protocol
  32). Validated: `store_sync` run 173245 (the host census-authored the
  placed chest as kind=1, the join applied 74 rows, the host's chest add
  crossed onto the join's minted copy and the FINAL content hashes
  agreed - so the reconcile-removal crossed too); regressions green.
  `KENSHICOOP_STORE_SYNC` (default ON) is the A/B hatch; forced OFF
  inside `store_probe`; layered on `invSync` (the carrier channel).
- **Remaining (the rest of the gap):** research tech-tree unlocks (the
  progress store is unmapped - `getTechLevel` reads it but no
  writable surface found yet; probe evidence collected), gates/wall
  doors on placed WALLS, door HEALTH/broken state and lockpicking
  (doctrine 31 out-of-scope list); a peer that missed the PLACE also
  misses the REMOVE (harmless - the shared-save workflow reloads from a
  common save anyway).
- **Spikes:** backlog theme 17 (#399-418); both slices ran as
  probe/sync scenario pairs instead of spikes.

### 6. World events / environment / far-world simulation

- **Symptom:** raids and sieges, weather (visuals + hazard damage like acid
  rain), wildlife/nest spawners, caravans, and Kenshi "world states" (town
  overrides after faction-leader deaths) all simulate independently.
- **Root cause:** protocol 21 covers runtime spawns NEAR the players; the
  wider world simulation has no authority partition at all.
- **Mechanism (candidate):** piecewise - weather snapshot (spike 37 read
  surface is PARTIAL), world-state flag snapshot, raid/event edges. Large;
  needs its own gap analysis when prioritized.
- **Spikes:** 37 (weather, PARTIAL), backlog theme 12 (#321-340).

### 7. Hunger and status effects [HUNGER SLICE DONE 2026-07-08 - protocol 29]

- **Symptom (was):** hunger, drunk/drugged states diverge on driven copies
  (doctrine 20 explicitly flags hunger as a local-only model needing the
  medical treatment).
- **Root cause:** local-only models with no stream; cosmetic simulation can
  also write them locally (the XP-pollution class of bug).
- **Hunger slice (protocol 29, DONE):** probe findings (`hunger_probe`, run
  213751): `MedicalSystem::hunger` uses a ~0..3 scale; decay is ACTIVITY-
  DRIVEN per client (the marching leader decayed ~0.024/s while its idle
  driven copy decayed ~0.0002/s - a 40% owner-vs-copy gap opened in one
  50 s run, the gap was real and fast); a direct hunger write STICKS
  (no clamp/reset from medicalUpdate); sentinel drops stayed local with
  the hatch off. Mechanism: hunger + fed ride the existing owner-
  authoritative `PKT_MEDICAL` snapshot (fold-in, no new channel) - two f32s
  in the packet, 0.1-unit quantization in the change-gate fingerprint
  (buckets flip slower than the 3 s safety resend, so the fold-in adds no
  traffic), -1 = field-not-carried lets `KENSHICOOP_HUNGER_SYNC` (default
  ON, forced OFF in `hunger_probe`) A/B the fields without touching the
  rest of the medical stream. Validated: `hunger_sync` run 214516
  (sentinels crossed 2/2, final owner-vs-copy gap 0.000); regressions
  green incl. `medic_order`/`limb_loss` (the medical channel's own gates).
- **Remaining (status effects):** drunk/drugged state has NO mapped engine
  surface (headers expose only `ITEM_NARCOTIC`, the generic
  `Character::startEffect(GameData*)`, and `MedicalSystem::dazedOrAlert` -
  which the probe measured as a 0..1 alertness-style flag at rest, not an
  intoxication scalar). Needs its own spike (runtime probe: what changes
  after alcohol/hashish consumption) before any wire design.
- **Spikes:** backlog #142, #384, #397.

## Priority 3 - session flow (world-state adjacent)

### 8. Save/load coordination + join-side persistence [COORDINATED SAVE + RESUME DONE 2026-07-09 - protocol 31; COORDINATED LOAD DONE 2026-07-09 - protocol 32]

- **Symptom (was):** no coordinated save; a resumed session needs a manually
  re-mirrored save; join-side progression persists only in the join's own
  save (documented stats limitation).
- **Design pivot:** the original candidate (per-side saves + a `_coop.dat`
  sidecar carrying session state) was DROPPED in favor of a strictly simpler
  invariant: the HOST's save is authoritative and travels IN-BAND. The host's
  save already contains the join's squad state via the sync channels, so
  baking ONE save with ONE hand and mirroring it re-runs the shared-save-
  lineage guarantee all the hand-keyed replication rests on - session-placed
  buildings/recruits get save-stable identity on both sides for free, and no
  sidecar format ever has to version-track the engine's serialization.
- **Mechanism (protocol 31, wire version 30, DONE):** `SaveManager::save`
  detour (every local save - menu, quicksave, autosave, programmatic - logs a
  `[save] LOCAL-SAVE` edge; spike 39's `getCurrentGame`/`getSavePath` RVAs
  resolve the live save identity/root, runtime-validated by `save_probe`).
  On the HOST an edge arms a folder-quiescence watch (the save is deferred +
  multi-file; poll the folder until mtimes/sizes hold still ~1.5 s, measured
  ~2 s to settle for the 3.7 MB / 35-file fixture); on QUIESCED the whole
  folder streams to the join over CH_RELIABLE in ~4 KB chunks paced ~32 per
  50 ms (`PKT_SAVE_BEGIN`/`FILE`/`DONE` with per-file FNV-1a-32 CRCs,
  measured ~1.8 s in-flight). The join stages into `save/<name>__incoming/`,
  verifies every CRC, commits ATOMICALLY over `save/<name>/` (old folder
  swapped aside and removed only after the rename lands; a failed verify
  discards staging and leaves the previous save untouched) and
  `PKT_SAVE_ACK`s. A save initiated on the JOIN is suppressed locally and
  forwarded as `PKT_SAVE_REQ` (host arbitrates - one authoritative save).
  Resume = both clients relaunch on the identical file.
  `KENSHICOOP_SAVE_SYNC` (default ON, forced OFF in `save_probe`) is the A/B
  hatch. Validated: `save_sync` run 111007 (35 files / 3,730,759 bytes sent =
  committed, badCrc=0, ACK ok=1, quiesce 2078 ms + transfer 1766 ms);
  `resume_test.ps1` two-stage run 111333 - stage 1 bakes a SESSION-PLACED
  half-built site into the coordinated save, stage 2 relaunches both on it
  (no harness mirror - the join loads what the TRANSFER wrote) and both
  clients enumerated the building under the SAME save-stable hand
  (`hand=0.2069.11111.1053.3641357312 prog=0.5` on both sides - the
  identity-reset claim, proven); regressions green (`coop_presence`,
  `build_sync`, `latejoin_sync`, `faction_sync`, `money_sync`, `time_sync`).
- **Addendum - coordinated LOAD (protocol 32, wire version 31, DONE
  2026-07-09):** a MID-SESSION load on the host now automatically drives the
  join to load the identical save (before this, a host load silently forked
  the two worlds until a manual restart). Probe findings (`load_probe`, runs
  2026-07-09): `SaveManager::load` only SETS a deferred LOADGAME signal;
  mid-session the engine usually consumes it itself within ~0.5 s (the title
  screen loop is the guaranteed consumer), so the plugin arms a 2 s
  grace-window backstop that pumps `SaveManager::execute()` manually from
  end-of-tick if the signal sits; the swap takes ~4-5 s during which
  `mainLoop_hook` keeps ticking; and a real world rebuild REALLOCATES every
  `Character*` while save-stable hands re-resolve - the stale-pointer hazard
  the session reset covers. Mechanism: `SaveManager::load` detour (every
  local load logs a `[load] LOCAL-LOAD` edge); a HOST edge broadcasts
  `PKT_LOAD_GO` (name + folder fingerprint = FNV-1a over sorted lower-cased
  relative paths + per-file content CRCs) and loads natively; the JOIN
  verifies its on-disk copy against the fingerprint - MATCH loads
  immediately (a bypass-once lever through its suppressed detour), missing/
  diverged `PKT_LOAD_NACK`s and the host streams the folder over the
  existing protocol-31 SaveXfer after its own reload, with the join loading
  on the verified commit. A join-initiated load is suppressed locally and
  forwarded as `PKT_LOAD_REQ` (host arbitrates, mirroring save). On each
  side's own reload edge (gameplay non-live >= 400 ms then live, in
  `mainLoop_hook`) the plugin runs the protocol-32 SESSION RESET:
  `Replicator::resetSession()` clears every pointer cache, session map,
  change-gate baseline and interp buffer (preserving config gates, the
  ownership partition and all OUTBOUND seq counters - a peer that did not
  reload keeps its stale-row guards) plus an inbound world-state queue
  flush. `KENSHICOOP_LOAD_SYNC` (default ON, forced OFF in `load_probe`) is
  the A/B hatch. Validated: `load_sync` run 131521 (GO broadcast ->
  fingerprint MATCH -> join followed, both swaps ~4.5-4.8 s, both session
  resets ran, and a SESSION-PLACED building enumerated on BOTH sides
  POST-load under the SAME save-stable hand); regressions green
  (`save_sync`, `resume_test.ps1` both stages, `coop_presence`,
  `build_sync`, `latejoin_sync`).
- **Remaining (accepted edges):** join-local-only state (camera, control
  groups, anything unsynced) is lost at resume by design; the transfer ships
  uncompressed (the fixture gzips ~8:1 - compression is a later optimization,
  not a correctness need); autosaves also trigger the transfer (bounded by
  the 10-minute autosave cadence); a host load DISCARDS join-local unsaved
  progress by design (host-authoritative); if the join is mid-load when host
  packets arrive they queue and the reset flushes them (loadId guards stale
  GO/NACK pairs).
- **Spikes:** 5, 39 (both DONE), backlog theme 13 (#341-356).

### 9. Late-join / reconnect state reconciliation [CONNECT-EDGE RESYNC DONE 2026-07-08 - protocol 30, no wire change]

- **Symptom (was):** a late joiner / reconnecting client trusts the shared
  save + live streams; anything that diverged before the connect stays
  diverged until each channel's safety resend happens to cover it - and the
  one-shot describe/mint edges (PKT_BUILD_PLACE) are lost FOREVER (a
  pre-connect placement never mints, which also strands its STATE rows, its
  PKT_BUILD_DOOR rows and any later PKT_BUILD_REMOVE).
- **Mechanism (protocol 30, DONE):** a connect-edge resync. Both sides
  already surface the connect on the game thread (`Inbound::drainConnects`
  in `processNetEvents`; host pushes on HELLO, join on WELCOME).
  `Replicator::onPeerConnected()` runs there: re-announces every live
  `ownBuilds_` PLACE from retained announce data (+ REMOVE for removed
  ones - the receiver's session maps dedupe both) and un-latches their
  STATE rows, then forces an immediate resend pass over every change-gated
  cache by aging `lastSendMs = 1` on rows EVER SENT (each channel's own
  safety-resend condition fires on its next sample:
  faction/door/bdoor/medical/stats/money/inventory/world-items). Rows never
  sent - the seeded shared-save baseline - correctly stay silent; edge-only
  caches (weapon census, KO/death edge state) are deliberately untouched
  (re-seeding would author phantom edges). No wire change -
  `PROTOCOL_VERSION` stays 29 (protocol 30 is doc numbering). Probe
  findings (`latejoin_probe`, run 230601, gate forced OFF): the pre-connect
  building NEVER minted (permanent), door/faction/money healed only via
  their safety resends, and the pre-arm surface works (mutations at
  gameplay-start t+3 s, connect ~14 s later). Validated: `latejoin_sync`
  run 231429 (the pre-connect building MINTED + latched complete on the
  join right after connect - `[latejoin] RESYNC place=1 ... med=1 stats=1
  money=1` burst on the host edge; faction sentinel + wallet censuses
  agreed from the first post-arm sample); regressions green (`npc_sync`
  machine-load flake, A/B-exonerated: fails identically with the gate OFF).
  `KENSHICOOP_LATEJOIN_SYNC` (default ON, forced OFF in `latejoin_probe`)
  is the escape hatch.
- **Remaining (accepted edges):** event HISTORY is not replayed (KO/death
  events, recruit re-keys, weapon-drop intents fired before the connect -
  the medical/stats snapshots self-heal the state those events carried, and
  the shared-save workflow bounds the rest; recruit re-key replay rides gap
  8's save coordination); the resync bursts one full snapshot per channel
  on CH_RELIABLE (bounded by squad size + nearby state); a reconnecting
  client keeps its own session maps (`peerBuilds_`, `proxyByKey_`), so
  duplicate re-announces dedupe by design.
- **Spikes:** backlog theme 19 (#437-450).

### 10. Squad management mid-session [DONE 2026-07-09 - protocol 35]

- **Symptom (was):** moving a unit between squad tabs (UI drag, "split into
  new squad", createSquad) broke it on the peer - the mover streamed an
  unresolvable new hand while the peer's copy of the old hand went stale
  (frozen/duplicated body), and any mid-session tab-set change could
  RESHUFFLE the sorted-container rank partition (whole-tab ownership flip)
  or mint a tab in ranks nobody owns (both engines simulate it divergently).
- **Probe findings (squad_probe runs 185825/191911):** a squad-tab move
  re-containers the body and mints a FULLY fresh hand (container AND
  index/serial change - unlike a baked recruit, nothing survives); the
  `Character*` body pointer DOES survive, so a per-tick pointer->hand diff
  catches every move flavor without a UI detour; a separate on a SOLO tab is
  an engine no-op; `Character::setFaction(playerFaction, targetPlatoon)`
  works as a programmatic move-into-existing-tab lever; a move back does NOT
  restore the original hand (every hop is a new re-key); with sync off the
  peer REQ/minted a duplicate proxy for the moved hand within ~100 ms.
- **Shipped (protocol 35, wire v34):** `Replicator::publishSquadMoves` polls
  the roster's pointer->hand baseline EVERY tick (engine
  `pollSquadRoster`/`drainSquadMoveEdges`) and authors one reliable
  `EVT_SQUAD_MOVE` per edge (subject = old hand, actor = new hand; zeroed
  actor = left roster), pinning the new hand's ownership BEFORE the wire.
  The receiver shares the EVT_RECRUIT re-key path (`rekeyPeerBody`): bind
  the old body to the new key in `proxyByKey_`, restore if suppressed, pin
  peer-owned - the pins are the generalized `pinOwned_`/`pinPeer_` sets
  covering recruits AND moves. The rank partition is LATCHED
  (container->rank assigned at first census, newly-seen containers APPEND) -
  a mid-session tab can never reshuffle existing ranks, and an appended tab
  inherits its author's ownership through the per-hand pins. Re-key also
  retires the OLD key's stream state (`targets_`, REQ/mint grace stamp
  `rekeyedOld_`) and repairs a duplicate that beat the edge (cull the mint,
  rebind the real body) - run 192211 showed both races live. Session reset
  clears latch + pins + the engine baseline. `KENSHICOOP_SQUAD_SYNC=0` is
  the escape hatch; `squad_probe` forces it off. Gates: `squad_sync` (all
  four scripted moves land, edge + REKEY ok=1 each, zero duplicate proxies,
  zero unresolved storms, peer tracks the final hands, pre-existing census
  ranks stable); regressions coop_presence, npc_sync (known time-slew
  smoothness flake, pre-existing), recruit_sync, money_sync, inv_bidir,
  latejoin_sync, save_sync green.
- **Remaining edge:** peer squad-UI roster placement stays deferred (same
  stance as recruits - moved units are visible/driven on the peer, not in
  its squad UI); a late joiner misses pre-connect move edges (bounded by
  the shared-save workflow, rides gap 8); dismissal (roster exit) edges are
  wired but untested (no programmatic dismiss lever).
- **Spikes:** squad_probe/squad_sync findings (runs 185825, 191911, 192211,
  193304).

### 11. Play-session bugs 2026-07-09 [DONE 2026-07-10 - protocol 36]

Four field bugs from the first real remote session, shipped as one bump
(protocol 36 / wire v35: `PKT_NPC_CENSUS` + entity-batch send timestamp).

- **Ghost-NPC culling radius.** Symptom: the join saw NPCs the host didn't
  have until a player walked within ~200 u (the stream bubble) - render
  range is far larger. Shipped: existence split from position. The host
  sends `PKT_NPC_CENSUS` at 1 Hz - the hand list of every NPC within
  `KENSHICOOP_CENSUS_RADIUS` (default 2000 u, published 25% wider than the
  join culls against so boundary NPCs aren't false-culled) via
  `engine::listNpcsWide`. The join widens `enforceHostAuthority` with a
  second wide-radius pass: NPCs absent from a FRESH census (<=5 s old)
  suppress through the existing 75/150-frame hysteresis; a stale census
  DISABLES wide culling rather than mass-suppressing. Driven bodies and
  the near-bubble keep the streamed/fresh logic unchanged. Gates:
  `npc_census` (4 baked far ghosts culled on the join, zero culls of
  census-listed NPCs); regressions npc_sync (pre-existing crosscheck
  flake, A/B-exonerated), coop_presence green.
- **Jumpy remote-player movement.** Session evidence: slew sat at 1.00
  (constant slewing exonerated) but ~140 hard snaps fired, and the interp
  buffer indexed on ARRIVAL time - relay jitter inflated straight into the
  buffer. Shipped: sender-side millisecond timestamp on every entity batch
  (`EntityBatchHeader.sendMs`, wire v35; `ENTITY_BATCH_MAX` 18 -> 17), a
  per-peer min-offset clock map on the receiver (creep-resistant, snaps on
  >2 s breaks), lag-aware `renderDelay` (peak queueing lag joins the
  jitter term), and interp/drive tuning exposed as env knobs
  (`KENSHICOOP_INTERP_*`, `KENSHICOOP_CATCHUP_K`, `KENSHICOOP_SNAP_DIST`,
  A/B lever `KENSHICOOP_SEND_STAMP`). Telemetry: per-mode interp counters
  + snap/reissue counts in the `[interp]` stat line and `SCENARIO INTERP`
  summary. Validated A/B under the WAN profile (60+/-25 ms, 1% loss):
  send-stamping halves interp jitter (~13 -> ~6 ms), cuts extrapolation
  (extrapFrac 0.088 -> 0.026), stabilizes loopback zeroFrac; regressions
  green.
- **Guard can't jail the join PC.** Root cause: `EVT_ENTER_FURNITURE` is
  occupant-owner-authored, but a guard jailing runs purely on the host
  sim - the owner never sees the action, the owner's stream keeps
  reporting no cage bit, and the host's 3 s furniture self-heal ejected
  the driven body ("the host kept taking it out"). Shipped: third-party
  placement authority. The host detects local furniture occupancy on a
  peer-owned driven squad body that is KO'd/down (`applyTargets`), authors
  `EVT_ENTER_FURNITURE` for it (re-authored every 5 s until the owner's
  stream carries the bit), and HOLDS its self-heal exit meanwhile. The
  join relaxes the own-hand skip in `applyEvents` for furniture enters
  when its own body is down and not already placed; a recent owner-side
  exit (10 s grace, `ownFurnExit_`) vetoes a stale in-flight enter.
  Conscious voluntary use stays owner-authored; exit stays symmetric.
  Gates: `cage_peer_sync` (host authored, join applied to its OWN body,
  occupancy held through a 28 s window, zero HEAL EXITs, clean owner
  exit); regressions cage_put, carry_order, npc_carry, coop_presence
  green.
- **Blank host squad portraits after reload.** Diagnosis: the engine
  writes `portraits_texture.png` as a late, separate step; the
  quiescence gate (quick.save + 1.5 s stable) could declare a save
  complete before the atlas landed, shipping/reloading a portrait-less
  folder. The 07-09 session's join copies ('people', 'Group') DID carry
  valid atlases, so the friend-host repro evidence is indirect (no host
  log); the gate race is real regardless. Shipped: quiescence holds a
  settled folder for `portraits_texture.png` up to 10 s past arm (bounded
  - a genuinely portrait-less save still completes, with a WARN), and
  every coordinated-load issue point logs a WARN when the target folder
  lacks the atlas. `load_sync` oracle now fails on either WARN and on a
  committed copy without the file on disk. If a future session log shows
  the WARNs clean AND blank avatars anyway, escalate to the engine-side
  PortraitManager investigation.

## Accepted edges (documented, deliberately out of scope)

- Ambush/NPC dialog content is not synced (backlog #379/380) - players see
  the squad and the fight, not the conversation.
- Proxy NPC appearance/equipment approximates the host's body (protocol 21).
- Spawn FREQUENCY far from the host camera differs from vanilla.
- Cage locks / lockpicking / slavery systems (doctrine 31 out-of-scope list).
- Cross-rendering the other player's detection arrows (engine limitation).
- Wall clocks are ms-since-midnight (sessions spanning midnight confuse log
  alignment only).
