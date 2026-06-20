# Kenshi Co-op Handoff: World Item Sync (Phases W0–W3)

> Status: Working / committed (`Co-op MP: world item sync - ground items +
> conservation drop/pickup`). Written as context for future sessions. Grounded
> in the actual code in `src/plugin/sync/Replicator.{h,cpp}`,
> `src/plugin/game/Engine.{h,cpp}`, `src/netproto/Wire.h`,
> `src/plugin/net/NetLink.cpp`, and `src/plugin/core/Inbound.h`. Companion to
> `POSTMORTEM.md` (doctrines 1–27) and `MASTER_PLAN.md` (charter).

## TL;DR for the next session

- Squad-dropped **gear** (weapons + armour/clothing) replicates drop AND pickup,
  **bidirectionally**, host↔join, by **relocating the real object** on each
  client — never fabricating it. This is the **conservation model**.
- Everything **non-gear** (stacks, future loot/corpses) rides the older
  **W1 host-authoritative `netId` proxy** stream.
- The single most important hard-won fact: **the engine's spatial item query
  (`getObjectsWithinSphere`) is unreliable in towns** — it returns 0 even for
  items lying in plain sight at a 240u radius. Do NOT build anything that
  depends on re-finding a ground item by position. We track the real `Item*`
  handle instead.
- Validated manually (inhabit mode, shared save): drop pants+weapon from both
  clients → all 4 show on both → join picks up all 4 → all carried on both,
  nothing orphaned. A *slight* positional drift remains (accepted).

## What ships today

### Phase W1 — host-authoritative ground items (non-gear)

The host scans free ground items near the players, assigns each a synthetic
32-bit `netId`, and streams a descriptive snapshot; the join spawns/moves/culls
a LOCAL proxy keyed by `netId`. Host-authoritative, interest-scoped,
change-detected (a settled world produces zero traffic + a slow safety resend).

- Wire: `PKT_WORLD_ITEM` (snapshot) / `PKT_WORLD_ITEM_REMOVE` (cull) +
  `WorldItemEntry` (`Wire.h`).
- Replicator: `publishWorldItems` (host), `applyWorldItems` (join).
  State: `worldTrack_` (host: localHand→netId+hash+pos), `worldProxies_`
  (join: netId→proxy), `nextWorldNetId_`.
- Engine: `captureWorldItems`, `spawnWorldItemProxy`, `updateWorldItemProxy`,
  `removeWorldItemProxy`.

### Phase W2/W3 — conservation drop + pickup (gear)

A WEAPON cannot be rebuilt on a peer (`createItem` returns null for all weapon
templates — see "What does NOT work"), so the W1 "stream a template, spawn a
proxy" path can never show one. The conservation model fixes this WITHOUT
creating anything: both clients already have the item (shared save, and for
armour the inventory-sync layer also reconstructs it), so the OWNER of the
acting character authors a reliable intent and **each client relocates its own
real copy** bag↔ground.

- Wire: `PKT_WORLD_DROP` / `PKT_WORLD_PICKUP` + `WorldDropPacket` /
  `WorldPickupPacket` (`Wire.h`). `PROTOCOL_VERSION = 11`.
- Detection (`Replicator::detectAndPublishWeaponDrops`, runs every tick on
  EVERY client, BEFORE the engine tick): diff each OWNED character's GEAR
  census (`captureContainerContents` filtered to `isGearType`).
  - count **DECREASE** ⇒ DROP (we never mutate owned inventories ourselves, so a
    loss is a genuine user action).
  - count **INCREASE** ⇒ PICKUP.
- Apply (AFTER the engine tick, BEFORE `applyInventories`):
  `applyWeaponDrops` (relocate own copy to ground) and `applyWeaponPickups`
  (re-home own tracked ground copy into the bag). Idempotent by
  `(ownerId, dropId/pickupId)`; never acts on a hand in `ownHands_` (no echo).
- Engine primitives: `relocateWeaponToGround` (drop own copy + reposition the
  exact object by handle), `addItemPtrToInventory` (`tryAddItem` of the tracked
  object — ground→bag, no fabrication), `captureWeaponPtrs` (real `Item*` per
  gear sid), `dropItemFromInventory(..., outLastDropped)`, `itemWorldPos`,
  `objectWorldPos`, `unequipItemToLoose`.
- Replicator state: `weaponCensus_` (per owned hand: `WCensus{ items, retries,
  ptrs, seeded }`), `groundedWeapons_` (sid→deque of real ground `Item*` THIS
  client tracks), `nextDropId_`/`nextPickupId_`, `appliedDrops_`/`appliedPickups_`.

### Gear vs. proxy routing

`isGearType(t)` ⇒ `t == 2 (WEAPON) || t == 3 (ARMOUR/clothing)`. Gear goes
through conservation; `publishWorldItems` skips gear. **These itemType numbers
were verified empirically** from `KENSHICOOP_INV_DUMP=1` traces
(`52295-rebirth.mod` weapon = type 2, `2308-clothes_v1.mod` clothes = type 3) —
the stale code comment claiming "0=WEAPON,1=ARMOUR,2=ITEM" is WRONG; don't trust
it.

## What works (keep doing this)

- **Conservation over fabrication.** For any object the peer already has
  (shared save, or reconstructed by inventory sync), MOVE the real object rather
  than destroy+recreate. Survives identity/quality, dodges the createItem
  limitation, and makes pickup a clean re-home.
- **Track the real `Item*` handle; never re-query the ground.** On a DROP we
  remember the departed item's pointer (the prior census tick's handle — after a
  UI drop it persists as the now-grounded object). On a peer-applied drop we
  remember the relocated object (`relocateWeaponToGround`'s `outDropped`).
  Pickup re-homes that exact pointer; the drop POSITION is read straight off it
  (`itemWorldPos`). All query-free, so both clients agree.
- **Order matters: conservation runs BEFORE `applyInventories`.** The relocation
  beats the (debounced) inventory reconcile that would otherwise try to
  destroy/recreate the item. Both converge: after the move, owner bag and peer
  bag agree, so reconcile is a no-op.
- **Census diff on OWNED characters only**, with idempotency by `(ownerId,id)`
  and an echo-guard via `ownHands_`. The acting client authors; the peer acts.
- **Debounce the DROP** (30 ticks ≈ 240 ms, `MAX_RETRY`) so a 1-frame
  equip-swap / cursor transient never fires a phantom drop. (Equip↔unequip keeps
  the census count unchanged, so it never looks like a drop in the first place.)
- **Diagnostics first.** `KENSHICOOP_INV_DUMP=1` turns on `[wd]` (conservation)
  and `[wi]` (W1 proxy) traces + full inventory dumps. Every cross-client
  desync this arc was diagnosed by reading both clients' logs side by side, not
  by guessing.

## What does NOT work (don't re-learn the hard way)

- **`RootObjectFactory::createItem` (6-arg) returns null for all weapons.** You
  cannot fabricate a weapon on a peer, even with manufacturer+material GameData.
  This is THE reason the conservation model exists. (Armour/loose items can be
  created, but see below.)
- **Fabricated equipped gear is discarded (Doctrine 25).** Even when create
  succeeds, equipping a fresh blank-handle item is dropped by the engine within
  a tick or two. Equip a real (synced) loose copy, never `create+equipItem`.
- **`getObjectsWithinSphere` / `g_getObjsFn` is unreliable in towns.** Returns 0
  even at a 240u radius for items visibly on the ground. Anything that "find the
  dropped item near the character" WILL intermittently fail in town play. We
  abandoned position-based correlation entirely in favour of handle tracking and
  an owner-position fallback.
- **W1's cull does not remove the host's real item.** `PKT_WORLD_ITEM_REMOVE`
  only deletes the JOIN's proxy. A host-dropped real item that the join "picks
  up" (it grabs its proxy) was orphaned on the host ground forever — this was
  the last visible bug, and it's why GEAR was moved off W1 onto conservation
  (which re-homes the real object on both ends).
- **Per-unit conservation is wrong for stacks.** The census sums quantity, so a
  dropped stack of 50 would author 50 intents. Conservation is therefore
  restricted to non-stackable GEAR (`isGearType`); stacks stay on W1.
- **Trusting the itemType comment in `Engine.cpp`.** Verify type numbers from a
  live `INV_DUMP`, not from comments.

## How to run / validate

```
powershell -ExecutionPolicy Bypass -File scripts/manual_session.ps1 -Save "c" -Inhabit -InvSync -WorldSync -InvDump -SkipBuild
```

- `-Inhabit`: both clients load the SAME save; host owns rank 0 (leader), join
  owns the rest (`~0`). Required for the gear census to attribute drops to an
  owner.
- `-WorldSync` gates the whole channel (`KENSHICOOP_WORLD_SYNC`); `-InvSync`
  gates inventory reconcile; `-InvDump` sets `KENSHICOOP_INV_DUMP=1`.
- Build: `cmd /c scripts\build_plugin.cmd` (VS2010/v100 x64). The manual script
  deploys to both installs unless `-SkipDeploy`. **Bumping `PROTOCOL_VERSION`
  means BOTH installs need the new DLL or the handshake rejects.**

Oracle/scenario tests (self-exit + verdict, run via `scripts/run_test.ps1`):
`drop_probe` (W0), `world_item_sync` / `Test-WorldItemSync`, `wpn_relocate` /
`Test-WpnRelocate`, `world_weapon_drop` / `Test-WeaponDrop`. Note: oracles run in
contexts where the spatial query often DOES work, so they passed while manual
town play failed — **manual validation in a town is the real test** for anything
touching ground-item discovery.

## Proposed doctrines (continue the POSTMORTEM numbering)

These extend the catalogue in `POSTMORTEM.md` (currently ends at Doctrine 27);
fold them in if/when that file is next updated.

- **Doctrine 28 — Conserve, don't fabricate: replicate a drop/pickup by MOVING
  the peer's own real object.** An item the peer already has (shared save, or
  reconstructed by inventory sync) must never be destroyed+recreated to mirror a
  drop. The owner authors a reliable intent (`PKT_WORLD_DROP`/`PKT_WORLD_PICKUP`)
  and each client relocates its real copy bag↔ground (`Inventory::dropItem` /
  `tryAddItem` of the existing `Item*`). This is the only way to show a WEAPON
  (createItem returns null) and is strictly better than fabrication for gear
  (identity/quality survive; pickup is a clean re-home). Detect via the OWNED
  character GEAR census: a sustained count decrease = drop, an increase = pickup.
  Runs BEFORE `applyInventories` so the move beats the destroy/recreate reconcile.

- **Doctrine 29 — Don't trust the spatial item query; track the real `Item*`
  handle.** `getObjectsWithinSphere` is unreliable in towns (returns nothing for
  items in plain sight). Never re-find a dropped item by position. Remember the
  actual object pointer (the prior census tick's handle becomes the grounded
  object after a UI drop; a peer-applied drop captures the relocated object),
  re-home THAT exact pointer on pickup, read the drop position off it, and
  reposition the peer's copy by handle. SEH-guard every dereference; a destroyed
  object is the only failure mode and it degrades to a no-op.

- **Doctrine 30 — Route gear through conservation, stacks/loot through the W1
  proxy.** Conservation is per-OBJECT, so it fits non-stackable equippable gear
  (`itemType` 2 WEAPON / 3 ARMOUR, verified from `INV_DUMP`, not comments).
  Stackable items would explode into per-unit intents, and items the peer truly
  lacks (future runtime loot/corpses) can't be relocated — those stay on the
  host-authoritative `netId` proxy stream (`publishWorldItems` skips gear). Note
  the W1 proxy cull only removes the JOIN's proxy, so the host's real item is
  orphaned when the join picks a host-dropped item up — which is exactly why
  gear must NOT be on W1.

## Known limitations / open work

- **Slight positional drift** between clients on dropped items remains (the
  drop authors off the real object, the peer repositions by handle, but the two
  copies still settle a little apart). Accepted as good-enough; revisit if it
  ever matters.
- **Pickups are not debounced.** A genuine increase is assumed to be a pickup.
  The peer no-ops if it has no tracked ground copy, so a spurious increase is
  safe today, but a transient false-increase while the peer DOES hold a copy
  could mis-fire. Add a small debounce if observed.
- **`groundedWeapons_` is keyed by sid (with a count), not per-instance.**
  Multiple identical items are interchangeable (the conservation result is the
  same), which is fine for gameplay but means you can't target a *specific*
  instance.
- **Only `itemType` 2 and 3 are gear.** If other equippable categories exist
  (heavy armour variants, etc. with a different type number) they'd fall back to
  W1 and could show the orphan bug. Confirm via `INV_DUMP` and extend
  `isGearType` if found.
- **Phase W4 (not started):** generalize the `netId` channel to runtime corpse
  loot / save-stable chests; document as doctrine. This is where the W1 proxy
  path (for peer-lacking items) earns its keep.

## File map (where things live)

| Concern | File / symbols |
| --- | --- |
| Wire formats + version | `src/netproto/Wire.h` — `PKT_WORLD_ITEM/_REMOVE/_DROP/_PICKUP`, `WorldItemEntry`, `WorldDropPacket`, `WorldPickupPacket`, `PROTOCOL_VERSION=11` |
| Net→game queues | `src/plugin/core/Inbound.h` — `InboundWorldItems/Remove/Drop/Pickup` + push/drain |
| Send/recv | `src/plugin/net/NetLink.cpp` — `queueWorldItems/Remove/Drop/Pickup`, recv dispatch |
| W1 publish/apply | `src/plugin/sync/Replicator.cpp` — `publishWorldItems`, `applyWorldItems` |
| Conservation detect/apply | `src/plugin/sync/Replicator.cpp` — `detectAndPublishWeaponDrops`, `applyWeaponDrops`, `applyWeaponPickups`, `isGearType` |
| Engine primitives | `src/plugin/game/Engine.cpp` — `captureWorldItems`, `spawn/update/removeWorldItemProxy`, `relocateWeaponToGround`, `addItemPtrToInventory`, `captureWeaponPtrs`, `dropItemFromInventory`, `itemWorldPos`, `objectWorldPos`, `unequipItemToLoose`, `diagGroundScan` |
| Tick wiring | `src/plugin/Plugin.cpp` — publish-phase `detectAndPublishWeaponDrops`; apply-phase `applyWeaponDrops` → `applyWeaponPickups` → `applyInventories` (all gated on `worldSync`) |
| Config gates | `src/plugin/core/Config.{h,cpp}` — `worldSync`, `invSync` |
| Tests | `src/plugin/test/Scenario.cpp` + `scripts/run_test.ps1` — `drop_probe`, `world_item_sync`, `wpn_relocate`, `world_weapon_drop` |
