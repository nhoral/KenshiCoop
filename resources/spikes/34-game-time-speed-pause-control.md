# Spike 34 - Game time / speed / pause control

- Type: DUMP
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Find the engine's game-time / speed / pause API and answer the co-op-critical
question: is pause/game-speed shared across a session, or per-client local? In Kenshi
SP, the player pauses or runs at 5x freely; in co-op that has to mean something
specific (does the host pausing freeze everyone?).

## Method

Static: read `kenshi/GameWorld.h` - the time/speed/pause surface is fully named.
Runtime: resolved `setGameSpeed`, `togglePause`, `getTimeStamp`,
`getTimeFromStamp_inGameHours` by RVA and added a probe (scenario id 34). BOTH clients
log `paused` (member 0x8B9), `frameSpeedMult` (member 0x700) and an in-game-hours read
each 1s; the HOST exercises the levers: pause at t=5s, unpause + 5x at t=12s, back to
1x at t=20s. Offline (`34/analyze.py`) the two clients' samples are aligned by WALL
CLOCK (their elapsed timers are independent) and compared for state agreement.
Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 34 -Save c -Seconds 30
python resources/spikes/34/analyze.py resources/spikes/34/raw/host.log resources/spikes/34/raw/join.log
```

NOTE: the in-game-hours read returned ~0.0 throughout (the
`getTimeFromStamp_inGameHours(getTimeStamp())` pairing measures hours-since-stamp, not
absolute clock), so sim speed is taken from `frameSpeedMult`/`paused`, not the clock.
Probe code reverted; raw + analyzer in `34/`.

## Raw evidence

Named API (GameWorld.h, verified): `togglePause(bool)` :126 (RVA 0x786B30),
`getFrameSpeedMultiplier()` :127, `setFrameSpeedMultiplier(float)` :128,
`setGameSpeed(float,bool)` :129 (RVA 0x786C30), `userPause(bool)` :130,
`isPaused()` :131 (RVA 0xDEDC0); members `float frameSpeedMult` :208 (0x700),
`bool paused` :242 (0x8B9). Clock: `getTimeStamp()` :234, `getTimeStamp_inGameHours()`
:238, `getLengthOfHourInRealSeconds()` :239; `TimeOfDay.time` (double) at offset 0.

Host state path and the disagreement with the join (`34/analyze.py`):

```
host state transitions (paused,fsm): [(0,1.0), (1,1.0), (0,5.0), (1,0.0), (0,1.0)]

wall=21168.7  HOST=RUN x5   JOIN=RUN x1   <-- DIVERGE     (host sped up, join normal)
wall=21176.7  HOST=RUN x1   JOIN=PAUSED   <-- DIVERGE     (host running, join frozen)
...
compared 21 wall-aligned pairs; host/join effective-state DISAGREE in 18
```

The join also went `paused=1 fsm=0` at its own t=9s with NO probe command and stayed
there; the host briefly did the same (t=15-20s, `paused=1 fsm=0`) - an auto-pause not
issued by the probe (the commanded `togglePause(true)` left `fsm=1`, not 0).

## Findings

1. **Kenshi exposes a complete, fully-named game-time/speed/pause API on GameWorld.**
   `setGameSpeed(speed,click)`, `togglePause(on)`, `userPause(p)`, `isPaused()`,
   `set/getFrameSpeedMultiplier`, plus a clock (`getTimeStamp*`,
   `getLengthOfHourInRealSeconds`) - all resolvable by RVA.
2. **The probe levers work and `paused` vs `frameSpeedMult` are two distinct knobs.**
   The host moved through (paused=0,fsm=1) -> `togglePause(true)` -> (1,1.0) ->
   `setGameSpeed(5)` -> (0,5.0) -> [auto] (1,0.0) -> `setGameSpeed(1)` -> (0,1.0).
   `togglePause` flips the boolean only; `setGameSpeed` drives `frameSpeedMult`.
   Effective pause = `paused || frameSpeedMult==0`.
3. **Game speed and pause are PER-CLIENT LOCAL and are NOT synchronized by the
   current mod.** Across wall-aligned samples the two clients disagreed 18/21 times,
   including host RUN x5 while join ran x1, and host running x1 while the join was
   fully paused. One client's speed/pause has no effect on the other's.
4. **An external auto-pause drives `frameSpeedMult` to 0 independently on each
   client.** Both clients entered (paused=1, fsm=0) with no probe command and the host
   recovered only when the next `setGameSpeed` was issued - a separate mechanism from
   the commanded boolean pause.

## Validation

- Finding 1: GameWorld.h lines cited above read directly; the runtime resolves
  succeeded (the levers had effect, Finding 2), proving the RVAs are correct.
- Finding 2: `34/analyze.py` `host state transitions` list, derived from the
  `SPIKE 34 time role=H ... paused= fsm=` series in `34/raw/host.log`, lines up 1:1
  with the `SPIKE 34 act` command timestamps (pause(true)@t5, unpause+speed5@t12,
  speed1@t20). The commanded pause shows fsm=1 while the auto-pause shows fsm=0,
  establishing the two are separate knobs.
- Finding 3: `34/analyze.py` `DISAGREE in 18` of 21 wall-aligned pairs, with the
  explicit divergence lines (`HOST=RUN x5 JOIN=RUN x1`, `HOST=RUN x1 JOIN=PAUSED`)
  computed from the wall-clock-aligned `host.log`/`join.log` samples.
- Finding 4: `34/raw/join.log` shows `paused=1 fsm=0` from join t=9s onward with no
  corresponding `SPIKE 34 act` line, and `host.log` shows the same transient at
  t=15-20s; the commanded `togglePause(true)` (host t=6-12s) is `paused=1 fsm=1.0`,
  so the fsm=0 state was not produced by the probe.

## Open questions / hypotheses (UNVALIDATED)

- **The auto-pause is most likely Kenshi's "pause when the window is unfocused"
  behaviour** (only one of the two windows can hold focus), but the trigger was not
  isolated. Test: a focused-window control run, or toggling the in-game
  "pause when not focused" option, and re-checking whether fsm drops to 0.
- **In-game clock advance rate vs `frameSpeedMult` was not measured** (the hours read
  returned 0). Test: read `getTimeStamp_inGameHours().time` (the TimeOfDay double at
  offset 0) directly and confirm it advances ~5x faster at fsm=5 and freezes when
  paused.
- **Whether `setGameSpeed`/`togglePause` are safe to drive every frame / from the net
  thread** is untested; here they were called once each on the main thread.

## Implications for co-op

- Speed/pause cannot be left as Kenshi's local SP control: today each client runs its
  own clock, so two players at different speeds desync their simulations. A co-op mode
  needs a policy - e.g. force 1x and disable pause, or host-authoritative speed
  broadcast (`setGameSpeed`/`togglePause` are the levers to apply it on each client).
- The unfocused-window auto-pause (Finding 4 / hypothesis) is a real hazard: a
  background player's world would freeze, so the mod likely must suppress that
  auto-pause (keep `frameSpeedMult` pinned) for the non-focused client.
- Two knobs exist; any sync must cover BOTH `paused` and `frameSpeedMult` to be
  correct (a peer could be "running" by the boolean yet frozen by fsm=0).

## Recommended follow-ups

- Focused-window control run to confirm the auto-pause cause; then a probe that pins
  `frameSpeedMult` (or re-issues `setGameSpeed(1)`) to defeat unfocused auto-pause.
- Direct clock read (`TimeOfDay.time`) to quantify sim-time advance per speed setting.
- Prototype host-authoritative speed/pause: broadcast a speed value on the reliable
  channel and apply via `setGameSpeed`/`togglePause` on every client.
- Keeper primitives (reverted): `engine::readTimeState`, `setGameSpeedProbe`,
  `togglePauseProbe` - the basis for any speed/pause sync feature.
