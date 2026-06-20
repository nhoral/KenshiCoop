# Spike 12 - Battle bandwidth profile

- Type: RUN
- Status: DONE
- Save: c, battle10, battle20, battle40 (baked in Spike 9)
- Branch commit: <filled at commit>

## Goal

Quantify the network cost of the entity stream and how it scales with the number of
streamed bodies, so we know the real bandwidth budget for a large battle and where
the practical ceiling is (and whether delta-compression is worth building).

## Method

Added a net-thread bandwidth meter (NetLink.cpp) that accumulates the unreliable
entity-stream datagrams over a 1s window and logs a `[bw]` line: packets/s,
application bytes/s, approximate wire bytes/s (+32 B/datagram for UDP/IP+ENet),
batches/s, entities/s, entities/batch. Ran `KENSHICOOP_SPIKE=12` (idle smoke; the
meter is independent of scenario logic) on four saves of increasing body count and
read the steady-state `[bw] role=H` (host) and `role=J` (join) lines. Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 12 -Save battle40 -Seconds 25
python resources/spikes/12/analyze.py
```

Reliable-channel traffic (events/inv/world) is event-driven and characterized in
Spike 10; this spike measures only the steady-state unreliable stream. Meter code is
reverted; raw logs + analyzer kept in `12/`.

## Raw evidence

Per-save aggregate (host), from `12/analyze.py` over `12/raw/*-host.log`:

```
save       ents/tick  pkts/s  appKB/s wireKB/s  B/ent/tick
c                2.9    11.2     4.60     4.96        80.2
battle10         7.7    11.6    12.20    12.58        79.4
battle20        10.5    17.2    16.64    17.19        79.5
battle40        18.9    27.7    30.07    30.95        79.4
join (own squad): 0.6 ents/tick, 0.98 KB/s app, 1.35 KB/s wire (flat across saves)
```

Steady-state host tail at battle40 (~22 bodies in interest, split into 2 datagrams):

```
[bw] role=H entsz=79 pkts/s=40.0 appB/s=35079 wireB/s~=36359 batches/s=40.0 ents/s=441 ents/batch=11.0
```

## Findings

1. **Each streamed body costs a fixed 79 bytes of application payload per network
   tick.** `entsz=79` and measured B/entity/tick is 79.4-80.2 across every save -
   the cost is purely count-driven, identical whether bodies are idle or fighting.
2. **The stream is a full snapshot at 20 Hz with no delta compression.** Every
   tick re-sends all streamed entities, so the per-entity rate is a flat
   `79 B x 20 Hz = 1.58 KB/s per streamed body per peer`.
3. **Host outbound bandwidth scales linearly with the number of streamed bodies.**
   Measured host app rate: c ~4.6, battle10 ~12.2, battle20 ~16.6, battle40 ~30 KB/s,
   tracking ents/tick (2.9 / 7.7 / 10.5 / 18.9) at the constant 1.58 KB/s/body slope.
4. **Datagrams chunk at 18 bodies each (`ENTITY_BATCH_MAX`).** Below ~18 streamed
   bodies it is one packet/tick (~20 pkts/s); battle40's ~22 bodies split into two
   datagrams/tick (~40 pkts/s), adding ~32 B UDP/IP+ENet overhead per extra datagram.
5. **The join uploads almost nothing and is flat across saves.** Join streams only
   its OWN squad (~1 body) at ~1 KB/s regardless of battle size - all battle cost is
   on the host's outbound link.
6. **The per-tick interest cap (MAX_PUBLISH=160, Spike 14) bounds host upload at
   ~253 KB/s app (~2 Mbps) per peer in the worst case** - large but within ordinary
   broadband upload; the cost multiplies by the number of connected peers (broadcast).

## Validation

- Findings 1-2: `12/raw/*-host.log` `[bw]` lines report `entsz=79`; the analyzer's
  `B/ent/tick` column is 79.4-80.2 for all four saves (independent body counts), and
  `batches/s` is ~20 (=1000/TICK_MS) when ents<=18, confirming a full 20 Hz snapshot.
  `TICK_MS=50` at NetLink.cpp:13.
- Finding 3: analyzer table - app KB/s rises monotonically with ents/tick at a
  constant slope (4.6/12.2/16.6/30 KB/s vs 2.9/7.7/10.5/18.9 ents/tick); ratio = entsz.
- Finding 4: `ENTITY_BATCH_MAX=18` at Wire.h:191; chunking loop NetLink.cpp:581-583.
  battle40 tail shows `pkts/s=40 batches/s=40` (2 datagrams/tick) vs ~20 for the
  smaller saves where all bodies fit one datagram.
- Finding 5: `[bw] role=J` lines in every `*-join.log` show ~0.6-1.0 ents/tick and
  ~1 KB/s app, unchanged from `c` to `battle40` (join leaves `streamNpcs_` off; only
  its own squad is published - Replicator.cpp:154-157).
- Finding 6: projection in `12/analyze.py` using the measured 1.58 KB/s/entity and
  the MAX_PUBLISH=160 cap (Spike 14): 160 entities -> ~253 KB/s app / ~259 KB/s wire
  per peer. (Per-peer; host broadcast multiplies by peer count - enet_host_broadcast,
  NetLink.cpp:585.)

## Open questions / hypotheses (UNVALIDATED)

- **Inbound/ENet ACK + retransmit overhead is not measured** - the meter counts only
  datagrams we create on the send path. Real wire cost includes ENet's protocol
  overhead and (on the reliable channel) ACKs; the +32 B/packet estimate is nominal.
- **Multi-peer host scaling is projected, not measured** (only 1 join was connected).
  Broadcast cost should be ~N_peers x the single-peer rate; needs a 3+ client run.
- **Delta/quantization savings are unquantified.** Many EntityState fields (hand id,
  task) rarely change tick-to-tick; a delta or field-dirty scheme could cut the 79 B
  substantially, but the achievable ratio was not measured here.

## Implications for co-op

- Bandwidth is not the battle bottleneck at current scales: a 40-body battle is only
  ~30 KB/s (~0.25 Mbps) on the host's uplink to one peer. Even the hard interest cap
  (160 bodies) is ~2 Mbps/peer - fine for a 2-4 player co-op on broadband.
- The cost model is simple and predictable: budget `1.58 KB/s x streamed_bodies x
  peers`. This makes interest-radius / cap tuning a direct bandwidth lever.
- Delta compression is a real but not urgent optimization: it would matter most for
  many-peer hosting or raising the interest cap, not for typical small co-op battles.

## Recommended follow-ups

- Re-run with 3+ connected clients to validate the per-peer broadcast multiplier.
- Prototype field-dirty/delta encoding of EntityState and measure the byte reduction
  on a steady battle (target: skip unchanged hand/task fields).
- Keeper instrumentation (reverted): the net-thread `[bw]` meter - cheap to re-add
  behind a config flag for any future bandwidth/scaling spike.
