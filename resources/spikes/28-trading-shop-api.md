# Spike 28 - Trading / shop API surface

- Type: STATIC (headers)
- Status: DONE
- Source: `ShopTrader.h`, `ShopTraderInventory.h`, `TradeCulture.h`, `Inventory.h`, `Item.h`

## Goal

Map the trading API so we can reason about syncing purchases in co-op.

## Findings

1. **Vendor = `ShopTrader`** wrapping a `Character`: `getTrader()`, `getInventory()`
   (the `inventory` holds stock), `getMoney()`/`takeMoney(int)`, `updateInventory()`
   (private; refreshes stock). Stock container is `ShopTraderInventory` (sections
   via `ShopTraderInventorySection`), seeded by
   `Inventory::fillFromVendorList(GameData* vendorData, Faction*)`.
2. **Execute a trade:** the only explicit mutation is
   `Item* Inventory::buyItem(Item* itemToBuy, RootObject* sendingTo)`. **There is no
   `sellItem`** in these headers - selling is presumably `buyItem` in the reverse
   direction (player inventory as the "shop") or implemented in code outside the SDK.
3. **Price:** `Item::getValueSingle(bool isPlayer)`, `getValueAll(isPlayer)`,
   `getAvgPrice()`, `getMaxAffordableNum(cashLimit, isPlayer)`, `merchantPriceMod()`;
   multiplied by `TradeCulture::getTradePriceMultiplier(GameData*)` (each Faction has
   `tradeCulture`) and `Platoon::priceMultWhenITrade`. `TradeCulture::isItemIllegal`,
   `Platoon::iBuyStolenGoods/iBuyIllegalGoods` gate legality.
4. **Item identity for sync:** `InventoryItemBase` has `itemType objectType`,
   `int quantity`, `manufacturerData/materialData/coloriseData` (GameData*),
   `getItemType()`. The template (`GameData* baseData`) is the stable id.

## Implications for co-op

- A purchase is fundamentally an **item transfer + a money delta** (spike 31). Both
  endpoints (vendor stock, player squad wallet) are mutated locally; neither is on
  the wire today, so a purchase by one player is invisible to the other.
- Price is deterministic given the same item + faction culture + squad mult, so two
  clients computing a price independently would agree IF they share the same world
  state - which argues for host-authoritative trades (host computes + applies, then
  replicates the resulting inventory + money delta).

## Recommended follow-ups

- See spike 30 (find a vendor at runtime), 31 (model purchase as transfer+delta),
  32 (shared-economy conflicts). Confirm the sell path in code (grep `buyItem`
  call sites) since the SDK has no `sellItem`.
