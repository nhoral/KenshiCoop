# Spike 50 - MyGUI native panel integration

- Type: STATIC
- Status: DONE
- Save: n/a (SDK-header / RVA analysis; builds on the runtime proofs of spikes 46-48)
- Branch commit: <filled at commit>

## Goal

Can the co-op mod build a real, native-looking PANEL (a multi-row session/roster/status
window with the game's own skin) rather than just floating labels? Determine the
integration path, the panel API, and what is callable vs blocked.

## Method

Static analysis of `kenshi/gui/DatapanelGUI.h` + `ForgottenGUI.h`, anchored by the
runtime facts already established: `Globals::gui` links and is non-null (spike 46), and
two ForgottenGUI factories actually render at runtime (`createScreenLabel` spike 47,
`createFloatingLabel` spike 48). Re-used the resolvable-symbol rule from spikes 40/41
(raw MyGUI has zero RVAs). No new runtime probe (the render mechanism is already proven;
this spike establishes the panel API surface and the build approach).

## Findings

1. **The native panel type is `DatapanelGUI`, minted by `ForgottenGUI::createDatapanel`.**
   Two overloads: parent-attached `createDatapanel(name, MyGUI::Widget*, scrolls)`
   (RVA 0x73EE10) and free-floating `createDatapanel(top,left,width,height,scrolls,layer,
   window)` (0x73EEE0) -> `DatapanelGUI*`. Reachable via the spike-46 `gui` singleton, the
   same way the proven `createScreenLabel`/`createFloatingLabel` are.
2. **`DatapanelGUI` is a full key/value panel widget with a deep, RVA-annotated API.**
   Row builders: `setLine(key,val,...)` (5 overloads, 0x6FD4B0..0x6FE610),
   `setLineButton`/`setLineToggleButton` (0x6FDE60/0x6FDF20), `setLineText` (0x6FCC80),
   `setLineProgress` (0x6FCF00), `setLineSlider` (0x6FCDA0), `setLineCheckbox` (0x6FDB70),
   `setLineDropBox` (0x6FDC30), `addSpace`, `removeLine` (0x6FC1F0). Structure:
   `addTab`/`showTabs`/`changeCategory`/`setCategoryIcon`, `setCaption` (0x6F78F0),
   `setPanelName`, `clear` (0x6FC730), `show` (0x6F4F80), `update` (0x6F9510),
   `resize`/`setPosition`. This is exactly the substrate for a co-op roster panel (one
   `setLine` per peer: name + ping + status), refreshed via `update()` each tick.
3. **Virtual panel methods have `_NV_` twins, so they are all resolvable.** `GetRealAddress`
   cannot take a virtual member pointer (spike 40/Functions.h), but `DatapanelGUI` exposes
   `_NV_` non-virtual twins for every virtual (e.g. `_NV_clear` 0x6FC730, `_NV_show`
   0x6F4F80, `_NV_update` 0x6F9510, `_NV_addTab` 0x6FB130, `_NV_setObject` 0x6F9580) - the
   same pattern spikes 47/48 used for `ScreenLabel::_NV_*`. The non-virtual `setLine*`
   builders need no twin (already non-virtual).
4. **Integration must go through Kenshi's wrappers, never raw MyGUI.** Spike 41 established
   raw MyGUI has zero RVA annotations; `DatapanelGUI`/`ForgottenGUI` ARE the native MyGUI
   integration layer (they wrap MyGUI internally but expose resolvable RVAs). The factories
   return `MyGUI::Widget*`/`DatapanelGUI*`, but the mod only ever calls back into RVA'd
   ForgottenGUI/DatapanelGUI methods, so the raw-MyGUI gap never blocks panel work. Lower-
   level building blocks if a hand-laid-out panel is preferred: `createPanel` (0x73F050),
   `createLabel`/`createButton`/`createListbox`/`createProgressBar`/`createTabPanel`/
   `createImage` (spike 41 catalog), and `messageBox` (0x740F60) for modal dialogs.
5. **Two concrete ABI/safety constraints for the implementation (carried from 47/48).**
   (a) `setLine*` take multiple `std::string` by const-ref + the float-overload
   `createDatapanel`/`DatapanelGUI` ctor take a BY-VALUE `std::string layer`; build those
   strings in a non-SEH outer fn and pass caller-owned pointers (MSVC x64 passes by-value
   non-trivial args by hidden ptr) - the exact technique spike 48 validated for
   `createFloatingLabel`. (b) `MyGUI::Align` args (e.g. `setLineText`) are header-inline
   4-byte values, constructible without linking MyGUI. Cleanup via
   `ForgottenGUI::destroy(DatapanelGUI*)` (0x6E2690).

## Validation

- Findings 1-3, 5: direct header/RVA citations from `DatapanelGUI.h` (createDatapanel via
  ForgottenGUI.h 0x73EE10/0x73EEE0; setLine 0x6FD4B0..; setLineButton 0x6FDE60;
  setLineProgress 0x6FCF00; setCaption 0x6F78F0; _NV_update 0x6F9510; _NV_clear 0x6FC730;
  ctor float-overload `(... const std::string& layr)` 0x6FBA00; destroy 0x6E2690).
- Finding 4: re-uses spike 41's verified count (raw MyGUI = 0 RVAs; ForgottenGUI = 120) and
  the proven render of `createScreenLabel`/`createFloatingLabel` (spikes 47/48 screenshots/
  non-null handles) - createDatapanel is the same factory family on the same `gui` instance.

## Open questions / hypotheses (UNVALIDATED)

- **No datapanel was actually rendered.** createScreenLabel/createFloatingLabel are
  screenshot/handle-proven, but `createDatapanel` + `setLine` rendering a visible, skinned
  multi-row panel is INFERRED, not run. A probe (create a 3-row panel, `setLine` per row,
  screenshot) would convert this to fully proven - it is the natural spike-50 RUN follow-up.
- **Which `layer` string is valid** for a free-floating datapanel is unconfirmed (spike 48's
  `createFloatingLabel` returned non-null with `"Info"` but its visible paint was not
  screenshot-confirmed). The parent-attached `createDatapanel` overload (attach to a known
  Kenshi window/widget) sidesteps the layer question.
- **Interaction (button/checkbox callbacks)** routes through MyGUI delegate types
  (`IDelegate2<...>`); whether the mod can register a callable callback without raw-MyGUI
  symbols is unproven (display-only panels avoid this).
- **Refresh cost / `setFrequentUpdateMode`** behaviour for a per-tick-updated co-op panel
  was not characterized.

## Implications for co-op

- **A native co-op panel is feasible with the proven HUD stack.** Use
  `gui->createDatapanel(...)` for a session/roster window and `setLine`/`setLineProgress`
  per peer (name, ping from spike 48, downed/health status), `update()` each tick,
  `destroy()` on session end. No raw MyGUI, no custom skin work.
- This completes the HUD toolkit: notices (spike 36 message bar), nameplates (47),
  connection/ping (48), and now a structured status PANEL (50) - all on `ForgottenGUI`.

## Recommended follow-ups

- RUN proof: build a small `DatapanelGUI` ("Co-op Session": one `setLine` per connected
  peer with ping), screenshot-verify it renders skinned and updates; prefer the
  parent-attached overload to avoid the open `layer` question.
- Probe a `setLineButton` callback to learn whether interactive panels are reachable, or
  confirm co-op panels stay display-only.
- Keeper (deferred until the RUN proof): an `engine::hudPanel*` helper family mirroring
  `hudStatusLabel`, with the C2712 SEH split + caller-owned `std::string` args.
