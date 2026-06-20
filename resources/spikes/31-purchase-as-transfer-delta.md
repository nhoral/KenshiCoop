# Spike 31 - Purchase modeled as transfer + money delta

- Type: STATIC (design) + code
- Status: DONE
- Source: spikes 28/29 + the existing inventory-sync / world-item conservation model

## Goal

Can a purchase be modeled with primitives we already sync (item transfer + money
delta), so co-op shopping reuses existing machinery?

## Findings

1. **A purchase decomposes cleanly into:**
   - an **item move**: vendor inventory -> player squad inventory
     (`Inventory::buyItem(item, sendingTo)` / `removeItem` + `addItem`), and
   - a **money delta**: `playerPlatoon.ownerships.takeMoney(price)` +
     `vendor.takeMoney(-price)` (or `addMoney` on the receiver).
2. **Both halves map onto existing sync concepts:**
   - The item move is exactly the **inventory-sync / world-item conservation**
     pattern already implemented (items keyed by hand, conserved across a transfer).
   - The money delta is a small per-platoon `int` (spike 29) - a trivial new field
     on the existing squad/ownership replication.
3. **Price is deterministic** (spike 28: value x culture mult x squad mult), so a
   host-authoritative trade computes the price once and replicates the RESULT (item
   in, money out), avoiding any client price disagreement.

## Implications for co-op

- **No new sync paradigm needed.** Shopping = (conserved item transfer) + (money
  delta), both host-authoritative. The host validates affordability
  (`getMaxAffordableNum`), performs `buyItem`, decrements the squad wallet, and
  replicates: the bought item (via the inventory/conservation channel) + the new
  `ownerships.money`.
- This keeps the vendor as authoritative stock owner (host), preventing
  double-spend/duplication (spike 32).

## Recommended follow-ups

- Add a `money` field to the squad/ownership replication; route bought items through
  the existing inventory-conservation channel; gate the transaction host-side.
- Confirm at runtime (spike 30) once a vendor save exists: buy an item, assert the
  item appears on the join's squad and both wallets agree.
