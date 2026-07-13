# Manual town-repro checklist: cross-client trading + ground drops

> Companion to the `coop-trading-hardening` plan. The automated scenarios
> (`xfer_block`, the inverted `trade_probe`/`trade_peer`, and the non-gear case in
> `ScenarioWorldItems`) run on TEST saves where the engine's spatial item query
> tends to succeed, so they cannot reproduce the town failure. This checklist is
> the manual counterpart: it exercises the block + the query-free drop path in an
> actual TOWN, where `getObjectsWithinSphere` is known to return 0 (see
> `WORLD_ITEM_SYNC_HANDOFF.md`, "the single most important hard-won fact").

## Why manual

- Drop DISCOVERY is now query-free (detour on `Inventory::_NV_dropItem` capturing
  the exact `Item*` + world position) and culling is handle-based (the tracked
  real `Item*` is gone), so a town's failing spatial scan should no longer cause
  mis-place / flicker / never-appear. Only a live town proves it.
- The trade veto is a purely LOCAL refusal on the dragging client; there is no
  packet, so the only evidence is the `[xfer] BLOCK` log + the item staying put.

## Setup (both clients)

1. Build + deploy the plugin (`scripts/build_plugin.cmd` then `scripts/deploy.cmd`).
2. Enable the veto + verbose inventory/world tracing. In each client's launch env:

```
set KENSHICOOP_BLOCK_XFER=1
```

```
set KENSHICOOP_INV_DUMP=1
```

3. Start a shared-save co-op session (host + one join), inhabit one squad each,
   and walk BOTH squads into the SAME town (e.g. a bar interior or a busy market
   tile - the denser the props, the more reliably the spatial query fails).
4. Tail both logs (`KenshiCoop_host.log` / `KenshiCoop_join.log`) filtered to the
   tags below.

Escape hatch: set `KENSHICOOP_BLOCK_XFER=0` on both to restore the old
Protocol 37 replicate-the-trade behaviour (for A/B comparison only).

## Log tags to watch

| Tag | Meaning |
| --- | --- |
| `[xfer] DRAG src=.. dst=.. block=..` | a squad-relevant drag was seen (class 1=own, 2=peer) |
| `[xfer] BLOCK cross-owner drag ...`  | the veto REFUSED a cross-owner drag (expected) |
| `[wi] DROP-CAP netId=.. pos=..`      | query-free capture of a dropped NON-gear item |
| `[wi] SEND` / `[wi] SPAWN` / `[wi] MOVE` / `[wi] CULL` | non-gear proxy stream on host / join |
| `[wd] DROP` / `[wd] APPLY` / `[wd] PICKUP` / `[wd] PICKUP-APPLY` | GEAR conservation drop/pickup |

## Test 1 - Direct cross-owner trade is BLOCKED

1. Open the trade / inventory UI between YOUR squad member and the PEER's squad
   member (a character owned by the other client).
2. Drag any item (common stack, weapon, or armour) from one bag into the other.
3. Expected on the dragging client:
   - the item does NOT move - it stays in the source bag (nothing lands in the
     destination, nothing is left on the cursor).
   - log shows `[xfer] BLOCK cross-owner drag sid='...' src=.. dst=..`.
   - NO `[xfer] SEND` / `PKT_INV_XFER` is emitted (Protocol 37 is retired).
4. Expected on the peer: no change to either inventory.
5. FAIL signals: item crosses; item vanishes/dupes; item stranded on cursor; a
   `[xfer] SEND` appears.

## Test 2 - Same-owner move still WORKS

1. Drag an item between two members of YOUR OWN squad (same client owner).
2. Expected: the move succeeds normally; `[xfer] DRAG ... block=0` may log, but
   NO `[xfer] BLOCK`. Repeat with a world container / chest and a vendor
   `buyItem` purchase - all must remain functional (they are out of veto scope).

## Test 3 - GEAR drop + appear + pickup (conservation)

1. Standing in the town, drop a WEAPON and an armour/clothing piece from your
   squad member to the ground.
2. Expected: `[wd] DROP` on the dropper, `[wd] APPLY moved=1` on the peer; the
   gear appears on the ground for BOTH players at (approximately) the same spot.
   (A slight positional drift is accepted - see the handoff doc.)
3. Have the PEER walk over and pick BOTH up.
4. Expected: `[wd] PICKUP` + `[wd] PICKUP-APPLY moved=1`; the items are carried on
   both clients, nothing orphaned on the ground.
5. Repeat with the roles reversed (peer drops, you pick up) to confirm bidirectionality.

## Test 4 - NON-gear drop + appear (query-free path, the town fix)

1. Drop a NON-gear stack (food, raw materials, a trade good) from your squad in
   the town.
2. Expected: `[wi] DROP-CAP netId=.. pos=..` fires at the drop frame (this is the
   query-free capture - it must appear EVEN IF a `captureWorldItems` spatial scan
   would return 0 here), followed by `[wi] SEND` on the host and `[wi] SPAWN
   ok=1` on the join. The item appears on the ground on the peer at the captured
   position.
3. Leave it a few seconds: it must NOT flicker or disappear (handle-based culling
   keeps it while the real `Item*` is still a free ground item - no `[wi] CULL`
   until it is actually picked up or destroyed).
4. Pick it back up on the OWNER's client.
5. Expected: `[wi] CULL netId=.. (gone/picked-up)` on the host and `[wi] CULL` on
   the join; the proxy is removed on the peer.
6. FAIL signals: no `[wi] DROP-CAP` at drop; item never spawns on the peer; item
   spawns at a wrong/origin position; item flickers or is culled while still
   lying on the ground.

## What to capture

- Both `KenshiCoop_host.log` and `KenshiCoop_join.log` for the session (drop them
  under `tools/manual-sessions/<date>_town/`).
- A note per test: PASS/FAIL + the relevant `[xfer]`/`[wi]`/`[wd]` lines, plus a
  screenshot of the ground item on the peer if positions look off.

## Known follow-ups (out of scope, expected to still be rough)

- Gear conservation `moved=0` when the peer lacks a shared-save copy (no ground
  fabrication fallback yet).
- Stale inventory reconcile (~1.8s settle) can still briefly duplicate/destroy a
  just-dropped item in edge cases.
- Non-gear proxies have no cross-pickup handshake: a receiver cannot yet take a
  dropped NON-gear proxy INTO their own bag (Test 4 pickup is by the OWNER).
