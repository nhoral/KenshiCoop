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
runtime NPC spawn proxies + suppression hardening (protocol 21); Steam P2P
transport. See the doctrine list (14-33) for the mechanism of each.

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
  (`recruitOwned_`/`peerRecruit_` pins - the probe showed a join recruit
  landing in the host-owned rank-0 container). `KENSHICOOP_RECRUIT_SYNC=0` is
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

### 3. Faction relations / reputation / aggro

- **Symptom:** attacking a faction (or paying off a bounty) flips hostility on
  one client only - guards attack one player's view of the world.
- **Root cause:** relation values, bounties, and hostility triggers are local
  faction state; no channel exists.
- **Mechanism (candidate):** host-authoritative relation snapshot (faction
  stringID -> relation value, change-gated reliable) + reliable edges for
  discrete events (bounty added/paid). Join-side aggro consequences already
  render via the driven world (host NPCs attack on the host's authority).
- **Spikes:** backlog theme 7 (#201-218), all unrun. Needs a STATIC spike on
  the relation-value surface first.

### 4. Game calendar / time-of-day

- **Symptom:** speed and pause are consensus (doctrine 24), but the CLOCK
  itself drifts (different load/pause moments); day/night drives NPC
  schedules, shop hours, stealth vision.
- **Root cause:** no packet carries game time; each client integrates its own.
- **Mechanism (candidate):** host-authoritative absolute game-time broadcast
  (low rate, reliable) + join-side set on join and on drift beyond a
  tolerance. Spike 34 confirmed the read/write surface.
- **Spikes:** 34 (DONE - surface known), backlog #322.

## Priority 2 - structural, bites longer campaigns

### 5. Buildings and base-building

- **Symptom:** player-placed buildings, construction progress, production
  machines, research, farming, and door/gate states exist per-client.
- **Root cause:** a placed building is a runtime object (host-only hand) -
  the protocol-21 identity problem for structures; continuous states
  (progress %, door open) have no channel.
- **Mechanism (candidate):** describe/mint proxy shape for placements
  (template sid + transform), content-snapshot shape for progress/production,
  reliable edges for door open/close. Door state is the cheapest first win.
- **Spikes:** backlog theme 17 (#399-418), all unrun.

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

### 7. Hunger and status effects

- **Symptom:** hunger, drunk/drugged states diverge on driven copies (doctrine
  20 explicitly flags hunger as a local-only model needing the medical
  treatment).
- **Root cause:** local-only models with no stream; cosmetic simulation can
  also write them locally (the XP-pollution class of bug).
- **Mechanism (candidate):** fold into the owner-authoritative medical
  snapshot (hunger is a scalar; status effects enumerate) - the protocol-16
  machinery has room.
- **Spikes:** backlog #142, #384, #397.

## Priority 3 - session flow (world-state adjacent)

### 8. Save/load coordination + join-side persistence

- **Symptom:** no coordinated save; a resumed session needs a manually
  re-mirrored save; join-side progression persists only in the join's own
  save (documented stats limitation).
- **Mechanism (candidate):** DLL-triggered coordinated save (spike 5 DONE -
  trigger surface known; spike 39 DONE - custom-blob persistence known) +
  session-resume flow in the harness/kit scripts.
- **Spikes:** 5, 39 (both DONE), backlog theme 13 (#341-356).

### 9. Late-join / reconnect state reconciliation

- **Symptom:** a reconnecting client trusts the shared save + live streams;
  anything that diverged while disconnected stays diverged until each
  channel's safety resend happens to cover it.
- **Mechanism (candidate):** on WELCOME, force a full-state publish of every
  reliable channel (inventory hashes cleared, medical/stats fingerprints
  cleared, world-item snapshot resent) - mostly a "clear the change-gate
  caches on peer-connect" pass.
- **Spikes:** backlog theme 19 (#437-450).

## Accepted edges (documented, deliberately out of scope)

- Ambush/NPC dialog content is not synced (backlog #379/380) - players see
  the squad and the fight, not the conversation.
- Proxy NPC appearance/equipment approximates the host's body (protocol 21).
- Spawn FREQUENCY far from the host camera differs from vanilla.
- Cage locks / lockpicking / slavery systems (doctrine 31 out-of-scope list).
- Cross-rendering the other player's detection arrows (engine limitation).
- Wall clocks are ms-since-midnight (sessions spanning midnight confuse log
  alignment only).
