# Spike 36 - In-game messages / notifications

- Type: DUMP
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Can the mod show the player an on-screen notification (e.g. "Flashoox joined",
"your peer was knocked out", "trade complete")? Co-op needs a way to surface session
events to each player without inventing new UI.

## Method

Static: `GameWorld` exposes `showPlayerAMessage(const std::string&, bool queued)`
(RVA 0x723830), `showPlayerAMessage_withLog`, and `playNotification(const char*)`.
Runtime: resolved `showPlayerAMessage` by RVA and added a probe (id 36) that pushes a
distinctive message every ~1s; the harness screenshots ~1-2s in, and the capture is
read back to confirm the text actually renders. Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 36 -Save c -Seconds 16
```

Implementation note (kept for the toolkit): the SEH call must be split into a
POD-only helper - constructing the `std::string` inside the `__try` triggers MSVC
C2712 ("cannot use __try in functions that require object unwinding"). Probe code
reverted; raw log + screenshot in `36/raw/`.

## Raw evidence

```
SPIKE 36 msg t=1016 n=1 ok=1 text="KENSHICOOP SPIKE 36 NOTIFY #1"
SPIKE 36 msg t=2016 n=2 ok=1 text="KENSHICOOP SPIKE 36 NOTIFY #2"
...
```

Host screenshot `36/raw/host.png` (read back): the message bar at bottom-centre shows
**"KENSHICOOP SPIKE 36 NOTIFY"**. The left-edge tutorial list (Basic Controls, "You
are under attack!", etc.) is the separate message-roller queue.

![spike 36 on-screen message](36/raw/host.png)

## Findings

1. **`GameWorld::showPlayerAMessage(text, queued)` renders an arbitrary string in the
   on-screen message bar.** Our text "KENSHICOOP SPIKE 36 NOTIFY" appears in the
   bottom-centre status bar in the captured frame.
2. **The call is safe to drive from the main-loop tick and returns cleanly.** Every
   1s push returned ok=1 across the run with no fault or instability.
3. **The notification is a local UI element** (rendered on the client that calls it),
   so co-op can target a notice to a specific player by calling it on that client.

## Validation

- Finding 1: `36/raw/host.png` read back shows the literal probe text in the message
  bar (the screenshot is the artifact). The `SPIKE 36 msg ... ok=1 text="..."` log
  lines in `36/raw/host.log` correlate the rendered text to the probe call.
- Finding 2: all `SPIKE 36 msg` lines report `ok=1` (the SEH-guarded
  `showMessage` returned true) for the full 16s run; the client stayed live and
  self-exited normally ("test duration elapsed; exiting").
- Finding 3: the call is on the local `GameWorld` and there is no message field in the
  wire protocol (Protocol.h/Wire.h carry transforms/events only); the host rendered
  it on the host screen.

## Open questions / hypotheses (UNVALIDATED)

- **`playNotification(const char*)` (a sound)** was not exercised - need a valid sound
  event name to test; unknown names may no-op or fault.
- **`queued=true` behaviour vs `false`** (immediate vs queued display, and how the
  roller coalesces/expires messages) was not characterized.
- **`showPlayerAMessage_withLog`** also writes to the in-game message log/history;
  whether that history is readable back (for a co-op event log UI) is untested.

## Implications for co-op

- Session notifications (peer joined/left, peer downed/died, trade/loot events) are a
  solved problem: call `showPlayerAMessage` on the relevant client. No new UI needed
  for basic notices.
- Combined with the reliable event channel (Spike 10), the mod can turn a received
  EVT_DEATH/EVT_KNOCKOUT into an on-screen "X was knocked out" line for the peer.

## Recommended follow-ups

- Wire EVT_* (Spike 10) -> `showPlayerAMessage` on the receiving client for a basic
  co-op kill/down feed.
- Probe `playNotification` with a known-good sound name for audio cues.
- Keeper primitive (reverted): `engine::showMessage` (with the C2712-safe SEH split).
