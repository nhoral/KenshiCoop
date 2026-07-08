# Validation Baseline (2026-07-05)

> The evidence record for the validation-layer overhaul: every tiered scenario
> re-validated under three regimes - **clean loopback**, **WAN proxy** (`bad`:
> 120 ms +/-40 ms one-way, 5% loss, both directions, below ENet), and **WAN +
> 30 s injected join clock skew** - with three-state (PASS/FAIL/SKIP) gates and
> the no-signal guard active. This document is what the smoke tier's
> "strong regression signal" claim rests on. Regenerate it after any oracle or
> protocol change by re-running the matrix (command below) and updating the
> tables from `tools/test-runs/history.jsonl`.

Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier full -Variants clean,wan,wanskew
```

Result of the 2026-07-05 run: **OVERALL PASS** - unit layer 63/63, 31 scenario
runs, 30 first-attempt passes, 1 flake (`npc_sync [wan]`, retry-passed;
recorded FLAKY, see findings). Plugin protocol v12.

## The validation stack

| Layer | What it proves | Where |
|---|---|---|
| `prototest` (unit) | Wire contract (packed sizes/offsets), reader truncation safety, content-hash determinism/field-sensitivity/order-independence, interpolation invariants | `src/prototest`, runs first in every tier |
| Scenario oracles | Per-pipeline replication correctness from structured logs; three-state gates, primary-gate no-signal guard | `scripts/CoopOracles.psm1` + `scripts/scenarios.psd1` |
| Tiers | smoke = one scenario per wire pipeline (~5 runs); full = everything + WAN variants | `scripts/regress.ps1` |
| Trending | per-gate metrics -> `tools/test-runs/history.jsonl`; drift vs rolling median | `scripts/report_history.ps1` |
| Offline judge | Same verdicts on ANY collected log pair (the remote-session path) | `scripts/analyze_run.ps1` |

Smoke tier (one per pipeline): `coop_presence` (entity stream + bidirectional
ownership), `npc_sync` (interest-managed NPCs + pose), `combat_kill` (reliable
events + attribution + outcome), `inv_bidir` (inventory snapshot/reconcile),
`world_weapon_drop` (world-item conservation).

## Matrix results and key measured values

`offset`/`rtt` from the wire time-sync (CLOCKSYNC); `evLat` = reliable-event
SEND->RECV latency (clock-offset-corrected); skew runs injected +30,000 ms into
the join's wall clock.

| Scenario | Save | clean | wan | wanskew | Key values |
|---|---|---|---|---|---|
| leader_move | sync | PASS | PASS | PASS | skew recovered err 0 ms (rtt 203) |
| coop_presence | squad1 | PASS | PASS | PASS | presence medians ~0.1-0.7u all regimes; skew err 8 ms |
| npc_sync | sync | PASS | FLAKY | PASS | clean worstMedian 0.51u ratio 1.0; wan 6.15u ratio 0.875 (tol 6 per manifest); skew err 6 ms |
| craft_order | craft1 | PASS | - | - | idle-before 38/38, operating-after 24/24 |
| down_order | down1 | PASS | - | - | upright->down transition clean |
| death_order | down1 | PASS | PASS | PASS | evLat 39 / 231 / 156 ms - reliable channel + retransmission under real loss |
| combat_probe | c | PASS | - | - | host-side read probe |
| combat_order | duel1 | PASS | - | - | live peaceful->combat flip |
| combat_kill | duel1 | PASS | PASS | PASS | downRatio 1.0 all regimes; evLat 37 / 179 / 277 ms |
| inv_order | squad1 | PASS | - | - | final-hash match |
| inv_bidir | squad1 | PASS | PASS | PASS | per-rank convergence both ways, all regimes |
| inv_equip | squad1 | PASS | - | - | worn-gear removal both ways |
| inv_reequip | squad1 | PASS | - | - | dip-then-restore both ways |
| inv_addequip | squad1 | PASS | - | - | durable create-loose-then-equip |
| world_item_sync | squad1 | PASS | - | - | proxy spawn + clean cull, hash match |
| wpn_relocate | squad1 | PASS | - | - | bag->ground->bag conservation |
| world_weapon_drop | squad1 | PASS | PASS | PASS | join relocated its copy in all regimes |

Diagnostics (`Tier = none`, never gate a tier): `drop_probe`, `inv_wpnseq`,
`spike`.

Highlights for the remote-co-op goal:

- **Clock alignment works on disagreeing clocks.** Every `wanskew` run recovered
  the injected 30,000 ms skew to within 0-11 ms (min-RTT filtered), and every
  time-aligned oracle produced the same verdict as its unskewed baseline.
- **Reliable events survive real loss with bounded latency.** Under 5% loss
  below ENet (retransmission genuinely engaged), EVT_DEATH / combat-KO delivery
  measured 156-277 ms vs 37-39 ms on clean loopback. These are now recorded
  metrics; drift shows up in `report_history.ps1`.
- **Whole-stack WAN coverage.** The proxy degrades ALL packet families both
  directions (handshake, reliable, unreliable) - unlike the legacy in-plugin
  NetSim, which only delayed inbound entity batches above ENet (kept for
  targeted experiments).

## Audit findings (what the re-validation caught or changed)

1. **`death_order` oracle rot (silent).** Its SEND regex predated combat
   attribution and anchored `hand=([\d,]+?) bs`; when events gained an
   ` actor=..` field the pattern stopped matching, so the oracle reported "host
   emitted no death event" against logs that plainly contained one. Fixed in
   `CoopOracles.psm1`; this alone justifies the re-validation exercise.
2. **Peer-ready scenario arming (harness redesign).** Scenario clocks previously
   started at HOST gameplay start, so scripted host actions (orders at +18 s,
   kills, item adds) frequently fired while the join was still loading (user-
   observed). Scenarios now ARM when the client first receives a peer's
   owned-entity batch (`Inbound::sawRemoteEntity`; fallback
   `KENSHICOOP_ARM_TIMEOUT_MS`, 0 = legacy for spikes). Subject-pinning
   scenarios pin at gameplay start via the new pre-arm `onGameplay` hook and
   HOLD until armed - pinning after the arming wait loses wandering subjects
   (observed: craft-worker "pin FAILED").
3. **`craft_order` manifest pairing.** Initially manifested with
   `-Setup craft`; the scenario in fact requires craft1 WITHOUT the host re-arm
   (it pins and orders the worker itself - that IS the test). Corrected;
   validated green.
4. **Deliberate WAN-regime adjustments (never silent).** `npc_sync` under the
   `bad` profile: position tolerance 3 -> 6 u (`WanTolerance`; one-plus update
   interval of lag at 120 ms) and `smoothness` demoted to advisory
   (`WanDemote`; the known latency catch-up stepping, already advisory for
   `coop_presence` per the postmortem). Declared in `scenarios.psd1` next to
   the scenario, applied only when a WAN proxy/link is active.
5. **Silent skips eliminated.** All formerly inconclusive-means-pass branches
   (pose, pose-state, body-state, smoothness, anim-truth, march, npc-track
   advisory) now report SKIP; a scenario whose PRIMARY gate skips FAILS
   (no-signal guard). SKIPs observed in the matrix and accepted as legitimate:
   `pose`/`body_state` on scenes with no seated/downed NPCs, `anim_truth` on
   mostly-stationary runs.
6. **Save pairings confirmed by execution.** `sync` (leader_move, npc_sync),
   `squad1` (presence + all inventory/world-item), `craft1`, `down1`
   (down/death), `duel1` (combat_order/kill), `c` (combat_probe, spikes). All
   fixture saves exist and load; re-bake path if one drifts: `bakeSave`
   (spike 82).
7. **Known flake, kept visible.** `npc_sync` judges ~8 autonomous bar NPCs;
   occasional boundary patrollers push the tracked ratio just under 0.8 (also
   seen once on clean). Retry-once records FLAKY (never silently green);
   repeat flakes surface in `report_history.ps1`. Accepted for now; tightening
   would mean a more controlled scene bake.
8. **Wire contract locked.** `prototest` asserts exact packed sizes (e.g.
   `EntityState` = 79 B, matching spike 12's measured 79 B/body) and content-
   hash properties. Canonical vector for cross-build comparison:
   `invEntryHash(wooden_sandals x2 q150) = 3104468019` (protocol v12).
   The hash moved to `src/netproto/ContentHash.h`, shared by `Engine.cpp` and
   the test so they cannot diverge.

## Remote-session readiness

- `scripts/make_remote_kit.ps1` packages the join side (mod + fixture save +
  `friend_join.ps1` one-command runner that bundles results for return).
- `scripts/host_session.ps1` runs the host side with a port-forward checklist.
- `scripts/analyze_run.ps1` judges the collected host/join logs with the exact
  oracles above; CLOCKSYNC makes the two machines' wall clocks irrelevant.
- `scripts/rehearse_remote_kit.ps1` dress-rehearses the whole flow locally
  (kit-installed join vs dev host through the WAN proxy) so the internet
  session changes only the network.

Session plan for the friend test: `coop_presence` first (connection + presence
proof), then escalate through the smoke set (`npc_sync`, `combat_kill`,
`inv_bidir`, `world_weapon_drop`), then free play.

## LAN two-machine validation (2026-07-05, addendum)

The friend-hosts topology rehearsed for real: machine 2 (`192.168.1.10`,
provisioned by `scripts\setup_lan_host.ps1` - SSH/SCP push + interactive
`KenshiCoopHost` scheduled task) plays HOST; the dev machine runs only the
JOIN via `scripts\run_lan_test.ps1`, judged by the same oracles after
collecting `host.log` over scp. `regress.ps1 -Lan` runs the tier matrices in
this topology (variants `lan`/`lanwan`/... in history).

Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts\regress.ps1 -Tier smoke -Lan -Variants clean,wan -SkipBuild
```

Result: **OVERALL PASS** - all 5 smoke scenarios green in both regimes, no
flakes.

| Scenario | lan (clean) | lanwan (proxy 'bad') | Key values |
|---|---|---|---|
| coop_presence | PASS | PASS | offsets -60 / -57 ms |
| npc_sync | PASS | PASS | worstMedian 0.77u -> 9.28u ratio 0.889 (WanTolerance) |
| combat_kill | PASS | PASS | event latency 11 ms LAN -> 160 ms through proxy |
| inv_bidir | PASS | PASS | per-rank convergence both ways |
| world_weapon_drop | PASS | PASS | conservation relocation held |

What LAN proved that loopback could not:

- **Real inter-machine clock offset, measured and corrected.** The two
  machines' wall clocks genuinely disagree by ~-57 to -77 ms (machine 2 behind
  the dev box), estimated with min-RTT 0-1 ms samples over the raw LAN. Every
  time-aligned oracle passed with the correction applied - the first
  non-simulated validation of CLOCKSYNC alignment. The offset also visibly
  wanders ~20 ms across a 40-minute matrix (live clock drift), which the
  per-run estimation absorbs.
- **Reliable-event latency on a real link**: 11 ms LAN clean (vs 37-39 ms
  loopback - loopback numbers include same-machine contention) and 160 ms
  through the WAN proxy.
- **Host-side firewall rule** (`netsh advfirewall` UDP 27800) added remotely
  and verified by the join actually connecting - the same rule
  `friend_host.ps1` self-installs in the real session.
- **Asymmetric machines**: machine 2 loads/runs independently; the `lan`
  timeout profile (StartTimeout 180 s, ArmTimeout 120 s) covered it without a
  single best-effort fallback triggering.
- **Same-Steam-account constraint** (spiked 2026-07-05): concurrent play on
  two machines works; keep ONE Steam client in OFFLINE mode or the second
  login bumps the first. `run_lan_test.ps1` pre-flight warns when it detects
  the same account online on both.

Session tooling for the real internet test (friend HOSTS):
`make_remote_kit.ps1 -Role host` -> friend runs `friend_host.ps1` (installs
mod+save, self-installs the firewall rule, prints the port-forward checklist
with its public IP, bundles results); you run `join_session.ps1 -HostIp <their
IP>`; judge with `analyze_run.ps1`. `rehearse_host_kit.ps1` dress-rehearses
that exact kit against machine 2 through the WAN proxy.

## Replication improvements (2026-07-05 evening, addendum)

The six architecture-review changes, each landed behind its own gate (plan:
"Replication Improvements"). New default-on mechanisms on the JOIN - every one
has an env escape hatch for A/B:

| Mechanism | Default | Escape hatch | Evidence |
|---|---|---|---|
| AI-suspend (`periodicUpdate` detour) as THE quieting layer | ON (join) | `KENSHICOOP_AI_SUSPEND=0` | 18-run A/B: pose_state 0.965 on vs 0.956 off; npc_track within the known flake band |
| Damage guard (`hitByMeleeAttack` -> HIT_MISSED on driven bodies) | ON (join) | `KENSHICOOP_DAMAGE_GUARD=0` | `damage_guard` oracle: host victim blood -30.8, join copy 0.0; green at 0 ms + WAN `bad` |
| Divergence-gated authority (trusted mode) | ON (join) | `KENSHICOOP_GATE_AUTHORITY=0` | 4-run A/B + retry: engages every run (grants 5-8, trusted ~5/12), tracking gates at parity |
| Dual-interest spheres (one per squad-tab leader) + suppression hysteresis | always | - | `split_interest`: bar NPCs keep streaming after the players split 260 u; churn 5/0 (no flapping) |

New conformance oracles (doctrine 17): `damage_guard` (gating in
`combat_kill`), `split_interest` (own scenario, save `sync`, full tier + WAN).

Negative results, kept deliberately (doctrine 22):

- **The quieting patchwork is still load-bearing.** Relapse counters fired
  449-2044x/run with AND without suspension; the once-on-entry clearGoals
  variant regressed npc_sync and was reverted. `SCENARIO QUIET` counters are
  permanent telemetry; deletion needs sustained zeros first.
- **`combat_order`'s cosmetic-engagement ratio is flaky** (post-order combat
  ratio measured 0.0-1.0 across 7 same-build runs; threshold 0.4). A/B showed
  no correlation with AI-suspend or damage-guard (fails with both off). The
  authoritative combat pipeline is unaffected (`combat_kill` + `damage_guard` +
  `death_order` all green incl. WAN); the join-side fight *rendering* sputter
  is a known issue for a future iteration.
- **WAN-regime adjustments added** (same precedent as npc_sync's, declared in
  the manifest): `split_interest` WanTolerance 18 (locally-animated rest wobble
  measured 14.6-17.5 u under `bad` vs 4.6-4.7 u clean; the streaming mechanism
  gate is latency-independent), `leader_move` smoothness demoted under WAN
  (zeroFrac 0.42-0.44 vs the 0.4 gate; catch-up stepping, crosscheck green).

### Final matrix (post-promotion, all six changes live)

`regress.ps1 -Tier full -Variants clean,wan,wanskew` with AI-suspend, damage
guard AND gate-authority default-on: **35 runs, all green** - 31 first-attempt
passes, 4 FLAKY (retry-passed, recorded): `leader_move [wanskew]` (smoothness
stepping, now demoted under WAN), `split_interest [wan]`/`[wanskew]` (one
17.5 u wobbler / one load-overlap no-signal run), `npc_sync [wan]` (the known
boundary-patroller band, ratio 0.7 -> 0.889 on retry). Skew runs recovered the
injected 30 s to 1-8 ms; combat KO delivery 31 ms clean / 150-221 ms under
loss; `damage_guard` green in all three regimes. The trusted-mode log lines
(`[trust] trusted=X driven=Y`) now appear in every join log as standing
telemetry.

`regress.ps1 -Tier smoke -Lan -Variants clean,wan` (real host on machine 2,
new DLL pushed per run): **10 runs, all green** - 8 first-attempt, 2 FLAKY
(`npc_sync [lan]` and `combat_kill [lan]`, both retried clean; retries measured
npc_track ratio 1.0 at worstMedian 2.4 u and KO delivery 20 ms). Cross-machine
clock offset measured 102-143 ms and corrected by CLOCKSYNC; lanwan KO
delivery 222 ms with the `bad` proxy on top of real LAN.

## Player combat + medical, phase 1 (2026-07-06, addendum)

Characterization truth table for the three new player-combat/medical scenarios
(plan: "Player Combat + Medical Replication"), run clean loopback BEFORE any
phase-2 replication features. These document what today's wire actually
carries; the phase-1 gates were tuned against these runs.

| Scenario | Verdict | Measured (representative runs) |
|---|---|---|
| `player_combat` | **PASS** | Striker intent applied by the join both windows (1-45 `[combat] order` applications); victim blood dropped on its OWNER: join-owned victim -11 to -90.4, host-owned victim -64.5 to -85.1. Copy divergence (phase-2 target): the HOST's copy of the join victim bled 40.3-44.4 (no host-side damage guard yet); the JOIN's copies stayed at 0 (join guard works). |
| `player_ko` | **PASS** | KO + revive edges cross as reliable events BOTH directions: KO latency 7 ms (host->join) / 33 ms (join->host), revive 27/36 ms; peer down-ratio 1.0 between the edges, upright-ratio 1.0 after the revive. |
| `medic_order` | **FAIL (expected)** | Spikes 21-23 confirmed end-to-end: owner-side wound (limb flesh 100 -> 39.8) NEVER appears on the peer's copy (flesh stays 100.0 all run, both directions); the healer finds 0 damaged limbs to bandage; no treatment ever returns to the owner. This is the exact gap phase 2 (vitals sync + treatment forwarding) closes; the scenario stays red until then. |

Scenario-shape lessons paid for in runs (recorded so nobody re-tries them):

- **Players cannot be each other's damage vector.** An unarmed ally-vs-ally
  player duel drew ZERO blood over a full 60 s window (blood flat at 75.8 both
  sides, run 004459) - block/stun sparring. An ARMED bar NPC took a leader from
  75.8 blood to -16 with limb flesh at 2.0 in ~25 s. Final shape: save `sync`,
  host orders NPC strikers onto each side's tab leader (A: join-owned victim,
  B: host-owned victim); both real-damage directions covered.
- **The bar brawl eats strikers.** Window A's striker was beaten unconscious by
  another bar NPC ~13 s in (run 010631), silently ending the window at 0.6
  blood of damage. The scenario now re-checks the striker every order tick and
  REPLACES it (fresh out-of-combat pick) when it is down or fighting the wrong
  body; window B's striker is reserved at window-A time, before the pool
  empties.
- **One applied order is the healthy case.** The join's replicator re-issues a
  driven combat order only while the local fight is broken, so a clean window
  can log exactly 1 application (run 011229, which still drew 11 blood). The
  intent gate is >= 1 application against the right victim; the blood gate
  independently proves the fight happened.
- **A driven squad member must never be detached from town AI** - the combat
  branch used to `detachFromTownAI` ALL driven bodies entering combat, which
  changes a squad member's hand CONTAINER and breaks its cross-client identity
  (found when player_combat first drove a peer-owned player character into a
  fight; fixed in `Replicator::applyTargets`, squad members now skip detach).

## Player combat + medical, phases 2-3 (2026-07-06, addendum)

Replication features shipped (protocol 12 -> 13): host-side damage guard
(detour now installed on BOTH clients; guard set = driven peer-squad bodies),
`PKT_MEDICAL` owner-authoritative vitals sync (player-squad only, reliable,
change-gated by quantized FNV hash, 400 ms send floor, 3 s safety resend,
`KENSHICOOP_MED_SYNC=0` escape hatch), and `PKT_TREATMENT` forwarding (first
aid on a driven copy -> per-limb bandage LEVELS to the owner, raise-only
apply). `prototest` covers both wire structs (size + roundtrip; 71 checks
green). The phase-1 advisory gates are now GATING:

| Scenario | Clean | WAN `bad` | Measured (promoted gates) |
|---|---|---|---|
| `player_combat` | **PASS** | **PASS** | Convergence gate (`MaxEndGap` 12): copy/owner final blood gap 0.2/0.5 clean, both windows, with owner-drops 67.1/34.0 - the copy now TRACKS the owner through a fight (phase-1 divergence was 40+). |
| `player_ko` | **PASS** | **PASS** | KO/revive edges still cross both directions under loss; down/up ratios 1.0. |
| `medic_order` | **PASS** | (full-tier) | All three legs green BOTH directions: wound crosses to the healer's copy (vitals sync), healer bandages it (n>0), treatment returns to the owner (bandRise >= 50). Phase 1 measured NOTHING crossing. |

Three failure modes found and fixed while promoting the gates:

- **A seat-injected driven copy ignores attack goals** (run 014713). The
  pre-seated window-A striker re-ordered 15x with `localFight=0` the whole
  window - the seat player-ORDER (`applyTaskOrder`, clear=true at the stool)
  outranks the AI GOAL `orderMeleeAttack` adds, so the copy never stood up;
  every window-A pass before the fix came from a never-seated RESTRIKE pick.
  The combat branch now flushes a committed pose via an order-path attack
  first (`addOrder` clear=true, `seatbrk=1` in the log). Post-fix window A
  engages first-order (36 applied orders, owner-drop 67.1).
- **Last-vs-last end-gap comparison is a timing artifact** (run 014713). The
  two scenario logs end ~6 s apart (arming skew + different durations) and the
  owner keeps bleeding after the copy's log stops: the run measured a fake
  38.3 "gap" while wall-clock-aligned samples agreed to 0.0. The oracle now
  compares the last samples inside the OVERLAP window (clock-offset-corrected
  `t`, 750 ms slack).
- **A dud striker can eat a whole window** (runs 033318/034659, wanskew). A
  striker that stays upright + un-engaged (goal silently ignored) or engages
  but draws no blood (weak/unarmed) was never replaced - the restrike check
  only caught DOWN or wrong-target strikers. The scenario now tracks the
  victim's blood per order tick and restrikes (excluding the dud from its own
  re-pick) after 3 no-fight / 5 no-blood ticks. Post-fix wanskew passes with
  owner-drops 79.3/65.6.

Full-tier regression (`regress.ps1 -Tier full -Variants clean,wan,wanskew`,
41 runs): green across the board with these exceptions - `player_combat
[wanskew]` failed pre-dud-fix (above; green after), `combat_order [clean]`
failed its known engagement-latency flake band twice (postRatio 0/0.214/0.432
across reruns vs 0.4 gate; A/B with `KENSHICOOP_MED_SYNC=0` showed the
phase-2 features are NOT the cause), and `npc_sync [clean]` hit its known
sitter band twice (ratio 0.7-0.778 vs 0.8; manual rerun 8/8 at worstMedian
1.8). Flaky-passed on retry: `leader_move [clean]`, `split_interest
[wanskew]`, `npc_sync [wan]`, `npc_sync [wanskew]`. LAN smoke (machine 2
host): 9/10 green; `npc_sync [lan]` failed only its gating smoothness
(zeroFrac band) with the primary tracking gate green.

## Consensus game-speed sync (2026-07-06, addendum)

Game speed (pause/1x/2x/3x) is now a consensus decision (plan: "Consensus
Game-Speed Sync", protocol 13 -> 14): each side detects local speed-button
clicks as REQUESTS (`PKT_SPEED_REQ`, reliable), the host arbitrates
`effective = min(hostReq, joinReq)` capped at 1x while EITHER player squad is
in combat, and broadcasts `PKT_SPEED_SET` (reliable, seq-guarded, 3 s safety
resend). Pause is speed 0, so min semantics give "either can pause, both must
raise". A denied raise SNAPS BACK immediately on the clicker's engine - a
click is a request, never a local override. `KENSHICOOP_SPEED_SYNC=0` is the
escape hatch.

| Scenario | Save | clean | wan | Key values |
|---|---|---|---|---|
| speed_sync | sync | PASS | PASS | 5 transitions, lone raise denied; join follow latency max 481 / 570 ms; steady-state match 1.0 over 103 aligned samples; combat window at 1x = 1.0 both sides both regimes |

The `speed_sync` scenario walks every consensus rule in one run: host clicks
3x alone (DENIED - min holds 1x), join clicks 3x (both raise), join clicks 1x
(either can lower), join clicks 3x again, then a host-squad fight demotes both
to 1x mid-3x (combat cap). `Test-SpeedSync` time-aligns the two SPEED series
(CLOCKSYNC-corrected) and gates the transition count, the denied lone raise,
per-transition follow latency, the steady-state match fraction, and the combat
window. Advisory `smoothness`/`anim_truth`/`march` stayed green through the 3x
windows - wall-clock-based interpolation tolerates fast-forward. `prototest`
78/78 (SpeedPacket = 14 B, REQ/SET roundtrip + truncation).

## Waiting-attacker combat stance (2026-07-06, addendum)

The join's teleporting-crowd artifact (waiting combatants AI-reset every 1.5 s
by the `clearGoals` re-issue loop, then snap-teleported) is fixed by the
"Combat waiting-attacker fix" plan (protocol 14 -> 15): the host classifies
each combatant's `CombatClass::combatState` and streams `TASK_COMBAT_WAIT` for
slot-queued attackers (active keeps `TASK_COMBAT_MELEE`); the join arms a
waiting copy ONCE and leaves it menacing. Re-issues happen only on target
mismatch or an ACTIVE copy disengaging, with 1.5 -> 6 s backoff and a ~6-order
episode budget; disarm is debounced 4 s (the stance rides the lossy batch);
the stable `combatModeActive` read replaced the flickering `isInCombatMode`
on both sides; position correction is graded (< 6 u leave alone / 6-20 u
walk-converge when not fighting or arming / > 20 u logged teleport on a 3 s
cooldown). Doctrine 25 in INTENT_REPLICATION.md.

| Scenario | Save | clean | wan | Key values |
|---|---|---|---|---|
| combat_crowd | sync | PASS | PASS | 5 strikers; both stances streamed (186/27 clean, 142/35 wan active/waiting samples); max re-orders per hand 11 / 13 (pre-fix 40-180); WAIT-stance snaps 1 / 3; all judged hands tracked, worst median 6.5 / 14.9 u (pre-fix 74-95 u) |

The `combat_crowd` scenario orders ~5 bar NPCs onto the host's own leader so
the AttackSlotManager builds a persistent waiting ring; both sides log every
captured NPC at 2 Hz (the JOIN also logs driven copies by direct resolve -
detached copies leave the interest capture). `Test-CombatCrowd` gates stance
coverage, per-hand re-issue count, WAIT-stance snap count after settle, and
per-hand median host-vs-join tracking (DOWN samples excluded on both sides -
where a KO'd body rests is Stage-2 territory; tolerance 20 u vs healthy 3-15 u
measured across 7 runs). Regressions on the final binary: `combat_order`,
`combat_kill`, `player_combat` all PASS (player_combat initially broke when
the walk band stomped an arming striker's attack goal - fixed by never
walk-driving a copy that still has re-issue budget; one earlier failure also
traced to the bar mob KO'ing the striker before it landed blood, a save-state
dynamic, and passed on re-run). Harness: `run_test.ps1` now waits for the
HOST to reach gameplay before launching the join - concurrent zone loads
starved the background host (a 12 s load measured 2.4 min, completing only
when the join exited).

## Full medical & limb sync (2026-07-06, addendum)

Protocol 15 -> 16 ("Full medical and limb sync" plan): the `PKT_MEDICAL`
snapshot now carries the FULL anatomy (up to 12 `MedPartEntry` slots keyed by
anatomy index: flesh + fleshStun + bandaging + juryRig, partType/side write
guard), the 4 `LimbState`s + robotic-limb template sids, and `PKT_TREATMENT`
forwards bandage levels for ALL parts. The same packet shape streams
combat-scoped WORLD-NPC vitals host -> join (fighting / being-fought /
down-dead within interest; change-gated, 1 Hz floor, 10 s stale window).
Limb loss replicates as reliable `EVT_AMPUTATE`/`EVT_CRUSH` edges + packet
self-heal; peers run the engine's own `amputate`/`crushLimb` with
`createSeveredItem=false`; the ground item rides the W1 world-item proxy
channel (host-authoritative), with join-side amputations deduping their local
copy. Doctrine 26 in INTENT_REPLICATION.md.

| Scenario | Save | clean | wan | Key values |
|---|---|---|---|---|
| limb_loss | squad1 | PASS | PASS | stump crossed 507/531 ms clean (both directions), sticky to run end; severed ground items owner=1 copy=1 both windows |
| medic_order | squad1 | PASS | - | full round trip green with the new PartCrossed/StunCrossed gates (full-anatomy wound incl. head/chest/stomach + stun) |
| combat_kill (+npc_vitals) | duel1 | PASS | PASS | new `npc_vitals` gate: streamed victim blood converges across sides (median tail gap within tolerance); damage_guard reworked to "no EXCESS join damage" since the join now legitimately tracks the host's drop |
| player_combat / player_ko | sync / squad1 | PASS | - | unchanged semantics on the protocol-16 packet |
| combat_crowd | sync | PASS | PASS | re-validated after the suppression fixes below |
| split_interest | squad1 | PASS | - | suppression still engages, churn 4/1 (in the historical 2-6/0-3 band) |

Bugs found and fixed during validation (both in the join's host-authority
suppression, doctrine 21 amendment):

- **Phantom walker** (user-reported): an NPC only the JOIN's local sim had
  (never streamed by the host) was never hidden - the `authCount_` hysteresis
  counters were pruned by `targets_` membership every tick, so a never-streamed
  hand could not accumulate the 75-frame suppress streak. Counters are now
  pruned by what `enforceHostAuthority`'s own enumeration saw this tick.
- **Detached-combatant freeze**: with the counters actually running, the
  hand-keyed streamed-set test suppressed combat-driven crowd copies -
  `detachFromTownAI` re-containers a world NPC, so its LOCAL key no longer
  matches the streamed key. The streamed test now also matches by body pointer
  against the set `applyTargets` drove this tick (`drivenChars_`).

Engine lesson: `RobotLimbs` is lazily allocated (null until a character first
loses a limb). The first limb_loss run no-op'd (`ok=0`) because the scaffold,
`readMedical`, and `applyLimbStates` all guarded on the pointer; they now read
through `MedicalSystem::getLimbState` (null == all-ORIGINAL). `createItem`
DOES fabricate `LIMB_REPLACEMENT` templates (the weapon/manufacturer failure
did not recur), so robotic replacements replicate without a conservation
fallback. `limb_loss` auto-enables `worldSync` (the severed ground item needs
the W1 channel); ordinary co-op sessions that want ground-item parity still
set `KENSHICOOP_WORLD_SYNC=1`.

## Character stats sync (2026-07-06, addendum)

Protocol 16 -> 17. `CharStats` (attributes, weapon/craft skills, xp) was
entirely local: driven copies kept save-load stats all session, the HOST
resolved a join character's authoritative world-NPC fights with those stale
numbers (doctrine 23), and cosmetic fights polluted driven copies with junk
XP. Each client now streams its OWNED player-squad members' full stat vector
(`PKT_STATS`: all 38 `StatsEnumerated` slots + xp + freeAttributePoints) on
the reliable channel - change-gated on a 0.1-unit quantized fingerprint, 1 s
floor, 5 s safety resend - and the peer writes it onto its driven copy via
the engine's own `CharStats::getStatRef` accessor + `periodicUpdate` recalc.
World NPCs are deliberately excluded (their authoritative fights already run
on the host with the host's correct local stats). Doctrine 27 in
INTENT_REPLICATION.md. `KENSHICOOP_STATS_SYNC=0` is the escape hatch.

| Scenario | Save | clean | wan | Key values |
|---|---|---|---|---|
| stats_sync | squad1 | PASS | PASS | raised stats crossed in 495/528 ms clean, 307/231 ms WAN (both directions); sticky to run end; untouched control stat (toughness) drift 0.0 both sides |
| prototest | - | PASS | - | 83/83 incl. `sizeof(StatsPacket)`=194 + round-trip/truncation |
| coop_presence | squad1 | PASS | - | presence pipeline unaffected |
| player_combat | sync | PASS | - | authoritative-damage path green with the stats channel live |
| medic_order | squad1 | PASS | - | reliable-channel coexistence (medical + treatment + stats) |

Measured channel behaviour (clean run): 3 host / 2 join `[stats] SEND` lines
over a ~110 s session - the seed snapshot per member plus one per scaffold
raise; the change gate keeps the channel silent otherwise (safety resends do
not log). The raise crossed inside one publish interval (~0.5 s loopback).

Known limitation (documented in the plan, out of scope): session setup
mirrors the host's save to the join, so join-side gains survive only in the
join's own save - cross-session persistence is a save/session-flow feature,
not a sync-channel one.

## Carried-body sync (2026-07-06, addendum)

Protocol 17 -> 18. Picking up / carrying / dropping a KO'd player-squad
member was not replicated at all, and the join's down-enforcement actively
fought it: a carried body still reads down/ragdoll, so the peer held it on
the ground (`knockDown`/`holdDown`) and teleported it after the carrier
whenever it drifted >2u - the dragged-body artifact. Now the CARRIER's owner
authors reliable `EVT_PICKUP_BODY`/`EVT_DROP_BODY` edges (subject = carried,
actor = carrier), each machine executes the SAME pickup between its LOCAL
pair via the engine's own `Character::pickupObject`/`dropCarriedObject`
(shoulder attach, carry animation and transform-follow all engine-native),
and continuous state self-heals: synthetic `TASK_CARRY_BODY` on the carrier
(throttled heal-pickup at 1.5 s; debounced heal-drop at 3 s) + a
`BODY_CARRIED` bodyState bit on the carried. The critical carve-out: a
carried copy (streamed bit OR locally `isBeingCarried`) skips the down
override and all position driving - the local attach owns its transform;
`koLatched` re-engages after the drop. A peer-left sweep drops any carry a
departed peer's copies still hold. Scope: player-squad carries only, own-tab
and both cross-tab directions. Doctrine 28 in INTENT_REPLICATION.md.
`KENSHICOOP_CARRY_SYNC=0` is the escape hatch.

| Scenario | Save | clean | wan | Key values |
|---|---|---|---|---|
| carry_order | squad1 | PASS | PASS | all 3 windows (host own-tab; join carries host-owned body; host carries join-owned body): pickup crossed in 63-465 ms clean / 186-340 ms WAN, drop likewise; carried copy rode its carrier at 0.0 median same-tick gap (pre-fix artifact = meters of drag); dropped body still DOWN on the peer (windows A/B) |
| prototest | - | PASS | - | 98/98 incl. TASK_CARRY_BODY/BODY_CARRIED collision + `bodyIsDown` exclusion checks |
| player_ko | squad1 | PASS | - | the touched KO-hold path: KO/revive latency 33-43 ms, down/up ratios 1.0 |
| medic_order | squad1 | PASS | - | reliable-channel coexistence (medical + treatment + carry events) |
| combat_kill | duel1 | PASS | - | KO event + damage guard + NPC vitals unaffected |
| coop_presence | squad1 | PASS | - | presence pipeline unaffected |

Fixed while validating (run 191905): the first carry_order run failed its
still-down gate because the SCAFFOLD's forced-KO timer (8 s) expired
mid-carry - the engine truthfully stood the body up the moment it was
dropped. The scenario now has each subject's OWNER re-top the KO timer every
2 s (`holdDown`, timer-only - never a fresh knockout on the shoulder) while
its windows need the body down; a real KO'd character (blood/stun-driven)
does not have this shape. Not a sync bug: pickups, tracking and drops all
crossed green in that same run.

Known limitations (documented follow-ups, out of scope): world-NPC carry
(kidnap/bounty/rescue) needs the NPC-authority/suppression interplay -
CLOSED 2026-07-07, see the "Real-session sync gaps" addendum below;
placing a carried body into beds/cages (`PUT_SOMEONE_IN_BED`/`PUT_IN_CAGE`)
is unreplicated - after a plain drop the existing KO/medical channels own
the state.

## Steam P2P transport (2026-07-07, addendum)

Remote sessions failed on network plumbing (UPnP refused by the router,
suspected CGNAT on the join side), not on sync. New OPT-IN transport: the
unchanged ENet protocol tunnelled over the legacy `ISteamNetworking` P2P API
in Kenshi's own `steam_api64.dll` (flat exports bound at runtime via
`GetProcAddress` - no SDK, no import lib, no new DLLs in the kit). Steam
brokers the connection BY STEAMID: UDP NAT punch first, silent Valve-relay
fallback - IPs, port forwarding and CGNAT leave the conversation. Wire
protocol untouched (`PROTOCOL_VERSION` stays 18); patch 0002 adds a
socket-hooks seam to the vendored ENet (`enet_set_socket_hooks`; NULL hooks
= byte-for-byte stock UDP path); host MTU clamps to Steam's 1200-byte
unreliable ceiling; sends are buffered-unreliable so the ENet handshake
survives session brokering. Two-code exchange: each side sets
`KENSHICOOP_TRANSPORT=steam` + `KENSHICOOP_STEAM_PEER=<other player's
steamid64>`; the receive path drops any other sender. Falls back to UDP
loudly on any init failure. Doctrine 29 in INTENT_REPLICATION.md.

| Check | Result | Key values |
|---|---|---|
| prototest | PASS | 98/98 - wire contract untouched by the transport layer |
| coop_presence (UDP, hooks compiled in but dormant) | PASS | run 091858 (build stamp verified fresh): presence 2/2 + 1/1 ratio=1.0, march 0, clock_sync 0 ms, smoothness 0.023 - stock path regression-clean. (The first citation, run 002909, had silently run a STALE DLL - the anti-stale build-stamp line is what caught it; always check it.) |
| Live-game bindings spike (single machine, self-ping) | PASS | run steam_spike_091731: `[steam] id=76561197970261979 loggedOn=1 iface=SteamNetworking005`; hooks installed + ENet host created over the fake socket; self-session went connecting=1 -> active=1; 19 pings round-tripped at 62-437 ms (Steam client IPC loopback), relay=0. Proves the hand-declared flat ABI, interface version, P2PSessionState layout, and the send/read path against the REAL Steam client. |
| tunneltest (in-process ENet-over-hooks) | PASS | 17/17: handshake, unreliable ch0, reliable ch1 8000-byte packet fragmented + reassembled byte-for-byte under 15% simulated loss, ZERO datagrams over the 1200-byte Steam cap (max seen exactly 1200), clean disconnect. `scripts\build_tunneltest.cmd` compiles the same patched ENet sources the plugin ships. |
| Steam spike (remote) | PASS (2026-07-07) | live two-account session: direct NAT punch (relay=0), RTT ~33 ms |
| Steam co-op session (remote) | PASS (2026-07-07) | full remote playtest over Steam P2P - positions, combat, health, speed all crossed; the session's three sync gaps (dropped items, equipment, NPC carry) are closed in the "Real-session sync gaps" addendum below |

Loopback note: a full steam-transport loopback run (both game clients on this
machine) is NOT possible - both processes share one Steam account/SteamID and
would race each other's ReadP2PPacket queue. The two single-machine spikes
above box that gap in from both sides: the self-ping proves everything from
the flat ABI down to real packet delivery through the Steam client, and
tunneltest proves everything from the ENet protocol down to the hook seam
under Steam's datagram constraints. The ONLY untested layer left for the
remote attempt is Valve's session brokering between two different accounts
(NAT punch / relay selection) - which is Valve-operated infrastructure, not
our code. Script support: `host_session.ps1 -PeerSteamId`,
`join_session.ps1/friend_join.ps1 -HostSteamId`, `friend_host.ps1
-PeerSteamId` (all accept short friend codes), `make_remote_kit.ps1
-Transport steam -MySteamId <id>` bakes the counterpart id into kit.json.

## Real-session sync gaps (2026-07-07, addendum)

The first successful remote playtest (Steam P2P, direct punch) surfaced three
gaps: dropped items never appeared on the other client, equipment changes
never crossed, and a host-side NPC carrying a downed PC never replicated.
Root causes and fixes (wire unchanged, protocol stays 18):

1. **Session defaults (bugs 1+2)**: `invSync` and `worldSync` were
   scenario-gated test flags, default-OFF in free play - the channels were
   implemented and validated but DORMANT in the real session. Both now
   default ON for `scenario == ""` (free play), aligned with
   `medSync`/`statsSync`/`carrySync`; `KENSHICOOP_INV_SYNC` /
   `KENSHICOOP_WORLD_SYNC` `"0"`/`"1"` are the escape hatches; scripted test
   scenarios outside the auto-on lists keep their old default. Doctrine 30.
2. **Equipped-armor drop coverage**: the session's exact action (drag
   equipped pants to ground) had no scenario - `world_weapon_drop` covers
   weapons only, though the W2 conservation channel always handled ARMOUR.
   New `world_armor_drop` scenario (prefers an EQUIPPED piece) shares the
   `WDROP` log contract and the (now parameterized) `Test-WeaponDrop`
   oracle.
3. **World-NPC carriers (bug 3)**: carry edges were authored for player-
   squad carriers only, and the join self-heal was `isSquad`-gated. The
   host now authors `EVT_PICKUP_BODY`/`EVT_DROP_BODY` for its streamed NPCs
   too; the join self-heals any driven carrier streaming `TASK_CARRY_BODY`
   and early-continues an NPC carrier with an active local attach (its
   local AI animates the carry walk; a graded soft-walk/cooldown-snap band
   keeps it on the host's path). A carrier that leaves the interest sphere
   mid-carry authors its drop from ABSENCE (3 s debounce) - found live:
   the NPC hauled the body out of interest and the transition detector
   never fired. Doctrine 28 amendment.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| world_armor_drop (new) | squad1 | PASS | host dropped EQUIPPED armor (invBase=1 -> 0, ground=1); join relocated its own copy (invMin=0, grndMax=1); `[wd]` DROP authored + APPLY moved=1 |
| npc_carry (new) | squad1 | PASS | pickup crossed in 140 ms; join detected the carrier from its own local world (`RECV PICKUP ok=1`); carried copy rode at 0 median same-tick gap; out-of-interest drop crossed via the absence path (~3 s) |
| inv_equip | squad1 | PASS | default-flip regression |
| inv_bidir | squad1 | PASS | default-flip regression |
| world_weapon_drop | squad1 | PASS | oracle parameterization regression |
| world_item_sync | squad1 | PASS | default-flip regression |
| carry_order | squad1 | PASS | NPC-carry extension regression (all 3 windows) |
| player_ko | squad1 | PASS | KO-hold path regression |
| coop_presence | squad1 | PASS | plain-session regression (x2: after the default flip and after the carry extension) |

## Bed & cage occupancy sync (2026-07-07, addendum)

Protocol 18 -> 19. Two halves (doctrine 31): the CONSCIOUS bed path
(`USE_BED`/`USE_BED_ORDER`/`SLEEP_ON_FLOOR`) was already on the
reproducible-pose allowlist but never runtime-validated (spike 24 PARTIAL);
UNCONSCIOUS placement into beds/prison cages was not replicated at all (the
doctrine-28 known limitation). New machinery, the carry shape applied to a
stateful attach: `BODY_IN_BED`/`BODY_IN_CAGE` bodyState bits + reliable
`EVT_ENTER_FURNITURE`/`EVT_EXIT_FURNITURE` (furniture hand rides the event;
`hostBody_` remembers it for the EXIT), engine-native
`setBedMode`/`setPrisonMode` between each machine's local pair, a carve-out
above the down branch scoped away from conscious bed TASKS, 1.5 s/3 s
self-heal, absence-authored exits, and a peer-left sweep.
`KENSHICOOP_FURN_SYNC=0` is the escape hatch.

New fixtures/tooling: `bedcage` setup scene (Camp Bed + Prisoner Cage spawned
by hand next to the squad) baked into save `bedcage1` by the new
`scripts/bake_scene.ps1` + `KENSHICOOP_BAKESAVE` deferred `SaveManager::save`
auto-bake - no manual save-menu round-trip.

Two transferred lessons found by `bed_pose`: (1) a fixture task that
TELEPORTS the body in (bed enter) fails `applyTaskOrder`'s proximity check
while the driven copy's interpolation still lags - the far-fixture result is
now RETRYABLE (throttled 1.5 s, `taskBad` only after 8 attempts) instead of a
permanent latch; (2) `detachFromTownAI` must never touch player-squad members
(it re-containers them, changing their hand identity mid-session) - now
`isSquad`-guarded.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| bed_pose (new) | bedcage1 | PASS | host + join commit the bed task at the same fixture (hostBed/joinBed >= 6 samples, medGap 0, matching pelvis ~2.5 vs standing ~9.6) |
| bed_put (new) | bedcage1 | PASS | A (host puts KO'd M2): enter 467 ms / exit 466 ms, gap 0; B (join puts L1): enter 65 ms / exit 62 ms, gap 0 |
| cage_put (new) | bedcage1 | PASS | A: enter 499 ms / exit 501 ms, gap 0; B: enter 29 ms / exit 533 ms, gap 0 |
| carry_order | squad1 | PASS | all 3 windows (one boundary-flake rerun: first sample at the drop marker read pre-drop) |
| npc_carry | squad1 | PASS | pick 489 ms, drop 483 ms, gap 0 (two reruns: the wandering world NPCs were outside interest twice - availability flake, suppresses=0 on BOTH sides those runs) |
| player_ko | squad1 | PASS | KO lat 13/12 ms, revive lat 30/31 ms both directions |
| down_order | squad1 | PASS | upright-before 38/38, down-after 23/23 |
| coop_presence | squad1 | PASS | bidirectional presence, tol=6 |
| prototest | - | PASS | 107/107, protocol 19, new occupancy-bit + event-constant checks |

## Stealth & detection-indicator sync (2026-07-07, addendum)

Protocol 19 -> 20. Two halves (doctrine 32): stealth POSTURE was streamed
half-way (`BODY_CRAWL` carried `isStealthModeOrCrawling` on the wire but no
receive path applied it - the peer saw a normal walk), and detection
indicators for a peer-owned sneaker existed nowhere (the owner's local NPCs
are suppressed copies that compute nothing; the host computes but cannot
render them). New machinery: a `BODY_SNEAK` bit streaming
`Character::stealthMode` exactly, applied to driven copies via the engine's
own `setStealthMode` (continuous idempotent state, 1 s throttle, placed after
the carried/furniture/down carve-outs); and `PKT_STEALTH` - the first
owner-directed FEEDBACK stream - carrying the driven sneaker's
`whoSeesMeSneaking` map (host-authored ~4 Hz change-gated UNRELIABLE
snapshots, one empty snapshot on the falling edge) back to the owner, who
replays each entry between its LOCAL pair via `notifyICanSeeYouSneaking`.
`KENSHICOOP_STEALTH_SYNC=0` is the escape hatch.

Phase-0 spike (`sneak_probe`, kept as a probe-tier scenario): host NPC vision
DOES fire against a driven (non-local-player) sneaker - the map filled within
1 s of `setStealthMode` on the driven copy, progress climbed continuously,
and entries aged out on their own when a seer looked away (no clear-on-leave
machinery needed). SEH-guarded boost-1.60 `unordered_map` iteration is
layout-correct against the game's in-tree boost.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| sneak_probe (new, probe tier) | sync | PASS | modeOn=64 samples, withSeers=40, detection entries with climbing progress on the driven copy |
| sneak_pose (new) | squad1 | PASS | both directions: peer copy mode=1 within 0.5 s (A) / same sample (B), 50 on-samples each window, off edges clean |
| sneak_detect (new) | sync | PASS | hostSends=21, joinApplied=21/21, join map fills through the channel (MAYBE prog rising -> YES) and drains after off |
| coop_presence | squad1 | PASS | plain-session regression |
| bed_put | squad1/bedcage1 | PASS | bodyState-bit neighbor regression |
| down_order | squad1 | PASS | upright/down enforcement regression |
| carry_order | squad1 | PASS | all 3 windows (pick/drop < 460 ms, gap 0) |
| prototest | - | PASS | 116/116, protocol 20, BODY_SNEAK bit + StealthPacket size/round-trip checks |

## Speed vote decoupled from the UI buttons (2026-07-08, addendum)

Wire protocol UNCHANGED (still 20) - this is a client-side rework of doctrine
24's capture/apply mechanics. The old state-diff click detector could never
show a vote (the arbitrated apply went through `setGameSpeed`, snapping the
buttons back to the min) and could never LOWER a stale vote with a click equal
to the current effective (stuck vote). Now the buttons express the local
player's REQUEST and stay where clicked; the sim runs at the arbitrated
min(host, join) underneath. Votes are captured by `KenshiLib::AddHook` detours
on `setGameSpeed`/`userPause`/`togglePause` (reentrancy-guarded against our own
writes) PLUS a poll fallback: manual sessions (2026-07-08) proved the MainBar
click handler writes the speed INLINE (real UI clicks never reach the detours).
The poll detects (a) the engine leaving the last quietly-written state (real
click / keyboard pause - engine state IS the request) and (b) the button
highlight moving while the engine did NOT: the same-value click AND the
speed-click-while-paused, where the vote's pause state is decoded from WHICH
button lit (pause button = pause vote; any speed button = UNPAUSE vote at that
speed - reading the engine's pause flag instead kept both clients stuck voting
pause). The effective is applied by `writeGameSpeedQuiet` =
`setFrameSpeedMultiplier` (bare, UI-silent) + guarded `userPause` +
vote-highlight restore. A continuous enforcement tick re-asserts the effective
when a pass-through click diverges the engine (replaces the same-tick
snap-back).

Phase-0 spike (`speed_probe`, kept as a probe-tier scenario, speedSync forced
off): `setFrameSpeedMultiplier` drives the sim and STICKS (12/12 samples at
3.0, buttons untouched); `userPause` DOES re-highlight the buttons from the
effective (first probe run FAILed on "quiet pause MOVED the buttons") - hence
the snapshot/restore fallback from the plan, which the second run proved
clean (12/12 pause samples, highlight restored).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| speed_probe (new, probe tier) | sync | PASS | quiet 3x stuck 12/12, buttons '0100' constant across quiet acts, loud click moved buttons + INTENT mult=2.00 captured, 0 intent leaks |
| speed_sync (reworked: + same-value vote-lowering leg) | sync | PASS | 5 transitions, max follow 490 ms, lone raise denied, sameValueVote=true, reraiseDenied=true, match 1.00/123, combat window 1x both sides |
| coop_presence | squad1 | PASS | plain-session regression |

Manual two-instance session (2026-07-08): 1x/2x votes shown per-client, speed
changes only when both agree, pause from either client pauses both, speed
clicks while paused unpause once both vote (the poll decodes the lit button;
the pause-flag read had left the session permanently paused - fixed same day).
Oracle hardening from the fix run: `Test-SpeedSync` skips host SET transitions
landing after the join's sampling window (host tail outlives the join; a
post-combat re-raise there is unjudgeable - was a flake).

## Runtime NPC spawn sync (2026-07-08, addendum)

Protocol 20 -> 21 (`PKT_SPAWN_REQ`, `PKT_SPAWN_INFO`). Root cause of the
2026-07-07 field report ("host fought enemies the join couldn't see; another
squad appeared only on the join"): NPC sync resolves bodies by save-stable
hand, so a squad the host's spawn manager mints at RUNTIME (roaming bandits,
dialog ambushes) has a host-only hand the join can never resolve - no
replication channel existed. Fix is pull-based proxy replication (doctrine 33):
the join sends `PKT_SPAWN_REQ` for streamed hands it cannot resolve (debounced,
retry-capped, proximity-gated to 250 u of its own squad), the host replies with
the spawn's DESCRIPTION (template stringID + faction stringID + transform +
dead flag), and the join mints a local proxy through the engine's own factory.
The hand->proxy translation lives at the single `applyTargets` resolve choke
point, so proxies inherit the entire world-NPC drive path (AI-suspend, damage
guard, combat, death latches, authority hysteresis). The join-only-enemies half
is suppression hardening: `enforceHostAuthority` now re-asserts the hide every
2 s (dialog/combat packages and zone streaming can undo a one-shot
`suppressNpc`), only books suppressions the engine confirmed, and logs
`suppress MISS` otherwise. `KENSHICOOP_SPAWN_SYNC=0` is the escape hatch.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| spawn_probe (new, probe tier; spawnSync forced OFF) | sync | PASS | evidence baseline: 8/8 host runtime hands (near + far legs) logged `[spawn] unresolved` on the join; 4/4 join-local runtime spawns suppressed in ~1.2 s |
| spawn_sync (new, full tier) | sync | PASS | proxies bound 8/8 (near 4/4, teleport-600u far 4/4), PROXY-vs-MEMBER median tracking 0.7-0.8 u (tol 6), join-local spawns suppressed 4/4 |
| coop_presence | squad1 | PASS | plain-session regression |
| combat_probe | c | PASS | runtime-spawned duelists regression (spawn path adjacency) |
| split_interest | sync | PASS | dual-sphere interest regression |
| npc_sync | sync | npc_track PASS | position/pose gates green; the `smoothness` gate flaked under tonight's machine load - the CONTROL (protocol-20 kit DLL) run failed it identically (zeroFrac 0.938 vs 0.912 new), so it is environmental, not protocol-21 |

Suppression finding (phase 0, for the record): the probe could NOT reproduce
the reported "join-only enemies stayed visible" gap in a clean scenario -
`enforceHostAuthority` caught all 4 join-local runtime spawns within ~1.2 s
(hysteresis working as designed). The field report's likeliest mechanism is
the engine UNDOING the one-shot hide afterwards (ambush dialog re-tasks the
body; zone streaming re-adds it to the update list) - hence the 2 s re-assert
hardening, which is cheap and idempotent either way.

Accepted limitations: proxy appearance/equipment approximate the host's body
(template + faction, randomized gear - cosmetic; combat outcomes stay
host-authoritative + damage-guarded); ambush dialog is not synced; host/join
runtime-hand collision is theoretically possible (both engines mint sequential
runtime ids) but unobserved - the join's own runtime spawns are suppressed
within ~1 s.

## Money / economy sync, phase 1 (2026-07-08, addendum)

Protocol 21 -> 22 (`PKT_MONEY`). Kenshi's wallet is per-`Platoon`
(`Ownerships::money` via the tab's Platoon - no global player wallet, spike
29) and NOTHING about money was on the wire: the shop_probe baseline proved a
sentinel `Ownerships::setMoney` write on one side never moved the peer's
series (host rank0=5000 / join rank1=7000, both "did NOT cross"). The channel
is owner-authoritative by squad-tab RANK (the same sorted-distinct-containers
partition positional/inventory sync own): each client publishes the wallet of
every tab it owns on the reliable channel (change-gated, ~1 Hz floor, 5 s
safety resend - the `PKT_STATS` pacing), and the receiver writes peer tabs via
`Ownerships::setMoney`. `KENSHICOOP_MONEY_SYNC=0` is the escape hatch;
`shop_probe` forces it off to keep the unsynced baseline measurable.

Probe findings that REFRAMED the planned vendor-stock half (shop_probe runs
101952/103018/103547/104036):
- a `ShopTrader`'s aggregated `Inventory` is LAZY - null until the trade UI
  first opens (every enumerated vendor read stock=-1; forcing
  `ActivePlatoon::refreshInventory` couldn't help because...)
- ...the SHOP_TRADER_CLASS objects near the leader carry NO bound trader
  (`trader` member AND `getTrader()` both null; sids are building/furniture
  GameData ids), and
- the wrapper hands are RUNTIME-minted (index stable, serial differs per
  client/run) - a hand-keyed vendor snapshot can never match cross-client.
Vendor stock is regenerated per client by the engine anyway, so the vendor-
side mirror is deferred: the `[shop] BUY-LOCAL` detour on `Inventory::buyItem`
(installed with money sync) now logs every REAL purchase (seller identity +
register money, item sid, buyer) so manual field sessions accumulate the
evidence for a shop-open-scoped design keyed by the trader Character's
save-stable hand. The buyer-side effects of a purchase - wallet debit + item
into the buyer's bag - already converge over PKT_MONEY + the phase-4a
inventory channel, which `vendor_trade` gates as a composite (`invSync`
auto-on for it, like the inv_* family).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| shop_probe (new, probe tier; moneySync forced OFF) | sync | PASS | baseline: sentinel wallets diverged by exactly +5000/-7000 (nothing crosses); 320+ vendor reads all stock=-1 / trader-null; both buy attempts res=-2 (no reachable stocked vendor) |
| money_sync (new, full tier) | sync | PASS | crossed=2/2 - host 5000 -> join rank0, join 7000 -> host rank1; `[money] SEND/RECV` pairs ~30 ms apart; no drift on co-visible ranks |
| vendor_trade (new, full tier) | sync | PASS | walletCrossed=2/2 (4750/6750 after the 250 debit) + TINV content hash converged both ranks (the bought item crossed) |
| inv_bidir | inv | PASS | regression (first run red: stray kenshi_x64 from an earlier session tripped the 3-player guard + join crash; clean rerun green both directions) |
| world_item_sync | sync | PASS | regression |
| combat_kill | c | PASS | regression |
| coop_presence | squad1 | PASS | plain-session regression |

prototest: 128/128 (adds `sizeof(MoneyPacket)`=13 + PKT_MONEY round-trip).

Accepted limitations: vendor register cash / stock stay per-client until the
shop-open-scoped mirror (divergence is cosmetic; the engine re-rolls stock on
its own refresh cadence); sell-side money flows are covered by the same wallet
stream (the credit lands on the seller's owned tab and crosses), but the SOLD
item entering vendor stock stays local like any vendor stock.

## Recruitment sync (2026-07-08, addendum)

Protocol 22 -> 23 (`EVT_RECRUIT` + bidirectional describe/mint). The
recruit_probe baseline (run 114151, recruitSync forced off) established the
gap's exact shape: `PlayerInterface::recruit` works programmatically on both
sides for both subject classes; the recruit RE-CONTAINERS the body (baked
subjects keep their SERIAL, index changes; runtime subjects spawned into the
player container keep the whole tail); host recruits reached the join only as
DUPLICATE proxies (spawn-sync minted a second body while the local baked copy
still stood); join recruits reached the host NOT AT ALL (the describe channel
was join-pull only); and a join recruit landed in the HOST-owned rank-0
container - the rank partition alone would misattribute ownership.

The channel: a detour on `PlayerInterface::recruit` records every successful
LOCAL recruit's before/after hand pair ("[recruit] LOCAL"); `publishRecruits`
drains the queue into reliable `EVT_RECRUIT` events (subject = old hand,
actor = new hand) and pins the new hand into `recruitOwned_` (publishOwned
streams it regardless of tab rank; the receiver's `peerRecruit_` pin vetoes
the inverse). The receiver RE-KEYS its existing local copy of the old hand
into `proxyByKey_` under the new key - the protocol-21 translation point -
so the recruit inherits the whole driven-body path (AI-suspend, damage guard,
latches) with ONE body and no duplicate mint; a suppressed copy is restored
first, and an already-proxied old hand migrates its binding. Runtime-born
recruits (old hand unresolvable on the peer) ride the describe/mint channel,
now BIDIRECTIONAL (both sides answer PKT_SPAWN_REQ and author them).
`KENSHICOOP_RECRUIT_SYNC=0` is the escape hatch; recruit_probe forces it off.

One fix out of the first gated run (120738): both sides' deterministic
nearest-NPC pick recruited the SAME bar NPC (the join's copy was already
peer-driven), colliding both ownership pins on one hand. probeRecruit now
skips AI-suspended (peer-driven) bodies; a REAL double-recruit of the same
NPC remains a documented race (the second recruiter's EVT is a no-op on the
first's machine since the hand is already recruitOwned_ there).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| recruit_probe (probe tier; recruitSync forced OFF) | sync | PASS | baseline run 114151: 4/4 legs res=1; baked serial survives / index+container change; join tabs 2->4 (new platoons sort AFTER - preexisting rank order stable); host recruits duplicated on join (proxyBound=1 beside the standing baked copy); join recruits invisible on host (unresolved/REQ=0) |
| recruit_sync (new, full tier) | sync | PASS | converged=4/4 - baked legs re-keyed (peer rekey=1, proxyBound=0), runtime legs minted over the bidirectional channel (proxyBound=1), every hand position-tracked (proxyTrack 35-68 samples); no duplicates |
| coop_presence | squad1 | PASS | plain-session regression |
| npc_sync | c | PASS | regression |
| spawn_sync | sync | PASS | regression (join-pull half of the now-bidirectional channel unchanged) |
| split_interest | squad1 | PASS | regression (rank partition + recruit pins coexist) |

prototest: 129/129 (adds EVT_RECRUIT distinctness; EventPacket shape unchanged).

Accepted limitations: the recruit appears on the peer as a DRIVEN body, not in
the peer's squad UI (peer-side platoon placement deferred - it would recruit
the copy locally and re-partition ownership); recruits are session-state (save
/load coordination is gap 8); two players recruiting the SAME NPC in the same
instant keeps one body per machine but ownership resolves to whoever's EVT
lands second on each side (benign for position sync, unmeasured for control).

## Known limitations (honest edges)

- Both clients still share one GPU/CPU in local runs; genuinely asymmetric
  machine performance is only exercised by the real remote session.
- The WAN proxy models delay/jitter/loss (+ reorder via jitter), not bandwidth
  caps or NAT behaviour.
- Wall clocks are ms-since-midnight; a session spanning local midnight would
  confuse time alignment (accepted; pre-existing).
- `combat_probe` duelists are runtime spawns (host-local); its WAN variant is
  intentionally off. Cross-client combat at scale rides the baked-save path
  (spikes 56/82).
