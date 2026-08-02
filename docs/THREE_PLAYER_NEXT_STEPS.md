# Three-player: next steps

Where the port stands after 2026-08-02 (second pass): the protocol-49 build
is live, the 2-player regression (coop_presence) passes unchanged, and the
3-player suite now proves the *visible* third player - the relayed
join<->join positional edge - under the SAME oracle battery the 2P
regression runs. Remaining work is live-Steam validation and quality-of-life
(sections 2-3).

## 1. Three-squad fixture + join<->join positional cross-check - DONE

Landed 2026-08-02:

- **Fixture `squad3`** (tracked in `fixtures/saves/`): squad1 grown to THREE
  squad tabs, one member per rank 0/1/2. Rebake any time with
  `scripts\bake_scene.ps1 -Setup squad3 -BaseSave squad1 -BakeSave squad3
  -Promote` (new `setupSquad3Scene` separates a non-leader member of a
  multi-member tab into its own platoon and dumps the container partition;
  idempotent on a 3-tab save).
- **Scenario**: `coop_presence` is now rank-generic. `ScenarioContext.ownRank`
  carries the resolved ownership rank (join 2's slot claim resolves to 2 -
  scenarios no longer hardcode `isHost ? 0 : 1`), MEMBER logs the own rank,
  RECV logs EVERY other rank (so join 1 logs a series for join 2's member,
  fed by the host relay). New `KENSHICOOP_ARM_MIN_PEERS` (Inbound counts
  distinct entity-batch owners) arms all three scenario clocks only when the
  LAST participant streams, so the three windows overlap; 2P behavior is
  unchanged (default 1).
- **Harness**: `scripts\run_3p_smoke.ps1 -Scenario coop_presence` - the smoke
  grew a scenario mode (save `squad3`, 150 s, rank claim 2 on join 2). Add
  `-Wan <profile>` for impaired links: one netsim relay PER JOIN
  (ports Port+1/Port+2), so relayed traffic really crosses two lossy links.
- **Oracle**: `Invoke-RunAnalysis3p` (CoopOracles.psm1) = the full 2P verdict
  battery (health, CHECK FAIL, SCENARIO RESULT, and the manifest's
  gating/advisory list - churn, snap-rate, march, clock-sync, presence...)
  run once per host<->join pair (gates suffixed `@join1`/`@join2`, pair
  primary enforced per pair) PLUS `Test-CoopPresence3p`, the run's PRIMARY
  gate: all six directed MEMBER->RECV pairs, with the join1<->join2 relayed
  pairs the new mechanism. Writes `verdict.json` beside the three logs.
  snap_rate is anchored 15 s past the SCENARIO arm in 3P (the serialized
  boot + the last participant's arrival settle land world-NPC load-covers
  before that; steady state still gates).
- **Results (loopback)**: PASS - all six pairs tracked, relayed pairs
  worstMedian 0.1-0.2 u; every per-pair gate green (snap_rate SKIPs on the
  short window, same as the 2P run of this scenario); 2P coop_presence
  regression PASS on the same build.
- **Results (`-Wan bad`)**: PASS - one netsim per join (120 ms +/-40 ms, 5%
  loss on EACH link), all six pairs green; the relayed pairs still tracked at
  worstMedian 0.1/0.4 u (judged at relayTol 12: two impaired links).
- **Cosmetics**: the squad3 members are NAMED by owning slot - 'Host',
  'Remote Player 1', 'Remote Player 2' (setupSquad3Scene renames via the
  engine's Character::setName, so the names are baked into the fixture and
  every screen shows whose body is whose), and the runner spreads the three
  windows host | join1 | join2 across the widest monitor
  (arrange_windows.ps1 -Join2Pid), re-pinned through the load screens.

## 2. Live Steam-transport 3-player session

Loopback cannot exercise Steam P2P (one machine = one Steam account), so
this needs three machines with three accounts.

- Host either pastes BOTH friends' Steam IDs in the F2 panel (paste twice -
  the panel shows the list) or sets `"steamPeers": "111...,222..."` in
  `coop_config.json`.
- What to verify in `KenshiCoop_*.log`: `tunnel peer[0]=... at 1.0.0.1` and
  `tunnel peer[1]=... at 1.0.0.2` on the host (the per-slot fake-address
  registry), both joins admitted with their ranks, and no
  `dropping packets from unexpected peer`.
- Watch: Steam's 1200-byte unreliable ceiling with THREE peers' worth of
  host traffic (the MTU clamp is per-link and unchanged, but total upload
  from the host doubles), and the Steam invite flow (the lobby now holds 3,
  but the automated hand-off still fires for the FIRST arrival only - the
  third player rides the pasted-ID/config path by design).

## 3. "Multiplayer (Wanderer x3)" game start

So players don't have to split tabs by hand. `KenshiCoop.mod` (FCS data mod,
shipped in `dist/mods/KenshiCoop/`) currently carries the "Multiplayer
(Wanderer x2)" start authored by zeroit789 - author the x3 variant in FCS:
the vanilla Wanderer start with three wanderers pre-split into three squad
tabs (ranks 0/1/2 = host / join 1 / join 2). Update the README start-list
text when it lands.

## Playing a real session today (before 1-3 land)

Works now, with manual setup:

1. **Host**: F2 panel, paste BOTH friends' Steam IDs (or `steamPeers` in
   `coop_config.json`), Role HOST, ONLINE.
2. **Player 2**: unchanged from 2-player - paste the host's ID, JOIN, ONLINE
   (Squad slot stays 1).
3. **Player 3**: paste the host's ID, JOIN, set **Squad slot: 2** in the F2
   panel (or `"ownRank": 2` in `coop_config.json`), ONLINE. The host rejects
   a slot that is already taken, so a mixed-up pair fails loudly instead of
   double-claiming a squad.
4. **The save needs a third squad tab**: in-game, drag some units into a
   third squad before (or after) the joins connect - rank 2 is player 3's.

## Hardening follow-ups (smaller, code-level)

- Decide relay-vs-host-echo per symmetric channel under real 3P play:
  faction/door/build/prod/research deliberately stay OFF the relay list
  (`NetLink::isRelayedType`) on the assumption the host's authoritative
  echo covers the third player - verify a join-2-placed building appears on
  join 1, and a join-1 door toggle lands on join 2.
- Soak the leave/rejoin matrix: join 2 drops mid-session (join 1 must be
  unaffected - surgical `clearOwnerReplicationState`), crash-rejoin slot
  reclaim, and a mid-session coordinated save with one join mid-transfer.
- Late-join while both joins are in-game: confirm the targeted connect-push
  reloads ONLY the newcomer.
