# Spike 451 - Weapon fabrication: capture the engine's real weapon-mint recipe

- Type: RUN
- Status: DONE
- Save: sync
- Branch commit: <filled at commit>

## Goal

`createItemAndAdd` cannot fabricate WEAPONS: our 6-arg `RootObjectFactory::createItem`
call returns null for every weapon template, even with a live weapon's exact
manufacturer/material GameData pointers (`diagWeaponCreate`: 0/24, all 6 recipe
variants null). This is the last item-loss vector in trading - a weapon acquired
mid-session (loot, vendor, container grab) exists only on the acquiring client and
can never be reconstructed on the peer. The engine mints weapons at runtime
constantly (armed NPC spawns, vendor stock), so: watch the ENGINE do it, capture its
exact arguments, and replay them from plugin context in the same run.

## Method

Five detours (`engine::installCreateItemTraceHook`), each logging full args, caller
RVA and result as `[mkspy]` lines:

- `RootObjectFactory::createItem` (6-arg) - every call (capped), capturing the last
  successful weapon mint's argument pointers,
- `RootObjectFactory::createItem(GameData* itemState)` (1-arg) - weapon-typed calls,
- `RootObjectFactory::copyItem` - weapon-typed calls,
- `RootObjectFactory::chooseDataFromList` - scoped to in-flight createItem6 calls,
- `Sword::_CONSTRUCTOR` (RVA 0x897330) - the weapon-object construction sink.

Script (SpikeScenario id 451, host side): @3s `spawnRuntimeSquad(gw, 2)` - the armed
runtime NPCs make the engine mint weapons; @14s `diagWeaponCreate` - our known-failing
calls, now traced side by side; @20s `probeReplayWeaponMint` - re-issue the captured
engine args from plugin context into the leader's inventory.

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 451 -Save sync
```

## Raw evidence

Engine weapon mints (host.log; gd types: 2=WEAPON, 50=MATERIAL_SPECS_WEAPON,
51=WEAPON_MANUFACTURER; hand t=11 is NULL_ITEM = blank handle):

```
[mkspy] mk6 gd='912-gamedata.base'/51 mesh='52295-rebirth.mod'/2 mat='(null)'/-1 lvl=0 fac=0 hand={i=0 s=0 t=11 c=0 cs=0} ret=1 caller=+0x621917
[mkspy] mk6 gd='917-gamedata.base'/51 mesh='924-gamedata.base'/2 mat='925-gamedata.base'/50 lvl=0 fac=0 hand={i=0 s=0 t=11 c=0 cs=0} ret=1 caller=+0x582ACD
[mkspy] CAPTURED weapon recipe (entry gdType=51)
[mkspy] swordCtor gd='924-gamedata.base' company='917-gamedata.base'/51 mat='925-gamedata.base'/50 lvl=20 hand={i=0 s=0 t=11 c=0 cs=0} ret=1 caller=+0x5805F2
```

Armour/item mints through the SAME entry point put the ITEM template first, as we
always assumed (3=ARMOUR, 47=MATERIAL_SPECS_CLOTHING):

```
[mkspy] mk6 gd='2308-clothes_v1.mod'/3 mesh='(null)'/-1 mat='(null)'/-1 lvl=20 fac=0 hand={i=0 s=0 t=11 c=0 cs=0} ret=1 caller=+0x62D306
[mkspy] mk6 gd='2154-gamedata.base'/3 mesh='(null)'/-1 mat='2153-gamedata.base'/47 lvl=20 fac=0 hand={i=0 s=0 t=11 c=0 cs=0} ret=1 caller=+0x582B53
```

Our failing diag calls (run 2; weapon template FIRST, manufacturer as `weaponMesh`),
all null even with the live weapon's genuine pointers:

```
[wpndiag] live wpn sid='52295-rebirth.mod' gdType=2 manType=51 matType=50 colType=-1
[wpndiag] recipe[0] real man,mat lvl-1 blankH -> null
... (all 6 variants) -> null
```

The replay of the captured (manufacturer-first) args from plugin context:

```
[mkspy] replay gd='917-gamedata.base' lvl=0 fac=0 -> created=1 added=1
SPIKE 451 replay res=1
```

Run 3 (after fixing `createItemAndAdd` + `diagWeaponCreate` to the discovered shape):

```
[wpndiag] tmpl[0..23] sid='...' -> OK   (every template)
[wpndiag] RESULT ok=24/24 (spike-451 manufacturer-first recipe)
SPIKE 451 fab loose res=1 sid='52309-rebirth.mod'   (createItemAndAdd wire path)
SPIKE 451 fab equip eq=0                            (nothing loose left to equip - see below)
SPIKE 451 fab persist loose=0 worn=1                (8s later: WORN and persistent)
```

## Findings

1. **The 6-arg `createItem`'s first two GameData args are role-SWAPPED for weapons.**
   The engine passes the WEAPON_MANUFACTURER GameData (type 51) as the FIRST arg
   (`gd`) and the WEAPON template (type 2) as the THIRD arg (the KenshiLib header
   calls it `weaponMesh`). Every previous attempt - `createItemAndAdd`,
   `diagWeaponCreate` - passed the weapon template first, which is why weapons
   (and only weapons) always returned null. Armour/items keep template-first.
2. **The full engine weapon recipe:** `createItem(manufacturerGd, blankHand(t=NULL_ITEM),
   weaponTemplateGd, materialSpecsWeaponGd_or_null, levelOverride=0, faction=0)`.
   The material arg may be null (912/52295 mint) or a MATERIAL_SPECS_WEAPON record
   (917/924 mints); `levelOverride` is 0 in every observed engine mint - the
   concrete grade emerges inside (Sword ctor received lvl 15/20/25/30/40).
3. **Internally the factory routes to the subclass constructor with the roles
   restored:** `Sword::_CONSTRUCTOR(baseData=weaponTemplate, companyData=manufacturer,
   materialData, blankHand, level)` at caller `createItem6+0x622` (+0x5805F2).
4. **The recipe works from PLUGIN context:** replaying the captured args via our own
   `g_createItemFn` + `tryAddItem` created a real weapon and landed it in the
   leader's inventory (`created=1 added=1`) - no engine-side allocator obstacle.
5. **Weapon mint call sites observed** (module RVAs, for future tracing):
   +0x621917, +0x62D306 (NPC loadout path), +0x582ACD / +0x582B53 (factory
   `process`/`create` range).
6. **`createItemAndAdd` fixed with the recipe fabricates EVERY weapon template:**
   `diagWeaponCreate` rebuilt on the manufacturer-first shape scored **24/24**
   (was 0/24), and the wire-path fabrication (`probeFabricateWeaponLoose` ->
   `createItemAndAdd`) fabricated a real weapon into the leader's inventory.
7. **A fabricated weapon PERSISTS - including worn.** The fabricated weapon was
   auto-routed into a weapon slot by `tryAddItem` (which is why the explicit
   equip leg found nothing loose: `eq=0`) and was still worn 8 s / many engine
   ticks later (`persist loose=0 worn=1`). d25's "fabricated equips are
   discarded within a tick" does NOT hold for a weapon fabricated through the
   engine's own mint shape (in-session; save/reload persistence untested).

## Validation

- (1) `451/raw/host.log`: every `mk6 ... ret=1` line whose result fed a
  `swordCtor` has `gd='...'/51 mesh='...'/2` (e.g. lines quoted above), while all
  armour/item `mk6` lines have `gd='...'/3|4` template-first. Our template-first
  weapon calls in the same log (`[wpndiag] recipe[0..5]`) are all `-> null`.
- (2) Quoted `mk6` lines: `lvl=0 fac=0 hand={... t=11 ...}` on every engine weapon
  mint; `mat='(null)'/-1` on the 912-manufacturer mint and `'925-gamedata.base'/50`
  on the 917 mints. t=11 = NULL_ITEM per `kenshi/Enums.h` (itemType index 11).
- (3) Consecutive log pairs: `mk6 gd='917...'/51 mesh='924...'/2` immediately
  followed by `swordCtor gd='924...' company='917...'/51` with the same material,
  caller `+0x5805F2` (inside createItem6, which starts at RVA 0x57FFD0 per
  `RootObjectFactory.h:39`).
- (4) `[mkspy] replay gd='917-gamedata.base' lvl=0 fac=0 -> created=1 added=1` and
  `SPIKE 451 replay res=1` (host.log line 275-276) - the call ran through
  `probeReplayWeaponMint` (plugin tick context, SEH-guarded, blank hand).
- (5) The `caller=+0x...` fields on the quoted `mk6` lines.
- (6) Run-3 host.log: `[wpndiag] RESULT ok=24/24 (spike-451 manufacturer-first
  recipe)` with a per-template `tmpl[i] ... -> OK` line for each; `SPIKE 451 fab
  loose res=1 sid='52309-rebirth.mod'`.
- (7) Run-3 host.log: `SPIKE 451 fab persist loose=0 worn=1` at t+32s vs the
  fabricate at t+24s (`captureContainerContents` census, equipped=1 row carrying
  the fabricated sid); `fab equip eq=0` at t+27s shows no loose copy remained
  (auto-equipped on add).

## Open questions / hypotheses (UNVALIDATED)

- Does a fabricated weapon survive a SAVE/RELOAD cycle (in-session persistence is
  proven, finding 7)? Test: fabricate, coordinated save, reload, re-census.
- Do crossbows take the same swapped layout (`Crossbow::_CONSTRUCTOR` has a
  different signature - `(baseData, hand, overalllevel)`, no company arg)? The
  24/24 diag walk did not identify which templates were crossbows. Test:
  trace a crossbow-armed NPC spawn with the same hooks.
- What does `levelOverride != 0` do on the swapped layout (grade pinning for
  quality-matched fabrication)? Test: replay with lvl=1..40 and read back
  `Gear::level_0_100`.

## Implications for co-op

- The "engine cannot fabricate weapons" doctrine is DEAD - it was an argument-order
  bug on our side, not an engine limitation. `createItemAndAdd`'s weapon branch can
  be fixed by swapping the first and third GameData args (manufacturer first,
  weapon template as the header's `weaponMesh`), unblocking:
  - container-reconcile CREATE for weapons (shortfall repair),
  - cross-squad transfer fabrication fallback (the `!isGearType` guard),
  - new-weapon acquisition propagation (loot/vendor buys appearing on the peer).
- The inventory snapshot already carries manufacturer/material sids
  (`fillItemProvenance`), so the peer has everything the recipe needs.

## Recommended follow-ups

- Phase 2 of the weapon-fabrication plan: fix `createItemAndAdd` (swap args for
  WEAPON templates; resolve the manufacturer GameData by sid from wire provenance,
  falling back to a null material), re-run `diagWeaponCreate` expecting >0/24, and
  settle the equip-persistence question.
- Keeper: `engine::installCreateItemTraceHook` + `engine::probeReplayWeaponMint`
  (spike-451-only instrumentation, installed exclusively by SpikeScenario id 451).

## Landed (2026-07-10)

All three unblocked sites shipped the same day: weapon CREATE in the container
reconcile, gear in the cross-owner transfer shortfall fallback (existing
latch/`wdSuppress_`/rebase dupe plumbing), and acquisition propagation over the
snapshot channel alone (the W2 PICKUP intent stays a no-op without a tracked
ground copy). `KENSHICOOP_WEAPON_FAB=0` is the escape hatch (gates the weapon
branch of `createItemAndAdd`, so every fabrication site dies at once). Gated by
the new `weapon_loot` scenario (novel-sid acquisition, exactly one copy per
side, matching quality, zero dupes - run 182239) + regressions inv_reequip /
trade_peer / inv_wpnseq / trade_probe / world_weapon_drop. Doctrine 52;
SYNC_GAPS entry 12; VALIDATION_BASELINE addendum 2026-07-10.
