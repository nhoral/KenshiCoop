# Spike 49 - Minimap markers for peer squad

- Type: STATIC
- Status: DONE
- Save: n/a (SDK-header / RVA analysis)
- Branch commit: <filled at commit>

## Goal

Can the co-op mod put markers for the PEER's squad on the map, so each player can see
where the other player's characters are? Determine the marker API, the instance-access
path, the world->map projection, and whether peer bodies already get markers for free.

## Method

Static analysis of the Kenshi GUI headers (`kenshi/gui/MapScreen.h`,
`ManagementScreen.h`, `ForgottenGUI.h`, `Globals.h`) - the decisive evidence for an API
that lives entirely in the (reconstructed, RVA-annotated) engine. Cross-referenced the
mod's existing peer-resolve path (received squad member -> local `Character*`, a
`RootObjectBase`) and the resolvable-symbol constraint from spikes 40/41 (`GetRealAddress`
needs an RVA annotation). No runtime probe shipped (this is a header/feasibility spike;
see Open questions for the runtime checks that would upgrade the hypotheses).

## Findings

1. **Kenshi has NO minimap - the only map UI is the full-screen `MapScreen`** (the M-key
   screen), a child of `ManagementScreen`. So "minimap markers" for co-op means markers on
   that map screen; there is no always-on corner minimap widget to annotate.
2. **`MapScreen` already has a complete per-character marker system.**
   `MapScreen::MapMarkerCharacter` (ctor `(RootObjectBase*, MapScreen*)` RVA 0x492690) holds
   a `hand` + a `MyGUI::ImageBox*` and an `update(mapPosition)` (0x48F9D0); live markers are
   stored in `mapMarkersCharacters` (an `ogre_unordered_map<hand, MapMarkerCharacter*>` at
   +0xE8) - **keyed by `hand`, the exact identity the mod already uses**. Town markers
   (`MapMarkerTown`) exist in parallel.
3. **World->map projection and faction colouring are built in.**
   `MapScreen::worldToMapCoords(const Ogre::Vector3&)` (0x48BC60) converts a world position
   to map pixels; `getMarkerColor(RootObjectBase*)` (0x48EBA0) + the static colours
   `MarkerColourAlly/Netural/Enemy/Player/PlayerSelected` (RVAs 0x212C468..0x212C4A8) give
   the standard faction tints. So a marker's placement + colour are engine-provided.
4. **The instance is reachable via a resolvable singleton.**
   `ManagementScreen::getSingleton()` (static, RVA 0x2967F0; also static field `singleton`
   at 0x212C428) -> `ManagementScreen::mapScreen` (`MapScreen*` at +0xA8). This is the same
   `getSingleton()` shape the mod already proves works for `SaveManager` (spike 5). High-
   level marker entry points hang off `ManagementScreen`: `addSquadToMap(Platoon*)`
   (0x494520), `removeSquadFromMap(Platoon*)` (0x48F510), `refreshMap(bool)` (0x49ADD0); the
   per-character entry is `MapScreen::updateCharacterMarker(RootObjectBase*)` (0x493350).
5. **The squad-level API needs a `Platoon*` the SDK does not expose; the per-character
   path does not.** `Platoon` is only forward-declared (no layout header, and no
   `getPlatoon()`/squad accessor anywhere in the SDK headers), so `addSquadToMap(Platoon*)`
   is hard to feed. But the mod ALREADY resolves each received peer squad member to a local
   `Character*` (a `RootObjectBase`), so `updateCharacterMarker(thatChar)` /
   `MapMarkerCharacter(thatChar, mapScreen)` is the usable route - it takes exactly what the
   mod has.
6. **Genuine shared-save peer characters likely get markers for free.** Peer squad members
   that exist in the shared save are player-faction `Character`s; `refreshSquads()`/
   `refreshMap()` enumerate the player's squads, so the engine should already mark them with
   the player colour. New marker code would only be needed for mod-spawned PROXIES (peers
   with no local save Character).

## Validation

- Findings 1-5: direct header/RVA citations from `MapScreen.h` (MapMarkerCharacter ctor
  0x492690, mapMarkersCharacters +0xE8, worldToMapCoords 0x48BC60, getMarkerColor 0x48EBA0,
  colour statics 0x212C468.., updateCharacterMarker 0x493350) and `ManagementScreen.h`
  (getSingleton 0x2967F0, singleton 0x212C428, mapScreen +0xA8, addSquadToMap 0x494520,
  removeSquadFromMap 0x48F510, refreshMap 0x49ADD0). `Globals.h` has no map global, and a
  repo-wide header search found `Platoon` only as a forward declaration (no definition),
  substantiating finding 5.
- Finding 6 is a reasoned inference from the player-squad refresh entry points
  (`refreshSquads` 0x49B7A0, `refreshMap`) - flagged for runtime confirmation below.

## Open questions / hypotheses (UNVALIDATED)

- **Is `mapScreen` non-null before the map is first opened?** `MapScreen` may be lazily
  constructed when `ManagementScreen` first shows the map tab, so a marker call at arbitrary
  time could hit a null `mapScreen`. A one-frame probe (resolve `getSingleton()`, read
  `mapScreen`, log null/non-null over time) would settle this - the same instance-probe
  shape as spike 46/48.
- **Do real shared-save peer characters already appear on the map** (finding 6)? Open the
  map on both clients and screenshot - if the peer's squad shows up player-coloured with no
  mod code, the spike's deliverable for save-resident peers is "nothing to build".
- **Do PROXY bodies appear, and does `updateCharacterMarker(proxy)` render a marker** that
  tracks via `worldToMapCoords`? Untested; needs a run with the map open.
- **`addSquadToMap(Platoon*)` is effectively unusable** without a `Platoon*` source - unless
  a `Platoon*` can be reached from a `Character`/`Faction` member offset (not in the SDK).
- **No minimap means no at-a-glance peer tracking during play** - if that UX is desired it
  must be BUILT (e.g. an off-screen direction arrow via `createImage`/`createFloatingImage`
  on `ForgottenGUI`, spike 41's factories), which is a different effort than reusing MapScreen.

## Implications for co-op

- **Map markers for peer squads are largely a reuse, not a build.** For save-resident peer
  characters, the engine's own `refreshMap`/`refreshSquads` probably already marks them;
  for proxies, call `MapScreen::updateCharacterMarker(localPeerChar)` via the
  `ManagementScreen::getSingleton()->mapScreen` path, keyed by the same `hand` the mod uses.
- **There is no minimap**, so a live on-screen peer tracker (off-map) would be NEW UI built
  on the spike-47/41 HUD primitives, not on `MapScreen`.

## Recommended follow-ups

- Quick runtime instance probe: resolve `ManagementScreen::getSingleton()`, log
  `mapScreen` null/non-null across the session (settles lazy-creation), then open the map
  and screenshot whether peer/proxy characters already carry markers.
- If proxies need explicit markers, prototype `updateCharacterMarker(proxy)` from the main
  loop while the map is open and screenshot-verify.
- For an always-on peer tracker, prototype an off-screen direction arrow with
  `ForgottenGUI::createFloatingImage` (spike 41) + the spike-35 camera/world-to-screen math.
