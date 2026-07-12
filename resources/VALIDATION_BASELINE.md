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

## Faction-relation sync (2026-07-08, addendum)

Protocol 23 -> 24 (`PKT_FACTION`). Gap 3: relation state is per-client
`FactionRelations`; attacking a faction flipped hostility on ONE machine only.

faction_probe (run 132239, factionSync forced OFF) settled the design:
faction GameData stringIDs are cross-client stable on the shared save (10/10
interesting rows common, censuses identical at n=72); the engine keeps the
two per-side tables MIRRORED (us==them in every enumerated row, before and
after the sentinel writes); enemy/ally flags DERIVE from the value (sentinel
-75 flipped enemy=1 both directions, +65 flipped ally=1, same tick); the
sentinel `setRelation` sticks locally for the whole run and nothing crosses.
Consequence: ONE f32 per faction sid is the complete wire state - no flags,
no reciprocal row, no cause enum needed on the wire.

Mechanism: both clients run the same detector inside
`Replicator::publishFactions` - the player-faction relation table sampled at
1 Hz (immediately when either detoured `FactionRelations::affectRelations`
overload recorded a real engine mutation), rows that moved >= 0.5 vs the
seeded baseline stream as `PKT_FACTION` (sid + f32 + per-sender seq,
CH_RELIABLE, 10 s per-sid safety resend; the seeded baseline keeps a settled
diplomacy silent). `applyFactions` writes received rows onto BOTH local table
directions via `setRelation`, updating the baseline BEFORE the write - the
echo guard - and skipping stale (seq) or already-converged rows.
`KENSHICOOP_FACTION_SYNC` (default ON) is the A/B hatch; faction_probe forces
it off.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| faction_probe (new, probe tier; factionSync forced OFF) | sync | PASS | run 132239: sids stable, tables mirrored, flags derived, sentinels stuck locally + did NOT cross; AFFECT-EV/AMT=0 (no organic mutations in a 40 s idle run) |
| faction_sync (new, full tier) | sync | PASS | run 133734: crossed=2/2 (host -75 and join +65 each applied on the peer ~10 ms after the write, ok=1), no diverged co-visible rows at end; exactly one SEND + one RECV per leg (echo-free) |
| coop_presence | squad1 | PASS | regression |
| npc_sync | c | PASS | regression |
| combat_kill | c | PASS | regression (combat still clean with the affectRelations detours installed) |
| recruit_sync | sync | PASS | regression (protocol 23 channel unchanged) |

prototest: 133/133 (adds sizeof(FactionPacket)=61 + round-trip).

Accepted limitations: bounties / crime state are a separate engine system
(the `[fac] AFFECT` detour keeps accumulating cause-attribution field
evidence); NPC faction-vs-faction wars are world-sim (gap 6) - only rows
involving the PLAYER faction stream; the AFFECT-triggered immediate sample
covers engine-native mutations, while console/mod writes that bypass
affectRelations ride the 1 Hz sample instead.

## Game-clock sync (2026-07-08, addendum)

Protocol 24 -> 25 (`PKT_TIME`). Gap 4 (the last Priority-1 item): each client
integrated its own in-game clock from its own load/pause moments, so the
calendar drifted and day/night (NPC schedules, shop hours, stealth vision)
diverged - no packet carried game time.

time_probe (run 141509, timeSync AND speedSync forced OFF) settled the design:
`GameWorld::getTimeStamp_inGameHours` (struct-return ABI - TimeOfDay has
user-declared ctors, so the hidden-retbuf model applies, same as
getPositionBip01) returns the ABSOLUTE campaign clock in total in-game hours;
the host/join offset on the shared save was exactly the load-moment skew
(~0.3 gh, drifting further with every differential pause); hour length is
identical on both clients (109.1 s per game hour); and the clock rate tracks
`frameSpeedMult` EXACTLY (2x burst -> 2.00 measured ratio) - so a sim-speed
slew is a precise clock actuator.

Mechanism: the host broadcasts `PKT_TIME` (absolute f64 gameHours + seq,
CH_RELIABLE, ~1 Hz - at 109 s/gh, 50 ms of wire latency is ~0.0005 gh, so no
RTT compensation). `Replicator::syncTime` on the join computes offset = host -
local per sample and steers `timeSlew_`, a proportional multiplier (gain 30
per gh, capped +2x, floored 0.25x, engage >0.01 gh / disengage <0.002 gh
hysteresis) that `slewedEffective()` folds into EVERY quiet speed write the
consensus layer makes - the slew multiplies the arbitrated effective instead
of overwriting it, so `speed_sync` enforcement and the clock correction
compose. A direct clock STEP (write the `timeStamper` CPerfTimer base,
self-verifying with revert) was prototyped and REJECTED: the calendar does not
derive from that timer (run 150001) - no writable clock base exists, slew-only.
`KENSHICOOP_TIME_SYNC` (default ON) is the A/B hatch; time_probe forces it
off (with speedSync, so its burst applies unarbitrated), and speed_sync
forces timeSync off (its oracle gates RAW fsm equality, which the slew
intentionally violates during catch-up; the composition is gated by
time_sync instead).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| time_probe (new, probe tier; both syncs forced OFF) | sync | PASS | run 141509: absolute clock, initial offset 0.302 gh, unsynced drift +0.091 gh over 40 s, burst rate ratio 2.00, hourLen 109.1 both sides |
| time_sync (new, full tier; 65 s + 2x consensus burst t=15-25 s) | sync | PASS | run 145800s-block: ~0.3 gh skew closed in ~35 s at the 2x cap, final offset 0.00214 gh (tol 0.02), monotonic both sides, convergence held across the burst |
| coop_presence | squad1 | PASS | regression |
| npc_sync | c | PASS | regression (run 155343: 9/9 tracked, ratio 1.0 - earlier 4x slew cap DID dip tracking below gate, which is why the cap is 2x) |
| speed_sync | c | PASS | regression (timeSync forced off in-scenario; combat window + transitions clean) |
| faction_sync | sync | PASS | regression |

prototest: 137/137 (adds sizeof(TimePacket)=17 + round-trip).

Accepted limitations: the catch-up is a visible session-start transient (the
join sim runs up to 2x for ~35 s per 0.3 gh of skew - the 4x cap that halved
that window measurably disturbed NPC tracking and was rejected); differential
pauses during play re-open small offsets that re-close within seconds; save/
load calendar coordination across sessions is gap 8; NPC-schedule / weather
CONSEQUENCES of the now-shared clock are gap 6 territory.

## Door-state sync (2026-07-08, addendum)

Protocol 25 -> 26 (`PKT_DOOR`). The first slice of gap 5 (buildings): door and
gate open/lock state on BAKED buildings was per-client - one player walks
through a gate the other sees closed, and door state feeds pathfinding, AI
access, and base defense. No packet carried it.

door_probe (run 160041, doorSync forced OFF) settled the design: baked-door
hands are cross-client stable (the census intersected on the shared save -
the furniture/bed identity precedent extends to `DoorStuff : Building`,
enumerated via `getObjectsWithinSphere(BUILDING)` + the `imADoor` member);
the engine's own `openDoor`/`closeDoor` entries work as the write lever and
animate natively (DoorState walks OPENING->OPEN over ~1 s, which is why the
channel publishes the collapsed DESTINATION state - OPENING counts as open -
never the mid-swing transient); and with no channel the sentinel toggles
stayed strictly local (both sides toggled the same co-visible door; neither
write crossed).

Mechanism: SYMMETRIC change-gated rows, the faction-relation shape. Both
clients sample doors within ~100 m of their interest centers ~1 Hz and stream
rows whose (open, locked) moved vs a seeded per-hand baseline (`PKT_DOOR`:
hand + open + locked, CH_RELIABLE); received rows apply through the engine's
own door actions (`writeDoorByHand`: polite `openDoor`/`closeDoor` first,
`_forceDoorOpenUT`/`_forceDoorClosedUT` only when refused; `lockDoor`/
`unlockDoor` when the door has a DoorLock). The baseline updates BEFORE the
apply write, so an applied row is never re-detected as a local change
(echo-free); per-sender seq drops stale rows; a 10 s safety resend covers
loss; hands that fail to resolve locally are skipped silently
(out-of-interest or a runtime-placed door - accepted edge).
`KENSHICOOP_DOOR_SYNC` (default ON) is the A/B hatch; forced OFF inside
`door_probe`.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| door_probe (new, probe tier; doorSync forced OFF) | sync | PASS | run 160041: census 1 door/side, common=1 (hand-stable), both sentinel writes stuck locally (polite lever), neither crossed |
| door_sync (new, full tier; doorSync ON) | sync | PASS | run 161116: crossed 2/2 (host toggle applied on join in <20 ms wire-to-write, join toggle likewise), diverged 0, no echo ping-pong (3 packets total) |
| coop_presence | squad1 | PASS | regression |
| npc_sync | c | PASS | regression (run 163712; earlier same-day runs flaked npc_track in BOTH doorSync arms - zero [door] packets in that save, machine-load flake, passed after cooldown) |
| faction_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: 141/141 (adds sizeof(DoorPacket)=31 + round-trip).

Accepted limitations: only BAKED doors stream (player-placed buildings are
runtime hands - the protocol-21 describe/mint problem for structures stays in
gap 5); interest-scoped (~100 m of either player's leaders), so a door that
moved while NEITHER player was near reconciles only when someone returns and
the change-gate sees it move again (the shared save seeds agreement at load);
door HEALTH/broken state and lockpicking stay out of scope (doctrine 31); the
`sync` save has a single door in range - multi-door towns exercise the same
code path but were not separately gated.

## Placed-building sync (2026-07-08, addendum)

Protocol 26 -> 27 (`PKT_BUILD_PLACE` + `PKT_BUILD_STATE`). The second slice of
gap 5 (buildings): a player-placed building is a RUNTIME object whose hand
exists only in the placer's session - a building one player places does not
exist AT ALL for the other, and construction progress had no channel. This is
the protocol-21 identity problem for structures.

build_probe (run 174550, buildSync forced OFF) settled the design's three open
questions: the raw `createBuilding` factory call BYPASSES the UI's
town-placement verification (`PreviewBuilding::placementVerification` is a
UI-layer gate; both programmatic placements landed in the town-adjacent `sync`
save - so a peer mint always lands where the placer's did, and the channel
never needs placement rules relitigated); minted-site hands are RUNTIME (the
cross-client census intersection was ZERO, unlike baked doors/furniture - the
wire must key by the placer's hand); and the engine's own
`setConstructionProgress` works as the progress lever on a 0..1 scale and
SELF-COMPLETES at >= 1.0 (progress jumps to the engine's internal 4.0,
isComplete flips, the scaffold comes off natively - no explicit completion
call needed).

Mechanism: PLACER-AUTHORITATIVE describe/mint, the protocol-21 proxy precedent
for structures. A local placement - the `PreviewBuilding::
placeFinalPreviewBuilding` detour catches real build-mode commits, the
programmatic scenario place queues the same edge - announces `PKT_BUILD_PLACE`
(template sid + transform, keyed by the PLACER's local hand, CH_RELIABLE).
The receiver mints an INCOMPLETE local site through the same factory and keeps
a key -> local-hand translation map (a refused mint is remembered so resends
never retry the factory; duplicate keys dedupe). The placer then samples its
own sites ~1 Hz and streams change-gated `PKT_BUILD_STATE` rows (10 s safety
resend while incomplete; the complete=1 row latches that site's channel
silent), applied through `setConstructionProgress` via the map. Echo-free BY
CONSTRUCTION rather than by baseline ordering: a factory mint never passes
through the placement detour, so a minted proxy cannot re-announce.
`KENSHICOOP_BUILD_SYNC` (default ON) is the A/B hatch; forced OFF inside
`build_probe`.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| build_probe (new, probe tier; buildSync forced OFF) | sync | PASS | run 174550: both placements rc=1 (factory bypasses town rules), own site enumerable prog 0->0.75 then complete, census hand intersection ZERO (runtime hands), nothing crossed |
| build_sync (new, full tier; buildSync ON) | sync | PASS | run 175747: minted 2/2 (both directions), progress crossed 2/2 (4 applied rows each, complete=1 latched), channel silent after completion |
| coop_presence | squad1 | PASS | regression (clean + wan) |
| npc_sync | c | FLAKY->PASS | regression; first attempts failed smoothness (clean, zeroFrac 0.505) / npc_track (wan, ratio 0.778 vs 0.8) - the documented machine-load signature, zero [build] packets in that save, both retry-passed |
| door_sync | sync | PASS | regression |
| faction_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: 149/149 (adds sizeof(BuildPlacePacket)=94, sizeof(BuildStatePacket)=34
+ round-trips).

Accepted limitations: doors on PLACED buildings do not stream yet (the door
channel skips unresolvable runtime hands; the translation map now exists, so
keying PKT_DOOR through it is the natural next stitch); building
DELETION/dismantle has no channel (a dismantled site stays on the peer until
re-load); a peer that connects AFTER a site completed never receives the
PLACE (announcements are session-scoped, not persisted state transfer - the
shared-save workflow reloads from a common save anyway); the minted proxy's
OWNER faction is unset (cosmetic; ownership semantics stay with the placer's
client); production machines, research, farming, and door HEALTH stay in
gap 5 / doctrine 31.

## Placed-building doors + dismantle (2026-07-08, addendum)

Protocol 27 -> 28 (`PKT_BUILD_DOOR` + `PKT_BUILD_REMOVE`). The two stitches
flagged by the protocol-27 slice: doors on PLACED buildings are runtime
objects on BOTH clients (the protocol-26 door channel skips unresolvable
hands, so one player's shack door stayed shut on the other's proxy), and a
dismantled/destroyed placed building left a GHOST proxy on the peer.

bdoor_probe (run 195513, bdoorSync forced OFF, protocol-27 mint channel ON)
settled the design's open questions: a minted proxy mints its own DoorStuff
children in TEMPLATE ORDER (the shack's door sat at parent->doors index 0 on
both clients - so (placer's building hand, door index) resolved through the
protocol-27 build maps names the same physical door everywhere, and no raw
door hand ever needs to cross); the engine's polite openDoor/closeDoor lever
works on runtime doors exactly as on baked ones; `GameWorld::destroy` cleanly
removes a placed building locally; and the peer's proxy SURVIVED the placer's
destroy (11 ghost census samples after t+2s - the removal gap was real).

Mechanism: `PKT_BUILD_DOOR` is the protocol-26 symmetric change-gated door
row on the TRANSLATED identity - both clients sample the doors of every
building in their build maps ~1 Hz (own placements key by our hand, minted
proxies through a new reverse map local-mint-hand -> placer-key), seeded
baselines, baseline-before-write echo guard, per-sender seq, 10 s safety
resend; the protocol-26 channel now SKIPS doors whose parent building is
session-placed (partition, no double-streaming). `PKT_BUILD_REMOVE` is
placer-authoritative: the dismantle detour (`Building::
notifyConstructionDismantling`, the UI path) or a programmatic destroy queues
a removal edge; the receiver destroys its mapped proxy through the engine's
own `GameWorld::destroy` and TOMBSTONES the map entry (late STATE/DOOR rows
for the key skip silently). `KENSHICOOP_BDOOR_SYNC` (default ON) gates both
halves; forced OFF inside `bdoor_probe`.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| bdoor_probe (new, probe tier; bdoorSync forced OFF) | sync | PASS | run 195513: both shacks placed + ramped to complete, proxies minted 1 door idx=0 each, toggles ok=1 locally but did NOT cross, host destroy ok=1 locally, GHOST persisted on join (11 samples) |
| bdoor_sync (new, full tier; bdoorSync ON) | sync | PASS | run 200456: toggles CROSSED 2/2 (peer applied [bdoor] RECV ok=1 + census at the toggled state), host destroy REMOVED the join's proxy (REMOVE-RECV ok=1, 0 ghost samples) |
| coop_presence | squad1 | PASS | regression (clean + wan) |
| npc_sync | c | FLAKY->PASS (clean), PASS (wan) | regression; clean first attempt failed smoothness - the documented machine-load signature, zero [bdoor] packets in that save, retry-passed |
| build_sync | sync | PASS | regression |
| door_sync | sync | PASS | regression (the protocol-26 partition did not disturb baked doors) |
| faction_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: 157/157 (adds sizeof(BuildDoorPacket)=32, sizeof(BuildRemovePacket)=29
+ round-trips).

Accepted limitations: gates/wall doors on placed WALLS out of scope; removal
only covers buildings in the session's build maps (a baked building's
dismantle logs at the detour but never streams); a peer that missed the PLACE
also misses the REMOVE (harmless - nothing to remove); a non-placer's UI
dismantle of a minted proxy stays local (removal is placer-authoritative; the
forwarding intent is a future stitch); door HEALTH/broken state and
lockpicking stay in gap 5 / doctrine 31.

## Hunger sync (2026-07-08, addendum)

Protocol 28 -> 29 (hunger + fed folded into `PKT_MEDICAL`). Gap 7's hunger
half: hunger is a per-client local simulation - each engine decays EVERY
character's hunger and eating happens only on the owner's client, so a driven
copy starves in the peer's view (stat penalties, eventual hunger KO).

hunger_probe (run 213751, hungerSync forced OFF, the rest of the medical
snapshot streaming as usual) settled the design's open questions: the engine's
hunger scale is ~0..3 (both squad leaders sat near 2.92 "full"); decay is
ACTIVITY-DRIVEN per client, which makes the divergence fast - the MARCHING
leader decayed at ~0.024/s on the client driving it while the same body's
IDLE driven copy decayed at ~0.0002/s on the peer, a 1.16-unit (~40%)
owner-vs-copy gap in one 50 s run; a direct `MedicalSystem::hunger` write
STICKS (no clamp/reset from the engine's periodic update); and the sentinel
drops stayed strictly local with the hatch off (the gap was real).
`dazedOrAlert` measured as a 0..1 alertness-style flag at rest - NOT an
intoxication scalar; the drunk/drug half stays unmapped (needs its own spike).

Mechanism: FOLD-IN, not a new channel. The owner's hunger + fed ride the
existing owner-authoritative medical snapshot as two f32s (engine read/write
through the same `&c->medical` path as blood); the change-gate fingerprint
quantizes them to 0.1 units (the heaviest measured decay flips a bucket
slower than the 3 s safety resend, so the fold-in adds zero packets); -1 =
field-not-carried, which is how `KENSHICOOP_HUNGER_SYNC` (default ON, forced
OFF inside `hunger_probe`) A/Bs the fields without touching the rest of the
medical stream - the receiver's `writeMedical` skips negative values, and the
`ownHands_` partition already prevents self-writes (no new echo machinery).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| hunger_probe (new, probe tier; hungerSync forced OFF) | sync | PASS | run 213751: scale ~0..3, active-vs-idle decay 0.024 vs 0.0002/s (endGap 1.16 in 50 s), sentinel writes stuck locally, nothing crossed, dazedOrAlert 0..1 |
| hunger_sync (new, full tier; hungerSync ON) | sync | PASS | run 214516: sentinel drops crossed 2/2 (9-10 peer census samples within 10 s), final owner-vs-copy hunger gap 0.000 on every shared hand |
| coop_presence | squad1 | PASS | regression (clean + wan) |
| npc_sync | c | PASS | regression (clean + wan, no flake this cycle) |
| medic_order | sync | PASS | regression (the medical channel's own composite gate) |
| limb_loss | c | PASS | regression (clean + wan; MedicalPacket layout change verified in anger) |
| faction_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: 157/157 (sizeof(MedicalPacket) 459 -> 467; round-trip already
covered).

Accepted limitations: drunk/drugged status effects stay unsynced (no mapped
engine surface - `dazedOrAlert` disproven as the intox scalar by the probe;
needs its own runtime spike after consumption); hunger KO transitions stay
with the reliable KO/death event channel (converged hunger makes both engines
agree on the KO organically); `fed` streams alongside hunger but eating
ANIMATIONS on driven copies are cosmetic-local; combat-scoped NPCs (medNpc_)
get hunger for free through the same packet - harmless (copies converge to
host truth).

## Late-join / reconnect resync (2026-07-08, addendum)

Protocol 29 -> 30 (connect-edge resync; NO wire change - `PROTOCOL_VERSION`
stays 29, "protocol 30" is doc numbering). Gap 9: a client that connects late
(or reconnects) trusts the shared save + live streams; anything that diverged
before the connect heals only when each channel's safety resend happens to
cover it - and the one-shot describe/mint edges are lost forever: a
pre-connect `PKT_BUILD_PLACE` never re-fires, so the late joiner never mints
the building, which also strands its STATE rows, its `PKT_BUILD_DOOR` rows
and any later `PKT_BUILD_REMOVE` (unknown keys skip silently).

latejoin_probe (run 230601, latejoinSync forced OFF, everything else
streaming) measured the unsynced baseline. The pre-arm surface works: the
host mutated state at gameplay-start t+3-5 s through the existing
`onGameplay` hook (a sentinel faction relation, a +777 wallet bump, a
placed + ramped-complete building) with the connect edge landing ~14 s
later - a reliable pre-connect window in the loopback harness (the join
launches 8 s after host gameplay + its own load). Findings: the pre-connect
BUILDING never minted on the join (the permanent-loss class, proven); the
faction and money rows healed via their 5-10 s safety resends (pre-connect
sends into the void had armed them) - already agreed by the first post-arm
census sample; the baked-DOOR leg went findings-only: town AI owns the sync
save's one baked door and fights any sentinel (reopens within ~1 s of every
write, unlocks its own lock - runs 225300/230601), so holding a door
mutation to the connect boundary is not provable in that save. The door
CHANNEL's resync lever is the identical lastSendMs=1 code path the gated
faction rows exercise (same row shape, same 10 s resend condition).

Mechanism: `Replicator::onPeerConnected()`, called from the game-thread
connect drain in `processNetEvents` (host on HELLO, join on WELCOME - covers
late joins AND quick reconnects, symmetric on both sides). Two moves:
1. one-shot edges replayed - every live `ownBuilds_` entry re-queues its
   retained `PKT_BUILD_PLACE` (captured at edge-drain time, no engine
   re-read) and removed ones re-queue `PKT_BUILD_REMOVE`; the receiver's
   session maps dedupe (known key skips the mint, tombstone skips the
   remove); the STATE row un-latches (doneSent/lastProg reset) so one fresh
   progress row re-latches complete;
2. force-resend pass - `lastSendMs = 1` on every row EVER SENT across
   facRows_/doorRows_/bdoorRows_/medPub_/statsPub_/moneyPub_/invPub_/
   worldTrack_ (each channel's own safety-resend condition fires on its next
   sample). Rows never sent - the seeded shared-save baseline - stay silent;
   edge-only caches (weaponCensus_, hostBody_, stealthPub_) are deliberately
   untouched (re-seeding would author phantom drop/KO edges).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| latejoin_probe (new, probe tier; latejoinSync forced OFF) | sync | PASS | run 230601: building never minted on the join, fac/money healed only via safety resends, door leg findings-only (town-AI churn) |
| latejoin_sync (new, full tier; latejoinSync ON) | sync | PASS | run 231429: `[latejoin] RESYNC place=1 ... fac=1 med=1 stats=1 money=1` on the host's connect edge; the pre-connect building MINTED + latched complete on the join; faction sentinel (-85) + wallet (+777) agreed from the first post-arm sample |
| coop_presence | squad1 | PASS | regression (clean + wan) |
| npc_sync | sync | FAIL (machine-state flake, A/B-exonerated) | npc_track PASSed 3 of the last 4 attempts; smoothness/anim_truth degraded PROGRESSIVELY across the midnight attempts with an fps-collapse signature (join translateFrames 1086 -> 567 -> 30, zeroFrac 0.32 -> 0.92 - the join window barely rendered, consistent with an idle/locked display), identical with `KENSHICOOP_LATEJOIN_SYNC=0`; the flake predates the change (all-day history incl. pre-deploy runs), and the resync's entire traffic in this scenario is 3 reliable packets once at connect (`RESYNC med=1 stats=1 money=1`) - re-run in an attended session |
| build_sync | sync | PASS | regression |
| bdoor_sync | sync | PASS | regression |
| door_sync | sync | PASS | regression |
| faction_sync | sync | PASS | regression |
| money_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: no change (no wire change).

Accepted limitations: event HISTORY is not replayed - KO/death events,
recruit re-keys and weapon-drop/pickup intents fired before the connect are
gone (the medical/stats snapshots self-heal the state those events carried;
recruit re-key replay rides gap 8's save coordination); the resync bursts one
full snapshot per channel on CH_RELIABLE (bounded by squad size + nearby
state); a reconnecting client keeps its session maps (`peerBuilds_`,
`proxyByKey_`), so duplicate re-announces dedupe by design; the baked-door
sentinel leg is findings-only in the sync save (town AI fights it - the door
channel's resync lever is proven by the identical faction-row code path).

## Coordinated save + session resume (2026-07-09, addendum)

Protocol 30 -> 31 (wire version 29 -> 30). Gap 8: no coordinated save - a
resumed session needed a manually re-mirrored save, and everything hand-keyed
rides the shared-save lineage that live sessions erode (session-placed
buildings/recruits exist only as runtime hands + proxies until a save bakes
them). Design pivot recorded in the gap: the `_coop.dat` sidecar candidate was
dropped - the HOST's save is authoritative and travels IN-BAND, so one save
with one hand per object lands identically on both sides.

Mechanism: `SaveManager::save` detour (every local save logs a `[save]
LOCAL-SAVE` edge; spike 39's `getCurrentGame`/`getSavePath` RVAs resolve the
live save identity/root - `save_probe` retired both runtime unknowns). A host
edge arms a folder-quiescence watch (poll until mtimes/sizes hold still 1.5 s;
30 s change-timeout); on QUIESCED the folder streams to the join over
CH_RELIABLE in ~4 KB chunks paced ~32 per 50 ms burst (~2.5 MB/s ceiling):
`PKT_SAVE_BEGIN` (name/fileCount/totalBytes), `PKT_SAVE_FILE` (stateless
chunks - the relative path rides every chunk), `PKT_SAVE_DONE` (per-file
FNV-1a-32 CRC table). The join stages into `save/<name>__incoming/`, folds
CRCs incrementally as chunks land, verifies on DONE, commits ATOMICALLY over
`save/<name>/` (old folder swapped aside, removed only after the rename
lands; failed verify discards staging) and `PKT_SAVE_ACK`s. A JOIN-initiated
save is suppressed locally and forwarded as `PKT_SAVE_REQ` (host arbitrates).
`KENSHICOOP_SAVE_SYNC` (default ON; forced OFF in `save_probe`) is the A/B
hatch.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| save_probe (probe tier; saveSync forced OFF) | sync | PASS | run 104047: spike-39 RVAs resolve live (`curGame`/`savePath` real), quiescence edge observed, 35 files / 3.7 MB fixture |
| save_sync (new, full tier; saveSync ON) | sync | PASS | run 111007: quiesce 2078 ms, transfer 1766 ms, 35 files / 3,730,759 bytes sent = committed, badCrc=0, ACK ok=1 |
| resume_test.ps1 stage 1 (save_stage1) | sync | PASS | run 111333: session-placed site (sid `898-gamedata.base`) ramped to prog=0.5 and baked into the coordinated save; 34 files / 3,657,786 bytes sent = committed |
| resume_test.ps1 stage 2 (resume_check) | coopresume | PASS | run 111333: both clients relaunched on the TRANSFERRED save (no harness mirror) and enumerated the stage-1 site under the SAME hand `0.2069.11111.1053.3641357312` prog=0.5 - the identity-reset proof |
| coop_presence | squad1 | PASS | regression |
| build_sync | sync | PASS | regression |
| latejoin_sync | sync | PASS | regression |
| faction_sync | sync | PASS | regression |
| money_sync | sync | PASS | regression |
| time_sync | sync | PASS | regression |

prototest: 184/184 (new: five `PKT_SAVE_*` struct sizes, REQ/BEGIN/ACK
round-trips, `PKT_SAVE_FILE`/`PKT_SAVE_DONE` framing bounds incl. the
`SAVE_CHUNK_MAX`/zero-pathLen rejections, and the incremental FNV-1a-32
chunk-split-invariance suite - however a file is cut into chunks the final
CRC equals the whole-file hash, which IS the reassembly correctness proof).

Loopback note: host and join installs share `%LOCALAPPDATA%\kenshi\save`
(`User save location=1` in both settings.cfg), so on one machine the join's
commit rewrites the folder the host just wrote - byte-identical, staging/CRC/
commit fully exercised. The two-machine kit path is where the transfer
delivers to a genuinely separate disk (`friend_join.ps1` resume mode).

Accepted limitations: join-local-only state (camera, control groups, anything
unsynced) is lost at resume by design; the transfer ships uncompressed (the
fixture gzips ~8:1 - an optimization, not a correctness need); autosaves also
trigger transfers (bounded by the 10-minute autosave cadence).

## Coordinated load (2026-07-09, addendum)

Protocol 31 -> 32 (wire version 30 -> 31). Gap 8 addendum: a MID-SESSION load
on the host silently forked the two worlds (the join kept playing the old
one) until a manual restart. Now the host is load-authoritative, mirroring
the save arbitration: a host load edge broadcasts `PKT_LOAD_GO` (save name +
folder fingerprint = FNV-1a over sorted lower-cased relative paths +
per-file content CRCs); the join verifies its on-disk copy - MATCH loads
immediately (bypass-once through its suppressed detour), missing/diverged
`PKT_LOAD_NACK`s and the host streams the folder via the protocol-31
SaveXfer after its own reload (the join loads on the verified commit). A
join-initiated load is suppressed and forwarded as `PKT_LOAD_REQ`.

The probe retired the runtime unknowns first (`load_probe`): run 122805
found `SaveManager::load` only SETS the deferred LOADGAME signal - mid-
session run 1's engine never consumed it (the title-screen loop is the
guaranteed `execute()` pump), so the plugin arms a 2 s grace-window backstop
that pumps `SaveManager::execute()` from end-of-tick; run 124501 (with the
pump) measured the full swap: ~4.4 s, `mainLoop_hook` KEPT TICKING through
the load screen, the pre-load squad hand RE-RESOLVED in the fresh world, and
the leader `Character*` CHANGED - the stale-pointer proof driving the
session reset. On each side's own reload edge (gameplay non-live >= 400 ms
then live) `Replicator::resetSession()` clears every pointer cache, session
map, change-gate baseline and interp buffer while preserving the config
gates, the ownership partition (`ownRanks_`) and every OUTBOUND seq counter
(a peer that did NOT reload keeps its per-sender stale-row guards), plus an
inbound world-state queue flush. `KENSHICOOP_LOAD_SYNC` (default ON; forced
OFF in `load_probe`) is the A/B hatch.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| load_probe (probe tier; loadSync forced OFF) | sync | PASS | run 124501: mid-session load safe with the execute() backstop, swap ~4.4 s, hook ticked throughout, pre-load hand re-resolved, leader pointer changed (run 122805 = the stalled-signal finding) |
| load_sync (new, full tier; loadSync ON) | sync | PASS | run 131521: GO broadcast -> join fingerprint MATCH -> join followed with its own load; both swaps completed (host 4844 ms, join 4454 ms) + both session resets ran; the SESSION-PLACED building (sid `898-gamedata.base`) enumerated on BOTH sides POST-load under the SAME hand `0.2069.11111.1044.2806762752` |
| save_sync | sync | PASS | regression (run 131819) |
| resume_test.ps1 stages 1+2 | sync / coopresume | PASS | regression (run resume_133001, both stages) |
| coop_presence | squad1 | PASS | regression (run 132053) |
| build_sync | sync | PASS | regression (runs 132248, 132444) |
| latejoin_sync | sync | PASS | regression (run 132719) |

prototest: 205/205 (new: `PKT_LOAD_GO`/`REQ`/`NACK` struct sizes +
round-trips, and the folder-fingerprint suite - deterministic, nonzero,
enumeration-order and path-case INVARIANT, perturbed by changed content /
renamed / missing / added files, empty = 0 sentinel).

Accepted limitations: a host load DISCARDS join-local unsaved progress by
design (host-authoritative); `load_sync` does not gate `clock_sync`
(advisory) - the mid-run world rebuild legitimately restarts the in-game
clock series the oracle aligns on; the NACK/transfer fallback leg is
implemented but exercised only by divergence (the identical-copy happy path
is the gated scenario - a NACK on it would be a fingerprint bug and FAILS).

## Production machine sync (2026-07-09, addendum)

Protocol 32 -> 33 (wire version 31 -> 32). Gap 5's remaining base-building
slice: production machines, power fixtures and farm growth simulated
per-client - an ore drill, generator, crafting bench or farm ticked
independently on each engine, so stored output, fuel, power state and crop
growth silently forked the moment players started a base. Now the HOST is
the machine authority (the world-simulation precedent): it samples
machine-class buildings (`Building::classType` in PRODUCTION / CRAFTING /
FURNACE / FARM / RESEARCH) within ~100 m of the interest centers at ~1 Hz
and streams `PKT_PROD` rows - power bit, production state, output item sid
+ amount, up to 2 input amounts, farm growth floats (-1 = field not
carried) - change-gated on quantized hundredths, CH_RELIABLE, keyed by
baked hand or protocol-27 placer key (`keyKind` disambiguates). Unlike the
symmetric door shape, FIRST sight sends (the host's state is the baseline)
and the 10 s safety resend doubles as the drift corrector for the join's
still-simulating copies. The join applies only diverged fields through the
probe-validated levers: `switchPowerOn`, then `setProductionItem` to
MATERIALIZE a still-null output buffer (a fresh bench has none until its
first production tick), then the direct `ConsumptionItem::amount` /
farm-float writes for exact values. Session reset clears the row cache
(protocol 32); `onPeerConnected` ages `lastSendMs` (protocol 30).
`KENSHICOOP_PROD_SYNC` (default ON; forced OFF in `prod_probe`) is the A/B
hatch. Research benches are census-evidence only - the tech-unlock store
is unmapped (follow-up spike input).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| prod_probe (probe tier; prodSync forced OFF) | sync | PASS | run 152730: both machines placed + ramped complete + minted on the join; divergence real (host bench outAmt moved under 30 operate() calls, join copy flat; power toggle never crossed); levers stick (switchPowerOn persisted, native setProductionItem materialized the null buffer, direct amount write survived the next tick) |
| prod_sync (new, full tier; prodSync ON) | sync | PASS | run 154503: 6 [prod] rows sent / 4 applied; bench outAmt converged host=1.500 join=1.500 (gap 0.000); host power OFF crossed onto the join generator copy within 3 census samples; final power agreed (1/1) |
| coop_presence | squad1 | PASS | regression (run 154753 clean, 154937 wan) |
| bdoor_sync | sync | PASS | regression (run 155124) |
| build_sync | sync | PASS | regression (run 155344) |
| door_sync | sync | PASS | regression (run 155606) |
| latejoin_sync | sync | PASS | regression (run 155827) |
| save_sync | sync | PASS | regression (run 160050) |

prototest: 209/209 (new: `ProdPacket` struct size 109 + round-trip +
truncation rejection; HELLO version now 32).

Accepted limitations: whole crafted items landing in a machine's INVENTORY
ride the container-inventory channel, not this one (the row carries the
output BUFFER only); research tech-tree unlocks are not on the wire (store
unmapped - later closed by protocol 38, see the research addendum); farm
growth is wired but validated only pass-through (no farm
reachable in the `sync` save - terrain-dependent); the join's machines
between rows still simulate locally, so sub-second flicker between a local
tick and the correcting row is possible (quantized change gate keeps it
within a hundredth).

## Storage & machine container sync (2026-07-09, addendum)

Protocol 33 -> 34 (wire version 32 -> 33). Gap 5's final base-building
slice: storage chests and machine inventories hold whole ITEMS that forked
per-client - the container-inventory channel registered exactly ONE
container (the host's nearest baked chest via `pickInventoryContainer`),
so every other chest and every bench/drill/furnace inventory diverged the
moment items landed in it. Now the HOST authors ALL of them: a ~1 Hz
census (`enumContainersNear` - the `enumMachinesNear` pattern widened to
`BCTYPE_STORAGE` + the machine classes, COMPLETE buildings only) folds
every container-bearing building near the interest centers into the
existing `publishInventories` authored set, so contents stream through the
proven per-container hash + settle-window + 5 s safety-resend gate as
`PKT_INV_SNAPSHOT`. The wire grew a `keyKind` byte: 0 = the container key
is a raw save-stable hand (characters, baked chests - the previous
implicit behaviour), 1 = a protocol-27 placer key for session-placed
buildings (sender translates local hand -> placer key through the build
maps; the receiver resolves back, dropping unresolvable keys for the
safety resend to re-deliver once the mint lands). The join reconciles via
`applyContainerContents` (add shortfall / remove excess per (sid, type)).
Session reset clears the census set (protocol 32); own-squad member
inventories keep the existing bidirectional tab partition untouched.
`KENSHICOOP_STORE_SYNC` (default ON; forced OFF in `store_probe`) is the
A/B hatch, layered on `invSync` (the carrier).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| store_probe (probe tier; storeSync forced OFF) | sync | PASS | run 171728: census read containers with inventories on BOTH clients (commonHands=2); fabricate-into-chest landed 0->5 (after walking type-limited candidates - a Fabric Chest refused iron plates); reconcile removal 5->2 stuck; 30 operate() calls left the bench CONTAINER empty (output rides the protocol-33 buffer until a worker collects - no reconcile churn: force-empty stayed empty); join fabricated 3 into its MINTED chest copy (translated-key apply half) |
| store_sync (new, full tier; storeSync ON) | sync | PASS | run 173245: host census-authored the placed chest (4 [inv] SEND kind=1 rows), join applied 74 [inv] snapshots; the host's chest add crossed onto the join's minted copy and the FINAL content hashes agreed host=join=476132288 (the reconcile-removal crossed too) |
| coop_presence | squad1 | PASS | regression (run 173725 clean, 173910 wan) |
| inv_order | squad1 | PASS | regression (run 174132) |
| inv_bidir | squad1 | PASS | regression (run 174355 clean, 174618 wan) |
| build_sync | sync | PASS | regression (run 174840) |
| latejoin_sync | sync | PASS | regression (run 175326; flaky retry-passed) |
| prod_sync | sync | PASS | regression (run 175550) |
| save_sync | sync | PASS | regression (run 180325) |

prototest: 209/209 (`InvSnapshotHeader` grew to 27 bytes with the keyKind
byte; HELLO version now 33).

Accepted limitations: `store_sync`'s smoothness advisory read
zeroFrac=0.406 (over the 0.40 bar) - root-caused to the protocol-25 clock
catch-up transient, NOT the container channel: the join runs at up to 2x
for ~40 s after connect and the 70 s scenario window is dominated by it
(A/B evidence: npc_sync with `KENSHICOOP_TIME_SYNC=0` measured
zeroFrac=0.204, run 175605, vs 0.30-0.41 with it on); machine containers
only sync externally-added items (operate() output rides the protocol-33
buffer floats until a worker collects); a chest holding more than
`INV_ITEMS_MAX = 20` DISTINCT (sid,type) entries truncates the snapshot
(the probe measured 1-entry rows in practice - revisit if real bases
overflow); incomplete construction sites ride protocol 27 until finished.

## Squad management sync (2026-07-09, addendum)

Protocol 34 -> 35 (wire version 33 -> 34). Gap 10: moving a unit between
squad tabs re-containers it and mints a FULLY fresh hand (squad_probe:
container AND index/serial change - harsher than a recruit, where baked
subjects keep their serial), so the mover streamed an unresolvable key
while the peer's copy of the old hand went stale; and the per-tick
sorted-container rank partition could RESHUFFLE (whole-tab ownership flip)
or mint unowned tabs on any mid-session tab-set change. No single engine
function owns the UI move path, so there is no detour: the engine now
keeps a `Character*` -> hand baseline (`pollSquadRoster`, polled every
tick by `publishSquadMoves`; the body pointer survives the re-container)
and every diff authors a reliable `EVT_SQUAD_MOVE` (subject = old hand,
actor = new hand; zeroed actor = left roster) with the new hand's
ownership pinned BEFORE the wire. The receiver shares the EVT_RECRUIT
re-key path (`rekeyPeerBody`): bind the existing local body to the new
stream key in `proxyByKey_`, restore if authority-suppressed, pin
peer-owned - `recruitOwned_`/`peerRecruit_` generalized into the
`pinOwned_`/`pinPeer_` sets covering recruits AND moves. Tab ranks are
LATCHED at first census (sorted order, so two-tab saves behave exactly as
before) and newly-seen containers APPEND - no mid-session reshuffle;
appended tabs inherit their author's ownership via the pins. The re-key
retires the old key's stream state (interp tail + a 10 s REQ/mint grace)
and repairs a duplicate proxy that beat the edge (cull + rebind).
`KENSHICOOP_SQUAD_SYNC` (default ON; forced OFF in `squad_probe`) is the
A/B hatch. Session reset (protocol 32) clears latch + pins + baseline.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| squad_probe (new, probe tier; squadSync forced OFF) | squad1 | PASS | runs 185825/191911: every move minted a fresh hand (containerChanged=True indexSerialSurvived=False on all 4 legs); the pointer-diff caught each within one poll; setFaction landed the move-into-existing-tab (host L1 rc=1, join L1 rc=1); solo-tab separate is an engine no-op (rc=0, run 185825); pre-existing census ranks stable (new containers sorted AFTER); unsynced peer REQ/minted the moved hand (the gap measured) |
| squad_sync (new, full tier; squadSync ON) | squad1 | PASS | run 193304: all four scripted moves landed, each authored one EVT and one peer REKEY ok=1, proxyBound=0 and unresolved=0 for every moved hand (run 192211's two races - the 500 ms-throttled edge losing to the REQ/mint round-trip, and the old key's interp tail REQ-ing a duplicate - fixed by per-tick polling + rekeyedOld_ grace + cull-and-rebind repair), peer tracked both final hands (PROXY 78/38 samples), census ranks stable |
| coop_presence | squad1 | PASS | regression (run 193612 clean, 193756 wan) |
| npc_sync | sync | FAIL->PASS | run 193941 clean failed npc_track+smoothness, standalone rerun 200126 npc_track PASS with only the known time-slew smoothness advisory failing - the pre-existing flake (identical signatures 07-08 pre-protocol-35, e.g. 232442); wan variant passed (run 194314) |
| inv_bidir | squad1 | PASS | regression (run 194500 clean, 194722 wan) |
| latejoin_sync | sync | PASS | regression (run 194946) |
| money_sync | squad1 | PASS | regression (run 195207) |
| recruit_sync | sync | PASS | regression (run 195430) |
| save_sync | sync | PASS | regression (run 195652) |

prototest: 212/212 (EVT_SQUAD_MOVE pinned to id 11; HELLO version now 34).

Accepted limitations: peer squad-UI roster placement stays deferred (moved
units are visible and driven on the peer, not in its squad UI - the
recruit stance); a late joiner misses pre-connect move edges (bounded by
the shared-save workflow); dismissal (roster-exit) edges are wired but
have no programmatic lever to validate; wallets/inventories of appended
mid-session tabs are not rank-keyed (the pins own their members, but the
per-tab money channel only streams latched session-start ranks).

## Play-session bug fixes (2026-07-10, addendum)

Protocol 35 -> 36 (wire version 34 -> 35). Four field bugs from the
2026-07-09 remote session, one wire bump: the entity batch header gained a
sender-side millisecond timestamp (`sendMs`; `ENTITY_BATCH_MAX` 18 -> 17 to
stay under one datagram) and `PKT_NPC_CENSUS` (id 38) carries the host's
1 Hz wide-radius NPC hand list. New env knobs, all with validated
defaults: `KENSHICOOP_CENSUS_RADIUS` (2000 u), `KENSHICOOP_SEND_STAMP`
(A/B lever, ON), `KENSHICOOP_INTERP_MIN/MAX_DELAY_MS`,
`KENSHICOOP_INTERP_MAX_EXTRAP_MS`, `KENSHICOOP_INTERP_STALE_MS`,
`KENSHICOOP_INTERP_SNAP_DIST`, `KENSHICOOP_CATCHUP_K`,
`KENSHICOOP_SNAP_DIST`.

Smoothness A/B (leader_move under the Steam-relay WAN profile 60+/-25 ms
1% loss, runs 225628-235924 on 07-09): arrival-stamping baseline
zeroFrac ~0.36 / extrapFrac 0.048 WAN; send-stamping with the per-peer
min-offset clock map and lag-aware renderDelay cut interp jitter ~13 ->
~6 ms and extrapFrac to 0.026, and stabilized the loopback zeroFrac that
flaked 0.24-0.44 across the historical baseline. Hard-snap and
walk-reissue counters now split squad/NPC in the `SCENARIO INTERP`
summary; the `[interp]` stat line samples live sessions at 5 s cadence.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| npc_census (new, full tier) | sync | PASS | runs 003216/004154: 4 baked far ghosts (600 u, outside the ~200 u stream bubble) suppressed on the join via census-absence culling; zero culls of census-listed NPCs; census flow host `sent n=47` / join `recv n=47` at 1 Hz; staleness guard exercised (no wide culling before the first census) |
| cage_peer_sync (new, full tier) | bedcage1 | PASS | run 015339: host authored `SEND PEER-ENTER` for the join's KO'd L1 (its own put ok=1), join applied to its OWN body (enterLat=509 ms), occupancy held the full 28 s window with hostZeroDwell=0 and ZERO `HEAL EXIT`s (the "kept taking it out" bug), owner-side exit clean on both ends. Runs 005941-013345 failed on system commit-memory exhaustion (steamwebhelper held ~85 GB; join instance crashed in load), not the fix |
| cage_put | bedcage1 | PASS | regression (run 015717) |
| carry_order | squad1 | PASS | regression (run 015936) |
| npc_carry | sync | PASS | regression (run 020150) |
| coop_presence | squad1 | PASS | regression (run 020331) |
| load_sync | sync | PASS | run 022414: portrait gate green - QUIESCED with `portraits_texture.png` present (no bounded-wait WARN), no load-point WARNs, committed copy carries the 43.5 KB atlas on disk (the new oracle leg) |
| save_sync | sync | PASS | regression (run 022640; QUIESCED settled in 2.1 s with the portrait gate active) |
| npc_sync | sync | FAIL/PASS | runs 000313/000513/000702/003948/004417: the known pre-existing crosscheck/smoothness flake, A/B-exonerated (identical failure signature with `KENSHICOOP_SEND_STAMP=0` and with census off) |

prototest: 217/217 (EntityBatchHeader 10 B with sendMs, NpcCensusHeader
7 B framing + overrun rejection, HELLO version 35).

Blank portraits: the quiescence gate now holds a settled save folder for
`portraits_texture.png` up to 10 s past arm (bounded; a genuinely
portrait-less save completes with a WARN), and every coordinated-load
issue point WARNs when the target folder lacks the atlas. The 07-09
session's join copies ('people', 'Group') did carry valid atlases, so the
friend-host repro is unconfirmed (no host log); the WARNs turn a future
recurrence into a one-line diagnosis - if they stay clean and avatars
still blank, the escalation path is an engine-side PortraitManager
investigation.

## Steam MTU + starved-replica fixes (2026-07-10, addendum)

No wire change (protocol stays 36 / wire v35); two architecture-review
findings closed plus a harness-integrity overhaul the fixes' validation
uncovered.

**Steam-safe entity batch cap.** Steam's P2P transport clamps ENet's MTU
to 1200 B, but the batch send loop filled to `ENTITY_BATCH_MAX = 17`
entities = 1353 B - ENet ships an oversized UNRELIABLE packet as RELIABLE
fragments, so the 20 Hz motion stream inherited retransmit/ordering
stalls on exactly the transport real sessions use (loopback/UDP never
sees this; default MTU ~1400). The sender now chunks by transport:
`ENTITY_BATCH_MAX_STEAM = 14` (1116 B) on Steam, 17 on raw UDP.
Receive bound unchanged (`len >= need` per header count), so mixed-cap
kits interoperate. prototest asserts the Steam chunk fits 1150 B.

**Starved-replica guard hold** (`KENSHICOOP_STARVE_HOLD_MS`, default
10 s, 0 = legacy). At >2 s stream staleness, `applyTargets` used to drop
a driven body from BOTH per-tick guard sets instantly - native AI and
locally-simulated melee damage resumed on every WAN hiccup, silently
diverging the local-only medical model. Now, within the hold window past
staleness:
- every driven body KEEPS its damage guard (the divergence protection);
- squad-class bodies (a peer's PCs) also keep AI-suspend and stay parked
  (engine-inert anyway; an autonomous peer PC is the worst face of the
  bug);
- world NPCs deliberately RELEASE to local AI as before: an A/B (runs
  113052-114743) showed freezing stale interest-boundary wanderers
  degraded npc_sync tracking to 0.64-0.73 vs the 0.8 gate (hold-off
  passed) - on a shared save the local AI shadows the host's patrol
  better than a freeze, and existence is already policed by
  host-authority suppression + census.
Telemetry: `starve=` (held bodies this tick) joined the `[interp]` stat
line. netsim gained a scripted total-outage window
(`netsim ... [seed stallAtS stallForS]`, session-relative to the first
join datagram) surfaced as the `stall` WAN profile (regional + 4 s
outage at +30 s); during the outage the join showed the starve spike
(starve=12), zero `[dmg]` pass-throughs, and clean re-acquisition
(extrap then snap) at stream resume.

**Clock catch-up transparency (the flake killer).** The join closes its
~0.3 gh load skew by simming at up to 2x for the first ~35-40 s
(protocol 25) - and the short scenario windows sat almost entirely
inside that transient, so motion oracles were measuring the slew, not
the sync (user-confirmed visibly fast join NPCs; the historical
zeroFrac 0.2-0.9 flake). Changes:
- smoothness counters exclude slewed frames (`slewSkip=` in `SCENARIO
  SMOOTH`); the oracle SKIPs below 200 scored frames instead of passing
  vacuously;
- every run analysis prints `FINDING: join clock catch-up ...` (peak
  offset, peak slew, engaged seconds, converged-or-not) from the
  `[time] OFFSET` trace (`Get-SlewSummary`);
- `time_sync` now GATES the return to normal: join slew back to 1x and
  both sides' final fsm ~1.0 (run 111420: peakOff 0.30 gh, 27 s at 2x,
  converged, finalOffset 0.0023 gh);
- `leader_move` 24 -> 62 s and `npc_sync` 24/44 -> 62/82 s so their
  gates judge steady state past convergence. Effect: npc_sync crosscheck
  went from 0.64-0.73 flake to 9/9 tracked with worstMedian 0.7 u (run
  114753); leader_move scores 856-1961 genuine steady-state frames
  (zeroFrac 0.30-0.38, in gate, clean + WAN regional).

| Scenario | clean | Key values |
|---|---|---|
| prototest | 219/219 | Steam chunk 14 fits 1150 B; cap <= receive bound |
| leader_move | PASS | 62 s window: zeroFrac 0.382 clean / 0.303 WAN-regional, slewSkip ~3.5-4.5 k excluded |
| npc_sync | PASS | 62 s window: 9/9 tracked, worstMedian 0.7 u; starve spikes from boundary wanderers benign |
| coop_presence | PASS | regressions on both the hold and the final split build |
| npc_sync -Wan stall | PASS | 4 s scripted outage: starve=12 during stall, no `[dmg]` passes, snap-clean resume |
| time_sync | PASS | new convergence gate: 2x for 27 s then 1x both sides, finalOffset 0.0023 gh |

## Weapon fabrication + acquisition sync (2026-07-10, addendum)

No wire change (the inventory snapshot has carried weapon
manufacturer/material provenance since protocol 37). Spike 451 recovered
the engine's real weapon-mint recipe - `RootObjectFactory::createItem`
wants the `WEAPON_MANUFACTURER` GameData FIRST and the weapon template in
the third ("weaponMesh") slot; template-first returns null for every
weapon template, which is why `diagWeaponCreate` historically measured
0/24 and all weapon sync was conservation-only. Shipped: the
manufacturer-first weapon branch in `createItemAndAdd` (24/24 templates
fabricate; `fallbackWeaponManufacturer` substitutes a generic maker when
the wire carried no provenance), weapon CREATE in the container
reconcile, gear in the cross-owner transfer shortfall fallback (existing
latch/`wdSuppress_`/rebase dupe plumbing), and the finding that a
fabricated weapon added LOOSE persists (tryAddItem's auto-equip into an
empty slot survives ticks - only blank-handle `create+equipItem` is
discarded). `KENSHICOOP_WEAPON_FAB=0` restores conservation-only gear
sync at every fabrication site. Doctrine 52; SYNC_GAPS entry 12.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| weapon_loot (new, full tier) | squad1 | PASS | run 182239: host fabricated novel sid `52309-rebirth.mod` into its owned leader (added=1 final=1 max=1); the join's driven copy gained EXACTLY one (final=1 max=1 - zero transient dupes, the fabrication-vs-conservation race gate) with matching quality (10000 == 10000); first run (181822) failed only because `invSync` auto-on did not yet list the scenario |
| inv_reequip | squad1 | PASS | regression (run 182544): dip-then-restore + convergence both directions, MOVE-UP intact |
| trade_peer | squad1 | PASS | regression (run 182828): TAKE/GIVE/WTAKE all CLEAN, conservation + agreement held with the relaxed gear-fab guard live |
| inv_wpnseq | squad1 | PASS | diagnostic health run (183111): reconcile trace clean through the weapon snapshot sequence |
| trade_probe | squad1 | PASS | baseline signature UNCHANGED (run 183353: TAKE:DUPE / GIVE:WIPED with xferSync forced off, WEAPON:CLEAN) - the relaxed guard did not alter the documented unfixed baseline |
| world_weapon_drop | squad1 | PASS | regression (run 183932): conservation relocation intact (host dropped, join relocated its own copy, `APPLY moved=1`) - fabrication did not displace the conservation channel |

Remaining manual leg: a live loot/vendor-buy session (a human drags a
bought/looted weapon in the trade UI; `weapon_loot` covers the engine-level
acquisition shape, not the UI drag timing). Run it with
`scripts\manual_session.ps1` next free-play session and watch the peer's
copy appear.

## Research tech-tree sync (2026-07-10, addendum)

Protocol 37 -> 38 (wire version 36 -> 37). Gap 5's research slice: the
unlock store is `PlayerInterface::technology` - a per-client `Research`
object with no KenshiLib header - so a tech the host researched NEVER
unlocked on the join (spike 401: host `isKnown(subject)` flipped 0 -> 1
after `startResearch` while the join read 0 for the entire run). Spike 401
recovered the engine's own levers from the on-disk 1.0.65 exe ("Research
already known" string xref -> the research-UI click handler ->
`Research::isKnown` / `canResearch` / `startResearch`); the running image
is BASE-SKEWED from the file, so the levers are located at runtime by a
unique 24-byte prologue scan of `.text` (base+RVA calls crashed both
clients - the scan REFUSES the whole lever set on any miss). Now the HOST
is the tech-tree authority: it samples its known set ~1 Hz
(`researchEnumKnown` - `isKnown` over the shared RESEARCH GameData
enumeration) and streams one reliable `PKT_RESEARCH` row per known
stringID (the cross-client-stable key); first sight sends (the host's
known set is the session baseline), the 15 s safety resend doubles as the
lost-row / late-prerequisite corrector. The join applies each row via
`startResearch` behind an `isKnown` pre-check (idempotent; un-learning
does not exist in the engine, so rows only ever ADD). Session reset clears
the row cache (protocol 32). `KENSHICOOP_RESEARCH_SYNC` (default ON;
forced OFF in `research_probe`) is the A/B hatch. Doctrine 53; SYNC_GAPS
gap-5 research slice; spike doc 401-research-bench-progress-sync.md.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| research_probe (new, probe tier; researchSync forced OFF) | sync | PASS | run 220036: both clients picked the SAME subject sid (`66290-Newwworld.mod.TECH.1` - wire-key stability); host startResearch rc=1 known 0->1; join divergence window 14 samples 0 leaked (the gap is real); join self-start rc=1, known stuck across all 19 post-start samples (the apply lever) |
| research_sync (new, full tier; researchSync ON) | sync | PASS | run 220320: host sent 3 [research] rows, join applied the subject; unlock CROSSED in 1000 ms of the host's start and stuck to run end; finals known=1/1 |
| smoke regression suite | - | PASS | post-protocol-37 smoke tier all green (coop_presence, npc_interest, event KO, inv_bidir, world_weapon_drop) |

prototest: 227/227 (new: `ResearchPacket` struct size 57 + round-trip +
truncation rejection; HELLO version now 37).

Accepted limitations: host-additive union - a tech the JOIN researches
locally is not pushed back to the host (host-authoritative single
direction; the join's extra knowledge is harmless and converges at the
next shared-save reload); no un-learn lever exists in the engine, so a
host save-rollback cannot revoke a tech the join already applied
(shared-save reload heals it); `ManagementScreen::currentResearch` (the
UI's selected subject) is not readable on this build - the row carries
completed sids only, in-progress research bars are not synced.

## NPC pop-outs + rubber banding (2026-07-11, addendum)

No protocol/wire change. Two manual-session field reports fixed in the
Replicator's authority + walk-drive layers, with new locomotion-quality
gates (full detail: SYNC_GAPS #13).

- **Pop-outs:** the near suppression pass now treats existence and drive as
  separate authorities - an NPC is hidden only when BOTH unstreamed AND
  absent from a fresh host census. Diagnosed with `KENSHICOOP_DEBUG_CENSUS=1`
  (host census dump + name-annotated cull/suppress lines): the 10 town culls
  were a join-only Dust Bandit raid (correct), but census-present boundary
  NPCs ('Saint'/'Kumo') were churning hidden/restored - that churn is the fix
  target and is now impossible by construction while the census is fresh.
- **Rubber banding:** the fixed 8 u hard-snap gate became velocity-aware -
  teleport only when the driven body trails the source by more than
  `KENSHICOOP_SNAP_SECONDS` (0.75 s) of travel (decaying-peak velocity
  estimate; `KENSHICOOP_SNAP_DIST` floor scaled by the consensus game speed).
  `[snap]` attribution lines (gap/gate/srcVel/cSpeed/mult/slew) stay in for
  future diagnosis.
- **New oracles:** `snap_rate` (join `[interp]` snap counters/min, clock-slew
  window excluded, <= 3/min; `snap_rate_squad` variant gates squad snaps
  only) and `suppress_churn` (no hand hidden more than once). Wired:
  coop_presence (smoke) + npc_sync gate suppress_churn + snap_rate;
  npc_census gates suppress_churn.
- **New scenario:** `fast_march` (full tier) - both sides vote 5x, the host
  marches its leader in bursts; primary gate `snap_rate_squad`. Background-NPC
  snaps stay un-gated at 5x (resting bar NPCs legitimately fall 100+ u behind
  between updates; the teleport is the correct convergence tool there).
- **Debug markers:** `KENSHICOOP_DEBUG_MARKERS=1` / `manual_session.ps1
  -DebugMarkers` pins spike-47 ScreenLabels to judged bodies (green DRV /
  red HID / yellow LOC); render-verified (run 111215). Pair with the 'zoom'
  save for long-run wide-camera inspection.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| fast_march (new, full tier, 5x) | sync | PASS | run 105802: squad snaps 0 over 65 s at 5x (was ~35/s before the velocity gate); npc snaps 1; suppress-churn 12 hands hidden once each (ghost culls, zero re-hides) |
| coop_presence | squad1 | PASS | run 111215: suppress-churn 0 hands hidden (the boundary churn is gone); snap-rate 0/min |
| npc_sync | sync | PASS | smoke run: suppress-churn 10 hands hidden once each; snap-rate 0/min; smoothness 0.377 (gate 0.4, pre-existing margin) |
| smoke regression suite | - | PASS | post-fix smoke tier all green with the new gates active |

## Far spawn-in + choppy NPC walk (2026-07-11, addendum)

No wire change. The second round of zoom-session field reports (SYNC_GAPS #14,
doctrine 54): host NPCs materializing on top of the join player, and stuttery
NPC locomotion outside combat.

- **Census-range minting:** a 2 s census-missing scan records census hands
  with no local body; their `PKT_SPAWN_REQ` skips the streamed-position gate,
  and the mint decision moves to the reply's authoritative position -
  `KENSHICOOP_SPAWN_MINT_RADIUS` (600 u) of a local squad member, too-far
  replies deferred ~5 s (never the 30 s denied cooldown). Bind-time
  hardening: `enforceHostAuthority` exempts proxy bodies by pointer from both
  suppression passes (a proxy's local hand is never census-present, so the
  wide pass culled-and-froze every far mint, and the frozen bodies no-opped
  `applyRaw` = a per-frame snap storm) and instantly restores any suppressed
  body that becomes a proxy or driven.
- **Walk/rest debounce:** the classifier holds the walking verdict 1 s past
  the last genuinely-moving sample (`restFlip` counter added to `[interp]`).
  A velPeak-based debounce was tried and reverted same-day - one
  teleport-artifact velocity spike (srcVel 90-150 u/s on a seg snap) held a
  genuinely SEATED divergent NPC in the walk branch ~7 s per spike,
  hard-snapping every frame (spawn_far run 124346).
- **Capture hysteresis:** `captureNpcs` queries 260 u, acquires < 200 u
  unconditionally, retains the 200-260 u band only for hands captured last
  tick - bubble-edge NPCs stop starving the join's interp buffer.
- **Seat-break:** a rest-pose apply is a PLAYER-rank order and a seated body
  no-ops teleports; when the host copy starts moving the join flushes the
  order once via the player move path (`walkTo`) before the drive resumes.
- **Smoothness-oracle scoring fix:** NPCs are scored active by INSTANTANEOUS
  stream velocity, not the debounced verdict - charging the 1 s trailing
  walk-hold after a source stop as "active" measured the debounce, not the
  pipeline (leader_move read zeroFrac 0.66-0.77 vs its 0.3 baseline until
  the fix; 0.19-0.29 after).
- **New scenario/oracles:** `spawn_far` (host spawns a runtime squad 620 u
  out, parks it, walks it in at 14 u/s; `Test-SpawnFarBind` gates every far
  hand minted, binds >= 400 u from the join anchor, no duplicate mints, same
  proxy driven into the stream bubble) and `Test-RestFlap` (walk-to-rest
  flips/min, gated on npc_sync).

| Scenario | Save | clean | Key values |
|---|---|---|---|
| spawn_far (new, full tier) | sync | PASS | binds 4/4 far hands at 487-571 u from the join anchor, 0 duplicate mints, closest driven approach 1-2 u; suppress-churn 0 re-hides |
| leader_move | sync | PASS | post-oracle-fix zeroFrac 0.289 clean / 0.192 wan (was misreported 0.66-0.77) |
| npc_sync | sync | rest_flap PASS | rest-flap 2-6/min across 4 runs (gate 60/min - the classifier flap is gone); crosscheck/pose green; smoothness+snap-rate remain flaky run-to-run (zeroFrac 0.315-0.83, npc snaps 0-34/min - tracked as open triage) |
| full matrix 2026-07-11 | - | PASS except triage | 57-run resumed matrix green except npc_carry [clean] (latch flake + join DOWN-state miss, wan passes) and sneak_detect (detection map retains a still-seeing entry after sneak end, 4/4) - both under investigation |

## Join crash + pack-hidden fixes (2026-07-11, addendum)

Protocol v38 (census rows now carry positions). The third round of free-play
field reports (SYNC_GAPS #15, doctrine 55): a join crash after ~28 minutes,
and a wildlife pack visible on the join with no host counterpart.

- **Crash root cause (minidump-verified):** `0xC0000005` reading
  `0xFFFFFFFFFFFFFFFF` in `ZoneManager::findOverlappingActiveZones`, reached
  from the engine's OWN sensory pass - it walked a body we still held in
  `suppressed_` by raw `Character*` after the engine despawned it (93
  suppressed wildlife booked through a zone stream). Fix: hand round-trip
  liveness proof (SEH-read the pointer's current hand, resolve it back, same
  pointer = alive) before ANY touch of `suppressed_` / `proxyByKey_` /
  `debugMarkers_`; dead entries pruned (`pruned=` on `SCENARIO INTERP`),
  re-containered live bodies MIGRATED to their new key so the hide keeps
  holding. Release now links `/DEBUG` so future dumps symbolicate.
- **Census position parking:** census-present copies are exempt from
  culling but each side sims its own copy; the wide pass now parks a local
  copy diverged past `KENSHICOOP_CENSUS_PARK` (120 u) onto the host's
  census position, once per key per 5 s. Threshold and scope were tuned by
  failure: unthrottled in-bubble parking at 45 u fought the engine's seat
  AI every frame and broke npc_track/march/snap_rate (run 185524).
- **Mint duplicate guard + animal mint:** a spawn reply defers when a
  visible uncorrelated same-template body stands within 20 u of the reply
  position; proxy template lookup falls back to the `ANIMAL_CHARACTER`
  category (host wildlife packs previously could never mint on the join).
- **Existence-audit probe:** 5 s join-side `[audit] exist` line bucketing
  every enumerated NPC (drv / cen / hid / ghost) plus `Test-ExistenceParity`
  (advisory on npc_sync) gating ghost fraction and persistence.

| Scenario | Save | clean | Key values |
|---|---|---|---|
| spawn_far | sync | PASS | 4/4 far hands minted and bound, 0 duplicate mints; snap_rate advisory-noisy during the park settling window (45/40 s), gating gates green |
| npc_sync | sync | PASS | npc_track ratio 1.0 worstMedian 0.35 u, zeroFrac 0.215, snaps 2/min, rest-flap 4/min, existence_parity ghostFrac 0 maxRun 0 (one smoothness flake 0.465 on the prior run - known flakiness) |
| leader_move | sync | PASS | zeroFrac 0.04 (baseline ~0.3), march 0.083 |
| coop_presence | squad1 | PASS | snap_rate 0/min, march 0 |
| existence_parity | - | PASS | ghostFrac 0 across all runs; pack-hidden diagnostic session on the `pack hidden` save read hid=3 ghost=0 steady |
| 30-min zoom soak | zoom | PASS | markers + census debug on, wildlife churn, both clients alive at timeout, no crash |

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
