# Spike 8 - Battle scale ceiling (tick time / FPS)

- Type: DUMP (runtime)
- Status: DONE (idle-body scaling); combat-load follow-up noted
- Save: c
- Probe: `SpikeScenario` id 8 (escalating spawn waves 5->10->20->40->60->90; both sides log FPS via raw onTick-per-second)

## Goal

How many bodies can the host hold before the frame rate collapses, and does the
join pay the same cost? Sets the realistic upper bound for large-scale battles.

## Method

`powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 8 -Save c -Seconds 58`

The scenario counts raw `onTick` calls (one per engine frame) and reports FPS each
second while the host spawns bodies in waves. FPS here is a true per-frame rate
(onTick runs inside the engine main loop). Full logs in `08/raw/`.

## Raw evidence (host FPS vs total spawned)

```
spawnedN=0   ~155-163 fps   (baseline)
spawnedN=5   ~91-124 fps    (settles ~120)
spawnedN=10  ~111-128 fps
spawnedN=20  ~94-115 fps
spawnedN=40  ~63-101 fps    (settles ~96)
spawnedN=60  ~63-88 fps     (settles ~87)
spawnedN=90  ~48-76 fps     (settles ~74)
```
Every wave shows a one-second dip on the SPAWN frame (e.g. 90-body wave: 48 fps the
first second, recovering to ~74). Join FPS stayed **130-170 throughout**, with its
own `near=9` population and `spawnedN=0`.

## Findings

1. **The host scales to ~90 idle NPCs at ~74 fps** (down from ~160 baseline) with
   no hard ceiling reached - roughly linear degradation, ~1 fps lost per ~1.5
   bodies in this range. 30 fps was never approached.
2. **Spawning itself is the spikiest cost** - each wave stalls one frame
   (worst: 48 fps on the 30-body burst) then recovers. Stagger spawns to avoid a
   hitch.
3. **The join is entirely unaffected** by the host's 90 spawns (stable 130-170
   fps). Because runtime spawns never replicate (spike 1), a host-side battle costs
   the join nothing today - which also means the join currently RENDERS NONE of it.
4. **These are IDLE bodies.** Real combat (pathing, weapon swings, hit resolution,
   ragdoll) costs materially more per body, and the network publish cost
   (`MAX_PUBLISH=160`, spike 12) is separate. So 90 is the idle-presence ceiling,
   not the fighting-and-synced ceiling.

## Implications for co-op

- A 20-40 per side melee is comfortably within host frame budget for PRESENCE; the
  open question is the COMBAT + SYNC cost, which needs a real fighting-NPC probe
  (requires an attack-order or two-hostile-faction spawn primitive - see
  follow-ups) and the publish-rate profile (spike 12).
- Because the join renders nothing of a host runtime battle, large-scale battle
  sync is gated on solving cross-client spawn visibility (spike 1's conclusion:
  bake-then-load, or a spawn-intent/content stream), NOT on raw frame budget.

## Recommended follow-ups

- Add a `spawnHostileBattle(perSide, faction)` engine helper (two mutually hostile
  factions) so the engine AI auto-fights, then re-run this probe to get the
  COMBAT-load FPS curve and the publish-bandwidth curve (spike 12).
- Re-run at the same counts on a BAKED battle save so the join actually drives the
  bodies, to measure join-side combat-sync FPS.
