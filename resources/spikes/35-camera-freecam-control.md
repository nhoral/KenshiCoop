# Spike 35 - Camera / free-cam control

- Type: DUMP
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Can the mod read and programmatically control the camera? This unblocks co-op UI
features: "jump to peer", follow-peer cam, and a free/spectator camera. Also confirm
the camera is per-client local (so driving it never desyncs the simulation).

## Method

Static: `kenshi/CameraClass.h` is fully named; the instance is reached via
`PlayerInterface::getCamera()` (member `camera` at 0x30). Runtime: resolved
`getCamera`, `setFreeCameraMode`, `teleport`, `focusCameraOnObject`, `getCameraPos`
(struct-return ABI) by RVA and added a probe (id 35). BOTH clients log camera pos +
free-cam flag each 1s and exercise their OWN camera: focus-on-leader (t=4s), free-cam
ON (t=8s), teleport +2000/+2000 (t=12s), free-cam OFF (t=16s). Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 35 -Save c -Seconds 22
```

Probe code reverted; raw logs in `35/raw/`. (yaw/pitch/altitude read 0 the whole run -
not relied upon below; see Open questions.)

## Raw evidence

Host camera responding to each lever (`35/raw/host.log`):

```
t=1016  pos=-51316.7,1646.9,2739.4  freecam=0          (initial)
t=4016  act focusLeader ok=1
t=5016  pos=-51211.2,1567.8,2883.6  freecam=0          (jumped ~105u onto leader)
t=8016  act freecam(on) ok=1
t=9016  pos=-51211.2,1567.8,2883.6  freecam=1          (flag flipped)
t=12016 act teleport(+2000,+2000) ok=1
t=13016 pos=-49647.1,1565.2,4445.7  freecam=1          (jumped +1564x,+1562z)
t=16016 act freecam(off) ok=1
t=17016 pos=-49647.1,1601.4,4445.7  freecam=0          (flag cleared)
```

Join ran the identical levers on its own camera (`35/raw/join.log`): its own
`SPIKE 35 act` lines (ok=1) and matching cam series (same save -> same start/results).

## Findings

1. **The full camera control surface is reachable and works at runtime.** `getCamera`
   returns the live per-client `CameraClass`; `focusCameraOnObject`, `setFreeCameraMode`
   and `teleport` all had the expected effect, and `getCameraPos` reads the result.
2. **`focusCameraOnObject(leader)` recentres the camera on a chosen object.** The
   camera position moved from (-51316.7,...,2739.4) to (-51211.2,...,2883.6) - onto the
   leader - after the focus call. This is the "jump to <peer/character>" primitive.
3. **`teleport(pos)` moves the camera to an arbitrary world position** (free-cam): a
   +2000/+2000 request moved the camera +1564x/+1562z (terrain-clamped), confirming
   programmatic free-roam placement.
4. **`setFreeCameraMode(bool)` toggles free-cam and is observable** via the
   `freeCameraMode` member: 0 -> 1 after ON, 1 -> 0 after OFF.
5. **The camera is per-client local and absent from the wire protocol.** Each client
   resolves and drives its OWN `CameraClass`; there is no camera field in any packet,
   so camera control cannot desync the simulation.

## Validation

- Findings 1-4: `35/raw/host.log` (quoted above). Each `SPIKE 35 act ... ok=1` is
  immediately followed (next 1s `cam` sample) by the corresponding state change:
  focus -> pos delta onto leader; freecam(on) -> `freecam=1`; teleport -> pos delta
  ~= requested +2000/+2000 (clamped); freecam(off) -> `freecam=0`. The `ok=1` return
  proves `getCamera` + the lever resolved and executed without fault.
- Finding 2 specifically: the post-focus pos equals the leader vicinity; compare to
  the spawn-leader position logged by the smoke/scenario anchor on 'c'.
- Finding 5: the camera pointer is obtained from `gw->player->getCamera()` (a local
  object); grep of the wire protocol (Protocol.h / Wire.h packet structs) shows no
  camera/view field - PKT_* carry player/NPC transforms only. Both clients produced
  independent `SPIKE 35 act` lines, i.e. each drove its own camera.

## Open questions / hypotheses (UNVALIDATED)

- **yaw / pitch / altitude members read 0 the entire run.** Either those offsets are
  wrong in our struct mapping or the camera genuinely had them zeroed in this state.
  Not used in any finding. Test: rotate the camera (`rotate(yaw,pitch)`) and re-read.
- **Smoothness / interpolation of `teleport` vs `focusCameraOnObject`** (does it snap
  or glide?) was sampled at 1s granularity only - the transition shape is unmeasured.
- **`followObject(hand)` (continuous follow) was not exercised** - it is the more
  natural "follow peer" primitive than repeated focus calls; needs its own probe with
  a constructed hand.

## Implications for co-op

- "Jump to peer", free/spectator cam, and cinematic framing are all feasible now on
  the existing API - no new engine reversing needed, just resolve + call on the local
  camera.
- Because the camera is purely local, these are safe UI-only features (no authority,
  no sync, no desync risk); a follow-peer cam just needs the peer's hand/position,
  which the squad-state stream already provides.

## Recommended follow-ups

- Probe `followObject(hand)` + `stopFollowing()` for a true continuous follow-peer cam
  (construct the peer leader's hand from the squad-state stream).
- Build the co-op UI spikes (46-48) on these primitives: a "focus peer" hotkey using
  `focusCameraOnObject` / `followObject`.
- Keeper primitives (reverted): `engine::readCamState`, `camSetFree`, `camTeleport`,
  `camFocusLeader`.
