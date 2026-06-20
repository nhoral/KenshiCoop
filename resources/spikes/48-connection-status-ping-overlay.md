# Spike 48 - Connection / status + ping overlay

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Surface live session health to the player: connection state (peer connected?) and
network latency (ping/RTT), rendered on the HUD. Builds on spike 46 (ForgottenGUI
instance) + 47 (ScreenLabel render proof). Two questions: (1) does the transport expose
a usable RTT/connection signal, and (2) can that data be rendered on a HUD overlay.

## Method

- **Data**: added a thread-safe RTT/peer-count sampler to the net layer
  (`NetLink`, the plugin's ENet wrapper). Each net-thread service tick reads ENet's
  smoothed `ENetPeer::roundTripTime` (ms) + counts `CONNECTED` peers and publishes them
  via `rttMs()` / `connectedPeers()`. The main thread copies these into `ScenarioContext`
  (`Plugin.cpp`) so a scenario can read them with no cross-thread hazard.
- **Render**: `engine::hudStatusLabel(anchor, text)` mints a green `ScreenLabel` pinned to
  the leader (the proven-safe spike-47 path) whose caption carries the telemetry;
  `engine::hudFixedLabel(top,left,w,h,text,layer)` calls `ForgottenGUI::createFloatingLabel`
  (RVA 0x740240) at a SCREEN-RELATIVE rect (a fixed-corner panel). The by-value
  `std::string layer` arg is passed as a caller-owned `const std::string*` (MSVC x64 ABI
  passes non-trivially-copyable by-value args by hidden pointer). Both SEH-guarded.
- `SpikeScenario` id `48` logs `conn ... peers rtt` each 1s, mints the status label on the
  first tick (so the ~1-2s screenshot catches it), and the fixed label at t=1.2s.
- Networked host+join on save `c`, 16s, with screenshots.

## Findings

1. **ENet exposes real, usable RTT + connection state.** Both roles logged `peers=1
   rtt=1ms` continuously while connected (1 ms is the expected loopback RTT). When the
   host self-exited at ~t=12s the join immediately read `peers=0 rtt=0ms` - so the signal
   tracks connect AND disconnect live. This is the data source for a ping/connection HUD.
2. **The telemetry renders on a HUD status overlay.** The green tracked label
   **"CONN peers=1 rtt=1ms"** is visible under the leader on the join
   (`48/raw/join_3.png`, `join_5.png`), carrying the live `connectedPeers`/`rttMs` values
   end-to-end (net thread -> ScenarioContext -> caption -> screen).
3. **The data path is thread-safe and cheap.** RTT is sampled on the net thread under no
   new lock (a single `InterlockedExchange` of a `volatile LONG`) and read on the main
   thread - no contention with the 20 Hz transmit loop.
4. **A fixed-corner overlay (`createFloatingLabel`) is callable without crashing.** It
   returned a non-null `MyGUI::Window*` on both roles (host `0xDE1DE580`, join
   `0x5806FA30`) with layer `"Info"`, and both clients ran the full duration and exited
   cleanly - so the screen-relative floating-label primitive (incl. the by-value
   `std::string` ABI handling) works at the call level.

## Validation

- Finding 1: `48/raw/{host,join}.log` - e.g. `SPIKE 48 conn role=J t=1015 peers=1 rtt=1ms`
  ... through `t=11015`, then `role=J t=12015 peers=0 rtt=0ms` (the host exited at
  `07:55:44`, the join saw the drop on its next sample). Host symmetric.
- Finding 2: screenshot `48/raw/join_3.png`/`join_5.png` read back - the green
  "CONN peers=1 rtt=1ms" label is on the leader body. The log line
  `SPIKE 48 status role=J t=0 label=000000000A692EB0 text="CONN peers=1 rtt=1ms"` ties the
  rendered caption to the probe and shows the caption was built from `ctx` telemetry.
- Finding 3: the implementation uses `InterlockedExchange(&rttMs_, ...)` /
  `(u32)rttMs_` (NetLink.cpp/.h); no new critical section; the run showed no stall.
- Finding 4: `SPIKE 48 fixed role=H t=1515 label=00000000DE1DE580` and
  `role=J t=1515 label=000000015806FA30` (non-null), plus both logs end with
  `test duration elapsed; exiting` (clean, no fault).

## Open questions / hypotheses (UNVALIDATED)

- **The fixed-corner overlay's VISIBLE render is not screenshot-confirmed.** It was placed
  at top-left (0.02,0.02), which overlaps Kenshi's tutorial message list, so it could not
  be visually distinguished in the capture. Whether layer `"Info"` is a real Kenshi MyGUI
  layer that paints (vs a non-null-but-invisible widget) is unproven - re-place it in a
  clear region (e.g. top-right) to confirm. (The TRACKED status label, finding 2, is the
  screenshot-proven render path.)
- **Live caption updates**: the status label snapshots telemetry at creation
  (`rtt=1ms`); updating it as RTT changes needs `ScreenLabel::_NV_setCaption` (0x6E3E10),
  not exercised here.
- **RTT under real WAN conditions** (the harness has a `setNetSim` delay/jitter knob) was
  not characterized - loopback RTT is ~1 ms; a delayed run would validate the number
  tracks induced latency.
- **`roundTripTimeVariance`** (jitter) and packet-loss counters are available on the ENet
  peer but were not surfaced.

## Implications for co-op

- **A connection/ping HUD is feasible today.** ENet already measures RTT + connection
  state; a tiny thread-safe sampler exposes it, and the spike-47 ScreenLabel path renders
  it. Wire `hudStatusLabel` (or a fixed panel once placement is confirmed) to show
  per-peer "connected / ping" and flip to a "reconnecting" notice on `peers=0`.
- Combine with spike 36 (message bar) for connect/disconnect notices and spike 47
  (nameplates) for a complete co-op HUD.

## Recommended follow-ups

- Re-run the fixed overlay in a clear screen region (top-right) to screenshot-confirm
  `createFloatingLabel` paints, then standardize on it for a fixed status panel.
- Drive `_NV_setCaption` to update the ping value live instead of recreating the label.
- Run with `setNetSim` delay (e.g. 80 ms) to validate the displayed RTT tracks real latency.
- Surface `roundTripTimeVariance` + ENet packet-loss for a richer connection-quality meter.
- Keeper primitives (reverted): `NetLink::rttMs/connectedPeers`, `ScenarioContext.rttMs/
  connectedPeers`, `engine::hudStatusLabel`, `engine::hudFixedLabel`.
