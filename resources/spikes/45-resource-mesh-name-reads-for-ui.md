# Spike 45 - Resource/mesh/name reads for UI

- Type: DUMP
- Status: PARTIAL
- Save: c
- Branch commit: <filled at commit>

## Goal

Determine which human-readable strings the mod can read for co-op UI - character/race
names, faction names, item names, mesh/resource names - and whether they are consistent
across clients. This feeds nameplates, kill feeds, peer rosters, and tooltips.

## Method

Static catalog of the name surface (`GameData`, `RaceData`, `Faction`, `Character`) plus
a runtime DUMP: both clients each 1.5s read their leader's race name through
`Character::getRace() -> RaceData::data (+0x40) -> GameData::name (+0x28)` and log it.
Cross-referenced the mod's existing runtime `GameData::name`/`stringID` reads. Reproduce:

```
powershell -ExecutionPolicy Bypass -File scripts/run_spike.ps1 -Id 45 -Save c -Seconds 25
```

Probe code reverted; raw logs in `45/raw/`.

## The name/string surface

- **`GameData::name` (offset 0x28) + `GameData::stringID` (0x58)** - the universal record
  label. The mod ALREADY reads these at runtime for item/material/manufacturer/template
  names (`Engine.cpp:1012/1219`). This covers item names, material names, manufacturer
  names, squad/template names.
- **Race name:** `RootObject/Character::getRace() -> RaceData*` (RVA 0x5E1780, public
  virtual); `RaceData::data` (GameData*, +0x40) -> `name` = e.g. "Greenlander".
- **Faction name:** `Faction::getName() -> const std::string&` (RVA 0x286780);
  `FactionManager::getFactionByName` 0x2E74A0. (Reaching a character's Faction* lacks a
  clean public getter - see Open questions.)
- **Mesh/Ogre resource names:** would require the `Ogre::Entity`/`Mesh` behind a
  `SceneNode` - blocked by the spike-40 finding (no resolvable Ogre symbols).

## Raw evidence

Both clients, every sample (`45/raw/host.log`, `45/raw/join.log`):

```
host t=...515  race='Greenlander'   (all host samples)
join t=1515..24015  race='Greenlander'   (17 join samples, identical)
```

## Findings

1. **Character race name is readable for UI and consistent across clients.** Every sample
   on both host and join read `race='Greenlander'` for the leader via the
   `getRace()->data->name` chain - a real, human-readable string, identical across the two
   processes (same save).
2. **The `GameData::name`/`stringID` read pattern generalises.** The same std::string
   field that the mod already reads for item names (runtime-proven) is what produced the
   race name here - confirming any record reachable as a `GameData*` yields a UI label via
   `name` (0x28) / `stringID` (0x58).
3. **`getRace()` is a safe, plain virtual call** returning a `RaceData*` that holds its
   backing `GameData*` at +0x40 - two guarded pointer hops, no fault across the whole run
   (clean self-exit).
4. **Mesh/Ogre-resource names are NOT reachable** with the current mechanism (spike 40:
   Ogre symbols carry no RVAs), so visual/mesh labels can't be read directly.

## Validation

- Findings 1-3: `45/raw/{host,join}.log` (quoted). 17+ samples per client, all
  `race='Greenlander'`, produced by two independent processes (role=H/role=J). The probe
  zero-initialises the output buffer and returns false on any null hop, so a real string
  (not a sentinel) proves the full `getRace()->data->name` chain resolved and the offsets
  (0x40, 0x28) are correct. Clean exit confirms no fault.
- Finding 2: the race name came through `GameData::name`, the same field+offset the mod
  reads for items at `Engine.cpp:1012/1219` (runtime-proven there).
- Finding 4: follows directly from spike 40's validated "zero Ogre RVAs" result.

## Open questions / hypotheses (UNVALIDATED)

- **Character DISPLAY name** (the player-given/unique name, not the race) was not read -
  no clean public `Character::getName()` was found (heavy obfuscation; a `CharBody`
  std::string getter at RVA 0x639930 is a candidate but returns by value, a riskier ABI).
  Needs its own probe.
- **Faction name end-to-end is unproven** - `Faction::getName()` exists (0x286780) but a
  character's `Faction*` has no obvious public getter; reaching it (member offset or
  `wearingUniformOf` inverse) is untested.
- **Item names on a live character's inventory** (vs the template/material reads the mod
  already does) were not exercised in this spike.
- **Mesh names** remain blocked pending any Ogre-symbol resolution (spike 40 follow-up).

## Implications for co-op

- Nameplates/roster can show **race** now, and **item/material/manufacturer/template**
  names via the proven `GameData::name` path - enough for a basic kill feed / tooltip.
- Character DISPLAY names and faction names need small follow-up probes before they can be
  relied on in UI.
- Do not plan mesh-name-based UI until the Ogre-symbol question (spike 40) is resolved.

## Recommended follow-ups

- Probe `Character` display-name (try the `CharBody` 0x639930 std::string getter under SEH,
  or find a `name` member offset) and validate cross-client.
- Probe a character's `Faction*` -> `Faction::getName()` for faction labels.
- Keeper primitive (reverted): `engine::readNames(gw, c, raceOut, cap)` (race via
  getRace()->data->name).
