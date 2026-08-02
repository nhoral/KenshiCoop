# Three-player support (host + 2 joins)

Design for lifting the two-player limit to three (N-generic code, policy cap
`MAX_PLAYERS = 3`). Topology stays a star: the host is authoritative for the
world, each player is authoritative for its own squad tab(s), and joins never
talk to each other directly - the host relays.

## Why 2 was the limit (audit summary)

The wire protocol already tags every packet with the sender's `ownerId`, the
host's ENet host already has 8 peer slots, and host-authored state already goes
out via `enet_host_broadcast` (all peers). The receive/apply path routes by
`ownerId` and the interp/dedup/clock maps are already per-owner. What actually
breaks with a third player:

1. **No relay** - join-authored state (squad motion, events, inventory, ...)
   reaches only the host and is never forwarded to the other join
   (`NetLink.cpp` receive ladder pushes to the local game thread only; the
   step-6 guard at the id assignment says exactly this).
2. **Squad-tab ownership is binary** - `resolveOwnRanks()` gives host `{0}`,
   any join `{1}` (`OwnRanks.h`); two joins would both claim tab 1.
3. **Single-slot peer state** - speed votes (`speedPeerReq_`), camera hint
   (`peerCam_`), per-row `seqSeen` on symmetric channels (faction/door/build/
   prod/research), the `pinPeer_` "peer authored" pin, presence
   (`g_peerPresent` bool), save acks (`lastAckXferId`), and the leave handler
   (`clearPeerReplicationState` wipes ALL peer state on any leave).
4. **Steam tunnel is single-peer** - `SteamP2P` binds one `g_peer` SteamID and
   drops packets from anyone else; config/panel hold one `steamPeer`.
5. **Connect-push save is broadcast** - a mid-session late joiner's save
   stream would also hit the already-playing join.

## Design

### Wire (PROTOCOL_VERSION 48 -> 49)

- `HelloPacket` gains `u32 ownRank`: the squad-tab rank this join claims
  (default 1; player 3 uses 2). The host **rejects** a join whose rank is
  already occupied (like a protocol mismatch: loud, deterministic) and rejects
  joins beyond `MAX_PLAYERS`. This replaces the step-6 "expect desync" guard.
- New `PKT_PEER_STATUS` (host -> joins, reliable): `{present, ownerId,
  ownRank}`. Sent to everyone when a peer arrives/leaves, and a full roster is
  sent to a newly welcomed join. Joins use it to:
  - maintain a presence set (replaces the `g_peerPresent` bool),
  - clear that owner's replication state + epoch entries on leave/rejoin,
  - re-burst their own reliable state when a new peer arrives (same re-queue
    path as the host's `onPeerConnected`), so a late joiner receives the
    other join's current state via relay.
- Save/load packets are unchanged on the wire; targeting is a send-side
  concern (below).

### NetLink (host)

- **Peer registry**: `std::map<u32, ENetPeer*> peersById_` (net thread only),
  filled at HELLO-accept, erased on disconnect. Rank occupancy map alongside.
- **Relay**: in the receive ladder, after local delivery, if `isHost_` and the
  packet type is in the relay set, re-send the same bytes to every connected
  peer except the sender, preserving channel (`ev.channelID`) and reliability
  flags (`ev.packet->flags`). Per-sender ordering is preserved because relays
  are enqueued in receive order on the same channel.
- **Relay set** (join-authored, owner-tagged, receiver-side dedup exists):
  entity batches, events, inventory snapshots, medical, treatment, stats,
  money, combat hits, inv-xfer, world drops/pickups/items as their flows
  allow (finalized per channel; host-directed intents - HELLO, TIME_*,
  SPEED_REQ, CAM_HINT, SPAWN_REQ, SAVE_*, LOAD_* - are never relayed).
- **Targeted sends**: save/load queue entries gain a `targetId`
  (`OWNER_ID_ALL` = broadcast). The net thread routes: ALL -> broadcast,
  otherwise `enet_peer_send(peersById_[targetId])`. The connect-push save
  targets only the connecting join; mid-session coordinated saves broadcast.
- **Epochs per owner**: clear `epochSeen_[owner]` on that owner's
  join/leave edges instead of blanket-clearing (joins still clear everything
  when their link to the host cycles).

### Ownership

`resolveOwnRanks()` keeps its role default (host `{0}`, join `{1}`) but a new
config key `ownRank` (and a panel control) lets the third player claim `{2}`.
The claim rides `HelloPacket.ownRank` so the host can enforce uniqueness at
admit time. Ownership stays decoupled from `playerId` (which remains a wire
tag), so reconnects behave exactly as today.

### Replicator (per-owner state)

- Speed: `speedPeerReq_/speedPeerCombat_/speedSeqSeen_` -> map keyed by
  ownerId; effective speed = min over host + all votes; combat cap ORs.
- Camera: `peerCam_` -> map; host interest = union of anchors.
- Symmetric change-gated rows (faction/door/build-door/prod/research/build):
  `seqSeen` becomes per (sender, row) so two senders' independent counters
  can't spuriously drop each other's rows.
- `pinPeer_` records WHICH owner authored the hand; driven records keep their
  owner. `clearPeerReplicationState(ownerId)` clears only that owner's
  bodies/pins/proxies; `OWNER_ID_ALL` keeps the full-sweep behavior for
  going offline.
- Save acks: per-peer (xferId, ownerId); the coordinated save completes when
  every present join has acked (stragglers logged).

### Steam transport

- `SteamP2P` grows a peer registry: SteamID <-> fabricated ENet address
  (`1.0.0.N:port`). Send routes by fake address; receive tags the sender's
  fake address and accepts sessions from any registered ID; unknown senders
  are still dropped. Joins keep a single peer (the host).
- Config: `steamPeers` list (comma string or array; `steamPeer` stays as an
  alias). Panel: the host can paste two friend IDs (list + clear); joins are
  unchanged.
- Steam invite/lobby: capacity raised to `MAX_PLAYERS`; the host registers
  each arriving member instead of firing once.

### Out of scope for the first cut

- A "Multiplayer (Wanderer x3)" game start (players can split a third squad
  tab in-game, same as the documented 2-player fallback).
- Interest tailoring per join (world state stays one broadcast snapshot).
- Test-harness N-client generalization beyond a third local install +
  a 3-player smoke scenario.

## Rollout order

1. Wire v49 + NetLink (registry, admit rules, relay, PEER_STATUS, targeted
   saves, per-owner epochs). **DONE**
2. Plugin/Replicator per-owner state + per-owner leave. **DONE**
3. SteamP2P registry + config/panel. **DONE**
4. Build (VC10 x64), 2-player regression baseline, then 3-player smoke
   (third local install). **DONE** - 2026-08-02: prototest 464/464;
   coop_presence (2P) full PASS; scripts/run_3p_smoke.ps1 (3 live instances,
   loopback UDP) PASS on all gates - both joins admitted with their slot
   claims, roster propagated join<->join, per-owner speed votes reduced on
   the host, zero errors in any log.
5. 3-squad fixture + full-oracle positional regression over three logs.
   **DONE** - 2026-08-02: fixture 'squad3' (ranks 0/1/2 one member each,
   baked via `bake_scene.ps1 -Setup squad3`); coop_presence generalized to
   ownership rank (ScenarioContext.ownRank; RECV logged for every non-own
   rank) with all-participants arming (KENSHICOOP_ARM_MIN_PEERS=2);
   `run_3p_smoke.ps1 -Scenario coop_presence` runs the 2P oracle battery
   per host<->join pair (gate names @join1/@join2) PLUS the relayed
   join<->join cross-check (Test-CoopPresence3p, the run's primary gate)
   via Invoke-RunAnalysis3p, writing verdict.json. Loopback: PASS - all six
   directed pairs tracked (relayed pairs worstMedian 0.1-0.2 u); WAN 'bad'
   (one netsim per join, 120 ms +/-40 ms 5% loss each link): PASS, relayed
   pairs worstMedian 0.1/0.4 u; 2P coop_presence regression stays PASS on
   the same build. The fixture's members are named by owning slot ('Host',
   'Remote Player 1', 'Remote Player 2') and the runner spreads the three
   windows across the widest monitor.

## Still open (beyond the smoke)

- Steam-transport 3-player needs a live 3-machine session (loopback cannot
  exercise Steam P2P).
- A "Multiplayer (Wanderer x3)" game start.

## Player-3 setup (UDP or Steam)

- **Host**: nothing extra on UDP. On Steam, paste BOTH friends' Steam IDs in
  the F2 panel (paste twice), or set `"steamPeers": "111...,222..."` in
  `coop_config.json`.
- **Second player**: exactly as before (squad slot 1 is the default).
- **Third player**: set **Squad slot: 2** in the F2 panel (JOIN role shows
  the toggle), or `"ownRank": 2` in `coop_config.json`. The host rejects a
  join whose slot is already taken, so a mixed-up pair fails loudly instead
  of double-claiming a squad.
- The shared save needs a third squad tab for the third player (split units
  into a third tab in-game, same as the documented 2-player fallback when a
  save has one squad).
