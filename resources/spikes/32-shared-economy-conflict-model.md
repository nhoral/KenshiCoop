# Spike 32 - Shared-economy conflict model

- Type: STATIC (design) + code
- Status: DONE
- Source: spikes 29/31 + the conservation/host-authority model

## Goal

What conflicts arise when two players share an economy, and how do we avoid
double-spend / item duplication?

## Findings - conflict surfaces

1. **Wallet contention:** if players SHARE one wallet, simultaneous purchases can
   over-draw (both see Cats X, both spend ~X). The SDK's NATURAL model is
   **per-platoon wallets** (spike 29), which sidesteps this: each player spends their
   own squad's `ownerships.money`, no shared counter to contend.
2. **Vendor stock contention:** two players buying the SAME vendor item could each
   receive a copy (duplication) if both clients mutate stock locally. Must be
   **host-authoritative**: the vendor's `inventory` lives on the host, `buyItem` is
   validated + applied host-side, result replicated. The existing world-item
   **conservation** model (an item is created on one side only and conserved on
   transfer) is exactly the anti-duplication tool.
3. **Money item duplication:** Cats-as-items (spike 29 layer B) must ride the
   conserved inventory channel, not be independently minted per client.

## Recommended policy

- **Per-squad wallets** (default, matches SDK) - no shared global counter.
- **Host-authoritative vendors + trades**: host owns stock + price + applies the
  transaction; join sends a "buy intent", host validates affordability/stock and
  replicates the conserved item + the wallet delta.
- **Conservation everywhere**: bought items and Cats move via the existing conserved
  transfer channel so nothing is duplicated.

## Implications for co-op

- Shared economy is tractable WITHOUT distributed-locking complexity, precisely
  because (a) Kenshi already scopes money per-squad and (b) we already have a
  conservation model for item transfer. The whole feature reduces to "route trades
  through the host + add a money delta".

## Recommended follow-ups

- Specify a `BUY_INTENT` (vendor hand + item hand + qty) join->host message; host
  validates + applies + replicates. Reuse inventory conservation for the item; add
  the `ownerships.money` delta. Test the double-buy race once a vendor save exists.
