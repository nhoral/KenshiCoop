# Spike 29 - Player money (Cats): scope + syncability

- Type: STATIC (headers) + DUMP (recipe)
- Status: DONE
- Source: `Platoon.h`, `Inventory.h`, `ShopTrader.h`, `Item.h`

## Goal

Where do a player's Cats live, what is their scope (per-character / squad / faction
/ global), and what would syncing money require?

## Findings - money has THREE layers, no global wallet

1. **Squad/Platoon wallet (primary):** `Platoon::ownerships.money` (int, 0x88) with
   `Ownerships::getMoney()/addMoney(int)/setMoney(int)/takeMoney(int)/
   takeMoneyByForce(int)`. `Ownerships{faction, me(Platoon*)}`. This is the explicit
   scalar wallet, **per-Platoon** (each player squad has its own).
2. **Inventory-aggregated:** `Inventory::getMoney()/takeMoney(int)` - Cats counted
   from money-item stacks in inventory (no `addMoney`; read+subtract only).
3. **Container/vendor:** `ShopTrader`/`ContainerItem` `getMoney()/takeMoney()`
   virtuals (RootObject-level).
4. **No per-Character, no per-Faction scalar, no global player wallet** in the SDK.
   Faction economy is reached indirectly via `Platoon -> Ownerships -> faction`.

So in Kenshi the player's spendable Cats are effectively **per active squad/platoon**
(the squad you have selected), aggregated from its `Ownerships.money` + inventory
money items. There is no single number "the player's money".

## Implications for co-op

- **This is the crux of shared economy.** If both players control squads in the same
  faction, do they share one wallet or have separate squad wallets? The data model
  says SEPARATE per-platoon wallets by default - which is actually convenient for
  co-op (each player spends their own squad's cash) and avoids a contended global.
- Syncing money means replicating `Ownerships.money` per platoon (a small int delta
  per squad) - cheap. But the inventory-money-item layer must be reconciled too (it
  already rides the inventory-sync channel as items, so money items may partially
  sync already as CONTAINER items - worth confirming).
- Recommended policy: **per-squad wallets, host-authoritative**, replicate
  `Ownerships.money` on change. Avoid a shared global wallet (write contention,
  spike 32).

## Recommended follow-ups

- `SpikeScenario` id 29 DUMP: log the player platoon's `ownerships.money` +
  `inventory.getMoney()` on both clients to see what (if anything) already syncs via
  the inventory channel, and whether the two clients' player platoons differ.
- Decide wallet policy (per-squad vs shared) before any purchase sync (spike 31/32).
