# Spike 30 - Vendor proximity probe at 'c'

- Type: RUN (recipe) + code
- Status: PARTIAL
- Save: c

## Goal

Is there a vendor near the `c` save so we can exercise a real trade, and how do we
find one at runtime?

## Findings

1. **How to find a vendor:** `ShopTrader` objects are `RootObject`s; enumerate world
   objects near the leader via the same `getObjectsWithinSphere` /
   `getCharactersWithinSphere` path used by `captureNpcs`, filtering for
   `getDataType()==SHOP_TRADER_CLASS` (or characters whose faction owns a shop).
   `ShopTrader::getCurrentTownLocation()` confirms a town context.
2. **`c` is a low-density area** (spikes 1/8: only ~3-7 world NPCs within 200u; the
   join saw ~9-15). Whether a TRADER is among them is unknown without a probe - `c`
   may not be in a town with shops. Per the original setup decision, shop-specific
   spikes degrade to API discovery if `c` has no vendor.

## Why PARTIAL

The vendor-finding method is clear, but `c`'s suitability is unconfirmed and likely
poor (not a market location). A proper trade run needs a save parked at a shop
counter.

## Recommended follow-ups

- Create/identify a save positioned at a vendor (e.g. a Hub/Squin shop), or bake one
  (spike 5: spawn a `ShopTrader`-class character + vendor list near the leader).
- `SpikeScenario` id 30: enumerate nearby `SHOP_TRADER_CLASS` objects, log
  `getMoney()` + stock count; if found, drive `Inventory::buyItem` and observe the
  money/inventory deltas (spike 31).
