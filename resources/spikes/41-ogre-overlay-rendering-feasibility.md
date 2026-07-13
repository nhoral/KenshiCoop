# Spike 41 - Ogre overlay rendering feasibility

- Type: DUMP
- Status: PARTIAL
- Save: c (runtime anchor reused from spike 36)
- Branch commit: <filled at commit>

## Goal

Determine whether the mod can render a custom on-screen overlay/HUD (the substrate for
spikes 46-48: HUD, peer nameplates, connection/ping). Decide between three candidate
render layers - raw Ogre 2.0 Overlay, raw MyGUI, or Kenshi's reconstructed GUI - and
identify which is actually callable given the mod's symbol-resolution constraints.

## Method

Static symbol-availability analysis (the decisive evidence) plus the runtime anchor from
spike 36. For each candidate layer, counted RVA-annotated methods in its headers
(`KenshiLib::GetRealAddress` only works on RVA-annotated symbols - established in spike
40). Cross-referenced the spike-36 result (`GameWorld::showPlayerAMessage` rendering,
screenshot-verified) as the one runtime-proven render path. No new probe code shipped
(see Open questions for why the custom-widget probe is deferred).

## Findings

1. **Raw Ogre 2.0 Overlay is NOT callable.** The `ogre/Overlay/` headers contain **zero**
   `RVA = 0x...` annotations, so `OverlayManager`/`Overlay`/`OverlayElement` factory and
   manager methods cannot be resolved via `GetRealAddress` (same constraint spike 40
   found for all of Ogre). Raw Ogre overlay rendering is therefore infeasible with the
   mod's current mechanism.
2. **Raw MyGUI is also NOT directly callable.** The `mygui/` headers likewise have
   **zero** RVA annotations. MyGUI is template/inline-heavy, but its inline wrappers
   bottom out in singleton accessors (`MyGUI::Gui::getInstance()`,
   `SkinManager`/`LayoutManager`, obfuscated to `n()`) whose compiled addresses are
   unknown - so creating MyGUI widgets directly is not resolvable either.
3. **Kenshi's reconstructed GUI (`ForgottenGUI`) IS the feasible layer - 120 RVA-annotated
   methods, including real widget factories.** Notably
   `createDatapanel(...) -> DatapanelGUI*` (RVA 0x73EE10/0x73EEE0),
   `messageBox(title,message,btn,modal,callback) -> MyGUI::Window*` (0x740F60),
   `showMainbar`/`showNames`/`getToolTip`, and the message-bar entry
   `GameWorld::showPlayerAMessage` (0x723830). These wrap MyGUI internally but expose
   resolvable RVAs, so they are reachable the same way every other mod engine call is.
4. **Exactly one overlay render path is runtime-proven today: the message bar.** Spike 36
   drove `GameWorld::showPlayerAMessage` and screenshot-verified the text on screen. That
   is the only HUD output validated at runtime; the richer factories (`createDatapanel`,
   `messageBox`) are resolvable but unexercised.

## Validation

- Findings 1-2: `rg "RVA = 0x"` over `ogre/Overlay/` and `mygui/` both returned **0**.
  This is hard, decisive evidence that those layers are outside the GetRealAddress
  mechanism (contrast: `ForgottenGUI.h` alone has 120).
- Finding 3: `ForgottenGUI.h` method/RVA lines quoted directly (createDatapanel 0x73EE10,
  messageBox 0x740F60, etc.); count of 120 RVAs from `rg`.
- Finding 4: reuse of spike 36's screenshot-verified result for `showPlayerAMessage`
  (RVA 0x723830, confirmed in `GameWorld.h:149`). No new claim is made beyond what 36
  proved at runtime.

## Open questions / hypotheses (UNVALIDATED)

- **No instance accessor for `ForgottenGUI` was found.** It has no `getSingleton`; the
  global `Globals::gui` is a `__declspec(dllimport)` whose linkability into the mod is
  unproven (the mod reaches `GameWorld` via the main-loop hook param, never via
  `Globals::ou`), and `GameWorld` exposes no GUI getter (only a `guiDisplayObject` hand
  at 0x4C0). So **how to obtain a `ForgottenGUI*` to call `createDatapanel`/`messageBox`
  is the blocking open question** - and the reason a custom-widget probe was not shipped
  (it could not be written without first solving instance access, and a half-probe would
  risk a crash/link failure for no validated gain).
- **Whether `createDatapanel`/`messageBox` actually render a usable custom HUD** is
  therefore UNVALIDATED - only the built-in message bar is proven.
- **`Globals::gui` link test**: does the KenshiLib import lib resolve the dllimport? A
  one-line compile/link test would answer this cheaply (failure = clean build error).

## Implications for co-op

- **Abandon raw Ogre Overlay and raw MyGUI** for HUD work - neither is resolvable.
- **Build all HUD/overlay features on `ForgottenGUI`'s reconstructed MyGUI wrappers.** The
  message bar (`showPlayerAMessage`) is ready now for notices (peer joined/downed, kill
  feed); `messageBox`/`createDatapanel` are the next tier (modal dialogs, status panels)
  once instance access is solved.
- This reframes spikes 46-48: they should target ForgottenGUI/MyGUI-via-Kenshi, not raw
  Ogre - matching spike 40's recommendation.

## Recommended follow-ups

- **Unblock instance access (highest priority):** test linking `Globals::gui`
  (`extern "C"`/import-lib check); if it resolves, validate `messageBox` renders a window
  (screenshot) and `createDatapanel` renders a panel. This converts spike 41 to DONE.
- If `Globals::gui` is not linkable, search for a `ForgottenGUI*` reachable from a hooked
  call's args or from a resolvable GameWorld/PlayerInterface member.
- Then build the spike-46 HUD proof on `createDatapanel` rather than raw Ogre.
