# Spike 51 - spawnHostileBattle(perSide) helper + 10v10 baseline sync

- Type: RUN
- Status: PARTIAL
- Save: c
- Branch commit: <filled at commit>

## Goal

Build the reusable `spawnHostileBattle(perSide, red, blue)` engine primitive that
themes 1-2 (combat load/sync, spikes 52-80) depend on, and establish a 10v10
baseline: prove the helper spawns the bodies, drives a battle, and characterise
what the host and the *join* actually see. This is the load/sync foundation, not a
lethality test.

## Method

Generalised the proven two-duelist scene (`setupDuelScene`/`startDuel`) to N-per-side
clusters. New engine surface (`Engine.h`/`Engine.cpp`):

- `int spawnHostileBattle(GameWorld*, int perSide)` - spawns `perSide` "red" NPCs in a
  front-LEFT arc and `perSide` "blue" NPCs in a front-RIGHT arc via the existing
  `spawnCharInFaction` (nearby non-player faction; `spawnNpcInFront` player fallback),
  lane/depth-staggered so bodies don't telefrag into one stack. Each is detached from
  town AI (`detachFromTownAI`) for a stable hand, its hand stashed in `g_battleRed[]`/
  `g_battleBlue[]`, then issued an `UNPROVOKED_FOCUSED_MELEE_ATTACK` order (the same
  faction-relation-independent order the duel uses) at the index-paired enemy.
- `int rearmBattle(GameWorld*)` - re-issues an attack to any pinned combatant no longer
  `inCombat`, retargeting it at its nearest surviving enemy (keeps the blob engaged).
- `int battleCensus(GameWorld*, BattleCensus*)` - per-side counts of resolvable (alive),
  `inCombat` (fighting), and wounded (`medical.blood < 50`) units.
- `SpikeScenario` id `51`: HOST spawns 10v10 at t=1.5s, then each second rearms + logs
  the census; BOTH roles log `census ... seen=<n>` = non-squad combatants captured
  within ~200u of the local leader (`engine::captureNpcs`, the same tool `npc_sync` uses).
- Networked host+join on save `c`, host window 70s / join 40s, harness screenshots.

## Findings

1. **The helper reliably spawns a full 10v10 and drives all 20 into combat mode.**
   Host log: `battle spawned perSide=10 total=20`, then `battle r(a10 f10 w0)
   b(a10 f10 w0) fighting=20 alive=20/20` sustained for the entire 70s window. The host
   screenshot (`51/raw/host_5.png`) shows the ~20-body crowd massed by the leader in the
   Border Zone. `spawnHostileBattle` is a working, reusable primitive.
2. **Same-faction `UNPROVOKED_FOCUSED_MELEE_ATTACK` = combat MODE without lethal damage.**
   Across 70s: `wounded=0`, `alive=20/20`, zero casualties; the host screenshot shows the
   group bunched/milling rather than striking. The order flips every unit's `inCombatMode`
   flag (so `fighting=20`), but bodies spawned in a single shared (peaceful) faction do
   **not** deal real damage to each other. A *lethal* battle needs two mutually-HOSTILE
   factions (spike 4 territory) or scripted weakening (`weakenBattle`), not just attack
   orders.
3. **Runtime-spawned battle NPCs are INVISIBLE on the join.** The join's `seen` count
   stayed at the shared-save baseline (~7-8) the whole run and the join screenshot
   (`51/raw/join_5.png`) shows only the 2 co-located shared-save characters - **none** of
   the host's 20 spawned bodies. Spawned NPCs get a host-only `hand` that does not exist
   in the join's save, so the mod's hand-keyed NPC streaming cannot resolve/replicate them.
4. **Therefore a 10v10 *cross-client* sync baseline requires a BAKED save**, not a live
   runtime spawn. Spawn-then-save-then-both-load gives every combatant a shared,
   join-resolvable hand (the path spikes 82 `bakeScene` / 88 `battleN saves` exist for).

## Validation

- Finding 1: `51/raw/host.log` - `SPIKE 51 battle spawned perSide=10 total=20` and ~68
  repetitions of `r(a10 f10 w0) b(a10 f10 w0) fighting=20 alive=20/20`; `host_5.png` shows
  the crowd. Run ended `SPIKE 51 CAPTURE-OK` (clean self-exit, no fault).
- Finding 2: same census lines - `w0` (wounded) and `alive=20/20` never changed over 70s;
  `host_5.png` shows no weapons-drawn melee. The host verdict line
  `verdict HOST total=20 peakFighting=20 casualty=0 -> FAIL` is itself the evidence: the
  only reason it didn't meet the lethal-battle bar was zero casualties.
- Finding 3: `51/raw/join.log` - `census role=J ... seen=7..8` for the full window
  (`verdict JOIN peakRecv=8`); `join_5.png` shows just leader `Flashbow` + `RemotePlayer1`
  and NO crowd, while `host_5.png` (same moment, same zone) shows 20 bodies. The host count
  (authoritative spawn) vs join count (what replicated) diverge exactly as the host-only-
  hand limitation predicts.
- Finding 4: inference from 3 + the established hand-resolution model (a join resolves
  NPCs only via shared-save hands); not independently re-run here - flagged below.

## Open questions / hypotheses (UNVALIDATED)

- **Lethal battle**: producing real casualties (two hostile factions, or `weakenBattle`
  seeding) was not exercised; combat-load spikes that need bodies *dying* must add this.
- **Baked 10v10 cross-client sync** (per-NPC hand-keyed MEMBER/RECV position agreement)
  is asserted as the correct path but NOT run here - it is exactly spike 82/88's job.
- **FPS under load** was not measured (no frame-time probe in this run) - that is the
  explicit deliverable of spikes 52/53 and should reuse this helper.
- **Telefrag/spawn stacking** at higher counts (20v20, 40v40) untested; the lane/depth
  stagger is a guess tuned for 10/side.

## Implications for co-op

- `spawnHostileBattle` is the load-test workhorse for theme 1 - it deterministically puts
  2*N bodies in combat mode near the leader from one call.
- **For repeatable cross-client combat tests, BAKE the scene** (spawn -> save -> both
  load). Live host spawns are host-local only; do not expect the join to see them.
- Lethality is orthogonal: attack orders alone keep allies alive. Wire hostility (spike 4)
  for kills.

## Recommended follow-ups

- Promote `spawnHostileBattle` to the baseline at spike 81 (DUMP) so 52-80 can `-SkipBuild`
  off one build.
- Spike 82/88: bake `battle10`/`battle20` saves from this helper for hand-keyed sync.
- Make a `spawnHostileBattle` variant that places the two sides in mutually-hostile
  factions for lethal-battle spikes (drift/desync, corpse-pile, KO storm: 56-59, 80).
- Keeper primitive (reverted): `engine::spawnHostileBattle` / `rearmBattle` /
  `battleCensus` (Engine.cpp), `SpikeScenario` id 51.
