# Spike 54 - Attribution correctness with 2 simultaneous fights (+ lethal battle unlock)

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Stage TWO simultaneous fights and check that combat is attributed correctly (each
fighter engages an ENEMY, the two melees stay separate, casualties map to the right
victims). This first required solving the lethality gap spikes 51-53 left open
(same-faction orders = posturing, 0 casualties), so this spike also validates a
working LETHAL battle mechanism.

## Method

- Two lethal-staging approaches tried:
  1. **getOrCreateFaction recipe (spike 4's static recommendation) - FAILED.** Creating
     two fresh factions (`FactionManager::getOrCreateFaction`, RVA 0x2E7340) and spawning
     into them produced **0 bodies** in the new faction (a fresh faction is not
     spawn-ready), the 2nd `getOrCreateFaction` returned **NULL**, and after the spawn the
     run stopped advancing the battle census (a half-initialised faction registered in
     the manager destabilised the run). Abandoned.
  2. **Two EXISTING non-player factions set mutually ENEMY - WORKS.** `findTwoNonPlayer
     Factions` scans loaded NPCs for two distinct non-player factions (player-faction
     fallback for the 2nd), then `FactionRelations::_NV_setEnemy` (RVA 0x6B2750) makes
     them hostile both ways. Spawning into an existing faction is the proven spike-51
     path.
- `spawnLethalBattle(perSide)` spawns `perSide` into each faction, split into TWO spatial
  clusters per side -> two melee knots. `lethalCensus` reports per-side alive/fighting/
  KO'd(BODY_DOWN|RAGDOLL)/dead(BODY_DEAD) + attribution: how many fighters' attack target
  is an ENEMY faction (`targetEnemy`) vs a SAME faction (`targetFriend`).
- `SpikeScenario` id `54` (HOST-ONLY), 8v8, 60s. Run on save `c`.

## Findings

1. **Lethal combat works via two existing hostile factions.** Real KOs accrued from
   genuine melee: down-count climbed 0 -> 1 -> 2 -> 3 -> 4 per side over the window,
   **peakDown=6** simultaneously. This fixes spike 51's non-lethality (0 wounded): the
   cause was same-faction (AI won't damage allies); cross-faction hostility makes the
   identical orders lethal.
2. **Attribution is correct: NO fighter ever targeted a friend.** `targetFriend=0` on
   EVERY census tick for the whole 60s; `targetEnemy` rose to **16** (all 16 fighters
   targeting the opposing faction) and stayed 8-16 as fighters were KO'd. Across the two
   simultaneous clusters there was zero cross-faction-target contamination.
3. **KO, not death, is Kenshi's combat outcome in this window.** `totalDead=0` while
   `peakDown=6` - melee knocks combatants unconscious (BODY_DOWN/RAGDOLL); actual death
   (BODY_DEAD) needs finishing blows / bleed-out beyond 60s. The down/dead distinction is
   cleanly readable via `readBodyState`.
4. **`setEnemy` on existing factions is safe and immediate** - hostile=1 logged, AI
   engaged within ~6s (all fighting by t=7.5s), no instability for the full run.

## Validation

- All quoted numbers are from `54/raw/host.log` `SPIKE 54 lethal ...` census lines and the
  final `SPIKE 54 lethal-summary total=16 peakDown=6 totalDead=0 peakTgtEnemy=16
  sawTgtFriend=0 casualties=1 attribOk=1 -> PASS`.
- Finding 1: progressive `d` (down) counts across timestamped census lines (t=13515 d1 ->
  t=39328 d4 ...); `host_5.png` shows the melee with downed bodies.
- Finding 2: `tgtFriend=0` appears on every census line; `tgtEnemy` peaks at 16 at t=7515.
- The FAILED recipe is evidenced by the earlier run's log:
  `SETUP(lethal): red=0x... blue=0x0 hostile=0` + `perSide=8 red=0 blue=8` (0 spawned into
  the new faction) followed by the census halting after one tick.

## Open questions / hypotheses (UNVALIDATED)

- **Cross-client attribution** (does the JOIN attribute the same KO/death events to the
  same victims?) is NOT tested: runtime spawns don't replicate (spike 51), so the join
  never sees this battle. This needs a BAKED battle save (spike 82/88) - and it is the
  attribution question that actually matters for co-op.
- **Killer attribution** (who gets credit for a KO/kill) was not read - only victim and
  target-faction. `FactionRelations::affectRelations(KILLED_AN_ENEMY)` is the engine's
  own credit path (spike 4) but was not instrumented.
- **Deaths** (vs KOs) need a longer window or bleed-out; the down->dead transition timing
  is unmeasured.
- **>2 fights / 4+ simultaneous** (spike 55) and whether two clusters can drift into one
  blob (here they held separate by spawn geometry) is untested at higher counts.
- **Collateral**: setting two town factions hostile may turn their world-wide loaded
  members on each other; not observed to harm this run but not bounded.

## Implications for co-op

- **Lethal battle staging is now solved for HOST-LOCAL tests**: find/relate two existing
  factions + `spawnLethalBattle`. Use this for all combat-load/sync spikes that need real
  casualties (56-59, 66, 80) - NOT runtime faction creation, which is unsafe.
- Host-local attribution is reliable; the open risk is the JOIN's attribution, which is
  untestable until battles are baked. Prioritise bakeScene (82) to unblock the cluster.
- KO is the dominant combat outcome - co-op must replicate the BODY_DOWN edge (already in
  the event protocol, EVT_KNOCKOUT) far more often than EVT_DEATH.

## Recommended follow-ups

- Pull spike 82 (bakeScene) forward: bake a 2-fight save so the JOIN sees the battle and
  cross-client attribution can finally be measured (54's real co-op question).
- Instrument killer attribution (affectRelations / kill credit) and KO->death timing.
- Keeper primitives (reverted): `engine::spawnLethalBattle` (two existing factions +
  `_NV_setEnemy`), `findTwoNonPlayerFactions`, `lethalCensus`, `SpikeScenario` id 54.
  NOTE: do NOT use `getOrCreateFaction` for runtime battle staging (proven unsafe here).
