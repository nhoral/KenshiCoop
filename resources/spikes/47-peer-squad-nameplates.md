# Spike 47 - Peer-squad nameplates / markers (HUD render proof)

- Type: RUN
- Status: DONE
- Save: c
- Branch commit: <filled at commit>

## Goal

Spikes 40/41 proved raw Ogre/MyGUI are not callable and that Kenshi's `ForgottenGUI`
is the only feasible HUD layer; spike 46 proved its instance (`Globals::gui`) links and
is non-null at runtime. This spike closes the loop: **actually render a custom HUD widget
that tracks a character on screen** - the substrate for peer-squad nameplates/markers -
and screenshot-verify it on both clients. This converts spikes 41/46 from "reachable"
to "renders".

## Method

Used `ForgottenGUI::createScreenLabel(text, colour, size, rising)` (RVA 0x73E920) - a
purpose-built floating text widget - plus `ScreenLabel::_NV_setTracking(hand, offset)`
(0x6E1BB0) which PINS the label to a character so the engine's own per-frame projection
keeps it on the body (no world->screen math by us = a nameplate), and
`_NV_setRisingSpeed(RS_STOPPED)` (0x6E1C10) to keep it persistent (vs combat-text
rise/fade).

- `engine::hudNameplate(Character*, text)` (`Engine.cpp`): resolves the three fns,
  reads `Globals::gui`, builds the `std::string` + a 4-float `MyGUI::Colour` (amber
  `1.0,0.85,0.30,1.0`) + a head-height `Ogre::Vector3(0,2.2,0)` offset in the (non-SEH)
  outer fn, then a POD-only inner fn does the SEH-guarded `createScreenLabel` +
  `setRisingSpeed` + `setTracking` (avoids MSVC C2712, same split spike 36 used).
- `SpikeScenario` id `47` mints ONE label pinned to the local leader, logs
  `nameplate ... leader=<ptr> label=<ptr>`, then logs `nameplate-alive` each 1.5s.
- Networked host+join on save `c`, 18s, with the harness screenshots
  (`47/raw/{host,join}_*.png`).

## Findings

1. **A custom `ScreenLabel` renders on screen and TRACKS the character.** The amber text
   **"PEER: leader"** appears directly under the leader body on both clients
   (`47/raw/host_3.png`, `join_3.png`), visually distinct from Kenshi's native white
   nameplate `[Flashoox]` (which is the engine's own `showNames`). Our amber colour and
   caption are exactly what `hudNameplate` set, confirming we control the widget's text,
   colour, and tracked target.
2. **Creation + tracking are safe from the main-loop tick and stable for the session.**
   `createScreenLabel` returned a non-null `ScreenLabel*` on both roles (host
   `label=0x0B273A60`, join `label=0x0AA931A0`); the label persisted and rendered across
   the full run with no fault, no flicker, and the client self-exited cleanly.
3. **`setTracking(hand, offset)` is the nameplate mechanism.** Passing the character's
   `handle` + a head-height offset makes the engine project and follow the body every
   frame - the label stayed on the moving character with zero per-frame work from the mod.
4. **The label is a LOCAL HUD element** (each client rendered a label on its own leader);
   to show a *peer's* nameplate, the receiving client calls `hudNameplate` on the
   peer's locally-resolved `Character*` (the same hand-resolve path the mod already uses).

## Validation

- Finding 1: screenshots read back - `47/raw/host_3.png` and `47/raw/join_3.png` both
  show the amber "PEER: leader" text on the body (the screenshot is the artifact). The
  amber matches the probe's `col[4]` and the caption matches the probe's literal string.
- Finding 2: `SPIKE 47 nameplate role=H ... label=000000000B273A60` and
  `role=J ... label=000000000AA931A0` (non-null both roles) in `47/raw/{host,join}.log`,
  followed by `nameplate-alive` lines through t=16516/16532 and `SPIKE 47 CAPTURE-OK`
  (clean self-exit, no fault).
- Finding 3: the label appears at the body (not screen origin) and the `_NV_setTracking`
  call is the only positioning we issued - so the on-body placement is the engine's
  tracking projection working.
- Finding 4: each role created the label on its OWN `engine::leader(ctx.gw)`; the labels
  are at each client's own leader. (Cross-client identity itself is proven elsewhere; here
  only local rendering is claimed.)

## Open questions / hypotheses (UNVALIDATED)

- **Tracking a PEER body specifically** (resolve the peer's hand -> local `Character*` ->
  `hudNameplate`) was not exercised here (we tracked the local leader for a clean
  screenshot). The mechanism is identical, but the end-to-end "label on the remote
  squad" path is unproven.
- **Lifecycle/cleanup**: `ForgottenGUI::destroy(ScreenLabelInterface*)` (0x6E9080) was not
  called - per-peer label creation/destruction on join/leave (and avoiding leaks/dupes)
  is untested.
- **Live caption/colour updates** (`_NV_setCaption` 0x6E3E10, `_NV_setColor` 0x6E1C30) for
  status changes (downed/health/ping) were not driven.
- **Markers vs labels**: an off-screen direction marker or minimap pip (spike 49) is a
  different widget (`createImage`/`createFloatingImage`) and is not covered here.

## Implications for co-op

- **Peer nameplates are a solved render problem.** Build them on `createScreenLabel` +
  `setTracking`: on each client, for every resolved peer/squad `Character*`, mint a
  tracked label once and update its caption/colour on state change. No raw Ogre/MyGUI,
  no world->screen math.
- Combine with the message bar (spike 36) for a complete co-op HUD: persistent nameplates
  for who-is-who + transient notices for events.

## Recommended follow-ups

- Wire `hudNameplate` to the peer-resolve path (received squad members) and screenshot a
  label on the *remote* squad to close the UNVALIDATED peer-tracking gap.
- Add a per-peer label registry keyed by netId/hand with `destroy(...)` on leave to
  validate lifecycle (spike 48's connection/status overlay can reuse it).
- Probe `_NV_setCaption`/`_NV_setColor` for live "downed"/health/ping states.
- Keeper primitive (reverted): `engine::hudNameplate` (createScreenLabel + setTracking,
  C2712-safe SEH split).
