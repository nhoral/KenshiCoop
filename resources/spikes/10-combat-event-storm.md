# Spike 10 - Combat event storm (reliable PKT_EVENT channel)

- Type: RUN
- Status: DONE
- Save: battle40 (baked 20v20 from spike 9)
- Branch commit: <filled at commit>

## Goal

KO/death/revive transitions ride the RELIABLE channel (`PKT_EVENT`: EVT_KNOCKOUT /
EVT_DEATH / EVT_REVIVE) so a combat outcome is never lost. How many events/sec can
that channel sustain before it "backs up" - i.e. starts losing, reordering, or
badly delaying events? This bounds how violent a battle the event layer can carry.

## Method

The production `Replicator` already detects bodyState edges on streamed NPCs and
emits `[event] SEND id=.. ev=..`; the join logs `[event] RECV id=..` (eventId is a
monotonic per-sender counter). So no new instrumentation is needed - only a way to
DRIVE a high event rate.

Real combat is too sparse to stress it (see baseline below), so spike-scenario
case 10 makes the HOST FLOOD edges: every ~500ms it `knockDown(on)`/`knockDown(off)`
toggles every baked body in the interest sphere (~48 bodies on battle40), generating
KO + REVIVE edges in bursts. Both clients run on the same machine (shared wall
clock), so matching `[event] SEND`/`RECV` by id yields true send->recv latency.

Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 10 -Save battle40 -Seconds 45
python resources/spikes/10/analyze.py resources/spikes/10/raw/host.log resources/spikes/10/raw/join.log
```

## Raw evidence

Baseline (natural combat, from spike 9 battle40 measure run, no flood): host emitted
**4** `[event] SEND` (all EVT_KNOCKOUT), join logged **4** `[event] RECV`, ids 1-4 -
~0.09 events/s, 100% delivered.

Flood (this run, battle40, 59 toggle waves of ~48 bodies):

```
SEND=2798 RECV=2798 max_send_id=2798 max_recv_id=2798
contiguous send ids 1..max: True
lost (sent, never recv) = 0  spurious (recv, never sent) = 0
send span=33.1s  mean rate=84.4 events/s   peak 1s-bucket send rate = 103 events/s
latency ms over 2798 matched: min=1 med=34 p95=753 max=2262 mean=131.4
event types sent: 1423 ev=1 (KNOCKOUT) + 1375 ev=3 (REVIVE)
```

## Findings

1. **The reliable channel is loss-free and order-preserving well past realistic
   combat rates.** Flooded at a mean 84.4 events/s (peak 103/s) for 33s, all 2798
   sent events arrived: RECV count == SEND count, recv ids are exactly the contiguous
   set 1..2798, with 0 lost and 0 spurious/duplicate. No saturation/loss point was
   reached - a reliable ENet channel does not drop, it queues.

2. **"Backing up" manifests as a LATENCY TAIL, not loss.** Under the sustained flood,
   send->recv latency was median 34ms but heavy-tailed: p95 753ms, max 2262ms (mean
   131ms). Bursts of ~48 edges emitted in one host capture tick queue up and drain
   over subsequent frame-gated ticks on both ends, so the tail grows with the burst
   rate while delivery stays complete.

3. **Realistic combat is ~1000x below this rate.** A genuine 20v20 melee produced
   ~0.09 events/s (4 events in 45s). The event layer therefore has enormous headroom
   for battle KO/death traffic; the practical risk is not battle volume but
   pathological misuse (e.g. emitting per-frame state as events).

## Validation

- Finding 1: `analyze.py` over the committed `10/raw/{host,join}.log`:
  `SEND=2798 RECV=2798`, `contiguous send ids 1..max: True`,
  `lost=0 spurious=0`. These are exact counts/set-equality over the parsed
  `[event] SEND id=N` / `[event] RECV id=N` lines; re-run to reproduce.
- Finding 2: the `latency ms ... med=34 p95=753 max=2262` line is computed by
  matching each eventId's SEND and RECV `[HH:MM:SS.mmm]` timestamps (same machine
  clock) and taking recv-send. The flood rate (84.4/s mean, 103/s peak) is from the
  SEND timestamp span and per-1s buckets.
- Finding 3 baseline: spike 9's `b40` logs contain exactly 4 `[event] SEND` /
  4 `[event] RECV` over a 45s combat run (`grep -c "\[event\] SEND"`), all ev=1.

## Open questions / hypotheses (UNVALIDATED)

- **This is loopback (both clients on one machine): no real RTT, no packet loss.**
  The latency tail here is frame-cadence queueing, not wire congestion. Real-network
  behaviour (ENet retransmit under loss/jitter, true RTT) is untested - needs a
  `-NetSimLossPct`/`-NetSimDelayMs` flood run (backlog 296-298) to find whether loss
  forces unbounded latency growth.
- **The flood is artificial** (KO/REVIVE toggling), so it stresses event THROUGHPUT
  but not the EVT_DEATH path specifically (death is one-shot per body, latched). A
  mass-simultaneous-death burst (e.g. kill all 48 at once) would test a single large
  spike rather than sustained rate - not yet measured.
- **Whether the latency tail harms gameplay** (a 2s-late KO render) depends on the
  feature; not evaluated here.

## Implications for co-op

- KO/death events for battles are safe on the reliable channel: realistic rates are
  ~0.1/s and even a 100/s flood is delivered intact. No need for an event-rate cap or
  a separate bulk channel for combat outcomes.
- Do keep events for genuine one-shot TRANSITIONS only (the existing doctrine): the
  latency tail shows that abusing the reliable channel for high-frequency continuous
  data would stack delay. Continuous posture stays on the unreliable EntityState
  batch (as it already does).
- The next real question is loss/jitter resilience, not throughput - prioritise the
  NetSim sweeps for the event channel.

## Recommended follow-ups

- Re-run the flood under `-NetSimLossPct 5 -NetSimDelayMs 80` to measure reliable
  retransmit latency under real loss (resolves open question 1; feeds backlog 296+).
- Add a mass-simultaneous-death burst variant (kill N bodies in one tick) to test a
  single large spike vs sustained rate.
- Probe code (scenario case 10 flood) reverted; it reused the existing
  `engine::knockDown` + event channel, so re-arming it later is a one-case add.
