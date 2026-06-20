# Spike 46 - Ogre overlay HUD proof (ForgottenGUI instance access)

- Type: RUN
- Status: PARTIAL
- Save: c
- Branch commit: <filled at commit>

## Goal

Unblock the HUD work flagged by spikes 40-41: those proved raw Ogre Overlay and raw
MyGUI are not callable (zero RVA annotations) and that Kenshi's `ForgottenGUI` (120 RVAs,
incl. `createDatapanel`/`messageBox`) is the only feasible custom-HUD layer - but left one
hard blocker: **there was no proven way to obtain a `ForgottenGUI*` instance**. The only
candidate was the `__declspec(dllimport)` global `Globals::gui`, whose linkability into
the mod was unproven (the mod reaches the engine only through hook params, never via the
`Globals::` imports). This spike answers the cheap, decisive question: **does `Globals::gui`
link, and is it non-null and stable at runtime on both clients?**

## Method

Added a one-line link/instance probe rather than a full widget test (the widget factories
cannot be exercised at all until instance access is proven, so this is the correct first
gate):

- `engine::probeGuiGlobal()` (`src/plugin/game/Engine.cpp`) `#include <kenshi/Globals.h>`
  and returns `(void*)gui` inside a `__try/__except` guard.
- `SpikeScenario` id `46` (`src/plugin/test/Scenario.cpp`) calls it on a dedicated 1.5s
  timer (`lastHudMs_`) and logs `hud role=<H|J> t=<ms> gui=<ptr>`; sets PASS when non-null.

Ran networked host+join on save `c` for 20s, captured `46/raw/{host,join}.log`.

## Findings

1. **`Globals::gui` LINKS into the mod.** The build resolved the `__declspec(dllimport)`
   symbol with no link error, so `kenshilib.lib` exports the `ForgottenGUI* gui` global.
   This was the unproven step in spike 41; it is now confirmed by a clean build + a runtime
   read (a dangling import would not have produced a stable pointer every tick).
2. **`gui` is non-null and stable for the entire session, on BOTH clients.** Host logged
   `gui=00007FF661242750` and join `gui=00007FF6215F2750`, each identical across all 13
   samples (t=1516 -> 19516 ms). The two values differ between processes - expected, since
   they are separate ASLR'd images (both based at 0x7FF6...); the pointer is process-local
   and never compared across the wire.
3. **Instance access to `ForgottenGUI` is therefore solved.** `Globals::gui` is the
   accessor spikes 41/47/48/50 needed. The HUD layer can now be built on the reconstructed
   MyGUI wrappers (`createDatapanel`, `messageBox`, `createScreenLabel`, ...), reached the
   same way as every other resolvable engine call.

## Validation

- Finding 1: clean build/link of the mod with `#include <kenshi/Globals.h>` and
  `(void*)gui` referenced - a missing export would fail at link. Confirmed by the run
  producing live pointer values (a stale/garbage import could not stay constant).
- Finding 2: quoted runtime log lines, both roles:
  - `[HOST] SPIKE 46 hud role=H t=1516 gui=00007FF661242750` (constant through t=...; sampled).
  - `[JOIN] SPIKE 46 hud role=J t=1516 gui=00007FF6215F2750` (constant through t=19516).
  - All 13 samples per role identical -> stable singleton, non-null. Scenario set PASS
    (`SPIKE 46 CAPTURE-OK`).
- Finding 3: follows directly from 1+2; the accessor exists and yields a usable pointer.

## Open questions / hypotheses (UNVALIDATED)

- **No widget was actually rendered in this spike.** Only the *instance pointer* is proven.
  Whether calling `gui->messageBox(...)` / `gui->createDatapanel(...)` actually draws a
  usable custom HUD (and is safe from the main-loop hook) is still UNVALIDATED - that is
  the job of spike 47/48's render proof now that the pointer is in hand.
- **Thread/lifetime safety of dereferencing `gui` from the hook** is assumed (the hook runs
  on the main thread per spike 44) but not stress-tested across load/save/teardown.
- **`gui` validity before the HUD is constructed** (very early boot, main-menu) is untested;
  here it was already non-null by t=1.5s in-game. A null-guard is still required.

## Implications for co-op

- The HUD blocker from spikes 40/41 is cleared: **use `Globals::gui` as the `ForgottenGUI`
  accessor** and build all custom overlay/HUD features (peer nameplates, connection/ping,
  kill feed beyond the message bar) on its RVA-annotated MyGUI wrappers.
- Keep a null-guard + `__try/__except` around the first deref until the render proof
  (spike 47/48) hardens the path.

## Recommended follow-ups

- **Spike 47/48 render proof:** call `gui->messageBox(...)` (or `createScreenLabel`) once
  per session and screenshot-verify a custom window/label appears - this is what converts
  41 + 46 from "reachable" to "renders", and unblocks nameplates/ping overlays.
- Probe `gui` validity at boot/main-menu to size the null-guard window.
